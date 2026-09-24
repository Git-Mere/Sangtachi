#include "sangtachi/timer.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>

namespace sangtachi {

std::uint32_t next_timeout_ms(Millis now, std::optional<Millis> deadline) noexcept {
    if (!deadline) {
        return kInfiniteTimeout;
    }
    // 비교가 먼저다. 뺄셈을 먼저 하면 언더플로한다.
    if (*deadline <= now) {
        return 0;
    }
    const Millis remaining = *deadline - now;
    // INFINITE 에 착지하지 않게 1 을 뺀 값으로 자른다.
    const Millis capped = std::min<Millis>(remaining, kInfiniteTimeout - 1);
    return static_cast<std::uint32_t>(capped);
}

void TimerSet::add_periodic(std::string name, Millis interval_ms, Millis now) {
    Timer timer;
    timer.name = std::move(name);
    timer.interval_ms = interval_ms;
    timer.deadline = now + interval_ms;
    timer.last_fired = now;
    timers_.push_back(std::move(timer));
}

std::optional<Millis> TimerSet::earliest_deadline() const noexcept {
    std::optional<Millis> earliest;
    for (const auto& timer : timers_) {
        if (!earliest || timer.deadline < *earliest) {
            earliest = timer.deadline;
        }
    }
    return earliest;
}

void TimerSet::run_expired(Millis now, const std::function<void(const TimerTick&)>& on_tick) {
    for (auto& timer : timers_) {
        if (timer.deadline > now) {
            continue;
        }
        const TimerTick tick{timer.name, now - timer.last_fired};
        timer.last_fired = now;
        // 밀린 주기를 따라잡지 않는다. deadline += interval 이 아니다.
        timer.deadline = now + timer.interval_ms;
        on_tick(tick);
    }
}

}  // namespace sangtachi
