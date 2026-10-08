#include "sangtachi/tunnel/replay.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <random>
#include <set>

// 시험 케이스 이름은 ASCII 로만 적는다. 이유는 network/wsa_test.cpp 머리에 있다.
//
// protocol.md 4.4 sequence 와 4.5 중복 억제의 케이스 표다. 4.5 가 "틀리기 쉽다" 고 적은
// 경계 넷을 각각 하나 이상의 케이스가 잡는다. 어느 케이스가 어느 규칙을 잡는지는 각 케이스
// 머리에 적었다.

using sangtachi::tunnel::is_newer;
using sangtachi::tunnel::ReplayDecision;
using sangtachi::tunnel::ReplayVerdict;
using sangtachi::tunnel::ReplayWindow;

namespace {

constexpr std::uint64_t bit(unsigned back) {
    // back=b 는 비트 (b-1) 다 (4.5).
    return std::uint64_t{1} << (back - 1);
}

// 창을 seq 하나로 시작시킨다.
ReplayWindow window_at(std::uint32_t first) {
    ReplayWindow w;
    const ReplayDecision d = w.accept(first);
    REQUIRE(d.verdict == ReplayVerdict::kAdvance);
    return w;
}

}  // namespace

// ---------------------------------------------------------------- is_newer (4.4)

TEST_CASE("replay: is_newer is false for equal values", "[tunnel][replay]") {
    // a != b 조건을 지우면 같은 번호가 새 패킷이 되어 재생 방어가 무력화된다 (4.4).
    REQUIRE_FALSE(is_newer(0u, 0u));
    REQUIRE_FALSE(is_newer(1u, 1u));
    REQUIRE_FALSE(is_newer(0x7FFFFFFFu, 0x7FFFFFFFu));
    REQUIRE_FALSE(is_newer(0x80000000u, 0x80000000u));
    REQUIRE_FALSE(is_newer(0xFFFFFFFFu, 0xFFFFFFFFu));
}

TEST_CASE("replay: is_newer follows 32-bit wraparound", "[tunnel][replay]") {
    REQUIRE(is_newer(1u, 0u));
    REQUIRE_FALSE(is_newer(0u, 1u));
    // 0xFFFFFFFF 다음은 0 이다 (4.4 "32비트에서 순환한다").
    REQUIRE(is_newer(0u, 0xFFFFFFFFu));
    REQUIRE_FALSE(is_newer(0xFFFFFFFFu, 0u));
    REQUIRE(is_newer(5u, 0xFFFFFFF0u));
    // 반 바퀴 경계. 차이가 2^31-1 이면 새롭고 2^31 이면 어느 쪽도 새롭지 않다.
    REQUIRE(is_newer(0x7FFFFFFFu, 0u));
    REQUIRE_FALSE(is_newer(0x80000000u, 0u));
    REQUIRE_FALSE(is_newer(0u, 0x80000000u));
    REQUIRE(is_newer(0x80000009u, 10u));
}

// ---------------------------------------------------------------- 첫 패킷

TEST_CASE("replay: the first packet initializes the window", "[tunnel][replay]") {
    ReplayWindow w;
    REQUIRE_FALSE(w.initialized());

    const ReplayDecision d = w.accept(1000);
    REQUIRE(d.verdict == ReplayVerdict::kAdvance);
    REQUIRE(d.accepted());
    REQUIRE(d.advanced());
    REQUIRE(d.pkt_pos == 0);

    REQUIRE(w.initialized());
    REQUIRE(w.highest() == 1000);
    REQUIRE(w.position() == 0);
    REQUIRE(w.seen_bits() == 0);
}

TEST_CASE("replay: the first packet may be any value including old-looking ones", "[tunnel][replay]") {
    // 재시작한 상대가 0 부터 보내도 첫 패킷은 수용된다 (8.3). 초기화 전 highest 는 0 이지만
    // 그 값과 비교하지 않는다.
    ReplayWindow a;
    REQUIRE(a.accept(0).verdict == ReplayVerdict::kAdvance);
    REQUIRE(a.highest() == 0);

    ReplayWindow b;
    REQUIRE(b.accept(0x80000000u).verdict == ReplayVerdict::kAdvance);
    REQUIRE(b.highest() == 0x80000000u);
}

