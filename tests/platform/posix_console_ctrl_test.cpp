#include "sangtachi/platform/console_ctrl.hpp"

#include "sangtachi/platform/wait.hpp"

#include <catch2/catch_test_macros.hpp>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <thread>

// POSIX 종료 신호의 분류와 신호 스레드 (ADR 0035 결정 5, concurrency.md 7장).
// 시험 케이스 이름은 ASCII 로만 적는다.

using sangtachi::platform::classify_ctrl_signal;
using sangtachi::platform::close_event;
using sangtachi::platform::create_event;
using sangtachi::platform::CtrlSignal;
using sangtachi::platform::install_console_ctrl_handler;
using sangtachi::platform::monotonic_ms;
using sangtachi::platform::read_stdin_line;
using sangtachi::platform::ResetMode;
using sangtachi::platform::run_ctrl_handler;
using sangtachi::platform::signal_cleanup_done;
using sangtachi::platform::signal_event;
using sangtachi::platform::wait_any;
using sangtachi::platform::WaitHandle;
using sangtachi::platform::WaitStatus;

namespace {

WaitStatus peek(WaitHandle handle, std::uint32_t timeout_ms = 0) {
    const std::array<WaitHandle, 1> handles{handle};
    return wait_any(handles, timeout_ms).status;
}

// 표준 입력을 파이프로 바꾸고 쓰는 쪽을 돌려준다.
int pipe_into_stdin() {
    int fds[2];
    REQUIRE(::pipe(fds) == 0);
    REQUIRE(::dup2(fds[0], STDIN_FILENO) == STDIN_FILENO);
    ::close(fds[0]);
    return fds[1];
}

}  // namespace

TEST_CASE("posix ctrl: the four shutdown signals map to the table", "[platform][ctrl][posix]") {
    REQUIRE(classify_ctrl_signal(SIGINT) == CtrlSignal::Interrupt);
    REQUIRE(classify_ctrl_signal(SIGQUIT) == CtrlSignal::Break);
    REQUIRE(classify_ctrl_signal(SIGHUP) == CtrlSignal::Close);
    REQUIRE(classify_ctrl_signal(SIGTERM) == CtrlSignal::Shutdown);
    REQUIRE(classify_ctrl_signal(SIGUSR1) == CtrlSignal::Unknown);
    REQUIRE(classify_ctrl_signal(SIGPIPE) == CtrlSignal::Unknown);
    REQUIRE(classify_ctrl_signal(0) == CtrlSignal::Unknown);
}

TEST_CASE("posix ctrl: an unknown signal is not handled and signals nothing", "[platform][ctrl][posix]") {
    const WaitHandle shutdown = create_event(ResetMode::Manual);
    REQUIRE_FALSE(run_ctrl_handler(CtrlSignal::Unknown, shutdown, nullptr, 0));
    REQUIRE(peek(shutdown) == WaitStatus::Timeout);
    close_event(shutdown);
}

TEST_CASE("posix ctrl: interrupt signals shutdown and does not wait", "[platform][ctrl][posix]") {
    const WaitHandle shutdown = create_event(ResetMode::Manual);
    const WaitHandle cleanup = create_event(ResetMode::Manual);
    const auto before = monotonic_ms();
    REQUIRE(run_ctrl_handler(CtrlSignal::Interrupt, shutdown, cleanup, 2000));
    REQUIRE(monotonic_ms() - before < 1000);
    REQUIRE(peek(shutdown) == WaitStatus::Signaled);
    close_event(shutdown);
    close_event(cleanup);
}

TEST_CASE("posix ctrl: close returns as soon as cleanup is done", "[platform][ctrl][posix]") {
    const WaitHandle shutdown = create_event(ResetMode::Manual);
    const WaitHandle cleanup = create_event(ResetMode::Manual);
    REQUIRE(signal_event(cleanup));
    const auto before = monotonic_ms();
    REQUIRE(run_ctrl_handler(CtrlSignal::Close, shutdown, cleanup, 5000));
    REQUIRE(monotonic_ms() - before < 1000);
    REQUIRE(peek(shutdown) == WaitStatus::Signaled);
    close_event(shutdown);
    close_event(cleanup);
}

