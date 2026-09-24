#include "sangtachi/loop.hpp"

#include "sangtachi/console.hpp"
#include "sangtachi/counters.hpp"
#include "sangtachi/network/endpoint.hpp"
#include "sangtachi/network/udp_socket.hpp"
#include "sangtachi/network/wsa.hpp"
#include "sangtachi/protocol_constants.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

// 시험 케이스 이름은 ASCII 로만 적는다. 이유는 network/wsa_test.cpp 머리에 있다.

using sangtachi::ConsoleQueue;
using sangtachi::Counter;
using sangtachi::Counters;
using sangtachi::EventLoop;
using sangtachi::kMaxDrain;
using sangtachi::LoopOptions;
using sangtachi::Millis;
using sangtachi::TimerTick;
using sangtachi::network::Endpoint;
using sangtachi::network::open_udp_socket;
using sangtachi::network::UdpSocket;
using sangtachi::network::WsaContext;
using sangtachi::protocol::kMaxDatagram;

namespace {

constexpr std::uint32_t kLoopback = 0x7F000001u;

std::vector<std::byte> pattern(std::size_t size) {
    std::vector<std::byte> out(size);
    for (std::size_t i = 0; i < size; ++i) {
        out[i] = static_cast<std::byte>(i & 0xFF);
    }
    return out;
}

// Sleep 을 쓰지 않는다. 기본 타이머 해상도에서 Sleep(1) 이 15ms 가까이 자므로 한 바퀴
// 예산(64개)에 곱하면 판정 기준을 정상 구현도 넘긴다.
void spin_microseconds(std::int64_t micros) {
    LARGE_INTEGER frequency{};
    LARGE_INTEGER start{};
    ::QueryPerformanceFrequency(&frequency);
    ::QueryPerformanceCounter(&start);
    const std::int64_t ticks = (frequency.QuadPart * micros) / 1000000;
    LARGE_INTEGER now{};
    do {
        ::QueryPerformanceCounter(&now);
    } while (now.QuadPart - start.QuadPart < ticks);
}

}  // namespace

TEST_CASE("loop: shutdown makes one wheel return false", "[loop]") {
    const WsaContext wsa;
    auto opened = open_udp_socket();
    REQUIRE(opened.ok());

    Counters counters;
    auto session = sangtachi::ConsoleSession::create();
    REQUIRE(session != nullptr);
    ConsoleQueue& console = session->queue();
    EventLoop loop(std::move(*opened.socket), counters, console, session->event(), LoopOptions{});
    REQUIRE(loop.valid());

    loop.request_shutdown();
    REQUIRE_FALSE(loop.run_once());
    REQUIRE(loop.shutdown_requested());
}

TEST_CASE("loop: one wheel drains many datagrams", "[loop]") {
    // architecture.md 3.2.3: 한 번의 신호로 도착한 데이터그램 여러 개가 같은 바퀴에
    // 처리된다. 한 바퀴에 하나만 읽는 구현은 여기서 걸린다.
    const WsaContext wsa;
    auto sender = open_udp_socket();
    auto receiver = open_udp_socket();
    REQUIRE(sender.ok());
    REQUIRE(receiver.ok());

    const Endpoint to(kLoopback, receiver.socket->local().port());
    for (int i = 0; i < 10; ++i) {
        REQUIRE(sender.socket->send_to(to, pattern(32)).ok);
    }

    Counters counters;
    auto session = sangtachi::ConsoleSession::create();
    REQUIRE(session != nullptr);
    ConsoleQueue& console = session->queue();
    EventLoop loop(std::move(*receiver.socket), counters, console, session->event(), LoopOptions{});
    std::size_t seen = 0;
    loop.set_datagram_handler([&seen](const Endpoint&, std::span<const std::byte>) { ++seen; });

    REQUIRE(loop.run_once());
    REQUIRE(seen == 10);
}

