#include "sangtachi/control/runner.hpp"

#include "sangtachi/control/channel.hpp"
#include "sangtachi/control/exchange.hpp"
#include "sangtachi/control/http.hpp"
#include "sangtachi/control/lobby.hpp"
#include "sangtachi/control/ops.hpp"
#include "sangtachi/counters.hpp"
#include "sangtachi/network/endpoint.hpp"
#include "sangtachi/network/stun.hpp"
#include "sangtachi/network/stun_client.hpp"
#include "sangtachi/peer/candidates.hpp"
#include "sangtachi/protocol_constants.hpp"
#include "sangtachi/timer.hpp"
#include "sangtachi/tunnel/data_sender.hpp"
#include "sangtachi/tunnel/tunnel.hpp"

#include "../session_fixture.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// 시험 케이스 이름은 ASCII 로만 적는다. 이유는 network/wsa_test.cpp 머리에 있다.
//
// control/lobby.hpp 머리의 통합 계약을 실행기가 지키는지 본다. 채널, 소켓, 시계, 표준 출력을
// 전부 가짜로 둔다. 로비의 판단 자체는 lobby_test 가 본다. 여기는 행동이 제자리에 닿는지다.

using sangtachi::Counter;
using sangtachi::Counters;
using sangtachi::Millis;
using sangtachi::TimerSet;
using sangtachi::control::ClientNonce;
using sangtachi::control::ControlRequest;
using sangtachi::control::ControlResponse;
using sangtachi::control::ControlRunner;
using sangtachi::control::ExchangeResult;
using sangtachi::control::ExchangeStatus;
using sangtachi::control::LobbyCommand;
using sangtachi::control::LobbyCommandKind;
using sangtachi::control::RunnerDeps;
using sangtachi::control::http::Op;
using sangtachi::network::Endpoint;
using sangtachi::network::StunServer;

namespace {

constexpr std::string_view kHostHeader = "127.0.0.1:9";
// 이 프로세스의 표시 이름 (architecture.md 3.5 --name).
constexpr std::string_view kName = "Alice";

constexpr std::string_view kIssued =
    "{\"room_id\":\"ABCDEF\",\"peer_id\":123,\"peer_token\":\"0123456789abcdef0123456789abcdef\","
    "\"virtual_ip\":\"10.100.0.1\",\"expires_in_s\":120,\"ok\":true}";

std::string respond(std::string_view body) {
    return "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
           std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + std::string(body);
}

ClientNonce fixed_nonce() {
    std::array<std::byte, ClientNonce::kBytes> bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<std::byte>(0xA0 + i);
    }
    return ClientNonce::from_bytes(bytes);
}

struct Sent {
    Endpoint to;
    std::vector<std::byte> bytes;
};

// RecordSink 가 받은 줄 하나 (architecture.md 9장 줄 형식의 값).
struct Line {
    bool end = false;
    std::uint64_t attempt = 0;
    std::string role;
    std::optional<std::uint32_t> peer;
    std::string result;
    std::optional<std::uint64_t> rtt_us;
    std::optional<std::uint64_t> setup_ms;
};

// 가짜 의존성 한 벌. 넣은 요청, 보낸 데이터그램, 표준 출력 줄을 모은다.
struct Rig {
    TimerSet timers;
    Counters counters;
    Millis now = 1000;
    bool queue_full = false;
    std::vector<ControlRequest> submitted;
    std::vector<Sent> sent;
    std::vector<std::string> printed;
    // 실행기가 해석 때 읽는 자기 서브넷 집합 (RunnerDeps::own_subnets). 시험이 바꿔 넣는다.
    sangtachi::peer::OwnSubnets own_subnets;
    // 터널. 세션의 송신은 tunnel_sent 에 모은다.
    std::vector<Sent> tunnel_sent;
    std::unique_ptr<sangtachi::tunnel::Tunnel> tunnel;
    std::vector<Line> lines;
    std::uint64_t next_attempt = 0;
    std::unique_ptr<ControlRunner> runner;

    explicit Rig(std::size_t stun_count = 2, sangtachi::peer::CandidateSources sources = {}) {
        sangtachi::tunnel::TunnelDeps tdeps;
        tdeps.send = [this](const Endpoint& to, std::span<const std::byte> payload) {
            tunnel_sent.push_back(Sent{to, std::vector<std::byte>(payload.begin(), payload.end())});
            return true;
        };
        tdeps.clock = [this] { return now; };
        tdeps.rtt_clock_us = [this] { return static_cast<std::uint64_t>(now) * 1000; };
        tdeps.random = [](std::span<std::byte> out) {
            for (std::size_t i = 0; i < out.size(); ++i) {
                out[i] = static_cast<std::byte>(i + 1);
            }
            return true;
        };
        tunnel = std::make_unique<sangtachi::tunnel::Tunnel>(counters, std::move(tdeps));

        RunnerDeps deps;
        deps.host_header = std::string(kHostHeader);
        for (std::size_t i = 0; i < stun_count; ++i) {
            deps.stun_servers.push_back(
                StunServer{"stun" + std::to_string(i), Endpoint(0xCB007100u + static_cast<std::uint32_t>(i), 3478)});
        }
        deps.nonce = [] { return std::optional<ClientNonce>(fixed_nonce()); };
        deps.name = std::string(kName);
        deps.submit = [this](ControlRequest r) {
            if (queue_full) {
                counters.increment(Counter::ControlQueueDropped);
                return false;
            }
            submitted.push_back(std::move(r));
            return true;
        };
        deps.send = [this](const Endpoint& to, std::span<const std::byte> payload) {
            sent.push_back(Sent{to, std::vector<std::byte>(payload.begin(), payload.end())});
            return true;
        };
        deps.print = [this](std::string_view line) { printed.emplace_back(line); };
        deps.clock = [this] { return now; };
        deps.own_subnets = [this]() -> const sangtachi::peer::OwnSubnets& { return own_subnets; };
        deps.candidate_sources = std::move(sources);
        deps.local_port = 50000;
        deps.set_own_subnets = [this](sangtachi::peer::OwnSubnets s) { own_subnets = std::move(s); };
        deps.tunnel = tunnel.get();
        deps.records.establish = [this](std::string_view role, std::optional<std::uint32_t> peer,
                                        std::string_view result, std::optional<std::uint64_t> rtt,
                                        std::optional<std::uint64_t> setup) {
            lines.push_back(Line{false, ++next_attempt, std::string(role), peer, std::string(result), rtt, setup});
            return next_attempt;
        };
        deps.records.end = [this](std::uint64_t attempt, std::string_view role,
                                  std::optional<std::uint32_t> peer, std::string_view result,
                                  std::optional<std::uint64_t> rtt) {
            lines.push_back(Line{true, attempt, std::string(role), peer, std::string(result), rtt, std::nullopt});
        };
        runner = std::make_unique<ControlRunner>(std::move(deps), timers, counters);
        tunnel->set_transition_handler([this](const sangtachi::peer::Session& s,
                                              sangtachi::peer::SessionState from,
                                              sangtachi::peer::SessionState to) {
            runner->on_session_transition(s, from, to);
        });
    }

