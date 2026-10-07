#include "sangtachi/control/channel.hpp"

#include "sangtachi/console.hpp"
#include "sangtachi/control/http.hpp"
#include "sangtachi/control/lobby.hpp"
#include "sangtachi/control/runner.hpp"
#include "sangtachi/counters.hpp"
#include "sangtachi/loop.hpp"
#include "sangtachi/network/udp_socket.hpp"
#include "sangtachi/network/wsa.hpp"
#include "sangtachi/platform/wait.hpp"
#include "sangtachi/timer.hpp"

#include "loopback_tcp.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

// 시험 케이스 이름은 ASCII 로만 적는다. 이유는 network/wsa_test.cpp 머리에 있다.
//
// concurrency.md 8장 `[control]` 스레드의 표와 2장 대기의 순위 4, 3장의 drain_control,
// roadmap.md Phase 3 의 "클라이언트 쪽 계약" 을 본다.

using sangtachi::Counter;
using sangtachi::Counters;
using sangtachi::control::ControlChannel;
using sangtachi::control::ControlRequest;
using sangtachi::control::ControlResponse;
using sangtachi::control::ExchangeStatus;
using sangtachi::control::ExchangeTimeouts;
using sangtachi::control::kControlQueueCapacity;
using sangtachi::control::http::Op;
using sangtachi::network::WsaContext;
using sangtachi::platform::monotonic_ms;
using sangtachi::platform::WaitHandle;
using sangtachi_test::http_reply;
using sangtachi_test::LoopbackServer;
using sangtachi_test::Script;

namespace {

class OwnedEvent {
public:
    OwnedEvent() : handle_(sangtachi::platform::create_event(sangtachi::platform::ResetMode::Manual)) {}
    ~OwnedEvent() { sangtachi::platform::close_event(handle_); }
    OwnedEvent(const OwnedEvent&) = delete;
    OwnedEvent& operator=(const OwnedEvent&) = delete;
    [[nodiscard]] WaitHandle get() const noexcept { return handle_; }

private:
    WaitHandle handle_;
};

bool wait_signaled(WaitHandle handle, std::uint32_t timeout_ms) {
    const WaitHandle handles[] = {handle};
    return sangtachi::platform::wait_any(std::span<const WaitHandle>(handles, 1), timeout_ms)
               .status == sangtachi::platform::WaitStatus::Signaled;
}

ControlRequest request(Op op = Op::kGetPeers) {
    auto bytes = sangtachi::control::http::build_request("127.0.0.1:8000", op, R"({"x":1})");
    REQUIRE(bytes.has_value());
    return ControlRequest{op, *bytes};
}

std::unique_ptr<ControlChannel> start_on(std::uint16_t port, WaitHandle shutdown, Counters& counters,
                                         const ExchangeTimeouts& timeouts = {}) {
    auto channel = ControlChannel::start("127.0.0.1", port, shutdown, counters, timeouts);
    REQUIRE(channel != nullptr);
    const auto resolved = channel->wait_resolved();
    REQUIRE(resolved.has_value());
    REQUIRE(resolved->to_string() == "127.0.0.1:" + std::to_string(port));
    return channel;
}

Script silent() {
    Script s;
    s.silent = true;
    return s;
}

}  // namespace

TEST_CASE("control_channel: a request round trip signals the response event",
          "[control][channel]") {
    const WsaContext wsa;
    const std::string body = R"({"ok":true})";
    LoopbackServer server({Script{http_reply(body)}});
    OwnedEvent shutdown;
    Counters counters;
    auto channel = start_on(server.port(), shutdown.get(), counters);

    ControlRequest sent = request(Op::kJoinRoom);
    sent.self_peer_id = 77;  // [control] 은 보지 않고 응답에 되싣는다
    REQUIRE(channel->submit(sent));

    // `[loop]` 는 응답 이벤트로 깨어난다 (concurrency.md 8장의 깨우기 행).
    REQUIRE(wait_signaled(channel->response_event(), 5000));
    auto response = channel->try_pop();
    REQUIRE(response.has_value());
    REQUIRE(response->op == Op::kJoinRoom);
    REQUIRE(response->self_peer_id == 77);
    REQUIRE(response->result.status == ExchangeStatus::kComplete);
    REQUIRE(response->result.response == http_reply(body));
    REQUIRE_FALSE(channel->try_pop().has_value());

    server.stop();
    REQUIRE(server.requests().size() == 1);
    REQUIRE(server.requests()[0] == sent.bytes);
}

