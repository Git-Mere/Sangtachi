#include "sangtachi/timer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
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
    // 착지해 루프가 영원히 깨지 않는다 (concurrency.md 2장 대기).
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

    REQUIRE(timers.add_periodic("slow", 1000, 0));
    REQUIRE(timers.add_periodic("fast", 200, 0));
    REQUIRE(timers.add_periodic("mid", 500, 0));
    REQUIRE(timers.size() == 3);
    REQUIRE(timers.earliest_deadline() == 200u);
}

TEST_CASE("timer: an expired timer fires and reschedules", "[timer]") {
    TimerSet timers;
    REQUIRE(timers.add_periodic("probe200", 200, 0));

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
    REQUIRE(timers.add_periodic("probe200", 200, 0));

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
    REQUIRE(timers.add_periodic("probe200", 200, 0));

    std::vector<TimerTick> fired;
    const auto record = [&fired](const TimerTick& tick) { fired.push_back(tick); };

    timers.run_expired(250, record);
    timers.run_expired(500, record);
    REQUIRE(fired.size() == 2);
    REQUIRE(fired[0].elapsed_ms == 250);
    REQUIRE(fired[1].elapsed_ms == 250);  // 250 에서 500 까지다
}

// --- 아래는 Phase 2 가 부르는 자리다 (plan.md "구현 때 같이 볼 자리"). --------------
// 값의 출처는 protocol.md 11장 타이머의 STUN 재시도(500ms, 1s, 2s)와 STUN 마감(5s)이다.

TEST_CASE("timer: adding a duplicate name is rejected, not overwritten", "[timer]") {
    // 조용히 덮어쓰면 먼저 걸린 마감이 사라진 것을 아무도 모른다 (timer.hpp).
    TimerSet timers;
    REQUIRE(timers.add_periodic("stun.retry.0", 500, 0));
    REQUIRE(timers.size() == 1);

    // 두 번째 add 는 거짓이고 기존 타이머를 건드리지 않는다.
    REQUIRE_FALSE(timers.add_periodic("stun.retry.0", 1000, 0));
    REQUIRE(timers.size() == 1);
    REQUIRE(timers.earliest_deadline() == 500u);

    // 일회성도 같은 이름 공간을 쓴다.
    REQUIRE_FALSE(timers.add_once("stun.retry.0", 5000, 0));
    REQUIRE(timers.size() == 1);
    REQUIRE(timers.earliest_deadline() == 500u);

    // 반대 방향도 같다.
    REQUIRE(timers.add_once("stun.deadline.0", 5000, 0));
    REQUIRE_FALSE(timers.add_periodic("stun.deadline.0", 200, 0));
    REQUIRE(timers.size() == 2);
    REQUIRE(timers.earliest_deadline() == 500u);
}

TEST_CASE("timer: contains reports what is scheduled", "[timer]") {
    TimerSet timers;
    REQUIRE_FALSE(timers.contains("stun.deadline.0"));
    REQUIRE(timers.add_once("stun.deadline.0", 5000, 0));
    REQUIRE(timers.contains("stun.deadline.0"));
    REQUIRE_FALSE(timers.contains("stun.deadline.1"));
}

TEST_CASE("timer: a one-shot fires once and then disappears", "[timer]") {
    TimerSet timers;
    REQUIRE(timers.add_once("stun.deadline.0", 5000, 0));
    REQUIRE(timers.earliest_deadline() == 5000u);

    std::vector<std::string> fired;
    std::vector<Millis> elapsed;
    std::size_t size_inside = 99;
    bool contains_inside = true;
    const auto record = [&](const TimerTick& tick) {
        fired.emplace_back(tick.name);
        elapsed.push_back(tick.elapsed_ms);
        // 일회성은 콜백 전에 사라진다.
        size_inside = timers.size();
        contains_inside = timers.contains("stun.deadline.0");
    };

    timers.run_expired(4999, record);
    REQUIRE(fired.empty());

    timers.run_expired(5000, record);  // 마감 비교는 deadline <= now 다
    REQUIRE(fired.size() == 1);
    REQUIRE(fired[0] == "stun.deadline.0");
    REQUIRE(elapsed[0] == 5000);
    REQUIRE(size_inside == 0);
    REQUIRE_FALSE(contains_inside);

    // 다시 잡지 않는다.
    REQUIRE(timers.size() == 0);
    REQUIRE_FALSE(timers.earliest_deadline().has_value());
    timers.run_expired(100000, record);
    REQUIRE(fired.size() == 1);
}

