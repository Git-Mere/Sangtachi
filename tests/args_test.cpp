#include "sangtachi/args.hpp"

#include <catch2/catch_test_macros.hpp>

#include <span>
#include <string>
#include <string_view>
#include <vector>

// 시험 케이스 이름은 ASCII 로만 적는다. 이유는 network/wsa_test.cpp 머리에 있다.
//
// architecture.md 3.5 가 "반례 목록은 시험이 갖는다" 로 넘긴 표다.

using sangtachi::ArgError;
using sangtachi::parse_args;
using sangtachi::ParseResult;
using sangtachi::Role;

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

TEST_CASE("args: only the server is needed and there is no role", "[args]") {
    // 역할은 어느 구간에서나 없어도 기동한다. 그때는 로비에서 시작한다 (architecture.md 3.5).
    // --server 는 Phase 3 이후 필수다.
    const auto r = run({"--server", "a.example", "--name", "Alice"});
    REQUIRE(r.ok());
    REQUIRE(r.args->role == Role::None);
    REQUIRE(r.args->server.has_value());
    REQUIRE(r.args->stun.empty());
}

TEST_CASE("args: role positional", "[args]") {
    REQUIRE(run({"host", "--server", "a.example", "--name", "Alice"}).args->role == Role::Host);
    REQUIRE(run({"player", "--server", "a.example", "--room", "ABCDEF", "--name", "Alice"}).args->role == Role::Player);
}

TEST_CASE("args: server takes the control port by default", "[args]") {
    const auto r = run({"--server", "control.example.com", "--name", "Alice"});
    REQUIRE(r.ok());
    REQUIRE(r.args->server->host == "control.example.com");
    REQUIRE(r.args->server->port == 8000);  // control_plane.md 2.6 CONTROL_PORT
}

TEST_CASE("args: server keeps an explicit port", "[args]") {
    const auto r = run({"--server", "203.0.113.7:9000", "--name", "Alice"});
    REQUIRE(r.ok());
    REQUIRE(r.args->server->host == "203.0.113.7");
    REQUIRE(r.args->server->port == 9000);
}

TEST_CASE("args: room is normalized to upper case", "[args]") {
    const auto r = run({"player", "--server", "a.example", "--room", "abcdef", "--name", "Alice"});
    REQUIRE(r.ok());
    REQUIRE(*r.args->room == "ABCDEF");
    REQUIRE(*run({"player", "--server", "a.example", "--room", "aBcDeF", "--name", "Alice"}).args->room == "ABCDEF");
    REQUIRE(*run({"player", "--server", "a.example", "--room", "ABCDEF", "--name", "Alice"}).args->room == "ABCDEF");
}

TEST_CASE("args: room is rejected for the host role", "[args]") {
    // architecture.md 3.5. 방 코드는 create_room 응답으로만 생긴다.
    const auto r = run({"host", "--server", "a.example", "--room", "ABCDEF"});
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.error == ArgError::RoomWithHost);
    REQUIRE(r.offending == "--room");

    // player 는 그대로 받는다.
    REQUIRE(run({"player", "--server", "a.example", "--room", "ABCDEF", "--name", "Alice"}).ok());
}

TEST_CASE("args: stun accumulates and the rest is last wins", "[args]") {
    const auto r = run({"player", "--server", "a.example", "--name", "Alice",
                        "--stun", "a.example:3478",
                        "--stun", "b.example:19302",
                        "--room", "ABCDEF",
                        "--room", "GHJKLM"});
    REQUIRE(r.ok());
    REQUIRE(r.args->stun.size() == 2);
    REQUIRE(r.args->stun[0].host == "a.example");
    REQUIRE(r.args->stun[0].port == 3478);
    REQUIRE(r.args->stun[1].port == 19302);
    REQUIRE(*r.args->room == "GHJKLM");
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
        {{"--peer", "192.0.2.1:1"}, ArgError::UnknownOption, "the Phase 1 test input is gone (ADR 0028)"},

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
    REQUIRE(sangtachi::to_token(ArgError::UnknownOption) == "unknown_option");
    REQUIRE(sangtachi::to_token(ArgError::MissingValue) == "missing_value");
    REQUIRE(sangtachi::to_token(ArgError::BadRole) == "bad_role");
    REQUIRE(sangtachi::to_token(ArgError::ExtraPositional) == "extra_positional");
    REQUIRE(sangtachi::to_token(ArgError::BadRoom) == "bad_room");
    REQUIRE(sangtachi::to_token(ArgError::RoomWithHost) == "room_with_host");
    REQUIRE(sangtachi::to_token(ArgError::BadHost) == "bad_host");
    REQUIRE(sangtachi::to_token(ArgError::BadPort) == "bad_port");
    REQUIRE(sangtachi::to_token(ArgError::MissingPort) == "missing_port");
    REQUIRE(sangtachi::to_token(ArgError::MissingServer) == "missing_server");
    REQUIRE(sangtachi::to_token(ArgError::MissingRoom) == "missing_room");
    REQUIRE(sangtachi::to_token(ArgError::RoomWithoutRole) == "room_without_role");
    REQUIRE(sangtachi::to_token(ArgError::BadName) == "bad_name");
    REQUIRE(sangtachi::to_token(ArgError::MissingName) == "missing_name");
}

