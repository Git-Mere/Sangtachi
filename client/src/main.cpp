#include "sangtachi/args.hpp"
#include "sangtachi/console.hpp"
#include "sangtachi/counters.hpp"
#include "sangtachi/log.hpp"
#include "sangtachi/loop.hpp"
#include "sangtachi/network/stun_client.hpp"
#include "sangtachi/network/udp_socket.hpp"
#include "sangtachi/network/wsa.hpp"
#include "sangtachi/platform/console_ctrl.hpp"
#include "sangtachi/platform/resolve.hpp"
#include "sangtachi/platform/wait.hpp"
#include "sangtachi/timer.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

// 기동 실패의 종료 코드. architecture.md 3.5 기동 입력이 정했다.
constexpr int kStartupFailure = 2;

sangtachi::Millis now_ms() noexcept {
    return static_cast<sangtachi::Millis>(sangtachi::platform::monotonic_ms());
}

// architecture.md 3.5 기동 입력. `--stun` 을 주지 않았으면 기본 목록을 쓴다.
//
// 돌려주는 것은 인자 문자열을 가리키는 뷰다. 부르는 쪽의 args 가 살아 있는 동안만 쓴다.
std::vector<sangtachi::network::StunServerName> stun_list(const sangtachi::Args& args) {
    std::vector<sangtachi::network::StunServerName> list;
    if (args.stun.empty()) {
        list.assign(sangtachi::network::kDefaultStunServers.begin(),
                    sangtachi::network::kDefaultStunServers.end());
        return list;
    }
    list.reserve(args.stun.size());
    for (const auto& entry : args.stun) {
        list.push_back(sangtachi::network::StunServerName{entry.host, entry.port});
    }
    return list;
}

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

    // **`--stun` 목록이 한 개면 기동 시 WARN 한 줄을 남긴다** (architecture.md 3.5 기동
    // 입력). 그 목록으로는 서로 다른 두 서버의 응답을 얻을 수 없어 5초를 기다린 뒤
    // STUN_DISCOVERY_FAILED 로 끝난다. 기다리기 전에 알린다.
    if (parsed.args->stun.size() == 1) {
        const sangtachi::LogField fields[] = {
            sangtachi::field("reason", std::string_view("single_server")),
        };
        sangtachi::emit(sangtachi::LogLevel::Warn, "stun.config", fields);
    }

    try {
        const sangtachi::network::WsaContext wsa;
        {
            const sangtachi::LogField fields[] = {
                sangtachi::field("version", wsa.negotiated_version()),
            };
            sangtachi::emit(sangtachi::LogLevel::Info, "wsa.init", fields);
        }

        // **이름 해석은 `[loop]` 시작 전에 목록 전체를 한 번 한다** (architecture.md 3.5
        // 기동 입력). getaddrinfo 는 동기 호출이라 `[loop]` 안에 두면 응답이 올 때까지 한
        // 바퀴가 멈춘다. Phase 1~2 에는 그 일을 맡길 `[control]` 스레드가 없다
        // (concurrency.md 1장). 해석 실패와 중복 엔드포인트를 거르는 규칙은 그 절에 있고
        // 판정은 network/stun_client.hpp 의 resolve_stun_servers 가 한다.
        const auto stun_names = stun_list(*parsed.args);
        auto stun_servers = sangtachi::network::resolve_stun_servers(
            stun_names, [](std::string_view host, std::uint16_t port) {
                return sangtachi::platform::resolve_ipv4(host, port);
            });

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

        // STUN 은 루프의 소켓으로 보내고 루프에서 응답을 전달받는다 (protocol.md 6장
        // 소켓 소유권). 타이머 집합도 루프의 것을 빌린다 (concurrency.md 3장, 4장).
        sangtachi::network::StunClient stun(
            std::move(stun_servers),
            [&loop](const sangtachi::network::Endpoint& to, std::span<const std::byte> payload) {
                return loop.send_datagram(to, payload);
            },
            loop.timers(), counters);

        loop.set_stun_handler([&stun](const sangtachi::network::Endpoint& from,
                                      std::span<const std::byte> payload) {
            stun.on_datagram(from, payload, now_ms());
        });
        loop.set_tick_handler([&stun](const sangtachi::TimerTick& tick) {
            if (sangtachi::network::StunClient::owns_timer(tick.name)) {
                stun.on_timer(tick.name, now_ms());
            }
        });

        // 목록이 두 개 미만이면 start 가 그 자리에서 STUN_DISCOVERY_FAILED 를 낸다
        // (architecture.md 3.5 기동 입력). 어느 쪽이든 기동 실패가 아니다. 루프는 계속
        // 돌고 종료는 concurrency.md 7장 종료가 맡는다.
        (void)stun.start(now_ms());

        // `[console]` 은 join 하지 않는다 (concurrency.md 7장 종료). 표준 입력 읽기를
        // 밖에서 취소하는 수단을 쓰지 않으므로 join 하면 사용자가 한 줄을 더 칠 때까지
        // 종료가 멈춘다.
        //
        // 그래서 그 스레드가 건드리는 것을 스택에 두지 않는다. 세션을 값으로 넘겨 스레드가
        // 스스로 수명을 붙들게 한다. `main` 이 먼저 빠져나가도 해제된 것을 건드리지 않는다.
        std::thread(sangtachi::run_console_reader, session).detach();

        loop.run();
        session->request_stop();

        // 루프가 든 핸들러를 먼저 끊는다. `stun` 은 `loop` 뒤에 선언되어 **먼저** 소멸하는데,
        // 그 핸들러들은 `stun` 을 참조로 붙든다. 지금은 이 뒤에 루프를 도는 코드가 없어
        // 실제로 불리지 않지만, 수명이 길어지는 Phase 3 에서 그 순서가 그대로 함정이 된다.
        // 끊는 비용이 없으므로 여기서 끊는다.
        loop.set_stun_handler(nullptr);
        loop.set_tick_handler(nullptr);

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
