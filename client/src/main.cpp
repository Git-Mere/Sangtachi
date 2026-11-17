#include "sangtachi/args.hpp"
#include "sangtachi/client_process.hpp"
#include "sangtachi/console.hpp"
#include "sangtachi/default_server.hpp"
#include "sangtachi/default_telemetry.hpp"
#include "sangtachi/log.hpp"
#include "sangtachi/loop.hpp"
#include "sangtachi/platform/console_ctrl.hpp"
#include "sangtachi/platform/stdout.hpp"
#include "sangtachi/platform/tun.hpp"

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

// 콘솔 실행 파일의 화면. 표준 출력, 표준 오류, 콘솔 제어 핸들러, `[console]` 이다.
class ConsoleUi final : public sangtachi::ClientUi {
public:
    // 안내 줄과 입력 자리 표시는 사람에게만 보인다 (architecture.md 3.5 로비 안내). 표준 출력이 리다이렉트되면
    // 내지 않는다. 그 출력을 읽는 것은 스크립트이고, 스크립트는 ROOM 과 FAIL 줄만 읽는다. 콘솔이면 ROOM 과 FAIL
    // 줄도 입력 자리 표시를 거친다.
    ConsoleUi()
        : guide_visible_(sangtachi::platform::is_console(sangtachi::platform::stdout_handle())),
          prompt_(std::make_shared<sangtachi::ConsolePrompt>([](std::string_view text, bool newline) {
              const auto out = sangtachi::platform::stdout_handle();
              (void)(newline ? sangtachi::platform::write_line(out, text)
                             : sangtachi::platform::write_text(out, text));
          })) {}

    // 기동 안내 (architecture.md 3.5 로비 안내). 다음에 화면에 무엇이 나오기 전에 지운다.
    void loading() override {
        if (guide_visible_) {
            prompt_->status("Loading...");
        }
    }

    sangtachi::FileLogSink::WriteFn log_screen() override {
        // 기동 실패 줄이 `Loading...` 뒤에 붙지 않게 먼저 지운다. 받는 곳이 이 객체보다 오래 살아 입력 자리 표시를
        // 나눠 든다 (ClientUi::log_screen).
        return [prompt = prompt_](std::string_view line) {
            prompt->clear_status();
            sangtachi::write_stderr(line);
        };
    }

    bool log_screen_all() override {
        return !sangtachi::platform::is_console(sangtachi::platform::stderr_handle());
    }

    // Ctrl+C 와 창 닫기에서도 종료 절차가 돌아야 한다 (concurrency.md 7장 종료).
    // 핸들러는 `[loop]` 소유 상태를 건드리지 않고 이벤트 핸들 둘만 만진다.
    sangtachi::platform::CtrlHandlerInstall install_shutdown_source(
        sangtachi::platform::WaitHandle shutdown_event) override {
        return sangtachi::platform::install_console_ctrl_handler(shutdown_event);
    }

    void adapter_files_missing() override {
        if (!guide_visible_) {
            return;
        }
        prompt_->clear_status();
        std::string guide = "  Keep these files in the same folder as sangtachi_client.exe:";
        for (const auto name : sangtachi::platform::adapter_files()) {
            guide += " ";
            guide += name;
        }
        (void)sangtachi::platform::write_stdout_line(guide);
    }

    void print_line(std::string_view line) override {
        if (guide_visible_) {
            prompt_->line(line);
            return;
        }
        // architecture.md 3.5: 리다이렉트면 UTF-8 과 LF, 콘솔이면 유니코드 쓰기.
        (void)sangtachi::platform::write_stdout_line(line);
    }

    void guide_line(std::string_view line) override {
        if (guide_visible_) {
            prompt_->line(line);
        }
    }

    void command_begin() override {
        if (guide_visible_) {
            prompt_->input();
        }
    }

    void command_end() override {
        if (guide_visible_) {
            prompt_->show();
        }
    }