TEST_CASE("args: required inputs from Phase 3 case table", "[args]") {
    // architecture.md 3.5 의 "필수" 열, Phase 3 이후. 형식 검사는 이보다 먼저다.
    const RejectCase cases[] = {
        {{}, ArgError::MissingServer, "no arguments: the server is required"},
        {{"host"}, ArgError::MissingServer, "host without a server"},
        {{"player", "--server", "a.example"}, ArgError::MissingRoom, "player needs a room"},
        {{"player"}, ArgError::MissingRoom, "player without room or server reports the room"},
        {{"--room", "ABCDEF", "--server", "a.example"}, ArgError::RoomWithoutRole, "room without a role"},
        {{"--room", "ABCDEF"}, ArgError::RoomWithoutRole, "room without a role or server"},
        {{"host", "--room", "ABCDEF"}, ArgError::RoomWithHost, "host with a room is still its own error"},
        {{"--room", "ABCDE0"}, ArgError::BadRoom, "format errors come before required checks"},
    };
    for (const auto& c : cases) {
        INFO("why=" << c.why);
        const auto r = parse_args(std::span<const std::string_view>(c.argv));
        REQUIRE_FALSE(r.ok());
        REQUIRE(r.error == c.error);
    }

    // 필수 입력이 다 있으면 통과한다. --name 은 Phase 4 이후 필수다.
    REQUIRE(run({"--server", "a.example", "--name", "Alice"}).ok());
    REQUIRE(run({"host", "--server", "a.example", "--name", "Alice"}).ok());
    REQUIRE(run({"player", "--server", "a.example", "--room", "ABCDEF", "--name", "Alice"}).ok());
}

TEST_CASE("args: the missing input is named in offending", "[args]") {
    REQUIRE(run({}).offending == "--server");
    REQUIRE(run({"player", "--server", "a.example"}).offending == "--room");
    REQUIRE(run({"--server", "a.example", "--room", "ABCDEF"}).offending == "--room");
    REQUIRE(run({"--server", "a.example"}).offending == "--name");
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
        const std::vector<std::string_view> argv{"--server", label63, "--name", "Alice"};
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
        const std::vector<std::string_view> argv{"--server", host253, "--name", "Alice"};
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
    const auto ok = run({"--server", "203.0.113.7", "--name", "Alice"});
    REQUIRE(ok.ok());
    REQUIRE(ok.args->server->host == "203.0.113.7");

    const auto bad = run({"--server", "203.0.113.256"});
    REQUIRE_FALSE(bad.ok());
    REQUIRE(bad.error == ArgError::BadHost);
}

// ---------------------------------------------------------------- --name (architecture.md 3.5, control_plane.md 2.7)
//
// 형식 판정은 tunnel::is_valid_name 이 한다 (control_plane.md 2.7). 그 함수의 케이스 표는
// tunnel/packet_test.cpp 에 있다. 여기 표는 인자가 그 판정을 실제로 거치는지와 오류 토큰을 본다.

TEST_CASE("args: name keeps the case it was given", "[args]") {
    // 2.7: 대소문자는 등록한 그대로다. 바꾸지 않는다.
    const auto r = run({"--server", "a.example", "--name", "Alice_01"});
    REQUIRE(r.ok());
    REQUIRE(r.args->name.has_value());
    REQUIRE(*r.args->name == "Alice_01");
}

TEST_CASE("args: name accept table", "[args]") {
    const std::string sixteen(16, 'z');
    const std::vector<std::string_view> names = {"a", "Z", "_", "0", "abcdefghijklmnop", sixteen,
                                                 "ABCDEFGHIJKLMNOP", "a_b_C_9"};
    for (const auto name : names) {
        INFO("name=[" << name << "]");
        const std::vector<std::string_view> argv{"--server", "a.example", "--name", name};
        const auto r = parse_args(std::span<const std::string_view>(argv));
        REQUIRE(r.ok());
        REQUIRE(*r.args->name == name);
    }
}