TEST_CASE("control_channel: a transport failure still comes back as a response",
          "[control][channel]") {
    // 실패도 응답 큐로 온다. 오지 않으면 `[loop]` 의 미결 요청이 영영 풀리지 않는다.
    const WsaContext wsa;
    LoopbackServer server({Script{"HTTP/1.1 200 OK\r\nContent-Length: 20\r\n\r\n{}"}});
    OwnedEvent shutdown;
    Counters counters;
    auto channel = start_on(server.port(), shutdown.get(), counters);

    REQUIRE(channel->submit(request()));
    REQUIRE(wait_signaled(channel->response_event(), 5000));
    auto response = channel->try_pop();
    REQUIRE(response.has_value());
    REQUIRE(response->result.status == ExchangeStatus::kClosedEarly);
    REQUIRE_FALSE(response->result.complete());
}

TEST_CASE("control_channel: a resolve failure is reported and the thread ends",
          "[control][channel]") {
    // control_plane.md 8.2: 해석에 실패하면 기동 실패다. "1.2.3" 은 숫자 모양이라 이름으로
    // 묻지 않고 실패한다 (platform/resolve.hpp). 망이 없어도 같은 결과다.
    const WsaContext wsa;
    OwnedEvent shutdown;
    Counters counters;
    auto channel = ControlChannel::start("1.2.3", 8000, shutdown.get(), counters);
    REQUIRE(channel != nullptr);
    REQUIRE_FALSE(channel->wait_resolved().has_value());
    REQUIRE(channel->join_for(1000));
}

TEST_CASE("control_channel: a full request ring drops and counts control_queue_dropped",
          "[control][channel]") {
    // concurrency.md 8장의 가득 찼을 때 행. 첫 요청이 조용한 서버에 붙들려 있는 동안 여덟을
    // 더 넣으면 링이 차고, 아홉째는 버려지며 카운터가 오른다.
    const WsaContext wsa;
    LoopbackServer server({silent()});
    OwnedEvent shutdown;
    Counters counters;
    ExchangeTimeouts t;
    t.io_ms = 20000;
    auto channel = start_on(server.port(), shutdown.get(), counters, t);

    REQUIRE(channel->submit(request()));
    // 서버가 요청을 받았다면 `[control]` 은 그 요청을 이미 링에서 꺼냈다.
    REQUIRE(server.wait_requests(1, 5000));

    for (std::size_t i = 0; i < kControlQueueCapacity; ++i) {
        REQUIRE(channel->submit(request()));
    }
    REQUIRE(counters.value(Counter::ControlQueueDropped) == 0);
    REQUIRE_FALSE(channel->submit(request()));
    REQUIRE(counters.value(Counter::ControlQueueDropped) == 1);

    // 종료는 붙들린 요청을 기다리지 않는다 (concurrency.md 8장의 종료 행).
    const auto start = monotonic_ms();
    REQUIRE(sangtachi::platform::signal_event(shutdown.get()));
    REQUIRE(channel->join_for(2000));
    REQUIRE(monotonic_ms() - start < 1000);
    // 버린 요청과 남은 요청에 대한 응답은 없다.
    REQUIRE_FALSE(channel->try_pop().has_value());
}

