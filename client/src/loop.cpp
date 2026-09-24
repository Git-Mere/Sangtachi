#include "sangtachi/loop.hpp"

#include "sangtachi/hash.hpp"
#include "sangtachi/log.hpp"
#include "sangtachi/protocol_constants.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <memory>
#include <utility>
#include <vector>

namespace sangtachi {
namespace {

Millis now_ms() noexcept {
    return static_cast<Millis>(::GetTickCount64());
}

std::string_view trim(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' ||
                             text.front() == '\r' || text.front() == '\n')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' ||
                             text.back() == '\r' || text.back() == '\n')) {
        text.remove_suffix(1);
    }
    return text;
}

// architecture.md 3.5 의 `raw` 가 정한 내용. 0x00 부터 1씩 증가한다.
std::vector<std::byte> raw_pattern(std::size_t length) {
    std::vector<std::byte> out(length);
    for (std::size_t i = 0; i < length; ++i) {
        out[i] = static_cast<std::byte>(i & 0xFF);
    }
    return out;
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

EventLoop::EventLoop(network::UdpSocket socket, Counters& counters, ConsoleQueue& console,
                     void* console_event, LoopOptions options)
    : socket_(std::move(socket)),
      counters_(counters),
      console_(console),
      options_(std::move(options)),
      console_event_(console_event) {
    // 종료 이벤트는 수동 리셋이다. 신호되면 기다리는 모든 스레드가 함께 깨어난다.
    shutdown_event_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);

    if (options_.probe_timer) {
        timers_.add_periodic(std::string(kProbeTimerName), kProbeTimerIntervalMs, now_ms());
    }
}

EventLoop::~EventLoop() {
    // 콘솔 이벤트는 닫지 않는다. ConsoleSession 이 소유한다.
    if (shutdown_event_ != nullptr) {
        ::CloseHandle(shutdown_event_);
    }
}

void EventLoop::request_shutdown() noexcept {
    if (shutdown_event_ != nullptr) {
        ::SetEvent(shutdown_event_);
    }
}

DrainOutcome EventLoop::drain_udp() {
    std::array<std::byte, network::kRecvBufferSize> buffer{};
    for (std::size_t i = 0; i < kMaxDrain; ++i) {
        const auto result = socket_.recv_from(buffer);
        switch (result.status) {
            case network::RecvStatus::WouldBlock:
                return DrainOutcome::Empty;
            case network::RecvStatus::Oversize:
                // 과대 데이터그램에서 비우기를 멈추지 않는다 (architecture.md 3.2.3).
                // 멈추면 과대분을 섞어 보내는 것만으로 배칭이 무력화된다.
                counters_.increment(Counter::DropOversizeDatagram);
                continue;
            case network::RecvStatus::Error: {
                // 원인을 모르므로 그 바퀴를 끝낸다. 소켓은 계속 쓴다.
                const LogField fields[] = {
                    field("op", std::string_view("recvfrom")),
                    field("code", static_cast<std::uint64_t>(result.error)),
                };
                emit(LogLevel::Warn, "socket.error", fields);
                return DrainOutcome::Empty;
            }
            case network::RecvStatus::Received:
                emit_rx_raw(result.from, std::span<const std::byte>(buffer.data(), result.length));
                if (on_datagram_) {
                    on_datagram_(result.from,
                                 std::span<const std::byte>(buffer.data(), result.length));
                }
                break;
        }
    }
    return DrainOutcome::Budget;
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
    const auto sent = socket_.send_to(*options_.peer, payload);
    if (!sent.ok) {
        counters_.increment(Counter::TxErrSend);
        const LogField fields[] = {
            field("op", std::string_view("sendto")),
            field("code", static_cast<std::uint64_t>(sent.error)),
        };
        emit(LogLevel::Warn, "socket.error", fields);
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
    if (command == "quit") {
        // shutdown() 을 직접 부르지 않는다. 종료 이벤트를 신호해 다음 바퀴가 3.2.7 의
        // 순서를 그대로 타게 한다 (architecture.md 3.2.3).
        request_shutdown();
        return;
    }
    if (command == "counters") {
        // architecture.md 9장이 정한 세 시점 중 하나다.
        emit_all(counters_);
        return;
    }
    if (command.rfind("raw", 0) == 0) {
        const std::string_view rest = trim(command.substr(3));
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

void EventLoop::drain_console() {
    // 콘솔이 버린 수를 여기서 표에 반영한다. 그 카운터만 생산자가 올린다
    // (architecture.md 3.2.6).
    counters_.set(Counter::ConsoleQueueDropped, console_.dropped());

    while (auto line = console_.try_pop()) {
        handle_command(*line);
    }
}

bool EventLoop::run_once() {
    // 살아 있는 핸들만 모아 조밀한 배열을 만든다. 빈자리에 NULL 을 넣으면 WAIT_FAILED 가
    // 난다 (architecture.md 3.2.2). 논리적 순위와 배열 인덱스는 다르다.
    HANDLE handles[3];
    DWORD count = 0;
    const DWORD shutdown_index = count;
    handles[count++] = shutdown_event_;
    const DWORD udp_index = count;
    handles[count++] = socket_.read_event();
    const DWORD console_index = count;
    handles[count++] = console_event_;

    const DWORD timeout =
        busy_ ? 0 : next_timeout_ms(now_ms(), timers_.earliest_deadline());
    const DWORD result = ::WaitForMultipleObjects(count, handles, FALSE, timeout);

    if (result == WAIT_FAILED) {
        // 여기서 바로 빠져나가면 정리를 건너뛴다. 비정상 종료야말로 정리가 필요하다.
        const LogField fields[] = {
            field("op", std::string_view("WaitForMultipleObjects")),
            field("code", static_cast<std::uint64_t>(::GetLastError())),
        };
        emit(LogLevel::Error, "socket.error", fields);
        shutting_down_ = true;
        return false;
    }
    if (result == WAIT_OBJECT_0 + shutdown_index) {
        shutting_down_ = true;
        return false;
    }
    (void)udp_index;
    (void)console_index;

    // 반환값으로 분기하지 않는다. 매 바퀴 양쪽을 모두 비운다 (architecture.md 3.2.3).
    // 이 호출이 이벤트 리셋과 네트워크 이벤트 조회를 한 번에 처리한다. 빠뜨리면 이벤트가
    // 신호 상태로 남아 루프가 스핀한다.
    socket_.enumerate_events();
    const DrainOutcome udp = drain_udp();
    drain_console();
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
        ::SetEvent(session->event());
    }
}

}  // namespace sangtachi