    ~Rig() {
        tunnel->set_transition_handler(nullptr);
    }

    void command(LobbyCommandKind kind, std::string argument = {}) {
        LobbyCommand c;
        c.kind = kind;
        c.argument = std::move(argument);
        runner->on_command(c);
    }

    void reply(Op op, std::string raw, std::uint32_t self = 0) {
        ControlResponse r;
        r.op = op;
        r.self_peer_id = self;
        r.result.status = ExchangeStatus::kComplete;
        r.result.response = std::move(raw);
        runner->on_response(r);
    }

    void transport(Op op, ExchangeStatus status) {
        ControlResponse r;
        r.op = op;
        r.result.status = status;
        runner->on_response(r);
    }

    bool printed_starts_with(std::string_view prefix) const {
        for (const auto& line : printed) {
            if (line.rfind(prefix, 0) == 0) {
                return true;
            }
        }
        return false;
    }
};

// Binding Success Response 한 통 (RFC 5389, protocol.md 13장).
std::vector<std::byte> stun_success(const sangtachi::network::TransactionId& id, const Endpoint& mapped) {
    std::vector<std::byte> out;
    auto u16 = [&out](std::uint16_t v) {
        out.push_back(static_cast<std::byte>(v >> 8));
        out.push_back(static_cast<std::byte>(v & 0xFF));
    };
    auto u32 = [&u16](std::uint32_t v) {
        u16(static_cast<std::uint16_t>(v >> 16));
        u16(static_cast<std::uint16_t>(v & 0xFFFF));
    };
    u16(0x0101);
    u16(12);
    u32(sangtachi::protocol::kStunCookie);
    for (const std::byte b : id) {
        out.push_back(b);
    }
    u16(0x0020);
    u16(8);
    u16(0x0001);
    u16(static_cast<std::uint16_t>(mapped.port() ^ (sangtachi::protocol::kStunCookie >> 16)));
    u32(mapped.address() ^ sangtachi::protocol::kStunCookie);
    return out;
}

}  // namespace

TEST_CASE("control_runner: host submits create_room with the Host header and the body",
          "[control][runner]") {
    Rig rig;
    rig.command(LobbyCommandKind::kHost);

    REQUIRE(rig.submitted.size() == 1);
    REQUIRE(rig.submitted[0].op == Op::kCreateRoom);
    // 바이트는 build_request(Host 헤더, op, 로비가 만든 본문) 그대로다 (control_plane.md 3.3).
    // 로비의 처음 이름은 RunnerDeps::name 이다 (architecture.md 3.5 --name).
    const auto body = sangtachi::control::create_room_body(fixed_nonce(), kName);
    REQUIRE(body.has_value());
    const auto expected = sangtachi::control::http::build_request(kHostHeader, Op::kCreateRoom, *body);
    REQUIRE(expected.has_value());
    REQUIRE(rig.submitted[0].bytes == *expected);
    REQUIRE(rig.submitted[0].bytes.rfind("POST /v1/create_room HTTP/1.1\r\nHost: 127.0.0.1:9\r\n", 0) == 0);
}

TEST_CASE("control_runner: a create_room reply prints ROOM and starts a fresh STUN",
          "[control][runner]") {
    Rig rig;
    rig.command(LobbyCommandKind::kHost);
    REQUIRE(rig.runner->stun() == nullptr);
    REQUIRE(rig.sent.empty());

    rig.reply(Op::kCreateRoom, respond(kIssued));

    REQUIRE(rig.printed_starts_with("ROOM ABCDEF"));
    // STUN 은 시도마다 돈다. 앞 두 서버에 동시에 질의한다 (architecture.md 3.5 서버 선택).
    REQUIRE(rig.runner->stun() != nullptr);
    REQUIRE(rig.sent.size() == 2);
    REQUIRE(rig.timers.contains("stun.deadline.0"));
}

TEST_CASE("control_runner: the tunnel knows its own virtual IP from create_room, before any session",
          "[control][runner]") {
    // protocol.md 8.5 출발지 검사. 호스트 혼자인 동안에도 어댑터에서 읽은 패킷의 출발지를 가른다. 모르면 전부
    // tx_drop_bad_src 가 되어 경로 없는 유니캐스트(tx_drop_no_route)와 운영체제 잡음(tx_drop_not_unicast)을
    // 셀 수 없다. adapter-check.ps1 의 route 장면이 이것으로 처음 떨어졌다.
    Rig rig;
    rig.command(LobbyCommandKind::kHost);
    REQUIRE_FALSE(rig.tunnel->local_virtual_ip().has_value());
    rig.reply(Op::kCreateRoom, respond(kIssued));
    REQUIRE(rig.tunnel->session_count() == 0);
    REQUIRE(rig.tunnel->local_virtual_ip() == 0x0A640001u);

    const auto to_empty_slot = sangtachi::tunnel::make_synthetic(100, 0x0A640001u, 0x0A640003u, 0, 0);
    REQUIRE(rig.tunnel->send_data(to_empty_slot) == sangtachi::tunnel::TxOutcome::kDropped);
    REQUIRE(rig.counters.value(Counter::TxDropNoRoute) == 1);
    REQUIRE(rig.counters.value(Counter::TxDropBadSrc) == 0);

    // 방을 떠나면 잊는다.
    rig.command(LobbyCommandKind::kLeave);
    REQUIRE_FALSE(rig.tunnel->local_virtual_ip().has_value());
}

