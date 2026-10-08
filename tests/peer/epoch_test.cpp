#include "sangtachi/peer/epoch.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <vector>

// 시험 케이스 이름은 ASCII 로만 적는다. 이유는 network/wsa_test.cpp 머리에 있다.
//
// protocol.md 4.3 session_epoch, 5.1 HELLO 의 nonce, 3장의 MAX_RETIRED_EPOCHS 를 본다.
// 난수원은 주입한다. 값 분포로는 약한 난수원을 가릴 수 없으므로(roadmap.md Phase 4 검증)
// 기본 경로가 OS CSPRNG 인지는 epoch.cpp 의 호출 경로로 확인한다. 여기서는 그 경로가 실제로
// 값을 내는지만 본다.

using sangtachi::Millis;
using sangtachi::peer::generate_attempt_nonce;
using sangtachi::peer::generate_session_epoch;
using sangtachi::peer::kRetiredEpochTtlMs;
using sangtachi::peer::kRetiredNonceTtlMs;
using sangtachi::peer::Nonce;
using sangtachi::peer::RandomFn;
using sangtachi::peer::RetiredEpochs;
using sangtachi::peer::RetiredNonces;

namespace {

// 정해 둔 바이트를 순서대로 내주는 난수원. 다 쓰면 실패를 돌려준다.
struct ScriptedRandom {
    std::vector<std::byte> bytes;
    std::size_t offset = 0;
    std::vector<std::size_t> requests;  // 호출마다 요청한 길이

    RandomFn fn() {
        return [this](std::span<std::byte> out) {
            requests.push_back(out.size());
            if (bytes.size() - offset < out.size()) {
                return false;
            }
            for (std::byte& b : out) {
                b = bytes[offset++];
            }
            return true;
        };
    }
};

std::vector<std::byte> be32(std::initializer_list<std::uint32_t> values) {
    std::vector<std::byte> out;
    for (const std::uint32_t v : values) {
        out.push_back(static_cast<std::byte>(v >> 24));
        out.push_back(static_cast<std::byte>(v >> 16));
        out.push_back(static_cast<std::byte>(v >> 8));
        out.push_back(static_cast<std::byte>(v));
    }
    return out;
}

Nonce nonce_of(std::uint8_t fill) {
    Nonce n;
    n.bytes.fill(static_cast<std::byte>(fill));
    return n;
}

}  // namespace

// ---------------------------------------------------------------- session_epoch (4.3)

TEST_CASE("epoch: a nonzero draw is used as is", "[peer][epoch]") {
    ScriptedRandom src{be32({0x12345678u})};
    const auto e = generate_session_epoch(src.fn());
    REQUIRE(e.has_value());
    REQUIRE(*e == 0x12345678u);
    REQUIRE(src.requests == std::vector<std::size_t>{4});
}

TEST_CASE("epoch: zero is redrawn", "[peer][epoch]") {
    // 4.3 "0은 쓰지 않는다. 0이 나오면 다시 뽑는다".
    ScriptedRandom src{be32({0u, 0u, 0u, 1u})};
    const auto e = generate_session_epoch(src.fn());
    REQUIRE(e.has_value());
    REQUIRE(*e == 1u);
    REQUIRE(src.requests == std::vector<std::size_t>{4, 4, 4, 4});
}

TEST_CASE("epoch: a random source failure is not replaced by a fixed value", "[peer][epoch]") {
    ScriptedRandom empty{};
    REQUIRE_FALSE(generate_session_epoch(empty.fn()).has_value());

    // 0 을 내고 그 다음에 실패하면 0 을 돌려주지 않는다.
    ScriptedRandom zero_then_fail{be32({0u})};
    REQUIRE_FALSE(generate_session_epoch(zero_then_fail.fn()).has_value());
    REQUIRE(zero_then_fail.requests.size() == 2);
}

TEST_CASE("epoch: the default source draws nonzero values", "[peer][epoch]") {
    // 기본 경로(platform::random_bytes)가 실제로 값을 낸다. 두 값이 다르다는 것은 보지 않는다.
    // 4.3 이 32비트 충돌을 허용하므로 2^-32 확률로 정상 구현이 떨어진다.
    const auto a = generate_session_epoch();
    const auto b = generate_session_epoch();
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    REQUIRE(*a != 0u);
    REQUIRE(*b != 0u);
}

// ---------------------------------------------------------------- 폐기 목록 (4.3)

