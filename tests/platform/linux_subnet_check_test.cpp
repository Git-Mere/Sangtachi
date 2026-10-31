#include "../../client/src/platform/linux/subnet_check.hpp"

#include "sangtachi/network/endpoint.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

// 시험 케이스 이름은 ASCII 로만 적는다. 이유는 network/wsa_test.cpp 머리에 있다.
//
// 판정표는 windows-prereq.md 7절, 순서는 tools/winprereq/Test-SubnetOverlap.ps1 의 Test-PrefixOverlap 이다.
// 대상은 제품이 넘기는 10.100.0.0/24 다.

using sangtachi::platform::OsFailure;
using sangtachi::platform::SubnetVerdict;
using sangtachi::platform::subnet::classify;
using sangtachi::platform::subnet::judge;
using sangtachi::platform::subnet::Overlap;
using sangtachi::platform::subnet::parse_target;
using sangtachi::platform::subnet::Prefix;

namespace {

Prefix prefix(const char* address, std::uint8_t length) {
    return Prefix{*sangtachi::network::parse_ipv4(address), length};
}

std::uint32_t ip(const char* address) {
    return *sangtachi::network::parse_ipv4(address);
}

const Prefix kTarget = prefix("10.100.0.0", 24);

}  // namespace

TEST_CASE("subnet_check: classify follows the table row by row", "[platform][subnet]") {
    struct Row {
        const char* address;
        std::uint8_t length;
        Overlap expected;
    };
    const Row rows[] = {
        // 겹치지 않음
        {"192.168.1.0", 24, Overlap::kIgnore},
        {"10.100.1.0", 24, Overlap::kIgnore},      // 바로 옆 /24
        {"10.99.255.255", 32, Overlap::kIgnore},   // 대상 바로 앞 주소
        {"10.100.1.0", 32, Overlap::kIgnore},      // 대상 바로 뒤 주소
        {"128.0.0.0", 1, Overlap::kIgnore},        // /1 이라도 겹치지 않으면 무시
        // 같은 길이 이상 = BLOCK-route
        {"10.100.0.0", 24, Overlap::kBlockRoute},
        {"10.100.0.128", 25, Overlap::kBlockRoute},
        {"10.100.0.0", 32, Overlap::kBlockRoute},  // 대상 첫 주소
        {"10.100.0.255", 32, Overlap::kBlockRoute},  // 대상 끝 주소
        // 길이 0~1 = catch-all
        {"0.0.0.0", 0, Overlap::kCatchAll},
        {"0.0.0.0", 1, Overlap::kCatchAll},
        // 길이 2 ~ 대상-1 = BLOCK-shadow
        {"0.0.0.0", 2, Overlap::kBlockShadow},     // 아래 경계
        {"10.0.0.0", 8, Overlap::kBlockShadow},
        {"10.100.0.0", 23, Overlap::kBlockShadow},  // 위 경계
    };
    for (const Row& row : rows) {
        INFO(row.address << "/" << static_cast<int>(row.length));
        CHECK(classify(prefix(row.address, row.length), kTarget) == row.expected);
    }
}

TEST_CASE("subnet_check: a same-length overlap wins over catch-all", "[platform][subnet]") {
    // 순서가 판정이다. 대상이 /1 이면 같은 /1 경로는 catch-all 이 아니라 BLOCK-route 다.
    CHECK(classify(Prefix{0, 1}, Prefix{0, 1}) == Overlap::kBlockRoute);
    CHECK(classify(Prefix{0, 0}, Prefix{0, 1}) == Overlap::kCatchAll);
}

