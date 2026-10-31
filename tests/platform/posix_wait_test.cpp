#include "sangtachi/platform/wait.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <thread>
#include <vector>

// POSIX 대기(파이프와 poll)가 Windows 대기 의미를 재현하는지 본다 (ADR 0035 결정 4).
// 공용 의미(낮은 인덱스, 수동 유지, 자동 소비, 0xDEAD)는 platform/wait_test.cpp 가 본다.
// 시험 케이스 이름은 ASCII 로만 적는다.

using sangtachi::platform::close_event;
using sangtachi::platform::create_event;
using sangtachi::platform::duplicate_event;
using sangtachi::platform::ResetMode;
using sangtachi::platform::signal_event;
using sangtachi::platform::wait_any;
using sangtachi::platform::WaitHandle;
using sangtachi::platform::WaitResult;
using sangtachi::platform::WaitStatus;

namespace {

class OwnedEvent {
public:
    explicit OwnedEvent(ResetMode mode) noexcept : handle_(create_event(mode)) {}
    explicit OwnedEvent(WaitHandle handle) noexcept : handle_(handle) {}
    ~OwnedEvent() { close_event(handle_); }
    OwnedEvent(const OwnedEvent&) = delete;
    OwnedEvent& operator=(const OwnedEvent&) = delete;
    OwnedEvent(OwnedEvent&&) = delete;
    OwnedEvent& operator=(OwnedEvent&&) = delete;

    [[nodiscard]] WaitHandle get() const noexcept { return handle_; }

private:
    WaitHandle handle_ = nullptr;
};

WaitResult wait_one(WaitHandle handle, std::uint32_t timeout_ms) {
    const std::array<WaitHandle, 1> handles{handle};
    return wait_any(handles, timeout_ms);
}

}  // namespace

TEST_CASE("posix wait: repeated signals of an auto event merge into one wake", "[platform][wait][posix]") {
    // Windows 자동 리셋 이벤트는 신호 상태가 하나뿐이다. 세 번 신호해도 깨움은 한 번이다.
    const OwnedEvent automatic(ResetMode::Auto);
    REQUIRE(automatic.get() != nullptr);
    REQUIRE(signal_event(automatic.get()));
    REQUIRE(signal_event(automatic.get()));
    REQUIRE(signal_event(automatic.get()));

    REQUIRE(wait_one(automatic.get(), 0).status == WaitStatus::Signaled);
    REQUIRE(wait_one(automatic.get(), 0).status == WaitStatus::Timeout);
}

TEST_CASE("posix wait: only the returned auto event is consumed", "[platform][wait][posix]") {
    // 두 자동 이벤트가 다 신호되어 있으면 낮은 인덱스 하나만 가져간다. 높은 쪽은 다음 대기가 받는다.
    const OwnedEvent low(ResetMode::Auto);
    const OwnedEvent high(ResetMode::Auto);
    const std::array<WaitHandle, 2> handles{low.get(), high.get()};
    REQUIRE(signal_event(low.get()));
    REQUIRE(signal_event(high.get()));

    const WaitResult first = wait_any(handles, 0);
    REQUIRE(first.status == WaitStatus::Signaled);
    REQUIRE(first.index == 0);
    const WaitResult second = wait_any(handles, 0);
    REQUIRE(second.status == WaitStatus::Signaled);
    REQUIRE(second.index == 1);
    REQUIRE(wait_any(handles, 0).status == WaitStatus::Timeout);
}

TEST_CASE("posix wait: a manual event in front keeps winning over an auto event", "[platform][wait][posix]") {
    // 종료(수동, 인덱스 0)가 신호되면 뒤의 자동 이벤트는 소비되지 않고 남는다 (loop.cpp 의 배열).
    const OwnedEvent shutdown(ResetMode::Manual);
    const OwnedEvent console(ResetMode::Auto);
    const std::array<WaitHandle, 2> handles{shutdown.get(), console.get()};
    REQUIRE(signal_event(console.get()));
    REQUIRE(signal_event(shutdown.get()));

    REQUIRE(wait_any(handles, 0).index == 0);
    REQUIRE(wait_any(handles, 0).index == 0);
    REQUIRE(wait_one(console.get(), 0).status == WaitStatus::Signaled);
}

TEST_CASE("posix wait: a duplicated handle is the same event", "[platform][wait][posix]") {
    const OwnedEvent original(ResetMode::Manual);
    const OwnedEvent copy(duplicate_event(original.get()));
    REQUIRE(copy.get() != nullptr);
    REQUIRE(copy.get() != original.get());

    REQUIRE(signal_event(copy.get()));
    REQUIRE(wait_one(original.get(), 0).status == WaitStatus::Signaled);
}

