#pragma once

// 타이머 (architecture.md 3.2.4 타이머, 3.2.2 대기).
//
// 단조 시계는 GetTickCount64 다. 밀리초 단위 64비트라 실질적으로 랩어라운드가 없다.
// RTT 측정만 QueryPerformanceCounter 를 쓰고 두 시계의 값을 서로 비교하지 않는다.
//
// 타이머 집합이 작으므로 매 바퀴 선형 주사로 가장 이른 마감을 구한다. 타이머 휠은 쓰지
// 않는다 (3.2.4).

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sangtachi {

using Millis = std::uint64_t;

// WaitForMultipleObjects 의 INFINITE 와 같은 값. 헤더를 들이지 않으려고 여기 둔다.
inline constexpr std::uint32_t kInfiniteTimeout = 0xFFFFFFFFu;

// 다음 대기의 타임아웃을 구한다.
//
// **뺄셈을 먼저 하면 안 된다.** GetTickCount64 는 부호 없는 값이라 마감이 이미 지났을 때
// 뺄셈이 언더플로해 거대한 값이 되고, 32비트로 좁히면 0xFFFFFFFF 즉 INFINITE 에 착지할 수
// 있다. 그러면 루프가 영원히 깨지 않는다.
//
// now 를 인자로 받는 이유도 같다. 함수 안에서 두 번 읽으면 비교와 뺄셈 사이에 시계가
// 마감을 지나가 같은 언더플로가 난다.
[[nodiscard]] std::uint32_t next_timeout_ms(Millis now, std::optional<Millis> deadline) noexcept;

// 주기 타이머 하나.
struct Timer {
    std::string name;
    Millis interval_ms = 0;
    Millis deadline = 0;
    Millis last_fired = 0;
};

// 만료한 타이머에 넘기는 값. elapsed_ms 는 직전 만료로부터 실제로 흐른 밀리초다
// (architecture.md 9장의 timer.tick 이 싣는 값).
struct TimerTick {
    std::string_view name;
    Millis elapsed_ms = 0;
};

class TimerSet {
public:
    // 주기 타이머를 더한다. 첫 만료는 now + interval_ms 다.
    void add_periodic(std::string name, Millis interval_ms, Millis now);

    [[nodiscard]] std::optional<Millis> earliest_deadline() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept { return timers_.size(); }

    // 마감이 지난 타이머를 돌린다. 마감 비교는 엄격 부등호가 아니라 `deadline <= now` 다
    // (protocol.md 11장 타이머).
    //
    // **밀린 주기를 따라잡지 않는다.** 다음 마감은 `직전 마감 + 간격` 이 아니라
    // `now + 간격` 이다. 따라잡으면 부하가 걷힌 뒤 한 바퀴에 여러 번 만료해 그 사이
    // 수신이 굶는다.
    void run_expired(Millis now, const std::function<void(const TimerTick&)>& on_tick);

private:
    std::vector<Timer> timers_;
};

}  // namespace sangtachi