TEST_CASE("control_channel: join_for reports false while a request is still pending",
          "[control][channel]") {
    // join 상한은 main 이 `_exit` 를 고르는 근거다 (concurrency.md 7장 종료). 종료가 신호되지
    // 않은 채 요청이 붙들려 있으면 상한 안에 끝나지 않았다고 말해야 한다.
    const WsaContext wsa;
    LoopbackServer server({silent()});
    OwnedEvent shutdown;
    Counters counters;
    ExchangeTimeouts t;
    t.io_ms = 20000;
    auto channel = start_on(server.port(), shutdown.get(), counters, t);

    REQUIRE(channel->submit(request()));
    REQUIRE(server.wait_requests(1, 5000));
    REQUIRE_FALSE(channel->join_for(200));
    REQUIRE(sangtachi::platform::signal_event(shutdown.get()));
    REQUIRE(channel->join_for(2000));
}

TEST_CASE("control_channel: a full response ring holds the next response instead of dropping it",
          "[control][channel]") {
    // 응답 링이 차면 `[control]` 은 자리가 날 때까지 기다린다 (control/channel.cpp 의 deliver).
    // 버리면 그 응답을 기다리는 시도가 마감까지 멈춘다.
    const WsaContext wsa;
    std::vector<Script> scripts(kControlQueueCapacity + 1, Script{http_reply("{}")});
    LoopbackServer server(scripts);
    OwnedEvent shutdown;
    Counters counters;
    auto channel = start_on(server.port(), shutdown.get(), counters);

    for (std::size_t i = 0; i < kControlQueueCapacity; ++i) {
        REQUIRE(channel->submit(request()));
    }
    REQUIRE(server.wait_requests(kControlQueueCapacity, 5000));
    // 여덟 응답이 링을 채울 시간을 준다. 꺼내지 않는다.
    const auto deadline = monotonic_ms() + 5000;
    while (server.served() < kControlQueueCapacity && monotonic_ms() < deadline) {
        wait_signaled(shutdown.get(), 10);
    }
    REQUIRE(channel->submit(request()));
    REQUIRE(server.wait_requests(kControlQueueCapacity + 1, 5000));
    while (server.served() < kControlQueueCapacity + 1 && monotonic_ms() < deadline) {
        wait_signaled(shutdown.get(), 10);
    }
    wait_signaled(shutdown.get(), 200);

    std::size_t popped = 0;
    const auto until = monotonic_ms() + 3000;
    while (popped < kControlQueueCapacity + 1 && monotonic_ms() < until) {
        if (auto r = channel->try_pop()) {
            REQUIRE(r->result.status == ExchangeStatus::kComplete);
            ++popped;
        } else {
            wait_signaled(channel->response_event(), 100);
        }
    }
    REQUIRE(popped == kControlQueueCapacity + 1);
}

TEST_CASE("control_channel: the channel keeps Winsock alive after the caller releases it",
          "[control][channel]") {
    // main 의 WsaContext 는 정리 완료(7)보다 먼저 사라지고 join(8)은 그 뒤다 (concurrency.md 7장
    // 종료). 채널이 자기 참조를 들지 않으면 그 사이 `[control]` 의 소켓 밑에서 Winsock 이 끝난다.
    OwnedEvent shutdown;
    Counters counters;
    std::unique_ptr<LoopbackServer> server;
    std::unique_ptr<ControlChannel> channel;
    {
        const WsaContext wsa;
        server = std::make_unique<LoopbackServer>(std::vector<Script>{Script{http_reply("{}")}});
        channel = start_on(server->port(), shutdown.get(), counters);
    }
    REQUIRE(channel->submit(request()));
    REQUIRE(wait_signaled(channel->response_event(), 5000));
    auto response = channel->try_pop();
    REQUIRE(response.has_value());
    REQUIRE(response->result.status == ExchangeStatus::kComplete);
    server.reset();
    channel.reset();
}

TEST_CASE("control_channel: destroying an idle channel does not hang", "[control][channel]") {
    const WsaContext wsa;
    LoopbackServer server({});
    OwnedEvent shutdown;
    Counters counters;
    {
        auto channel = start_on(server.port(), shutdown.get(), counters);
    }
    // 소멸자가 종료를 걸고 join 한다. 그 이벤트는 복제본을 통해 원본에도 보인다.
    REQUIRE(wait_signaled(shutdown.get(), 0));
}