TEST_CASE("posix wait: a duplicate keeps the event alive after the original closes", "[platform][wait][posix]") {
    // console_ctrl 가 종료 이벤트를 복제해 드는 이유다 (concurrency.md 7장).
    const WaitHandle original = create_event(ResetMode::Manual);
    REQUIRE(original != nullptr);
    const OwnedEvent copy(duplicate_event(original));
    REQUIRE(copy.get() != nullptr);
    close_event(original);

    REQUIRE(signal_event(copy.get()));
    REQUIRE(wait_one(copy.get(), 0).status == WaitStatus::Signaled);
}

TEST_CASE("posix wait: a duplicated auto event is still consumed by one wait", "[platform][wait][posix]") {
    const OwnedEvent original(ResetMode::Auto);
    const OwnedEvent copy(duplicate_event(original.get()));
    REQUIRE(signal_event(original.get()));
    REQUIRE(wait_one(copy.get(), 0).status == WaitStatus::Signaled);
    REQUIRE(wait_one(original.get(), 0).status == WaitStatus::Timeout);
}

TEST_CASE("posix wait: a null handle in the set fails without waiting", "[platform][wait][posix]") {
    const OwnedEvent signaled(ResetMode::Manual);
    REQUIRE(signal_event(signaled.get()));
    const std::array<WaitHandle, 2> handles{signaled.get(), nullptr};
    const WaitResult result = wait_any(handles, 0);
    REQUIRE(result.status == WaitStatus::Failed);
    REQUIRE(result.error != 0);
}

TEST_CASE("posix wait: a bogus handle fails the whole wait even behind a signaled one", "[platform][wait][posix]") {
    // WaitForMultipleObjects 는 무효 핸들 하나로 대기 전체가 실패한다. 앞의 신호로 덮지 않는다.
    const OwnedEvent signaled(ResetMode::Manual);
    REQUIRE(signal_event(signaled.get()));
    const auto bogus = reinterpret_cast<WaitHandle>(static_cast<std::uintptr_t>(0xDEAD));
    const std::array<WaitHandle, 2> handles{signaled.get(), bogus};
    const WaitResult result = wait_any(handles, 0);
    REQUIRE(result.status == WaitStatus::Failed);
    REQUIRE(result.error != 0);
}

TEST_CASE("posix wait: a handle that decodes to a negative fd fails instead of hanging", "[platform][wait][posix]") {
    // 1 은 읽기 fd 자리가 0 이라 -1 로 풀린다. poll 은 음수 fd 를 건너뛰므로 거르지 않으면 무한 대기가 돌아오지 않는다.
    const auto bogus = reinterpret_cast<WaitHandle>(static_cast<std::uintptr_t>(1));
    const std::array<WaitHandle, 1> handles{bogus};
    const WaitResult result = wait_any(handles, sangtachi::platform::kInfiniteWaitMs);
    REQUIRE(result.status == WaitStatus::Failed);
    REQUIRE(result.error != 0);
}

TEST_CASE("posix wait: more than 64 handles fail without waiting", "[platform][wait][posix]") {
    const OwnedEvent signaled(ResetMode::Manual);
    REQUIRE(signal_event(signaled.get()));
    const std::vector<WaitHandle> handles(65, signaled.get());
    REQUIRE(wait_any(handles, 0).status == WaitStatus::Failed);
    REQUIRE(wait_any(std::span<const WaitHandle>(handles).first(64), 0).status ==
            WaitStatus::Signaled);
}

TEST_CASE("posix wait: a signal from another thread wakes an infinite wait", "[platform][wait][posix]") {
    const OwnedEvent automatic(ResetMode::Auto);
    std::thread signaler([&] { (void)signal_event(automatic.get()); });
    const WaitResult result = wait_one(automatic.get(), sangtachi::platform::kInfiniteWaitMs);
    signaler.join();
    REQUIRE(result.status == WaitStatus::Signaled);
}

TEST_CASE("posix wait: one signal wakes exactly one of two auto waiters", "[platform][wait][posix]") {
    // 두 대기자가 같은 자동 이벤트에서 깨어나도 신호를 가져가는 것은 하나다. 진 쪽은 다시 기다린다.
    const OwnedEvent automatic(ResetMode::Auto);
    std::atomic<int> woken{0};
    auto waiter = [&] {
        if (wait_one(automatic.get(), 500).status == WaitStatus::Signaled) {
            woken.fetch_add(1);
        }
    };
    std::thread a(waiter);
    std::thread b(waiter);
    REQUIRE(signal_event(automatic.get()));
    a.join();
    b.join();
    REQUIRE(woken.load() == 1);
}

TEST_CASE("posix wait: signaling a read only handle fails", "[platform][wait][posix]") {
    // 소켓과 TUN 의 읽기 핸들은 쓰는 쪽이 없다. 신호할 수 없다.
    const OwnedEvent event(ResetMode::Manual);
    const auto read_only = reinterpret_cast<WaitHandle>(
        reinterpret_cast<std::uintptr_t>(event.get()) & 0xFFFFFFFFu);
    REQUIRE_FALSE(signal_event(read_only));
}
