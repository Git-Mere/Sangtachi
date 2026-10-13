#include "sangtachi/testbuild/instrument.hpp"

#include "../log_capture.hpp"
#include "sangtachi/hash.hpp"
#include "sangtachi/tunnel/data_sender.hpp"
#include "sangtachi/tunnel/tunnel.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// 시험 케이스 이름은 ASCII 로만 적는다. 이유는 network/wsa_test.cpp 머리에 있다.
//
// 시험 빌드 장치 (ADR 0023). roadmap.md Phase 4 의 RTT 기준선 에코와 M-5 의 data.tx/data.rx.

using namespace sangtachi;

namespace {

constexpr std::uint32_t kA = 0x0A640002u;
constexpr std::uint32_t kB = 0x0A640001u;

}  // namespace

TEST_CASE("echo: a request comes back swapped, marked 0x5A and with a valid checksum",
          "[testbuild]") {
    const auto request = tunnel::make_synthetic(100, kA, kB, 4, 77);
    const auto reply = testbuild::make_echo_reply(request);
    REQUIRE(reply.has_value());
    REQUIRE(reply->size() == request.size());
    REQUIRE(tunnel::check_inner_ipv4(*reply, kB, kA) == tunnel::InnerError::kNone);
    REQUIRE((*reply)[40] == std::byte{0x5A});
    // 그 밖의 바이트는 그대로다 (송신 시각과 번호 포함)
    for (std::size_t i = 20; i < request.size(); ++i) {
        if (i != 40) {
            REQUIRE((*reply)[i] == request[i]);
        }
    }
}

TEST_CASE("echo: no reply to an echo, to a short packet or to an unknown mark", "[testbuild]") {
    const auto request = tunnel::make_synthetic(100, kA, kB, 0, 0);
    const auto reply = testbuild::make_echo_reply(request);
    REQUIRE(reply.has_value());
    REQUIRE_FALSE(testbuild::make_echo_reply(*reply).has_value());  // 0x5A 에 응답하지 않는다
    REQUIRE_FALSE(testbuild::make_echo_reply(tunnel::make_synthetic(20, kA, kB, 0, 0)).has_value());
    REQUIRE_FALSE(testbuild::make_echo_reply(tunnel::make_synthetic(40, kA, kB, 0, 0)).has_value());
    REQUIRE(testbuild::make_echo_reply(tunnel::make_synthetic(41, kA, kB, 0, 0)).has_value());
    auto odd = request;
    odd[40] = std::byte{0x00};
    REQUIRE_FALSE(testbuild::make_echo_reply(odd).has_value());
    // 옵션이 있는 헤더(IHL 6)는 합성 레이아웃이 아니다. 오프셋 40 이 표시라는 보장이 없다
    auto with_options = request;
    with_options[0] = std::byte{0x46};
    REQUIRE(with_options[40] == std::byte{0xA5});
    REQUIRE_FALSE(testbuild::make_echo_reply(with_options).has_value());
}

TEST_CASE("data.tx and data.rx carry the peer, the length and the sha256 of the bytes",
          "[testbuild]") {
    const auto p = tunnel::make_synthetic(100, kA, kB, 1, 2);
    const std::string hash = *sha256_hex(p);
    testing::LogCapture logs;
    testbuild::emit_data_tx(p);
    testbuild::emit_data_rx(p);
    const auto tx = logs.with_event("data.tx");
    const auto rx = logs.with_event("data.rx");
    REQUIRE(tx.size() == 1);
    REQUIRE(rx.size() == 1);
    REQUIRE(tx[0].find(" to=10.100.0.1 ") != std::string::npos);
    REQUIRE(rx[0].find(" from=10.100.0.2 ") != std::string::npos);
    for (const auto& line : {tx[0], rx[0]}) {
        REQUIRE(line.find(" len=100 ") != std::string::npos);
        REQUIRE(line.find(" sha256=" + hash) != std::string::npos);
    }
}