TEST_CASE("timer: cancel by name removes the timer before it fires", "[timer]") {
    TimerSet timers;
    REQUIRE(timers.add_once("stun.deadline.0", 5000, 0));
    REQUIRE(timers.add_periodic("stun.retry.0", 500, 0));

    REQUIRE_FALSE(timers.cancel("stun.deadline.1"));  // 없는 이름
    REQUIRE(timers.size() == 2);

    REQUIRE(timers.cancel("stun.retry.0"));
    REQUIRE(timers.size() == 1);
    REQUIRE_FALSE(timers.contains("stun.retry.0"));
    REQUIRE(timers.earliest_deadline() == 5000u);

    std::vector<std::string> fired;
    const auto record = [&fired](const TimerTick& tick) { fired.emplace_back(tick.name); };
    timers.run_expired(5000, record);
    REQUIRE(fired.size() == 1);
    REQUIRE(fired[0] == "stun.deadline.0");  // 취소한 것은 돌지 않는다

    // 취소한 이름은 다시 쓸 수 있다.
    REQUIRE(timers.add_periodic("stun.retry.0", 500, 5000));
    REQUIRE(timers.earliest_deadline() == 5500u);
}

TEST_CASE("timer: a callback can cancel another timer due in the same round", "[timer]") {
    // 응답 하나가 같은 바퀴에 만료할 다른 타이머를 지운다. STUN 응답이 그 서버의 재시도와
    // 마감을 함께 취소하는 경로다 (protocol.md 11장 타이머).
    //
    // 취소한 것 뒤에 만료할 타이머를 하나 더 둔다. 그것이 그대로 도는지까지 봐야 "그
    // 하나만 빠진다" 가 된다.
    TimerSet timers;
    REQUIRE(timers.add_periodic("first", 100, 0));
    REQUIRE(timers.add_periodic("second", 100, 0));
    REQUIRE(timers.add_periodic("third", 100, 0));

    std::vector<std::string> fired;
    const auto record = [&](const TimerTick& tick) {
        fired.emplace_back(tick.name);
        if (fired.size() == 1) {
            REQUIRE(timers.cancel("second"));
        }
    };

    timers.run_expired(100, record);
    REQUIRE(fired.size() == 2);
    REQUIRE(fired[0] == "first");
    REQUIRE(fired[1] == "third");
    REQUIRE_FALSE(timers.contains("second"));
    REQUIRE(timers.size() == 2);
}

TEST_CASE("timer: a callback can cancel the timer that is firing", "[timer]") {
    TimerSet timers;
    REQUIRE(timers.add_periodic("stun.retry.0", 500, 0));

    std::vector<std::string> fired;
    std::string name_after_cancel;
    const auto record = [&](const TimerTick& tick) {
        fired.emplace_back(tick.name);
        REQUIRE(timers.cancel("stun.retry.0"));
        // 취소한 뒤에도 콜백이 받은 이름은 읽을 수 있다.
        name_after_cancel.assign(tick.name);
        // 취소 직후에 없는 것으로 보인다.
        REQUIRE_FALSE(timers.contains("stun.retry.0"));
        REQUIRE(timers.size() == 0);
    };

    timers.run_expired(500, record);
    REQUIRE(fired.size() == 1);
    REQUIRE(name_after_cancel == "stun.retry.0");
    REQUIRE(timers.size() == 0);
    REQUIRE_FALSE(timers.earliest_deadline().has_value());

    timers.run_expired(10000, record);
    REQUIRE(fired.size() == 1);  // 다시 돌지 않는다
}

TEST_CASE("timer: a timer added from a callback waits for the next round", "[timer]") {
    // 마감이 이미 지난 타이머를 콜백이 더해도 이번 바퀴에는 돌지 않는다. 돌면 STUN 이
    // 다음 서버로 바꾸며 건 타이머가 같은 바퀴에 연쇄로 만료한다.
    //
    // 더한 것 뒤에 만료할 타이머가 하나 더 있어야 이 케이스가 뜻을 갖는다. 마지막
    // 타이머의 콜백에서 더하면, 살아 있는 목록을 그대로 순회하는 구현도 목록 끝에
    // 닿아 있어 새 타이머를 보지 못한 채 끝난다. 변이 시험으로 확인했다.
    TimerSet timers;
    REQUIRE(timers.add_once("stun.deadline.0", 5000, 0));
    REQUIRE(timers.add_periodic("keepalive", 5000, 0));

    std::vector<std::string> fired;
    const auto record = [&](const TimerTick& tick) {
        fired.emplace_back(tick.name);
        if (tick.name == "stun.deadline.0") {
            REQUIRE(timers.add_once("stun.retry.1", 0, 5000));  // 마감이 곧 지금이다
        }
    };

    timers.run_expired(5000, record);
    REQUIRE(fired.size() == 2);
    REQUIRE(fired[0] == "stun.deadline.0");
    REQUIRE(fired[1] == "keepalive");
    REQUIRE(timers.contains("stun.retry.1"));

    fired.clear();
    timers.run_expired(5000, record);  // 다음 바퀴
    REQUIRE(fired.size() == 1);
    REQUIRE(fired[0] == "stun.retry.1");
}

