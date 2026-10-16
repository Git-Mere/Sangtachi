#include "sangtachi/adapter.hpp"
#include "sangtachi/loop.hpp"

#include "log_capture.hpp"

#include "sangtachi/console.hpp"
#include "sangtachi/counters.hpp"
#include "sangtachi/network/udp_socket.hpp"
#include "sangtachi/network/wsa.hpp"
#include "sangtachi/platform/wait.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <future>
#include <memory>
#include <span>
#include <thread>
#include <vector>

// 시험 케이스 이름은 ASCII 로만 적는다. 이유는 network/wsa_test.cpp 머리에 있다.
//
// concurrency.md 2장 대기의 순위 2 와 3장 drain_wintun, inject 를 가짜 어댑터로 본다. 진짜 Wintun 은
// 관리자 권한이 있어야 해서 이 실행 파일에서 돌지 않는다. 그쪽은 scripts/adapter-check.ps1 이 본다.

using sangtachi::Counter;
using sangtachi::Counters;
using sangtachi::EventLoop;
using sangtachi::kMaxDrain;
using sangtachi::LoopOptions;
using sangtachi::adapter::InjectResult;
using sangtachi::adapter::InjectStatus;
using sangtachi::adapter::Received;
using sangtachi::adapter::ReceiveStatus;
using sangtachi::network::open_udp_socket;
using sangtachi::network::WsaContext;

namespace {

class FakePort final : public sangtachi::adapter::Port {
public:
    FakePort() : event_(sangtachi::platform::create_event(sangtachi::platform::ResetMode::Auto)) {}
    ~FakePort() override { sangtachi::platform::close_event(event_); }
    FakePort(const FakePort&) = delete;
    FakePort& operator=(const FakePort&) = delete;

    void* read_event() const noexcept override { return null_event ? nullptr : event_; }

    Received receive() noexcept override {
        if (script.empty()) {
            return Received{ReceiveStatus::kEmpty, {}, 0};
        }
        Step step = script.front();
        script.pop_front();
        if (step.status != ReceiveStatus::kPacket) {
            return Received{step.status, {}, step.error};
        }
        packets.push_back(std::vector<std::byte>(step.size, std::byte{0x45}));
        ++outstanding;
        return Received{ReceiveStatus::kPacket, packets.back(), 0};
    }

    void release(std::span<const std::byte>) noexcept override {
        --outstanding;
        ++released;
    }

    InjectResult inject(std::span<const std::byte> packet) noexcept override {
        injected.emplace_back(packet.begin(), packet.end());
        return next_inject;
    }

    bool restart_session() noexcept override {
        ++restarts;
        return restart_ok;
    }

    void push_packets(std::size_t n, std::size_t size = 28) {
        for (std::size_t i = 0; i < n; ++i) {
            script.push_back(Step{ReceiveStatus::kPacket, size, 0});
        }
    }
    void push(ReceiveStatus status, std::uint32_t error = 0) {
        script.push_back(Step{status, 0, error});
    }
    void signal() { (void)sangtachi::platform::signal_event(event_); }

    struct Step {
        ReceiveStatus status;
        std::size_t size;
        std::uint32_t error;
    };
    std::deque<Step> script;
    std::deque<std::vector<std::byte>> packets;
    std::vector<std::vector<std::byte>> injected;
    InjectResult next_inject{InjectStatus::kOk, 0};
    int outstanding = 0;
    int released = 0;
    int restarts = 0;
    bool restart_ok = true;
    bool null_event = false;

private:
    void* event_;
};

// 루프 하나와 그 주변. 소켓은 쓰지 않지만 루프가 요구한다.
struct Rig {
    Rig() : session(sangtachi::ConsoleSession::create()) {
        auto opened = open_udp_socket();
        REQUIRE(opened.ok());
        REQUIRE(session != nullptr);
        loop = std::make_unique<EventLoop>(std::move(*opened.socket), counters, session->queue(),
                                           session->event(), LoopOptions{});
        loop->attach_adapter(&port);
        REQUIRE(loop->valid());
    }

