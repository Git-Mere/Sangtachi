#include "sangtachi/args.hpp"
#include "sangtachi/console.hpp"
#include "sangtachi/control/channel.hpp"
#include "sangtachi/control/http.hpp"
#include "sangtachi/control/lobby.hpp"
#include "sangtachi/control/ops.hpp"
#include "sangtachi/control/runner.hpp"
#include "sangtachi/counters.hpp"
#include "sangtachi/log.hpp"
#include "sangtachi/loop.hpp"
#include "sangtachi/network/stun_client.hpp"
#include "sangtachi/network/udp_socket.hpp"
#include "sangtachi/network/wsa.hpp"
#include "sangtachi/platform/console_ctrl.hpp"
#include "sangtachi/platform/resolve.hpp"
#include "sangtachi/platform/stdout.hpp"
#include "sangtachi/platform/wait.hpp"
#include "sangtachi/timer.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
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

// join 상한. `[telemetry]` 와 `[control]` 을 합쳐 2초다 (concurrency.md 7장 종료).
// `[telemetry]` 는 Phase 9 이므로 지금은 `[control]` 하나가 이 상한을 다 쓴다.
constexpr std::uint32_t kJoinCapMs = 2000;

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

// 이벤트 하나를 들고 스코프 끝에서 닫는다.
class OwnedEvent {
public:
    explicit OwnedEvent(sangtachi::platform::WaitHandle handle) noexcept : handle_(handle) {}
    ~OwnedEvent() { sangtachi::platform::close_event(handle_); }
    OwnedEvent(const OwnedEvent&) = delete;
    OwnedEvent& operator=(const OwnedEvent&) = delete;
    OwnedEvent(OwnedEvent&&) = delete;
    OwnedEvent& operator=(OwnedEvent&&) = delete;

    [[nodiscard]] sangtachi::platform::WaitHandle get() const noexcept { return handle_; }

private:
    sangtachi::platform::WaitHandle handle_;
};

