#include "sangtachi/platform/console_ctrl.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

// 시험 케이스 이름은 ASCII 로만 적는다. 이유는 network/wsa_test.cpp 머리에 있다.

using sangtachi::platform::classify_ctrl_signal;
using sangtachi::platform::CtrlSignal;
using sangtachi::platform::kCleanupWaitMs;
using sangtachi::platform::run_ctrl_handler;
using sangtachi::platform::waits_for_cleanup;

namespace {

// 실제 핸들러가 쓰는 상한(3000ms)으로 시험을 돌리면 케이스 하나가 3초를 잔다. 기다리는
// 경로가 도는지는 짧은 상한으로도 같은 코드로 판정된다.
constexpr std::uint32_t kShortWaitMs = 150;

struct Event {
    HANDLE handle = nullptr;

    explicit Event(BOOL manual_reset) : handle(::CreateEventW(nullptr, manual_reset, FALSE, nullptr)) {}
    ~Event() {
        if (handle != nullptr) {
            ::CloseHandle(handle);
        }
    }
    Event(const Event&) = delete;
    Event& operator=(const Event&) = delete;

    [[nodiscard]] bool signaled() const noexcept {
        return ::WaitForSingleObject(handle, 0) == WAIT_OBJECT_0;
    }
};

}  // namespace

TEST_CASE("console ctrl: the OS values map to the documented five", "[console_ctrl]") {
    // concurrency.md 7장 종료의 표에 있는 다섯. 그 밖의 값은 Unknown 이다.
    REQUIRE(classify_ctrl_signal(CTRL_C_EVENT) == CtrlSignal::Interrupt);
    REQUIRE(classify_ctrl_signal(CTRL_BREAK_EVENT) == CtrlSignal::Break);
    REQUIRE(classify_ctrl_signal(CTRL_CLOSE_EVENT) == CtrlSignal::Close);
    REQUIRE(classify_ctrl_signal(CTRL_LOGOFF_EVENT) == CtrlSignal::Logoff);
    REQUIRE(classify_ctrl_signal(CTRL_SHUTDOWN_EVENT) == CtrlSignal::Shutdown);
    REQUIRE(classify_ctrl_signal(4242) == CtrlSignal::Unknown);
}

TEST_CASE("console ctrl: only the three closing signals wait", "[console_ctrl]") {
    // 뒤의 셋은 핸들러가 반환하는 순간 OS 가 프로세스를 끝낸다 (concurrency.md 7장 종료).
    STATIC_REQUIRE_FALSE(waits_for_cleanup(CtrlSignal::Interrupt));
    STATIC_REQUIRE_FALSE(waits_for_cleanup(CtrlSignal::Break));
    STATIC_REQUIRE(waits_for_cleanup(CtrlSignal::Close));
    STATIC_REQUIRE(waits_for_cleanup(CtrlSignal::Logoff));
    STATIC_REQUIRE(waits_for_cleanup(CtrlSignal::Shutdown));
    STATIC_REQUIRE_FALSE(waits_for_cleanup(CtrlSignal::Unknown));
    // 상한 값의 출처는 문서다. 코드에서 새로 정하지 않는다.
    STATIC_REQUIRE(kCleanupWaitMs == 3000);
}

TEST_CASE("console ctrl: ctrl-c signals shutdown and returns at once", "[console_ctrl]") {
    Event shutdown(TRUE);
    Event cleanup(TRUE);
    REQUIRE(shutdown.handle != nullptr);
    REQUIRE(cleanup.handle != nullptr);

    const ULONGLONG started = ::GetTickCount64();
    REQUIRE(run_ctrl_handler(CtrlSignal::Interrupt, shutdown.handle, cleanup.handle,
                             kShortWaitMs));
    const ULONGLONG elapsed = ::GetTickCount64() - started;

    REQUIRE(shutdown.signaled());
    // 정리 완료를 신호하지 않았는데도 기다리지 않았다.
    INFO("elapsed=" << elapsed);
    REQUIRE(elapsed < kShortWaitMs);
    REQUIRE_FALSE(cleanup.signaled());
}

TEST_CASE("console ctrl: closing the window waits for the cleanup event", "[console_ctrl]") {
    Event shutdown(TRUE);
    Event cleanup(TRUE);
    REQUIRE(shutdown.handle != nullptr);
    REQUIRE(cleanup.handle != nullptr);

    const ULONGLONG started = ::GetTickCount64();
    REQUIRE(run_ctrl_handler(CtrlSignal::Close, shutdown.handle, cleanup.handle, kShortWaitMs));
    const ULONGLONG elapsed = ::GetTickCount64() - started;

    REQUIRE(shutdown.signaled());
    // 정리 완료가 오지 않으면 상한까지 기다린다. 시계 해상도만큼의 여유를 둔다.
    INFO("elapsed=" << elapsed);
    REQUIRE(elapsed + 16 >= kShortWaitMs);
}

TEST_CASE("console ctrl: a signalled cleanup event ends the wait early", "[console_ctrl]") {
    Event shutdown(TRUE);
    Event cleanup(TRUE);
    REQUIRE(shutdown.handle != nullptr);
    REQUIRE(cleanup.handle != nullptr);
    REQUIRE(::SetEvent(cleanup.handle) != 0);

    const ULONGLONG started = ::GetTickCount64();
    REQUIRE(run_ctrl_handler(CtrlSignal::Shutdown, shutdown.handle, cleanup.handle,
                             kShortWaitMs));
    const ULONGLONG elapsed = ::GetTickCount64() - started;

    REQUIRE(shutdown.signaled());
    INFO("elapsed=" << elapsed);
    REQUIRE(elapsed < kShortWaitMs);
}

TEST_CASE("console ctrl: an unknown signal is not handled", "[console_ctrl]") {
    // 문서가 정하지 않은 신호다. 처리했다고 말하지 않고 종료도 걸지 않는다.
    Event shutdown(TRUE);
    Event cleanup(TRUE);
    REQUIRE(shutdown.handle != nullptr);
    REQUIRE(cleanup.handle != nullptr);

    REQUIRE_FALSE(run_ctrl_handler(CtrlSignal::Unknown, shutdown.handle, cleanup.handle,
                                   kShortWaitMs));
    REQUIRE_FALSE(shutdown.signaled());
}

TEST_CASE("console ctrl: null handles do not crash the handler", "[console_ctrl]") {
    // 설치 전에 신호가 오면 핸들이 없다. 그래도 돌아와야 한다.
    REQUIRE(run_ctrl_handler(CtrlSignal::Interrupt, nullptr, nullptr, kShortWaitMs));
    REQUIRE(run_ctrl_handler(CtrlSignal::Close, nullptr, nullptr, kShortWaitMs));
}
