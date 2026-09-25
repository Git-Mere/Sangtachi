#include "sangtachi/args.hpp"
#include "sangtachi/console.hpp"
#include "sangtachi/counters.hpp"
#include "sangtachi/log.hpp"
#include "sangtachi/loop.hpp"
#include "sangtachi/network/udp_socket.hpp"
#include "sangtachi/network/wsa.hpp"

#include <cstdint>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

namespace {

// 기동 실패의 종료 코드. architecture.md 3.5 기동 입력이 정했다.
constexpr int kStartupFailure = 2;

void emit_socket_error(std::string_view op, int code) {
    const sangtachi::LogField fields[] = {
        sangtachi::field("op", op),
        sangtachi::field("code", static_cast<std::uint64_t>(code)),
    };
    sangtachi::emit(sangtachi::LogLevel::Error, "socket.error", fields);
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

    return 0;
}