TEST_CASE("control_runner: a transport failure is interpreted as one and the retry timer is armed",
          "[control][runner]") {
    // 받지 못한 응답은 ops::transport_failure 다 (control_plane.md 8.3 일시 오류). create_room 은
    // 1초 뒤 같은 본문으로 다시 보낸다.
    Rig rig;
    rig.command(LobbyCommandKind::kHost);
    rig.transport(Op::kCreateRoom, ExchangeStatus::kConnectFailed);

    REQUIRE(rig.printed.empty());
    REQUIRE(rig.timers.contains("control.retry"));
    rig.runner->on_timer("control.retry");
    REQUIRE(rig.submitted.size() == 2);
    REQUIRE(rig.submitted[1].bytes == rig.submitted[0].bytes);
}

TEST_CASE("control_runner: a full request queue ends the attempt with one FAIL line",
          "[control][runner]") {
    // concurrency.md 8장 "가득 찼을 때": 시도를 CONTROL_PLANE_EXCHANGE_FAILED 로 끝낸다.
    Rig rig;
    rig.queue_full = true;
    rig.command(LobbyCommandKind::kHost);

    REQUIRE(rig.submitted.empty());
    REQUIRE(rig.counters.value(Counter::ControlQueueDropped) == 1);
    REQUIRE(rig.printed.size() == 1);
    REQUIRE(rig.printed[0].rfind("FAIL CONTROL_PLANE_EXCHANGE_FAILED ", 0) == 0);
    REQUIRE(rig.runner->lobby().in_lobby());
}

TEST_CASE("control_runner: leave during STUN clears the STUN timers", "[control][runner]") {
    // StunClient 는 소멸자에서 타이머를 지우지 않는다. 남기면 버린 객체의 타이머가 만료한다.
    Rig rig;
    rig.command(LobbyCommandKind::kHost);
    rig.reply(Op::kCreateRoom, respond(kIssued));
    REQUIRE(rig.runner->stun() != nullptr);
    REQUIRE(rig.timers.contains("stun.retry.0"));

    rig.command(LobbyCommandKind::kLeave);

    REQUIRE(rig.runner->stun() == nullptr);
    for (const char* name : {"stun.retry.0", "stun.retry.1", "stun.deadline.0", "stun.deadline.1"}) {
        INFO("timer=" << name);
        REQUIRE_FALSE(rig.timers.contains(name));
    }
}

TEST_CASE("control_runner: a STUN stage that ends at start is reported to the lobby",
          "[control][runner]") {
    // 서버가 하나면 start 가 그 자리에서 실패한다 (architecture.md 3.5). 로비가 그것을 들어야
    // FAIL 줄이 나온다.
    Rig rig(1);
    rig.command(LobbyCommandKind::kHost);
    rig.reply(Op::kCreateRoom, respond(kIssued));

    REQUIRE(rig.printed_starts_with("FAIL STUN_DISCOVERY_FAILED "));
}

TEST_CASE("control_runner: two STUN answers lead to register_candidate with the mapping",
          "[control][runner]") {
    Rig rig;
    rig.command(LobbyCommandKind::kJoin, "abcdef");
    REQUIRE(rig.submitted.size() == 1);
    REQUIRE(rig.submitted[0].op == Op::kJoinRoom);
    rig.reply(Op::kJoinRoom, respond(kIssued));
    REQUIRE(rig.sent.size() == 2);

    const Endpoint mapped(0xC6336401u, 40000);  // 198.51.100.1:40000
    for (std::size_t i = 0; i < 2; ++i) {
        const auto id = sangtachi::network::peek_transaction_id(rig.sent[i].bytes);
        REQUIRE(id.has_value());
        rig.runner->on_stun_datagram(rig.sent[i].to, stun_success(*id, mapped));
    }

    REQUIRE(rig.submitted.size() == 2);
    REQUIRE(rig.submitted[1].op == Op::kRegisterCandidate);
    REQUIRE(rig.submitted[1].bytes.find("198.51.100.1") != std::string::npos);
}

TEST_CASE("control_runner: get_peers is interpreted with the echoed self peer id",
          "[control][runner]") {
    // 응답에 자신의 peer_id 가 있으면 확정 오류다 (control_plane.md 4.5, 8.4 의 8번). 실행기가
    // 요청에 실었던 self_peer_id 를 쓰지 않으면 이 판정이 사라진다.
    Rig rig;
    rig.command(LobbyCommandKind::kJoin, "ABCDEF");
    rig.reply(Op::kJoinRoom, respond(kIssued));
    const Endpoint mapped(0xC6336401u, 40000);
    for (std::size_t i = 0; i < 2; ++i) {
        const auto id = sangtachi::network::peek_transaction_id(rig.sent[i].bytes);
        rig.runner->on_stun_datagram(rig.sent[i].to, stun_success(*id, mapped));
    }
    rig.reply(Op::kRegisterCandidate, respond("{\"accepted\":1,\"rejected\":0,\"ok\":true}"));
    rig.runner->on_timer("control.poll");
    REQUIRE(rig.submitted.back().op == Op::kGetPeers);
    REQUIRE(rig.submitted.back().self_peer_id == 123);

    const std::string self_in_peers =
        "{\"ready\":false,\"peers\":[{\"peer_id\":123,\"virtual_ip\":\"10.100.0.1\",\"name\":\"Host\",\"ready\":false}],\"ok\":true}";
    rig.reply(Op::kGetPeers, respond(self_in_peers), rig.submitted.back().self_peer_id);
    REQUIRE(rig.printed_starts_with("FAIL CONTROL_PLANE_EXCHANGE_FAILED "));
}

