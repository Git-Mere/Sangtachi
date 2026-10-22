#include "sangtachi/loop.hpp"

#include "sangtachi/log.hpp"
#include "sangtachi/network/stun.hpp"
#include "sangtachi/platform/wait.hpp"
#include "sangtachi/protocol_constants.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <memory>
#include <optional>
#include <utility>

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

// protocol.md 7장 수신 분류가 보는 값들. 주소는 호스트 바이트 순서다.
constexpr std::uint32_t kMulticastNetwork = 0xE0000000u;  // 224.0.0.0
constexpr std::uint32_t kMulticastMask = 0xF0000000u;     // /4
constexpr std::uint32_t kLimitedBroadcast = 0xFFFFFFFFu;  // 255.255.255.255
constexpr std::uint32_t kUnspecified = 0x00000000u;       // 0.0.0.0
constexpr std::uint32_t kLoopbackNetwork = 0x7F000000u;   // 127.0.0.0
constexpr std::uint32_t kLoopbackMask = 0xFF000000u;      // /8

// 첫 바이트의 상위 2비트로만 분류한다. magic 과 version 은 분류 기준이 아니라 검증
// 항목이다 (protocol.md 7장).
constexpr std::uint8_t kClassMask = 0xC0;
constexpr std::uint8_t kClassStun = 0x00;
constexpr std::uint8_t kClassTunnel = 0x40;

[[nodiscard]] constexpr std::uint8_t byte_at(std::span<const std::byte> bytes,
                                             std::size_t offset) noexcept {
    return std::to_integer<std::uint8_t>(bytes[offset]);
}

}  // namespace

RxClass classify_datagram(const network::Endpoint& from,
                          std::span<const std::byte> payload,
                          const peer::OwnSubnets& own_subnets) noexcept {
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
    // 루프백은 자기 서브넷 행보다 먼저 통과시킨다. 아주 넓은 자기 서브넷(/1 등)의 브로드캐스트가
    // 127.255.255.255 가 되어 두 행이 겹칠 수 있고, 7장 첫 표가 루프백을 통과로 정했다.
    const bool loopback = (address & kLoopbackMask) == kLoopbackNetwork;
    if (!loopback && own_subnets.blocks_broadcast(address)) {
        return RxClass::BadSource;  // 자기 서브넷의 directed broadcast. 증폭이 실제로 일어나는 곳이다
    }
    // 원격 서브넷의 브로드캐스트는 넷마스크를 모르므로 막지 못한다 (7장의 남는 위험).
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
                if (result.length > 0) {  // WSAEMSGSIZE 로 실패했으면 0 이다. 돌려준 값이 없으므로 세지 않는다
                    count_received(result.length);
                }
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
            case network::RecvStatus::Received:
                count_received(result.length);
                receive_datagram(result.from,
                                 std::span<const std::byte>(buffer.data(), result.length));
                break;
        }
    }
    return DrainOutcome::Budget;
}

void EventLoop::count_received(std::size_t length) noexcept {
    // recvfrom 이 성공해 돌려준 길이다 (concurrency.md 3장 송수신 바이트). 0바이트 데이터그램도 하나로 센다.
    counters_.add(Counter::RxBytes, length);
    counters_.increment(Counter::RxDatagrams);
}