TEST_CASE("subnet_check: target parsing rejects what the script rejects", "[platform][subnet]") {
    const auto ok = parse_target("10.100.0.0/24");
    REQUIRE(ok.has_value());
    CHECK(ok->network == ip("10.100.0.0"));
    CHECK(ok->length == 24);
    CHECK(parse_target("64.0.0.0/2").has_value());  // 길이 아래 경계
    CHECK_FALSE(parse_target("10.0.0.0/2").has_value());  // /2 에서는 10 이 호스트 비트다
    CHECK(parse_target("10.100.0.1/32").has_value());
    CHECK_FALSE(parse_target("10.100.0.1/24").has_value());  // 호스트 비트
    CHECK_FALSE(parse_target("0.0.0.0/1").has_value());      // 길이 2 미만
    CHECK_FALSE(parse_target("0.0.0.0/0").has_value());
    CHECK_FALSE(parse_target("10.100.0.0/33").has_value());
    CHECK_FALSE(parse_target("10.100.0.0/024").has_value());
    CHECK_FALSE(parse_target("10.100.0.0/").has_value());
    CHECK_FALSE(parse_target("10.100.0.0").has_value());
    CHECK_FALSE(parse_target("10.100.0/24").has_value());
}

TEST_CASE("subnet_check: verdict and exit code", "[platform][subnet]") {
    const std::vector<Prefix> none;
    const std::vector<std::uint32_t> no_addresses;

    SECTION("nothing overlaps -> proceed") {
        const auto r = judge("10.100.0.0/24", {prefix("0.0.0.0", 0), prefix("192.168.0.0", 24)},
                             {ip("192.168.0.10"), ip("127.0.0.1")}, {});
        CHECK(r.verdict == SubnetVerdict::kProceed);
        CHECK(r.exit_code == 0);
        CHECK(r.blocks.empty());
        CHECK(r.failure.op.empty());
    }
    SECTION("catch-all alone is not a conflict") {
        const auto r = judge("10.100.0.0/24", {prefix("0.0.0.0", 0), prefix("0.0.0.0", 1)}, no_addresses, {});
        CHECK(r.verdict == SubnetVerdict::kProceed);
    }
    SECTION("route and shadow blocks are listed in route order") {
        const auto r = judge("10.100.0.0/24",
                             {prefix("10.0.0.0", 8), prefix("172.16.0.0", 12), prefix("10.100.0.0", 24)},
                             no_addresses, {});
        CHECK(r.verdict == SubnetVerdict::kBlock);
        CHECK(r.exit_code == 1);
        CHECK(r.blocks == std::vector<std::string>{"10.0.0.0/8", "10.100.0.0/24"});
    }
    SECTION("an address inside the target blocks but is not listed") {
        const auto r = judge("10.100.0.0/24", none, {ip("10.100.0.7")}, {});
        CHECK(r.verdict == SubnetVerdict::kBlock);
        CHECK(r.exit_code == 1);
        CHECK(r.blocks.empty());
    }
    SECTION("an address just outside the target does not block") {
        const auto r = judge("10.100.0.0/24", none, {ip("10.100.1.0"), ip("10.99.255.255")}, {});
        CHECK(r.verdict == SubnetVerdict::kProceed);
    }
    SECTION("a failed query without a block is unknown") {
        const auto r = judge("10.100.0.0/24", none, no_addresses, OsFailure{"RTM_GETROUTE", 13});
        CHECK(r.verdict == SubnetVerdict::kUnknown);
        CHECK(r.exit_code == 2);
        CHECK(r.failure.op == "RTM_GETROUTE");
        CHECK(r.failure.code == 13);
    }
    SECTION("a block wins over a failed query") {
        const auto r = judge("10.100.0.0/24", none, {ip("10.100.0.1")}, OsFailure{"RTM_GETROUTE", 13});
        CHECK(r.verdict == SubnetVerdict::kBlock);
        CHECK(r.exit_code == 1);
    }
    SECTION("a prefix length the table cannot read is unknown") {
        const auto r = judge("10.100.0.0/24", {Prefix{ip("10.100.0.0"), 33}}, no_addresses, {});
        CHECK(r.verdict == SubnetVerdict::kUnknown);
        CHECK(r.exit_code == 2);
    }
    SECTION("an invalid target is unknown") {
        const auto r = judge("10.100.0.1/24", none, no_addresses, {});
        CHECK(r.verdict == SubnetVerdict::kUnknown);
        CHECK(r.exit_code == 2);
    }
}
