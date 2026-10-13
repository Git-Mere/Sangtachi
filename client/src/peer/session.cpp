#include "sangtachi/peer/session.hpp"

#include "sangtachi/log.hpp"
#include "sangtachi/protocol_constants.hpp"
#include "sangtachi/tunnel/tunnel.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sangtachi::peer {
namespace {

using tunnel::PacketType;

tunnel::Nonce to_wire(const Nonce& nonce) noexcept {
    tunnel::Nonce out{};
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = nonce.bytes[i];
    }
    return out;
}

void emit_state(std::uint32_t peer_id, std::string_view from, std::string_view to) {
    // architecture.md 9장의 고정 키. from, to 는 9.1 상태 이름 그대로다.
    const LogField fields[] = {
        field("peer_id", static_cast<std::uint64_t>(peer_id)),
        field("from", from),
        field("to", to),
    };
    emit(LogLevel::Info, "session.state", fields);
}

}  // namespace

std::string_view to_token(SessionState state) noexcept {
    switch (state) {
        case SessionState::kPunching: return "PUNCHING";
        case SessionState::kHandshaking: return "HANDSHAKING";
        case SessionState::kConnected: return "CONNECTED";
        case SessionState::kFailed: return "FAILED";
        case SessionState::kClosed: return "CLOSED";
    }
    return "unknown";
}

std::string_view to_token(SessionFailure failure) noexcept {
    switch (failure) {
        case SessionFailure::kHolePunchTimeout: return "HOLE_PUNCH_TIMEOUT";
        case SessionFailure::kPeerHandshakeFailed: return "PEER_HANDSHAKE_FAILED";
        case SessionFailure::kTunnelDropped: return "TUNNEL_DROPPED";
    }
    return "unknown";
}

Session::Session(SessionConfig config, SessionIo io, Millis now)
    : config_(std::move(config)),
      io_(std::move(io)),
      budget_milli_(protocol::kUnverifiedTxBucket * 1000),  // 처음에는 가득 차 있다 (머리 표)
      budget_at_(now) {
    // 9.1 IDLE -> PUNCHING. IDLE 은 객체가 없으므로 on_transition 을 부르지 않고 줄만 낸다.
    emit_state(config_.peer_id, kIdleToken, to_token(SessionState::kPunching));
}

SessionState Session::state() const noexcept {
    if (terminal_) {
        return *terminal_;
    }
    // 9.2 CONNECTED <=> got_ack && sent_ack. 하나만 선 것이 HANDSHAKING 이다.
    if (got_ack_ && sent_ack_) {
        return SessionState::kConnected;
    }
    if (got_ack_ || sent_ack_) {
        return SessionState::kHandshaking;
    }
    return SessionState::kPunching;
}

