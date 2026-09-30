#include "sangtachi/network/stun_client.hpp"

#include "sangtachi/log.hpp"
#include "sangtachi/platform/random.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>
#include <string>
#include <string_view>
#include <utility>

namespace sangtachi::network {
namespace {

// 타이머 이름의 앞머리. 값은 이 파일이 정했고 근거는 헤더의 "이 코드가 정한 것" 표다.
constexpr std::string_view kRetryPrefix = "stun.retry.";
constexpr std::string_view kDeadlinePrefix = "stun.deadline.";

[[nodiscard]] bool starts_with(std::string_view text, std::string_view prefix) noexcept {
    return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

// 이름 꼬리의 10진 인덱스. 우리가 만든 이름만 들어오므로 형식이 어긋나면 값이 없다.
// 이름 꼬리의 10진 인덱스. **우리가 만드는 모양만 받는다.**
//
// std::to_string 이 내는 표기다. 숫자뿐이고, 비어 있지 않고, 여러 자리인데 0 으로
// 시작하지 않는다. 자릿수 상한을 두는 이유는 값이 넘치지 않게 하려는 것이다. 목록 길이는
// 그보다 한참 작다. 받는 것을 목록으로 정한다 (network/endpoint.hpp 의 parse_ipv4 와 같은
// 자세다).
constexpr std::size_t kMaxIndexDigits = 10;

[[nodiscard]] std::optional<std::size_t> tail_index(std::string_view name,
                                                    std::string_view prefix) noexcept {
    if (!starts_with(name, prefix)) {
        return std::nullopt;
    }
    const std::string_view tail = name.substr(prefix.size());
    if (tail.empty() || tail.size() > kMaxIndexDigits) {
        return std::nullopt;
    }
    if (tail.size() > 1 && tail.front() == '0') {
        return std::nullopt;  // 우리는 "07" 을 만들지 않는다
    }
    std::size_t value = 0;
    for (const char c : tail) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        value = value * 10 + static_cast<std::size_t>(c - '0');
    }
    return value;
}

// 파싱 실패의 이유를 사람이 읽는 토큰으로 적는다. 카운터는 이유를 나누지 않지만
// (protocol.md 13장) 로그는 싣는다. 이름은 network/stun.hpp 의 열거 이름을 따른다.
[[nodiscard]] std::string_view to_token(StunParseError error) noexcept {
    switch (error) {
        case StunParseError::kNone:                   return "none";
        case StunParseError::kTooShort:               return "too_short";
        case StunParseError::kNotStun:                return "not_stun";
        case StunParseError::kBadCookie:              return "bad_cookie";
        case StunParseError::kTransactionMismatch:    return "transaction_mismatch";
        case StunParseError::kUnknownType:            return "unknown_type";
        case StunParseError::kLengthNotAligned:       return "length_not_aligned";
        case StunParseError::kLengthMismatch:         return "length_mismatch";
        case StunParseError::kAttributeTruncated:     return "attribute_truncated";
        case StunParseError::kMappedAddressFamily:    return "mapped_address_family";
        case StunParseError::kMappedAddressMalformed: return "mapped_address_malformed";
        case StunParseError::kMissingMappedAddress:   return "missing_mapped_address";
        case StunParseError::kDuplicateMappedAddress: return "duplicate_mapped_address";
    }
    return "unknown";
}

// 타이머를 걸지 못한 자리를 조용히 넘기지 않는다. 이름이 겹치는 것은 이 파일의 결함이다.
void emit_timer_rejected(std::string_view name) {
    const LogField fields[] = {field("name", name)};
    emit(LogLevel::Warn, "timer.rejected", fields);
}

}  // namespace

std::vector<StunServer> resolve_stun_servers(std::span<const StunServerName> list,
                                             const StunResolveFn& resolve) {
    std::vector<StunServer> out;
    out.reserve(list.size());
    for (const StunServerName& entry : list) {
        // architecture.md 9장의 stun.result 가 싣는 server 필드 표기. 사람이 적은 그대로다.
        std::string label = std::string(entry.host) + ":" + std::to_string(entry.port);

        const auto endpoint = resolve ? resolve(entry.host, entry.port) : std::nullopt;
        if (!endpoint) {
            // 해석 실패는 기동 실패가 아니다. 그 항목만 뺀다 (architecture.md 3.5).
            const LogField fields[] = {field("server", label)};
            emit(LogLevel::Warn, "stun.unresolved", fields);
            continue;
        }

        // 해석 결과가 같은 엔드포인트면 앞의 것 하나만 둔다 (같은 절). 이름이 다르다고
        // 서버가 다른 것이 아니다.
        const StunServer* kept = nullptr;
        for (const StunServer& existing : out) {
            if (existing.endpoint == *endpoint) {
                kept = &existing;
                break;
            }
        }
        if (kept != nullptr) {
            const LogField fields[] = {
                field("server", label),
                field("same_as", kept->label),
                field("endpoint", endpoint->to_string()),
            };
            emit(LogLevel::Warn, "stun.duplicate", fields);
            continue;
        }

        out.push_back(StunServer{std::move(label), *endpoint});
    }
    return out;
}

Millis stun_stage_cap_ms(std::size_t server_count) noexcept {
    // ceil(server_count / kStunConcurrency). 정수 나눗셈으로 올린다.
    const std::size_t rounds = (server_count + kStunConcurrency - 1) / kStunConcurrency;
    return kStunDeadlineMs * static_cast<Millis>(rounds);
}

void emit_stun_discovery_failed() {
    const LogField fields[] = {field("code", kStunDiscoveryFailed)};
    emit(LogLevel::Error, "session.failed", fields);
}

StunClient::StunClient(std::vector<StunServer> servers, StunSendFn send, TimerSet& timers,
                       Counters& counters, StunRandomFn random)
    : servers_(std::move(servers)),
      send_(std::move(send)),
      random_(std::move(random)),
      timers_(timers),
      counters_(counters) {
    if (!random_) {
        // 제품 경로의 기본값. 여기 한 자리에서만 고른다 (protocol.md 13장 트랜잭션 ID).
        random_ = [](std::span<std::byte> out) { return platform::random_bytes(out); };
    }
}

std::string StunClient::retry_timer(std::size_t server_index) const {
    return std::string(kRetryPrefix) + std::to_string(server_index);
}

std::string StunClient::deadline_timer(std::size_t server_index) const {
    return std::string(kDeadlinePrefix) + std::to_string(server_index);
}

bool StunClient::owns_timer(std::string_view timer_name) noexcept {
    // 모양이 맞는 둘만 우리 것이다. 헤더가 그 이유를 갖는다.
    return tail_index(timer_name, kRetryPrefix).has_value() ||
           tail_index(timer_name, kDeadlinePrefix).has_value();
}

std::size_t StunClient::in_flight() const noexcept {
    std::size_t count = 0;
    for (const Slot& slot : slots_) {
        if (slot.active) {
            ++count;
        }
    }
    return count;
}

StunClient::Slot* StunClient::find_slot(std::size_t server_index) noexcept {
    for (Slot& slot : slots_) {
        if (slot.active && slot.server_index == server_index) {
            return &slot;
        }
    }
    return nullptr;
}

StunClient::Slot* StunClient::free_slot() noexcept {
    for (Slot& slot : slots_) {
        if (!slot.active) {
            return &slot;
        }
    }
    return nullptr;
}

void StunClient::send_request(const Slot& slot) {
    const auto message = build_binding_request(slot.transaction);
    // 보내지 못해도 자리를 놓지 않는다. 재시도가 남아 있다 (헤더의 표).
    (void)send_(servers_[slot.server_index].endpoint,
                std::span<const std::byte>(message.data(), message.size()));
}

bool StunClient::begin_query(Slot& slot, std::size_t server_index, Millis now) {
    TransactionId transaction{};
    if (!random_(std::span<std::byte>(transaction.data(), transaction.size()))) {
        // 예측 가능한 값으로 대신하지 않는다. 단계를 여기서 끝낸다 (헤더의 표).
        // architecture.md 9장이 이 이벤트의 필수 필드를 `op` 와 `code` 둘로 고정했다.
        // 이 실패는 Winsock 오류 코드가 없으므로 0 을 싣는다. 필드를 빼면 그 줄을 찾는
        // 검증이 필드 이름으로 고르지 못한다.
        const LogField fields[] = {
            field("op", std::string_view("random_bytes")),
            field("code", static_cast<std::uint64_t>(0)),
        };
        emit(LogLevel::Error, "socket.error", fields);
        return false;
    }

    slot.active = true;
    slot.server_index = server_index;
    slot.transaction = transaction;
    slot.sent = 1;
    send_request(slot);

    if (!timers_.add_once(retry_timer(server_index), kStunRetryIntervalsMs[0], now)) {
        emit_timer_rejected(retry_timer(server_index));
    }
    if (!timers_.add_once(deadline_timer(server_index), kStunDeadlineMs, now)) {
        emit_timer_rejected(deadline_timer(server_index));
    }
    return true;
}

void StunClient::release(Slot& slot) {
    // 취소는 없는 것을 지워도 된다 (timer.hpp). 마감이 이미 만료한 뒤의 응답이 정상이다.
    (void)timers_.cancel(retry_timer(slot.server_index));
    (void)timers_.cancel(deadline_timer(slot.server_index));
    // **끝난 트랜잭션을 여기서 기록한다.** 자리가 끝나는 경로는 셋(응답, 마감, 단계 종료)
    // 이고 셋 다 이 함수를 지난다. 응답 경로에만 넣으면 마감 직후에 도착한 정상 응답이
    // 폐기로 세어진다. 목록 길이만큼만 늘어난다.
    resolved_.push_back(slot.transaction);
    slot.active = false;
    slot.sent = 0;
}

void StunClient::fail() {
    // 난수 실패처럼 다른 자리가 아직 떠 있는 채로 끝나는 경로가 있다. 타이머를 남기면
    // 끝난 단계가 계속 재시도를 보낸다.
    for (Slot& slot : slots_) {
        if (slot.active) {
            release(slot);
        }
    }
    phase_ = StunPhase::Failed;
    emit_stun_discovery_failed();
}

void StunClient::refill(Millis now) {
    if (done()) {
        return;
    }
    while (mappings_.size() < kStunRequiredResponses && in_flight() < kStunConcurrency &&
           next_server_ < servers_.size()) {
        Slot* slot = free_slot();
        if (slot == nullptr) {
            break;
        }
        const std::size_t index = next_server_++;
        if (!begin_query(*slot, index, now)) {
            // 난수를 얻지 못했다. 다음 서버도 같은 결과이므로 여기서 끝낸다.
            fail();
            return;
        }
    }

    if (mappings_.size() >= kStunRequiredResponses) {
        phase_ = StunPhase::Succeeded;
        // 남은 질의를 거둔다. 자리를 다시 채우는 규칙 때문에 성공 시점에 다른 서버로 보낸
        // 요청이 떠 있을 수 있고, 그 타이머를 두면 단계가 끝난 뒤에도 재시도가 나간다.
        for (Slot& slot : slots_) {
            if (slot.active) {
                release(slot);
            }
        }
        return;
    }
    if (in_flight() == 0) {
        // 목록을 다 썼는데 서로 다른 두 서버의 응답이 없다. 하나뿐이어도 실패다
        // (architecture.md 3.5 기동 입력의 서버 선택).
        fail();
        return;
    }
    phase_ = StunPhase::Running;
}

bool StunClient::start(Millis now) {
    if (phase_ != StunPhase::Idle) {
        return phase_ == StunPhase::Running;
    }
    if (servers_.size() < kStunRequiredResponses) {
        // 결과가 이미 정해졌다. 한 서버로는 서로 다른 두 응답이 나올 수 없다
        // (architecture.md 3.5 기동 입력). 마감 5초를 기다리지 않는다.
        fail();
        return false;
    }
    refill(now);
    return phase_ == StunPhase::Running;
}

void StunClient::on_timer(std::string_view timer_name, Millis now) {
    if (done()) {
        return;
    }

    if (const auto index = tail_index(timer_name, kRetryPrefix)) {
        Slot* slot = find_slot(*index);
        if (slot == nullptr) {
            return;  // 그 자리는 이미 끝났다
        }
        if (slot->sent >= kStunRetryIntervalsMs.size() + 1) {
            return;  // 재시도 3회를 다 썼다. 남은 것은 마감뿐이다
        }
        send_request(*slot);
        ++slot->sent;
        if (slot->sent < kStunRetryIntervalsMs.size() + 1) {
            const Millis next = kStunRetryIntervalsMs[slot->sent - 1];
            if (!timers_.add_once(retry_timer(*index), next, now)) {
                emit_timer_rejected(retry_timer(*index));
            }
        }
        return;
    }

    if (const auto index = tail_index(timer_name, kDeadlinePrefix)) {
        Slot* slot = find_slot(*index);
        if (slot == nullptr) {
            return;
        }
        const LogField fields[] = {field("server", servers_[*index].label)};
        emit(LogLevel::Warn, "stun.timeout", fields);
        release(*slot);
        // 그 서버의 마감이다. 목록의 다음 서버로 바꾸고 일정을 새로 시작한다
        // (protocol.md 11장 타이머).
        refill(now);
    }
}

void StunClient::on_datagram(const Endpoint& from, std::span<const std::byte> payload, Millis now) {
    if (done()) {
        return;
    }
    (void)from;  // 응답은 트랜잭션 ID 로만 잇는다 (헤더의 표)

    const auto peeked = peek_transaction_id(payload);
    if (!peeked) {
        counters_.increment(Counter::DropStunParse);
        return;
    }
    for (const TransactionId& id : resolved_) {
        if (id == *peeked) {
            return;  // 재시도가 부른 뒤늦은 응답이다. 폐기로 세지 않는다
        }
    }

    Slot* slot = nullptr;
    for (Slot& candidate : slots_) {
        if (candidate.active && candidate.transaction == *peeked) {
            slot = &candidate;
            break;
        }
    }
    if (slot == nullptr) {
        // 대기 중인 요청에 없는 트랜잭션 ID (protocol.md 13장 응답 검증).
        counters_.increment(Counter::DropStunParse);
        return;
    }

    const auto parsed = parse_binding_response(payload, slot->transaction);
    if (!parsed.ok()) {
        counters_.increment(Counter::DropStunParse);
        const LogField fields[] = {
            field("server", servers_[slot->server_index].label),
            field("reason", to_token(parsed.error)),
        };
        emit(LogLevel::Warn, "stun.rejected", fields);
        return;  // 재시도 일정은 그대로 둔다
    }

    const std::string& label = servers_[slot->server_index].label;
    if (parsed.response.type == StunMessageType::kBindingErrorResponse) {
        const LogField fields[] = {
            field("server", label),
            field("code", parsed.response.error_code
                              ? std::to_string(*parsed.response.error_code)
                              : std::string("-")),
        };
        emit(LogLevel::Warn, "stun.error", fields);
    } else {
        // architecture.md 9장의 stun.result. 필드 이름 둘은 그 표가 고정했다.
        const LogField fields[] = {
            field("server", label),
            field("mapped", parsed.response.mapped->to_string()),
        };
        emit(LogLevel::Info, "stun.result", fields);
        mappings_.push_back(StunMapping{label, *parsed.response.mapped});
    }

    release(*slot);
    refill(now);
}

}  // namespace sangtachi::network