TEST_CASE("timer: two servers keep separate retry and deadline timers", "[timer]") {
    // protocol.md 11장 타이머의 시나리오 하나를 끝에서 끝까지 따라간다. 서버 둘에 동시에
    // 질의하고(architecture.md 3.5 기동 입력의 서버 선택) 서버 0 은 500ms 재시도 뒤에
    // 응답하고, 서버 1 은 5s 마감까지 침묵한다.
    TimerSet timers;
    REQUIRE(timers.add_once("stun.retry.0", 500, 0));
    REQUIRE(timers.add_once("stun.deadline.0", 5000, 0));
    REQUIRE(timers.add_once("stun.retry.1", 500, 0));
    REQUIRE(timers.add_once("stun.deadline.1", 5000, 0));
    REQUIRE(timers.size() == 4);
    REQUIRE(timers.earliest_deadline() == 500u);

    std::vector<std::string> fired;
    const auto record = [&fired](const TimerTick& tick) { fired.emplace_back(tick.name); };

    // 500ms. 두 서버의 첫 재시도가 만료하고 각자 다음 재시도(1s 뒤)를 건다.
    timers.run_expired(500, record);
    REQUIRE(fired.size() == 2);
    REQUIRE(fired[0] == "stun.retry.0");
    REQUIRE(fired[1] == "stun.retry.1");
    REQUIRE(timers.add_once("stun.retry.0", 1000, 500));
    REQUIRE(timers.add_once("stun.retry.1", 1000, 500));

    // 서버 0 의 응답이 왔다. 그 서버의 재시도와 마감만 지운다.
    REQUIRE(timers.cancel("stun.retry.0"));
    REQUIRE(timers.cancel("stun.deadline.0"));
    REQUIRE(timers.size() == 2);
    REQUIRE(timers.contains("stun.retry.1"));
    REQUIRE(timers.contains("stun.deadline.1"));

    // 1.5s. 서버 1 의 두 번째 재시도. 서버 0 은 아무것도 남지 않았다.
    fired.clear();
    timers.run_expired(1500, record);
    REQUIRE(fired.size() == 1);
    REQUIRE(fired[0] == "stun.retry.1");
    REQUIRE(timers.add_once("stun.retry.1", 2000, 1500));

    // 3.5s. 세 번째이자 마지막 재시도.
    fired.clear();
    timers.run_expired(3500, record);
    REQUIRE(fired.size() == 1);
    REQUIRE(fired[0] == "stun.retry.1");

    // 5s. 서버 1 의 마감만 남아 만료한다.
    fired.clear();
    REQUIRE(timers.size() == 1);
    REQUIRE(timers.earliest_deadline() == 5000u);
    timers.run_expired(5000, record);
    REQUIRE(fired.size() == 1);
    REQUIRE(fired[0] == "stun.deadline.1");
    REQUIRE(timers.size() == 0);
}

TEST_CASE("timer: a periodic timer keeps firing while a one-shot does not", "[timer]") {
    TimerSet timers;
    REQUIRE(timers.add_periodic("keepalive", 1000, 0));
    REQUIRE(timers.add_once("once", 1000, 0));

    std::vector<std::string> fired;
    const auto record = [&fired](const TimerTick& tick) { fired.emplace_back(tick.name); };

    timers.run_expired(1000, record);
    REQUIRE(fired.size() == 2);
    REQUIRE(timers.size() == 1);
    REQUIRE(timers.earliest_deadline() == 2000u);

    fired.clear();
    timers.run_expired(2000, record);
    REQUIRE(fired.size() == 1);
    REQUIRE(fired[0] == "keepalive");
}
