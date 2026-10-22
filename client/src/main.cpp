#include "sangtachi/args.hpp"
#include "sangtachi/default_server.hpp"
#include "sangtachi/console.hpp"
#include "sangtachi/control/channel.hpp"
#include "sangtachi/control/http.hpp"
#include "sangtachi/control/lobby.hpp"
#include "sangtachi/control/ops.hpp"
#include "sangtachi/control/runner.hpp"
#include "sangtachi/counters.hpp"
#include "sangtachi/log.hpp"
#include "sangtachi/loop.hpp"
#include "sangtachi/record.hpp"
#include "sangtachi/network/stun_client.hpp"
#include "sangtachi/network/udp_socket.hpp"
#include "sangtachi/network/wsa.hpp"
#include "sangtachi/protocol_constants.hpp"
#include "sangtachi/platform/console_ctrl.hpp"
#include "sangtachi/platform/random.hpp"
#include "sangtachi/platform/record_file.hpp"
#include "sangtachi/platform/resolve.hpp"
#include "sangtachi/platform/stdout.hpp"
#include "sangtachi/platform/tun.hpp"
#include "sangtachi/platform/wait.hpp"
#include "sangtachi/timer.hpp"
#include "sangtachi/tunnel/data_sender.hpp"
#include "sangtachi/tunnel/tunnel.hpp"

#ifdef SANGTACHI_TEST_BUILD
#include "sangtachi/testbuild/instrument.hpp"
#endif

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
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

// 로그 파일 이름. 기동 실패의 창이 안내에 싣는다. 열기 전이면 비어 있다.
std::string g_log_name;
// 로그의 받는 곳. 루프가 시작할 때 기동이 끝났다고 알린다. 열기 전이면 널이다.
sangtachi::FileLogSink* g_log_sink = nullptr;


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

std::string guid_text(const sangtachi::platform::InterfaceGuid& g) {
    // Windows GUID 의 메모리 배치에서 표기 순서로 (Data1, Data2, Data3 은 리틀 엔디언).
    constexpr int kOrder[] = {3, 2, 1, 0, -1, 5, 4, -1, 7, 6, -1, 8, 9, -1, 10, 11, 12, 13, 14, 15};
    constexpr char kHex[] = "0123456789ABCDEF";
    std::string out = "{";
    for (const int i : kOrder) {
        if (i < 0) {
            out += '-';
            continue;
        }
        out += kHex[g[static_cast<std::size_t>(i)] >> 4];
        out += kHex[g[static_cast<std::size_t>(i)] & 0x0F];
    }
    out += '}';
    return out;
}

