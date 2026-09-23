#include "hamychi/counters.hpp"

#include "hamychi/log.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace hamychi {
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
        case Counter::DropShort: return "drop_short";
        case Counter::DropStaleEpoch: return "drop_stale_epoch";
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
        case Counter::TelemetryQueueDropped: return "telemetry_queue_dropped";
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

std::vector<std::string> format_all(const Counters& counters) {
    std::vector<std::string> lines;
    lines.reserve(kCounterCount);
    for (std::size_t i = 0; i < kCounterCount; ++i) {
        const auto counter = static_cast<Counter>(i);
        const LogField fields[] = {
            field("name", to_token(counter)),
            field("value", counters.value(counter)),
        };
        lines.push_back(format_line(LogLevel::Info, "counter", fields));
    }
    return lines;
}

void emit_all(const Counters& counters) {
    for (std::size_t i = 0; i < kCounterCount; ++i) {
        const auto counter = static_cast<Counter>(i);
        const LogField fields[] = {
            field("name", to_token(counter)),
            field("value", counters.value(counter)),
        };
        emit(LogLevel::Info, "counter", fields);
    }
}

}  // namespace hamychi
