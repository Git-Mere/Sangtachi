#include "sangtachi/counters.hpp"

#include "sangtachi/log.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// 시험 케이스 이름은 ASCII 로만 적는다. 이유는 network/wsa_test.cpp 머리에 있다.

using sangtachi::Counter;
using sangtachi::Counters;
using sangtachi::format_all;
using sangtachi::kCounterCount;
using sangtachi::to_token;

TEST_CASE("counters: the table matches what the documents name", "[counters]") {
    // 이름은 protocol.md 6/7/8 장과 architecture.md 3.2.3 / 3.2.6 / 3.2.8 이 갖는다.
    // 개수가 바뀌면 이 줄을 먼저 보게 된다.
    REQUIRE(kCounterCount == 39);
}

TEST_CASE("counters: every token is unique, non-empty and sorted", "[counters]") {
    std::vector<std::string_view> tokens;
    for (std::size_t i = 0; i < kCounterCount; ++i) {
        const auto token = to_token(static_cast<Counter>(i));
        INFO("index=" << i);
        REQUIRE_FALSE(token.empty());
        REQUIRE(token != "unknown");
        tokens.push_back(token);
    }
    const std::set<std::string_view> unique(tokens.begin(), tokens.end());
    REQUIRE(unique.size() == tokens.size());

    // 이름 순으로 둔다. 전량 출력의 순서가 실행마다 같아야 두 기록을 나란히 볼 수 있다.
    for (std::size_t i = 1; i < tokens.size(); ++i) {
        INFO("previous=" << tokens[i - 1] << " current=" << tokens[i]);
        REQUIRE(tokens[i - 1] < tokens[i]);
    }
}

TEST_CASE("counters: known names are present", "[counters]") {
    std::set<std::string_view> tokens;
    for (std::size_t i = 0; i < kCounterCount; ++i) {
        tokens.insert(to_token(static_cast<Counter>(i)));
    }
    // Phase 1 검증이 직접 읽는 둘과, 문서가 예외로 다루는 하나.
    REQUIRE(tokens.count("drop_oversize_datagram") == 1);
    REQUIRE(tokens.count("console_queue_dropped") == 1);
    REQUIRE(tokens.count("tx_err_send") == 1);
}

TEST_CASE("counters: start at zero and count up", "[counters]") {
    Counters c;
    REQUIRE(c.value(Counter::DropMagic) == 0);
    c.increment(Counter::DropMagic);
    c.increment(Counter::DropMagic);
    REQUIRE(c.value(Counter::DropMagic) == 2);
    REQUIRE(c.value(Counter::DropShort) == 0);  // 다른 칸은 움직이지 않는다
    c.add(Counter::DropShort, 5);
    REQUIRE(c.value(Counter::DropShort) == 5);
}

TEST_CASE("counters: set replaces the value", "[counters]") {
    // [loop] 가 [console] 의 원자 변수를 읽어 넣는 자리다 (architecture.md 3.2.6).
    Counters c;
    c.increment(Counter::ConsoleQueueDropped);
    c.set(Counter::ConsoleQueueDropped, 17);
    REQUIRE(c.value(Counter::ConsoleQueueDropped) == 17);
}

TEST_CASE("counters: saturate instead of wrapping", "[counters]") {
    // 감싸 돌면 "오르지 않았다" 를 보는 검증이 통과한다.
    Counters c;
    c.set(Counter::DropMagic, UINT64_MAX - 1);
    c.add(Counter::DropMagic, 5);
    REQUIRE(c.value(Counter::DropMagic) == UINT64_MAX);
}

TEST_CASE("counters: the dump carries every counter including the zeros", "[counters]") {
    // architecture.md 9장: 값이 0 인 카운터도 함께 낸다.
    Counters c;
    c.add(Counter::DropMagic, 3);
    const auto lines = format_all(c);
    REQUIRE(lines.size() == kCounterCount);

    bool saw_nonzero = false;
    bool saw_zero = false;
    for (const auto& line : lines) {
        REQUIRE(line.rfind("INFO counter name=", 0) == 0);
        if (line == "INFO counter name=drop_magic value=3") {
            saw_nonzero = true;
        }
        if (line == "INFO counter name=drop_short value=0") {
            saw_zero = true;
        }
    }
    REQUIRE(saw_nonzero);
    REQUIRE(saw_zero);
}