// architecture.md 7장 어댑터 수명의 기동 단계. 0 이면 tun 에 어댑터가 들었다. 그 밖의 값은 기동
// 실패의 종료 코드다. 실패마다 `adapter.error` 줄 하나를 낸다.
int open_adapter(std::unique_ptr<sangtachi::platform::TunDevice>& tun) {
    using sangtachi::LogLevel;
    using sangtachi::field;
    // 실행 파일 옆의 파일 셋을 먼저 본다 (architecture.md 7장 어댑터 수명). 없으면 뒤 단계가 이유를 알 수 없는 실패로
    // 끝난다. 스크립트가 없으면 PowerShell 이 종료 코드만 남긴다.
    if (const auto missing = sangtachi::platform::missing_adapter_files(); !missing.empty()) {
        std::string names;
        for (const auto name : missing) {
            names += names.empty() ? "" : ",";
            names += name;
        }
        const sangtachi::LogField fields[] = {
            field("reason", std::string_view("missing_file")),
            field("file", std::string_view(names)),
        };
        sangtachi::emit(LogLevel::Error, "adapter.error", fields);
        if (sangtachi::platform::is_console(sangtachi::platform::stdout_handle())) {
            std::string guide = "  Keep these files in the same folder as sangtachi_client.exe:";
            for (const auto name : sangtachi::platform::kAdapterFiles) {
                guide += " ";
                guide += name;
            }
            (void)sangtachi::platform::write_stdout_line(guide);
        }
        return kStartupFailure;
    }
    if (!sangtachi::platform::is_elevated()) {
        // windows-prereq.md 1절. 어댑터를 만들다 실패하는 것보다 시작 전에 막는다.
        const sangtachi::LogField fields[] = {field("reason", std::string_view("not_elevated"))};
        sangtachi::emit(LogLevel::Error, "adapter.error", fields);
        return kStartupFailure;
    }

    // windows-prereq.md 7절. 판정은 스크립트의 종료 코드다. unknown 은 통과가 아니다.
    const auto subnet = sangtachi::platform::run_subnet_check(sangtachi::adapter::kSubnetText);
    if (subnet.verdict != sangtachi::platform::SubnetVerdict::kProceed) {
        std::string blocks;
        for (const auto& prefix : subnet.blocks) {
            blocks += blocks.empty() ? "" : ",";
            blocks += prefix;
        }
        const std::string_view verdict =
            subnet.verdict == sangtachi::platform::SubnetVerdict::kBlock ? "block" : "unknown";
        const sangtachi::LogField fields[] = {
            field("reason", std::string_view("subnet")),
            field("verdict", verdict),
            field("exit_code", static_cast<std::uint64_t>(subnet.exit_code)),
            field("overlaps", blocks.empty() ? std::string_view("-") : std::string_view(blocks)),
            field("op", subnet.failure.op.empty() ? std::string_view("-") : subnet.failure.op),
            field("code", static_cast<std::uint64_t>(subnet.failure.code)),
        };
        sangtachi::emit(LogLevel::Error, "adapter.error", fields);
        return kStartupFailure;
    }

    auto opened = sangtachi::platform::TunDevice::open(sangtachi::protocol::kAdapterMtu);
    if (!opened.device) {
        const sangtachi::LogField fields[] = {
            field("reason", std::string_view("open")),
            field("op", opened.failure.op),
            field("code", static_cast<std::uint64_t>(opened.failure.code)),
        };
        sangtachi::emit(LogLevel::Error, "adapter.error", fields);
        return kStartupFailure;
    }
    tun = std::move(opened.device);
    const sangtachi::LogField fields[] = {
        field("name", sangtachi::adapter::kName),
        field("guid", guid_text(tun->guid())),
        field("luid", tun->luid()),
        field("mtu", static_cast<std::uint64_t>(sangtachi::protocol::kAdapterMtu)),
    };
    sangtachi::emit(LogLevel::Info, "adapter.open", fields);

    // windows-prereq.md 2절 클라이언트가 만드는 규칙. 어댑터가 선 뒤에 부른다. 규칙이 그 어댑터에 묶여야
    // 하기 때문이다. 판정은 스크립트의 종료 코드이고 unknown 은 통과가 아니다. 어긋나면 어댑터를 닫고
    // 끝난다(부르는 쪽이 tun 을 버린다).
    const auto firewall = sangtachi::platform::run_firewall_check();
    const auto joined = [](const std::vector<std::string>& items) {
        std::string out;
        for (const auto& item : items) {
            out += out.empty() ? "" : ",";
            out += item;
        }
        return out.empty() ? std::string("-") : out;
    };
    const std::string failed = joined(firewall.failed);
    const std::string unknown = joined(firewall.unknown);
    if (firewall.verdict != sangtachi::platform::FirewallVerdict::kPass) {
        const std::string_view verdict =
            firewall.verdict == sangtachi::platform::FirewallVerdict::kFail ? "fail" : "unknown";
        // 규칙은 그 이름으로 지운다(`Remove-NetFirewallRule -DisplayName '<이름>'`). 로그 값에는 공백을
        // 싣지 않으므로 명령이 아니라 이름을 싣는다 (architecture.md 9장).
        const sangtachi::LogField error_fields[] = {
            field("reason", std::string_view("firewall")),
            field("verdict", verdict),
            field("exit_code", static_cast<std::uint64_t>(firewall.exit_code)),
            field("failed", failed),
            field("unknown", unknown),
            field("op", firewall.failure.op.empty() ? std::string_view("-") : firewall.failure.op),
            field("code", static_cast<std::uint64_t>(firewall.failure.code)),
        };
        sangtachi::emit(LogLevel::Error, "adapter.error", error_fields);
        return kStartupFailure;
    }
    const sangtachi::LogField firewall_fields[] = {
        field("verdict", std::string_view("pass")),
        field("created", joined(firewall.created)),
    };
    sangtachi::emit(LogLevel::Info, "adapter.firewall", firewall_fields);
    return 0;
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
               sangtachi::control::ControlChannel& control,
               sangtachi::record::RecordWriter& records,
               sangtachi::platform::TunDevice* tun) {
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

    // 로컬 후보의 포트다. 6장 getsockname 의 값 하나를 모든 로컬 후보에 쓴다 (protocol.md 10.1).
    const std::uint16_t local_port = opened.socket->local().port();

    sangtachi::LoopOptions options;
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

    // 어댑터의 읽기 이벤트가 순위 2 로 들어온다 (concurrency.md 2장 대기). `--no-adapter` 면 없다.
    if (tun != nullptr) {
        loop.attach_adapter(tun);
        if (!loop.valid()) {
            emit_op_error("WintunGetReadWaitEvent", 0);
            return kStartupFailure;
        }
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

    // 8장 수신 검증 파이프라인과 세션 표 (tunnel/tunnel.hpp). 세션은 로비가 랑데부(10.2)에서 연다.
    // 실행기보다 먼저 만든다. 실행기가 이것을 가리킨다.
    //
    // 8.4 를 통과한 DATA 가 갈 곳 (concurrency.md 3장의 표). 어댑터가 있으면 두 빌드 모두 주입한다.
    // 어댑터가 없으면 제품 빌드는 훅을 비워 drop_no_sink 이고, 시험 빌드는 그 칸에 `data.rx` 와 에코
    // 응답기를 붙인다 (ADR 0023).
    sangtachi::tunnel::DataSender* sender_slot = nullptr;  // 훅이 에코를 넘길 곳. 아래에서 채운다
    sangtachi::tunnel::TunnelDeps tunnel_deps;
    if (tun != nullptr) {
        // 실패는 inject_packet 이 센다. 재시도하지 않는다 (concurrency.md 3장 주입 실패).
        tunnel_deps.data_sink = [&loop](std::span<const std::byte> inner) { (void)loop.inject_packet(inner); };
    } else {
#ifdef SANGTACHI_TEST_BUILD
        tunnel_deps.data_sink = [&sender_slot](std::span<const std::byte> inner) {
            sangtachi::testbuild::emit_data_rx(inner);
            sangtachi::testbuild::emit_echo_rtt(inner, sangtachi::platform::monotonic_us() / 1000);
            // 수신 처리 중에 다시 보내지 않는다. 큐에 넣고 루프의 송신 몫에서 보낸다.
            if (auto reply = sangtachi::testbuild::make_echo_reply(inner); reply && sender_slot) {
                sender_slot->enqueue(std::move(*reply));
            }
        };
#endif
    }
    tunnel_deps.send = [&loop](const sangtachi::network::Endpoint& to,
                               std::span<const std::byte> payload) {
        // tx_err_send 는 세션이 자기 표에 센다 (counters.hpp 의 두 범위).
        return loop.send_datagram_uncounted(to, payload);
    };
    tunnel_deps.clock = [] { return now_ms(); };
    tunnel_deps.rtt_clock_us = [] { return sangtachi::platform::monotonic_us(); };
    sangtachi::tunnel::Tunnel tunnel(counters, std::move(tunnel_deps));
#ifdef SANGTACHI_TEST_BUILD
    tunnel.set_tx_observer(
        [](std::span<const std::byte> inner) { sangtachi::testbuild::emit_data_tx(inner); });
#endif

    // 어댑터에서 읽은 패킷 (concurrency.md 3장 drain_wintun 의 route_and_send). 8.5 를 거쳐 DATA 로
    // 나간다. 시험 빌드는 라우팅 범위 밖으로 가는 ICMP 에코 요청에 먼저 답하고, 답한 것은 라우팅으로
    // 보내지 않는다 (roadmap.md Phase 6 검증).
    if (tun != nullptr) {
        loop.set_adapter_handler([&loop, &tunnel](std::span<const std::byte> packet) {
#ifdef SANGTACHI_TEST_BUILD
            if (auto reply = sangtachi::testbuild::make_icmp_echo_reply(packet)) {
                (void)loop.inject_packet(*reply);
                return;
            }
#else
            (void)loop;
#endif
            (void)tunnel.send_data(packet);
        });
    }

    // 로비와 그 행동 실행기 (control/runner.hpp). STUN 은 시도마다 새로 돈다
    // (control_plane.md 8.4: 2번 DNS 와 3번 bind 만 프로세스에 한 번이다). 그래서 기동 시
    // 여기서 STUN 을 돌리지 않는다. 해석한 서버 목록만 넘긴다.
    // 입력 자리 표시 (console.hpp ConsolePrompt). 실행기보다 먼저 만든다. 실행기의 출력 함수가 이것을 붙든다.
    const bool guide_visible =
        sangtachi::platform::is_console(sangtachi::platform::stdout_handle());
    sangtachi::ConsolePrompt prompt([](std::string_view text, bool newline) {
        const auto out = sangtachi::platform::stdout_handle();
        (void)(newline ? sangtachi::platform::write_line(out, text)
                       : sangtachi::platform::write_text(out, text));
    });

    sangtachi::control::RunnerDeps deps;
    deps.host_header = std::move(*host_header);
    deps.stun_servers = std::move(stun_servers);
    deps.nonce = [] { return sangtachi::control::generate_client_nonce(); };
    // 처음 표시 이름 (architecture.md 3.5). --name 이거나 main 이 만든 기본 이름이다.
    deps.name = *args.name;
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
    // 안내 줄과 입력 자리 표시는 사람에게만 보인다 (architecture.md 3.5 로비 안내). 표준 출력이 리다이렉트되면
    // 내지 않는다. 그 출력을 읽는 것은 스크립트이고, 스크립트는 ROOM 과 FAIL 줄만 읽는다. 콘솔이면 ROOM 과 FAIL
    // 줄도 입력 자리 표시를 거친다.
    if (guide_visible) {
        deps.print = [&prompt](std::string_view line) { prompt.line(line); };
        deps.print_guide = [&prompt](std::string_view line) { prompt.line(line); };
    }
    deps.clock = [] { return now_ms(); };
    // 받은 후보 위생의 자기 서브넷 행 (protocol.md 10.1). 집합은 루프가 소유한다 (concurrency.md 5장).
    deps.own_subnets = [&loop]() -> const sangtachi::peer::OwnSubnets& { return loop.own_subnets(); };
    // 로컬 후보 수집 (protocol.md 10.1). register_candidate 직전에 한 번 모으고 자기 서브넷 집합을 바꾼다.
    deps.candidate_sources = sangtachi::peer::platform_candidate_sources();
    deps.local_port = local_port;
    deps.set_own_subnets = [&loop](sangtachi::peer::OwnSubnets subnets) {
        loop.set_own_subnets(std::move(subnets));
    };
    deps.tunnel = &tunnel;
    // 가상 어댑터의 주소 (architecture.md 7장 어댑터 수명). 붙이지 못하면 종료한다.
    if (tun != nullptr) {
        deps.own_wintun = tun->guid();
        deps.assign_address = [tun, &loop](std::uint32_t virtual_ip) {
            const auto assigned = tun->assign_address(virtual_ip);
            if (!assigned.ok) {
                const sangtachi::LogField fields[] = {
                    sangtachi::field("reason", std::string_view("address")),
                    sangtachi::field("stage", assigned.reason),
                    sangtachi::field("op", assigned.failure.op.empty() ? std::string_view("-")
                                                                      : assigned.failure.op),
                    sangtachi::field("code", static_cast<std::uint64_t>(assigned.failure.code)),
                };
                sangtachi::emit(sangtachi::LogLevel::Error, "adapter.error", fields);
                loop.request_shutdown();
                return;
            }
            const sangtachi::LogField fields[] = {
                sangtachi::field("virtual_ip", sangtachi::network::format_ipv4(virtual_ip)),
                sangtachi::field("route", assigned.route_created ? std::string_view("created")
                                                                 : std::string_view("on_link")),
            };
            sangtachi::emit(sangtachi::LogLevel::Info, "adapter.address", fields);
        };
        deps.clear_address = [tun] {
            if (const auto failed = tun->clear_address(); !failed.op.empty()) {
                const sangtachi::LogField fields[] = {
                    sangtachi::field("reason", std::string_view("clear_address")),
                    sangtachi::field("op", failed.op),
                    sangtachi::field("code", static_cast<std::uint64_t>(failed.code)),
                };
                sangtachi::emit(sangtachi::LogLevel::Warn, "adapter.error", fields);
            }
        };
    }
    // 로컬 기록 파일 (architecture.md 9장). 줄은 실행기가 정하고 이 파일이 쓴다.
    deps.records.establish = [&records](std::string_view role, std::optional<std::uint32_t> peer,
                                        std::string_view result, std::optional<std::uint64_t> rtt_us,
                                        std::optional<std::uint64_t> setup_ms) {
        return records.establish(role, peer, result, rtt_us, setup_ms);
    };
    deps.records.end = [&records](std::uint64_t attempt, std::string_view role,
                                  std::optional<std::uint32_t> peer, std::string_view result,
                                  std::optional<std::uint64_t> rtt_us) {
        records.end(attempt, role, peer, result, rtt_us);
    };
    sangtachi::control::ControlRunner runner(std::move(deps), loop.timers(), counters);

    // 세션의 전이는 실행기가 베껴 두고, 터널 호출이 끝난 뒤 flush 가 처리한다 (control/runner.hpp).
    tunnel.set_transition_handler([&runner](const sangtachi::peer::Session& session,
                                            sangtachi::peer::SessionState from,
                                            sangtachi::peer::SessionState to) {
        runner.on_session_transition(session, from, to);
    });

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

    loop.set_tunnel_handler([&tunnel, &runner](const sangtachi::network::Endpoint& from,
                                               std::span<const std::byte> payload) {
        tunnel.on_datagram(from, payload);
        runner.flush();
    });
    loop.set_deadline_source(sangtachi::EventLoop::DeadlineSource{
        [&tunnel] { return tunnel.earliest_deadline(); },
        [&tunnel, &runner](sangtachi::Millis now) {
            tunnel.run_timers(now);
            runner.flush();
        },
    });
    // 콘솔 `counters`: 프로세스 표 뒤에 남아 있는 세션마다 그 세션의 표 (architecture.md 9장).
    loop.set_counter_dump_handler([&tunnel] { tunnel.emit_session_counters(); });

    // 콘솔 `send` (roadmap.md Phase 4). 바퀴마다 kMaxDrain 개씩 나눠 보낸다.
    // 합성 패킷의 송신 시각(오프셋 32)은 RTT 기준선이 쓴다. RTT 시계로 채운다(concurrency.md 4장). 타이머
    // 시계(GetTickCount64)는 해상도가 약 15ms 라 같은 기기의 에코가 0 이나 15 로만 잰다.
    const auto rtt_ms = [] { return static_cast<sangtachi::Millis>(sangtachi::platform::monotonic_us() / 1000); };
    sangtachi::tunnel::DataSender sender(tunnel, rtt_ms);
    sender_slot = &sender;
    loop.set_send_handler([&sender](std::string_view rest) { sender.on_command(rest); });
    loop.set_work_source([&sender](std::size_t budget) { return sender.pump(budget); });

    if (guide_visible) {
        loop.set_unknown_command_handler(
            [&prompt] { prompt.line(sangtachi::control::kUnknownCommandGuide); });
        loop.set_console_line_hooks([&prompt] { prompt.input(); }, [&prompt] { prompt.show(); });
    }

    // CLI 역할은 기동 직후의 로비 명령이다 (architecture.md 3.5). 역할이 없으면 아무
    // 요청도 STUN 도 나가지 않고 로비 안내를 낸 뒤 기다린다.
    if (const auto command =
            sangtachi::control::startup_command(args.role, args.room)) {
        runner.on_command(*command);
    } else {
        runner.start();
    }
    // 역할 인자로 시작하면 응답이 올 때까지 표준 출력이 비어 있다. 그동안에도 입력 자리를 띄운다.
    if (guide_visible) {
        prompt.show();
    }

    // `[console]` 은 join 하지 않는다 (concurrency.md 7장 종료). 표준 입력 읽기를
    // 밖에서 취소하는 수단을 쓰지 않으므로 join 하면 사용자가 한 줄을 더 칠 때까지
    // 종료가 멈춘다.
    //
    // 그래서 그 스레드가 건드리는 것을 스택에 두지 않는다. 세션을 값으로 넘겨 스레드가
    // 스스로 수명을 붙들게 한다. `main` 이 먼저 빠져나가도 해제된 것을 건드리지 않는다.
    std::thread(sangtachi::run_console_reader, session).detach();

    // 기동이 끝났다. 콘솔 창에는 이제 안내와 ROOM, FAIL 줄만 나온다 (architecture.md 9장 로그 출력).
    if (g_log_sink != nullptr) {
        g_log_sink->end_startup();
    }
    loop.run();
    session->request_stop();
    prompt.close();

    // concurrency.md 7장 종료의 (2)와 (3). 세션 없이 진행 중이던 시도의 기록 줄을 먼저 쓰고 시도를
    // 끝낸다(요청을 더 내지 않는다). 그 뒤 5.6 이 정한 대상 세션마다 CLOSE 1회를 보내고, 그 전이가
    // 세션의 기록 줄을 쓴다.
    runner.shutdown();
    tunnel.close_all(now_ms());
    runner.flush();
    // 같은 장의 (4) 가운데 세션 표 몫. 프로세스 표는 main 이 run_client 뒤에 낸다. 세션 표는
    // 터널과 함께 이 함수 끝에서 사라지므로 여기서 낸다.
    tunnel.emit_session_counters();

    // 루프가 든 핸들러를 먼저 끊는다. `runner` 는 `loop` 뒤에 선언되어 **먼저** 소멸하는데,
    // 그 핸들러들은 `runner` 를 참조로 붙든다.
    loop.set_lobby_command_handler(nullptr);
    loop.set_stun_handler(nullptr);
    loop.set_tick_handler(nullptr);
    loop.set_control_handler(nullptr);
    loop.set_tunnel_handler(nullptr);
    loop.set_deadline_source({});
    loop.set_counter_dump_handler(nullptr);
    loop.set_unknown_command_handler(nullptr);
    loop.set_console_line_hooks(nullptr, nullptr);
    loop.set_send_handler(nullptr);
    loop.set_work_source(nullptr);
    loop.set_adapter_handler(nullptr);
    tunnel.set_transition_handler(nullptr);
    return 0;
}


}  // namespace