    WsaContext wsa;
    Counters counters;
    std::shared_ptr<sangtachi::ConsoleSession> session;
    FakePort port;
    std::unique_ptr<EventLoop> loop;
};

bool shutdown_signaled(const EventLoop& loop) {
    const sangtachi::platform::WaitHandle handles[] = {loop.shutdown_event()};
    return sangtachi::platform::wait_any(std::span<const sangtachi::platform::WaitHandle>(handles, 1), 0)
               .status == sangtachi::platform::WaitStatus::Signaled;
}

}  // namespace

TEST_CASE("adapter loop: the read event wakes the wait", "[loop][adapter]") {
    // 순위 2 가 대기 집합에 없으면 이 바퀴는 타이머도 없이 영원히 기다린다.
    Rig rig;
    std::size_t seen = 0;
    rig.loop->set_adapter_handler([&seen](std::span<const std::byte>) { ++seen; });
    rig.port.push_packets(1);
    rig.port.signal();

    auto wheel = std::async(std::launch::async, [&rig] { return rig.loop->run_once(); });
    if (wheel.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
        rig.loop->request_shutdown();  // 매달린 바퀴를 풀어 준 뒤 실패로 적는다
        wheel.wait();
        FAIL("the adapter read event did not wake the loop");
    }
    REQUIRE(wheel.get());
    REQUIRE(seen == 1);
}

TEST_CASE("adapter loop: packets are counted, handed over and released", "[loop][adapter]") {
    Rig rig;
    std::vector<std::size_t> sizes;
    rig.loop->set_adapter_handler([&sizes](std::span<const std::byte> p) { sizes.push_back(p.size()); });
    rig.port.push_packets(3, 60);
    rig.port.signal();

    REQUIRE(rig.loop->run_once());
    REQUIRE(sizes == std::vector<std::size_t>{60, 60, 60});
    REQUIRE(rig.counters.value(Counter::AdapterRx) == 3);
    REQUIRE(rig.port.released == 3);
    REQUIRE(rig.port.outstanding == 0);
}

TEST_CASE("adapter loop: without a handler packets are counted and still released", "[loop][adapter]") {
    Rig rig;
    rig.port.push_packets(2);
    rig.port.signal();

    REQUIRE(rig.loop->run_once());
    REQUIRE(rig.counters.value(Counter::AdapterRx) == 2);
    REQUIRE(rig.port.released == 2);
}

TEST_CASE("adapter loop: the drain budget stops one wheel and the next wheel does not wait", "[loop][adapter]") {
    // 예산에 걸린 바퀴는 busy 다. 다음 대기가 타임아웃 0 이 아니면 남은 10개는 이벤트 없이
    // 영원히 기다린다 (자동 리셋 이벤트는 이미 가져갔다).
    Rig rig;
    std::size_t seen = 0;
    rig.loop->set_adapter_handler([&seen](std::span<const std::byte>) { ++seen; });
    rig.port.push_packets(kMaxDrain + 10);
    rig.port.signal();

    REQUIRE(rig.loop->run_once());
    REQUIRE(seen == kMaxDrain);

    auto wheel = std::async(std::launch::async, [&rig] { return rig.loop->run_once(); });
    if (wheel.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
        rig.loop->request_shutdown();
        wheel.wait();
        FAIL("a wheel that stopped on the budget did not make the next wait return at once");
    }
    REQUIRE(wheel.get());
    REQUIRE(seen == kMaxDrain + 10);
}

TEST_CASE("adapter loop: end of file signals shutdown", "[loop][adapter]") {
    Rig rig;
    const sangtachi::testing::LogCapture logs;
    rig.port.push(ReceiveStatus::kEof);
    rig.port.signal();

    REQUIRE(rig.loop->run_once());
    REQUIRE(shutdown_signaled(*rig.loop));
    REQUIRE_FALSE(rig.loop->run_once());  // 다음 바퀴가 종료 이벤트에서 끝난다
}