TEST_CASE("loop: the drain budget stops one wheel at the documented count", "[loop]") {
    // 예산이 없으면 한 바퀴가 전부를 삼킨다. 예산은 64 다 (architecture.md 3.2.3).
    const WsaContext wsa;
    auto sender = open_udp_socket();
    auto receiver = open_udp_socket();
    REQUIRE(sender.ok());
    REQUIRE(receiver.ok());

    const Endpoint to(kLoopback, receiver.socket->local().port());
    const std::size_t total = kMaxDrain + 10;
    for (std::size_t i = 0; i < total; ++i) {
        REQUIRE(sender.socket->send_to(to, pattern(16)).ok);
    }

    Counters counters;
    auto session = sangtachi::ConsoleSession::create();
    REQUIRE(session != nullptr);
    ConsoleQueue& console = session->queue();
    EventLoop loop(std::move(*receiver.socket), counters, console, session->event(), LoopOptions{});
    std::size_t seen = 0;
    loop.set_datagram_handler([&seen](const Endpoint&, std::span<const std::byte>) { ++seen; });

    REQUIRE(loop.run_once());
    REQUIRE(seen == kMaxDrain);

    // 남은 것은 다음 바퀴에 처리된다. 예산에 걸린 바퀴는 busy 라 즉시 돌아온다.
    REQUIRE(loop.run_once());
    REQUIRE(seen == total);
}

TEST_CASE("loop: an oversize datagram does not break the batch", "[loop]") {
    // architecture.md 3.2.3: 과대 데이터그램을 정상 데이터그램 사이에 끼워 보내고,
    // 카운터가 오르면서 뒤따르는 정상 데이터그램이 계속 처리되는지 본다.
    // (a) 1473바이트는 길이 비교로, (b) 1474바이트는 WSAEMSGSIZE 로 걸린다.
    const WsaContext wsa;
    auto sender = open_udp_socket();
    auto receiver = open_udp_socket();
    REQUIRE(sender.ok());
    REQUIRE(receiver.ok());

    const Endpoint to(kLoopback, receiver.socket->local().port());
    REQUIRE(sender.socket->send_to(to, pattern(32)).ok);
    REQUIRE(sender.socket->send_to(to, pattern(kMaxDatagram + 1)).ok);   // (a)
    REQUIRE(sender.socket->send_to(to, pattern(32)).ok);
    REQUIRE(sender.socket->send_to(to, pattern(kMaxDatagram + 2)).ok);   // (b)
    REQUIRE(sender.socket->send_to(to, pattern(32)).ok);

    Counters counters;
    auto session = sangtachi::ConsoleSession::create();
    REQUIRE(session != nullptr);
    ConsoleQueue& console = session->queue();
    EventLoop loop(std::move(*receiver.socket), counters, console, session->event(), LoopOptions{});
    std::size_t seen = 0;
    loop.set_datagram_handler([&seen](const Endpoint&, std::span<const std::byte>) { ++seen; });

    REQUIRE(loop.run_once());
    REQUIRE(counters.value(Counter::DropOversizeDatagram) == 2);
    REQUIRE(seen == 3);  // 과대분 뒤의 정상 데이터그램도 같은 바퀴에 처리됐다
}

TEST_CASE("loop: the quit command asks for shutdown", "[loop]") {
    const WsaContext wsa;
    auto opened = open_udp_socket();
    REQUIRE(opened.ok());

    Counters counters;
    auto session = sangtachi::ConsoleSession::create();
    REQUIRE(session != nullptr);
    ConsoleQueue& console = session->queue();
    EventLoop loop(std::move(*opened.socket), counters, console, session->event(), LoopOptions{});
    REQUIRE(console.try_push("quit"));
    ::SetEvent(loop.console_event());

    REQUIRE(loop.run_once());        // 이 바퀴가 명령을 비운다
    REQUIRE_FALSE(loop.run_once());  // 다음 바퀴가 종료 이벤트에서 돌아온다
}

