#include "hamychi/args.hpp"

#include <catch2/catch_test_macros.hpp>

#include <span>
#include <string>
#include <string_view>
#include <vector>

// 시험 케이스 이름은 ASCII 로만 적는다. 이유는 network/wsa_test.cpp 머리에 있다.
//
// architecture.md 3.5 가 "반례 목록은 시험이 갖는다" 로 넘긴 표다.

using hamychi::ArgError;
using hamychi::parse_args;
using hamychi::ParseResult;
using hamychi::Role;

namespace {

ParseResult run(std::initializer_list<std::string_view> argv) {
    const std::vector<std::string_view> v(argv);
    return parse_args(std::span<const std::string_view>(v));
}

struct RejectCase {
    std::vector<std::string_view> argv;
    ArgError error;
    std::string_view why;
};

}  // namespace

TEST_CASE("args: empty argv starts with no role", "[args]") {
    // Phase 1~2 는 역할이 없어도 기동한다. 필수 판정은 Phase 3 에서 붙는다.
    const auto r = run({});
    REQUIRE(r.ok());
    REQUIRE(r.args->role == Role::None);
    REQUIRE_FALSE(r.args->server.has_value());
    REQUIRE_FALSE(r.args->peer.has_value());
    REQUIRE(r.args->stun.empty());
}

TEST_CASE("args: role positional", "[args]") {
    REQUIRE(run({"host"}).args->role == Role::Host);
    REQUIRE(run({"player"}).args->role == Role::Player);
}

TEST_CASE("args: server takes the control port by default", "[args]") {
    const auto r = run({"--server", "control.example.com"});
    REQUIRE(r.ok());
    REQUIRE(r.args->server->host == "control.example.com");
    REQUIRE(r.args->server->port == 8000);  // control_plane.md 2.6 CONTROL_PORT
}

TEST_CASE("args: server keeps an explicit port", "[args]") {
    const auto r = run({"--server", "203.0.113.7:9000"});
    REQUIRE(r.ok());
    REQUIRE(r.args->server->host == "203.0.113.7");
    REQUIRE(r.args->server->port == 9000);
}

TEST_CASE("args: room is normalized to upper case", "[args]") {
    const auto r = run({"--room", "abcdef"});
    REQUIRE(r.ok());
    REQUIRE(*r.args->room == "ABCDEF");
    REQUIRE(*run({"--room", "aBcDeF"}).args->room == "ABCDEF");
    REQUIRE(*run({"--room", "ABCDEF"}).args->room == "ABCDEF");
}

TEST_CASE("args: rejoin splits id and token", "[args]") {
    const auto r = run({"--rejoin", "4294967295:0123456789abcdef0123456789abcdef"});
    REQUIRE(r.ok());
    REQUIRE(r.args->rejoin->peer_id == 4294967295u);
    REQUIRE(r.args->rejoin->peer_token == "0123456789abcdef0123456789abcdef");
}

TEST_CASE("args: peer is an IPv4 endpoint", "[args]") {
    const auto r = run({"--peer", "192.0.2.10:30000"});
    REQUIRE(r.ok());
    REQUIRE(r.args->peer->to_string() == "192.0.2.10:30000");
}

TEST_CASE("args: stun accumulates and the rest is last wins", "[args]") {
    const auto r = run({"--stun", "a.example:3478",
                        "--stun", "b.example:19302",
                        "--room", "ABCDEF",
                        "--room", "GHJKLM",
                        "--peer", "192.0.2.1:1",
                        "--peer", "192.0.2.2:2"});
    REQUIRE(r.ok());
    REQUIRE(r.args->stun.size() == 2);
    REQUIRE(r.args->stun[0].host == "a.example");
    REQUIRE(r.args->stun[0].port == 3478);
    REQUIRE(r.args->stun[1].port == 19302);
    REQUIRE(*r.args->room == "GHJKLM");
    REQUIRE(r.args->peer->to_string() == "192.0.2.2:2");
}

