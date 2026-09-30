#include "sangtachi/loop.hpp"

#include "sangtachi/console.hpp"
#include "sangtachi/counters.hpp"
#include "sangtachi/network/endpoint.hpp"
#include "sangtachi/network/udp_socket.hpp"
#include "sangtachi/network/wsa.hpp"
#include "sangtachi/platform/wait.hpp"
#include "sangtachi/protocol_constants.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <span>
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

// 터널 후보로 분류되는 바이트열 (protocol.md 7장 수신 분류: 첫 바이트의 상위 2비트가
// 01). 분류가 들어오기 전에는 첫 바이트가 0x00 이어도 핸들러에 닿았지만 이제는 닿지
// 않는다. 그것을 확인하는 케이스는 아래 "classify" 무리가 갖는다.
std::vector<std::byte> tunnel_pattern(std::size_t size) {
    std::vector<std::byte> out(size);
    for (std::size_t i = 0; i < size; ++i) {
        out[i] = static_cast<std::byte>(i & 0xFF);
    }
    if (!out.empty()) {
        out[0] = std::byte{0x40};
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

TEST_CASE("loop: a failed wait signals the shutdown event", "[loop]") {
    // concurrency.md 7장 종료의 (1) 은 대기 실패 경로에도 적용된다. 표시만 세우고
    // 빠져나가면 그 이벤트를 기다리는 다른 스레드가 깨어나지 않는다. 지금은 기다리는
    // 스레드가 없지만 `[control]` 이 들어오는 Phase 3 에서 결함이 된다.
    const WsaContext wsa;
    auto opened = open_udp_socket();
    REQUIRE(opened.ok());

    Counters counters;
    auto session = sangtachi::ConsoleSession::create();
    REQUIRE(session != nullptr);
    ConsoleQueue& console = session->queue();

    // 대기 집합에 못 쓰는 핸들을 넣어 대기 자체를 실패시킨다. 세션의 이벤트를 쓰지 않고
    // 이 케이스가 소유하는 이벤트를 따로 만든다. 세션 것을 닫으면 소멸자가 한 번 더 닫는다.
    //
    // **닫는 것은 루프를 만든 뒤다.** 먼저 닫으면 그 핸들 값이 비고, 루프 생성자가 만드는
    // 종료 이벤트가 같은 값을 받아 대기 집합에 같은 핸들이 두 번 들어간다. 그러면 대기가
    // 실패하지 않고 영원히 기다린다. 실제로 그렇게 걸렸다.
    void* const doomed = sangtachi::platform::create_event(sangtachi::platform::ResetMode::Auto);
    REQUIRE(doomed != nullptr);

    EventLoop loop(std::move(*opened.socket), counters, console, doomed, LoopOptions{});
    REQUIRE(loop.valid());
    REQUIRE_FALSE(loop.shutdown_requested());

    sangtachi::platform::close_event(doomed);

    REQUIRE_FALSE(loop.run_once());
    REQUIRE(loop.shutdown_requested());

    // 표시가 아니라 이벤트가 실제로 신호됐는지 본다. 표시만 보면 변이가 통과한다.
    const sangtachi::platform::WaitHandle handles[] = {loop.shutdown_event()};
    const auto signaled = sangtachi::platform::wait_any(
        std::span<const sangtachi::platform::WaitHandle>(handles, 1), 0);
    REQUIRE(signaled.status == sangtachi::platform::WaitStatus::Signaled);
}

TEST_CASE("loop: one wheel drains many datagrams", "[loop]") {
    // concurrency.md 3장: 한 번의 신호로 도착한 데이터그램 여러 개가 같은 바퀴에
    // 처리된다. 한 바퀴에 하나만 읽는 구현은 여기서 걸린다.
    const WsaContext wsa;
    auto sender = open_udp_socket();
    auto receiver = open_udp_socket();
    REQUIRE(sender.ok());
    REQUIRE(receiver.ok());

    const Endpoint to(kLoopback, receiver.socket->local().port());
    for (int i = 0; i < 10; ++i) {
        REQUIRE(sender.socket->send_to(to, tunnel_pattern(32)).ok);
    }

    Counters counters;
    auto session = sangtachi::ConsoleSession::create();
    REQUIRE(session != nullptr);
    ConsoleQueue& console = session->queue();
    EventLoop loop(std::move(*receiver.socket), counters, console, session->event(), LoopOptions{});
    std::size_t seen = 0;
    loop.set_tunnel_handler([&seen](const Endpoint&, std::span<const std::byte>) { ++seen; });

    REQUIRE(loop.run_once());
    REQUIRE(seen == 10);
}

TEST_CASE("loop: the drain budget stops one wheel at the documented count", "[loop]") {
    // 예산이 없으면 한 바퀴가 전부를 삼킨다. 예산은 64 다 (concurrency.md 3장).
    const WsaContext wsa;
    auto sender = open_udp_socket();
    auto receiver = open_udp_socket();
    REQUIRE(sender.ok());
    REQUIRE(receiver.ok());

    const Endpoint to(kLoopback, receiver.socket->local().port());
    const std::size_t total = kMaxDrain + 10;
    for (std::size_t i = 0; i < total; ++i) {
        REQUIRE(sender.socket->send_to(to, tunnel_pattern(16)).ok);
    }

    Counters counters;
    auto session = sangtachi::ConsoleSession::create();
    REQUIRE(session != nullptr);
    ConsoleQueue& console = session->queue();
    EventLoop loop(std::move(*receiver.socket), counters, console, session->event(), LoopOptions{});
    std::size_t seen = 0;
    loop.set_tunnel_handler([&seen](const Endpoint&, std::span<const std::byte>) { ++seen; });

    REQUIRE(loop.run_once());
    REQUIRE(seen == kMaxDrain);

    // 남은 것은 다음 바퀴에 처리된다. 예산에 걸린 바퀴는 busy 라 즉시 돌아온다.
    REQUIRE(loop.run_once());
    REQUIRE(seen == total);
}

TEST_CASE("loop: an oversize datagram does not break the batch", "[loop]") {
    // concurrency.md 3장: 과대 데이터그램을 정상 데이터그램 사이에 끼워 보내고,
    // 카운터가 오르면서 뒤따르는 정상 데이터그램이 계속 처리되는지 본다.
    // (a) 1473바이트는 길이 비교로, (b) 1474바이트는 WSAEMSGSIZE 로 걸린다.
    const WsaContext wsa;
    auto sender = open_udp_socket();
    auto receiver = open_udp_socket();
    REQUIRE(sender.ok());
    REQUIRE(receiver.ok());

    const Endpoint to(kLoopback, receiver.socket->local().port());
    REQUIRE(sender.socket->send_to(to, tunnel_pattern(32)).ok);
    REQUIRE(sender.socket->send_to(to, tunnel_pattern(kMaxDatagram + 1)).ok);   // (a)
    REQUIRE(sender.socket->send_to(to, tunnel_pattern(32)).ok);
    REQUIRE(sender.socket->send_to(to, tunnel_pattern(kMaxDatagram + 2)).ok);   // (b)
    REQUIRE(sender.socket->send_to(to, tunnel_pattern(32)).ok);

    Counters counters;
    auto session = sangtachi::ConsoleSession::create();
    REQUIRE(session != nullptr);
    ConsoleQueue& console = session->queue();
    EventLoop loop(std::move(*receiver.socket), counters, console, session->event(), LoopOptions{});
    std::size_t seen = 0;
    loop.set_tunnel_handler([&seen](const Endpoint&, std::span<const std::byte>) { ++seen; });

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
        const auto payload = tunnel_pattern(1200);
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
    loop.set_tunnel_handler([](const Endpoint&, std::span<const std::byte>) {
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
    // concurrency.md 3장: WSAEnumNetworkEvents 를 빠뜨리면 첫 데이터그램 이후 이벤트가
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
    REQUIRE(sender.socket->send_to(to, tunnel_pattern(32)).ok);

    Counters counters;
    auto session = sangtachi::ConsoleSession::create();
    REQUIRE(session != nullptr);
    ConsoleQueue& console = session->queue();
    // 타이머를 두지 않는다. 그래야 대기 타임아웃이 INFINITE 가 되어 "기다린다" 와
    // "즉시 돌아온다" 가 갈린다.
    EventLoop loop(std::move(*receiver.socket), counters, console, session->event(), LoopOptions{});
    std::size_t seen = 0;
    loop.set_tunnel_handler([&seen](const Endpoint&, std::span<const std::byte>) { ++seen; });

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

TEST_CASE("loop: a word that only starts with raw is not the raw command", "[loop]") {
    // architecture.md 3.5 기동 입력의 어휘는 `raw <바이트 수>` 하나다. 접두만 보면
    // raw500 이 500바이트를 보낸다. 그 낱말은 unknown_command 여야 한다.
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
    REQUIRE(console.try_push("raw500"));
    REQUIRE(console.try_push("rawx"));
    ::SetEvent(loop.console_event());
    REQUIRE(loop.run_once());

    // 어느 쪽도 보내지 않았다.
    REQUIRE(::WaitForSingleObject(receiver.socket->read_event(), 200) == WAIT_TIMEOUT);

    // 어휘 안의 낱말은 그대로 보낸다.
    REQUIRE(console.try_push("raw 500"));
    ::SetEvent(loop.console_event());
    REQUIRE(loop.run_once());

    std::array<std::byte, sangtachi::network::kRecvBufferSize> buffer{};
    REQUIRE(::WaitForSingleObject(receiver.socket->read_event(), 2000) == WAIT_OBJECT_0);
    REQUIRE(receiver.socket->enumerate_events());
    const auto got = receiver.socket->recv_from(buffer);
    REQUIRE(got.status == sangtachi::network::RecvStatus::Received);
    REQUIRE(got.length == 500);
}

TEST_CASE("loop: a receive error raises rx_err_recv", "[loop]") {
    // concurrency.md 3장 루프 한 바퀴: 그 밖의 수신 오류는 카운터를 올리고 그 바퀴를
    // 끝낸다. 로그만 내면 바퀴가 왜 짧게 끝났는지가 카운터 전량 출력에 남지 않는다.
    //
    // 오류를 결정적으로 만든다. 수신 방향을 닫으면 recvfrom 이 WSAESHUTDOWN 으로
    // 실패하고, 그것은 WSAEWOULDBLOCK 도 WSAEMSGSIZE 도 아니라 "그 외 오류" 다.
    const WsaContext wsa;
    auto opened = open_udp_socket();
    REQUIRE(opened.ok());

    Counters counters;
    auto session = sangtachi::ConsoleSession::create();
    REQUIRE(session != nullptr);
    ConsoleQueue& console = session->queue();
    EventLoop loop(std::move(*opened.socket), counters, console, session->event(), LoopOptions{});
    REQUIRE(::shutdown(static_cast<SOCKET>(loop.socket().native_handle()), SD_RECEIVE) == 0);

    // 콘솔 이벤트로 대기를 깨운다. 비우기는 반환값으로 분기하지 않으므로 그 바퀴도
    // drain_udp 를 돈다.
    ::SetEvent(loop.console_event());
    REQUIRE(loop.run_once());

    // 한 번만 오른다. 오류에서 그 바퀴를 끝내지 않으면 예산만큼 오른다.
    REQUIRE(counters.value(Counter::RxErrRecv) == 1);
}

TEST_CASE("loop: the shutdown wheel refreshes the console drop counter", "[loop]") {
    // 종료 바퀴는 drain_console() 앞에서 돌아간다. 그 자리에서 반영하지 않으면 뒤따르는
    // 카운터 전량 출력이 낡은 값을 낸다 (concurrency.md 6장 텔레메트리 격리).
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
    loop.request_shutdown();
    // 이 바퀴는 명령을 비우지 않고 돌아간다.
    REQUIRE_FALSE(loop.run_once());
    REQUIRE(counters.value(Counter::ConsoleQueueDropped) == 1);
}

// --- protocol.md 7장 수신 분류 -------------------------------------------------

namespace {

// 분류만 보는 바이트열. 첫 바이트와 쿠키 자리만 뜻이 있다.
std::vector<std::byte> stun_bytes(std::size_t size = 20, bool good_cookie = true) {
    std::vector<std::byte> out(size, std::byte{0x00});
    if (size >= 8 && good_cookie) {
        out[4] = std::byte{0x21};
        out[5] = std::byte{0x12};
        out[6] = std::byte{0xA4};
        out[7] = std::byte{0x42};
    }
    return out;
}

constexpr std::uint32_t kPublicAddress = 0xCB007101u;  // 203.0.113.1

}  // namespace

TEST_CASE("classify: the source hygiene table decides before the class", "[loop][classify]") {
    // protocol.md 7장의 출발지 표. 행마다 하나씩 본다. 본문은 STUN 으로 분류될 바이트열
    // 이므로, 위생이 먼저가 아니면 BadSource 대신 Stun 이 나온다.
    const auto payload = stun_bytes();

    struct Row {
        std::uint32_t address;
        std::uint16_t port;
        sangtachi::RxClass expected;
        const char* what;
    };
    const Row rows[] = {
        {0xE0000001u, 3478, sangtachi::RxClass::BadSource, "multicast 224.0.0.1"},
        {0xEFFFFFFFu, 3478, sangtachi::RxClass::BadSource, "multicast 239.255.255.255"},
        {0xFFFFFFFFu, 3478, sangtachi::RxClass::BadSource, "limited broadcast"},
        {0x00000000u, 3478, sangtachi::RxClass::BadSource, "unspecified"},
        {kPublicAddress, 0, sangtachi::RxClass::BadSource, "port zero"},
        {0x7F000001u, 3478, sangtachi::RxClass::Stun, "loopback passes"},
        {0x7FFFFFFFu, 3478, sangtachi::RxClass::Stun, "127.255.255.255 passes"},
        {kPublicAddress, 3478, sangtachi::RxClass::Stun, "anything else passes"},
        // 원격 서브넷의 브로드캐스트는 막지 못한다. 7장이 남는 위험으로 적었다.
        {0xCB0071FFu, 3478, sangtachi::RxClass::Stun, "remote directed broadcast passes"},
    };

    for (const Row& row : rows) {
        INFO(row.what);
        REQUIRE(sangtachi::classify_datagram(Endpoint(row.address, row.port), payload) ==
                row.expected);
    }
}

TEST_CASE("classify: the leading two bits decide the class", "[loop][classify]") {
    const Endpoint from(kPublicAddress, 3478);

    // STUN: 상위 2비트 00, 길이 20 이상, buf[4..8) 이 쿠키.
    REQUIRE(sangtachi::classify_datagram(from, stun_bytes()) == sangtachi::RxClass::Stun);
    // 상위 2비트가 00 인 값 전부가 같다. 0x3F 까지다.
    auto high_bits = stun_bytes();
    high_bits[0] = std::byte{0x3F};
    REQUIRE(sangtachi::classify_datagram(from, high_bits) == sangtachi::RxClass::Stun);

    // 쿠키가 틀리면 STUN 이 아니고, 터널도 아니므로 그 외다.
    REQUIRE(sangtachi::classify_datagram(from, stun_bytes(20, false)) ==
            sangtachi::RxClass::Unclassified);
    // 19바이트는 헤더가 안 된다.
    REQUIRE(sangtachi::classify_datagram(from, stun_bytes(19)) ==
            sangtachi::RxClass::Unclassified);

    // 터널 후보: 상위 2비트 01. magic 과 version 은 분류 기준이 아니다 (7장).
    REQUIRE(sangtachi::classify_datagram(from, tunnel_pattern(1)) ==
            sangtachi::RxClass::TunnelCandidate);
    REQUIRE(sangtachi::classify_datagram(from, tunnel_pattern(1472)) ==
            sangtachi::RxClass::TunnelCandidate);
    auto tunnel_high = tunnel_pattern(8);
    tunnel_high[0] = std::byte{0x7F};
    REQUIRE(sangtachi::classify_datagram(from, tunnel_high) ==
            sangtachi::RxClass::TunnelCandidate);

    // 그 외: 상위 2비트 10 과 11, 그리고 빈 데이터그램.
    auto ten = tunnel_pattern(8);
    ten[0] = std::byte{0x80};
    REQUIRE(sangtachi::classify_datagram(from, ten) == sangtachi::RxClass::Unclassified);
    auto eleven = tunnel_pattern(8);
    eleven[0] = std::byte{0xC0};
    REQUIRE(sangtachi::classify_datagram(from, eleven) == sangtachi::RxClass::Unclassified);
    REQUIRE(sangtachi::classify_datagram(from, std::span<const std::byte>()) ==
            sangtachi::RxClass::Unclassified);
}

TEST_CASE("loop: each class goes to its own sink", "[loop][classify]") {
    // 분류가 루프에 실제로 걸려 있는지 본다. 세 종류를 한 바퀴에 넣는다.
    const WsaContext wsa;
    auto sender = open_udp_socket();
    auto receiver = open_udp_socket();
    REQUIRE(sender.ok());
    REQUIRE(receiver.ok());

    const Endpoint to(kLoopback, receiver.socket->local().port());
    REQUIRE(sender.socket->send_to(to, stun_bytes()).ok);
    REQUIRE(sender.socket->send_to(to, tunnel_pattern(40)).ok);
    REQUIRE(sender.socket->send_to(to, stun_bytes(20, false)).ok);  // 쿠키가 틀렸다

    Counters counters;
    auto session = sangtachi::ConsoleSession::create();
    REQUIRE(session != nullptr);
    ConsoleQueue& console = session->queue();
    EventLoop loop(std::move(*receiver.socket), counters, console, session->event(), LoopOptions{});

    std::size_t stun_seen = 0;
    std::size_t tunnel_seen = 0;
    loop.set_stun_handler([&stun_seen](const Endpoint&, std::span<const std::byte> payload) {
        REQUIRE(payload.size() == 20);
        ++stun_seen;
    });
    loop.set_tunnel_handler([&tunnel_seen](const Endpoint&, std::span<const std::byte> payload) {
        REQUIRE(payload.size() == 40);
        ++tunnel_seen;
    });

    REQUIRE(loop.run_once());
    REQUIRE(stun_seen == 1);
    REQUIRE(tunnel_seen == 1);
    REQUIRE(counters.value(Counter::DropUnclassified) == 1);
    REQUIRE(counters.value(Counter::DropBadSource) == 0);  // 루프백은 통과다
}

TEST_CASE("loop: send_datagram counts and logs a failed send", "[loop]") {
    // protocol.md 6장 소켓 소유권. 타입과 무관하게 tx_err_send 를 올리고 버린다.
    // STUN 이 이 자리를 쓴다 (stun_client.hpp).
    const WsaContext wsa;
    auto opened = open_udp_socket();
    auto receiver = open_udp_socket();
    REQUIRE(opened.ok());
    REQUIRE(receiver.ok());
    const Endpoint to(kLoopback, receiver.socket->local().port());

    Counters counters;
    auto session = sangtachi::ConsoleSession::create();
    REQUIRE(session != nullptr);
    ConsoleQueue& console = session->queue();
    EventLoop loop(std::move(*opened.socket), counters, console, session->event(), LoopOptions{});

    REQUIRE(loop.send_datagram(to, tunnel_pattern(32)));
    REQUIRE(counters.value(Counter::TxErrSend) == 0);

    // SO_BROADCAST 를 켜지 않은 소켓에서 브로드캐스트로 보내면 실패가 결정적이다.
    REQUIRE_FALSE(loop.send_datagram(Endpoint(0xFFFFFFFFu, 9), tunnel_pattern(10)));
    REQUIRE(counters.value(Counter::TxErrSend) == 1);
}