// ---------------------------------------------------------------- 루프와의 연결

namespace {

struct LoopRig {
    std::shared_ptr<sangtachi::ConsoleSession> session = sangtachi::ConsoleSession::create();
    Counters counters;
    std::unique_ptr<sangtachi::EventLoop> loop;

    explicit LoopRig(sangtachi::LoopOptions options = {}) {
        auto opened = sangtachi::network::open_udp_socket();
        REQUIRE(opened.ok());
        REQUIRE(session != nullptr);
        loop = std::make_unique<sangtachi::EventLoop>(std::move(*opened.socket), counters,
                                                      session->queue(), session->event(), options);
        REQUIRE(loop->valid());
    }
};

}  // namespace

TEST_CASE("control_channel: the loop wakes on the response event and hands the response over",
          "[control][channel][loop]") {
    // 순위 4 가 대기 집합에 없으면 루프는 다른 출처(여기서는 3초짜리 보호 타이머)가 깨울
    // 때까지 응답을 보지 못한다 (concurrency.md 2장 대기, 3장의 drain_control).
    const WsaContext wsa;
    Script script{http_reply(R"({"ok":true})")};
    script.hold_ms = 500;  // 응답이 늦게 와야 루프가 대기에 들어간 뒤에 깨어난다
    LoopbackServer server({script});
    LoopRig rig;
    auto channel = start_on(server.port(), rig.loop->shutdown_event(), rig.counters);

    rig.loop->attach_control(channel.get());
    REQUIRE(rig.loop->valid());
    REQUIRE(rig.loop->timers().add_periodic("guard", 3000, monotonic_ms()));

    std::vector<ControlResponse> seen;
    rig.loop->set_control_handler([&seen](ControlResponse r) { seen.push_back(std::move(r)); });

    REQUIRE(channel->submit(request(Op::kHostReport)));
    const auto start = monotonic_ms();
    while (seen.empty() && monotonic_ms() - start < 5000) {
        REQUIRE(rig.loop->run_once());
    }
    const auto elapsed = monotonic_ms() - start;

    REQUIRE(seen.size() == 1);
    REQUIRE(seen[0].op == Op::kHostReport);
    REQUIRE(seen[0].result.complete());
    REQUIRE(elapsed < 1500);
    rig.loop->set_control_handler(nullptr);
}

TEST_CASE("control_channel: loop timers keep firing while a control request hangs",
          "[control][channel][loop]") {
    // roadmap.md Phase 3 의 클라이언트 쪽 계약. 제어 요청이 미결인 구간에서도 probe200 의
    // elapsed_ms 가 400 을 넘지 않는다. 조용한 서버가 요청을 붙든다.
    const WsaContext wsa;
    LoopbackServer server({silent()});
    sangtachi::LoopOptions options;
    options.probe_timer = true;
    LoopRig rig(options);
    ExchangeTimeouts t;
    t.io_ms = 20000;
    auto channel = start_on(server.port(), rig.loop->shutdown_event(), rig.counters, t);
    rig.loop->attach_control(channel.get());

    std::vector<std::uint64_t> elapsed;
    rig.loop->set_tick_handler([&elapsed](const sangtachi::TimerTick& tick) {
        if (tick.name == sangtachi::kProbeTimerName) {
            elapsed.push_back(tick.elapsed_ms);
        }
    });

    REQUIRE(channel->submit(request()));
    REQUIRE(server.wait_requests(1, 5000));
    const auto start = monotonic_ms();
    while (monotonic_ms() - start < 1500) {
        REQUIRE(rig.loop->run_once());
    }
    rig.loop->set_tick_handler(nullptr);

    REQUIRE(elapsed.size() >= 5);
    for (const auto e : elapsed) {
        INFO("elapsed_ms=" << e);
        REQUIRE(e <= 400);
    }
    // 요청은 아직 미결이다. 응답이 오지 않았다.
    REQUIRE_FALSE(channel->try_pop().has_value());

    // 루프의 종료가 `[control]` 까지 끝낸다. 같은 종료 이벤트를 본다 (concurrency.md 7장 종료).
    rig.loop->request_shutdown();
    REQUIRE_FALSE(rig.loop->run_once());
    const auto stop_start = monotonic_ms();
    REQUIRE(channel->join_for(2000));
    REQUIRE(monotonic_ms() - stop_start < 1000);
}

