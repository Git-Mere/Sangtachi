#include "sangtachi/network/endpoint.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <string_view>

// 시험 케이스 이름은 ASCII 로만 적는다. 이유는 network/wsa_test.cpp 머리에 있다.

using sangtachi::network::Endpoint;
using sangtachi::network::format_ipv4;
using sangtachi::network::parse_ipv4;
using sangtachi::network::parse_port;

namespace {

struct Ipv4Case {
    std::string_view text;
    bool accepted;
    std::uint32_t value;  // accepted 일 때만 본다
    std::string_view why;
};

// architecture.md 3.5 가 "반례 목록은 시험이 갖는다" 로 넘긴 표다.
constexpr Ipv4Case kIpv4Cases[] = {
    {"0.0.0.0",         true,  0x00000000u, "lowest"},
    {"255.255.255.255", true,  0xFFFFFFFFu, "highest"},
    {"10.100.0.1",      true,  0x0A640001u, "virtual network host"},
    {"1.2.3.0",         true,  0x01020300u, "single zero octet is fine"},
    {"127.0.0.1",       true,  0x7F000001u, "loopback"},

    {"1.2.3",           false, 0u, "three octets; inet_addr would accept this"},
    {"1.2.3.4.5",       false, 0u, "five octets"},
    {"1.2.3.",          false, 0u, "trailing dot"},
    {".1.2.3",          false, 0u, "leading dot"},
    {"1..2.3",          false, 0u, "empty octet"},
    {"01.2.3.4",        false, 0u, "leading zero; octal in some parsers"},
    {"1.2.3.04",        false, 0u, "leading zero in the last octet"},
    {"0.0.0.00",        false, 0u, "two-digit zero"},
    {"1.2.3.256",       false, 0u, "octet out of range"},
    {"1.2.3.999",       false, 0u, "octet far out of range"},
    {"1.2.3.1234",      false, 0u, "octet too long"},
    {" 1.2.3.4",        false, 0u, "leading space is not trimmed"},
    {"1.2.3.4 ",        false, 0u, "trailing space is not trimmed"},
    {"1.2.3.+4",        false, 0u, "sign"},
    {"1.2.3.-4",        false, 0u, "negative"},
    {"0x7f.0.0.1",      false, 0u, "hex; inet_addr would accept this"},
    {"1.2.3.4a",        false, 0u, "trailing garbage"},
    {"",                false, 0u, "empty"},
    {".",               false, 0u, "dot only"},
    {"1.2.3.4:5",       false, 0u, "port belongs to Endpoint, not to the address"},
};

struct PortCase {
    std::string_view text;
    bool accepted;
    std::uint16_t value;
    std::string_view why;
};

constexpr PortCase kPortCases[] = {
    {"0",      true,  0,     "the socket binds port 0; the argument layer rejects it"},
    {"1",      true,  1,     "lowest usable"},
    {"8000",   true,  8000,  "CONTROL_PORT"},
    {"65535",  true,  65535, "highest"},

    {"65536",  false, 0, "out of range"},
    {"99999",  false, 0, "out of range, five digits"},
    {"123456", false, 0, "too many digits"},
    {"00",     false, 0, "leading zero"},
    {"0080",   false, 0, "leading zero"},
    {"",       false, 0, "empty"},
    {" 1",     false, 0, "leading space"},
    {"1 ",     false, 0, "trailing space"},
    {"+1",     false, 0, "sign"},
    {"-1",     false, 0, "negative"},
    {"1e3",    false, 0, "not decimal digits"},
    {"8o00",   false, 0, "letter in the middle"},
};

}  // namespace

TEST_CASE("endpoint: parse_ipv4 case table", "[endpoint]") {
    for (const auto& c : kIpv4Cases) {
        INFO("input=[" << c.text << "] why=" << c.why);
        const auto got = parse_ipv4(c.text);
        REQUIRE(got.has_value() == c.accepted);
        if (c.accepted) {
            REQUIRE(*got == c.value);
        }
    }
}

TEST_CASE("endpoint: parse_port case table", "[endpoint]") {
    for (const auto& c : kPortCases) {
        INFO("input=[" << c.text << "] why=" << c.why);
        const auto got = parse_port(c.text);
        REQUIRE(got.has_value() == c.accepted);
        if (c.accepted) {
            REQUIRE(*got == c.value);
        }
    }
}

TEST_CASE("endpoint: format_ipv4 round-trips every accepted address", "[endpoint]") {
    for (const auto& c : kIpv4Cases) {
        if (!c.accepted) {
            continue;
        }
        INFO("input=[" << c.text << "]");
        REQUIRE(format_ipv4(c.value) == std::string(c.text));
    }
}

TEST_CASE("endpoint: parse accepts address and port together", "[endpoint]") {
    const auto ep = Endpoint::parse("10.100.0.1:25565");
    REQUIRE(ep.has_value());
    REQUIRE(ep->address() == 0x0A640001u);
    REQUIRE(ep->port() == 25565);
    REQUIRE(ep->to_string() == "10.100.0.1:25565");
}

TEST_CASE("endpoint: parse rejects malformed pairs", "[endpoint]") {
    struct Case {
        std::string_view text;
        std::string_view why;
    };
    const Case cases[] = {
        {"1.2.3.4",        "no colon"},
        {"1.2.3.4:",       "empty port"},
        {":5000",          "empty address"},
        {"1.2.3.4:5000:6", "two colons"},
        {"[::1]:80",       "IPv6 is out of scope"},
        {"1.2.3.4:65536",  "port out of range"},
        {"1.2.3:5000",     "three octets"},
        {"",               "empty"},
        {":",              "colon only"},
    };
    for (const auto& c : cases) {
        INFO("input=[" << c.text << "] why=" << c.why);
        REQUIRE_FALSE(Endpoint::parse(c.text).has_value());
    }
}

TEST_CASE("endpoint: parse keeps port zero", "[endpoint]") {
    // protocol.md 6장이 포트 0 으로 bind 한다. 이 타입은 표현이라 0 을 담는다.
    const auto ep = Endpoint::parse("0.0.0.0:0");
    REQUIRE(ep.has_value());
    REQUIRE(ep->port() == 0);
    REQUIRE(ep->to_string() == "0.0.0.0:0");
}

TEST_CASE("endpoint: comparison distinguishes address and port", "[endpoint]") {
    const Endpoint a(0x0A640001u, 25565);
    const Endpoint same(0x0A640001u, 25565);
    const Endpoint other_port(0x0A640001u, 25566);
    const Endpoint other_address(0x0A640002u, 25565);

    REQUIRE(a == same);
    REQUIRE_FALSE(a == other_port);
    REQUIRE_FALSE(a == other_address);
    REQUIRE(a != other_port);

    // 순서는 주소가 먼저, 같은 주소에서는 포트가 뒤다.
    REQUIRE(a < other_port);
    REQUIRE(a < other_address);
    REQUIRE(other_port < other_address);
}

TEST_CASE("endpoint: default is the unspecified endpoint", "[endpoint]") {
    const Endpoint e;
    REQUIRE(e.address() == 0u);
    REQUIRE(e.port() == 0);
    REQUIRE(e.to_string() == "0.0.0.0:0");
}