// ---------------------------------------------------------------- seq == highest (4.5 경계 첫째)

TEST_CASE("replay: the same seq twice is a duplicate", "[tunnel][replay]") {
    // seq == highest 를 먼저 거르지 않으면 back == 0 이 되어 1 << (0-1) 이라는 미정의
    // 동작이 된다. x64 에서 그 시프트는 비트 63 을 가리켜 중복이 수용된다.
    ReplayWindow w = window_at(77);
    const ReplayDecision d = w.check(77);
    REQUIRE(d.verdict == ReplayVerdict::kDuplicate);
    REQUIRE_FALSE(d.accepted());
    REQUIRE(w.accept(77).verdict == ReplayVerdict::kDuplicate);

    // 전진한 뒤의 highest 도 같다.
    REQUIRE(w.accept(78).verdict == ReplayVerdict::kAdvance);
    REQUIRE(w.accept(78).verdict == ReplayVerdict::kDuplicate);
    REQUIRE(w.accept(77).verdict == ReplayVerdict::kDuplicate);
}

TEST_CASE("replay: a duplicate of highest is caught even when bit 63 is set", "[tunnel][replay]") {
    // 비트 63 이 이미 서 있으면 위 케이스의 미정의 동작이 우연히 중복으로 판정된다.
    // 비트 63 이 비어 있는 상태(위 케이스)와 서 있는 상태 둘 다에서 본다.
    ReplayWindow w = window_at(0);
    REQUIRE(w.accept(64).verdict == ReplayVerdict::kAdvance);
    REQUIRE(w.seen_bits() == bit(64));
    REQUIRE(w.accept(64).verdict == ReplayVerdict::kDuplicate);
}

// ---------------------------------------------------------------- 전진과 shift

TEST_CASE("replay: advancing by one sets bit 0", "[tunnel][replay]") {
    ReplayWindow w = window_at(10);
    const ReplayDecision d = w.accept(11);
    REQUIRE(d.verdict == ReplayVerdict::kAdvance);
    REQUIRE(d.distance == 1);
    REQUIRE(d.pkt_pos == 1);
    REQUIRE(w.highest() == 11);
    REQUIRE(w.position() == 1);
    REQUIRE(w.seen_bits() == bit(1));
}

TEST_CASE("replay: shift 63 keeps the old highest at back 63", "[tunnel][replay]") {
    ReplayWindow w = window_at(100);
    const ReplayDecision d = w.accept(163);
    REQUIRE(d.verdict == ReplayVerdict::kAdvance);
    REQUIRE(d.distance == 63);
    REQUIRE(w.seen_bits() == bit(63));
    REQUIRE(w.position() == 63);

    REQUIRE(w.accept(100).verdict == ReplayVerdict::kDuplicate);  // back 63, 이미 받았다
    // back 64 는 받은 적 없는 창 안 번호다.
    const ReplayDecision r = w.accept(99);
    REQUIRE(r.verdict == ReplayVerdict::kReorder);
    REQUIRE(r.distance == 64);
}

TEST_CASE("replay: shift exactly 64 sets bit 63 instead of clearing", "[tunnel][replay]") {
    // 4.5 경계 셋째. shift == 64 에서 비트맵을 지우면 직전 highest 가 back == 64 에 미기록으로
    // 남아 중복이 다시 수용된다. seen_bits << 64 는 미정의 동작이라 따로 처리해야 한다.
    ReplayWindow w = window_at(1000);
    REQUIRE(w.accept(1001).verdict == ReplayVerdict::kAdvance);  // 비트맵에 무언가 있게 한다

    const ReplayDecision d = w.accept(1065);  // shift 64
    REQUIRE(d.verdict == ReplayVerdict::kAdvance);
    REQUIRE(d.distance == 64);
    REQUIRE(w.seen_bits() == bit(64));
    REQUIRE(w.position() == 65);

    REQUIRE(w.accept(1001).verdict == ReplayVerdict::kDuplicate);  // back 64
    REQUIRE(w.accept(1000).verdict == ReplayVerdict::kTooOld);     // back 65
}

