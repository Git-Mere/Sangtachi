#include "sangtachi/timer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// 시험 케이스 이름은 ASCII 로만 적는다. 이유는 network/wsa_test.cpp 머리에 있다.

using sangtachi::kInfiniteTimeout;
using sangtachi::Millis;
using sangtachi::next_timeout_ms;
using sangtachi::TimerSet;
using sangtachi::TimerTick;

TEST_CASE("timer: no deadline waits forever", "[timer]") {
    REQUIRE(next_timeout_ms(1000, std::nullopt) == kInfiniteTimeout);
}

TEST_CASE("timer: a deadline in the future is the remaining milliseconds", "[timer]") {
    REQUIRE(next_timeout_ms(1000, 1200) == 200);
    REQUIRE(next_timeout_ms(0, 1) == 1);
}

TEST_CASE("timer: a deadline that already passed waits zero", "[timer]") {
    // 뺄셈을 먼저 하면 여기서 언더플로해 거대한 값이 되고, 32비트로 좁히면 INFINITE 에
    // 착지해 루프가 영원히 깨지 않는다 (architecture.md 3.2.2 대기).
    REQUIRE(next_timeout_ms(1000, 1000) == 0);
    REQUIRE(next_timeout_ms(1000, 999) == 0);
    REQUIRE(next_timeout_ms(1000, 0) == 0);
    REQUIRE(next_timeout_ms(0xFFFFFFFFFFFFFFFFull, 0) == 0);
}

TEST_CASE("timer: a far deadline never lands on INFINITE", "[timer]") {
    // 좁히기가 0xFFFFFFFF 를 만들면 그것이 곧 INFINITE 다.
    REQUIRE(next_timeout_ms(0, static_cast<Millis>(kInfiniteTimeout)) == kInfiniteTimeout - 1);
    REQUIRE(next_timeout_ms(0, static_cast<Millis>(kInfiniteTimeout) + 1000) == kInfiniteTimeout - 1);
    REQUIRE(next_timeout_ms(0, 0xFFFFFFFFFFFFFFFFull) == kInfiniteTimeout - 1);
    // 경계 바로 아래는 그대로 나온다.
    REQUIRE(next_timeout_ms(0, static_cast<Millis>(kInfiniteTimeout) - 1) == kInfiniteTimeout - 1);
}

TEST_CASE("timer: the earliest deadline wins", "[timer]") {
    TimerSet timers;
    REQUIRE_FALSE(timers.earliest_deadline().has_value());

    timers.add_periodic("slow", 1000, 0);
    timers.add_periodic("fast", 200, 0);
    timers.add_periodic("mid", 500, 0);
    REQUIRE(timers.size() == 3);
    REQUIRE(timers.earliest_deadline() == 200u);
}

TEST_CASE("timer: an expired timer fires and reschedules", "[timer]") {
    TimerSet timers;
    timers.add_periodic("probe200", 200, 0);

    std::vector<TimerTick> fired;
    const auto record = [&fired](const TimerTick& tick) { fired.push_back(tick); };

    timers.run_expired(199, record);
    REQUIRE(fired.empty());  // 아직 마감 전이다

    timers.run_expired(200, record);  // 마감 비교는 deadline <= now 다
    REQUIRE(fired.size() == 1);
    REQUIRE(fired[0].name == "probe200");
    REQUIRE(fired[0].elapsed_ms == 200);
    REQUIRE(timers.earliest_deadline() == 400u);
}

TEST_CASE("timer: a late timer does not catch up", "[timer]") {
    // 판정 축이 횟수가 아니라 간격이다. 따라잡는 구현은 한 번의 run_expired 로 밀린
    // 주기를 모두 만료시키거나, 다음 마감을 과거에 둔다.
    TimerSet timers;
    timers.add_periodic("probe200", 200, 0);

    std::vector<TimerTick> fired;
    const auto record = [&fired](const TimerTick& tick) { fired.push_back(tick); };

    // 1초가 밀렸다. 주기 다섯 번 분량이다.
    timers.run_expired(1000, record);
    REQUIRE(fired.size() == 1);            // 다섯 번이 아니라 한 번이다
    REQUIRE(fired[0].elapsed_ms == 1000);  // 실제로 흐른 시간을 그대로 싣는다

    // 다음 마감이 과거가 아니라 now + interval 이다.
    REQUIRE(timers.earliest_deadline() == 1200u);
}

TEST_CASE("timer: elapsed is measured from the previous firing", "[timer]") {
    TimerSet timers;
    timers.add_periodic("probe200", 200, 0);

    std::vector<TimerTick> fired;
    const auto record = [&fired](const TimerTick& tick) { fired.push_back(tick); };

    timers.run_expired(250, record);
    timers.run_expired(500, record);
    REQUIRE(fired.size() == 2);
    REQUIRE(fired[0].elapsed_ms == 250);
    REQUIRE(fired[1].elapsed_ms == 250);  // 250 에서 500 까지다
}