TEST_CASE("adapter loop: a corrupt ring restarts the session", "[loop][adapter]") {
    Rig rig;
    const sangtachi::testing::LogCapture logs;
    std::size_t seen = 0;
    rig.loop->set_adapter_handler([&seen](std::span<const std::byte>) { ++seen; });
    rig.port.push(ReceiveStatus::kCorrupt);
    rig.port.push_packets(1);
    rig.port.signal();

    REQUIRE(rig.loop->run_once());
    REQUIRE(rig.port.restarts == 1);
    REQUIRE_FALSE(shutdown_signaled(*rig.loop));
    REQUIRE(seen == 0);  // 그 바퀴는 거기서 끝난다

    rig.port.signal();
    REQUIRE(rig.loop->run_once());
    REQUIRE(seen == 1);
}

TEST_CASE("adapter loop: a failed restart signals shutdown", "[loop][adapter]") {
    // 어댑터 없이 계속 돌면 터널이 죽은 채 살아 있는 척한다 (concurrency.md 3장).
    Rig rig;
    const sangtachi::testing::LogCapture logs;
    rig.port.restart_ok = false;
    rig.port.push(ReceiveStatus::kCorrupt);
    rig.port.signal();

    REQUIRE(rig.loop->run_once());
    REQUIRE(rig.port.restarts == 1);
    REQUIRE(shutdown_signaled(*rig.loop));
    REQUIRE_FALSE(rig.loop->run_once());
}

TEST_CASE("adapter loop: another receive error is counted and ends the wheel", "[loop][adapter]") {
    Rig rig;
    const sangtachi::testing::LogCapture logs;
    std::size_t seen = 0;
    rig.loop->set_adapter_handler([&seen](std::span<const std::byte>) { ++seen; });
    rig.port.push(ReceiveStatus::kError, 31);
    rig.port.push_packets(1);
    rig.port.signal();

    REQUIRE(rig.loop->run_once());
    REQUIRE(rig.counters.value(Counter::RxErrAdapter) == 1);
    REQUIRE(seen == 0);
    REQUIRE_FALSE(shutdown_signaled(*rig.loop));
}

TEST_CASE("adapter loop: inject failures go to their own counters", "[loop][adapter]") {
    Rig rig;
    const std::vector<std::byte> packet(28, std::byte{0x45});

    rig.port.next_inject = InjectResult{InjectStatus::kOk, 0};
    REQUIRE(rig.loop->inject_packet(packet));

    rig.port.next_inject = InjectResult{InjectStatus::kRingFull, 0};
    REQUIRE_FALSE(rig.loop->inject_packet(packet));

    rig.port.next_inject = InjectResult{InjectStatus::kError, 38};
    REQUIRE_FALSE(rig.loop->inject_packet(packet));

    REQUIRE(rig.port.injected.size() == 3);  // 재시도하지 않는다
    REQUIRE(rig.counters.value(Counter::DropInjectRingFull) == 1);
    REQUIRE(rig.counters.value(Counter::DropInjectError) == 1);
}

TEST_CASE("adapter loop: inject without an adapter is an inject error", "[loop][adapter]") {
    const WsaContext wsa;
    auto opened = open_udp_socket();
    REQUIRE(opened.ok());
    Counters counters;
    auto session = sangtachi::ConsoleSession::create();
    REQUIRE(session != nullptr);
    EventLoop loop(std::move(*opened.socket), counters, session->queue(), session->event(), LoopOptions{});

    const std::vector<std::byte> packet(28, std::byte{0x45});
    REQUIRE_FALSE(loop.inject_packet(packet));
    REQUIRE(counters.value(Counter::DropInjectError) == 1);
}

TEST_CASE("adapter loop: an adapter without a read event makes the loop invalid", "[loop][adapter]") {
    // 빈자리에 널을 넣으면 WaitForMultipleObjects 가 WAIT_FAILED 를 낸다 (concurrency.md 2장).
    Rig rig;
    rig.port.null_event = true;
    REQUIRE_FALSE(rig.loop->valid());
}