TEST_CASE("replay: shift 65 clears the bitmap", "[tunnel][replay]") {
    // 세 분기가 상호 배타여야 한다 (4.5 경계 둘째). if / if / else 로 쓰면 비트맵을 지운 뒤
    // else 로도 들어가 65 칸을 민다. x64 는 시프트 수를 64 로 나눈 나머지로 쓰므로 그 구현은
    // 비트 0 을 세워 back 1 을 중복으로 판정한다.
    ReplayWindow w = window_at(1000);
    REQUIRE(w.accept(1001).verdict == ReplayVerdict::kAdvance);

    const ReplayDecision d = w.accept(1066);  // shift 65
    REQUIRE(d.verdict == ReplayVerdict::kAdvance);
    REQUIRE(d.distance == 65);
    REQUIRE(w.seen_bits() == 0);
    REQUIRE(w.position() == 66);

    REQUIRE(w.accept(1065).verdict == ReplayVerdict::kReorder);  // back 1
    REQUIRE(w.accept(1002).verdict == ReplayVerdict::kReorder);  // back 64
    REQUIRE(w.accept(1001).verdict == ReplayVerdict::kTooOld);   // back 65, 받은 적 있어도 창 밖
}

TEST_CASE("replay: a large advance clears the bitmap", "[tunnel][replay]") {
    ReplayWindow w = window_at(0);
    REQUIRE(w.accept(1).verdict == ReplayVerdict::kAdvance);
    const ReplayDecision d = w.accept(0x7FFFFFFFu + 1u);  // shift 2^31-1
    REQUIRE(d.verdict == ReplayVerdict::kAdvance);
    REQUIRE(d.distance == 0x7FFFFFFFu);
    REQUIRE(w.seen_bits() == 0);
    REQUIRE(w.position() == 1u + 0x7FFFFFFFu);
}

TEST_CASE("replay: half a lap away is too old not newer", "[tunnel][replay]") {
    // 차이 2^31 은 is_newer 가 거짓이므로 오래된 쪽으로 간다. back = 2^31 이고 창 밖이다.
    ReplayWindow w = window_at(10);
    const ReplayDecision d = w.accept(10u + 0x80000000u);
    REQUIRE(d.verdict == ReplayVerdict::kTooOld);
    REQUIRE(w.highest() == 10);
}

// ---------------------------------------------------------------- 오래된 쪽

TEST_CASE("replay: back 63 64 and 65 at the window edge", "[tunnel][replay]") {
    // 창은 back = 1..64 다. 65 부터 drop_too_old 다.
    ReplayWindow w = window_at(500);
    REQUIRE(w.accept(600).verdict == ReplayVerdict::kAdvance);  // shift 100, 비트맵 비움

    const ReplayDecision d63 = w.accept(537);
    REQUIRE(d63.verdict == ReplayVerdict::kReorder);
    REQUIRE(d63.distance == 63);
    const ReplayDecision d64 = w.accept(536);
    REQUIRE(d64.verdict == ReplayVerdict::kReorder);
    REQUIRE(d64.distance == 64);
    REQUIRE(w.accept(535).verdict == ReplayVerdict::kTooOld);
    REQUIRE(w.seen_bits() == (bit(63) | bit(64)));
}

TEST_CASE("replay: a reordered packet is accepted once", "[tunnel][replay]") {
    ReplayWindow w = window_at(0);
    REQUIRE(w.accept(10).verdict == ReplayVerdict::kAdvance);

    const ReplayDecision d = w.accept(5);
    REQUIRE(d.verdict == ReplayVerdict::kReorder);
    REQUIRE(d.accepted());
    REQUIRE_FALSE(d.advanced());  // 10.4 (b): 재정렬 수용은 엔드포인트 학습에 쓰지 않는다
    REQUIRE(d.distance == 5);
    REQUIRE(d.pkt_pos == 5);  // position - back
    REQUIRE(w.highest() == 10);
    REQUIRE(w.position() == 10);

    REQUIRE(w.accept(5).verdict == ReplayVerdict::kDuplicate);
}