bool Session::is_candidate(const network::Endpoint& endpoint) const noexcept {
    for (const auto& c : config_.candidates) {
        if (c == endpoint) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------- 송신

bool Session::send_packet(PacketType type, std::span<const std::byte> payload,
                          const network::Endpoint& to) {
    std::array<std::byte, protocol::kMaxDatagram> buffer{};
    // 4.4: 송신 직전에 현재 값을 할당하고 그 뒤 1을 더한다. 32비트에서 순환한다.
    const tunnel::HeaderFields fields{config_.local_peer_id, config_.local_epoch, next_seq_++};
    const std::size_t length = tunnel::build_packet(type, fields, payload, buffer);
    if (length == 0) {
        return false;  // 이 세션이 만드는 페이로드는 늘 규정 길이다. 닿지 않는 자리다
    }
    if (!io_.send(to, std::span<const std::byte>(buffer.data(), length))) {
        // 6장: 타입과 무관하게 tx_err_send. 이 세션의 송신이라 이 세션의 표다.
        counters_.increment(Counter::TxErrSend);
        return false;
    }
    return true;
}

bool Session::is_verified(const network::Endpoint& endpoint) const noexcept {
    return is_candidate(endpoint) || peer_endpoint_ == endpoint;
}

bool Session::spend_for(const network::Endpoint& to, Millis now) {
    if (is_verified(to)) {
        return true;  // 9.4: 후보 집합 안이나 peer_endpoint 로 가는 송신은 토큰을 쓰지 않는다
    }
    // 1ms 에 1/1000 토큰 5개. 용량을 넘겨 쌓지 않는다. 시계가 뒤로 가면 보충하지 않는다.
    constexpr std::uint64_t kCost = 1000;
    constexpr std::uint64_t kCapacity = protocol::kUnverifiedTxBucket * kCost;
    if (now > budget_at_) {
        const std::uint64_t room = kCapacity - budget_milli_;
        // elapsed 를 room 으로 먼저 자르면 곱이 넘치지 않는다.
        const std::uint64_t elapsed = std::min<std::uint64_t>(now - budget_at_, room);
        budget_milli_ += std::min(room, elapsed * protocol::kUnverifiedTxRefill);
        budget_at_ = now;
    }
    if (budget_milli_ < kCost) {
        counters_.increment(Counter::DropUnverifiedTx);
        return false;
    }
    budget_milli_ -= kCost;
    return true;
}

void Session::start_path_probe(const network::Endpoint& from, Millis now) {
    expire_probes(now);
    for (const auto& p : probes_) {
        if (p.to == from) {
            return;  // 경로당 검증 HELLO 1회. 자리는 5초를 채운다 (10.4 (c) 7번)
        }
    }
    // 1. 가득 차 있으면 등록만 건너뛴다. 가장 오래된 것을 밀어내지 않는다.
    if (probes_.size() >= protocol::kMaxProbePaths) {
        counters_.increment(Counter::DropProbeFull);
        return;
    }
    const auto nonce = generate_probe_nonce(io_.random);
    if (!nonce) {
        return;  // 머리 표: 뽑지 못하면 등록하지 않는다
    }
    // 2. 토큰. 없으면 spend_for 가 drop_unverified_tx 를 올렸다. 잠정 경로를 만들지 않는다.
    if (!spend_for(from, now)) {
        return;
    }
    // 3. 기록한다. peer_endpoint 는 그대로다. 4. 검증 HELLO 1회. 재전송하지 않는다.
    probes_.push_back(ProbePath{from, to_wire(*nonce), now, false});
    const auto payload =
        tunnel::encode_hello(tunnel::HelloPayload{probes_.back().nonce, config_.local_virtual_ip});
    (void)send_packet(PacketType::kHello, payload, from);
}

void Session::expire_probes(Millis now) {
    // 11장 만료 값과 같은 `<=` 다. 6. echo 가 없었으면 path_probe_failed. 7. 결과와 무관하게 비운다.
    for (auto it = probes_.begin(); it != probes_.end();) {
        if (it->registered + protocol::kProbeTtlMs <= now) {
            if (!it->answered) {
                counters_.increment(Counter::PathProbeFailed);
            }
            it = probes_.erase(it);
        } else {
            ++it;
        }
    }
}

void Session::send_to_peer(PacketType type, std::span<const std::byte> payload) {
    // 10.4: 학습했으면 peer_endpoint 하나, 아니면 후보 전체.
    if (peer_endpoint_) {
        (void)send_packet(type, payload, *peer_endpoint_);
        return;
    }
    for (const auto& c : config_.candidates) {
        (void)send_packet(type, payload, c);
    }
}

void Session::send_hello_round() {
    // 10.3, 9.5 9번: 후보 전체. 학습 여부와 무관하다. nonce 는 시도당 하나다 (5.1).
    const auto payload =
        tunnel::encode_hello(tunnel::HelloPayload{to_wire(config_.attempt_nonce),
                                                  config_.local_virtual_ip});
    for (const auto& c : config_.candidates) {
        (void)send_packet(PacketType::kHello, payload, c);
    }
}

void Session::send_hello_ack(const tunnel::Nonce& echo, const network::Endpoint& to, Millis now) {
    // 9.4: 무검증 목적지인데 토큰이 없으면 보내지 않는다. 보내지 않았으므로 sent_ack 도 없다.
    if (!spend_for(to, now)) {
        return;
    }
    const auto payload =
        tunnel::encode_hello(tunnel::HelloPayload{echo, config_.local_virtual_ip});
    // 9.2: sendto 성공 뒤에만 sent_ack. 실패하면 tx_err_ack (tx_err_send 는 send_packet 이 올렸다).
    if (send_packet(PacketType::kHelloAck, payload, to)) {
        sent_ack_ = true;
    } else {
        counters_.increment(Counter::TxErrAck);
    }
}

void Session::send_keepalive(Millis now) {
    send_to_peer(PacketType::kKeepalive, {});
    arm(keepalive_due_, keepalive_since_, now, protocol::kKeepaliveIntervalMs);  // 11장: 초기화 계기는 매 송신
}

void Session::expire_pings(Millis now) {
    // 5.5: 5초가 지난 항목을 제거한다. 비교는 11장 마감과 같은 `<=` 다.
    for (auto it = pending_pings_.begin(); it != pending_pings_.end();) {
        if (it->second.sent_ms + protocol::kPendingPingTtlMs <= now) {
            it = pending_pings_.erase(it);
        } else {
            ++it;
        }
    }
}

void Session::send_ping(Millis now) {
    arm(ping_due_, ping_since_, now, protocol::kPingIntervalMs);
    // 5.5: 삽입 전에 정리하고, 그래도 가득 차면 새 PING 을 보내지 않는다.
    expire_pings(now);
    if (pending_pings_.size() >= protocol::kMaxPendingPings) {
        return;
    }
    const std::uint64_t id = next_ping_id_++;
    pending_pings_[id] = PendingPing{now, io_.rtt_clock_us ? io_.rtt_clock_us() : 0};
    const auto payload = tunnel::encode_ping(id);
    send_to_peer(PacketType::kPing, payload);
}

// ---------------------------------------------------------------- 타이머

bool Session::start_punch(Millis now) {
    if (!active() || punch_started_) {
        return false;
    }
    punch_started_ = true;
    // 펀치 시작 전에 이미 CONNECTED 면 펀치 타이머를 걸지 않는다. 11장: 둘 다 CONNECTED 도달로
    // 취소되는 타이머다. 걸면 양쪽 증거가 선 세션이 10초 뒤 실패로 끝난다.
    if (state() == SessionState::kConnected) {
        return false;
    }
    // 10.3: 펀치 시작부터 200ms 간격. 마감은 실제 로컬 시작 시각으로부터 10초.
    send_hello_round();
    arm(hello_due_, hello_since_, now, protocol::kHelloRetransmitMs);
    arm(punch_deadline_, punch_since_, now, protocol::kPunchDeadlineMs);
    return true;
}

std::optional<Millis> Session::next_deadline() const noexcept {
    std::optional<Millis> earliest;
    for (const auto& d : {hello_due_, punch_deadline_, keepalive_due_, ping_due_, idle_deadline_}) {
        if (d && (!earliest || *d < *earliest)) {
            earliest = d;
        }
    }
    // 잠정 경로 자리의 만료. 표는 등록 순서라 앞이 가장 이르다. 종료 상태에서는 비어 있다.
    if (!probes_.empty()) {
        const Millis due = probes_.front().registered + protocol::kProbeTtlMs;
        if (!earliest || due < *earliest) {
            earliest = due;
        }
    }
    return earliest;
}

void Session::poll(Millis now) {
    if (!active()) {
        return;
    }
    expire_probes(now);
    // 마감이 먼저다. 끝난 세션이 같은 바퀴에 한 번 더 쏘지 않는다 (9.6 1번).
    if (punch_deadline_ && *punch_deadline_ <= now) {
        emit_tick("punch", now, punch_since_);
        // 9.6: 둘 다 미성립이면 HOLE_PUNCH_TIMEOUT, 하나만이면 PEER_HANDSHAKE_FAILED.
        // CONNECTED 진입이 마감을 지우므로 여기 오는 세션은 둘 다 선 일이 없다.
        fail(got_ack_ || sent_ack_ ? SessionFailure::kPeerHandshakeFailed
                                   : SessionFailure::kHolePunchTimeout,
             now);
        return;
    }
    if (idle_deadline_ && *idle_deadline_ <= now) {
        emit_tick("idle", now, idle_since_);
        fail(SessionFailure::kTunnelDropped, now);  // 9.6: CONNECTED 이후 유휴 타임아웃
        return;
    }
    // 밀린 주기를 따라잡지 않는다. 다음은 now + 간격이다 (timer.hpp 와 같은 규칙).
    if (hello_due_ && *hello_due_ <= now) {
        emit_tick("hello", now, hello_since_);
        send_hello_round();
        arm(hello_due_, hello_since_, now, protocol::kHelloRetransmitMs);
    }
    if (keepalive_due_ && *keepalive_due_ <= now) {
        emit_tick("keepalive", now, keepalive_since_);
        send_keepalive(now);
    }
    if (ping_due_ && *ping_due_ <= now) {
        emit_tick("ping", now, ping_since_);
        send_ping(now);
    }
}

void Session::arm(std::optional<Millis>& due, Millis& since, Millis now,
                  std::uint64_t interval) noexcept {
    due = now + interval;
    since = now;  // timer.tick 의 elapsed_ms 기준. 직전 만료 또는 시작(갱신) 시각이다
}

void Session::emit_tick(std::string_view kind, Millis now, Millis since) const {
    // architecture.md 9장: 만료하는 모든 타이머가 timer.tick(name, elapsed_ms)을 낸다. 이름은
    // STUN 타이머(stun.retry.<자리>)와 같은 꼴로 session.<종류>.<상대 peer_id 10진> 이다.
    const std::string name = std::string("session.") + std::string(kind) + "." +
                             std::to_string(config_.peer_id);
    const LogField fields[] = {
        field("name", name),
        field("elapsed_ms", static_cast<std::uint64_t>(now >= since ? now - since : 0)),
    };
    emit(LogLevel::Info, "timer.tick", fields);
}

void Session::stop_timers() noexcept {
    hello_due_.reset();
    punch_deadline_.reset();
    keepalive_due_.reset();
    ping_due_.reset();
    idle_deadline_.reset();
}

void Session::enter_connected(Millis now) {
    // 11장: HELLO 재전송과 펀치 마감은 CONNECTED 도달로 취소한다.
    hello_due_.reset();
    punch_deadline_.reset();
    // KEEPALIVE 는 진입 즉시 1회 보낸 뒤 15초 간격, 유휴 타임아웃은 진입에 시작, PING 은 첫
    // 송신이 진입 5초 뒤다. 4.5 손실 집계 초기화는 Phase 5 다 (15장).
    arm(idle_deadline_, idle_since_, now, protocol::kIdleTimeoutMs);
    arm(ping_due_, ping_since_, now, protocol::kPingIntervalMs);
    send_keepalive(now);
}

void Session::notify(SessionState from, SessionState to) {
    emit_state(config_.peer_id, to_token(from), to_token(to));
    if (io_.on_transition) {
        io_.on_transition(*this, from, to);
    }
}

void Session::settle(SessionState before, Millis now) {
    const SessionState after = state();
    if (after == before) {
        return;
    }
    if (after == SessionState::kConnected) {
        enter_connected(now);
    }
    notify(before, after);
}

void Session::enter_terminal(SessionState terminal, Millis now) {
    const SessionState before = state();
    stop_timers();
    probes_.clear();  // 머리 표: 끝난 세션은 검증할 경로가 없다
    terminal_ = terminal;
    terminal_since_ = now;
    notify(before, terminal);
}

void Session::fail(SessionFailure failure, Millis now) {
    // 9.6 의 세 가지. 1) 재전송과 세션 타이머를 멈춘다.
    stop_timers();
    // 2) 학습한 세션이면 CLOSE 0x03 을 그 주소로 1회. 학습 전이면 보내지 않는다 (5.6).
    if (peer_endpoint_) {
        const auto payload = tunnel::encode_close(tunnel::CloseReason::kSetupFailed);
        (void)send_packet(PacketType::kClose, payload, *peer_endpoint_);
    }
    failure_ = failure;
    // 3) 기록. architecture.md 9장 session.failed 와 session.state.
    const LogField fields[] = {
        field("peer_id", static_cast<std::uint64_t>(config_.peer_id)),
        field("code", to_token(failure)),
    };
    emit(LogLevel::Warn, "session.failed", fields);
    enter_terminal(SessionState::kFailed, now);
}

void Session::close_by_user(Millis now) {
    if (!active()) {
        return;  // 5.6: FAILED, CLOSED 는 보내지 않고 그대로 남는다
    }
    // 5.6 "보내는가" 의 기준은 상태가 아니라 학습 여부다. "어느 값" 은 상태가 정한다.
    if (peer_endpoint_) {
        const auto reason = state() == SessionState::kConnected ? tunnel::CloseReason::kNormal
                                                                : tunnel::CloseReason::kUserCancel;
        const auto payload = tunnel::encode_close(reason);
        (void)send_packet(PacketType::kClose, payload, *peer_endpoint_);
    }
    // 송신 성공 여부와 무관하게 CLOSED (5.6). 학습 전 세션도 CLOSED 로 둔다 (머리 표).
    enter_terminal(SessionState::kClosed, now);
}

// ---------------------------------------------------------------- 수신

RxResult Session::drop(Counter counter) {
    counters_.increment(counter);
    return RxResult{counter};
}

void Session::learn(const network::Endpoint& from) {
    if (peer_endpoint_ != from) {
        peer_endpoint_ = from;
        counters_.increment(Counter::EndpointLearned);  // 10.4 "엔드포인트가 바뀌면"
    }
}

void Session::renegotiate(const RxPacket& packet, const tunnel::Nonce& nonce, Millis now) {
    const SessionState before = state();
    ++renegotiations_;
    last_renegotiation_ = now;
    // 1. 상대의 이전 epoch 를 폐기 목록에 (4.3 등록 표 둘째 행)
    io_.retired->add(*pinned_, now);
    // 2. 증거를 지운다
    got_ack_ = false;
    sent_ack_ = false;
    // 3. 중복 억제 상태, pending_pings 를 초기화한다. 손실 집계는 Phase 5 다
    window_.reset();
    pending_pings_.clear();
    // 4. 새 epoch 고정. 5. 우리 epoch 와 nonce 는 그대로다 (config_ 를 건드리지 않는다)
    pinned_ = packet.header.session_epoch;
    // 6. 그 데이터그램의 출발지로 HELLO_ACK. peer_endpoint 는 바꾸지 않는다
    send_hello_ack(nonce, packet.from, now);
    // 10. KEEPALIVE·PING·유휴를 멈춘다
    keepalive_due_.reset();
    ping_due_.reset();
    idle_deadline_.reset();
    // 7, 10. 펀치 마감을 다시 잡고 HELLO 재전송(9번: 후보 전체)을 다시 시작한다. 펀치 시작
    // 전이면 두 타이머는 start_punch 가 건다 (머리 표, 9.1)
    if (punch_started_) {
        arm(punch_deadline_, punch_since_, now, protocol::kPunchDeadlineMs);
        send_hello_round();
        arm(hello_due_, hello_since_, now, protocol::kHelloRetransmitMs);
    }
    // 7. 상태는 플래그 개수가 정한다. 8. 후보 목록은 그대로 쓴다
    settle(before, now);
}

RxResult Session::receive(const RxPacket& packet, Millis now) {
    if (!active()) {
        return drop(Counter::DropTerminalState);  // 9.4.1. 터널이 먼저 거르지만 세션도 지킨다
    }
    const auto type = static_cast<PacketType>(packet.header.type);
    const std::uint32_t epoch = packet.header.session_epoch;
    bool renegotiated = false;
    tunnel::HelloPayload hello{};
    std::optional<std::size_t> probe;  // 8.2 probe nonce echo 행이면 그 잠정 경로

    // ---- 8.2 epoch 및 출발지 검사 ----
    if (type == PacketType::kHello || type == PacketType::kHelloAck) {
        const auto decoded = tunnel::decode_hello(packet.payload);
        if (!decoded.ok()) {
            return drop(Counter::DropTypeLength);  // 8.1 6번을 지난 패킷이면 닿지 않는다
        }
        hello = decoded.value;
        // virtual_ip 가 epoch 판정보다 먼저다. 뒤에 두면 위조 HELLO 하나가 epoch 를 고정한다.
        if (hello.virtual_ip != config_.peer_virtual_ip) {
            return drop(Counter::DropVipMismatch);
        }
    }
    if (type == PacketType::kHello) {
        // 고정 여부와 무관하게 수용한다. 출발지가 후보 집합에 있을 것을 요구하지 않는다.
        if (pinned_ && epoch != *pinned_) {
            // 9.3 "상대가 재시도했다". 9.5 의 상한 둘을 먼저 본다. 기준은 수용한 재협상의 시각이고
            // 비교는 11장 마감과 같다: 기준 + 1000 <= now 이면 지났다.
            if (last_renegotiation_ && now < *last_renegotiation_ + protocol::kMinRenegIntervalMs) {
                return drop(Counter::DropRenegRate);
            }
            if (renegotiations_ >= protocol::kMaxRenegotiations) {
                return drop(Counter::DropRenegLimit);
            }
            renegotiate(packet, hello.nonce, now);
            renegotiated = true;
        }
    } else if (type == PacketType::kHelloAck) {
        // probe nonce echo 행이 먼저다. nonce 와 출발지가 함께 맞아야 그 행이다. 아니면 아래 행들이다.
        expire_probes(now);
        for (std::size_t i = 0; i < probes_.size(); ++i) {
            if (probes_[i].nonce == hello.nonce && probes_[i].to == packet.from) {
                probe = i;
                break;
            }
        }
        if (probe) {
            if (!pinned_ || epoch != *pinned_) {
                return drop(Counter::DropStaleEpoch);  // 8.2: probe 는 고정 뒤에만 나가므로 미고정은 없다
            }
        } else {
            if (hello.nonce != to_wire(config_.attempt_nonce)) {
                return drop(Counter::DropBadNonce);
            }
            if (pinned_ && epoch != *pinned_) {
                return drop(Counter::DropStaleEpoch);  // 이미 끝난 시도의 epoch 아래서 got_ack 를 세우지 않는다
            }
        }
    } else {
        if (!pinned_) {
            return drop(Counter::DropNoEpoch);
        }
        if (epoch != *pinned_) {
            return drop(Counter::DropStaleEpoch);
        }
    }

    // ---- 8.3 중복 억제 ----
    // 미고정이면 창은 비어 있다(생성 때, 재협상 3번). 고정값이면 그 epoch 의 창이다.
    const tunnel::ReplayDecision decision = window_.accept(packet.header.sequence);
    if (decision.verdict == tunnel::ReplayVerdict::kDuplicate) {
        return drop(Counter::DropDuplicate);
    }
    if (decision.verdict == tunnel::ReplayVerdict::kTooOld) {
        return drop(Counter::DropTooOld);
    }

    // ---- 타입별 검사 (ADR 0021). 여기서 버려도 창에는 기록됐다 (8.3) ----
    std::uint64_t ping_id = 0;
    std::optional<tunnel::CloseReason> close_reason;
    std::optional<std::uint32_t> generation;
    switch (type) {
        case PacketType::kData: {
            if (state() != SessionState::kConnected) {
                return drop(Counter::DropDataEarly);  // 8.4 9번
            }
            const auto inner = tunnel::check_inner_ipv4(packet.payload, config_.peer_virtual_ip,
                                                        config_.local_virtual_ip);
            if (const auto counter = tunnel::to_counter(inner)) {
                return drop(*counter);
            }
            break;
        }
        case PacketType::kPing:
        case PacketType::kPong: {
            const auto decoded = tunnel::decode_ping(packet.payload);
            if (!decoded.ok()) {
                return drop(Counter::DropTypeLength);
            }
            ping_id = decoded.ping_id;
            if (type == PacketType::kPong) {
                // 5.5: 만료 항목을 먼저 치우고 조회한다. 없으면 drop_unmatched_pong.
                expire_pings(now);
                if (pending_pings_.find(ping_id) == pending_pings_.end()) {
                    return drop(Counter::DropUnmatchedPong);
                }
            }
            break;
        }
        case PacketType::kClose: {
            const auto decoded = tunnel::decode_close(packet.payload);
            if (!decoded.ok()) {
                return drop(tunnel::to_counter(decoded.error).value_or(Counter::DropBadReason));
            }
            close_reason = decoded.reason;
            break;
        }
        case PacketType::kRoster: {
            const auto decoded = tunnel::decode_roster(
                packet.payload, tunnel::RosterContext{config_.peer_is_host, last_generation_});
            if (!decoded.ok()) {
                return drop(tunnel::to_counter(decoded.error).value_or(Counter::DropTypeLength));
            }
            generation = decoded.value.generation;
            break;
        }
        case PacketType::kHello:
        case PacketType::kHelloAck:
        case PacketType::kKeepalive:
            break;
    }

    // ---- 부수 효과. 여기부터는 해당 타입의 모든 검증을 통과한 패킷이다 ----
    const SessionState before = state();
    if (renegotiated) {
        // 재협상이 6번까지 이미 했다. 이 HELLO 로 학습하지 않는다(9.5 6번). 출발지가 검증된 주소가
        // 아니면 잠정 경로로 올릴 뿐 재전송 대상에 넣지 않는다(9.5 9번). 상태는 이제 CONNECTED 가
        // 아니므로 유휴 타이머도 없다.
        if (!is_verified(packet.from)) {
            start_path_probe(packet.from, now);
        }
        return RxResult{};
    }
    if (!pinned_) {
        pinned_ = epoch;  // 9.3: 미고정 상태의 유효 HELLO 또는 nonce 일치 HELLO_ACK
    }
    // 10.4 학습. (a) 통과, (b) 전진, (c) 검증된 주소이거나 이 패킷이 그 주소의 probe echo 다.
    // (c) 만 못 했으면 잠정 경로로 올린다. 올리는 일은 아래 9.4 동작(회신) 뒤다 (머리 표).
    bool probe_source = false;
    if (probe) {
        probes_[*probe].answered = true;
        if (decision.advanced()) {
            learn(packet.from);  // 10.4 (c) 5번
        }
    } else if (decision.advanced()) {
        if (is_verified(packet.from)) {
            learn(packet.from);
        } else {
            probe_source = true;
        }
    }
    if (before == SessionState::kConnected) {
        arm(idle_deadline_, idle_since_, now, protocol::kIdleTimeoutMs);  // 9.4, 11장 유휴 타이머 갱신
    }

    bool no_sink = false;
    switch (type) {
        case PacketType::kHello:
            // 9.4: 그 데이터그램의 출발지로 응답. CONNECTED 에서도 한다(상대 ACK 유실).
            send_hello_ack(hello.nonce, packet.from, now);
            break;
        case PacketType::kHelloAck:
            if (probe) {
                break;  // 9.4 probe 행: 그 잠정 경로만 검증한다. got_ack 와 epoch 는 그대로다
            }
            if (before == SessionState::kConnected) {
                counters_.increment(Counter::DupAck);  // 9.4: 플래그는 그대로
            } else {
                got_ack_ = true;
            }
            break;
        case PacketType::kPing:
            // 9.4: 무검증 목적지로 가는 PONG 은 공통 예산을 쓴다.
            if (spend_for(packet.from, now)) {
                const auto payload = tunnel::encode_ping(ping_id);
                (void)send_packet(PacketType::kPong, payload, packet.from);
            }
            break;
        case PacketType::kPong: {
            const auto it = pending_pings_.find(ping_id);
            if (io_.rtt_clock_us) {
                last_rtt_us_ = io_.rtt_clock_us() - it->second.sent_us;
                ++rtt_samples_;
            }
            pending_pings_.erase(it);  // 각 ping_id 는 한 번만 유효하다
            break;
        }
        case PacketType::kClose:
            // 5.6: 세션은 CLOSED 로 남긴다. 라우팅 칸은 터널의 종료 처리기가 비운다.
            // 잠정 경로를 올리지 않는다 (머리 표).
            peer_close_reason_ = close_reason;
            enter_terminal(SessionState::kClosed, now);
            return RxResult{};
        case PacketType::kRoster:
            last_generation_ = generation;  // 명부 내용을 쓰는 일은 3차 I 다
            break;
        case PacketType::kData:
            if (io_.data_sink) {
                io_.data_sink(packet.payload);
            } else {
                no_sink = true;  // concurrency.md 3장 제품 빌드. 판정은 아래에서 돌려준다
            }
            break;
        case PacketType::kKeepalive:
            break;
    }
    if (probe_source) {
        start_path_probe(packet.from, now);
    }
    settle(before, now);
    if (no_sink) {
        return drop(Counter::DropNoSink);
    }
    return RxResult{};
}

}  // namespace sangtachi::peer