DrainOutcome EventLoop::drain_wintun() {
    if (adapter_ == nullptr) {
        return DrainOutcome::Empty;  // 어댑터가 없다. Phase 1~5 와 --no-adapter
    }
    for (std::size_t i = 0; i < kMaxDrain; ++i) {
        const adapter::Received got = adapter_->receive();
        switch (got.status) {
            case adapter::ReceiveStatus::kEmpty:
                return DrainOutcome::Empty;
            case adapter::ReceiveStatus::kEof: {
                // 어댑터가 끝나는 중이다. 종료 절차를 타게 한다 (concurrency.md 3장).
                const LogField fields[] = {field("reason", std::string_view("eof"))};
                emit(LogLevel::Error, "adapter.error", fields);
                request_shutdown();
                return DrainOutcome::Empty;
            }
            case adapter::ReceiveStatus::kCorrupt: {
                // 링이 손상됐다. 세션을 다시 만든다. 새 세션은 새 읽기 이벤트를 주고, 대기 배열은
                // 다음 바퀴에 그 값으로 다시 만들어진다. 다시 만들지 못하면 어댑터 없이 돌지 않는다.
                // 터널이 죽은 채 살아 있는 척하게 된다 (concurrency.md 3장).
                const bool restarted = adapter_->restart_session();
                const LogField fields[] = {
                    field("reason", std::string_view("ring_corrupt")),
                    field("restarted", restarted ? std::string_view("yes") : std::string_view("no")),
                };
                emit(LogLevel::Error, "adapter.error", fields);
                if (!restarted) {
                    request_shutdown();
                }
                return DrainOutcome::Empty;
            }
            case adapter::ReceiveStatus::kError: {
                counters_.increment(Counter::RxErrAdapter);
                const LogField fields[] = {
                    field("op", std::string_view("WintunReceivePacket")),
                    field("code", static_cast<std::uint64_t>(got.error)),
                };
                emit(LogLevel::Warn, "adapter.error", fields);
                return DrainOutcome::Empty;
            }
            case adapter::ReceiveStatus::kPacket: {
                // 돌려주기를 스코프 가드로 건다. 처리 중에 빠져나가도 링 자리가 새지 않는다.
                struct Release {
                    adapter::Port* port;
                    std::span<const std::byte> packet;
                    ~Release() { port->release(packet); }
                } release{adapter_, got.packet};
                counters_.increment(Counter::AdapterRx);
                if (on_adapter_packet_) {
                    on_adapter_packet_(got.packet);
                }
                break;
            }
        }
    }
    return DrainOutcome::Budget;
}

bool EventLoop::inject_packet(std::span<const std::byte> packet) {
    if (adapter_ == nullptr) {
        counters_.increment(Counter::DropInjectError);
        return false;
    }
    const adapter::InjectResult result = adapter_->inject(packet);
    switch (result.status) {
        case adapter::InjectStatus::kOk:
            return true;
        case adapter::InjectStatus::kRingFull:
            counters_.increment(Counter::DropInjectRingFull);
            return false;
        case adapter::InjectStatus::kError:
            break;
    }
    counters_.increment(Counter::DropInjectError);
    return false;
}

void EventLoop::receive_datagram(const network::Endpoint& from,
                                 std::span<const std::byte> payload) {
    // 자기 서브넷 집합은 루프가 소유한 그것이다 (concurrency.md 5장). 빈 집합을 넘기면 7장
    // directed broadcast 행이 아무것도 막지 않는다. loop_test 의 출발지 위생 (b) 가 지킨다.
    switch (classify_datagram(from, payload, own_subnets_)) {
        case RxClass::BadSource:
            counters_.increment(Counter::DropBadSource);
            break;
        case RxClass::Stun:
            if (on_stun_) {
                on_stun_(from, payload);
            }
            break;
        case RxClass::TunnelCandidate:
            // 8장 검증 파이프라인 (tunnel/tunnel.hpp).
            if (on_tunnel_) {
                on_tunnel_(from, payload);
            }
            break;
        case RxClass::Unclassified:
            counters_.increment(Counter::DropUnclassified);
            break;
    }
}

bool EventLoop::send_datagram(const network::Endpoint& to, std::span<const std::byte> payload) {
    if (!send_datagram_uncounted(to, payload)) {
        // 타입과 무관하게 tx_err_send 를 올리고 버린다. 재전송하지 않는다
        // (protocol.md 6장 소켓 소유권). 세션이 정해지지 않은 송신이라 프로세스 표다.
        counters_.increment(Counter::TxErrSend);
        return false;
    }
    return true;
}

bool EventLoop::send_datagram_uncounted(const network::Endpoint& to,
                                        std::span<const std::byte> payload) {
    const auto sent = socket_.send_to(to, payload);
    if (!sent.ok) {
        const LogField fields[] = {
            field("op", std::string_view("sendto")),
            field("code", static_cast<std::uint64_t>(sent.error)),
        };
        emit(LogLevel::Warn, "socket.error", fields);
        return false;
    }
    // 패킷 타입과 상대를 가리지 않는다 (concurrency.md 3장). 캡처 대조가 이 값을 읽는다.
    counters_.add(Counter::TxBytes, sent.bytes);
    counters_.increment(Counter::TxDatagrams);
    return true;
}

