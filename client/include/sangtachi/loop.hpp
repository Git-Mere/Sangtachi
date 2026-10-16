#pragma once

// 이벤트 루프 (concurrency.md 2장 대기, 3장 루프 한 바퀴, 4장 타이머).
//
// `[loop]` 는 프로세스 주 스레드다. UDP 수신, 콘솔 명령, 타이머, 모든 송신을 혼자 한다.
// 터널 상태를 건드리는 코드가 한 스레드에서만 도는 것이 이 설계의 전부다 (concurrency.md).

#include "sangtachi/adapter.hpp"
#include "sangtachi/console.hpp"
#include "sangtachi/control/channel.hpp"
#include "sangtachi/control/lobby.hpp"
#include "sangtachi/counters.hpp"
#include "sangtachi/network/endpoint.hpp"
#include "sangtachi/network/udp_socket.hpp"
#include "sangtachi/peer/candidates.hpp"
#include "sangtachi/timer.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace sangtachi {

// 한 바퀴당 소스별 비우기 상한 (concurrency.md 3장 루프 한 바퀴).
//
// 1472바이트 기준 94KB 다. 예산에 걸려 멈추면 busy 가 참이 되어 다음 대기가 타임아웃 0
// 으로 즉시 반환하고, 남은 것은 다음 바퀴에 처리된다. **그 사이에 타이머가 한 번 돈다.**
inline constexpr std::size_t kMaxDrain = 64;

// 시험 빌드의 주기 타이머 이름 (architecture.md 9장의 timer.tick).
inline constexpr std::string_view kProbeTimerName = "probe200";
inline constexpr Millis kProbeTimerIntervalMs = 200;

enum class DrainOutcome {
    Empty,   // 소스가 비었다
    Budget,  // 예산을 다 썼다. 아직 남았을 수 있다
};

// 수신 분류의 결과 (protocol.md 7장 수신 분류).
enum class RxClass {
    BadSource,        // 출발지 위생에 걸렸다. drop_bad_source
    Stun,             // STUN 메시지. 트랜잭션 ID 로 대기 중인 요청과 매칭 (13장)
    TunnelCandidate,  // 터널 후보. 8장 검증 파이프라인으로
    Unclassified,     // 그 외. drop_unclassified
};

// 데이터그램 하나를 분류한다. **출발지 위생이 분류보다 먼저다** (protocol.md 7장).
//
// 순수 함수다. 카운터도 로그도 여기 없다. 부르는 쪽이 결과로 카운터를 가른다.
//
// ## 출발지 위생 (7장의 표)
//
// | 출발지 | 판정 |
// |--------|------|
// | 멀티캐스트 `224.0.0.0/4` | 폐기 |
// | 제한 브로드캐스트 `255.255.255.255` | 폐기 |
// | 미지정 `0.0.0.0` | 폐기 |
// | 포트 0 | 폐기 |
// | 루프백 `127.0.0.0/8` | **통과** |
// | 그 외 | 통과 |
//
// ## 서브넷 브로드캐스트 (7장의 두 번째 표)
//
// | 무엇 | 판정 |
// |------|------|
// | 자기 서브넷의 브로드캐스트. own_subnets 가 막는 주소 (OwnSubnets::blocks_broadcast) | 폐기 |
// | 원격 서브넷의 브로드캐스트 | 통과. 넷마스크를 모르므로 막지 못한다. 7장이 남는 위험으로 적었다 |
//
// own_subnets 는 10.1 후보 수집과 위생의 자기 서브넷 집합이다. 소유는 [loop] 이고(concurrency.md
// 5장) 첫 수집 전에는 비어 있다. 비어 있는 동안 이 행은 아무것도 막지 않는다.
//
// **루프백을 통과시키는 것은 10.1 후보 수집과 위생의 표와 다르다.** 그 절은 우리가 먼저
// 보낼 목적지 목록을 판정하고 여기는 받은 데이터그램의 출발지를 판정한다. 7장이 두 표를
// 합치지 말라고 적었다.
[[nodiscard]] RxClass classify_datagram(const network::Endpoint& from,
                                        std::span<const std::byte> payload,
                                        const peer::OwnSubnets& own_subnets) noexcept;

