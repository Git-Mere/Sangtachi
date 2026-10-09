#include "sangtachi/counters.hpp"

#include "sangtachi/log.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sangtachi {
namespace {

constexpr std::size_t index_of(Counter counter) noexcept {
    return static_cast<std::size_t>(counter);
}

}  // namespace

std::string_view to_token(Counter counter) noexcept {
    switch (counter) {
        case Counter::AdapterRx: return "adapter_rx";
        case Counter::ConsoleQueueDropped: return "console_queue_dropped";
        case Counter::ControlQueueDropped: return "control_queue_dropped";
        case Counter::DropBadNonce: return "drop_bad_nonce";
        case Counter::DropBadReason: return "drop_bad_reason";
        case Counter::DropBadSource: return "drop_bad_source";
        case Counter::DropDataEarly: return "drop_data_early";
        case Counter::DropDuplicate: return "drop_duplicate";
        case Counter::DropInjectError: return "drop_inject_error";
        case Counter::DropInnerChecksum: return "drop_inner_checksum";
        case Counter::DropInnerDst: return "drop_inner_dst";
        case Counter::DropInnerIhl: return "drop_inner_ihl";
        case Counter::DropInnerLength: return "drop_inner_length";
        case Counter::DropInnerNotIpv4: return "drop_inner_not_ipv4";
        case Counter::DropInnerSrc: return "drop_inner_src";
        case Counter::DropLength: return "drop_length";
        case Counter::DropMagic: return "drop_magic";
        case Counter::DropNoEpoch: return "drop_no_epoch";
        case Counter::DropNoSink: return "drop_no_sink";
        case Counter::DropOversizeDatagram: return "drop_oversize_datagram";
        case Counter::DropProbeFull: return "drop_probe_full";
        case Counter::DropRenegLimit: return "drop_reneg_limit";
        case Counter::DropRenegRate: return "drop_reneg_rate";
        case Counter::DropRetiredEpoch: return "drop_retired_epoch";
        case Counter::DropRosterDirection: return "drop_roster_direction";
        case Counter::DropRosterFlags: return "drop_roster_flags";
        case Counter::DropRosterName: return "drop_roster_name";
        case Counter::DropRosterStale: return "drop_roster_stale";
        case Counter::DropShort: return "drop_short";
        case Counter::DropStaleEpoch: return "drop_stale_epoch";
        case Counter::DropStunParse: return "drop_stun_parse";
        case Counter::DropTerminalState: return "drop_terminal_state";
        case Counter::DropTooOld: return "drop_too_old";
        case Counter::DropTypeLength: return "drop_type_length";
        case Counter::DropUnclassified: return "drop_unclassified";
        case Counter::DropUnknownPeer: return "drop_unknown_peer";
        case Counter::DropUnknownType: return "drop_unknown_type";
        case Counter::DropUnmatchedPong: return "drop_unmatched_pong";
        case Counter::DropUnverifiedTx: return "drop_unverified_tx";
        case Counter::DropVersion: return "drop_version";
        case Counter::DropVipMismatch: return "drop_vip_mismatch";
        case Counter::DupAck: return "dup_ack";
        case Counter::EndpointLearned: return "endpoint_learned";
        case Counter::RxErrRecv: return "rx_err_recv";
        case Counter::TelemetryQueueDropped: return "telemetry_queue_dropped";
        case Counter::TxDropBadSrc: return "tx_drop_bad_src";
        case Counter::TxDropNoRoute: return "tx_drop_no_route";
        case Counter::TxDropNotConnected: return "tx_drop_not_connected";
        case Counter::TxDropNotIpv4: return "tx_drop_not_ipv4";
        case Counter::TxDropOversize: return "tx_drop_oversize";
        case Counter::TxDropShort: return "tx_drop_short";
        case Counter::TxErrAck: return "tx_err_ack";
        case Counter::TxErrSend: return "tx_err_send";
    }
    return "unknown";
}

void Counters::add(Counter counter, std::uint64_t amount) noexcept {
    const std::size_t i = index_of(counter);
    if (i >= values_.size()) {
        return;
    }
    // 넘치면 그 자리에 멈춘다. 감싸 돌면 "오르지 않았다" 를 보는 검증이 통과한다.
    if (values_[i] > UINT64_MAX - amount) {
        values_[i] = UINT64_MAX;
        return;
    }
    values_[i] += amount;
}

void Counters::set(Counter counter, std::uint64_t value) noexcept {
    const std::size_t i = index_of(counter);
    if (i < values_.size()) {
        values_[i] = value;
    }
}

std::uint64_t Counters::value(Counter counter) const noexcept {
    const std::size_t i = index_of(counter);
    return i < values_.size() ? values_[i] : 0;
}

namespace {

// 한 줄의 필드. 세션 표면 peer_id 와 peer_name 이 뒤에 붙는다.
template <typename Sink>
void each_line(const Counters& counters, const CounterPeer* peer, Sink&& sink) {
    for (std::size_t i = 0; i < kCounterCount; ++i) {
        const auto counter = static_cast<Counter>(i);
        if (peer != nullptr) {
            const LogField fields[] = {
                field("name", to_token(counter)),
                field("value", counters.value(counter)),
                field("peer_id", static_cast<std::uint64_t>(peer->peer_id)),
                field("peer_name", peer->peer_name.empty() ? std::string_view("-") : peer->peer_name),
            };
            sink(std::span<const LogField>(fields));
        } else {
            const LogField fields[] = {
                field("name", to_token(counter)),
                field("value", counters.value(counter)),
            };
            sink(std::span<const LogField>(fields));
        }
    }
}

}  // namespace

std::vector<std::string> format_all(const Counters& counters, const CounterPeer* peer) {
    std::vector<std::string> lines;
    lines.reserve(kCounterCount);
    each_line(counters, peer, [&lines](std::span<const LogField> fields) {
        lines.push_back(format_line(LogLevel::Info, "counter", fields));
    });
    return lines;
}

void emit_all(const Counters& counters, const CounterPeer* peer) {
    each_line(counters, peer, [](std::span<const LogField> fields) {
        emit(LogLevel::Info, "counter", fields);
    });
}

}  // namespace sangtachi