int run_main(int argc, char** argv) {
    std::vector<std::string_view> raw;
    raw.reserve(argc > 0 ? static_cast<std::size_t>(argc - 1) : 0);
    for (int i = 1; i < argc; ++i) {
        raw.emplace_back(argv[i]);
    }

    // 빌드에 넣은 제어 서버 주소가 --server 의 기본값이다 (architecture.md 3.5, CMake 의
    // SANGTACHI_DEFAULT_SERVER). 비어 있으면 --server 가 필수다.
    const auto parsed = sangtachi::parse_args(std::span<const std::string_view>(raw),
                                              sangtachi::kDefaultServer);
    if (!parsed.ok()) {
        const sangtachi::LogField fields[] = {
            sangtachi::field("reason", sangtachi::to_token(parsed.error)),
            sangtachi::field("arg", parsed.offending),
        };
        sangtachi::emit(sangtachi::LogLevel::Error, "args.invalid", fields);
        return kStartupFailure;
    }
    sangtachi::Args args = *parsed.args;

    // 기동 안내 (architecture.md 3.5 로비 안내). 로그 파일, 서버 주소 해석, 어댑터와 방화벽 규칙까지 몇 초가
    // 걸린다. 그동안 창이 비어 있지 않게 한 줄을 낸다. 안내 줄과 같이 표준 출력이 콘솔일 때만 낸다.
    if (sangtachi::platform::is_console(sangtachi::platform::stdout_handle())) {
        (void)sangtachi::platform::write_stdout_line("Loading...");
    }

    sangtachi::Counters counters;

    // 로컬 기록 파일 (architecture.md 9장). 인자 검사 뒤, `[loop]` 를 시작하기 전에 연다. 열지 못하면
    // 기동 실패다. 기록 없이 돈 실행은 M-6 으로 판정할 수 없다. 로그에는 파일 이름만 싣는다.
    const std::string record_name = sangtachi::record::file_name(
        sangtachi::platform::wall_clock_unix_ms(), sangtachi::platform::current_process_id());
    auto record_file = sangtachi::platform::open_record_file(record_name);
    if (!record_file.ok()) {
        emit_op_error(record_file.failed_op, record_file.error);
        return kStartupFailure;
    }

    // 로그 파일 (architecture.md 9장 로그 출력). 기록 파일과 같은 이름으로 실행 파일 옆 logs\ 에 연다. 열지
    // 못하면 기동 실패다. 이 줄 앞의 로그는 표준 오류로만 나간다. 받는 곳과 파일은 프로세스 수명이다. 해제하지
    // 않는다. `[console]` 처럼 join 하지 않는 스레드가 main 뒤에도 emit 할 수 있다.
    {
        auto log_file = sangtachi::platform::open_log_file(record_name);
        if (!log_file.ok()) {
            emit_op_error(log_file.failed_op, log_file.error);
            return kStartupFailure;
        }
        auto* file = new sangtachi::platform::RecordFile(std::move(log_file.file));
        const bool screen_all =
            !sangtachi::platform::is_console(sangtachi::platform::stderr_handle());
        g_log_sink = new sangtachi::FileLogSink(
            [file](std::string_view line) { (void)file->write(line); },
            [](std::string_view line) { sangtachi::write_stderr(line); }, screen_all);
        (void)sangtachi::set_log_sink(g_log_sink);
        g_log_name = record_name;
    }

    // `--name` 이 없으면 기본 이름을 만든다 (architecture.md 3.5). 난수원이 실패하면 기동 실패다.
    // 예측 가능한 값으로 대신하면 같은 방의 두 프로세스가 같은 이름을 받는다.
    if (!args.name) {
        args.name = sangtachi::make_default_name(
            [](std::span<std::byte> out) { return sangtachi::platform::random_bytes(out); });
        if (!args.name) {
            emit_op_error("random_bytes", 0);
            return kStartupFailure;
        }
        const sangtachi::LogField fields[] = {sangtachi::field("name", std::string_view(*args.name))};
        sangtachi::emit(sangtachi::LogLevel::Info, "lobby.name", fields);
    }

    {
        const sangtachi::LogField fields[] = {
            sangtachi::field("file", record_name),
        };
        sangtachi::emit(sangtachi::LogLevel::Info, "record.open", fields);
    }
    sangtachi::record::RecordWriter records(
        [&file = record_file.file](std::string_view bytes) { return file.write(bytes); },
        [] { return sangtachi::platform::wall_clock_unix_ms(); }, counters);

    // `[control]` 의 채널. **try 블록 밖에 둔다.** concurrency.md 7장 종료가 join(8) 을 정리
    // 완료(7) 뒤에 두고, 정리 완료는 try 블록이 닫힌 뒤에 신호하기 때문이다(아래). 블록 안에
    // 두면 블록이 닫힐 때 소멸자가 join 해 순서가 뒤집힌다. 채널은 Winsock 참조를 따로 들므로
    // 블록 안의 WsaContext 가 먼저 사라져도 `[control]` 의 소켓은 살아 있다
    // (control/channel.hpp).
    std::unique_ptr<sangtachi::control::ControlChannel> control;

    // **`--stun` 목록이 한 개면 기동 시 WARN 한 줄을 남긴다** (architecture.md 3.5 기동
    // 입력). 그 목록으로는 서로 다른 두 서버의 응답을 얻을 수 없어 5초를 기다린 뒤
    // STUN_DISCOVERY_FAILED 로 끝난다. 기다리기 전에 알린다.
    if (args.stun.size() == 1) {
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
        const auto& server = *args.server;
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
        // 가상 어댑터 (architecture.md 7장 어댑터 수명의 기동 단계). **이 블록이 닫힐 때 닫힌다.**
        // 그 자리가 concurrency.md 7장 종료의 (5)(6) 이다. 카운터 전량(4) 뒤, 정리 완료(7) 앞이다.
        std::unique_ptr<sangtachi::platform::TunDevice> tun;
        if (!stopped_during_startup && !args.no_adapter) {
            if (const int code = open_adapter(tun); code != 0) {
                return code;
            }
        }
        if (!stopped_during_startup) {
            const int code =
                run_client(args, counters, shutdown_event.get(), *control, records, tun.get());
            if (code != 0) {
                return code;
            }
        }

        // concurrency.md 7장 종료의 (4). 카운터 전량을 낸다.
        sangtachi::emit_all(counters);

        // 같은 장의 (5)(6). 세션을 닫고 주소와 경로를 지우고 어댑터를 닫는다.
        if (tun) {
            if (const auto failed = tun->close(); !failed.op.empty()) {
                const sangtachi::LogField fields[] = {
                    sangtachi::field("reason", std::string_view("clear_address")),
                    sangtachi::field("op", failed.op),
                    sangtachi::field("code", static_cast<std::uint64_t>(failed.code)),
                };
                sangtachi::emit(sangtachi::LogLevel::Warn, "adapter.error", fields);
            }
            const sangtachi::LogField fields[] = {sangtachi::field("name", sangtachi::adapter::kName)};
            sangtachi::emit(sangtachi::LogLevel::Info, "adapter.close", fields);
        }

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

int main(int argc, char** argv) {
    const int code = run_main(argc, argv);
    // 기동 실패의 창 (architecture.md 3.5). 탐색기에서 더블클릭해 이 프로세스 혼자 쓰는 창이면, 끝나는 순간 창이
    // 닫혀 ERROR 줄을 읽을 수 없다. 그래서 안내 두 줄을 내고 Enter 를 기다린다. 안내는 다른 안내 줄처럼 표준 출력이
    // 콘솔일 때만 낸다. 정리는 run_main 안에서 이미 끝났다. 셸에서 띄운 실행과 시험은 창을 셸과 나눠 쓰므로 여기서
    // 멈추지 않는다.
    if (code != 0 && sangtachi::platform::is_console(sangtachi::platform::stdout_handle()) &&
        sangtachi::platform::console_is_own()) {
        std::string message = "  Could not start. The ERROR line above says why.";
        if (!g_log_name.empty()) {
            message += " Full log: logs\\" + g_log_name;
        }
        (void)sangtachi::platform::write_stdout_line(message);
        (void)sangtachi::platform::write_stdout_line("  Press Enter to close this window.");
        sangtachi::platform::wait_for_enter();
    }
    return code;
}
