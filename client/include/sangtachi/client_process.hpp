#pragma once

// 클라이언트 프로세스 하나의 기동, 루프, 종료 (architecture.md 3.5 기동 입력, concurrency.md 7장 종료).
//
// 콘솔 실행 파일의 main 이 인자를 읽은 뒤 이것을 부른다. GUI 실행 파일은 작업 스레드에서 부른다 (concurrency.md 1장). 화면에 닿는 일은 모두 ClientUi 를 거친다. 부르는 자리와
// 순서는 화면과 무관하게 이 본체가 정한다.
//
// **이 원본은 sangtachi_core 에 넣지 않는다.** 시험 빌드 장치를 붙이는 자리가 있다 (testbuild/instrument.hpp,
// ADR 0023). 실행 파일 대상이 직접 컴파일한다.

#include "sangtachi/args.hpp"
#include "sangtachi/console.hpp"
#include "sangtachi/log.hpp"
#include "sangtachi/platform/console_ctrl.hpp"
#include "sangtachi/platform/wait.hpp"
#include "sangtachi/ui/view.hpp"

#include <memory>
#include <string_view>

namespace sangtachi {

// 기동 실패의 종료 코드. architecture.md 3.5 기동 입력이 정했다.
inline constexpr int kStartupFailure = 2;

// 화면 쪽. 기동 실패 뒤의 startup_failed 를 빼면 모두 run_client_process 를 부른 스레드에서 불린다.
class ClientUi {
public:
    ClientUi() = default;
    virtual ~ClientUi() = default;
    ClientUi(const ClientUi&) = delete;
    ClientUi& operator=(const ClientUi&) = delete;
    ClientUi(ClientUi&&) = delete;
    ClientUi& operator=(ClientUi&&) = delete;

    // 로그 줄의 화면 몫 (log.hpp FileLogSink 의 화면 함수와 screen_all). 받는 곳은 프로세스 수명이고 여러 스레드가
    // 부르므로 돌려주는 함수는 이 객체를 붙들지 않는다.
    [[nodiscard]] virtual FileLogSink::WriteFn log_screen() = 0;
    [[nodiscard]] virtual bool log_screen_all() = 0;

    // 로그 파일을 열어 받는 곳을 단 뒤에 부른다. 기동 안내를 띄울 자리다 (architecture.md 3.5 로비 안내). 그 앞의 기동
    // 실패 줄은 받는 곳을 거치지 않으므로 그 뒤에 띄운다.
    virtual void loading() = 0;

    // 종료 이벤트를 신호할 쪽을 단다. 실패하면 기동 실패다 (concurrency.md 7장 종료).
    [[nodiscard]] virtual platform::CtrlHandlerInstall install_shutdown_source(
        platform::WaitHandle shutdown_event) = 0;

    // 실행 파일 옆의 파일 셋 가운데 없는 것이 있다 (architecture.md 7장 어댑터 수명). 그 ERROR 줄 뒤에 부른다.
    virtual void adapter_files_missing() = 0;

    // ROOM 과 FAIL 줄, 안내 줄 (control/lobby.hpp 의 PrintLine 과 GuideLine).
    virtual void print_line(std::string_view line) = 0;
    virtual void guide_line(std::string_view line) = 0;

    // 명령 한 줄을 처리하기 전과 뒤.
    virtual void command_begin() = 0;
    virtual void command_end() = 0;

    // 루프 한 바퀴의 끝에서 창에 보일 상태 (concurrency.md 6장 화면 큐). 바퀴마다 부른다. 바뀌었는지는 받는 쪽이
    // 본다. 넣지 못해 들고 있는 것이 남으면 참이고, 그러면 루프가 kGuiPollMs 안에 다시 돌아 또 부른다.
    [[nodiscard]] virtual bool screen(const ui::ScreenState& state) = 0;

    // 기동이 끝나 루프가 곧 돈다. commands 의 큐와 이벤트가 명령이 들어올 곳이다.
    virtual void ready(std::shared_ptr<ConsoleSession> commands) = 0;
    // 루프가 끝났다. 종료 절차가 이어진다.
    virtual void closing() = 0;
    // concurrency.md 7장 종료의 (7). log_name 은 로그 파일 이름이다. GUI 가 스스로 끝난 때의 대화 상자에 쓴다.
    virtual void cleanup_done(std::string_view log_name) = 0;
    // 기동 실패로 끝났다. 정리는 이미 끝났다. log_name 은 로그 파일 이름이고 열기 전이면 비어 있다.
    virtual void startup_failed(std::string_view log_name) = 0;
};

// 인자 검사를 지난 args 로 프로세스를 돌린다. 0 이면 종료 절차로 끝났다. 그 밖의 값은 기동 실패의 종료 코드이고 그때
// ui.startup_failed 를 부른 뒤 돌아온다. `[control]` join 이 상한을 넘기면 돌아오지 않는다 (concurrency.md 7장 종료).
//
// shutdown_event 는 부르는 쪽이 미리 만든 종료 이벤트(수동 리셋)다. GUI 빌드는 작업 스레드를 띄우기 전에 만들어
// 넘긴다. 그래야 준비 중에 창을 닫아도 종료가 걸린다 (concurrency.md 7장 GUI 빌드의 종료). 이 함수는 복제해서 쓰고
// 원본을 닫지 않는다. 널이면 이 함수가 만든다(콘솔 빌드).
int run_client_process(Args args, ClientUi& ui, platform::WaitHandle shutdown_event = nullptr);

}  // namespace sangtachi