// 분류를 지난 데이터그램 하나를 넘겨받는 자리.
using DatagramHandler =
    std::function<void(const network::Endpoint& from, std::span<const std::byte> payload)>;

struct LoopOptions {
    std::optional<network::Endpoint> peer;  // architecture.md 3.5 의 --peer
    bool probe_timer = false;               // 시험 빌드의 probe200 타이머
    // 종료 이벤트. 비워 두면 루프가 만든다. 주면 루프는 그것을 **복제해서** 든다.
    //
    // main 은 제어 서버 주소를 UDP bind 보다 먼저 해석한다 (control_plane.md 8.4 의 2번이
    // 3번 앞이다). 그 사이의 Ctrl+C 가 종료 절차를 타려면 종료 이벤트와 콘솔 제어 핸들러가
    // 루프보다 먼저 있어야 한다. 그래서 main 이 이벤트를 만들어 넘긴다.
    void* shutdown_event = nullptr;
};

class EventLoop {
public:
    // console_event 의 소유는 ConsoleSession 이 한다. 이 객체는 빌려 쓰기만 한다.
    // 그 세션은 `[console]` 스레드와 함께 이 객체보다 오래 산다.
    EventLoop(network::UdpSocket socket, Counters& counters, ConsoleQueue& console,
              void* console_event, LoopOptions options);
    ~EventLoop();

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;
    EventLoop(EventLoop&&) = delete;
    EventLoop& operator=(EventLoop&&) = delete;

    // 만든 뒤 한 번 확인한다.
    //
    // 대기 배열에 들어가는 핸들이 모두 살아 있어야 한다. 빈자리에 널을 넣으면
    // WaitForMultipleObjects 가 WAIT_FAILED 를 낸다 (concurrency.md 2장 대기).
    // 종료·UDP·콘솔의 세 핸들은 전부 항상 있으므로, 여기서 한 번 확인하면 배열은 구성상
    // 조밀하다. 제어 응답 이벤트(순위 4)는 채널을 붙였을 때만 배열에 들어가므로 그때만 본다.
    [[nodiscard]] bool valid() const noexcept {
        return shutdown_event_ != nullptr && console_event_ != nullptr &&
               socket_.read_event() != nullptr &&
               (control_ == nullptr || control_->response_event() != nullptr) &&
               (adapter_ == nullptr || adapter_->read_event() != nullptr);
    }

    // 가상 어댑터를 붙인다 (concurrency.md 2장 대기의 순위 2, 3장의 drain_wintun).
    //
    // 소유하지 않는다. 어댑터는 이 루프보다 오래 산다. 붙이지 않은 루프는 순위 2 없이 돈다.
    // `--no-adapter` 실행과 Phase 1~5 의 시험이 그 모양이다. 붙인 뒤에는 valid() 를 다시 본다.
    void attach_adapter(adapter::Port* port) noexcept { adapter_ = port; }

    // drain_wintun 이 읽은 패킷을 하나씩 넘길 곳. 받은 바이트열은 그 호출 안에서만 유효하다.
    // 비워 두면 세고 버린다 (concurrency.md 3장 "route_and_send 는 Phase 7 작업이다").
    void set_adapter_handler(std::function<void(std::span<const std::byte> packet)> handler) {
        on_adapter_packet_ = std::move(handler);
    }

    // 어댑터에 패킷 하나를 넣는다. 넣었으면 참이다. 실패는 여기서 센다 (concurrency.md 3장 inject).
    // 어댑터가 붙지 않았으면 drop_inject_error 다.
    bool inject_packet(std::span<const std::byte> packet);

