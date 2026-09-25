#pragma once

// 폐기·오류 카운터 (architecture.md 9장 로그 출력의 `counter` 이벤트).
//
// 이름의 출처는 세 문서다. 여기서 새로 짓지 않는다.
//   protocol.md 6장·7장·8장   tx_err_*, drop_* 의 대부분
//   concurrency.md 3장     drop_oversize_datagram, drop_inject_error, drop_no_sink, adapter_rx
//   concurrency.md 6장·8장  telemetry_queue_dropped, console_queue_dropped, control_queue_dropped
//
// 아직 아무도 올리지 않는 이름도 들고 있다. 9장이 "값이 0인 카운터도 함께 낸다" 로 정했고,
// 그래야 검증이 "오르지 않았다" 를 판정할 수 있다. 없는 것과 0 인 것은 다르다.
//
// **이 표는 `[loop]` 가 단독 소유한다** (concurrency.md 5장 상태 소유). 예외는
// `console_queue_dropped` 하나이고, 그것은 `[console]` 이 올리는 원자 변수라 큐 쪽이
// 들고 있다. `[loop]` 가 낼 때 그 값을 읽어 `set` 으로 이 표에 반영한다 (concurrency.md 6장).

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sangtachi {

enum class Counter : std::size_t {
    AdapterRx,
    ConsoleQueueDropped,
    ControlQueueDropped,
    DropBadNonce,
    DropBadReason,
    DropBadSource,
    DropDataEarly,
    DropDuplicate,
    DropInjectError,
    DropInnerChecksum,
    DropInnerDst,
    DropInnerIhl,
    DropInnerLength,
    DropInnerNotIpv4,
    DropInnerSrc,
    DropLength,
    DropMagic,
    DropNoEpoch,
    DropNoSink,
    DropOversizeDatagram,
    DropProbeFull,
    DropRenegLimit,
    DropRenegRate,
    DropRetiredEpoch,
    DropShort,
    DropStaleEpoch,
    DropTerminalState,
    DropTooOld,
    DropTypeLength,
    DropUnclassified,
    DropUnknownPeer,
    DropUnknownType,
    DropUnmatchedPong,
    DropUnverifiedTx,
    DropVersion,
    DropVipMismatch,
    TelemetryQueueDropped,
    TxErrAck,
    TxErrSend,
};

// 이름 순으로 적어 두었다. 값 자체에 의미를 두지 않는다.
inline constexpr std::size_t kCounterCount = 39;

// architecture.md 9장의 `counter` 이벤트가 `name` 으로 싣는 문자열.
[[nodiscard]] std::string_view to_token(Counter counter) noexcept;

class Counters {
public:
    void increment(Counter counter) noexcept { add(counter, 1); }
    void add(Counter counter, std::uint64_t amount) noexcept;

    // `[console]` 이 올린 원자 변수를 `[loop]` 가 읽어 넣는 자리다 (concurrency.md 6장).
    void set(Counter counter, std::uint64_t value) noexcept;

    [[nodiscard]] std::uint64_t value(Counter counter) const noexcept;

private:
    std::array<std::uint64_t, kCounterCount> values_{};
};

// 전량을 줄 목록으로 만든다. 순수 함수라 시험이 표준 오류를 가로채지 않아도 된다.
[[nodiscard]] std::vector<std::string> format_all(const Counters& counters);

// 전량을 낸다. 시점은 셋이다. 세션이 종료 상태에 닿을 때, 프로세스 종료 절차,
// 콘솔 명령 (architecture.md 9장). 증가할 때마다 내지 않는다.
void emit_all(const Counters& counters);

}  // namespace sangtachi