TEST_CASE("loop: the raw command sends the documented pattern", "[loop]") {
    const WsaContext wsa;
    auto sender = open_udp_socket();
    auto receiver = open_udp_socket();
    REQUIRE(sender.ok());
    REQUIRE(receiver.ok());

    LoopOptions options;
    options.peer = Endpoint(kLoopback, receiver.socket->local().port());

    Counters counters;
    auto session = sangtachi::ConsoleSession::create();
    REQUIRE(session != nullptr);
    ConsoleQueue& console = session->queue();
    EventLoop loop(std::move(*sender.socket), counters, console, session->event(), options);
    REQUIRE(console.try_push("raw 100"));
    ::SetEvent(loop.console_event());
    REQUIRE(loop.run_once());

    std::array<std::byte, sangtachi::network::kRecvBufferSize> buffer{};
    REQUIRE(::WaitForSingleObject(receiver.socket->read_event(), 2000) == WAIT_OBJECT_0);
    REQUIRE(receiver.socket->enumerate_events());
    const auto got = receiver.socket->recv_from(buffer);
    REQUIRE(got.status == sangtachi::network::RecvStatus::Received);
    REQUIRE(got.length == 100);
    for (std::size_t i = 0; i < 100; ++i) {
        INFO("byte " << i);
        REQUIRE(buffer[i] == static_cast<std::byte>(i & 0xFF));
    }
}

TEST_CASE("loop: a raw length outside the range sends nothing", "[loop]") {
    const WsaContext wsa;
    auto sender = open_udp_socket();
    auto receiver = open_udp_socket();
    REQUIRE(sender.ok());
    REQUIRE(receiver.ok());

    LoopOptions options;
    options.peer = Endpoint(kLoopback, receiver.socket->local().port());

    Counters counters;
    auto session = sangtachi::ConsoleSession::create();
    REQUIRE(session != nullptr);
    ConsoleQueue& console = session->queue();
    EventLoop loop(std::move(*sender.socket), counters, console, session->event(), options);
    REQUIRE(console.try_push("raw 0"));
    REQUIRE(console.try_push("raw 1473"));
    REQUIRE(console.try_push("raw abc"));
    ::SetEvent(loop.console_event());
    REQUIRE(loop.run_once());

    // 아무것도 오지 않아야 한다.
    REQUIRE(::WaitForSingleObject(receiver.socket->read_event(), 200) == WAIT_TIMEOUT);
}

TEST_CASE("loop: the console drop counter reaches the table", "[loop]") {
    const WsaContext wsa;
    auto opened = open_udp_socket();
    REQUIRE(opened.ok());

    Counters counters;
    auto session = sangtachi::ConsoleSession::create();
    REQUIRE(session != nullptr);
    ConsoleQueue& console = session->queue();
    for (std::size_t i = 0; i < sangtachi::kConsoleQueueCapacity; ++i) {
        REQUIRE(console.try_push("x"));
    }
    REQUIRE_FALSE(console.try_push("dropped"));

    EventLoop loop(std::move(*opened.socket), counters, console, session->event(), LoopOptions{});
    ::SetEvent(loop.console_event());
    REQUIRE(loop.run_once());
    REQUIRE(counters.value(Counter::ConsoleQueueDropped) == 1);
}