TEST_CASE("replay: bits above 32 use 64-bit shifts", "[tunnel][replay]") {
    // 4.5 경계 넷째. 1 << (back-1) 을 32비트로 하면 back 33 부터 엉뚱한 비트를 본다.
    ReplayWindow w = window_at(0);
    REQUIRE(w.accept(64).verdict == ReplayVerdict::kAdvance);  // back 64 = seq 0 기록
    for (std::uint32_t back = 33; back <= 63; ++back) {
        const std::uint32_t seq = 64 - back;
        INFO("back " << back);
        REQUIRE(w.accept(seq).verdict == ReplayVerdict::kReorder);
        REQUIRE((w.seen_bits() & bit(back)) != 0);
        REQUIRE(w.accept(seq).verdict == ReplayVerdict::kDuplicate);
    }
    // back 1..32 는 아직 비어 있다.
    REQUIRE((w.seen_bits() & 0xFFFFFFFFull) == 0);
    REQUIRE(w.accept(63).verdict == ReplayVerdict::kReorder);  // back 1
}

TEST_CASE("replay: a reorder older than the first packet follows the 4.5 formula", "[tunnel][replay]") {
    // 첫 패킷보다 앞선 번호가 창 안에서 오면 수용된다. pkt_pos 는 4.5 의 position - back
    // 그대로이고 position 이 0 이라 uint64_t 에서 감싸 돈다. 이 값은 Phase 5 손실 집계의
    // baseline 비교에 들어가면 baseline 보다 크게 보인다. 문서가 이 경우를 따로 정하지 않아
    // 공식을 그대로 따랐다. 문서를 고치면 이 케이스를 같이 고친다.
    ReplayWindow w = window_at(100);
    const ReplayDecision d = w.accept(99);
    REQUIRE(d.verdict == ReplayVerdict::kReorder);
    REQUIRE(d.distance == 1);
    REQUIRE(d.pkt_pos == ~std::uint64_t{0});
    REQUIRE(w.position() == 0);
    REQUIRE(w.highest() == 100);
}

// ---------------------------------------------------------------- 랩어라운드

TEST_CASE("replay: advancing across the 32-bit wrap", "[tunnel][replay]") {
    ReplayWindow w = window_at(0xFFFFFFF0u);
    const ReplayDecision d = w.accept(5);  // shift 0x15 = 21
    REQUIRE(d.verdict == ReplayVerdict::kAdvance);
    REQUIRE(d.distance == 21);
    REQUIRE(w.highest() == 5);
    REQUIRE(w.position() == 21);  // position 은 순환하지 않는다

    REQUIRE(w.accept(0xFFFFFFFFu).verdict == ReplayVerdict::kReorder);  // back 6
    REQUIRE(w.accept(0xFFFFFFF0u).verdict == ReplayVerdict::kDuplicate);  // back 21
    REQUIRE(w.accept(0).verdict == ReplayVerdict::kReorder);  // back 5
    REQUIRE(w.accept(6).verdict == ReplayVerdict::kAdvance);
    REQUIRE(w.position() == 22);
}

TEST_CASE("replay: too old across the 32-bit wrap", "[tunnel][replay]") {
    ReplayWindow w = window_at(0xFFFFFFC0u);  // 2^32 - 64
    REQUIRE(w.accept(1).verdict == ReplayVerdict::kAdvance);  // shift 65
    REQUIRE(w.accept(0xFFFFFFC1u).verdict == ReplayVerdict::kReorder);  // back 64
    REQUIRE(w.accept(0xFFFFFFC0u).verdict == ReplayVerdict::kTooOld);   // back 65
}

// ---------------------------------------------------------------- 판정과 반영

