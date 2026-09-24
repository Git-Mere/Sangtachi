#pragma once

// 이벤트 루프 (architecture.md 3.2.2 대기, 3.2.3 루프 한 바퀴, 3.2.4 타이머).
//
// `[loop]` 는 프로세스 주 스레드다. UDP 수신, 콘솔 명령, 타이머, 모든 송신을 혼자 한다.
// 터널 상태를 건드리는 코드가 한 스레드에서만 도는 것이 이 설계의 전부다 (3.2).

#include "sangtachi/console.hpp"
#include "sangtachi/counters.hpp"
#include "sangtachi/network/endpoint.hpp"
#include "sangtachi/network/udp_socket.hpp"
#include "sangtachi/timer.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace sangtachi {

// 한 바퀴당 소스별 비우기 상한 (architecture.md 3.2.3 루프 한 바퀴).
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

// 수신한 데이터그램 하나를 넘겨받는 자리. protocol.md 7장 수신 분류가 들어올 곳이다.
using DatagramHandler =
    std::function<void(const network::Endpoint& from, std::span<const std::byte> payload)>;

struct LoopOptions {
    std::optional<network::Endpoint> peer;  // architecture.md 3.5 의 --peer
    bool probe_timer = false;               // 시험 빌드의 probe200 타이머
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
    // WaitForMultipleObjects 가 WAIT_FAILED 를 낸다 (architecture.md 3.2.2 대기).
    // Phase 1~2 의 세 핸들은 전부 항상 있으므로, 여기서 한 번 확인하면 배열은 구성상
    // 조밀하다.
    [[nodiscard]] bool valid() const noexcept {
        return shutdown_event_ != nullptr && console_event_ != nullptr &&
               socket_.read_event() != nullptr;
    }

    // 수신 데이터그램을 넘길 곳. 비워 두면 아무것도 하지 않는다.
    void set_datagram_handler(DatagramHandler handler) { on_datagram_ = std::move(handler); }

    // 타이머 만료를 함께 보는 곳. 로그 줄은 그대로 나가고 이것이 더 불린다.
    //
    // 시험이 timer.tick 의 elapsed_ms 를 표준 오류에서 긁지 않고 프로세스 안에서 판정할
    // 수 있게 하려는 것이다. 판정 수단이 로그 문구에 묶이면 문구를 고칠 때 같이 낡는다.
    void set_tick_handler(std::function<void(const TimerTick&)> handler) {
        on_tick_ = std::move(handler);
    }

    // `[console]` 이 큐에 넣은 뒤 신호하는 핸들. 소유하지 않는다.
    [[nodiscard]] void* console_event() const noexcept { return console_event_; }

    // 다른 스레드에서 종료를 건다. 3.2.7 종료의 (1) 이다.
    void request_shutdown() noexcept;
    [[nodiscard]] bool shutdown_requested() const noexcept { return shutting_down_; }

    // 한 바퀴를 돈다. 종료로 빠져나가야 하면 거짓을 돌려준다.
    bool run_once();

    // 종료할 때까지 돈다.
    void run();

    [[nodiscard]] const network::UdpSocket& socket() const noexcept { return socket_; }
    [[nodiscard]] TimerSet& timers() noexcept { return timers_; }

private:
    DrainOutcome drain_udp();
    void drain_console();
    void handle_command(std::string_view line);
    void send_raw(std::size_t length);

    network::UdpSocket socket_;
    Counters& counters_;
    ConsoleQueue& console_;
    LoopOptions options_;
    TimerSet timers_;
    DatagramHandler on_datagram_;
    std::function<void(const TimerTick&)> on_tick_;

    void* shutdown_event_ = nullptr;  // 수동 리셋. 이 객체가 소유한다
    void* console_event_ = nullptr;   // 자동 리셋. ConsoleSession 이 소유한다
    bool shutting_down_ = false;
    bool busy_ = false;
};

// 표준 입력을 읽어 큐에 넣고 이벤트를 신호한다 (`[console]`).
//
// 세션을 값으로 받는다. 이 스레드는 join 하지 않으므로 (architecture.md 3.2.7 종료)
// `main` 이 빠져나간 뒤에도 깨어날 수 있고, 그때 건드릴 것을 스스로 붙들고 있어야 한다.
void run_console_reader(std::shared_ptr<ConsoleSession> session);

}  // namespace sangtachi