    // 콘솔에는 화면 큐가 없다. 같은 것을 ROOM, FAIL 줄과 안내 줄이 알린다.
    bool screen(const sangtachi::ui::ScreenState& /*state*/) override { return false; }

    void ready(std::shared_ptr<sangtachi::ConsoleSession> commands) override {
        // 역할 인자로 시작하면 응답이 올 때까지 표준 출력이 비어 있다. 그동안에도 입력 자리를 띄운다.
        if (guide_visible_) {
            prompt_->show();
        }
        // `[console]` 은 join 하지 않는다 (concurrency.md 7장 종료). 표준 입력 읽기를
        // 밖에서 취소하는 수단을 쓰지 않으므로 join 하면 사용자가 한 줄을 더 칠 때까지
        // 종료가 멈춘다.
        //
        // 그래서 그 스레드가 건드리는 것을 스택에 두지 않는다. 세션을 값으로 넘겨 스레드가
        // 스스로 수명을 붙들게 한다. `main` 이 먼저 빠져나가도 해제된 것을 건드리지 않는다.
        std::thread(sangtachi::run_console_reader, std::move(commands)).detach();
    }

    void closing() override { prompt_->close(); }

    void cleanup_done(std::string_view /*log_name*/) override { (void)sangtachi::platform::signal_cleanup_done(); }

    // 기동 실패의 창 (architecture.md 3.5). 탐색기에서 더블클릭해 이 프로세스 혼자 쓰는 창이면, 끝나는 순간 창이
    // 닫혀 ERROR 줄을 읽을 수 없다. 그래서 안내 두 줄을 내고 Enter 를 기다린다. 안내는 다른 안내 줄처럼 표준 출력이
    // 콘솔일 때만 낸다. 셸에서 띄운 실행과 시험은 창을 셸과 나눠 쓰므로 여기서 멈추지 않는다.
    void startup_failed(std::string_view log_name) override {
        if (!guide_visible_ || !sangtachi::platform::console_is_own()) {
            return;
        }
        prompt_->clear_status();
        std::string message = "  Could not start. The ERROR line above says why.";
        if (!log_name.empty()) {
            message += " Full log: logs\\";
            message += log_name;
        }
        (void)sangtachi::platform::write_stdout_line(message);
        (void)sangtachi::platform::write_stdout_line("  Press Enter to close this window.");
        sangtachi::platform::wait_for_enter();
    }

private:
    bool guide_visible_;
    // 입력 자리 표시 (console.hpp ConsolePrompt). `[loop]` 만 쓴다. clear_status 만 로그 화면도 부른다.
    std::shared_ptr<sangtachi::ConsolePrompt> prompt_;
};

}  // namespace

int main(int argc, char** argv) {
    ConsoleUi ui;

    std::vector<std::string_view> raw;
    raw.reserve(argc > 0 ? static_cast<std::size_t>(argc - 1) : 0);
    for (int i = 1; i < argc; ++i) {
        raw.emplace_back(argv[i]);
    }

    // 빌드에 넣은 제어 서버 주소가 --server 의 기본값이다 (architecture.md 3.5, CMake 의
    // SANGTACHI_DEFAULT_SERVER). 비어 있으면 --server 가 필수다. 텔레메트리 서비스 주소도 같다.
    const auto parsed = sangtachi::parse_args(std::span<const std::string_view>(raw),
                                              sangtachi::kDefaultServer, sangtachi::kDefaultTelemetry);
    if (!parsed.ok()) {
        const sangtachi::LogField fields[] = {
            sangtachi::field("reason", sangtachi::to_token(parsed.error)),
            sangtachi::field("arg", parsed.offending),
        };
        sangtachi::emit(sangtachi::LogLevel::Error, "args.invalid", fields);
        ui.startup_failed({});
        return sangtachi::kStartupFailure;
    }

    return sangtachi::run_client_process(*parsed.args, ui);
}
