#pragma once

// 타이머 (concurrency.md 4장 타이머, 2장 대기).
//
// 단조 시계는 platform::monotonic_ms 다. 밀리초 단위 64비트라 실질적으로 랩어라운드가
// 없다. RTT 측정만 QueryPerformanceCounter 를 쓰고 두 시계의 값을 서로 비교하지 않는다.
//
// 타이머 집합이 작으므로 매 바퀴 선형 주사로 가장 이른 마감을 구한다. 타이머 휠은 쓰지
// 않는다 (concurrency.md 4장).

#include "sangtachi/platform/wait.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <optional>
#include <string>
#include <string_view>

namespace sangtachi {

using Millis = std::uint64_t;

// 무한 대기 타임아웃. 값의 출처는 platform/wait.hpp 의 kInfiniteWaitMs 다.
inline constexpr std::uint32_t kInfiniteTimeout = platform::kInfiniteWaitMs;

// 다음 대기의 타임아웃을 구한다.
//
// **뺄셈을 먼저 하면 안 된다.** GetTickCount64 는 부호 없는 값이라 마감이 이미 지났을 때
// 뺄셈이 언더플로해 거대한 값이 되고, 32비트로 좁히면 0xFFFFFFFF 즉 INFINITE 에 착지할 수
// 있다. 그러면 루프가 영원히 깨지 않는다.
//
// now 를 인자로 받는 이유도 같다. 함수 안에서 두 번 읽으면 비교와 뺄셈 사이에 시계가
// 마감을 지나가 같은 언더플로가 난다.
[[nodiscard]] std::uint32_t next_timeout_ms(Millis now, std::optional<Millis> deadline) noexcept;

// 타이머 하나. 주기와 일회성을 같은 구조로 담는다.
struct Timer {
    std::string name;
    Millis interval_ms = 0;
    Millis deadline = 0;
    Millis last_fired = 0;
    // 참이면 만료 후 다시 잡는다. 거짓이면 만료와 함께 집합에서 사라진다.
    bool repeating = true;
    // 집합 안에서만 뜻이 있는 일련번호. 이름과 달리 재사용하지 않는다.
    // run_expired 가 이번 바퀴에 만료할 것을 이 번호로 들고 있다가 다시 찾는다.
    // 콜백이 취소하고 같은 이름을 다시 더해도 그것은 다른 타이머다.
    std::uint64_t id = 0;
};

// 만료한 타이머에 넘기는 값. elapsed_ms 는 직전 만료로부터 실제로 흐른 밀리초다
// (architecture.md 9장의 timer.tick 이 싣는 값). 일회성 타이머의 첫 만료는 add_once 의
// now 로부터 흐른 시간이다.
//
// name 이 가리키는 문자열은 콜백이 도는 동안만 유효하다. 콜백 밖으로 들고 나가려면
// 복사한다. 일회성 타이머는 만료와 함께 집합에서 사라지므로 그 이름의 수명이 콜백
// 호출 한 번이다.
struct TimerTick {
    std::string_view name;
    Millis elapsed_ms = 0;
};

// 이름 규칙은 이 집합이 정한다. 문서가 정해 준 것이 아니다.
//
// **이름은 집합 전체에서 유일하다.** 주기 타이머와 일회성 타이머가 같은 이름 공간을
// 쓴다. 이미 있는 이름을 다시 더하면 **더하지 않고 거짓을 돌려준다.** 기존 타이머는
// 간격도 마감도 그대로다.
//
// 왜 거부인가. 조용히 덮어쓰면 먼저 걸린 마감이 사라진 것을 아무도 모른다. STUN 은
// 서버 둘에 동시에 질의하므로 (architecture.md 3.5 기동 입력의 서버 선택) 같은 종류의
// 타이머가 서버마다 따로 산다. 이름을 가르는 것은 부르는 쪽 일이고, 가르지 못한 것을
// 이 집합이 조용히 삼키면 한 서버의 마감이 다른 서버의 요청에 지워진다.
//
// 왜 예외가 아닌가. 이름이 겹치는 것은 부르는 쪽의 결함이지 실행 중 상황이 아니지만,
// 타이머 하나 때문에 프로세스를 세우지 않는다. 반환값으로 알리고 판단을 맡긴다
// (network/wsa.hpp 가 적은 것과 같은 기준이다. 기동을 막는 것만 예외다).
class TimerSet {
public:
    // 주기 타이머를 더한다. 첫 만료는 now + interval_ms 다.
    //
    // 더했으면 참, 같은 이름이 이미 있어 더하지 않았으면 거짓이다.
    //
    // add_once 와 같은 기준으로 [[nodiscard]] 다. 반환값을 버리면 이름이 겹쳐 더하지
    // 못한 것을 아무도 모르고, 그 자리의 마감은 영영 오지 않는다.
    [[nodiscard]] bool add_periodic(std::string name, Millis interval_ms, Millis now);