TEST_CASE("control_runner: get_peers hygiene uses the own subnet set of the loop",
          "[control][runner]") {
    // protocol.md 10.1 자기 서브넷 행. 상대 후보가 우리 서브넷의 브로드캐스트 하나뿐이면 위생 뒤 0개이고
    // 준비 완료가 아니다(control_plane.md 8.4). 실행기가 RunnerDeps::own_subnets 를 해석에 넘기지 않으면
    // 그 후보가 남아 플레이어가 폴링을 멈춘다.
    auto run = [](bool with_set) {
        Rig rig;
        if (with_set) {
            rig.own_subnets = sangtachi::peer::OwnSubnets({{0xC0A8000Au, 24}});  // 192.168.0.10/24
        }
        rig.command(LobbyCommandKind::kJoin, "ABCDEF");
        rig.reply(Op::kJoinRoom, respond(kIssued));
        const Endpoint mapped(0xC6336401u, 40000);
        for (std::size_t i = 0; i < 2; ++i) {
            const auto id = sangtachi::network::peek_transaction_id(rig.sent[i].bytes);
            rig.runner->on_stun_datagram(rig.sent[i].to, stun_success(*id, mapped));
        }
        rig.reply(Op::kRegisterCandidate, respond("{\"accepted\":1,\"rejected\":0,\"ok\":true}"));
        rig.runner->on_timer("control.poll");
        REQUIRE(rig.submitted.back().op == Op::kGetPeers);
        const std::string ready =
            "{\"ready\":true,\"peers\":[{\"peer_id\":7,\"virtual_ip\":\"10.100.0.2\",\"name\":\"Host\",\"ready\":true,"
            "\"punch_delay_ms\":1000,\"elapsed_since_ready_ms\":0,\"candidates\":["
            "{\"ip\":\"192.168.0.255\",\"port\":5000,\"kind\":\"local\"}]}],\"ok\":true}";
        rig.reply(Op::kGetPeers, respond(ready), rig.submitted.back().self_peer_id);
        return rig.runner->lobby().stage();
    };
    CHECK(run(true) == sangtachi::control::LobbyStage::kPolling);
    CHECK(run(false) == sangtachi::control::LobbyStage::kSettled);
}

TEST_CASE("control_runner: a STUN datagram outside an attempt counts drop_stun_parse",
          "[control][runner]") {
    Rig rig;
    std::array<std::byte, 20> junk{};
    rig.runner->on_stun_datagram(Endpoint(0x7F000001u, 3478), junk);
    REQUIRE(rig.counters.value(Counter::DropStunParse) == 1);
}

TEST_CASE("control_runner: host_report is interpreted with the echoed self peer id",
          "[control][runner]") {
    // host_report 응답의 peers 에 호스트 자신이 있으면 확정 오류다 (control_plane.md 4.6 오류 표의
    // "그 밖의 확정 오류", 8.4 의 8번). FAIL 한 줄, host_report 멈춤, 세션이 없으므로 로비다.
    // 실행기가 요청에 실었던 self_peer_id 를 해석에 쓰지 않으면 이 응답이 성공이 된다.
    Rig rig;
    rig.command(LobbyCommandKind::kHost);
    rig.reply(Op::kCreateRoom, respond(kIssued));
    const Endpoint mapped(0xC6336401u, 40000);
    for (std::size_t i = 0; i < 2; ++i) {
        const auto id = sangtachi::network::peek_transaction_id(rig.sent[i].bytes);
        REQUIRE(id.has_value());
        rig.runner->on_stun_datagram(rig.sent[i].to, stun_success(*id, mapped));
    }
    REQUIRE(rig.submitted.back().op == Op::kRegisterCandidate);
    rig.reply(Op::kRegisterCandidate, respond("{\"accepted\":1,\"rejected\":0,\"ok\":true}"));
    REQUIRE(rig.runner->lobby().host_reporting());

    rig.runner->on_timer("control.host_report");
    REQUIRE(rig.submitted.back().op == Op::kHostReport);
    REQUIRE(rig.submitted.back().self_peer_id == 123);
    const std::size_t submitted_before = rig.submitted.size();

    const std::string self_in_peers =
        "{\"expires_in_s\":120,\"released\":[],\"confirmed\":[],\"peers\":[{\"peer_id\":123,"
        "\"virtual_ip\":\"10.100.0.1\",\"name\":\"Host\",\"ready\":false}],\"ok\":true}";
    rig.reply(Op::kHostReport, respond(self_in_peers), rig.submitted.back().self_peer_id);

    std::size_t fails = 0;
    for (const auto& line : rig.printed) {
        if (line.rfind("FAIL CONTROL_PLANE_EXCHANGE_FAILED ", 0) == 0) {
            ++fails;
        }
    }
    REQUIRE(fails == 1);
    REQUIRE_FALSE(rig.runner->lobby().host_reporting());
    REQUIRE_FALSE(rig.timers.contains("control.host_report"));
    REQUIRE(rig.runner->lobby().in_lobby());
    rig.runner->on_timer("control.host_report");
    REQUIRE(rig.submitted.size() == submitted_before);
}

