#include "sangtachi/platform/console_ctrl.hpp"

#include <pthread.h>
#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>

// 종료 신호는 전용 스레드가 sigwaitinfo 로 받는다 (ADR 0035 결정 5). 신호 처리기 안에서 정리를
// 기다리면 그 신호가 배달된 스레드(예: [loop])가 멈춰 정리가 끝나지 않는다.
//
// 표준 입력도 여기서 읽는다. stdio 로 읽으면 블록한 읽기가 stdin 잠금을 쥐고, exit 가 그 잠금을 기다려
// 다음 입력이 올 때까지 프로세스가 끝나지 않는다 (concurrency.md 7장 종료의 `[console]`).

namespace sangtachi::platform {
namespace {

// 신호 스레드가 만지는 것은 이 둘뿐이다. 둘 다 프로세스 수명이고 닫지 않는다 (win32 판과 같은 이유).
std::atomic<WaitHandle> g_shutdown{nullptr};
std::atomic<WaitHandle> g_cleanup_done{nullptr};

sigset_t shutdown_signals() noexcept {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGQUIT);
    sigaddset(&set, SIGHUP);
    sigaddset(&set, SIGTERM);
    return set;
}

void* signal_thread(void* /*unused*/) {
    const sigset_t set = shutdown_signals();
    while (true) {
        siginfo_t info{};
        const int signo = ::sigwaitinfo(&set, &info);
        if (signo < 0) {
            continue;  // EINTR
        }
        // 핸들러가 반환해도 OS 가 프로세스를 끝내지 않는다. 다음 신호를 계속 받는다.
        (void)run_ctrl_handler(classify_ctrl_signal(static_cast<std::uint32_t>(signo)),
                               g_shutdown.load(std::memory_order_acquire),
                               g_cleanup_done.load(std::memory_order_acquire), kCleanupWaitMs);
    }
    return nullptr;
}

CtrlHandlerInstall install_failure(std::string_view op, int error) noexcept {
    CtrlHandlerInstall result;
    result.error = static_cast<std::uint32_t>(error);
    result.failed_op = op;
    return result;
}

}  // namespace

CtrlSignal classify_ctrl_signal(std::uint32_t os_event) noexcept {
    switch (os_event) {
        case SIGINT: return CtrlSignal::Interrupt;
        case SIGQUIT: return CtrlSignal::Break;
        case SIGHUP: return CtrlSignal::Close;
        case SIGTERM: return CtrlSignal::Shutdown;
        default: return CtrlSignal::Unknown;
    }
}

bool run_ctrl_handler(CtrlSignal signal, WaitHandle shutdown_event, WaitHandle cleanup_done,
                      std::uint32_t cleanup_wait_ms) noexcept {
    if (signal == CtrlSignal::Unknown) {
        return false;
    }

    if (shutdown_event != nullptr) {
        (void)signal_event(shutdown_event);
    }

    if (waits_for_cleanup(signal) && cleanup_done != nullptr) {
        // 기다린 결과로 분기하지 않는다 (win32 판과 같다).
        const WaitHandle handles[] = {cleanup_done};
        (void)wait_any(std::span<const WaitHandle>(handles, 1), cleanup_wait_ms);
    }
    return true;
}

CtrlHandlerInstall install_console_ctrl_handler(WaitHandle shutdown_event) noexcept {
    const WaitHandle duplicated = duplicate_event(shutdown_event);
    if (duplicated == nullptr) {
        return install_failure("dup", errno);
    }

    // 수동 리셋이다. 신호되면 그대로 남아, 신호 스레드가 늦게 기다려도 정리가 끝난 것을 본다.
    const WaitHandle cleanup = create_event(ResetMode::Manual);
    if (cleanup == nullptr) {
        const int error = errno;
        close_event(duplicated);
        return install_failure("pipe", error);
    }

    g_shutdown.store(duplicated, std::memory_order_release);
    g_cleanup_done.store(cleanup, std::memory_order_release);

    // 이 마스크는 뒤에 만드는 스레드가 물려받는다. 그래서 첫 스레드를 만들기 전에 설치한다
    // (client_process.cpp 가 ControlChannel::start 앞에서 부른다).
    const sigset_t set = shutdown_signals();
    if (const int error = ::pthread_sigmask(SIG_BLOCK, &set, nullptr); error != 0) {
        return install_failure("pthread_sigmask", error);
    }

    // 상대가 닫은 소켓에 쓰면 오는 SIGPIPE 의 기본 동작은 프로세스 종료다. 쓰기 실패는 반환값으로 다룬다.
    struct sigaction ignore{};
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);
    if (::sigaction(SIGPIPE, &ignore, nullptr) != 0) {
        return install_failure("sigaction", errno);
    }

    pthread_t thread{};
    if (const int error = ::pthread_create(&thread, nullptr, signal_thread, nullptr); error != 0) {
        return install_failure("pthread_create", error);
    }
    ::pthread_detach(thread);

    CtrlHandlerInstall result;
    result.ok = true;
    return result;
}

bool signal_cleanup_done() noexcept {
    const WaitHandle cleanup = g_cleanup_done.load(std::memory_order_acquire);
    if (cleanup == nullptr) {
        return false;
    }
    return signal_event(cleanup);
}

bool console_is_own() noexcept {
    // 탐색기 더블클릭처럼 클라이언트만을 위해 열린 창이 없다. 기동 실패 안내를 기다리지 않는다.
    return false;
}

void wait_for_enter() noexcept {
    char buffer[256];
    (void)::read(STDIN_FILENO, buffer, sizeof(buffer));
}

bool read_stdin_line(std::string& line) {
    // 표준 입력은 프로세스에 하나라 읽고 남은 바이트도 하나다. 읽는 쪽은 `[console]` 하나뿐이다.
    // 소멸시키지 않는다. `[console]` 은 join 하지 않으므로 exit 가 정적 객체를 부수는 동안에도 읽고 있을 수 있다.
    static std::string& pending = *new std::string;
    while (true) {
        if (const auto newline = pending.find('\n'); newline != std::string::npos) {
            line.assign(pending, 0, newline);
            pending.erase(0, newline + 1);
            return true;
        }
        char buffer[4096];
        const ssize_t got = ::read(STDIN_FILENO, buffer, sizeof(buffer));
        if (got > 0) {
            pending.append(buffer, static_cast<std::size_t>(got));
            continue;
        }
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (pending.empty()) {
            return false;
        }
        line = std::move(pending);
        pending.clear();
        return true;
    }
}

}  // namespace sangtachi::platform