TEST_CASE("control_channel: a host command typed into the loop sends create_room and prints ROOM",
          "[control][channel][loop][runner]") {
    // 콘솔 줄 -> parse_lobby_command -> 로비 -> 실행기 -> 요청 큐 -> [control] -> 루프백 서버 ->
    // 응답 큐 -> drain_control -> interpret -> 로비 -> ROOM 줄. 끝에서 끝까지 한 번이다.
    const WsaContext wsa;
    const std::string issued =
        "{\"room_id\":\"ABCDEF\",\"peer_id\":123,\"peer_token\":\"0123456789abcdef0123456789abcdef\","
        "\"virtual_ip\":\"10.100.0.1\",\"expires_in_s\":120,\"ok\":true}";
    LoopbackServer server({Script{http_reply(issued)}, silent()});
    LoopRig rig;
    auto channel = start_on(server.port(), rig.loop->shutdown_event(), rig.counters);
    rig.loop->attach_control(channel.get());

    std::vector<std::string> printed;
    sangtachi::control::RunnerDeps deps;
    deps.host_header = "127.0.0.1:" + std::to_string(server.port());
    // 답하지 않는 루프백 STUN 서버 둘. 이 케이스는 STUN 결과를 보지 않는다.
    deps.stun_servers = {{"a", sangtachi::network::Endpoint(0x7F000001u, 9)},
                         {"b", sangtachi::network::Endpoint(0x7F000001u, 19)}};
    deps.nonce = [] { return sangtachi::control::generate_client_nonce(); };
    deps.submit = [&channel](ControlRequest r) { return channel->submit(std::move(r)); };
    deps.send = [&rig](const sangtachi::network::Endpoint& to, std::span<const std::byte> p) {
        return rig.loop->send_datagram(to, p);
    };
    deps.print = [&printed](std::string_view line) { printed.emplace_back(line); };
    deps.clock = [] { return monotonic_ms(); };
    sangtachi::control::ControlRunner runner(std::move(deps), rig.loop->timers(), rig.counters);
    rig.loop->set_lobby_command_handler(
        [&runner](const sangtachi::control::LobbyCommand& c) { runner.on_command(c); });
    rig.loop->set_control_handler([&runner](ControlResponse r) { runner.on_response(r); });
    rig.loop->set_stun_handler([&runner](const sangtachi::network::Endpoint& from,
                                         std::span<const std::byte> p) { runner.on_stun_datagram(from, p); });
    rig.loop->set_tick_handler([&runner](const sangtachi::TimerTick& t) { runner.on_timer(t.name); });

    REQUIRE(rig.session->queue().try_push("host"));
    REQUIRE(sangtachi::platform::signal_event(rig.session->event()));

    const auto start = monotonic_ms();
    while (printed.empty() && monotonic_ms() - start < 5000) {
        REQUIRE(rig.loop->run_once());
    }
    REQUIRE(printed.size() == 1);
    REQUIRE(printed[0] == "ROOM ABCDEF");
    REQUIRE(runner.stun() != nullptr);  // 방 코드를 낸 뒤 STUN 이 돈다 (8.4 의 4번 뒤 5번)

    const auto requests = server.requests();
    REQUIRE_FALSE(requests.empty());
    REQUIRE(requests[0].rfind("POST /v1/create_room HTTP/1.1\r\nHost: 127.0.0.1:", 0) == 0);

    rig.loop->set_lobby_command_handler(nullptr);
    rig.loop->set_control_handler(nullptr);
    rig.loop->set_stun_handler(nullptr);
    rig.loop->set_tick_handler(nullptr);
    rig.loop->request_shutdown();
    REQUIRE_FALSE(rig.loop->run_once());
    REQUIRE(channel->join_for(2000));
}