TEST_CASE("control_runner: finished STUN slots are remembered until the next STUN starts",
          "[control][runner]") {
    // protocol.md 13장 세는 표의 기억 범위. STUN 이 끝난 뒤 시도가 끝나면 기억이 로비까지 남고,
    // 다음 시도의 STUN 이 시작될 때 지운다. 지운 뒤 온 앞 시도의 응답은 센다.
    Rig rig;
    rig.command(LobbyCommandKind::kHost);
    rig.reply(Op::kCreateRoom, respond(kIssued));
    REQUIRE(rig.sent.size() == 2);
    const Endpoint mapped(0xC6336401u, 40000);
    const auto first = sangtachi::network::peek_transaction_id(rig.sent[0].bytes);
    REQUIRE(first.has_value());
    for (std::size_t i = 0; i < 2; ++i) {
        const auto id = sangtachi::network::peek_transaction_id(rig.sent[i].bytes);
        REQUIRE(id.has_value());
        rig.runner->on_stun_datagram(rig.sent[i].to, stun_success(*id, mapped));
    }
    REQUIRE(rig.submitted.back().op == Op::kRegisterCandidate);

    rig.command(LobbyCommandKind::kLeave);
    // 로비에서 온 앞 시도의 재전송 응답. 끝난 자리의 트랜잭션이다.
    rig.runner->on_stun_datagram(rig.sent[0].to, stun_success(*first, mapped));
    REQUIRE(rig.counters.value(Counter::DropStunParse) == 0);

    // 버려진 register_candidate 의 응답이 와야 다음 host 를 받는다 (concurrency.md 8장).
    rig.reply(Op::kRegisterCandidate, respond("{\"accepted\":1,\"rejected\":0,\"ok\":true}"));
    rig.command(LobbyCommandKind::kHost);
    // 다음 시도는 시작했지만 STUN 은 아직이다. 기억이 남아 있다.
    rig.runner->on_stun_datagram(rig.sent[0].to, stun_success(*first, mapped));
    REQUIRE(rig.counters.value(Counter::DropStunParse) == 0);

    rig.reply(Op::kCreateRoom, respond(kIssued));
    REQUIRE(rig.sent.size() == 4);  // 다음 시도의 STUN 이 시작됐다

    rig.runner->on_stun_datagram(rig.sent[0].to, stun_success(*first, mapped));
    REQUIRE(rig.counters.value(Counter::DropStunParse) == 1);
}

TEST_CASE("control_runner: leaving during STUN forgets that attempt's transactions",
          "[control][runner]") {
    // STUN 이 끝나기 전에 시도가 끝나면 기억을 그 자리에서 지운다 (protocol.md 13장, 11장
    // "시도가 끝나면"). 그 뒤 온 응답은 센다.
    Rig rig;
    rig.command(LobbyCommandKind::kHost);
    rig.reply(Op::kCreateRoom, respond(kIssued));
    const auto first = sangtachi::network::peek_transaction_id(rig.sent[0].bytes);
    REQUIRE(first.has_value());

    rig.command(LobbyCommandKind::kLeave);
    REQUIRE(rig.runner->stun() == nullptr);
    rig.runner->on_stun_datagram(rig.sent[0].to,
                                 stun_success(*first, Endpoint(0xC6336401u, 40000)));
    REQUIRE(rig.counters.value(Counter::DropStunParse) == 1);
}

// ---------------------------------------------------------------- 4차 J: 세션, 기록 줄, 로컬 후보

namespace {

using sangtachi::control::SessionEvent;
using sangtachi::peer::SessionFailure;
using sangtachi::peer::SessionState;
using sangtachi::tunnel::CloseReason;

constexpr std::uint32_t kHostPeer = 7;
constexpr std::uint32_t kHostVip = 0x0A640002u;  // 10.100.0.2
const Endpoint kHostCand(0xC6336407u, 5000);     // 198.51.100.7:5000

std::string ready_reply(std::string_view vip = "10.100.0.2", std::int64_t elapsed = 0) {
    return "{\"ready\":true,\"peers\":[{\"peer_id\":7,\"virtual_ip\":\"" + std::string(vip) +
           "\",\"name\":\"Host\",\"ready\":true,\"punch_delay_ms\":1000,\"elapsed_since_ready_ms\":" +
           std::to_string(elapsed) +
           ",\"candidates\":[{\"ip\":\"198.51.100.7\",\"port\":5000,\"kind\":\"reflexive\"}]}],\"ok\":true}";
}

// join 부터 STUN 성공까지. STUN 은 now 에 시작한다.
void player_registered(Rig& rig) {
    rig.command(LobbyCommandKind::kJoin, "ABCDEF");
    rig.reply(Op::kJoinRoom, respond(kIssued));
    const Endpoint mapped(0xC6336401u, 40000);
    for (std::size_t i = 0; i < 2; ++i) {
        const auto id = sangtachi::network::peek_transaction_id(rig.sent[i].bytes);
        rig.runner->on_stun_datagram(rig.sent[i].to, stun_success(*id, mapped));
    }
    rig.reply(Op::kRegisterCandidate, respond("{\"accepted\":1,\"rejected\":0,\"ok\":true}"));
}

void player_ready(Rig& rig, std::string_view vip = "10.100.0.2") {
    player_registered(rig);
    rig.runner->on_timer("control.poll");
    REQUIRE(rig.submitted.back().op == Op::kGetPeers);
    rig.reply(Op::kGetPeers, respond(ready_reply(vip)), rig.submitted.back().self_peer_id);
}

// 호스트 쪽에서 본 HELLO 와 HELLO_ACK 을 넣어 플레이어 세션을 CONNECTED 로 만든다.
void connect_host(Rig& rig) {
    using namespace sangtachi::testing;
    sangtachi::peer::Session* s = rig.tunnel->find(kHostPeer);
    REQUIRE(s != nullptr);
    Rx h = hello(kPeerEpoch, 0, kHostCand, kHostVip);
    h.header.peer_id = kHostPeer;
    rig.tunnel->on_datagram(kHostCand, h.wire());
    Rx a = hello_ack(kPeerEpoch, 1, kHostCand, kHostVip, s->attempt_nonce());
    a.header.peer_id = kHostPeer;
    rig.tunnel->on_datagram(kHostCand, a.wire());
    rig.runner->flush();
    REQUIRE(s->state() == SessionState::kConnected);
}

}  // namespace

