#include "sangtachi/ui/gui_ui.hpp"

#include "sangtachi/platform/console_ctrl.hpp"

#include <utility>

namespace sangtachi::ui {

void ErrorCapture::write(std::string_view line) {
    if (!line.starts_with("ERROR ")) {
        return;
    }
    const std::lock_guard lock(mutex_);
    last_.assign(line);
}

std::string ErrorCapture::last() const {
    const std::lock_guard lock(mutex_);
    return last_;
}

GuiUi::GuiUi(platform::WaitHandle shutdown_event, platform::WaitHandle cleanup_event,
             std::function<platform::WaitMillis()> clock)
    : shutdown_event_(shutdown_event), cleanup_event_(cleanup_event), clock_(std::move(clock)) {}

FileLogSink::WriteFn GuiUi::log_screen() {
    return [errors = errors_](std::string_view line_with_newline) {
        if (line_with_newline.ends_with('\n')) {
            line_with_newline.remove_suffix(1);
        }
        errors->write(line_with_newline);
    };
}

// 전부 받아 ERROR 줄만 남긴다. 기동 중의 것만 받으면 루프가 스스로 끝날 때의 ERROR 줄을 놓친다.
bool GuiUi::log_screen_all() {
    return true;
}

// 콘솔 제어 핸들러를 달지 않는다. 종료 이벤트는 창 닫기와 WM_ENDSESSION 이 신호한다 (concurrency.md 7장 GUI
// 빌드의 종료).
platform::CtrlHandlerInstall GuiUi::install_shutdown_source(platform::WaitHandle /*shutdown_event*/) {
    return platform::CtrlHandlerInstall{true, 0, {}};
}

bool GuiUi::screen(const ScreenState& state) {
    return queue_.publish(state);
}

void GuiUi::ready(std::shared_ptr<ConsoleSession> commands) {
    const std::lock_guard lock(commands_mutex_);
    commands_ = std::move(commands);
}

void GuiUi::closing() {
    publish_end(Phase::kClosing, {});
}

void GuiUi::cleanup_done(std::string_view log_name) {
    if (session_ending_.load()) {
        const LogField fields[] = {field("elapsed_ms", clock_() - session_end_at_.load())};
        emit(LogLevel::Info, "session_end.cleanup", fields);
    }
    publish_end(user_closed_.load() ? Phase::kClosed : Phase::kEndedByItself, log_name);
    (void)platform::signal_event(cleanup_event_);
}

// 기동 실패 경로는 cleanup_done 을 부르지 않는다. 정리는 이미 끝났으므로 여기서 정리 완료를 알린다. 그래야
// WM_ENDSESSION 이 3초를 다 기다리지 않는다.
void GuiUi::startup_failed(std::string_view log_name) {
    publish_end(Phase::kStartupFailed, log_name);
    (void)platform::signal_event(cleanup_event_);
}

bool GuiUi::send(std::string line) {
    std::shared_ptr<ConsoleSession> commands;
    {
        const std::lock_guard lock(commands_mutex_);
        commands = commands_;
    }
    if (!commands || !commands->queue().try_push(std::move(line))) {
        return false;
    }
    return platform::signal_event(commands->event());
}

void GuiUi::request_close() {
    user_closed_.store(true);
    (void)platform::signal_event(shutdown_event_);
}

void GuiUi::end_session() {
    session_end_at_.store(clock_());
    session_ending_.store(true);
    user_closed_.store(true);
    (void)platform::run_ctrl_handler(platform::CtrlSignal::Shutdown, shutdown_event_, cleanup_event_,
                                     platform::kCleanupWaitMs);
}

void GuiUi::publish_end(Phase phase, std::string_view log_name) {
    ScreenState state = queue_.last();
    state.phase = phase;
    if (phase == Phase::kStartupFailed || phase == Phase::kEndedByItself) {
        state.error_line.assign(errors_->last());
        state.log_name.assign(log_name);
    }
    (void)queue_.publish_end(state);
}

}  // namespace sangtachi::ui