TEST_CASE("args: name reject table", "[args]") {
    const std::string seventeen(17, 'z');
    struct Case {
        std::string_view name;
        std::string_view why;
    };
    const Case cases[] = {
        {"", "2.7: shorter than one"},
        {seventeen, "2.7: longer than MAX_NAME_LEN"},
        {"a b", "2.7: space"},
        {" Alice", "2.7: leading space is not trimmed"},
        {"Alice ", "2.7: trailing space is not trimmed"},
        {"a\tb", "2.7: tab"},
        {"Al-ice", "2.7: hyphen"},
        {"a.b", "2.7: dot"},
        {"-", "the FAIL line placeholder is not a name"},
        {"\xED\x95\x9C", "2.7: hangul (UTF-8)"},
        {"\xEF\xBC\xA1", "2.7: fullwidth A"},
        {"\xC3\xA9", "2.7: latin small e with acute"},
        {std::string_view("a\0b", 3), "embedded NUL"},
    };
    for (const auto& c : cases) {
        INFO("why=" << c.why);
        const std::vector<std::string_view> argv{"--server", "a.example", "--name", c.name};
        const auto r = parse_args(std::span<const std::string_view>(argv));
        REQUIRE_FALSE(r.ok());
        REQUIRE(r.error == ArgError::BadName);
        REQUIRE(r.offending == c.name);
    }
}

TEST_CASE("args: name is required from Phase 4", "[args]") {
    // architecture.md 3.5 의 "필수" 열. --name 은 Phase 4 이후 예다. 앞의 필수 판정과 형식 검사가 먼저다.
    const RejectCase cases[] = {
        {{"--server", "a.example"}, ArgError::MissingName, "no role, no name"},
        {{"host", "--server", "a.example"}, ArgError::MissingName, "host without a name"},
        {{"player", "--server", "a.example", "--room", "ABCDEF"}, ArgError::MissingName, "player without a name"},
        {{"--name", "Alice"}, ArgError::MissingServer, "a name does not replace the server"},
        {{}, ArgError::MissingServer, "the server check stays first"},
        {{"player", "--server", "a.example", "--name", "Alice"}, ArgError::MissingRoom, "the room check stays before"},
        {{"--server", "a.example", "--name"}, ArgError::MissingValue, "option at the end without a value"},
        {{"--name", "a b"}, ArgError::BadName, "format errors come before required checks"},
        {{"--server", "a.example", "--name", "Alice", "--name", "a-b"}, ArgError::BadName,
         "every given value is checked, even one a later value would replace"},
    };
    for (const auto& c : cases) {
        INFO("why=" << c.why);
        const auto r = parse_args(std::span<const std::string_view>(c.argv));
        REQUIRE_FALSE(r.ok());
        REQUIRE(r.error == c.error);
    }
}

TEST_CASE("args: name is last wins", "[args]") {
    const auto r = run({"--server", "a.example", "--name", "Alice", "--name", "Bob"});
    REQUIRE(r.ok());
    REQUIRE(*r.args->name == "Bob");
}

TEST_CASE("args: --no-adapter is a switch without a value", "[args]") {
    // architecture.md 3.5 의 "어댑터 없이". 기본은 어댑터를 만든다.
    REQUIRE_FALSE(run({"--server", "a.example", "--name", "Alice"}).args->no_adapter);

    const auto first = run({"--no-adapter", "--server", "a.example", "--name", "Alice"});
    REQUIRE(first.ok());
    REQUIRE(first.args->no_adapter);

    const auto last = run({"host", "--server", "a.example", "--name", "Alice", "--no-adapter"});
    REQUIRE(last.ok());
    REQUIRE(last.args->no_adapter);
    REQUIRE(last.args->role == Role::Host);

    // 값을 붙인 꼴은 받지 않는다
    const auto glued = run({"--no-adapter=1", "--server", "a.example", "--name", "Alice"});
    REQUIRE(glued.error == ArgError::UnknownOption);
    // 뒤의 낱말은 값이 아니라 위치 인자다
    const auto valued = run({"--no-adapter", "yes", "--server", "a.example", "--name", "Alice"});
    REQUIRE(valued.error == ArgError::BadRole);
}