    // `[control]` 의 채널을 붙인다 (concurrency.md 2장 대기의 순위 4, 3장의 drain_control).
    //
    // 소유하지 않는다. 채널은 이 루프보다 오래 산다. concurrency.md 7장 종료가 join 을 정리
    // 완료 뒤에 두므로 채널은 루프가 소멸한 뒤에야 끝난다. 붙인 뒤에는 valid() 를 다시 본다.
    //
    // 붙이지 않은 루프는 순위 4 없이 돈다. Phase 1~2 의 시험이 그 모양이다. 제품 경로는
    // main 이 늘 붙인다 (concurrency.md 2장의 "Phase 3~5 배열 길이 4").
    void attach_control(control::ControlChannel* channel) noexcept { control_ = channel; }

    // drain_control 이 꺼낸 응답을 하나씩 넘길 곳. 비워 두면 꺼내서 버린다.
    //
    // 응답을 세션 상태에 반영하는 것이 이 자리다 (concurrency.md 8장). 해석(ops.hpp 의
    // interpret_*)도 여기서 한다.
    void set_control_handler(std::function<void(control::ControlResponse)> handler) {
        on_control_ = std::move(handler);
    }

    [[nodiscard]] control::ControlChannel* control() const noexcept { return control_; }

    // 로비 명령(`host`, `join`, `leave`, `name`)을 넘길 곳 (architecture.md 3.5 로비 명령).
    //
    // 콘솔 줄은 control::parse_lobby_command 가 먼저 본다. 로비 명령이면 여기로 가고, 아니면
    // quit, counters, raw 의 기존 경로다. 비워 두면 로비 명령도 기존 경로로 가서
    // unknown_command 가 된다.
    void set_lobby_command_handler(std::function<void(const control::LobbyCommand&)> handler) {
        on_lobby_command_ = std::move(handler);
    }

    // 콘솔 `send` 의 나머지를 넘길 곳 (roadmap.md Phase 4 콘솔 명령). 제품 경로는
    // tunnel::DataSender::on_command 다. 비워 두면 `send` 는 unknown_command 다.
    void set_send_handler(std::function<void(std::string_view rest)> handler) {
        on_send_ = std::move(handler);
    }

    // 바퀴마다 나눠 하는 송신 (concurrency.md 3장 "콘솔이 만든 송신 잔량은 busy 에 포함한다").
    // 타이머 다음에 많아야 kMaxDrain 개를 맡긴다. 참이면 남았으므로 다음 대기가 타임아웃 0 이다.
    void set_work_source(std::function<bool(std::size_t budget)> source) {
        work_ = std::move(source);
    }

    // 터널 후보로 분류된 데이터그램을 넘길 곳 (protocol.md 8장 검증 파이프라인). 제품 경로는
    // tunnel::Tunnel::on_datagram 이다 (main.cpp). 비워 두면 아무것도 하지 않는다.
    void set_tunnel_handler(DatagramHandler handler) { on_tunnel_ = std::move(handler); }

    // 세션 타이머의 자리 (protocol.md 11장, concurrency.md 4장). 세션은 마감을 스스로 들고,
    // 루프는 매 바퀴 가장 이른 마감을 다음 대기의 타임아웃에 넣고 타이머 집합 다음에 돌린다.
    // 제품 경로는 tunnel::Tunnel 의 earliest_deadline 과 run_timers 다. 비워 두면 없는 것이다.
    struct DeadlineSource {
        std::function<std::optional<Millis>()> earliest;
        std::function<void(Millis now)> run;
    };
    void set_deadline_source(DeadlineSource source) { deadlines_ = std::move(source); }

    // 수신 데이터그램 하나를 recvfrom 뒤의 경로에 태운다 (concurrency.md 3장 drain_udp 의
    // classify, protocol.md 7장 수신 분류). **drain_udp 가 받은 데이터그램마다 부르는 함수가
    // 이것이다.** 시험은 이 함수로 출발지를 위조한 데이터그램을 주입한다 (roadmap.md Phase 4
    // 검증의 출발지 위생 (b)). 보통의 UDP 소켓으로는 그런 출발지를 만들 수 없다. 시험 전용
    // 경로를 따로 짜지 않는 것이 이 함수의 이유다.
    void receive_datagram(const network::Endpoint& from, std::span<const std::byte> payload);