TEST_CASE("epoch: retired list holds at most MAX_RETIRED_EPOCHS and evicts the oldest",
          "[peer][epoch]") {
    // roadmap.md Phase 4 검증의 폐기 목록 상한. 17개를 등록해 16개를 넘지 않고 가장 오래된
    // 것이 밀려나는지 본다. 상한 없이 자라는 구현은 size 에서, 새 항목을 거부하는 구현은
    // 17번째의 contains 에서 걸린다.
    RetiredEpochs list;
    for (std::uint32_t i = 1; i <= 17; ++i) {
        list.add(1000u + i, static_cast<Millis>(i) * 100);
        REQUIRE(list.size() == (i <= 16 ? i : 16u));
    }
    const Millis now = 2000;
    REQUIRE(list.size() == 16);
    REQUIRE_FALSE(list.contains(1001u, now));  // 가장 오래된 것이 밀려났다
    for (std::uint32_t i = 2; i <= 17; ++i) {
        INFO("epoch " << 1000u + i);
        REQUIRE(list.contains(1000u + i, now));
    }

    // 하나 더 넣으면 다음으로 오래된 것이 밀린다.
    list.add(5000u, 2100);
    REQUIRE(list.size() == 16);
    REQUIRE_FALSE(list.contains(1002u, now));
    REQUIRE(list.contains(1003u, now));
    REQUIRE(list.contains(5000u, now));
}

TEST_CASE("epoch: eviction follows registration order not value", "[peer][epoch]") {
    // "가장 오래된 것" 은 등록 시각이다. 값의 크기로 고르는 구현이 여기서 걸린다.
    RetiredEpochs list;
    for (std::uint32_t i = 0; i < 16; ++i) {
        list.add(0xFFFFFFF0u - i, i);  // 값은 내려간다
    }
    list.add(7u, 100);
    REQUIRE_FALSE(list.contains(0xFFFFFFF0u, 100));  // 처음 등록한 것
    REQUIRE(list.contains(0xFFFFFFF0u - 15u, 100));  // 값이 가장 작은 것은 남는다
    REQUIRE(list.contains(7u, 100));
}

