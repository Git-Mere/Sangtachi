#include "sangtachi/args.hpp"
#include "sangtachi/console.hpp"
#include "sangtachi/counters.hpp"
#include "sangtachi/log.hpp"
#include "sangtachi/loop.hpp"
#include "sangtachi/network/udp_socket.hpp"
#include "sangtachi/network/wsa.hpp"
#include "sangtachi/platform/console_ctrl.hpp"

#include <cstdint>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

namespace {

// 기동 실패의 종료 코드. architecture.md 3.5 기동 입력이 정했다.
constexpr int kStartupFailure = 2;

// architecture.md 9장의 socket.error. op 는 실패한 호출 이름이다. 소켓 밖의 호출도
// 같은 줄로 낸다. 기동 실패를 읽는 쪽이 이벤트 키를 하나만 알면 되게 하려는 것이다.
void emit_op_error(std::string_view op, std::uint64_t code) {
    const sangtachi::LogField fields[] = {
        sangtachi::field("op", op),
        sangtachi::field("code", code),
    };
    sangtachi::emit(sangtachi::LogLevel::Error, "socket.error", fields);
}

void emit_socket_error(std::string_view op, int code) {
    emit_op_error(op, static_cast<std::uint64_t>(code));
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string_view> raw;
    raw.reserve(argc > 0 ? static_cast<std::size_t>(argc - 1) : 0);
    for (int i = 1; i < argc; ++i) {
        raw.emplace_back(argv[i]);
    }

    const auto parsed = sangtachi::parse_args(std::span<const std::string_view>(raw));
    if (!parsed.ok()) {
        const sangtachi::LogField fields[] = {
            sangtachi::field("reason", sangtachi::to_token(parsed.error)),
            sangtachi::field("arg", parsed.offending),
        };
        sangtachi::emit(sangtachi::LogLevel::Error, "args.invalid", fields);
        return kStartupFailure;
    }

    sangtachi::Counters counters;

    try {
        const sangtachi::network::WsaContext wsa;
        {
            const sangtachi::LogField fields[] = {
                sangtachi::field("version", wsa.negotiated_version()),
            };
            sangtachi::emit(sangtachi::LogLevel::Info, "wsa.init", fields);
        }

        auto opened = sangtachi::network::open_udp_socket();
        if (!opened.ok()) {
            emit_socket_error(opened.failed_op, opened.error);
            return kStartupFailure;
        }

        // architecture.md 9장의 socket.bind. spec.md M-1 기동 판정이 이 줄을 읽는다.
        {
            const sangtachi::LogField fields[] = {
                sangtachi::field("local", opened.socket->local().to_string()),
                sangtachi::field("rcvbuf_requested",
                               static_cast<std::uint64_t>(opened.socket->rcvbuf_requested())),
                sangtachi::field("rcvbuf_applied",
                               static_cast<std::uint64_t>(opened.socket->rcvbuf_applied())),
            };
            sangtachi::emit(sangtachi::LogLevel::Info, "socket.bind", fields);
        }

        sangtachi::LoopOptions options;
        options.peer = parsed.args->peer;
#ifdef SANGTACHI_TEST_BUILD
        options.probe_timer = true;
        {
            const sangtachi::LogField fields[] = {
                sangtachi::field("build", std::string_view("test")),
            };
            sangtachi::emit(sangtachi::LogLevel::Warn, "build.test", fields);
        }
#endif

        auto session = sangtachi::ConsoleSession::create();
        if (!session) {
            emit_socket_error("CreateEvent", 0);
            return kStartupFailure;
        }

        sangtachi::EventLoop loop(std::move(*opened.socket), counters, session->queue(),
                                session->event(), options);
        if (!loop.valid()) {
            emit_socket_error("CreateEvent", 0);
            return kStartupFailure;
        }

        // Ctrl+C 와 창 닫기에서도 종료 절차가 돌아야 한다 (concurrency.md 7장 종료).
        // 핸들러는 `[loop]` 소유 상태를 건드리지 않고 이벤트 핸들 둘만 만진다.
        const auto installed =
            sangtachi::platform::install_console_ctrl_handler(loop.shutdown_event());
        if (!installed.ok) {
            // 설치에 실패하면 창 닫기가 정리 없이 프로세스를 끝낸다. 그 상태로 기동하면
            // 종료 계약을 지키지 못하므로 기동 실패로 다룬다.
            emit_op_error(installed.failed_op, installed.error);
            return kStartupFailure;
        }

        // `[console]` 은 join 하지 않는다 (concurrency.md 7장 종료). 표준 입력 읽기를
        // 밖에서 취소하는 수단을 쓰지 않으므로 join 하면 사용자가 한 줄을 더 칠 때까지
        // 종료가 멈춘다.
        //
        // 그래서 그 스레드가 건드리는 것을 스택에 두지 않는다. 세션을 값으로 넘겨 스레드가
        // 스스로 수명을 붙들게 한다. `main` 이 먼저 빠져나가도 해제된 것을 건드리지 않는다.
        std::thread(sangtachi::run_console_reader, session).detach();

        loop.run();
        session->request_stop();

        // concurrency.md 7장 종료의 (4). 카운터 전량을 낸다.
        sangtachi::emit_all(counters);

    } catch (const sangtachi::network::WsaStartupError& e) {
        emit_socket_error("WSAStartup", e.code());
        return kStartupFailure;
    }

    // concurrency.md 7장 종료의 (7). 정리가 끝났음을 콘솔 제어 핸들러에게 알린다. 창
    // 닫기로 들어온 경우 그 핸들러가 이것을 보고 반환하고, 그 순간 OS 가 프로세스를 끝낸다.
    //
    // **try 블록 밖이다.** 그 블록이 닫힐 때 루프와 소켓과 콘솔 세션과 Winsock 이 소멸하고,
    // 그것이 끝난 뒤에 신호해야 한다. 안에서 신호하면 핸들러가 먼저 반환해 OS 가 소멸자를
    // 돌기 전에 프로세스를 끝낼 수 있다. Phase 1 에는 소켓과 Winsock 해제뿐이지만 Phase 6
    // 의 어댑터 정리가 같은 자리에 들어온다. 기동 실패 경로는 이 줄에 닿지 않는다.
    (void)sangtachi::platform::signal_cleanup_done();

    return 0;
}