TEST_CASE("loop: timers keep firing under sustained receive load", "[loop][slow]") {
    // roadmap.md Phase 1 검증. 예산을 넘는 수신 부하를 10초간 끊기지 않게 준 상태에서
    // 200ms 주기 타이머의 elapsed_ms 가 400 을 넘지 않아야 한다.
    //
    // 부하가 실제로 예산을 넘게 만든다. 데이터그램마다 200 마이크로초를 태운다. 한 바퀴
    // 예산이 64 개이므로 바퀴당 약 12.8ms 이고, 그 속도로는 송신을 따라갈 수 없다.
    // 지연 값을 여기 적어 두는 것이 검증 항목이 요구하는 "그 지연 값을 기록한다" 이다.
    constexpr std::int64_t kPerDatagramMicros = 200;
    constexpr Millis kLoadMillis = 10000;

    const WsaContext wsa;
    auto sender = open_udp_socket();
    auto receiver = open_udp_socket();
    REQUIRE(sender.ok());
    REQUIRE(receiver.ok());

    const Endpoint to(kLoopback, receiver.socket->local().port());
    std::atomic<bool> stop{false};
    std::thread producer([&sender, &to, &stop]() {
        const auto payload = pattern(1200);
        while (!stop.load(std::memory_order_relaxed)) {
            (void)sender.socket->send_to(to, payload);
        }
    });

    LoopOptions options;
    options.probe_timer = true;

    Counters counters;
    auto session = sangtachi::ConsoleSession::create();
    REQUIRE(session != nullptr);
    ConsoleQueue& console = session->queue();
    EventLoop loop(std::move(*receiver.socket), counters, console, session->event(), options);
    loop.set_datagram_handler([](const Endpoint&, std::span<const std::byte>) {
        spin_microseconds(kPerDatagramMicros);
    });

    Millis worst = 0;
    std::size_t ticks = 0;
    loop.set_tick_handler([&worst, &ticks](const TimerTick& tick) {
        if (tick.elapsed_ms > worst) {
            worst = tick.elapsed_ms;
        }
        ++ticks;
    });

    const Millis started = ::GetTickCount64();
    while (::GetTickCount64() - started < kLoadMillis) {
        REQUIRE(loop.run_once());
    }
    stop.store(true, std::memory_order_relaxed);
    producer.join();

    INFO("ticks=" << ticks << " worst=" << worst);
    REQUIRE(ticks > 0);
    REQUIRE(worst <= 400);
}

TEST_CASE("loop: an idle wheel waits instead of spinning", "[loop]") {
    // architecture.md 3.2.3: WSAEnumNetworkEvents 를 빠뜨리면 첫 데이터그램 이후 이벤트가
    // 계속 신호 상태로 남아 대기가 매번 즉시 반환하고 루프가 CPU 를 태우며 스핀한다.
    //
    // 변이 시험이 그 호출을 지워도 걸리지 않아 넣은 케이스다. 바퀴를 정해진 횟수만 도는
    // 시험은 스핀을 볼 수 없다. 비었을 때 **실제로 기다리는지**를 봐야 한다.
    const WsaContext wsa;
    auto sender = open_udp_socket();
    auto receiver = open_udp_socket();
    REQUIRE(sender.ok());
    REQUIRE(receiver.ok());

    const Endpoint to(kLoopback, receiver.socket->local().port());
    REQUIRE(sender.socket->send_to(to, pattern(32)).ok);

    Counters counters;
    auto session = sangtachi::ConsoleSession::create();
    REQUIRE(session != nullptr);
    ConsoleQueue& console = session->queue();
    // 타이머를 두지 않는다. 그래야 대기 타임아웃이 INFINITE 가 되어 "기다린다" 와
    // "즉시 돌아온다" 가 갈린다.
    EventLoop loop(std::move(*receiver.socket), counters, console, session->event(), LoopOptions{});
    std::size_t seen = 0;
    loop.set_datagram_handler([&seen](const Endpoint&, std::span<const std::byte>) { ++seen; });

    REQUIRE(loop.run_once());  // 도착한 것을 다 비운다
    REQUIRE(seen == 1);

    // 이제 소스가 비었다. 다음 바퀴는 깨울 것이 올 때까지 돌아오면 안 된다.
    std::atomic<bool> returned{false};
    std::thread waiter([&loop, &returned]() {
        (void)loop.run_once();
        returned.store(true, std::memory_order_release);
    });

    ::Sleep(300);
    const bool returned_early = returned.load(std::memory_order_acquire);

    loop.request_shutdown();
    waiter.join();

    INFO("the wheel returned without anything to do");
    REQUIRE_FALSE(returned_early);
    REQUIRE(returned.load(std::memory_order_acquire));  // 종료 신호로는 돌아온다
}
