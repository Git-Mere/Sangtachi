#pragma once

// 생산자 하나, 소비자 하나의 고정 크기 링 (concurrency.md 6장 텔레메트리 격리, 8장 `[control]`
// 스레드).
//
// 6장이 정한 구현을 그대로 옮긴다.
//   - 원자 head/tail 만으로 **락 없이** 돈다. 생산자와 소비자가 하나씩이라 그것으로 충분하다
//   - 가득 차면 **새 항목을 버린다.** 가장 오래된 것을 밀어내지 않는다. 그러려면 생산자가
//     소비자의 tail 을 움직여야 해서 락이 필요하다
//   - 버린 수를 세는 것은 부르는 쪽이다. 카운터의 이름과 소유 스레드가 큐마다 다르다
//     (concurrency.md 6장의 telemetry_queue_dropped, console_queue_dropped, 8장의
//     control_queue_dropped)
//
// **종료 표식을 넣지 않는다** (concurrency.md 7장 종료). 가득 찼을 때 그 표식이 버려지면
// 소비자가 종료를 영영 못 본다. 종료는 링 밖의 이벤트로만 전한다.
//
// 콘솔 명령 큐(console.hpp)는 이 템플릿보다 먼저 같은 설계로 따로 쓰였다. 합치는 일은 이
// 파일의 범위 밖이다.

#include <array>
#include <atomic>
#include <cstddef>
#include <optional>
#include <utility>

namespace sangtachi {

template <class T, std::size_t Capacity>
class SpscRing {
    static_assert(Capacity > 0, "capacity must be positive");

public:
    static constexpr std::size_t kCapacity = Capacity;

    // 생산자만 부른다. 가득 차면 넣지 않고 거짓을 돌려준다. 값은 그때 옮겨지지 않는다.
    [[nodiscard]] bool try_push(T&& value) {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t next = (tail + 1) % kSlots;
        if (next == head_.load(std::memory_order_acquire)) {
            return false;  // 가득 찼다. 새 항목을 버린다
        }
        slots_[tail] = std::move(value);
        // release: 위의 쓰기가 소비자의 acquire 보다 먼저 보이게 한다.
        tail_.store(next, std::memory_order_release);
        return true;
    }

    // 소비자만 부른다. 비어 있으면 값이 없다.
    [[nodiscard]] std::optional<T> try_pop() {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        if (head == tail_.load(std::memory_order_acquire)) {
            return std::nullopt;
        }
        std::optional<T> out(std::move(slots_[head]));
        slots_[head] = T{};  // 옮긴 뒤의 껍데기가 자원을 붙들지 않게 한다
        head_.store((head + 1) % kSlots, std::memory_order_release);
        return out;
    }

    // 어느 스레드에서 읽어도 되지만 읽는 순간 이미 낡을 수 있다. 시험과 진단용이다.
    [[nodiscard]] std::size_t size() const noexcept {
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        const std::size_t head = head_.load(std::memory_order_acquire);
        return (tail + kSlots - head) % kSlots;
    }

private:
    // 한 칸을 비워 둔다. head == tail 이 "빈 것" 하나만 뜻하게 하려는 것이다. 그래서 담을
    // 수 있는 항목 수는 Capacity 그대로다.
    static constexpr std::size_t kSlots = Capacity + 1;

    std::array<T, kSlots> slots_{};
    std::atomic<std::size_t> head_{0};  // 소비자가 움직인다
    std::atomic<std::size_t> tail_{0};  // 생산자가 움직인다
};

}  // namespace sangtachi
