#include "sangtachi/platform/console_ctrl.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <cstdint>

namespace sangtachi::platform {
namespace {

// 핸들러가 만지는 것은 이 둘뿐이다 (concurrency.md 7장 종료). `[loop]` 소유 상태는
// 건드리지 않는다.
//
// **둘 다 프로세스 수명이고 닫지 않는다.** 콘솔 핸들러는 OS 가 만든 다른 스레드에서
// 아무 때나 실행되고, `main` 이 빠져나가는 중에도 실행될 수 있다. 닫아 두면 그때
// 핸들러가 닫힌 핸들을 건드리고, 그 값이 재사용됐으면 남의 커널 객체를 신호한다.
// 프로세스가 끝나는 자리에서 핸들 둘을 남기는 비용은 0 이므로 수명을 프로세스에 맞춘다.
//
// 종료 이벤트를 `[loop]` 의 것 그대로 쓰지 않고 **복제**해 두는 이유도 같다. `EventLoop`
// 는 소멸자에서 자기 종료 이벤트를 닫는다. 복제본은 원본이 닫혀도 커널 객체를 살려 두므로
// 핸들러가 이미 사라진 객체의 핸들을 만지지 않는다.
std::atomic<HANDLE> g_shutdown{nullptr};
std::atomic<HANDLE> g_cleanup_done{nullptr};

BOOL WINAPI console_ctrl_handler(DWORD ctrl_type) {
    const CtrlSignal signal = classify_ctrl_signal(static_cast<std::uint32_t>(ctrl_type));
    return run_ctrl_handler(signal, g_shutdown.load(std::memory_order_acquire),
                            g_cleanup_done.load(std::memory_order_acquire), kCleanupWaitMs)
               ? TRUE
               : FALSE;
}

}  // namespace

CtrlSignal classify_ctrl_signal(std::uint32_t os_event) noexcept {
    switch (os_event) {
        case CTRL_C_EVENT: return CtrlSignal::Interrupt;
        case CTRL_BREAK_EVENT: return CtrlSignal::Break;
        case CTRL_CLOSE_EVENT: return CtrlSignal::Close;
        case CTRL_LOGOFF_EVENT: return CtrlSignal::Logoff;
        case CTRL_SHUTDOWN_EVENT: return CtrlSignal::Shutdown;
        default: return CtrlSignal::Unknown;
    }
}

bool run_ctrl_handler(CtrlSignal signal, WaitHandle shutdown_event, WaitHandle cleanup_done,
                      std::uint32_t cleanup_wait_ms) noexcept {
    if (signal == CtrlSignal::Unknown) {
        // 문서가 정하지 않은 신호다. 처리했다고 말하지 않고 다음 핸들러에게 넘긴다.
        return false;
    }

    if (shutdown_event != nullptr) {
        ::SetEvent(static_cast<HANDLE>(shutdown_event));
    }

    if (waits_for_cleanup(signal) && cleanup_done != nullptr) {
        // 기다린 결과로 분기하지 않는다. 기다림이 상한에 걸렸든 이벤트를 봤든 이 신호는
        // 반환하는 순간 프로세스가 끝나므로 할 수 있는 일이 남지 않는다.
        ::WaitForSingleObject(static_cast<HANDLE>(cleanup_done),
                              static_cast<DWORD>(cleanup_wait_ms));
    }
    return true;
}

CtrlHandlerInstall install_console_ctrl_handler(WaitHandle shutdown_event) noexcept {
    CtrlHandlerInstall result;

    HANDLE duplicated = nullptr;
    if (::DuplicateHandle(::GetCurrentProcess(), static_cast<HANDLE>(shutdown_event),
                          ::GetCurrentProcess(), &duplicated, 0, FALSE,
                          DUPLICATE_SAME_ACCESS) == 0) {
        result.error = static_cast<std::uint32_t>(::GetLastError());
        result.failed_op = "DuplicateHandle";
        return result;
    }

    // 수동 리셋이다. 신호되면 그대로 남아, 핸들러가 늦게 깨어나도 정리가 끝난 것을 본다.
    HANDLE cleanup = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (cleanup == nullptr) {
        result.error = static_cast<std::uint32_t>(::GetLastError());
        result.failed_op = "CreateEvent";
        ::CloseHandle(duplicated);
        return result;
    }

    g_shutdown.store(duplicated, std::memory_order_release);
    g_cleanup_done.store(cleanup, std::memory_order_release);

    if (::SetConsoleCtrlHandler(console_ctrl_handler, TRUE) == 0) {
        result.error = static_cast<std::uint32_t>(::GetLastError());
        result.failed_op = "SetConsoleCtrlHandler";
        return result;
    }

    result.ok = true;
    return result;
}

bool signal_cleanup_done() noexcept {
    HANDLE cleanup = g_cleanup_done.load(std::memory_order_acquire);
    if (cleanup == nullptr) {
        return false;
    }
    return ::SetEvent(cleanup) != 0;
}

}  // namespace sangtachi::platform
