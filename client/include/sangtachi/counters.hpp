#pragma once

// 폐기·오류 카운터 (architecture.md 9장 로그 출력의 `counter` 이벤트).
//
// 이름의 출처는 세 문서다. 여기서 새로 짓지 않는다.
//   protocol.md 5.7          drop_roster_direction, drop_roster_flags, drop_roster_name, drop_roster_stale
//   protocol.md 9.4          dup_ack
//   protocol.md 10.4         endpoint_learned, path_probe_failed
//   protocol.md 13장         drop_stun_parse
//   protocol.md 6장·7장·8장   tx_err_*, tx_drop_*, 나머지 drop_* 의 대부분
//   concurrency.md 3장     drop_oversize_datagram, drop_inject_error, drop_no_sink, adapter_rx,
//                          rx_err_recv
//   concurrency.md 6장·8장  telemetry_queue_dropped, console_queue_dropped, control_queue_dropped
//
// 아직 아무도 올리지 않는 이름도 들고 있다. 9장이 "값이 0인 카운터도 함께 낸다" 로 정했고,
// 그래야 검증이 "오르지 않았다" 를 판정할 수 있다. 없는 것과 0 인 것은 다르다.
//
// **표는 두 범위다.** 프로세스 표 하나와 세션마다 표 하나이고, 둘 다 이 클래스(같은 이름 전체)다.
// **어느 표에 올리는지는 이름이 아니라 오르는 자리가 정한다.** 세션이 정해진 자리(8.1 검사 7 을
// 지난 뒤의 수신 판정, 그 세션의 송신과 타이머, 재협상, 엔드포인트 학습)는 그 세션의 표이고
// (peer/session.hpp 의 Session::counters), 정해지지 않은 자리(분류, 8.1 검사 1~7, STUN 송신 실패,
// 큐 카운터, 세션을 고르기 전의 송신 측 검사)는 프로세스 표다. 같은 이름이 두 범위에 다 생길 수
// 있다. 예: `drop_type_length` 는 8.1 6번이면 프로세스, 5.7 구조 검사면 세션이다.
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
    DropRosterDirection,
    DropRosterFlags,
    DropRosterName,
    DropRosterStale,
    DropShort,
    DropStaleEpoch,
    DropStunParse,
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
    DupAck,
    EndpointLearned,
    PathProbeFailed,
    RxErrRecv,
    TelemetryQueueDropped,
    TxDropBadSrc,
    TxDropNoRoute,
    TxDropNotConnected,
    TxDropNotIpv4,
    TxDropOversize,
    TxDropShort,
    TxErrAck,
    TxErrSend,
};

// 이름 순으로 적어 두었다. 값 자체에 의미를 두지 않는다.
inline constexpr std::size_t kCounterCount = 54;

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

// 세션 표의 `counter` 줄이 더 싣는 두 필드. 프로세스 표의 줄에는 없다.
struct CounterPeer {
    std::uint32_t peer_id = 0;
    // control_plane.md 2.7 형식의 표시 이름. 모르면 `-` 다.
    std::string_view peer_name = "-";
};

// 전량을 줄 목록으로 만든다. 순수 함수라 시험이 표준 오류를 가로채지 않아도 된다.
// peer 를 주면 세션 표의 줄이다. 줄마다 `name`, `value` 뒤에 `peer_id`, `peer_name` 을 싣는다.
[[nodiscard]] std::vector<std::string> format_all(const Counters& counters,
                                                  const CounterPeer* peer = nullptr);

// 전량을 낸다. 0 인 이름도 낸다. 시점은 셋이다 (architecture.md 9장). 증가할 때마다 내지 않는다.
//   세션이 종료 상태에 닿을 때          그 세션의 표
//   프로세스 종료 절차, 콘솔 `counters`  프로세스 표, 그리고 남아 있는 세션마다 그 세션의 표
void emit_all(const Counters& counters, const CounterPeer* peer = nullptr);

}  // namespace sangtachi