TEST_CASE("control_runner: record_result table of architecture 9", "[control][runner][record]") {
    struct Row {
        SessionState to;
        std::optional<SessionFailure> failure;
        std::optional<CloseReason> reason;
        bool connected;
        std::string_view result;
    };
    const Row rows[] = {
        {SessionState::kFailed, SessionFailure::kHolePunchTimeout, std::nullopt, false, "HOLE_PUNCH_TIMEOUT"},
        {SessionState::kFailed, SessionFailure::kPeerHandshakeFailed, std::nullopt, false, "PEER_HANDSHAKE_FAILED"},
        {SessionState::kFailed, SessionFailure::kTunnelDropped, std::nullopt, true, "TUNNEL_DROPPED"},
        // 재협상 뒤 펀치 마감은 닿은 뒤에도 그 코드다
        {SessionState::kFailed, SessionFailure::kHolePunchTimeout, std::nullopt, true, "HOLE_PUNCH_TIMEOUT"},
        {SessionState::kClosed, std::nullopt, CloseReason::kNormal, false, "ABORTED"},
        {SessionState::kClosed, std::nullopt, CloseReason::kUserCancel, false, "ABORTED"},
        {SessionState::kClosed, std::nullopt, CloseReason::kConfigError, false, "PEER_FAILED"},
        {SessionState::kClosed, std::nullopt, CloseReason::kSetupFailed, false, "PEER_FAILED"},
        {SessionState::kClosed, std::nullopt, CloseReason::kNormal, true, "CLOSED"},
        {SessionState::kClosed, std::nullopt, CloseReason::kUserCancel, true, "CLOSED"},
        {SessionState::kClosed, std::nullopt, CloseReason::kConfigError, true, "PEER_FAILED"},
        {SessionState::kClosed, std::nullopt, CloseReason::kSetupFailed, true, "PEER_FAILED"},
        {SessionState::kClosed, std::nullopt, std::nullopt, false, "ABORTED"},  // 이쪽이 닫음
        {SessionState::kClosed, std::nullopt, std::nullopt, true, "CLOSED"},
    };
    for (const auto& row : rows) {
        SessionEvent e;
        e.peer_id = 9;
        e.to = row.to;
        e.failure = row.failure;
        e.peer_close_reason = row.reason;
        INFO("result=" << row.result << " connected=" << row.connected);
        CHECK(sangtachi::control::record_result(e, row.connected) == row.result);
    }
}

TEST_CASE("control_runner: a player opens the session on ready and punches after the delay",
          "[control][runner]") {
    // control_plane.md 8.4 의 9번, protocol.md 10.2 의 6번. 지연이 끝나기 전에는 HELLO 가 없다.
    Rig rig;
    player_ready(rig);
    sangtachi::peer::Session* s = rig.tunnel->find(kHostPeer);
    REQUIRE(s != nullptr);
    CHECK(s->peer_virtual_ip() == kHostVip);
    CHECK(s->is_candidate(kHostCand));
    CHECK_FALSE(s->punch_started());
    CHECK(rig.tunnel_sent.empty());
    REQUIRE(rig.timers.contains("control.punch.7"));

    rig.runner->on_timer("control.punch.7");
    CHECK(s->punch_started());
    REQUIRE(rig.tunnel_sent.size() == 1);
    CHECK(rig.tunnel_sent[0].to == kHostCand);
}

TEST_CASE("control_runner: a connected player records OK with setup_ms and CLOSED on leave",
          "[control][runner][record]") {
    // architecture.md 9장. 플레이어의 기점은 STUN 단계 시작이다. 수립 줄은 CONNECTED 진입에, 종료 줄은
    // 세션이 끝날 때 쓰고 같은 시도 번호를 쓴다.
    Rig rig;
    rig.now = 1000;
    player_ready(rig);  // STUN 은 1000 에 시작했다
    rig.runner->on_timer("control.punch.7");
    // 전이는 3410 에 일어나고 flush 는 그 뒤다. setup_ms 는 전이 시각으로 잰다.
    {
        using namespace sangtachi::testing;
        sangtachi::peer::Session* s = rig.tunnel->find(kHostPeer);
        rig.now = 3410;
        Rx h = hello(kPeerEpoch, 0, kHostCand, kHostVip);
        h.header.peer_id = kHostPeer;
        rig.tunnel->on_datagram(kHostCand, h.wire());
        Rx a = hello_ack(kPeerEpoch, 1, kHostCand, kHostVip, s->attempt_nonce());
        a.header.peer_id = kHostPeer;
        rig.tunnel->on_datagram(kHostCand, a.wire());
        REQUIRE(s->state() == SessionState::kConnected);
        rig.now = 5000;
        rig.runner->flush();
    }
    REQUIRE(rig.lines.size() == 1);
    CHECK_FALSE(rig.lines[0].end);
    CHECK(rig.lines[0].role == "player");
    CHECK(rig.lines[0].peer == kHostPeer);
    CHECK(rig.lines[0].result == "OK");
    CHECK(rig.lines[0].setup_ms == 2410);

    rig.command(LobbyCommandKind::kLeave);
    REQUIRE(rig.lines.size() == 2);
    CHECK(rig.lines[1].end);
    CHECK(rig.lines[1].attempt == rig.lines[0].attempt);
    CHECK(rig.lines[1].result == "CLOSED");
    CHECK(rig.lines[1].setup_ms == std::nullopt);
    CHECK(rig.runner->lobby().in_lobby());
    CHECK(rig.tunnel->session_count() == 0);  // 방을 떠나면 남긴 세션까지 지운다 (protocol.md 5.6)
}

TEST_CASE("control_runner: a punch timeout prints FAIL with the host name and records one line",
          "[control][runner][record]") {
    Rig rig;
    player_ready(rig);
    rig.runner->on_timer("control.punch.7");
    rig.now += sangtachi::protocol::kPunchDeadlineMs;
    rig.tunnel->run_timers(rig.now);
    rig.runner->flush();
    REQUIRE(rig.printed_starts_with("FAIL HOLE_PUNCH_TIMEOUT Host "));
    REQUIRE(rig.lines.size() == 1);
    CHECK(rig.lines[0].result == "HOLE_PUNCH_TIMEOUT");
    CHECK(rig.lines[0].peer == kHostPeer);
    CHECK(rig.runner->lobby().in_lobby());
    CHECK(rig.tunnel->session_count() == 0);
}

