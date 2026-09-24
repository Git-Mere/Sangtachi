#include "hamychi/console.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

// 시험 케이스 이름은 ASCII 로만 적는다. 이유는 network/wsa_test.cpp 머리에 있다.

using hamychi::ConsoleQueue;
using hamychi::kConsoleQueueCapacity;

TEST_CASE("console: the queue holds exactly the documented number of lines", "[console]") {
    // architecture.md 3.2.6 이 16 줄로 정했다.
    REQUIRE(kConsoleQueueCapacity == 16);
}

TEST_CASE("console: lines come back in order", "[console]") {
    ConsoleQueue q;
    REQUIRE(q.try_push("first"));
    REQUIRE(q.try_push("second"));
    REQUIRE(q.size() == 2);
    REQUIRE(*q.try_pop() == "first");
    REQUIRE(*q.try_pop() == "second");
    REQUIRE_FALSE(q.try_pop().has_value());
    REQUIRE(q.size() == 0);
}

TEST_CASE("console: the seventeenth line is dropped, not the first", "[console]") {
    // roadmap.md Phase 1 검증 항목이다. 소비자를 멈춘 상태에서 본다.
    // 가장 오래된 것을 밀어내는 구현은 여기서 걸린다.
    ConsoleQueue q;
    for (std::size_t i = 0; i < kConsoleQueueCapacity; ++i) {
        INFO("line " << i);
        REQUIRE(q.try_push("line" + std::to_string(i)));
    }
    REQUIRE(q.size() == kConsoleQueueCapacity);
    REQUIRE(q.dropped() == 0);

    // 17번째.
    REQUIRE_FALSE(q.try_push("line16"));
    REQUIRE(q.dropped() == 1);
    REQUIRE(q.size() == kConsoleQueueCapacity);

    // 앞의 16줄이 순서대로 남아 있다.
    for (std::size_t i = 0; i < kConsoleQueueCapacity; ++i) {
        INFO("line " << i);
        REQUIRE(*q.try_pop() == "line" + std::to_string(i));
    }
    REQUIRE_FALSE(q.try_pop().has_value());
}

TEST_CASE("console: the drop counter keeps counting", "[console]") {
    ConsoleQueue q;
    for (std::size_t i = 0; i < kConsoleQueueCapacity; ++i) {
        REQUIRE(q.try_push("x"));
    }
    for (int i = 0; i < 5; ++i) {
        REQUIRE_FALSE(q.try_push("y"));
    }
    REQUIRE(q.dropped() == 5);
}

TEST_CASE("console: the queue wraps around", "[console]") {
    // 고리 버퍼가 감싸 돌아도 순서와 상한이 유지되는지 본다.
    ConsoleQueue q;
    for (int round = 0; round < 5; ++round) {
        for (std::size_t i = 0; i < kConsoleQueueCapacity; ++i) {
            REQUIRE(q.try_push("r" + std::to_string(round) + "_" + std::to_string(i)));
        }
        REQUIRE_FALSE(q.try_push("overflow"));
        for (std::size_t i = 0; i < kConsoleQueueCapacity; ++i) {
            REQUIRE(*q.try_pop() == "r" + std::to_string(round) + "_" + std::to_string(i));
        }
    }
    REQUIRE(q.dropped() == 5);
}
