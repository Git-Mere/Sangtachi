#include "sangtachi/loop.hpp"

#include "sangtachi/hash.hpp"
#include "sangtachi/log.hpp"
#include "sangtachi/network/stun.hpp"
#include "sangtachi/platform/wait.hpp"
#include "sangtachi/protocol_constants.hpp"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <memory>
#include <utility>
#include <vector>

namespace sangtachi {
namespace {

Millis now_ms() noexcept {
    return static_cast<Millis>(platform::monotonic_ms());
}

constexpr bool is_space(char ch) noexcept {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

std::string_view trim(std::string_view text) noexcept {
    while (!text.empty() && is_space(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && is_space(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

// architecture.md 3.5 기동 입력의 어휘는 `raw <바이트 수>` 하나다. 어휘 밖 낱말은
// unknown_command 여야 하므로 접두만 보지 않는다. `raw` 다음이 줄 끝이거나 공백일 때만
// 이 명령이다. `raw500` 과 `rawx` 는 다른 낱말이다.
constexpr std::string_view kRawCommand = "raw";

constexpr bool is_raw_command(std::string_view command) noexcept {
    if (command.substr(0, kRawCommand.size()) != kRawCommand) {
        return false;
    }
    return command.size() == kRawCommand.size() || is_space(command[kRawCommand.size()]);
}

// architecture.md 3.5 의 `raw` 가 정한 내용. 0x00 부터 1씩 증가한다.
std::vector<std::byte> raw_pattern(std::size_t length) {
    std::vector<std::byte> out(length);
    for (std::size_t i = 0; i < length; ++i) {
        out[i] = static_cast<std::byte>(i & 0xFF);
    }
    return out;
}

// protocol.md 7장 수신 분류가 보는 값들. 주소는 호스트 바이트 순서다.
constexpr std::uint32_t kMulticastNetwork = 0xE0000000u;  // 224.0.0.0
constexpr std::uint32_t kMulticastMask = 0xF0000000u;     // /4
constexpr std::uint32_t kLimitedBroadcast = 0xFFFFFFFFu;  // 255.255.255.255
constexpr std::uint32_t kUnspecified = 0x00000000u;       // 0.0.0.0

// 첫 바이트의 상위 2비트로만 분류한다. magic 과 version 은 분류 기준이 아니라 검증
// 항목이다 (protocol.md 7장).
constexpr std::uint8_t kClassMask = 0xC0;
constexpr std::uint8_t kClassStun = 0x00;
constexpr std::uint8_t kClassTunnel = 0x40;

[[nodiscard]] constexpr std::uint8_t byte_at(std::span<const std::byte> bytes,
                                             std::size_t offset) noexcept {
    return std::to_integer<std::uint8_t>(bytes[offset]);
}

void emit_rx_raw(const network::Endpoint& from, std::span<const std::byte> payload) {
    const auto digest = sha256_short(payload);
    const LogField fields[] = {
        field("from", from.to_string()),
        field("len", static_cast<std::uint64_t>(payload.size())),
        field("sha256", digest ? std::string_view(*digest) : std::string_view("unavailable")),
    };
    emit(LogLevel::Info, "rx.raw", fields);
}

}  // namespace

RxClass classify_datagram(const network::Endpoint& from,
                          std::span<const std::byte> payload) noexcept {
    // 1. 출발지 위생. 분류보다 먼저다 (protocol.md 7장). 수신 데이터그램의 출발지는
    //    우리가 되돌려 보내는 목적지가 되므로, 분류 전에 걸러야 STUN 경로와 터널 경로가
    //    같은 보호를 받는다.
    const std::uint32_t address = from.address();
    if (from.port() == 0) {
        return RxClass::BadSource;  // 회신할 곳이 없다
    }
    if (address == kUnspecified || address == kLimitedBroadcast) {
        return RxClass::BadSource;
    }
    if ((address & kMulticastMask) == kMulticastNetwork) {
        return RxClass::BadSource;  // 회신 1개가 그룹 구독자 수만큼 증폭된다
    }
    // 루프백은 통과다. 회신이 자신에게만 가므로 증폭이 없고, 같은 기기에서 두 프로세스를
    // 띄우는 시험이 이 경로를 쓴다 (protocol.md 7장의 표).

    // 2. 분류. 첫 바이트가 없으면 볼 것이 없다.
    if (payload.empty()) {
        return RxClass::Unclassified;
    }
    const std::uint8_t leading = static_cast<std::uint8_t>(byte_at(payload, 0) & kClassMask);
    if (leading == kClassStun && payload.size() >= network::kStunHeaderSize) {
        // buf[4..8) 는 반열린 구간이다. 오프셋 4,5,6,7 의 4바이트다 (protocol.md 7장).
        const std::uint32_t cookie = (static_cast<std::uint32_t>(byte_at(payload, 4)) << 24) |
                                     (static_cast<std::uint32_t>(byte_at(payload, 5)) << 16) |
                                     (static_cast<std::uint32_t>(byte_at(payload, 6)) << 8) |
                                     static_cast<std::uint32_t>(byte_at(payload, 7));
        if (cookie == protocol::kStunCookie) {
            return RxClass::Stun;
        }
    }
    if (leading == kClassTunnel) {
        return RxClass::TunnelCandidate;
    }
    return RxClass::Unclassified;
}

EventLoop::EventLoop(network::UdpSocket socket, Counters& counters, ConsoleQueue& console,
                     void* console_event, LoopOptions options)
    : socket_(std::move(socket)),
      counters_(counters),
      console_(console),
      options_(std::move(options)),
      console_event_(console_event) {
    // 종료 이벤트는 수동 리셋이다. 신호되면 기다리는 모든 스레드가 함께 깨어난다.
    // 받은 것이 있으면 복제한다. 소유는 어느 쪽이든 이 객체가 자기 핸들 하나를 갖는 것이다.
    shutdown_event_ = options_.shutdown_event != nullptr
                          ? platform::duplicate_event(options_.shutdown_event)
                          : platform::create_event(platform::ResetMode::Manual);

    if (options_.probe_timer) {
        if (!timers_.add_periodic(std::string(kProbeTimerName), kProbeTimerIntervalMs, now_ms())) {
            // 빈 집합에 처음 더하는 자리라 실패할 수 없다. 그래도 조용히 넘기지 않는다.
            // 못 걸린 타이머는 만료가 없고, timer.tick 을 기다리는 검증이 영영 멈춘다.
            const LogField fields[] = {field("name", kProbeTimerName)};
            emit(LogLevel::Warn, "timer.rejected", fields);
        }
    }
}

EventLoop::~EventLoop() {
    // 콘솔 이벤트는 닫지 않는다. ConsoleSession 이 소유한다.
    platform::close_event(shutdown_event_);
}

void EventLoop::request_shutdown() noexcept {
    platform::signal_event(shutdown_event_);
}

DrainOutcome EventLoop::drain_udp() {
    std::array<std::byte, network::kRecvBufferSize> buffer{};
    for (std::size_t i = 0; i < kMaxDrain; ++i) {
        const auto result = socket_.recv_from(buffer);
        switch (result.status) {
            case network::RecvStatus::WouldBlock:
                return DrainOutcome::Empty;
            case network::RecvStatus::Oversize:
                // 과대 데이터그램에서 비우기를 멈추지 않는다 (concurrency.md 3장).
                // 멈추면 과대분을 섞어 보내는 것만으로 배칭이 무력화된다.
                counters_.increment(Counter::DropOversizeDatagram);
                continue;
            case network::RecvStatus::Error: {
                // 원인을 모르므로 그 바퀴를 끝낸다. 소켓은 계속 쓴다.
                //
                // 로그만 내면 그 바퀴가 왜 짧게 끝났는지가 카운터 전량 출력에 남지 않는다
                // (concurrency.md 3장 루프 한 바퀴). 오류 코드별로 나누지 않는다. 코드는
                // 로그가 싣는다.
                counters_.increment(Counter::RxErrRecv);
                const LogField fields[] = {
                    field("op", std::string_view("recvfrom")),
                    field("code", static_cast<std::uint64_t>(result.error)),
                };
                emit(LogLevel::Warn, "socket.error", fields);
                return DrainOutcome::Empty;
            }
            case network::RecvStatus::Received: {
                const std::span<const std::byte> payload(buffer.data(), result.length);
                // Phase 1 의 대조 수단이다 (architecture.md 3.5 기동 입력). 분류 앞에
                // 둔다. 로그 한 줄은 회신이 아니라 진단이므로 출발지 위생이 막는 반사·
                // 증폭 경로에 들지 않고, 버려진 데이터그램도 진단에 남아야 한다.
                // 시험용 `--peer`, `raw` 와 함께 Phase 6 에서 사라진다.
                emit_rx_raw(result.from, payload);

                switch (classify_datagram(result.from, payload)) {
                    case RxClass::BadSource:
                        counters_.increment(Counter::DropBadSource);
                        break;
                    case RxClass::Stun:
                        if (on_stun_) {
                            on_stun_(result.from, payload);
                        }
                        break;
                    case RxClass::TunnelCandidate:
                        // 8장 검증 파이프라인은 Phase 4 다. 지금은 넘기기만 한다.
                        if (on_tunnel_) {
                            on_tunnel_(result.from, payload);
                        }
                        break;
                    case RxClass::Unclassified:
                        counters_.increment(Counter::DropUnclassified);
                        break;
                }
                break;
            }
        }
    }
    return DrainOutcome::Budget;
}

bool EventLoop::send_datagram(const network::Endpoint& to, std::span<const std::byte> payload) {
    const auto sent = socket_.send_to(to, payload);
    if (!sent.ok) {
        // 타입과 무관하게 tx_err_send 를 올리고 버린다. 재전송하지 않는다
        // (protocol.md 6장 소켓 소유권).
        counters_.increment(Counter::TxErrSend);
        const LogField fields[] = {
            field("op", std::string_view("sendto")),
            field("code", static_cast<std::uint64_t>(sent.error)),
        };
        emit(LogLevel::Warn, "socket.error", fields);
        return false;
    }
    return true;
}

void EventLoop::send_raw(std::size_t length) {
    if (!options_.peer) {
        const LogField fields[] = {field("reason", std::string_view("no_peer"))};
        emit(LogLevel::Warn, "console.rejected", fields);
        return;
    }
    if (length < 1 || length > protocol::kMaxDatagram) {
        const LogField fields[] = {
            field("reason", std::string_view("length_out_of_range")),
            field("len", static_cast<std::uint64_t>(length)),
        };
        emit(LogLevel::Warn, "console.rejected", fields);
        return;
    }

    const auto payload = raw_pattern(length);
    if (!send_datagram(*options_.peer, payload)) {
        return;
    }
    // 송신 측도 같은 줄을 낸다. from 은 자신의 로컬 엔드포인트다 (architecture.md 3.5).
    emit_rx_raw(socket_.local(), payload);
}

void EventLoop::handle_command(std::string_view line) {
    const std::string_view command = trim(line);
    if (command.empty()) {
        return;
    }
    // 로비 명령이 먼저다. 첫 낱말이 정확히 host, join, leave 일 때만 값이 있다
    // (control/lobby.hpp 의 parse_lobby_command). 형식 검사와 WARN 은 로비가 한다.
    if (on_lobby_command_) {
        if (const auto lobby_command = control::parse_lobby_command(command)) {
            on_lobby_command_(*lobby_command);
            return;
        }
    }
    if (command == "quit") {
        // shutdown() 을 직접 부르지 않는다. 종료 이벤트를 신호해 다음 바퀴가 concurrency.md 7장 의
        // 순서를 그대로 타게 한다 (concurrency.md 3장).
        request_shutdown();
        return;
    }
    if (command == "counters") {
        // architecture.md 9장이 정한 세 시점 중 하나다.
        emit_all(counters_);
        return;
    }
    if (is_raw_command(command)) {
        const std::string_view rest = trim(command.substr(kRawCommand.size()));
        std::size_t length = 0;
        const auto* begin = rest.data();
        const auto* end = begin + rest.size();
        const auto parsed = std::from_chars(begin, end, length);
        if (rest.empty() || parsed.ec != std::errc() || parsed.ptr != end) {
            const LogField fields[] = {field("reason", std::string_view("bad_length"))};
            emit(LogLevel::Warn, "console.rejected", fields);
            return;
        }
        send_raw(length);
        return;
    }

    const LogField fields[] = {
        field("reason", std::string_view("unknown_command")),
        field("command", command),
    };
    emit(LogLevel::Warn, "console.rejected", fields);
}

void EventLoop::sync_console_drop_counter() noexcept {
    // 콘솔이 버린 수를 표에 반영하는 자리다. 그 카운터만 생산자가 올리고 `[loop]` 는
    // 읽어서 넣는다 (concurrency.md 6장 텔레메트리 격리).
    counters_.set(Counter::ConsoleQueueDropped, console_.dropped());
}

void EventLoop::drain_console() {
    sync_console_drop_counter();

    while (auto line = console_.try_pop()) {
        handle_command(*line);
    }
}

void EventLoop::drain_control() {
    if (control_ == nullptr) {
        return;
    }
    // 응답 이벤트는 자동 리셋이라 따로 리셋하지 않는다. 깨어난 바퀴가 링을 다 비운다.
    while (auto response = control_->try_pop()) {
        if (on_control_) {
            on_control_(std::move(*response));
        }
    }
}

bool EventLoop::run_once() {
    // 살아 있는 핸들만 모아 조밀한 배열을 만든다. 빈자리에 NULL 을 넣으면 WAIT_FAILED 가
    // 난다 (concurrency.md 2장). 논리적 순위와 배열 인덱스는 다르다.
    //
    // 순위 2(Wintun)는 Phase 6 이후라 아직 없다. 순위 4(제어 응답)는 채널이 붙었을 때만 든다.
    platform::WaitHandle handles[4];
    std::size_t count = 0;
    const std::size_t shutdown_index = count;
    handles[count++] = shutdown_event_;
    const std::size_t udp_index = count;
    handles[count++] = socket_.read_event();
    const std::size_t console_index = count;
    handles[count++] = console_event_;
    if (control_ != nullptr) {
        handles[count++] = control_->response_event();
    }

    const std::uint32_t timeout =
        busy_ ? 0 : next_timeout_ms(now_ms(), timers_.earliest_deadline());
    const platform::WaitResult result =
        platform::wait_any(std::span<const platform::WaitHandle>(handles, count), timeout);

    if (result.status == platform::WaitStatus::Failed) {
        // 여기서 바로 빠져나가면 정리를 건너뛴다. 비정상 종료야말로 정리가 필요하다.
        const LogField fields[] = {
            field("op", std::string_view("WaitForMultipleObjects")),
            field("code", static_cast<std::uint64_t>(result.error)),
        };
        emit(LogLevel::Error, "socket.error", fields);
        // 종료 이벤트를 신호한다. concurrency.md 7장 종료의 (1) 이 이 경로에도 적용된다.
        // 표시만 세우고 빠져나가면 그 이벤트를 기다리는 다른 스레드가 깨어나지 않는다.
        // 지금은 기다리는 스레드가 없지만 `[control]` 이 들어오는 Phase 3 에서 결함이 된다.
        request_shutdown();
        // 종료 바퀴는 drain_console() 앞에서 돌아간다. 여기서 반영하지 않으면 그 뒤의
        // 카운터 전량 출력이 낡은 값을 낸다 (concurrency.md 6장 텔레메트리 격리).
        sync_console_drop_counter();
        shutting_down_ = true;
        return false;
    }
    if (result.status == platform::WaitStatus::Signaled && result.index == shutdown_index) {
        sync_console_drop_counter();
        shutting_down_ = true;
        return false;
    }
    (void)udp_index;
    (void)console_index;

    // 반환값으로 분기하지 않는다. 매 바퀴 양쪽을 모두 비운다 (concurrency.md 3장).
    // 이 호출이 이벤트 리셋과 네트워크 이벤트 조회를 한 번에 처리한다. 빠뜨리면 이벤트가
    // 신호 상태로 남아 루프가 스핀한다.
    socket_.enumerate_events();
    const DrainOutcome udp = drain_udp();
    drain_console();
    drain_control();  // concurrency.md 3장. Phase 3 이후
    timers_.run_expired(now_ms(), [this](const TimerTick& tick) {
        const LogField fields[] = {
            field("name", tick.name),
            field("elapsed_ms", tick.elapsed_ms),
        };
        emit(LogLevel::Info, "timer.tick", fields);
        if (on_tick_) {
            on_tick_(tick);
        }
    });

    busy_ = (udp == DrainOutcome::Budget);
    return true;
}

void EventLoop::run() {
    while (run_once()) {
    }
}

void run_console_reader(std::shared_ptr<ConsoleSession> session) {
    if (!session) {
        return;
    }
    std::string line;
    while (!session->stopped() && std::getline(std::cin, line)) {
        session->queue().try_push(std::move(line));
        platform::signal_event(session->event());
    }
}

}  // namespace sangtachi