TEST_CASE("control_runner: a session the tunnel cannot open fails the attempt with CPEF",
          "[control][runner][record]") {
    // 10.100.0.9 는 라우팅 칸 밖이라 터널이 만들지 않는다 (tunnel.hpp 의 머리 표).
    Rig rig;
    player_ready(rig, "10.100.0.9");
    CHECK(rig.tunnel->session_count() == 0);
    REQUIRE(rig.printed_starts_with("FAIL CONTROL_PLANE_EXCHANGE_FAILED - "));
    REQUIRE(rig.lines.size() == 1);
    CHECK(rig.lines[0].result == "CONTROL_PLANE_EXCHANGE_FAILED");
    CHECK(rig.lines[0].peer == kHostPeer);
    CHECK(rig.runner->lobby().in_lobby());
    CHECK_FALSE(rig.timers.contains("control.punch.7"));
}

TEST_CASE("control_runner: shutdown records ABORTED for an attempt without a session",
          "[control][runner][record]") {
    // concurrency.md 7장 종료 3번. 요청을 더 내지 않는다.
    Rig rig;
    rig.command(LobbyCommandKind::kJoin, "ABCDEF");
    rig.reply(Op::kJoinRoom, respond(kIssued));
    const std::size_t submitted = rig.submitted.size();
    rig.runner->shutdown();
    REQUIRE(rig.lines.size() == 1);
    CHECK(rig.lines[0].result == "ABORTED");
    CHECK(rig.lines[0].peer == std::nullopt);
    CHECK(rig.submitted.size() == submitted);
    CHECK(rig.runner->lobby().in_lobby());
}

TEST_CASE("control_runner: shutdown with a connected session leaves the lines to close_all",
          "[control][runner][record]") {
    Rig rig;
    player_ready(rig);
    rig.runner->on_timer("control.punch.7");
    connect_host(rig);
    rig.runner->shutdown();
    CHECK(rig.lines.size() == 1);            // Lobby 는 세션이 있는 시도에 줄을 쓰지 않는다
    CHECK(rig.tunnel->session_count() == 1);  // LeaveRoom 을 내지 않는다
    rig.tunnel->close_all(rig.now);
    rig.runner->flush();
    REQUIRE(rig.lines.size() == 2);
    CHECK(rig.lines[1].end);
    CHECK(rig.lines[1].result == "CLOSED");
}

TEST_CASE("control_runner: local candidates are collected before register_candidate",
          "[control][runner]") {
    // protocol.md 10.1 수집 시점과 등록 목록. 반사 후보 뒤에 기본 경로 인터페이스의 로컬 후보가 오고,
    // 자기 서브넷 집합이 그 수집의 것으로 바뀐다. 로컬 후보의 포트는 bind 한 포트다.
    using sangtachi::platform::DadState;
    using sangtachi::platform::OperStatus;
    sangtachi::peer::CandidateSources sources;
    int interface_calls = 0;
    sources.query_interfaces = [&interface_calls] {
        ++interface_calls;
        sangtachi::platform::InterfaceQuery q;
        q.ok = true;
        sangtachi::platform::InterfaceInfo info;
        info.if_index = 4;
        info.if_type = 71;
        info.oper_status = OperStatus::kUp;
        info.addresses.push_back({0xC0A8000Au, 24, DadState::kPreferred});  // 192.168.0.10/24
        q.interfaces.push_back(info);
        return q;
    };
    sources.query_routes = [] {
        sangtachi::platform::RouteQuery q;
        q.ok = true;
        q.routes.push_back({0, 0, 4, 10, 25});
        return q;
    };
    Rig rig(2, sources);
    rig.command(LobbyCommandKind::kJoin, "ABCDEF");
    rig.reply(Op::kJoinRoom, respond(kIssued));
    CHECK(interface_calls == 0);  // STUN 이 끝나기 전에는 모으지 않는다
    const Endpoint mapped(0xC6336401u, 40000);
    for (std::size_t i = 0; i < 2; ++i) {
        const auto id = sangtachi::network::peek_transaction_id(rig.sent[i].bytes);
        rig.runner->on_stun_datagram(rig.sent[i].to, stun_success(*id, mapped));
    }
    CHECK(interface_calls == 1);
    REQUIRE(rig.submitted.back().op == Op::kRegisterCandidate);
    const std::string& bytes = rig.submitted.back().bytes;
    const auto reflexive = bytes.find("\"198.51.100.1\"");
    const auto local = bytes.find("\"192.168.0.10\"");
    REQUIRE(reflexive != std::string::npos);
    REQUIRE(local != std::string::npos);
    CHECK(reflexive < local);
    CHECK(bytes.find("50000") != std::string::npos);
    CHECK(rig.own_subnets.blocks_broadcast(0xC0A800FFu));
}

