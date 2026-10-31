#include "sangtachi/hash.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// 시험 케이스 이름은 ASCII 로만 적는다. 이유는 network/wsa_test.cpp 머리에 있다.

using sangtachi::sha256_hex;

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

TEST_CASE("hash: different payloads give different digests", "[hash]") {
    // data.tx 와 data.rx 를 대조하는 판정이 이 성질에 기댄다 (architecture.md 9장).
    const auto a = sha256_hex(bytes_of("payload-a"));
    const auto b = sha256_hex(bytes_of("payload-b"));
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

TEST_CASE("hash: the two block NIST vector matches", "[hash]") {
    // 448비트 입력은 길이 자리가 첫 블록에 들어가지 않아 패딩이 두 번째 블록으로 넘어간다.
    const auto data = bytes_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq");
    const auto got = sha256_hex(data);
    REQUIRE(got.has_value());
    REQUIRE(*got == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST_CASE("hash: one million a matches the NIST vector", "[hash]") {
    const std::vector<std::byte> data(1000000, std::byte{'a'});
    const auto got = sha256_hex(data);
    REQUIRE(got.has_value());
    REQUIRE(*got == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("hash: lengths around the padding boundary match", "[hash]") {
    // 55 는 한 블록에 패딩이 다 들어가는 마지막 길이, 56 은 두 블록이 되는 첫 길이다. 기대값은
    // Python hashlib 이 같은 입력((i * 7 + 3) mod 256)으로 낸 것이다.
    struct Vector {
        std::size_t length;
        std::string_view digest;
    };
    constexpr std::array<Vector, 6> kVectors = {{
        {55, "e7313d333c272e639f790978283f9eb392e843d0f29b7016828bb1daa4aac70b"},
        {56, "4324d65f3c103567f5589c710bc08f8523f929a9272e3af36fc968e52abc6c27"},
        {63, "81c80242132f230c3bd41b3e63bbcff16107339549214a99614ff26664625055"},
        {64, "39e3d7b6b5d075d37d053ad89b24b41bef4f3c29760c84447cab3f3be1882241"},
        {119, "9ce7368e4daf32341631b492e80359dc9f594b48453cd0dd5bf0b19279cc177e"},
        {1000, "1e9bc38cbf860b9ec31918b065f9b52476c549a782e0e7990bed8ce3868d2371"},
    }};
    for (const Vector& v : kVectors) {
        std::vector<std::byte> data(v.length);
        for (std::size_t i = 0; i < v.length; ++i) {
            data[i] = static_cast<std::byte>((i * 7 + 3) & 0xFFu);
        }
        INFO("length " << v.length);
        const auto got = sha256_hex(data);
        REQUIRE(got.has_value());
        REQUIRE(*got == v.digest);
    }
}