TEST_CASE("replay: check has no side effects", "[tunnel][replay]") {
    // 4.5 "이 판정은 어떤 부수 효과보다 먼저 온다". 판정만으로 상태가 바뀌면 판정 뒤에 끼운
    // 검사가 패킷을 버려도 그 번호가 이미 소모된다.
    ReplayWindow fresh;
    const ReplayDecision first = fresh.check(42);
    REQUIRE(first.verdict == ReplayVerdict::kAdvance);
    REQUIRE_FALSE(fresh.initialized());
    REQUIRE(fresh.check(42) == first);

    ReplayWindow w = window_at(100);
    REQUIRE(w.accept(110).verdict == ReplayVerdict::kAdvance);
    const std::uint64_t bits = w.seen_bits();
    for (std::uint32_t seq : {111u, 105u, 110u, 100u, 20u, 300u}) {
        (void)w.check(seq);
        (void)w.check(seq);
    }
    REQUIRE(w.highest() == 110);
    REQUIRE(w.position() == 10);
    REQUIRE(w.seen_bits() == bits);

    // 판정만 하고 반영하지 않은 번호는 다시 와도 수용된다.
    REQUIRE(w.accept(105).verdict == ReplayVerdict::kReorder);
}

TEST_CASE("replay: commit applies a fresh decision", "[tunnel][replay]") {
    ReplayWindow w;
    const ReplayDecision d0 = w.check(7);
    REQUIRE(w.commit(d0));
    REQUIRE(w.initialized());
    REQUIRE(w.highest() == 7);

    const ReplayDecision d1 = w.check(9);
    REQUIRE(w.commit(d1));
    REQUIRE(w.highest() == 9);
    REQUIRE(w.seen_bits() == bit(2));

    const ReplayDecision d2 = w.check(8);
    REQUIRE(d2.verdict == ReplayVerdict::kReorder);
    REQUIRE(w.commit(d2));
    REQUIRE(w.seen_bits() == (bit(1) | bit(2)));
}

TEST_CASE("replay: commit refuses drops and stale decisions", "[tunnel][replay]") {
    ReplayWindow w = window_at(50);

    // 폐기 판정은 반영할 것이 없다.
    const ReplayDecision dup = w.check(50);
    REQUIRE_FALSE(w.commit(dup));

    // 같은 번호의 판정을 두 번 반영하면 두 번째는 낡은 판정이다.
    const ReplayDecision a = w.check(55);
    REQUIRE(w.commit(a));
    REQUIRE_FALSE(w.commit(a));
    REQUIRE(w.highest() == 55);
    REQUIRE(w.position() == 5);

    // 판정 둘을 먼저 내고 뒤의 것을 먼저 반영하면 앞의 것은 낡았다. 그대로 반영하면 shift 5
    // 로 다시 밀어 창이 망가진다.
    const ReplayDecision b = w.check(60);
    const ReplayDecision c = w.check(62);
    REQUIRE(w.commit(c));
    const std::uint64_t bits = w.seen_bits();
    REQUIRE_FALSE(w.commit(b));
    REQUIRE(w.highest() == 62);
    REQUIRE(w.position() == 12);
    REQUIRE(w.seen_bits() == bits);
    // 60 은 아직 받지 않았으므로 지금 상태의 판정으로는 수용된다.
    REQUIRE(w.accept(60).verdict == ReplayVerdict::kReorder);
}

TEST_CASE("replay: a dropped packet changes no state", "[tunnel][replay]") {
    // 8.3: drop_duplicate 나 drop_too_old 로 버려진 패킷은 어떤 상태도 바꾸지 않는다.
    ReplayWindow w = window_at(1000);
    REQUIRE(w.accept(1003).verdict == ReplayVerdict::kAdvance);
    const std::uint32_t h = w.highest();
    const std::uint64_t p = w.position();
    const std::uint64_t s = w.seen_bits();

    REQUIRE(w.accept(1003).verdict == ReplayVerdict::kDuplicate);
    REQUIRE(w.accept(1000).verdict == ReplayVerdict::kDuplicate);
    REQUIRE(w.accept(900).verdict == ReplayVerdict::kTooOld);
    REQUIRE(w.highest() == h);
    REQUIRE(w.position() == p);
    REQUIRE(w.seen_bits() == s);
}

TEST_CASE("replay: reset returns to the uninitialized state", "[tunnel][replay]") {
    // 9.5 재협상 3번. 재시작한 상대의 sequence = 0 이 이전 창의 drop_too_old 에 걸리지 않는다.
    ReplayWindow w = window_at(5000);
    REQUIRE(w.accept(5100).verdict == ReplayVerdict::kAdvance);
    REQUIRE(w.check(0).verdict == ReplayVerdict::kTooOld);

    w.reset();
    REQUIRE_FALSE(w.initialized());
    REQUIRE(w.highest() == 0);
    REQUIRE(w.position() == 0);
    REQUIRE(w.seen_bits() == 0);

    const ReplayDecision d = w.accept(0);
    REQUIRE(d.verdict == ReplayVerdict::kAdvance);
    REQUIRE(d.pkt_pos == 0);
    REQUIRE(w.accept(1).verdict == ReplayVerdict::kAdvance);
}

