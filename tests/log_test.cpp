#include "sangtachi/log.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <span>
#include <string>
#include <string_view>

// 시험 케이스 이름은 ASCII 로만 적는다. 이유는 network/wsa_test.cpp 머리에 있다.

using sangtachi::field;
using sangtachi::format_line;
using sangtachi::LogField;
using sangtachi::LogLevel;
using sangtachi::sanitize_value;
using sangtachi::to_token;

TEST_CASE("log: level tokens are the three the document fixes", "[log]") {
    REQUIRE(to_token(LogLevel::Info) == "INFO");
    REQUIRE(to_token(LogLevel::Warn) == "WARN");
    REQUIRE(to_token(LogLevel::Error) == "ERROR");
}

TEST_CASE("log: a line is level, event, then name=value pairs", "[log]") {
    const LogField fields[] = {
        field("local", std::string_view("0.0.0.0:51234")),
        field("rcvbuf_requested", std::uint64_t{262144}),
        field("rcvbuf_applied", std::uint64_t{262144}),
    };
    REQUIRE(format_line(LogLevel::Info, "socket.bind", fields) ==
            "INFO socket.bind local=0.0.0.0:51234 rcvbuf_requested=262144 rcvbuf_applied=262144");
}

TEST_CASE("log: a line with no fields is level and event only", "[log]") {
    REQUIRE(format_line(LogLevel::Warn, "stun.result", std::span<const LogField>()) ==
            "WARN stun.result");
}

TEST_CASE("log: byte counts carry no unit suffix", "[log]") {
    // architecture.md 9장: rcvbuf_* 는 10진 바이트 수다. 262144 이지 256KB 가 아니다.
    const auto f = field("rcvbuf_requested", std::uint64_t{262144});
    REQUIRE(f.value == "262144");
}

TEST_CASE("log: sanitize replaces spaces and control characters", "[log]") {
    struct Case {
        std::string_view input;
        std::string_view expected;
        std::string_view why;
    };
    const Case cases[] = {
        {"plain", "plain", "nothing to do"},
        {"a b", "a_b", "a space would hide the name=value boundary"},
        {"a\tb", "a_b", "tab"},
        {"a\nb", "a_b", "newline would split the line in two"},
        {"a\r\nb", "a__b", "each byte is replaced, not each sequence"},
        {"a\x7F" "b", "a_b", "DEL"},          // 16진 이스케이프가 뒤 글자를 먹지 않게 끊는다
        {"a\x01" "b", "a_b", "control character"},
        {"  ", "__", "only spaces"},
        {"", "", "empty stays empty"},
        {"a=b", "a=b", "equals is not touched; the reader splits on the first one"},
        {"a\"b", "a\"b", "quotes are not escaped and not removed"},
        {"a\\b", "a\\b", "backslash is not escaped"},
    };
    for (const auto& c : cases) {
        INFO("input=[" << c.input << "] why=" << c.why);
        REQUIRE(sanitize_value(c.input) == std::string(c.expected));
    }
}

TEST_CASE("log: sanitize keeps bytes above ASCII", "[log]") {
    // UTF-8 의 이어지는 바이트는 줄을 끊지도 경계를 흐리지도 않는다.
    const std::string korean = "\xED\x95\x9C";  // U+D55C
    REQUIRE(sanitize_value(korean) == korean);
}

TEST_CASE("log: a sanitized value cannot break the line shape", "[log]") {
    // 이 케이스가 9장의 "한 줄에 이벤트 하나" 를 지킨다.
    const LogField fields[] = {field("arg", std::string_view("a b\nc"))};
    const std::string line = format_line(LogLevel::Error, "args.invalid", fields);
    REQUIRE(line == "ERROR args.invalid arg=a_b_c");
    REQUIRE(line.find('\n') == std::string::npos);
    // 공백으로 잘랐을 때 조각이 셋이다. 수준, 이벤트키, 그리고 필드 하나.
    std::size_t spaces = 0;
    for (const char ch : line) {
        if (ch == ' ') {
            ++spaces;
        }
    }
    REQUIRE(spaces == 2);
}

TEST_CASE("log: the guarantee is byte level, not unicode level", "[log]") {
    // 리뷰가 짚은 자리다. 9장이 그 한계를 적고 이 케이스가 그것을 고정한다.
    const std::string line_separator = "a\xE2\x80\xA8" "b";  // U+2028
    const std::string got = sanitize_value(line_separator);

    // 0x80 이상은 그대로 둔다. 바꾸지 않는다는 것을 케이스로 못박는다.
    REQUIRE(got == line_separator);

    // 그래도 바이트 단위 계약은 성립한다. 줄을 끊는 0x0A 도, 경계를 흐리는 0x20 도 없다.
    REQUIRE(got.find('\n') == std::string::npos);
    REQUIRE(got.find('\r') == std::string::npos);
    REQUIRE(got.find(' ') == std::string::npos);
}
