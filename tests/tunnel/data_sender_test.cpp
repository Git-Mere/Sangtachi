#include "sangtachi/tunnel/data_sender.hpp"

#include "sangtachi/tunnel/tunnel.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

// 시험 케이스 이름은 ASCII 로만 적는다. 이유는 network/wsa_test.cpp 머리에 있다.
//
// roadmap.md Phase 4 의 100바이트 레이아웃 표와 Phase 5 크기 단서, `send <n> [size] [to]` 의 인자.
// 명령이 터널을 거쳐 보내는 동작은 tunnel/tunnel_test.cpp 가 본다.

using namespace sangtachi;
using tunnel::make_synthetic;
using tunnel::parse_send_args;

namespace {

constexpr std::uint32_t kSrc = 0x0A640002u;
constexpr std::uint32_t kDst = 0x0A640001u;

std::uint64_t be(const std::vector<std::byte>& p, std::size_t at, std::size_t width) {
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < width; ++i) {
        v = (v << 8) | std::to_integer<std::uint64_t>(p[at + i]);
    }
    return v;
}

}  // namespace

TEST_CASE("synthetic: the 100-byte layout matches the roadmap table", "[tunnel][send]") {
    const auto p = make_synthetic(100, kSrc, kDst, 7, 0x0102030405060708ull);
    REQUIRE(p.size() == 100);
    REQUIRE(be(p, 0, 1) == 0x45);
    REQUIRE(be(p, 1, 1) == 0x00);
    REQUIRE(be(p, 2, 2) == 100);
    REQUIRE(be(p, 4, 2) == 0);
    REQUIRE(be(p, 6, 2) == 0);
    REQUIRE(be(p, 8, 1) == 64);
    REQUIRE(be(p, 9, 1) == 17);
    REQUIRE(be(p, 12, 4) == kSrc);
    REQUIRE(be(p, 16, 4) == kDst);
    REQUIRE(be(p, 20, 2) == 25565);
    REQUIRE(be(p, 22, 2) == 25565);
    REQUIRE(be(p, 24, 2) == 80);
    REQUIRE(be(p, 26, 2) == 0);
    REQUIRE(be(p, 28, 4) == 7);
    REQUIRE(be(p, 32, 8) == 0x0102030405060708ull);
    REQUIRE(be(p, 40, 1) == 0xA5);
    for (std::size_t i = 41; i < 100; ++i) {
        REQUIRE(be(p, i, 1) == 0xA5);
    }
    // 체크섬과 길이는 8.4 가 보는 그대로 맞는다
    REQUIRE(tunnel::check_inner_ipv4(p, kSrc, kDst) == tunnel::InnerError::kNone);
}

TEST_CASE("synthetic: other sizes cut or pad the same layout", "[tunnel][send]") {
    SECTION("20 bytes is the IPv4 header only") {
        const auto p = make_synthetic(20, kSrc, kDst, 0, 0);
        REQUIRE(p.size() == 20);
        REQUIRE(be(p, 2, 2) == 20);
        REQUIRE(tunnel::check_inner_ipv4(p, kSrc, kDst) == tunnel::InnerError::kNone);
    }
    for (const std::size_t size : {std::size_t{576}, std::size_t{1452}}) {
        const auto p = make_synthetic(size, kSrc, kDst, 3, 9);
        REQUIRE(p.size() == size);
        REQUIRE(be(p, 2, 2) == size);
        REQUIRE(be(p, 24, 2) == size - 20);
        REQUIRE(be(p, 40, 1) == 0xA5);
        REQUIRE(be(p, size - 1, 1) == 0xA5);
        REQUIRE(tunnel::check_inner_ipv4(p, kSrc, kDst) == tunnel::InnerError::kNone);
    }
}

TEST_CASE("send args: the case table", "[tunnel][send]") {
    struct Case {
        std::string_view rest;
        bool ok;
        std::uint32_t count;
        std::size_t size;
        std::optional<std::uint32_t> to;
    };
    const Case cases[] = {
        {" 5", true, 5, 100, std::nullopt},
        {" 1000", true, 1000, 100, std::nullopt},
        {" 5 576", true, 5, 576, std::nullopt},
        {" 5 20", true, 5, 20, std::nullopt},
        {" 5 1452", true, 5, 1452, std::nullopt},
        {" 5 10.100.0.3", true, 5, 100, 0x0A640003u},
        {" 5  576   10.100.0.3", true, 5, 576, 0x0A640003u},
        {"", false, 0, 0, std::nullopt},
        {" 0", false, 0, 0, std::nullopt},
        {" 1001", false, 0, 0, std::nullopt},
        {" x", false, 0, 0, std::nullopt},
        {" -1", false, 0, 0, std::nullopt},
        {" 5 19", false, 0, 0, std::nullopt},
        {" 5 1453", false, 0, 0, std::nullopt},
        {" 5 10.100.0", false, 0, 0, std::nullopt},
        {" 5 10.100.0.3 576", false, 0, 0, std::nullopt},
        {" 5 576 10.100.0.3 x", false, 0, 0, std::nullopt},
    };
    for (const auto& c : cases) {
        INFO("rest=[" << c.rest << "]");
        const auto parsed = parse_send_args(c.rest);
        REQUIRE(parsed.has_value() == c.ok);
        if (c.ok) {
            REQUIRE(parsed->count == c.count);
            REQUIRE(parsed->size == c.size);
            REQUIRE(parsed->to == c.to);
        }
    }
}
