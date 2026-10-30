#include "sangtachi/ui/gui_ui.hpp"

#include "sangtachi/console.hpp"
#include "sangtachi/platform/wait.hpp"
#include "sangtachi/ui/view.hpp"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <string>
#include <string_view>

// 시험 케이스 이름은 ASCII 로만 적는다. 이유는 network/wsa_test.cpp 머리에 있다.
//
// 규칙의 출처: concurrency.md 7장 GUI 빌드의 종료(창 닫기, 스스로 끝날 때, WM_ENDSESSION), 6장 화면 큐,
// architecture.md 3.5 GUI 실행 파일(기동 실패의 대화 상자).

using namespace sangtachi::ui;
namespace platform = sangtachi::platform;

namespace {

struct Events {
    platform::WaitHandle shutdown = platform::create_event(platform::ResetMode::Manual);
    platform::WaitHandle cleanup = platform::create_event(platform::ResetMode::Manual);

    Events() = default;
    ~Events() {
        platform::close_event(shutdown);
        platform::close_event(cleanup);
    }
    Events(const Events&) = delete;
    Events& operator=(const Events&) = delete;
};

bool signaled(platform::WaitHandle handle) {
    const platform::WaitHandle handles[] = {handle};
    return platform::wait_any(handles, 0).status == platform::WaitStatus::Signaled;
}

ScreenState ready_in_room() {
    ScreenState s;
    s.phase = Phase::kReady;
    s.in_lobby = false;
    s.room_id.assign("ABCDEF");
    return s;
}

}  // namespace

TEST_CASE("gui ui: the end state follows who closed", "[ui][gui_ui]") {
    // 사람이 닫았으면 kClosed, 아니면 kEndedByItself 이고 그때만 ERROR 줄과 로그 파일 이름을 싣는다.
    struct Case {
        const char* what;
        bool user_closed;
        Phase end;
        std::string_view error_line;
        std::string_view log_name;
    };
    const Case cases[] = {
        {"window closed", true, Phase::kClosed, "", ""},
        {"loop ended by itself", false, Phase::kEndedByItself, "ERROR adapter.error reason=eof", "x.log"},
    };
    for (const auto& c : cases) {
        INFO(c.what);
        Events ev;
        REQUIRE(ev.shutdown != nullptr);
        REQUIRE(ev.cleanup != nullptr);
        GuiUi ui(ev.shutdown, ev.cleanup);
        const auto screen = ui.log_screen();
        CHECK(ui.log_screen_all());
        screen("ERROR adapter.error reason=eof\n");
        screen("WARN console.rejected reason=bad_name\n");

        CHECK_FALSE(ui.screen(ready_in_room()));
        if (c.user_closed) {
            ui.request_close();
        }
        CHECK(signaled(ev.shutdown) == c.user_closed);
        ui.closing();
        CHECK_FALSE(signaled(ev.cleanup));
        ui.cleanup_done("x.log");
        CHECK(signaled(ev.cleanup));

        const auto got = ui.take_latest();
        REQUIRE(got);
        CHECK(got->phase == c.end);
        CHECK(got->room_id.view() == "ABCDEF");  // 마지막 루프 상태 위에 단계만 바꾼다
        CHECK(got->error_line.view() == c.error_line);
        CHECK(got->log_name.view() == c.log_name);
    }
}

TEST_CASE("gui ui: startup failure carries the last ERROR line and signals cleanup", "[ui][gui_ui]") {
    Events ev;
    GuiUi ui(ev.shutdown, ev.cleanup);
    // 로그 파일을 열기 전에는 로그의 받는 곳으로 받는다. 줄바꿈 없는 줄이다.
    ui.error_sink().write("ERROR record.error op=CreateFileW code=5");
    ui.error_sink().write("INFO wsa.init version=2.2");
    ui.log_screen()("ERROR adapter.error reason=not_elevated\n");
    ui.startup_failed("20261029T000000Z_1.log");

    CHECK(signaled(ev.cleanup));
    CHECK_FALSE(signaled(ev.shutdown));
    const auto got = ui.take_latest();
    REQUIRE(got);
    CHECK(got->phase == Phase::kStartupFailed);
    CHECK(got->error_line.view() == "ERROR adapter.error reason=not_elevated");
    CHECK(got->log_name.view() == "20261029T000000Z_1.log");
}

TEST_CASE("gui ui: commands go to the session queue only after ready", "[ui][gui_ui]") {
    Events ev;
    GuiUi ui(ev.shutdown, ev.cleanup);
    CHECK_FALSE(ui.send("host"));

    const auto session = sangtachi::ConsoleSession::create();
    REQUIRE(session);
    ui.ready(session);
    CHECK(ui.send("name Bob"));
    CHECK(ui.send("host"));
    CHECK(signaled(session->event()));
    CHECK(session->queue().try_pop() == std::optional<std::string>("name Bob"));
    CHECK(session->queue().try_pop() == std::optional<std::string>("host"));
    CHECK_FALSE(session->queue().try_pop());
}

TEST_CASE("gui ui: end session signals shutdown, waits for cleanup and counts as closed", "[ui][gui_ui]") {
    Events ev;
    GuiUi ui(ev.shutdown, ev.cleanup);
    // 정리가 이미 끝난 뒤라 기다리지 않고 돌아온다. 기다리는 경로 자체는 console_ctrl_test 가 run_ctrl_handler 로 본다.
    REQUIRE(platform::signal_event(ev.cleanup));
    ui.end_session();
    CHECK(signaled(ev.shutdown));
    ui.cleanup_done("x.log");
    const auto got = ui.take_latest();
    REQUIRE(got);
    CHECK(got->phase == Phase::kClosed);
}

TEST_CASE("gui ui: install_shutdown_source installs nothing and succeeds", "[ui][gui_ui]") {
    Events ev;
    GuiUi ui(ev.shutdown, ev.cleanup);
    CHECK(ui.install_shutdown_source(ev.shutdown).ok);
    CHECK_FALSE(signaled(ev.shutdown));
}