TEST_CASE("args: reject case table", "[args]") {
    const RejectCase cases[] = {
        // 위치 인자
        {{"Host"}, ArgError::BadRole, "role is case sensitive"},
        {{"hosts"}, ArgError::BadRole, "not a role"},
        {{""}, ArgError::BadRole, "empty positional"},
        {{"host", "player"}, ArgError::ExtraPositional, "two positionals"},

        // 옵션 자체
        {{"--nope", "x"}, ArgError::UnknownOption, "unknown option"},
        {{"--server=x"}, ArgError::UnknownOption, "equals form is not accepted"},
        {{"-server", "x"}, ArgError::BadRole, "single dash is a positional, not a role"},
        {{"--server"}, ArgError::MissingValue, "option at the end without a value"},
        {{"--peer"}, ArgError::MissingValue, "option at the end without a value"},

        // --room. control_plane.md 2.1
        {{"--room", "ABCDE"}, ArgError::BadRoom, "five characters"},
        {{"--room", "ABCDEFG"}, ArgError::BadRoom, "seven characters"},
        {{"--room", "ABCDE0"}, ArgError::BadRoom, "0 is outside the alphabet"},
        {{"--room", "ABCDEO"}, ArgError::BadRoom, "O is outside the alphabet"},
        {{"--room", "ABCDE1"}, ArgError::BadRoom, "1 is outside the alphabet"},
        {{"--room", "ABCDEI"}, ArgError::BadRoom, "I is outside the alphabet"},
        {{"--room", " ABCDE"}, ArgError::BadRoom, "leading space is not trimmed"},
        {{"--room", "ABCDE-"}, ArgError::BadRoom, "punctuation"},
        {{"--room", ""}, ArgError::BadRoom, "empty"},

        // --rejoin. control_plane.md 2.2, 2.3
        {{"--rejoin", "1"}, ArgError::BadRejoin, "no colon"},
        {{"--rejoin", "0:0123456789abcdef0123456789abcdef"}, ArgError::BadRejoin, "peer_id 0 is excluded"},
        {{"--rejoin", "4294967296:0123456789abcdef0123456789abcdef"}, ArgError::BadRejoin, "peer_id over 32 bits"},
        {{"--rejoin", "-1:0123456789abcdef0123456789abcdef"}, ArgError::BadRejoin, "negative peer_id"},
        {{"--rejoin", "1:0123456789abcdef0123456789abcde"}, ArgError::BadRejoin, "token of 31 characters"},
        {{"--rejoin", "1:0123456789abcdef0123456789abcdef0"}, ArgError::BadRejoin, "token of 33 characters"},
        {{"--rejoin", "1:0123456789ABCDEF0123456789abcdef"}, ArgError::BadRejoin, "token must be lower case"},
        {{"--rejoin", "1:0123456789abcdefg123456789abcdef"}, ArgError::BadRejoin, "g is not hex"},
        {{"--rejoin", "1:2:0123456789abcdef0123456789abcd"}, ArgError::BadRejoin, "second colon lands in the token"},

        // --server / --stun 호스트
        {{"--server", ""}, ArgError::BadHost, "empty host"},
        {{"--server", "-bad.example"}, ArgError::BadHost, "label starts with a hyphen"},
        {{"--server", "bad-.example"}, ArgError::BadHost, "label ends with a hyphen"},
        {{"--server", "a..b"}, ArgError::BadHost, "empty label"},
        {{"--server", "a.b."}, ArgError::BadHost, "trailing dot"},
        {{"--server", "under_score.example"}, ArgError::BadHost, "underscore"},
        {{"--server", "sp ace.example"}, ArgError::BadHost, "space"},

        // 포트
        {{"--server", "a.example:0"}, ArgError::BadPort, "port 0 is outside 1-65535"},
        {{"--server", "a.example:65536"}, ArgError::BadPort, "port out of range"},
        {{"--server", "a.example:"}, ArgError::BadPort, "empty port"},
        {{"--server", "a.example:80x"}, ArgError::BadPort, "not decimal"},
        {{"--stun", "a.example"}, ArgError::MissingPort, "stun port cannot be omitted"},
        {{"--stun", "a.example:"}, ArgError::BadPort, "the port slot exists but is empty"},

        // 전부 숫자인 마지막 라벨. 리터럴로도 실패하고 이름으로도 풀 수 없다.
        {{"--server", "999.999.999.999"}, ArgError::BadHost, "looks like an address but is not one"},
        {{"--server", "1.2.3.4.5"}, ArgError::BadHost, "five dotted numbers"},
        {{"--server", "12345"}, ArgError::BadHost, "all-numeric single label"},
        {{"--server", "a.123"}, ArgError::BadHost, "all-numeric last label"},

        // --peer
        {{"--peer", "a.example:3478"}, ArgError::BadHost, "peer takes an IPv4 literal only"},
        {{"--peer", "192.0.2.1"}, ArgError::MissingPort, "no port"},
        {{"--peer", "192.0.2.1:0"}, ArgError::BadPort, "port 0 is outside 1-65535"},
        {{"--peer", "01.2.3.4:80"}, ArgError::BadHost, "leading zero octet"},
        {{"--peer", "192.0.2.1:80:90"}, ArgError::BadPort, "the address parses; the colon lands in the port"},
        {{"--peer", "[::1]:80"}, ArgError::BadHost, "IPv6 is out of scope"},
    };

    for (const auto& c : cases) {
        INFO("why=" << c.why << " first=[" << (c.argv.empty() ? "" : c.argv[0]) << "]");
        const auto r = parse_args(std::span<const std::string_view>(c.argv));
        REQUIRE_FALSE(r.ok());
        REQUIRE(r.error == c.error);
    }
}

