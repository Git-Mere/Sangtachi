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

TEST_CASE("data.echo gives the round trip of an echoed request from the send time at offset 32",
          "[testbuild]") {
    // roadmap.md Phase 4 RTT 기준선. 요청은 내지 않고, 에코만 그 송신 시각으로 잰다.
    const auto request = tunnel::make_synthetic(100, kA, kB, 3, 1000);
    const auto echo = testbuild::make_echo_reply(request);
    REQUIRE(echo.has_value());
    testing::LogCapture logs;
    testbuild::emit_echo_rtt(request, 1500);
    REQUIRE(logs.with_event("data.echo").empty());
    testbuild::emit_echo_rtt(*echo, 1023);
    auto lines = logs.with_event("data.echo");
    REQUIRE(lines.size() == 1);
    REQUIRE(lines[0].find(" rtt_ms=23") != std::string::npos);
    REQUIRE(lines[0].find(" rtt_ms=230") == std::string::npos);
    // 시각이 지금보다 뒤면(다른 시계) 내지 않는다. 짧은 패킷과 IHL 6 도 내지 않는다
    testbuild::emit_echo_rtt(*echo, 999);
    auto short_echo = *echo;
    short_echo.resize(40);
    testbuild::emit_echo_rtt(short_echo, 2000);
    auto with_options = *echo;
    with_options[0] = std::byte{0x46};
    testbuild::emit_echo_rtt(with_options, 2000);
    REQUIRE(logs.with_event("data.echo").size() == 1);
}

namespace {

std::uint16_t sum16(const std::vector<std::byte>& p, std::size_t from, std::size_t to) {
    std::uint32_t sum = 0;
    for (std::size_t i = from; i < to; i += 2) {
        const std::uint32_t hi = std::to_integer<std::uint32_t>(p[i]) << 8;
        const std::uint32_t lo = i + 1 < to ? std::to_integer<std::uint32_t>(p[i + 1]) : 0;
        sum += hi | lo;
    }
    while ((sum >> 16) != 0) {
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }
    return static_cast<std::uint16_t>(sum);
}

void put16(std::vector<std::byte>& p, std::size_t at, std::uint16_t v) {
    p[at] = static_cast<std::byte>(v >> 8);
    p[at + 1] = static_cast<std::byte>(v & 0xFF);
}

// Windows ping 이 내는 모양의 에코 요청. 10.100.0.1 -> 10.100.0.9, 식별자 1, 순번 seq.
std::vector<std::byte> echo_request(std::size_t data_len, std::uint16_t seq = 7) {
    std::vector<std::byte> p(20 + 8 + data_len);
    p[0] = std::byte{0x45};
    put16(p, 2, static_cast<std::uint16_t>(p.size()));
    put16(p, 4, 0x1234);
    p[8] = std::byte{128};
    p[9] = std::byte{1};
    const std::uint8_t src[] = {10, 100, 0, 1};
    const std::uint8_t dst[] = {10, 100, 0, 9};
    for (std::size_t i = 0; i < 4; ++i) {
        p[12 + i] = static_cast<std::byte>(src[i]);
        p[16 + i] = static_cast<std::byte>(dst[i]);
    }
    put16(p, 10, static_cast<std::uint16_t>(~sum16(p, 0, 20)));
    p[20] = std::byte{8};
    put16(p, 24, 1);
    put16(p, 26, seq);
    for (std::size_t i = 0; i < data_len; ++i) {
        p[28 + i] = static_cast<std::byte>('a' + (i % 23));
    }
    put16(p, 22, static_cast<std::uint16_t>(~sum16(p, 20, p.size())));
    return p;
}

void refill_ip_checksum(std::vector<std::byte>& p) {
    put16(p, 10, 0);
    put16(p, 10, static_cast<std::uint16_t>(~sum16(p, 0, 20)));
}

void refill_icmp_checksum(std::vector<std::byte>& p) {
    put16(p, 22, 0);
    put16(p, 22, static_cast<std::uint16_t>(~sum16(p, 20, p.size())));
}

}  // namespace