    // STUN 으로 분류된 데이터그램을 넘길 곳 (protocol.md 7장 수신 분류).
    //
    // 루프가 소켓을 배타적으로 소유하므로 STUN 클라이언트는 recvfrom 을 부르지 않는다
    // (protocol.md 6장 소켓 소유권). 받는 쪽은 network/stun_client.hpp 다.
    void set_stun_handler(DatagramHandler handler) { on_stun_ = std::move(handler); }

    // 이 루프의 소켓으로 데이터그램 하나를 보낸다. 보냈으면 참이다.
    //
    // **송신은 `[loop]` 하나가 한다** (concurrency.md 1장 스레드, 3장 루프 한 바퀴).
    // STUN 이 같은 소켓으로 보내야 하는데 (protocol.md 6장 소켓 소유권) 소켓의 소유는
    // 이 객체가 하므로, 소켓을 빌려주는 대신 이 자리를 연다. 실패는 여기서 세고 (그
    // 문서의 tx_err_send) 로그도 여기서 낸다. 부르는 쪽은 그 둘을 다시 하지 않는다.
    //
    // 재전송하지 않는다. 다음 주기의 재전송이 그 자리를 대신한다 (protocol.md 6장).
    [[nodiscard]] bool send_datagram(const network::Endpoint& to,
                                     std::span<const std::byte> payload);

    // send_datagram 과 같되 `tx_err_send` 를 올리지 않는다. `socket.error` 줄은 낸다.
    //
    // 세션의 송신은 실패를 그 세션의 표에 센다 (counters.hpp 의 두 범위). 이 자리에서 프로세스
    // 표에 올리면 같은 실패가 두 번 세어진다. 세는 것은 부르는 쪽(peer/session)이다.
    [[nodiscard]] bool send_datagram_uncounted(const network::Endpoint& to,
                                               std::span<const std::byte> payload);

    // 콘솔 `counters` 가 프로세스 표 뒤에 부르는 곳 (architecture.md 9장). 제품 경로는 남아 있는
    // 세션마다 그 세션의 표를 낸다 (tunnel::Tunnel::emit_session_counters). 비워 두면 프로세스 표만이다.
    void set_counter_dump_handler(std::function<void()> handler) {
        on_counter_dump_ = std::move(handler);
    }

    // 타이머 만료를 함께 보는 곳. 로그 줄은 그대로 나가고 이것이 더 불린다.
    //
    // 시험이 timer.tick 의 elapsed_ms 를 표준 오류에서 긁지 않고 프로세스 안에서 판정할
    // 수 있게 하려는 것이다. 판정 수단이 로그 문구에 묶이면 문구를 고칠 때 같이 낡는다.
    void set_tick_handler(std::function<void(const TimerTick&)> handler) {
        on_tick_ = std::move(handler);
    }

    // `[console]` 이 큐에 넣은 뒤 신호하는 핸들. 소유하지 않는다.
    [[nodiscard]] void* console_event() const noexcept { return console_event_; }

    // 콘솔 제어 핸들러가 신호할 핸들 (concurrency.md 7장 종료). 소유는 이 객체가 한다.
    // 그 핸들러는 이것을 복제해 들고 가므로 이 객체가 먼저 사라져도 안전하다. 근거는
    // platform/console_ctrl.hpp 가 갖는다.
    [[nodiscard]] void* shutdown_event() const noexcept { return shutdown_event_; }

    // 다른 스레드에서 종료를 건다. concurrency.md 7장 종료의 (1) 이다.
    void request_shutdown() noexcept;
    [[nodiscard]] bool shutdown_requested() const noexcept { return shutting_down_; }