TEST_CASE("control_runner: a host opens a session per ready pair with the first-seen setup base",
          "[control][runner][record]") {
    // architecture.md 9장: 호스트의 기점은 그 플레이어가 host_report 응답에 처음 보인 때다.
    // 세션이 끝나면 departed 를 담은 host_report 가 바로 나간다 (control_plane.md 4.6).
    Rig rig;
    rig.now = 1000;
    rig.command(LobbyCommandKind::kHost);
    rig.reply(Op::kCreateRoom, respond(kIssued));  // 호스트의 가상 IP 는 10.100.0.1
    const Endpoint mapped(0xC6336401u, 40000);
    for (std::size_t i = 0; i < 2; ++i) {
        const auto id = sangtachi::network::peek_transaction_id(rig.sent[i].bytes);
        rig.runner->on_stun_datagram(rig.sent[i].to, stun_success(*id, mapped));
    }
    rig.reply(Op::kRegisterCandidate, respond("{\"accepted\":1,\"rejected\":0,\"ok\":true}"));
    CHECK(rig.tunnel->roster().active());

    rig.now = 5000;
    rig.runner->on_timer("control.host_report");
    REQUIRE(rig.submitted.back().op == Op::kHostReport);
    const std::string unready =
        "{\"expires_in_s\":120,\"released\":[],\"confirmed\":[],\"peers\":[{\"peer_id\":9,"
        "\"virtual_ip\":\"10.100.0.2\",\"name\":\"Bob\",\"ready\":false}],\"ok\":true}";
    rig.reply(Op::kHostReport, respond(unready), 123);
    CHECK(rig.tunnel->session_count() == 0);

    rig.now = 10000;
    rig.runner->on_timer("control.host_report");
    const std::string ready =
        "{\"expires_in_s\":120,\"released\":[],\"confirmed\":[9],\"peers\":[{\"peer_id\":9,"
        "\"virtual_ip\":\"10.100.0.2\",\"name\":\"Bob\",\"ready\":true,\"punch_delay_ms\":1000,"
        "\"elapsed_since_ready_ms\":1000,\"candidates\":[{\"ip\":\"198.51.100.9\",\"port\":6000,"
        "\"kind\":\"reflexive\"}]}],\"ok\":true}";
    rig.reply(Op::kHostReport, respond(ready), 123);
    sangtachi::peer::Session* s = rig.tunnel->find(9);
    REQUIRE(s != nullptr);
    CHECK(s->punch_started());  // 지연이 0 이면 바로 시작한다

    // 12410 에 연결된다. 기점은 처음 보인 5000 이다.
    using namespace sangtachi::testing;
    const Endpoint from(0xC6336409u, 6000);
    rig.now = 12410;
    Rx h = hello(kPeerEpoch, 0, from, 0x0A640002u);
    h.header.peer_id = 9;
    rig.tunnel->on_datagram(from, h.wire());
    Rx a = hello_ack(kPeerEpoch, 1, from, 0x0A640002u, s->attempt_nonce());
    a.header.peer_id = 9;
    rig.tunnel->on_datagram(from, a.wire());
    rig.runner->flush();
    REQUIRE(s->state() == SessionState::kConnected);
    REQUIRE(rig.lines.size() == 1);
    CHECK(rig.lines[0].result == "OK");
    CHECK(rig.lines[0].setup_ms == 7410);

    // 상대가 CLOSE 0x03 으로 끝낸다.
    Rx c = close_rx(kPeerEpoch, 2, 0x03, from);
    c.header.peer_id = 9;
    rig.tunnel->on_datagram(from, c.wire());
    rig.runner->flush();
    REQUIRE(s->state() == SessionState::kClosed);
    REQUIRE(rig.lines.size() == 2);
    CHECK(rig.lines[1].end);
    CHECK(rig.lines[1].role == "host");
    CHECK(rig.lines[1].peer == 9u);
    CHECK(rig.lines[1].result == "PEER_FAILED");
    CHECK_FALSE(rig.printed_starts_with("FAIL PEER"));  // 상대의 통지는 이쪽의 실패가 아니다
    REQUIRE(rig.submitted.back().op == Op::kHostReport);
    CHECK(rig.submitted.back().bytes.find("\"departed\":[9]") != std::string::npos);
    CHECK_FALSE(rig.runner->lobby().in_lobby());
}

TEST_CASE("control_runner: a renegotiated session keeps one establish line and its attempt number",
          "[control][runner][record]") {
    // runner.hpp: 다시 CONNECTED 에 들어가도 수립 줄을 다시 쓰지 않는다. 종료 줄은 처음 번호다.
    using namespace sangtachi::testing;
    Rig rig;
    player_ready(rig);
    rig.runner->on_timer("control.punch.7");
    connect_host(rig);
    sangtachi::peer::Session* s = rig.tunnel->find(kHostPeer);
    rig.now += 2000;
    Rx h = hello(kPeerEpoch2, 0, kHostCand, kHostVip);
    h.header.peer_id = kHostPeer;
    rig.tunnel->on_datagram(kHostCand, h.wire());
    rig.runner->flush();
    REQUIRE(s->renegotiations() == 1);
    REQUIRE(s->state() != SessionState::kConnected);
    Rx a = hello_ack(kPeerEpoch2, 1, kHostCand, kHostVip, s->attempt_nonce());
    a.header.peer_id = kHostPeer;
    rig.tunnel->on_datagram(kHostCand, a.wire());
    rig.runner->flush();
    REQUIRE(s->state() == SessionState::kConnected);
    CHECK(rig.lines.size() == 1);
    rig.command(LobbyCommandKind::kLeave);
    REQUIRE(rig.lines.size() == 2);
    CHECK(rig.lines[1].end);
    CHECK(rig.lines[1].attempt == rig.lines[0].attempt);
    CHECK(rig.lines[1].result == "CLOSED");
}

TEST_CASE("control_runner: the end line carries the last RTT sample and the establish line none",
          "[control][runner][record]") {
    // architecture.md 9장: 수립 줄은 대개 비고 종료 줄에 마지막 표본이 들어간다. RTT 시계는 now * 1000 us 다.
    using namespace sangtachi::testing;
    Rig rig;
    player_ready(rig);
    rig.runner->on_timer("control.punch.7");
    connect_host(rig);
    REQUIRE(rig.lines.size() == 1);
    CHECK(rig.lines[0].rtt_us == std::nullopt);
    // 진입 5초 뒤 첫 PING 이 나간다 (protocol.md 11장).
    rig.tunnel_sent.clear();
    rig.now += 5000;
    rig.tunnel->run_timers(rig.now);
    rig.runner->flush();
    std::optional<std::uint64_t> ping_id;
    for (const auto& sent : rig.tunnel_sent) {
        const auto check = sangtachi::tunnel::check_common(sent.bytes);
        if (check.ok() && check.header.type == static_cast<std::uint8_t>(sangtachi::tunnel::PacketType::kPing)) {
            std::uint64_t id = 0;
            for (const std::byte b : check.payload) {
                id = (id << 8) | std::to_integer<std::uint64_t>(b);
            }
            ping_id = id;
        }
    }
    REQUIRE(ping_id.has_value());
    rig.now += 23;
    Rx p = pong(kPeerEpoch, 2, *ping_id, kHostCand);
    p.header.peer_id = kHostPeer;
    rig.tunnel->on_datagram(kHostCand, p.wire());
    rig.runner->flush();
    rig.command(LobbyCommandKind::kLeave);
    REQUIRE(rig.lines.size() == 2);
    CHECK(rig.lines[1].rtt_us == 23000u);
}
