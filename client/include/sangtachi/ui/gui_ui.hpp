#pragma once

// GUI 실행 파일의 화면 쪽 (client_process.hpp ClientUi). 작업 스레드와 `[gui]` 가 만나는 자리다
// (concurrency.md 1장 스레드, 6장 화면 큐, 7장 GUI 빌드의 종료).
//
// Qt 를 쓰지 않는다. sangtachi_core 에 들어 시험 실행 파일이 링크한다. 창은 ui/main_window 다.
//
// | 쪽 | 부르는 것 |
// |----|-----------|
// | 작업 스레드 | ClientUi 의 메서드 전부 |
// | `[gui]` | take_latest, send, request_close, end_session |
// | 여러 스레드 | error_sink 와 log_screen 이 돌려준 함수 (로그를 내는 스레드 전부) |

#include "sangtachi/client_process.hpp"
#include "sangtachi/console.hpp"
#include "sangtachi/log.hpp"
#include "sangtachi/platform/wait.hpp"
#include "sangtachi/ui/view.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace sangtachi::ui {

// 대화 상자에 보일 ERROR 줄. 받은 줄 가운데 `ERROR ` 로 시작하는 마지막 것 하나를 든다. 로그 파일을 열기 전에는
// 로그의 받는 곳으로 단다 (log.hpp set_log_sink). 그 뒤로는 FileLogSink 의 화면 몫이 이리로 온다.
class ErrorCapture final : public LogSink {
public:
    void write(std::string_view line) override;
    [[nodiscard]] std::string last() const;

private:
    mutable std::mutex mutex_;
    std::string last_;
};

class GuiUi final : public ClientUi {
public:
    // 두 이벤트는 `[gui]` 가 작업 스레드를 띄우기 전에 만든 수동 리셋 이벤트다. 닫는 것도 그쪽이다. 이 객체보다
    // 오래 산다. clock 은 session_end.cleanup 의 시간을 잰다. 시험이 바꾼다.
    GuiUi(platform::WaitHandle shutdown_event, platform::WaitHandle cleanup_event,
          std::function<platform::WaitMillis()> clock = platform::monotonic_ms);

    // ---- 작업 스레드 (ClientUi)
    [[nodiscard]] FileLogSink::WriteFn log_screen() override;
    [[nodiscard]] bool log_screen_all() override;
    [[nodiscard]] platform::CtrlHandlerInstall install_shutdown_source(platform::WaitHandle shutdown_event) override;
    // 창은 `[gui]` 가 스스로 `Loading...` 을 보인다 (architecture.md 3.5 GUI 표).
    void loading() override {}
    void adapter_files_missing() override {}
    void print_line(std::string_view /*line*/) override {}
    void guide_line(std::string_view /*line*/) override {}
    void command_begin() override {}
    void command_end() override {}
    [[nodiscard]] bool screen(const ScreenState& state) override;
    void ready(std::shared_ptr<ConsoleSession> commands) override;
    void closing() override;
    void cleanup_done(std::string_view log_name) override;
    void startup_failed(std::string_view log_name) override;

    // ---- `[gui]`
    // 화면 큐를 비우고 마지막 항목. 새 항목이 없으면 값이 없다.
    [[nodiscard]] std::optional<ScreenState> take_latest() { return queue_.take_latest(); }
    // 명령 한 줄을 넣는다 (`[console]` 의 자리). 준비가 끝나기 전이거나 큐가 가득 차면 거짓이다.
    bool send(std::string line);
    // 창 닫기. 종료 이벤트를 신호한다. 기다리지 않는다.
    void request_close();
    // 로그오프와 시스템 종료(WM_ENDSESSION). 종료 이벤트를 신호하고 정리 완료를 kCleanupWaitMs 까지 기다린다.
    // 정리가 끝나면 cleanup_done 이 여기서부터 걸린 시간을 session_end.cleanup 줄로 낸다 (architecture.md 9장 로그 출력).
    void end_session();

    // ---- 여러 스레드
    [[nodiscard]] ErrorCapture& error_sink() noexcept { return *errors_; }

private:
    // 루프가 끝난 뒤의 항목을 넣는다.
    void publish_end(Phase phase, std::string_view log_name);

    platform::WaitHandle shutdown_event_;
    platform::WaitHandle cleanup_event_;
    std::function<platform::WaitMillis()> clock_;
    // end_session 이 불린 시각. session_ending_ 이 참일 때만 뜻이 있다.
    std::atomic<platform::WaitMillis> session_end_at_{0};
    std::atomic<bool> session_ending_{false};
    // log_screen 이 돌려준 함수가 이 객체보다 오래 살 수 있어 따로 든다 (ClientUi::log_screen).
    std::shared_ptr<ErrorCapture> errors_ = std::make_shared<ErrorCapture>();
    ScreenQueue queue_;
    // 사람이 닫았다. 정리가 끝났을 때 kClosed 와 kEndedByItself 를 가른다.
    std::atomic<bool> user_closed_{false};
    mutable std::mutex commands_mutex_;
    std::shared_ptr<ConsoleSession> commands_;
};

}  // namespace sangtachi::ui
