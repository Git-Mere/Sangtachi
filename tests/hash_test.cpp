#include "hamychi/hash.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <vector>

// 시험 케이스 이름은 ASCII 로만 적는다. 이유는 network/wsa_test.cpp 머리에 있다.

using hamychi::kShortHashChars;
using hamychi::sha256_hex;
using hamychi::sha256_short;

namespace {

std::vector<std::byte> bytes_of(std::string_view text) {
    std::vector<std::byte> out;
    out.reserve(text.size());
    for (const char c : text) {
        out.push_back(static_cast<std::byte>(c));
    }
    return out;
}

}  // namespace

TEST_CASE("hash: the empty input matches the published SHA-256 vector", "[hash]") {
    // FIPS 180-4 의 널리 알려진 값이다. 우리 구현이 아니라 CNG 를 쓰지만, 붙이는 방식이
    // 틀리면 여기서 걸린다.
    const auto got = sha256_hex(std::span<const std::byte>());
    REQUIRE(got.has_value());
    REQUIRE(*got == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST_CASE("hash: abc matches the published SHA-256 vector", "[hash]") {
    const auto data = bytes_of("abc");
    const auto got = sha256_hex(data);
    REQUIRE(got.has_value());
    REQUIRE(*got == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST_CASE("hash: the short form is the first sixteen hex characters", "[hash]") {
    // architecture.md 3.5 가 rx.raw 의 sha256 을 "16진 해시 앞 16자" 로 정했다.
    const auto data = bytes_of("abc");
    const auto full = sha256_hex(data);
    const auto shortened = sha256_short(data);
    REQUIRE(full.has_value());
    REQUIRE(shortened.has_value());
    REQUIRE(shortened->size() == kShortHashChars);
    REQUIRE(*shortened == full->substr(0, kShortHashChars));
    REQUIRE(*shortened == "ba7816bf8f01cfea");
}

TEST_CASE("hash: different payloads give different digests", "[hash]") {
    // rx.raw 두 줄을 대조하는 판정이 이 성질에 기댄다.
    const auto a = sha256_short(bytes_of("payload-a"));
    const auto b = sha256_short(bytes_of("payload-b"));
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    REQUIRE(*a != *b);
}

TEST_CASE("hash: the digest is lower case hex only", "[hash]") {
    const auto got = sha256_hex(bytes_of("Sangtachi"));
    REQUIRE(got.has_value());
    REQUIRE(got->size() == 64);
    for (const char c : *got) {
        INFO("character " << c);
        REQUIRE(((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')));
    }
}