// ---------------------------------------------------------------- 참조 모형과 대조

TEST_CASE("replay: matches a set-based reference model", "[tunnel][replay]") {
    // 순환 없는 64비트 위치로 판정하는 단순 모형과 같은 결과를 내는지 본다. 모형은 받은
    // 위치를 전부 집합에 둔다. 비트맵을 쓰지 않으므로 시프트 실수가 같이 생길 수 없다.
    //
    // 난수 씨앗을 고정한다. 실패가 재현돼야 한다.
    for (const std::uint32_t start : {0u, 0x7FFFFF00u, 0xFFFFFF00u}) {
        std::mt19937 rng(0x5A11u ^ start);
        std::uniform_int_distribution<int> step(-90, 90);
        std::uniform_int_distribution<int> jump(0, 99);

        ReplayWindow w;
        std::set<std::uint64_t> seen;
        bool init = false;
        std::uint64_t top = 0;  // 모형의 highest 위치. start 를 1 << 32 에 둔다
        const std::uint64_t base = std::uint64_t{1} << 32;

        for (int i = 0; i < 20000; ++i) {
            std::int64_t delta = step(rng);
            if (jump(rng) == 0) {
                delta = (jump(rng) < 50) ? 63 + jump(rng) % 4 : 64 + jump(rng) % 200;
            }
            std::uint64_t pos = init ? static_cast<std::uint64_t>(static_cast<std::int64_t>(top) + delta)
                                     : base;
            const std::uint32_t seq = static_cast<std::uint32_t>(start + (pos - base));

            ReplayVerdict expected;
            if (!init) {
                expected = ReplayVerdict::kAdvance;
            } else if (pos > top) {
                expected = ReplayVerdict::kAdvance;
            } else if (top - pos > 64) {
                expected = ReplayVerdict::kTooOld;
            } else if (seen.count(pos) != 0) {
                expected = ReplayVerdict::kDuplicate;
            } else {
                expected = ReplayVerdict::kReorder;
            }

            const ReplayDecision d = w.accept(seq);
            INFO("start " << start << " step " << i << " seq " << seq);
            REQUIRE(d.verdict == expected);
            if (d.accepted()) {
                seen.insert(pos);
                REQUIRE(d.pkt_pos == pos - base);
            }
            if (expected == ReplayVerdict::kAdvance) {
                top = pos;
                init = true;
            }
            REQUIRE(w.position() == top - base);
        }
    }
}

TEST_CASE("replay: position keeps counting past 2^32 without wrapping", "[tunnel][replay]") {
    // 4.5 의 position 은 순환 없는 64비트 위치다. 32비트로 잘라 들고 있는 구현은 sequence 가
    // 한 바퀴 돈 뒤 position 과 pkt_pos 가 작아진다. 한 번에 2^31 미만씩 세 번 전진해 합이
    // 2^32 를 넘게 한다.
    ReplayWindow w = window_at(0);
    constexpr std::uint32_t kStep = 0x7FFFFFF0u;
    std::uint32_t seq = 0;
    for (int i = 0; i < 3; ++i) {
        seq += kStep;
        REQUIRE(w.accept(seq).verdict == ReplayVerdict::kAdvance);
    }
    constexpr std::uint64_t kExpected = std::uint64_t{kStep} * 3;
    STATIC_REQUIRE(kExpected > 0xFFFFFFFFull);
    REQUIRE(w.position() == kExpected);

    // 창 안의 재정렬 패킷의 pkt_pos 도 64비트 위치다.
    const ReplayDecision d = w.accept(seq - 5);
    REQUIRE(d.verdict == ReplayVerdict::kReorder);
    REQUIRE(d.pkt_pos == kExpected - 5);
}