    // 한 바퀴를 돈다. 종료로 빠져나가야 하면 거짓을 돌려준다.
    bool run_once();

    // 종료할 때까지 돈다.
    void run();

    // 자기 서브넷 집합 (protocol.md 10.1, concurrency.md 5장 상태 소유). 수신 분류의 출발지 위생
    // (7장)과 받은 후보 위생이 읽는다. 바꾸는 것은 10.1 의 수집 한 번이고, 그 결과(peer::Collection 의
    // own_subnets)를 그대로 넣는다. 첫 수집 전에는 비어 있다.
    void set_own_subnets(peer::OwnSubnets subnets) { own_subnets_ = std::move(subnets); }
    [[nodiscard]] const peer::OwnSubnets& own_subnets() const noexcept { return own_subnets_; }

    [[nodiscard]] const network::UdpSocket& socket() const noexcept { return socket_; }
    [[nodiscard]] TimerSet& timers() noexcept { return timers_; }

private:
    DrainOutcome drain_udp();
    // concurrency.md 3장 drain_wintun. RESTART 는 이 함수가 세션을 다시 만드는 것으로 처리한다.
    // 다시 만들지 못하면 종료를 신호하고 Empty 를 돌려준다. 대기 배열은 바퀴마다 새로 만드므로 새
    // 세션의 읽기 이벤트가 다음 바퀴에 그대로 들어간다.
    DrainOutcome drain_wintun();
    // `[console]` 이 올린 원자 변수를 표에 넣는다 (concurrency.md 6장 텔레메트리 격리).
    // 비우는 바퀴와 종료 바퀴가 같이 쓴다. 종료 바퀴는 drain_console() 앞에서 돌아가므로
    // 여기를 거치지 않으면 카운터 전량 출력이 낡은 값을 낸다.
    void sync_console_drop_counter() noexcept;
    void drain_console();
    // `[control]` 이 응답 큐에 넣은 것을 비운다 (concurrency.md 3장, 8장). 예산이 없다.
    // 두 큐는 작고 요청이 사람 속도다 (concurrency.md 3장).
    void drain_control();
    void handle_command(std::string_view line);
    void send_raw(std::size_t length);

    network::UdpSocket socket_;
    Counters& counters_;
    ConsoleQueue& console_;
    LoopOptions options_;
    TimerSet timers_;
    DatagramHandler on_tunnel_;
    DeadlineSource deadlines_;
    std::function<void()> on_counter_dump_;
    DatagramHandler on_stun_;
    std::function<void(const TimerTick&)> on_tick_;
    std::function<void(control::ControlResponse)> on_control_;
    std::function<void(const control::LobbyCommand&)> on_lobby_command_;
    std::function<void(std::string_view)> on_send_;
    std::function<bool(std::size_t)> work_;
    control::ControlChannel* control_ = nullptr;  // 소유하지 않는다
    adapter::Port* adapter_ = nullptr;            // 소유하지 않는다
    std::function<void(std::span<const std::byte>)> on_adapter_packet_;
    peer::OwnSubnets own_subnets_;                // protocol.md 10.1. 첫 수집 전에는 비어 있다

    void* shutdown_event_ = nullptr;  // 수동 리셋. 이 객체가 소유한다
    void* console_event_ = nullptr;   // 자동 리셋. ConsoleSession 이 소유한다
    bool shutting_down_ = false;
    bool busy_ = false;
};

// 표준 입력을 읽어 큐에 넣고 이벤트를 신호한다 (`[console]`).
//
// 세션을 값으로 받는다. 이 스레드는 join 하지 않으므로 (concurrency.md 7장 종료)
// `main` 이 빠져나간 뒤에도 깨어날 수 있고, 그때 건드릴 것을 스스로 붙들고 있어야 한다.
void run_console_reader(std::shared_ptr<ConsoleSession> session);

}  // namespace sangtachi