TEST_CASE("args: error tokens are stable", "[args]") {
    // 로그가 이 문자열을 싣는다. 바꾸면 검증 항목이 같이 낡는다.
    REQUIRE(hamychi::to_token(ArgError::UnknownOption) == "unknown_option");
    REQUIRE(hamychi::to_token(ArgError::MissingValue) == "missing_value");
    REQUIRE(hamychi::to_token(ArgError::BadRole) == "bad_role");
    REQUIRE(hamychi::to_token(ArgError::ExtraPositional) == "extra_positional");
    REQUIRE(hamychi::to_token(ArgError::BadRoom) == "bad_room");
    REQUIRE(hamychi::to_token(ArgError::BadRejoin) == "bad_rejoin");
    REQUIRE(hamychi::to_token(ArgError::BadHost) == "bad_host");
    REQUIRE(hamychi::to_token(ArgError::BadPort) == "bad_port");
    REQUIRE(hamychi::to_token(ArgError::MissingPort) == "missing_port");
}

TEST_CASE("args: the offending argument is reported", "[args]") {
    const auto r = run({"--room", "ABCDE0"});
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.offending == "ABCDE0");
}

TEST_CASE("args: host length boundaries", "[args]") {
    // 변이 시험에서 라벨 길이 검사를 지워도 아무 케이스도 떨어지지 않아 채운 표다.
    const std::string label63(63, 'a');
    const std::string label64(64, 'a');

    {
        const std::vector<std::string_view> argv{"--server", label63};
        REQUIRE(parse_args(std::span<const std::string_view>(argv)).ok());
    }
    {
        const std::vector<std::string_view> argv{"--server", label64};
        const auto r = parse_args(std::span<const std::string_view>(argv));
        REQUIRE_FALSE(r.ok());
        REQUIRE(r.error == ArgError::BadHost);
    }

    // 전체 길이. 라벨 63자 셋에 점 둘이면 191자, 거기에 61자를 더해 253자를 만든다.
    const std::string host253 = label63 + "." + label63 + "." + label63 + "." + std::string(61, 'b');
    REQUIRE(host253.size() == 253);
    const std::string host254 = host253 + "b";
    {
        const std::vector<std::string_view> argv{"--server", host253};
        REQUIRE(parse_args(std::span<const std::string_view>(argv)).ok());
    }
    {
        const std::vector<std::string_view> argv{"--server", host254};
        const auto r = parse_args(std::span<const std::string_view>(argv));
        REQUIRE_FALSE(r.ok());
        REQUIRE(r.error == ArgError::BadHost);
    }
}

TEST_CASE("args: a dotted numeric host is accepted only as a literal", "[args]") {
    // is_valid_host 가 리터럴을 먼저 본다. 이 둘이 갈리는 자리다.
    const auto ok = run({"--server", "203.0.113.7"});
    REQUIRE(ok.ok());
    REQUIRE(ok.args->server->host == "203.0.113.7");

    const auto bad = run({"--server", "203.0.113.256"});
    REQUIRE_FALSE(bad.ok());
    REQUIRE(bad.error == ArgError::BadHost);
}
