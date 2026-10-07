#include "sangtachi/spsc_ring.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <thread>

// 시험 케이스 이름은 ASCII 로만 적는다. 이유는 network/wsa_test.cpp 머리에 있다.

using sangtachi::SpscRing;

TEST_CASE("spsc_ring: empty pop has no value", "[spsc_ring]") {
    SpscRing<int, 8> ring;
    REQUIRE_FALSE(ring.try_pop().has_value());
    REQUIRE(ring.size() == 0);
}

TEST_CASE("spsc_ring: holds exactly the capacity and drops the next", "[spsc_ring]") {
    // concurrency.md 8장 `[control]` 스레드: 8 항목. 가득 차면 새 항목을 버린다 (6장).
    SpscRing<int, 8> ring;
    for (int i = 0; i < 8; ++i) {
        REQUIRE(ring.try_push(int{i}));
    }
    REQUIRE(ring.size() == 8);
    REQUIRE_FALSE(ring.try_push(99));
    REQUIRE(ring.size() == 8);

    // 버린 것은 새 항목이다. 남은 것은 처음 넣은 여덟이다.
    for (int i = 0; i < 8; ++i) {
        const auto v = ring.try_pop();
        REQUIRE(v.has_value());
        REQUIRE(*v == i);
    }
    REQUIRE_FALSE(ring.try_pop().has_value());
}

TEST_CASE("spsc_ring: a failed push does not consume the value", "[spsc_ring]") {
    // 부르는 쪽이 같은 값을 다시 넣을 수 있어야 한다 (control/channel.cpp 의 deliver).
    SpscRing<std::string, 1> ring;
    REQUIRE(ring.try_push(std::string("first")));
    std::string second = "second";
    REQUIRE_FALSE(ring.try_push(std::move(second)));
    REQUIRE(second == "second");  // NOLINT(bugprone-use-after-move): 옮기지 않았음을 본다
    REQUIRE(*ring.try_pop() == "first");
    REQUIRE(ring.try_push(std::move(second)));
    REQUIRE(*ring.try_pop() == "second");
}

TEST_CASE("spsc_ring: wraps around many times in order", "[spsc_ring]") {
    SpscRing<int, 8> ring;
    int next_in = 0;
    int next_out = 0;
    for (int round = 0; round < 100; ++round) {
        const int burst = (round % 8) + 1;
        for (int i = 0; i < burst; ++i) {
            REQUIRE(ring.try_push(int{next_in++}));
        }
        for (int i = 0; i < burst; ++i) {
            const auto v = ring.try_pop();
            REQUIRE(v.has_value());
            REQUIRE(*v == next_out++);
        }
        REQUIRE(ring.size() == 0);
    }
}

TEST_CASE("spsc_ring: one producer and one consumer thread keep order", "[spsc_ring]") {
    SpscRing<int, 8> ring;
    constexpr int kTotal = 200000;
    std::thread producer([&ring] {
        for (int i = 0; i < kTotal;) {
            if (ring.try_push(int{i})) {
                ++i;
            } else {
                std::this_thread::yield();
            }
        }
    });
    int expected = 0;
    bool in_order = true;
    while (expected < kTotal) {
        if (const auto v = ring.try_pop()) {
            if (*v != expected) {
                in_order = false;
            }
            ++expected;
        } else {
            std::this_thread::yield();
        }
    }
    producer.join();
    REQUIRE(in_order);
    REQUIRE_FALSE(ring.try_pop().has_value());
}
