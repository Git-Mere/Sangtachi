// GUI 실행 파일 sangtachi_gui.exe (architecture.md 3.5 GUI 실행 파일, ADR 0034).
//
// 주 스레드가 `[gui]` 다. 작업 스레드가 run_client_process 로 기동, 루프, 종료를 돈다 (concurrency.md 1장 스레드).

#include "sangtachi/args.hpp"
#include "sangtachi/client_process.hpp"
#include "sangtachi/default_server.hpp"
#include "sangtachi/default_telemetry.hpp"
#include "sangtachi/log.hpp"
#include "sangtachi/platform/wait.hpp"
#include "sangtachi/ui/gui_ui.hpp"
#include "sangtachi/ui/main_window.hpp"
#include "sangtachi/ui/view.hpp"

#include <QApplication>
#include <QMessageBox>

#include <span>
#include <string_view>
#include <thread>
#include <utility>

namespace {

// 작업 스레드를 띄우기 전의 기동 실패. 대화 상자를 보이고 끝난다.
int fail_before_worker(const sangtachi::ui::ErrorCapture& errors) {
    sangtachi::ui::ScreenState state;
    state.phase = sangtachi::ui::Phase::kStartupFailed;
    state.error_line.assign(errors.last());
    QMessageBox::critical(nullptr, "Sangtachi", QString::fromStdString(*sangtachi::ui::dialog_text(state)));
    return sangtachi::kStartupFailure;
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    // 작업 스레드보다 먼저 만든다. 그래야 준비 중에 창을 닫아도 종료가 걸린다 (concurrency.md 7장 GUI 빌드의 종료).
    const auto shutdown = sangtachi::platform::create_event(sangtachi::platform::ResetMode::Manual);
    const auto cleanup = sangtachi::platform::create_event(sangtachi::platform::ResetMode::Manual);
    sangtachi::ui::GuiUi ui(shutdown, cleanup);
    // 표준 오류가 없다. 로그 파일을 열기 전의 ERROR 줄도 대화 상자에 보이게 받아 둔다.
    (void)sangtachi::set_log_sink(&ui.error_sink());

    int code = sangtachi::kStartupFailure;
    if (shutdown == nullptr || cleanup == nullptr) {
        const sangtachi::LogField fields[] = {sangtachi::field("op", std::string_view("CreateEvent"))};
        sangtachi::emit(sangtachi::LogLevel::Error, "socket.error", fields);
        code = fail_before_worker(ui.error_sink());
    } else {
        // 명령줄 인자를 받지 않는다. 콘솔에서 인자 없이 띄운 것과 같다 (architecture.md 3.5 GUI 실행 파일).
        auto parsed = sangtachi::parse_args(std::span<const std::string_view>(), sangtachi::kDefaultServer,
                                            sangtachi::kDefaultTelemetry);
        if (!parsed.ok()) {
            const sangtachi::LogField fields[] = {
                sangtachi::field("reason", sangtachi::to_token(parsed.error)),
                sangtachi::field("arg", parsed.offending),
            };
            sangtachi::emit(sangtachi::LogLevel::Error, "args.invalid", fields);
            code = fail_before_worker(ui.error_sink());
        } else {
            sangtachi::ui::MainWindow window(ui);
            window.show();
            std::thread worker([&ui, &code, shutdown, args = std::move(*parsed.args)]() mutable {
                code = sangtachi::run_client_process(std::move(args), ui, shutdown);
            });
            (void)QApplication::exec();
            worker.join();
        }
    }

    sangtachi::platform::close_event(shutdown);
    sangtachi::platform::close_event(cleanup);
    return code;
}
