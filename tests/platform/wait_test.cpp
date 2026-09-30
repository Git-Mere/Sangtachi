#include "sangtachi/platform/wait.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

// 시험 케이스 이름은 ASCII 로만 적는다. 이유는 network/wsa_test.cpp 머리에 있다.

using sangtachi::platform::close_event;
using sangtachi::platform::create_event;
using sangtachi::platform::kInfiniteWaitMs;
using sangtachi::platform::monotonic_ms;
using sangtachi::platform::ResetMode;
using sangtachi::platform::signal_event;
using sangtachi::platform::wait_any;
using sangtachi::platform::WaitHandle;
using sangtachi::platform::WaitMillis;
using sangtachi::platform::WaitResult;
using sangtachi::platform::WaitStatus;

namespace {

// 이벤트 하나를 들고 스코프 끝에서 닫는다. REQUIRE 가 중간에 빠져나가도 샌 핸들이
// 남지 않게 한다.
class OwnedEvent {
public:
    explicit OwnedEvent(ResetMode mode) noexcept : handle_(create_event(mode)) {}
    ~OwnedEvent() { close_event(handle_); }

    OwnedEvent(const OwnedEvent&) = delete;
    OwnedEvent& operator=(const OwnedEvent&) = delete;
    OwnedEvent(OwnedEvent&&) = delete;
    OwnedEvent& operator=(OwnedEvent&&) = delete;

    [[nodiscard]] WaitHandle get() const noexcept { return handle_; }

private:
    WaitHandle handle_ = nullptr;
};

WaitResult wait_on(std::span<const WaitHandle> handles, std::uint32_t timeout_ms) {
    return wait_any(handles, timeout_ms);
}

}  // namespace

TEST_CASE("wait: the timeout path returns Timeout and the clock advances", "[platform][wait]") {
    // 타임아웃 경로와 단조 시계를 같은 케이스에서 본다. 시계가 멈춰 있으면 경과가 0 이고,
    // 대기가 타임아웃을 지키지 않으면 status 가 다르다.
    const OwnedEvent never(ResetMode::Auto);
    REQUIRE(never.get() != nullptr);

    const std::array<WaitHandle, 1> handles{never.get()};

    const WaitMillis before = monotonic_ms();
    const WaitResult result = wait_on(handles, 50);
    const WaitMillis after = monotonic_ms();

    REQUIRE(result.status == WaitStatus::Timeout);
    // GetTickCount64 의 눈금이 약 15.6ms 다. 50ms 를 잔 뒤의 관측은 그 눈금만큼 작게
    // 나올 수 있으므로 하한을 30ms 로 둔다.
    REQUIRE(after >= before);
    REQUIRE(after - before >= 30);
}

TEST_CASE("wait: a signaled handle reports its own array index", "[platform][wait]") {
    const OwnedEvent a(ResetMode::Auto);
    const OwnedEvent b(ResetMode::Auto);
    const OwnedEvent c(ResetMode::Auto);
    REQUIRE(a.get() != nullptr);
    REQUIRE(b.get() != nullptr);
    REQUIRE(c.get() != nullptr);

    const std::array<WaitHandle, 3> handles{a.get(), b.get(), c.get()};

    REQUIRE(signal_event(c.get()));
    const WaitResult result = wait_on(handles, 1000);
    REQUIRE(result.status == WaitStatus::Signaled);
    REQUIRE(result.index == 2);
}

TEST_CASE("wait: two signaled handles give the lowest index", "[platform][wait]") {
    // concurrency.md 3장이 반환값으로 분기하지 말라고 한 근거다. 여럿이 신호되어도
    // 가장 낮은 인덱스 하나만 돌아온다.
    const OwnedEvent a(ResetMode::Manual);
    const OwnedEvent b(ResetMode::Manual);
    const OwnedEvent c(ResetMode::Manual);
    REQUIRE(a.get() != nullptr);
    REQUIRE(b.get() != nullptr);
    REQUIRE(c.get() != nullptr);

    const std::array<WaitHandle, 3> handles{a.get(), b.get(), c.get()};

    REQUIRE(signal_event(b.get()));
    REQUIRE(signal_event(c.get()));

    const WaitResult result = wait_on(handles, 1000);
    REQUIRE(result.status == WaitStatus::Signaled);
    REQUIRE(result.index == 1);
}

TEST_CASE("wait: a manual reset event stays signaled", "[platform][wait]") {
    // 종료 이벤트가 이 모드다. 한 번 신호되면 그 뒤의 모든 대기가 곧바로 돌아와야 한다.
    const OwnedEvent manual(ResetMode::Manual);
    REQUIRE(manual.get() != nullptr);
    const std::array<WaitHandle, 1> handles{manual.get()};

    REQUIRE(wait_on(handles, 0).status == WaitStatus::Timeout);  // 만든 직후는 비신호다
    REQUIRE(signal_event(manual.get()));

    const WaitResult first = wait_on(handles, 0);
    REQUIRE(first.status == WaitStatus::Signaled);
    REQUIRE(first.index == 0);

    const WaitResult second = wait_on(handles, 0);
    REQUIRE(second.status == WaitStatus::Signaled);
    REQUIRE(second.index == 0);
}

TEST_CASE("wait: an auto reset event is consumed by one wait", "[platform][wait]") {
    // 콘솔 이벤트가 이 모드다. 대기가 신호를 가져가므로 `[loop]` 가 리셋을 따로 부르지
    // 않는다 (concurrency.md 3장).
    const OwnedEvent automatic(ResetMode::Auto);
    REQUIRE(automatic.get() != nullptr);
    const std::array<WaitHandle, 1> handles{automatic.get()};

    REQUIRE(wait_on(handles, 0).status == WaitStatus::Timeout);  // 만든 직후는 비신호다
    REQUIRE(signal_event(automatic.get()));

    const WaitResult first = wait_on(handles, 0);
    REQUIRE(first.status == WaitStatus::Signaled);
    REQUIRE(first.index == 0);

    const WaitResult second = wait_on(handles, 0);
    REQUIRE(second.status == WaitStatus::Timeout);
}

TEST_CASE("wait: an empty handle set fails instead of waiting", "[platform][wait]") {
    // 빈 배열로 기다리면 OS 는 오류를 내고, 무한 타임아웃이면 영원히 돌아오지 않는
    // 구현도 가능하다. 여기서 먼저 판정한다.
    const WaitResult result = wait_any(std::span<const WaitHandle>(), kInfiniteWaitMs);
    REQUIRE(result.status == WaitStatus::Failed);
    REQUIRE(result.error != 0);
}

TEST_CASE("wait: an invalid handle reports Failed with an OS error code", "[platform][wait]") {
    // 대기가 실패하는 경로. 루프는 이 경로에서 로그를 남기고 종료 정리로 간다.
    //
    // 0xDEAD 는 커널 핸들의 정렬 조건을 만족하지 않으므로 어떤 객체도 가리키지 않는다.
    // 닫은 핸들을 쓰지 않는 이유는 그 값이 다른 객체에 재사용될 수 있기 때문이다.
    const auto bogus = reinterpret_cast<WaitHandle>(static_cast<std::uintptr_t>(0xDEAD));
    const std::array<WaitHandle, 1> handles{bogus};

    const WaitResult result = wait_on(handles, 0);
    REQUIRE(result.status == WaitStatus::Failed);
    REQUIRE(result.error != 0);
}

TEST_CASE("wait: signaling and closing a null handle is safe", "[platform][wait]") {
    REQUIRE_FALSE(signal_event(nullptr));
    close_event(nullptr);  // 아무것도 하지 않는다
}