TEST_CASE("icmp: an echo request comes back as a valid echo reply", "[testbuild]") {
    for (const std::size_t data_len : {std::size_t{32}, std::size_t{33}, std::size_t{0}, std::size_t{1372}}) {
        INFO("data_len=" << data_len);
        const auto request = echo_request(data_len);
        const auto reply = testbuild::make_icmp_echo_reply(request);
        REQUIRE(reply.has_value());
        REQUIRE(reply->size() == request.size());
        // 두 체크섬이 맞는다
        REQUIRE(sum16(*reply, 0, 20) == 0xFFFF);
        REQUIRE(sum16(*reply, 20, reply->size()) == 0xFFFF);
        // 출발지와 목적지가 뒤집혔다
        for (std::size_t i = 0; i < 4; ++i) {
            REQUIRE((*reply)[12 + i] == request[16 + i]);
            REQUIRE((*reply)[16 + i] == request[12 + i]);
        }
        REQUIRE((*reply)[8] == std::byte{64});
        REQUIRE((*reply)[9] == std::byte{1});
        REQUIRE((*reply)[20] == std::byte{0});
        REQUIRE((*reply)[21] == std::byte{0});
        // 식별자, 순번, 데이터는 그대로다
        for (std::size_t i = 24; i < request.size(); ++i) {
            REQUIRE((*reply)[i] == request[i]);
        }
    }
}

TEST_CASE("icmp: no reply to anything but an intact unfragmented echo request", "[testbuild]") {
    const auto good = echo_request(32);
    REQUIRE(testbuild::make_icmp_echo_reply(good).has_value());

    // 응답에는 다시 응답하지 않는다
    REQUIRE_FALSE(testbuild::make_icmp_echo_reply(*testbuild::make_icmp_echo_reply(good)).has_value());

    auto v6 = good;
    v6[0] = std::byte{0x65};
    REQUIRE_FALSE(testbuild::make_icmp_echo_reply(v6).has_value());

    auto options = good;
    options[0] = std::byte{0x46};
    refill_ip_checksum(options);
    REQUIRE_FALSE(testbuild::make_icmp_echo_reply(options).has_value());

    auto longer = good;
    longer.push_back(std::byte{0});
    REQUIRE_FALSE(testbuild::make_icmp_echo_reply(longer).has_value());
    auto shorter = good;
    shorter.pop_back();
    REQUIRE_FALSE(testbuild::make_icmp_echo_reply(shorter).has_value());
    REQUIRE_FALSE(testbuild::make_icmp_echo_reply(std::vector<std::byte>(good.begin(), good.begin() + 27)).has_value());

    auto more_fragments = good;
    more_fragments[6] = std::byte{0x20};
    refill_ip_checksum(more_fragments);
    REQUIRE_FALSE(testbuild::make_icmp_echo_reply(more_fragments).has_value());
    auto offset = good;
    offset[7] = std::byte{0x01};
    refill_ip_checksum(offset);
    REQUIRE_FALSE(testbuild::make_icmp_echo_reply(offset).has_value());
    auto dont_fragment = good;  // DF 만 선 것은 조각이 아니다
    dont_fragment[6] = std::byte{0x40};
    refill_ip_checksum(dont_fragment);
    REQUIRE(testbuild::make_icmp_echo_reply(dont_fragment).has_value());

    auto udp = good;
    udp[9] = std::byte{17};
    refill_ip_checksum(udp);
    REQUIRE_FALSE(testbuild::make_icmp_echo_reply(udp).has_value());

    auto unreachable = good;
    unreachable[20] = std::byte{3};
    refill_icmp_checksum(unreachable);
    REQUIRE_FALSE(testbuild::make_icmp_echo_reply(unreachable).has_value());
    auto code = good;
    code[21] = std::byte{1};
    refill_icmp_checksum(code);
    REQUIRE_FALSE(testbuild::make_icmp_echo_reply(code).has_value());

    auto bad_sum = good;
    bad_sum[30] = std::byte{0x00};
    REQUIRE_FALSE(testbuild::make_icmp_echo_reply(bad_sum).has_value());
}