// 기동의 나머지와 루프. 제어 서버 주소를 해석한 뒤에 부른다 (control_plane.md 8.4 의 3번부터).
//
// 0 이면 루프가 종료로 끝났다. 그 밖의 값은 기동 실패의 종료 코드다.
int run_client(const sangtachi::Args& args, sangtachi::Counters& counters,
               sangtachi::platform::WaitHandle shutdown_event,
               sangtachi::control::ControlChannel& control) {
    // **이름 해석은 `[loop]` 시작 전에 목록 전체를 한 번 한다** (architecture.md 3.5
    // 기동 입력). getaddrinfo 는 동기 호출이라 `[loop]` 안에 두면 응답이 올 때까지 한
    // 바퀴가 멈춘다. 해석 실패와 중복 엔드포인트를 거르는 규칙은 그 절에 있고
    // 판정은 network/stun_client.hpp 의 resolve_stun_servers 가 한다.
    const auto stun_names = stun_list(args);
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
    options.peer = args.peer;
    options.shutdown_event = shutdown_event;
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

    // 제어 응답 이벤트가 대기 집합의 순위 4 로 들어온다 (concurrency.md 2장 대기).
    loop.attach_control(&control);
    if (!loop.valid()) {
        emit_socket_error("CreateEvent", 0);
        return kStartupFailure;
    }

    const auto& server = *args.server;

    // 3.3 의 Host 헤더 값. `--server` 의 이름 또는 IPv4 와 포트다 (control_plane.md 3.3).
    // 인자 검사를 지난 값이라 거부되지 않는다. 거부되면 요청을 하나도 만들 수 없으므로
    // 기동 실패로 다룬다.
    auto host_header = sangtachi::control::http::host_header(server.host, server.port);
    if (!host_header) {
        const sangtachi::LogField fields[] = {
            sangtachi::field("reason", std::string_view("bad_host_header")),
        };
        sangtachi::emit(sangtachi::LogLevel::Error, "control.server", fields);
        return kStartupFailure;
    }

    // 로비와 그 행동 실행기 (control/runner.hpp). STUN 은 시도마다 새로 돈다
    // (control_plane.md 8.4: 2번 DNS 와 3번 bind 만 프로세스에 한 번이다). 그래서 기동 시
    // 여기서 STUN 을 돌리지 않는다. 해석한 서버 목록만 넘긴다.
    sangtachi::control::RunnerDeps deps;
    deps.host_header = std::move(*host_header);
    deps.stun_servers = std::move(stun_servers);
    deps.nonce = [] { return sangtachi::control::generate_client_nonce(); };
    deps.submit = [&control](sangtachi::control::ControlRequest request) {
        return control.submit(std::move(request));
    };
    deps.send = [&loop](const sangtachi::network::Endpoint& to,
                        std::span<const std::byte> payload) {
        // STUN 은 루프의 소켓으로 보낸다 (protocol.md 6장 소켓 소유권).
        return loop.send_datagram(to, payload);
    };
    deps.print = [](std::string_view line) {
        // architecture.md 3.5: 리다이렉트면 UTF-8 과 LF, 콘솔이면 유니코드 쓰기.
        (void)sangtachi::platform::write_stdout_line(line);
    };
    deps.clock = [] { return now_ms(); };
    sangtachi::control::ControlRunner runner(std::move(deps), loop.timers(), counters);

    loop.set_lobby_command_handler([&runner](const sangtachi::control::LobbyCommand& command) {
        runner.on_command(command);
    });
    loop.set_control_handler([&runner](sangtachi::control::ControlResponse response) {
        runner.on_response(response);
    });
    loop.set_stun_handler([&runner](const sangtachi::network::Endpoint& from,
                                    std::span<const std::byte> payload) {
        runner.on_stun_datagram(from, payload);
    });
    loop.set_tick_handler([&runner](const sangtachi::TimerTick& tick) {
        runner.on_timer(tick.name);
    });

    // CLI 역할은 기동 직후의 로비 명령이다 (architecture.md 3.5). 역할이 없으면 아무
    // 요청도 STUN 도 나가지 않고 로비에서 기다린다.
    if (const auto command =
            sangtachi::control::startup_command(args.role, args.room)) {
        runner.on_command(*command);
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

    // 루프가 든 핸들러를 먼저 끊는다. `runner` 는 `loop` 뒤에 선언되어 **먼저** 소멸하는데,
    // 그 핸들러들은 `runner` 를 참조로 붙든다.
    loop.set_lobby_command_handler(nullptr);
    loop.set_stun_handler(nullptr);
    loop.set_tick_handler(nullptr);
    loop.set_control_handler(nullptr);
    return 0;
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

    // `[control]` 의 채널. **try 블록 밖에 둔다.** concurrency.md 7장 종료가 join(8) 을 정리
    // 완료(7) 뒤에 두고, 정리 완료는 try 블록이 닫힌 뒤에 신호하기 때문이다(아래). 블록 안에
    // 두면 블록이 닫힐 때 소멸자가 join 해 순서가 뒤집힌다. 채널은 Winsock 참조를 따로 들므로
    // 블록 안의 WsaContext 가 먼저 사라져도 `[control]` 의 소켓은 살아 있다
    // (control/channel.hpp).
    std::unique_ptr<sangtachi::control::ControlChannel> control;

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

        // 종료 이벤트. 수동 리셋이다. **루프보다 먼저 만든다.** 제어 서버 주소의 해석이 UDP
        // bind 보다 앞이고(control_plane.md 8.4 의 2번과 3번), 그 사이의 Ctrl+C 도 종료 절차를
        // 타야 한다. 콘솔 제어 핸들러, `[control]`, 루프가 각자 복제해서 든다. 이 핸들은 try
        // 블록이 닫힐 때 닫는다.
        const OwnedEvent shutdown_event(sangtachi::platform::create_event(
            sangtachi::platform::ResetMode::Manual));
        if (shutdown_event.get() == nullptr) {
            emit_socket_error("CreateEvent", 0);
            return kStartupFailure;
        }

        // Ctrl+C 와 창 닫기에서도 종료 절차가 돌아야 한다 (concurrency.md 7장 종료).
        // 핸들러는 `[loop]` 소유 상태를 건드리지 않고 이벤트 핸들 둘만 만진다.
        const auto installed =
            sangtachi::platform::install_console_ctrl_handler(shutdown_event.get());
        if (!installed.ok) {
            // 설치에 실패하면 창 닫기가 정리 없이 프로세스를 끝낸다. 그 상태로 기동하면
            // 종료 계약을 지키지 못하므로 기동 실패로 다룬다.
            emit_op_error(installed.failed_op, installed.error);
            return kStartupFailure;
        }

        // `[control]` 을 띄우고 제어 서버 주소를 한 번 해석한다 (control_plane.md 8.2,
        // 8.4 의 2번). 해석은 그 스레드가 하고 여기서는 결과를 기다린다. 해석이 UDP bind(3번)
        // 보다 먼저다. 그래서 해석에 실패하면 socket.bind 줄이 나오지 않는다.
        //
        // **해석 실패는 기동 실패다. 재시도하지 않는다.** FR-13 의 실패 코드가 아니다.
        // ERROR 한 줄과 0 이 아닌 종료 코드로 끝난다 (control_plane.md 8.2).
        const auto& server = *parsed.args->server;
        control = sangtachi::control::ControlChannel::start(server.host, server.port,
                                                            shutdown_event.get(), counters);
        if (!control) {
            emit_op_error("ControlChannel.start", 0);
            return kStartupFailure;
        }
        const auto resolved = control->wait_resolved();
        if (!resolved) {
            const sangtachi::LogField fields[] = {
                sangtachi::field("reason", std::string_view("resolve_failed")),
                sangtachi::field("host", server.host),
                sangtachi::field("port", static_cast<std::uint64_t>(server.port)),
            };
            sangtachi::emit(sangtachi::LogLevel::Error, "control.server", fields);
            return kStartupFailure;
        }
        {
            const sangtachi::LogField fields[] = {
                sangtachi::field("host", server.host),
                sangtachi::field("resolved", resolved->to_string()),
            };
            sangtachi::emit(sangtachi::LogLevel::Info, "control.server", fields);
        }

        // DNS 를 기다리는 동안 종료가 신호됐으면 소켓도 루프도 만들지 않는다. 역할 인자의
        // 로비 명령도 내지 않는다. 아래의 정상 종료 경로(카운터 전량, 정리 완료, join)로 간다.
        const sangtachi::platform::WaitHandle shutdown_handles[] = {shutdown_event.get()};
        const bool stopped_during_startup =
            sangtachi::platform::wait_any(
                std::span<const sangtachi::platform::WaitHandle>(shutdown_handles, 1), 0)
                .status != sangtachi::platform::WaitStatus::Timeout;
        if (!stopped_during_startup) {
            const int code = run_client(*parsed.args, counters, shutdown_event.get(), *control);
            if (code != 0) {
                return code;
            }
        }

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
    //
    // Phase 3 부터 Winsock 의 실제 해제는 이 줄 뒤다. `[control]` 의 채널이 참조 하나를 따로
    // 들고 아래 join 뒤에 놓는다. 블록이 닫힐 때 줄어드는 것은 main 의 참조뿐이다.
    (void)sangtachi::platform::signal_cleanup_done();

    // concurrency.md 7장 종료의 (8). `[control]` 을 join 한다. 종료 이벤트는 이미 신호돼
    // 있다(quit, 콘솔 제어 핸들러, 대기 실패). `[control]` 은 그것을 보면 진행 중인 요청을 버리고 돌아온다.
    //
    // **상한을 넘기면 `_exit` 로 즉시 끝낸다.** 상한을 실제로 강제하는 것은 소켓 시간 제한이
    // 아니라 이것이다. 정리는 (7) 에서 이미 끝났으므로 안전하다. 표준 출력과 표준 오류는
    // `_exit` 가 비우지 않으므로 먼저 비운다. 종료 코드는 정상 종료와 같은 0 이다. 문서가
    // 정했다 (concurrency.md 7장 종료).
    if (control && !control->join_for(kJoinCapMs)) {
        std::cout.flush();
        std::fflush(stdout);
        std::fflush(stderr);
        std::_Exit(0);
    }
    // 여기서 채널이 사라지며 마지막 Winsock 참조를 놓는다.
    control.reset();

    return 0;
}
