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
#include "sangtachi/protocol_constants.hpp"
#include "sangtachi/timer.hpp"

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

// 가짜 의존성 한 벌. 넣은 요청, 보낸 데이터그램, 표준 출력 줄을 모은다.
struct Rig {
    TimerSet timers;
    Counters counters;
    Millis now = 1000;
    bool queue_full = false;
    std::vector<ControlRequest> submitted;
    std::vector<Sent> sent;
    std::vector<std::string> printed;
    std::unique_ptr<ControlRunner> runner;

    explicit Rig(std::size_t stun_count = 2) {
        RunnerDeps deps;
        deps.host_header = std::string(kHostHeader);
        for (std::size_t i = 0; i < stun_count; ++i) {
            deps.stun_servers.push_back(
                StunServer{"stun" + std::to_string(i), Endpoint(0xCB007100u + static_cast<std::uint32_t>(i), 3478)});
        }
        deps.nonce = [] { return std::optional<ClientNonce>(fixed_nonce()); };
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
        runner = std::make_unique<ControlRunner>(std::move(deps), timers, counters);
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
    const auto body = sangtachi::control::create_room_body(fixed_nonce());
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
        "{\"ready\":false,\"peers\":[{\"peer_id\":123,\"virtual_ip\":\"10.100.0.1\",\"ready\":false}],\"ok\":true}";
    rig.reply(Op::kGetPeers, respond(self_in_peers), rig.submitted.back().self_peer_id);
    REQUIRE(rig.printed_starts_with("FAIL CONTROL_PLANE_EXCHANGE_FAILED "));
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
        "\"virtual_ip\":\"10.100.0.1\",\"ready\":false}],\"ok\":true}";
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