TEST_CASE("epoch: retired entries expire after two minutes", "[peer][epoch]") {
    // 상수를 그대로 쓰면 상수를 고친 구현도 통과한다. 2분을 리터럴로 못박는다 (4.3, 5.1).
    STATIC_REQUIRE(kRetiredEpochTtlMs == 120'000);
    STATIC_REQUIRE(kRetiredNonceTtlMs == 120'000);
    // 만료 비교는 deadline <= now 다 (11장 타이머). 정확히 2분이 된 순간 사라진다.
    RetiredEpochs list;
    list.add(42u, 1000);
    REQUIRE(list.contains(42u, 1000));
    REQUIRE(list.contains(42u, 1000 + 119'999));
    REQUIRE_FALSE(list.contains(42u, 1000 + 120'000));
    REQUIRE_FALSE(list.contains(43u, 1000));

    REQUIRE(list.size() == 1);
    list.expire(1000 + kRetiredEpochTtlMs - 1);
    REQUIRE(list.size() == 1);
    list.expire(1000 + kRetiredEpochTtlMs);
    REQUIRE(list.size() == 0);

    // add 도 만료한 것을 먼저 치운다. 치우지 않으면 죽은 항목이 size 에 남는다.
    list.add(1u, 0);
    list.add(2u, 0);
    list.add(3u, 10);
    list.add(4u, kRetiredEpochTtlMs + 5);  // 1 과 2 는 만료, 3 은 아직
    REQUIRE(list.size() == 2);
    REQUIRE(list.contains(3u, kRetiredEpochTtlMs + 5));
}

TEST_CASE("epoch: expired entries are purged before evicting live ones", "[peer][epoch]") {
    // 만료한 항목이 자리를 잡고 있어 살아 있는 항목이 밀리면 2분 보관이 필요 이상으로 깨진다.
    RetiredEpochs list;
    list.add(1u, 0);
    for (std::uint32_t i = 2; i <= 16; ++i) {
        list.add(i, 60'000);
    }
    REQUIRE(list.size() == 16);
    const Millis now = kRetiredEpochTtlMs;  // epoch 1 만 만료
    list.add(99u, now);
    REQUIRE(list.size() == 16);
    for (std::uint32_t i = 2; i <= 16; ++i) {
        REQUIRE(list.contains(i, now));
    }
    REQUIRE(list.contains(99u, now));
}

TEST_CASE("epoch: re-adding a live epoch keeps one entry and restarts its time", "[peer][epoch]") {
    RetiredEpochs list;
    list.add(5u, 0);
    list.add(6u, 10);
    list.add(5u, 100'000);
    REQUIRE(list.size() == 2);
    REQUIRE(list.contains(5u, 150'000));
    REQUIRE_FALSE(list.contains(6u, 150'000));

    // 다시 넣은 것은 맨 뒤로 갔으므로 다음 밀어내기의 대상이 아니다.
    RetiredEpochs full;
    for (std::uint32_t i = 1; i <= 16; ++i) {
        full.add(i, i);
    }
    full.add(1u, 20);
    full.add(100u, 21);
    REQUIRE(full.contains(1u, 30));
    REQUIRE_FALSE(full.contains(2u, 30));
}

// ---------------------------------------------------------------- nonce (5.1)

TEST_CASE("nonce: an attempt nonce is 128 bits from the source", "[peer][nonce]") {
    ScriptedRandom src;
    for (int i = 0; i < 16; ++i) {
        src.bytes.push_back(static_cast<std::byte>(i + 1));
    }
    RetiredNonces retired;
    const auto n = generate_attempt_nonce(retired, 0, src.fn());
    REQUIRE(n.has_value());
    for (std::size_t i = 0; i < Nonce::kBytes; ++i) {
        REQUIRE(n->bytes[i] == static_cast<std::byte>(i + 1));
    }
    REQUIRE(src.requests == std::vector<std::size_t>{16});
}

TEST_CASE("nonce: a retired nonce is not reused", "[peer][nonce]") {
    // 5.1 "이전 시도의 nonce는 폐기 목록에 2분간 보관하고 재사용하지 않는다".
    RetiredNonces retired;
    retired.add(nonce_of(0xAA), 0);

    ScriptedRandom src;
    src.bytes.assign(16, std::byte{0xAA});
    src.bytes.insert(src.bytes.end(), 16, std::byte{0xBB});
    const auto n = generate_attempt_nonce(retired, 1000, src.fn());
    REQUIRE(n.has_value());
    REQUIRE(*n == nonce_of(0xBB));
    REQUIRE(src.requests.size() == 2);

    // 2분이 지나면 그 값은 더는 폐기 목록에 없다.
    ScriptedRandom again;
    again.bytes.assign(16, std::byte{0xAA});
    const auto m = generate_attempt_nonce(retired, kRetiredNonceTtlMs, again.fn());
    REQUIRE(m.has_value());
    REQUIRE(*m == nonce_of(0xAA));
}

TEST_CASE("nonce: a random source failure gives no nonce", "[peer][nonce]") {
    RetiredNonces retired;
    ScriptedRandom empty;
    REQUIRE_FALSE(generate_attempt_nonce(retired, 0, empty.fn()).has_value());

    retired.add(nonce_of(0x11), 0);
    ScriptedRandom only_retired;
    only_retired.bytes.assign(16, std::byte{0x11});
    REQUIRE_FALSE(generate_attempt_nonce(retired, 0, only_retired.fn()).has_value());
}

TEST_CASE("nonce: the retired nonce list has no cap and expires", "[peer][nonce]") {
    // 3장: 우리 자신의 시도만으로 자라는 표라 상한을 걸 대상이 아니다. 상한을 잘못 옮겨 온
    // 구현이 17번째에서 걸린다.
    RetiredNonces retired;
    for (int i = 0; i < 40; ++i) {
        retired.add(nonce_of(static_cast<std::uint8_t>(i)), static_cast<Millis>(i));
    }
    REQUIRE(retired.size() == 40);
    for (int i = 0; i < 40; ++i) {
        REQUIRE(retired.contains(nonce_of(static_cast<std::uint8_t>(i)), 100));
    }
    REQUIRE(retired.contains(nonce_of(0), kRetiredNonceTtlMs - 1));
    REQUIRE_FALSE(retired.contains(nonce_of(0), kRetiredNonceTtlMs));
    REQUIRE(retired.contains(nonce_of(1), kRetiredNonceTtlMs));

    retired.expire(kRetiredNonceTtlMs + 10);
    REQUIRE(retired.size() == 29);  // 11..39 이 남는다

    // add 도 만료한 것을 치운다.
    retired.add(nonce_of(0xEE), kRetiredNonceTtlMs + 39);
    REQUIRE(retired.size() == 1);
}

TEST_CASE("nonce: the default source draws distinct nonces", "[peer][nonce]") {
    RetiredNonces retired;
    const auto a = generate_attempt_nonce(retired, 0);
    const auto b = generate_attempt_nonce(retired, 0);
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    REQUIRE(*a != *b);  // 같을 확률은 2^-128 이다
}

TEST_CASE("epoch: retired lists keep 64-bit timestamps", "[peer][epoch][nonce]") {
    // 등록 시각을 32비트로 잘라 들고 있으면 프로세스가 약 49.7일 돈 뒤 만료가 어긋난다.
    // 2^32 를 넘는 시각에서 경계를 리터럴로 본다.
    constexpr Millis kBase = Millis{0x1'0000'0000} + 5;
    STATIC_REQUIRE(kBase > Millis{0xFFFFFFFF});

    RetiredEpochs epochs;
    epochs.add(0xAABBCCDDu, kBase);
    REQUIRE(epochs.contains(0xAABBCCDDu, kBase));
    REQUIRE(epochs.contains(0xAABBCCDDu, kBase + 119'999));
    REQUIRE_FALSE(epochs.contains(0xAABBCCDDu, kBase + 120'000));

    RetiredNonces nonces;
    Nonce n;
    n.bytes[0] = std::byte{0x5A};
    nonces.add(n, kBase);
    REQUIRE(nonces.contains(n, kBase));
    REQUIRE(nonces.contains(n, kBase + 119'999));
    REQUIRE_FALSE(nonces.contains(n, kBase + 120'000));
}