    // 일회성 타이머를 더한다. 만료는 now + delay_ms 한 번이고, 만료와 함께 집합에서
    // 사라진다. 사라지는 시점은 콜백을 부르기 전이라 콜백 안에서 contains 가 거짓이다.
    //
    // 더했으면 참, 같은 이름이 이미 있어 더하지 않았으면 거짓이다.
    [[nodiscard]] bool add_once(std::string name, Millis delay_ms, Millis now);

    // 이름으로 지운다. 있었으면 참, 없었으면 거짓이다.
    //
    // [[nodiscard]] 를 붙이지 않는다. 없는 것을 지우는 것이 정상 경로다. STUN 응답은
    // 그 서버의 마감이 이미 만료한 뒤에도 올 수 있고, 그때 취소는 할 일이 없다
    // (protocol.md 11장 타이머).
    //
    // run_expired 의 콜백 안에서 불러도 된다. 아직 만료하지 않은 타이머를 지우면 그
    // 타이머는 이번 바퀴에 돌지 않는다. **지금 돌고 있는 타이머를 지워도 된다.** 그
    // 경우 노드는 콜백이 끝난 뒤에 사라진다. 콜백이 들고 있는 TimerTick::name 을 그
    // 자리에서 무효로 만들지 않으려는 것이다. 그때도 이 함수가 돌아온 뒤로는 contains
    // 가 거짓이고 size 가 그것을 세지 않으며, 같은 이름을 다시 더할 수 있다.
    bool cancel(std::string_view name) noexcept;

    [[nodiscard]] bool contains(std::string_view name) const noexcept;

    [[nodiscard]] std::optional<Millis> earliest_deadline() const noexcept;

    // 살아 있는 타이머 수. 취소 표시만 된 것은 세지 않는다.
    [[nodiscard]] std::size_t size() const noexcept {
        return timers_.size() - (cancel_pending_ ? 1u : 0u);
    }

    // 마감이 지난 타이머를 돌린다. 마감 비교는 `deadline <= now` 다. 지난 마감뿐 아니라
    // 정확히 지금인 마감도 만료한다 (protocol.md 11장 타이머). next_timeout_ms 의 비교도
    // 같은 부등호다. 둘이 어긋나면 마감이 정확히 지금인 바퀴에서 타이머는 돌지 않는데
    // 대기는 0 으로 즉시 반환해 같은 바퀴를 반복한다.
    //
    // **밀린 주기를 따라잡지 않는다.** 다음 마감은 `직전 마감 + 간격` 이 아니라
    // `now + 간격` 이다. 따라잡으면 부하가 걷힌 뒤 한 바퀴에 여러 번 만료해 그 사이
    // 수신이 굶는다. 일회성 타이머는 한 번 돌고 사라지므로 이 규칙과 무관하다.
    //
    // **콜백 안에서 더하고 지워도 된다.** STUN 은 마감이 만료하면 다음 서버로 바꾸며
    // 그 자리에서 타이머를 새로 건다 (architecture.md 3.5 기동 입력의 서버 선택).
    // 이번 바퀴에 만료할 것은 콜백을 부르기 전에 정해지므로 콜백이 더한 타이머는
    // 마감이 이미 지났더라도 이번 바퀴에 돌지 않는다. 다음 바퀴에 돈다.
    //
    // **콜백 안에서 run_expired 를 다시 부르지 않는다.** 지금 돌고 있는 타이머를 하나만
    // 기억하므로 겹쳐 부르면 그 기억이 덮인다. 루프는 한 바퀴에 한 번만 부른다
    // (concurrency.md 3장 루프 한 바퀴).
    void run_expired(Millis now, const std::function<void(const TimerTick&)>& on_tick);

private:
    // 벡터가 아니라 리스트다. 콜백이 타이머를 더하면 벡터는 다시 할당하면서 원소를
    // 옮기고, 그러면 그 바퀴에 이미 넘긴 TimerTick::name 이 가리키던 자리가 사라진다
    // (짧은 이름은 std::string 안에 들어 있어 원소와 함께 움직인다). 리스트는 지운
    // 원소 말고는 자리가 움직이지 않는다. 집합이 작아 선형 주사라는 전제는 그대로다
    // (concurrency.md 4장 타이머).
    // 이름으로 찾는다. 취소 표시가 된 것은 없는 것으로 본다.
    [[nodiscard]] std::list<Timer>::const_iterator find(std::string_view name) const noexcept;

    std::list<Timer> timers_;
    std::uint64_t next_id_ = 1;
    // run_expired 가 지금 콜백을 돌리고 있는 타이머의 일련번호. 없으면 0 이다.
    std::uint64_t firing_id_ = 0;
    // 그 타이머를 콜백이 취소했는가. 참이면 콜백이 끝난 뒤 지운다. 한 번에 하나뿐이다.
    bool cancel_pending_ = false;
};

}  // namespace sangtachi