void EventLoop::handle_command(std::string_view line) {
    const std::string_view command = trim(line);
    if (command.empty()) {
        return;
    }
    // 로비 명령이 먼저다. 첫 낱말이 정확히 host, join, leave, name, help 일 때만 값이 있다
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
        if (on_counter_dump_) {
            on_counter_dump_();  // 남아 있는 세션마다 그 세션의 표
        }
        return;
    }
    if (on_send_ && (command == "send" || command.starts_with("send "))) {
        on_send_(command.substr(4));  // 형식 검사와 WARN 은 받는 쪽이 한다
        return;
    }
    // 친 문자열은 싣지 않는다. 틀리게 친 join 이면 방 코드가 들어 있다 (architecture.md 3.5 "방 코드도 표준 출력이다").
    // 로그는 로그 파일에 남는다.
    const LogField fields[] = {
        field("reason", std::string_view("unknown_command")),
    };
    emit(LogLevel::Warn, "console.rejected", fields);
    if (on_unknown_command_) {
        on_unknown_command_();
    }
}

void EventLoop::sync_console_drop_counter() noexcept {
    // 콘솔이 버린 수를 표에 반영하는 자리다. 그 카운터만 생산자가 올리고 `[loop]` 는
    // 읽어서 넣는다 (concurrency.md 6장 텔레메트리 격리).
    counters_.set(Counter::ConsoleQueueDropped, console_.dropped());
}

void EventLoop::drain_console() {
    sync_console_drop_counter();

    while (auto line = console_.try_pop()) {
        if (on_console_before_) {
            on_console_before_();
        }
        handle_command(*line);
        if (on_console_after_) {
            on_console_after_();
        }
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
    // 순위 2(Wintun)는 어댑터가 붙었을 때만, 순위 4(제어 응답)는 채널이 붙었을 때만 든다.
    platform::WaitHandle handles[5];
    std::size_t count = 0;
    const std::size_t shutdown_index = count;
    handles[count++] = shutdown_event_;
    const std::size_t udp_index = count;
    handles[count++] = socket_.read_event();
    if (adapter_ != nullptr) {
        handles[count++] = adapter_->read_event();
    }
    const std::size_t console_index = count;
    handles[count++] = console_event_;
    if (control_ != nullptr) {
        handles[count++] = control_->response_event();
    }

    // 가장 이른 마감은 타이머 집합과 세션 타이머 둘 가운데 이른 것이다 (concurrency.md 4장).
    std::optional<Millis> earliest = timers_.earliest_deadline();
    if (deadlines_.earliest) {
        const auto session = deadlines_.earliest();
        if (session && (!earliest || *session < *earliest)) {
            earliest = session;
        }
    }
    const std::uint32_t timeout = busy_ ? 0 : next_timeout_ms(now_ms(), earliest);
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
    const DrainOutcome wintun = drain_wintun();
    drain_console();
    drain_control();  // concurrency.md 3장. Phase 3 이후
    const Millis now = now_ms();
    timers_.run_expired(now, [this](const TimerTick& tick) {
        const LogField fields[] = {
            field("name", tick.name),
            field("elapsed_ms", tick.elapsed_ms),
        };
        emit(LogLevel::Info, "timer.tick", fields);
        if (on_tick_) {
            on_tick_(tick);
        }
    });

    // 세션 타이머 (protocol.md 11장). 마감 비교는 같은 `deadline <= now` 다. 수신을 먼저 비우고
    // 타이머를 나중에 도는 순서가 11장 "꺼낸 시각 기준" 의 구현이다 (concurrency.md 4장).
    if (deadlines_.run) {
        deadlines_.run(now);
    }
    const bool work_left = work_ ? work_(kMaxDrain) : false;

    busy_ = (udp == DrainOutcome::Budget) || (wintun == DrainOutcome::Budget) || work_left;
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