TEST_CASE("posix ctrl: shutdown waits for cleanup up to the bound", "[platform][ctrl][posix][slow]") {
    const WaitHandle shutdown = create_event(ResetMode::Manual);
    const WaitHandle cleanup = create_event(ResetMode::Manual);
    const auto before = monotonic_ms();
    REQUIRE(run_ctrl_handler(CtrlSignal::Shutdown, shutdown, cleanup, 200));
    const auto elapsed = monotonic_ms() - before;
    REQUIRE(elapsed >= 190);
    REQUIRE(elapsed < 2000);
    close_event(shutdown);
    close_event(cleanup);
}

TEST_CASE("posix ctrl: a real SIGTERM reaches the shutdown event through the signal thread", "[platform][ctrl][posix]") {
    // ctest 는 케이스마다 프로세스를 따로 띄운다. 신호 마스크와 신호 스레드가 이 프로세스에만 남는다.
    REQUIRE_FALSE(signal_cleanup_done());  // 설치 전
    const WaitHandle shutdown = create_event(ResetMode::Manual);
    const auto installed = install_console_ctrl_handler(shutdown);
    REQUIRE(installed.ok);

    // 정리를 먼저 끝내 둬서 신호 스레드가 3초를 기다리지 않게 한다.
    REQUIRE(signal_cleanup_done());
    REQUIRE(::kill(::getpid(), SIGTERM) == 0);
    REQUIRE(peek(shutdown, 5000) == WaitStatus::Signaled);

    // 원본을 닫아도 신호 스레드는 복제본을 들고 있다.
    close_event(shutdown);
    REQUIRE(::kill(::getpid(), SIGINT) == 0);
    // SIGPIPE 는 무시된다. 기본 동작이면 여기서 프로세스가 끝난다.
    REQUIRE(::kill(::getpid(), SIGPIPE) == 0);
    SUCCEED();
}

TEST_CASE("posix stdin: lines split like std::getline", "[platform][ctrl][posix]") {
    const int writer = pipe_into_stdin();
    const std::string input = "a\n\nb\r\nc";
    REQUIRE(::write(writer, input.data(), input.size()) == static_cast<ssize_t>(input.size()));
    ::close(writer);

    std::string line;
    REQUIRE(read_stdin_line(line));
    REQUIRE(line == "a");
    REQUIRE(read_stdin_line(line));
    REQUIRE(line == "");
    REQUIRE(read_stdin_line(line));
    REQUIRE(line == "b\r");
    REQUIRE(read_stdin_line(line));  // EOF 앞에 줄바꿈 없이 남은 것
    REQUIRE(line == "c");
    REQUIRE_FALSE(read_stdin_line(line));
}

TEST_CASE("posix stdin: a reader blocked on stdin does not hold up exit", "[platform][ctrl][posix]") {
    // 자식은 `[console]` 처럼 읽기에서 블록한 스레드를 두고 exit 한다. 쓰는 쪽은 부모가 열어 둔다.
    const int writer = pipe_into_stdin();
    const pid_t child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        std::thread([] {
            std::string line;
            (void)read_stdin_line(line);
        }).detach();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::exit(0);
    }

    int status = 0;
    pid_t done = 0;
    const auto before = monotonic_ms();
    while ((done = ::waitpid(child, &status, WNOHANG)) == 0 && monotonic_ms() - before < 3000) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (done == 0) {
        ::kill(child, SIGKILL);
        (void)::waitpid(child, &status, 0);
    }
    ::close(writer);
    REQUIRE(done == child);
    REQUIRE(WIFEXITED(status));
    REQUIRE(WEXITSTATUS(status) == 0);
}
