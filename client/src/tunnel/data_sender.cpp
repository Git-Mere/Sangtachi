#include "sangtachi/tunnel/data_sender.hpp"

#include "sangtachi/log.hpp"
#include "sangtachi/network/endpoint.hpp"
#include "sangtachi/protocol_constants.hpp"

#include <algorithm>
#include <charconv>
#include <string_view>
#include <utility>

namespace sangtachi::tunnel {
namespace {

void put(std::vector<std::byte>& p, std::size_t at, std::uint64_t value, std::size_t width) {
    // 네트워크 바이트 순서. size 밖의 칸은 쓰지 않는다 (머리 표: 100 미만은 자른다).
    for (std::size_t i = 0; i < width; ++i) {
        if (at + i < p.size()) {
            p[at + i] = static_cast<std::byte>((value >> (8 * (width - 1 - i))) & 0xFFu);
        }
    }
}

template <typename T>
std::optional<T> parse_number(std::string_view text) {
    T value{};
    const auto* end = text.data() + text.size();
    const auto parsed = std::from_chars(text.data(), end, value);
    if (text.empty() || parsed.ec != std::errc() || parsed.ptr != end) {
        return std::nullopt;
    }
    return value;
}

void reject(std::string_view reason) {
    const LogField fields[] = {field("reason", reason)};
    emit(LogLevel::Warn, "console.rejected", fields);
}

}  // namespace

std::vector<std::byte> make_synthetic(std::size_t size, std::uint32_t src, std::uint32_t dst,
                                      std::uint32_t number, std::uint64_t sent_ms) {
    std::vector<std::byte> p(size, kMarkerRequest);  // 41.. 의 채움과 40 의 요청 표시
    put(p, 0, 0x45, 1);
    put(p, 1, 0x00, 1);
    put(p, 2, size, 2);           // total_length
    put(p, 4, 0, 2);              // id
    put(p, 6, 0, 2);              // flags + fragment offset
    put(p, 8, 64, 1);             // TTL
    put(p, 9, 17, 1);             // UDP
    put(p, 10, 0, 2);             // 체크섬 자리. 아래에서 채운다
    put(p, 12, src, 4);
    put(p, 16, dst, 4);
    put(p, 20, kSyntheticPort, 2);
    put(p, 22, kSyntheticPort, 2);
    put(p, 24, size - 20, 2);     // UDP length = 헤더 8 + 페이로드
    put(p, 26, 0, 2);             // UDP 체크섬 0
    put(p, 28, number, 4);
    put(p, 32, sent_ms, 8);
    fill_ipv4_checksum(p);
    return p;
}

void fill_ipv4_checksum(std::span<std::byte> packet) noexcept {
    if (packet.size() < 20) {
        return;
    }
    packet[10] = std::byte{0};
    packet[11] = std::byte{0};
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i < 20; i += 2) {
        sum += (std::to_integer<std::uint32_t>(packet[i]) << 8) |
               std::to_integer<std::uint32_t>(packet[i + 1]);
    }
    while ((sum >> 16) != 0) {
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }
    const auto checksum = static_cast<std::uint16_t>(~sum & 0xFFFFu);
    packet[10] = static_cast<std::byte>(checksum >> 8);
    packet[11] = static_cast<std::byte>(checksum & 0xFFu);
}

std::optional<SendArgs> parse_send_args(std::string_view rest) {
    std::vector<std::string_view> words;
    while (!rest.empty()) {
        const auto start = rest.find_first_not_of(' ');
        if (start == std::string_view::npos) {
            break;
        }
        rest.remove_prefix(start);
        const auto end = std::min(rest.find(' '), rest.size());
        words.push_back(rest.substr(0, end));
        rest.remove_prefix(end);
    }
    if (words.empty() || words.size() > 3) {
        return std::nullopt;
    }
    SendArgs args;
    const auto count = parse_number<std::uint32_t>(words[0]);
    if (!count || *count < 1 || *count > kSendMaxCount) {
        return std::nullopt;
    }
    args.count = *count;
    std::size_t i = 1;
    if (i < words.size() && words[i].find('.') == std::string_view::npos) {
        const auto size = parse_number<std::size_t>(words[i]);
        if (!size || *size < kSyntheticMinSize || *size > protocol::kMaxInner) {
            return std::nullopt;
        }
        args.size = *size;
        ++i;
    }
    if (i < words.size()) {
        args.to = network::parse_ipv4(words[i]);
        if (!args.to) {
            return std::nullopt;
        }
        ++i;
    }
    if (i != words.size()) {
        return std::nullopt;  // 점이 든 낱말 뒤에 또 낱말이 있다
    }
    return args;
}

DataSender::DataSender(Tunnel& tunnel, std::function<Millis()> clock)
    : tunnel_(tunnel), clock_(std::move(clock)) {}

void DataSender::on_command(std::string_view rest) {
    const auto args = parse_send_args(rest);
    if (!args) {
        reject("bad_send_args");
        return;
    }
    if (job_) {
        reject("send_busy");
        return;
    }
    std::uint32_t to = 0;
    if (args->to) {
        to = *args->to;
        // 칸에 세션이 없으면 거르지 않는다. 8.5 의 tx_drop_no_route 가 센다 (머리).
        const peer::Session* session = tunnel_.route(to);
        if (session != nullptr && session->state() != peer::SessionState::kConnected) {
            reject("not_connected");
            return;
        }
    } else {
        const auto peers = tunnel_.connected_peers();
        if (peers.empty()) {
            reject("not_connected");
            return;
        }
        if (peers.size() > 1) {
            reject("ambiguous_destination");
            return;
        }
        to = peers.front();
    }
    job_ = Job{*args, to, 0, 0};
}

void DataSender::enqueue(std::vector<std::byte> inner) {
    queued_.push_back(std::move(inner));
}

void DataSender::finish(bool failed) {
    const LogField fields[] = {
        field("sent", static_cast<std::uint64_t>(job_->sent)),
        field("requested", static_cast<std::uint64_t>(job_->args.count)),
        field("reason", std::string_view(failed ? "send_failed" : "-")),
    };
    emit(failed ? LogLevel::Warn : LogLevel::Info, "console.send", fields);
    job_.reset();
}

bool DataSender::pump(std::size_t budget) {
    std::size_t used = 0;
    while (used < budget && !queued_.empty()) {
        (void)tunnel_.send_data(queued_.front());  // 에코는 실패해도 작업을 멈추지 않는다 (머리 표)
        queued_.pop_front();
        ++used;
    }
    while (used < budget && job_) {
        const auto src = tunnel_.local_virtual_ip().value_or(0);  // 모르면 8.5 가 tx_drop_bad_src
        const auto packet = make_synthetic(job_->args.size, src, job_->to, job_->next,
                                           clock_ ? clock_() : 0);
        ++job_->next;
        ++used;
        const TxOutcome outcome = tunnel_.send_data(packet);
        if (outcome == TxOutcome::kSendFailed) {
            finish(true);
            break;
        }
        if (outcome == TxOutcome::kSent) {
            ++job_->sent;
        }
        if (job_->next == job_->args.count) {
            finish(false);
        }
    }
    return busy();
}

}  // namespace sangtachi::tunnel
