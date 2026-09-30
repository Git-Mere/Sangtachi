#include "sangtachi/platform/random.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <vector>

// 시험 케이스 이름은 ASCII 로만 적는다. 이유는 network/wsa_test.cpp 머리에 있다.

// 무작위성 자체는 판정하지 않는다. 통계 검정은 드물게 거짓 실패하고, 그 실패는 다음
// 사람이 재현할 수 없다. 여기서 보는 축은 넷이다.
//   (1) 빈 요청이 성공이고 아무것도 쓰지 않는다
//   (2) 요청한 범위를 끝까지 채운다
//   (3) 그 범위 밖을 건드리지 않는다
//   (4) 호출마다 값이 다르다
// (2) 와 (4) 는 확률 명제다. 아래 각 자리에 그 확률을 적었고, 전부 2^-96 이하다.

using sangtachi::platform::random_bytes;

namespace {

// protocol.md 13장 STUN 사용 범위: 트랜잭션 ID 는 96비트다.
constexpr std::size_t kTransactionIdBytes = 12;

// 채우기 전에 깔아 두는 표시값. 이 값이 남아 있으면 그 자리를 쓰지 않은 것이다.
constexpr std::byte kSentinel{0xAB};

bool all_sentinel(std::span<const std::byte> bytes) noexcept {
    return std::all_of(bytes.begin(), bytes.end(),
                       [](std::byte b) { return b == kSentinel; });
}

}  // namespace

TEST_CASE("random: an empty span succeeds and writes nothing", "[platform][random]") {
    // 채울 것이 없는 요청과 실패를 같은 값으로 알리면 부르는 쪽이 둘을 가를 수 없다.
    std::array<std::byte, 8> buffer{};
    buffer.fill(kSentinel);

    REQUIRE(random_bytes(std::span<std::byte>{}));
    REQUIRE(random_bytes(std::span<std::byte>(buffer).subspan(0, 0)));
    REQUIRE(all_sentinel(buffer));
}

TEST_CASE("random: a transaction id sized buffer is filled", "[platform][random]") {
    std::array<std::byte, kTransactionIdBytes> id{};
    id.fill(kSentinel);

    REQUIRE(random_bytes(id));

    // 12바이트가 전부 표시값 그대로일 확률은 256^-12 = 2^-96 이다.
    REQUIRE_FALSE(all_sentinel(id));
}

TEST_CASE("random: two calls give different bytes", "[platform][random]") {
    std::array<std::byte, kTransactionIdBytes> first{};
    std::array<std::byte, kTransactionIdBytes> second{};

    REQUIRE(random_bytes(first));
    REQUIRE(random_bytes(second));

    // 같을 확률은 2^-96 이다. 같은 값이 두 번 나오면 그것은 운이 아니라 결함이다.
    // 트랜잭션 ID 가 겹치면 두 서버의 응답을 가를 수 없다 (protocol.md 13장).
    REQUIRE(first != second);
}

TEST_CASE("random: a large buffer is filled end to end", "[platform][random]") {
    // 길이를 좁히다 잘리면 앞부분만 채우고 성공을 알린다. 끝자락을 따로 본다.
    constexpr std::size_t kBytes = 4096;
    std::vector<std::byte> buffer(kBytes, kSentinel);

    REQUIRE(random_bytes(std::span<std::byte>(buffer)));

    const std::span<const std::byte> whole{buffer};
    REQUIRE_FALSE(all_sentinel(whole.first(32)));
    REQUIRE_FALSE(all_sentinel(whole.last(32)));   // 끝까지 채웠는가
    REQUIRE_FALSE(all_sentinel(whole.subspan(kBytes / 2, 32)));
    // 각 32바이트 창이 표시값 그대로일 확률은 256^-32 다.
}

TEST_CASE("random: bytes outside the span are untouched", "[platform][random]") {
    // 요청 범위 밖으로 넘치면 그 자리를 쓰는 다른 코드의 값을 조용히 망친다.
    constexpr std::size_t kGuard = 16;
    constexpr std::size_t kBody = 64;
    std::array<std::byte, kGuard + kBody + kGuard> buffer{};
    buffer.fill(kSentinel);

    REQUIRE(random_bytes(std::span<std::byte>(buffer).subspan(kGuard, kBody)));

    const std::span<const std::byte> whole{buffer};
    REQUIRE(all_sentinel(whole.first(kGuard)));
    REQUIRE(all_sentinel(whole.last(kGuard)));
    REQUIRE_FALSE(all_sentinel(whole.subspan(kGuard, kBody)));
}
