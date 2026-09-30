#include "sangtachi/timer.hpp"

#include <algorithm>
#include <cstdint>
#include <list>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

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

std::list<Timer>::const_iterator TimerSet::find(std::string_view name) const noexcept {
    for (auto it = timers_.begin(); it != timers_.end(); ++it) {
        // 취소 표시가 된 것은 없는 것으로 본다. 콜백이 끝나면 사라질 노드다.
        if (cancel_pending_ && it->id == firing_id_) {
            continue;
        }
        if (it->name == name) {
            return it;
        }
    }
    return timers_.end();
}

bool TimerSet::contains(std::string_view name) const noexcept {
    return find(name) != timers_.end();
}

bool TimerSet::add_periodic(std::string name, Millis interval_ms, Millis now) {
    if (contains(name)) {
        // 덮어쓰지 않는다. 이유는 timer.hpp 의 TimerSet 머리에 있다.
        return false;
    }
    Timer timer;
    timer.name = std::move(name);
    timer.interval_ms = interval_ms;
    timer.deadline = now + interval_ms;
    timer.last_fired = now;
    timer.repeating = true;
    timer.id = next_id_++;
    timers_.push_back(std::move(timer));
    return true;
}

bool TimerSet::add_once(std::string name, Millis delay_ms, Millis now) {
    if (contains(name)) {
        return false;
    }
    Timer timer;
    timer.name = std::move(name);
    timer.interval_ms = delay_ms;
    timer.deadline = now + delay_ms;
    timer.last_fired = now;
    timer.repeating = false;
    timer.id = next_id_++;
    timers_.push_back(std::move(timer));
    return true;
}

bool TimerSet::cancel(std::string_view name) noexcept {
    for (auto it = timers_.begin(); it != timers_.end(); ++it) {
        if (it->name != name) {
            continue;
        }
        if (cancel_pending_ && it->id == firing_id_) {
            // 이미 취소 표시가 된 노드다. 없는 것으로 본다.
            continue;
        }
        if (it->id == firing_id_) {
            // 지금 콜백이 돌고 있는 타이머다. 노드를 여기서 지우면 콜백이 들고 있는
            // TimerTick::name 이 그 자리에서 무효가 된다. 표시만 하고 run_expired 가
            // 콜백이 끝난 뒤에 지운다.
            cancel_pending_ = true;
            return true;
        }
        timers_.erase(it);
        return true;
    }
    return false;
}

std::optional<Millis> TimerSet::earliest_deadline() const noexcept {
    std::optional<Millis> earliest;
    for (const auto& timer : timers_) {
        if (cancel_pending_ && timer.id == firing_id_) {
            continue;
        }
        if (!earliest || timer.deadline < *earliest) {
            earliest = timer.deadline;
        }
    }
    return earliest;
}

void TimerSet::run_expired(Millis now, const std::function<void(const TimerTick&)>& on_tick) {
    // 이번 바퀴에 만료할 것을 먼저 정한다. 콜백이 타이머를 더해도 그것은 이번 목록에
    // 들지 않는다 (timer.hpp 의 run_expired 주석).
    std::vector<std::uint64_t> due;
    for (const auto& timer : timers_) {
        if (timer.deadline <= now) {
            due.push_back(timer.id);
        }
    }

    for (const std::uint64_t id : due) {
        auto it = timers_.begin();
        for (; it != timers_.end(); ++it) {
            if (it->id == id) {
                break;
            }
        }
        if (it == timers_.end()) {
            // 앞선 콜백이 취소했다. 돌지 않는다.
            continue;
        }

        const Millis elapsed = now - it->last_fired;
        if (!it->repeating) {
            // 일회성은 콜백 전에 집합에서 사라진다. 이름은 이 지역 변수가 콜백이 끝날
            // 때까지 살려 둔다.
            Timer expired = std::move(*it);
            timers_.erase(it);
            const TimerTick tick{expired.name, elapsed};
            on_tick(tick);
            continue;
        }

        it->last_fired = now;
        // 밀린 주기를 따라잡지 않는다. deadline += interval 이 아니다.
        it->deadline = now + it->interval_ms;
        const TimerTick tick{it->name, elapsed};

        firing_id_ = id;
        cancel_pending_ = false;
        on_tick(tick);
        const bool cancelled = cancel_pending_;
        firing_id_ = 0;
        cancel_pending_ = false;
        if (cancelled) {
            // 콜백이 자기 자신을 취소했다. 이제 노드를 지운다.
            timers_.erase(it);
        }
    }
}

}  // namespace sangtachi
