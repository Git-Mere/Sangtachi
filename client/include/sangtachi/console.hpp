#pragma once

// 콘솔 명령 큐와 읽기 스레드 (architecture.md 3.2.6 텔레메트리 격리, 3.2.1 스레드).
//
// `[loop]` 가 표준 입력에서 블록할 수 없으므로 별도 스레드가 읽어 큐에 넣는다. 큐는
// 16 줄이고 가득 차면 **새 줄을 버린다.** 가장 오래된 것을 밀어내지 않는다. 그러려면
// 생산자가 소비자의 tail 을 움직여야 해서 락이 필요하고, 지표 하나 더 살리자고 `[loop]`
// 에 락을 들이지 않는다.
//
// 버린 수를 세는 `console_queue_dropped` 는 3.2.5 단독 소유 규칙의 **유일한 예외**다.
// 버리는 쪽이 생산자인 `[console]` 이라 그 스레드가 올린다. 원자 변수 하나이고 `[loop]`
// 는 읽기만 한다.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace sangtachi {

// architecture.md 3.2.6 이 정한 줄 수.
inline constexpr std::size_t kConsoleQueueCapacity = 16;

// 생산자 하나, 소비자 하나. 원자 head/tail 만으로 락 없이 돈다.
class ConsoleQueue {
public:
    // 생산자(`[console]`)만 부른다. 가득 차면 버리고 거짓을 돌려준다.
    bool try_push(std::string line);

    // 소비자(`[loop]`)만 부른다. 비어 있으면 값이 없다.
    [[nodiscard]] std::optional<std::string> try_pop();

    [[nodiscard]] std::uint64_t dropped() const noexcept {
        return dropped_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::size_t size() const noexcept;

private:
    // 한 칸을 비워 둔다. head == tail 이 "빈 것" 하나만 뜻하게 하려는 것이다.
    static constexpr std::size_t kSlots = kConsoleQueueCapacity + 1;

    std::array<std::string, kSlots> slots_;
    std::atomic<std::size_t> head_{0};  // 소비자가 움직인다
    std::atomic<std::size_t> tail_{0};  // 생산자가 움직인다
    std::atomic<std::uint64_t> dropped_{0};
};

// `[console]` 이 쓰는 상태를 한 덩어리로 묶는다.
//
// **이 객체는 `[loop]` 보다 오래 산다.** 3.2.7 종료가 정한 대로 `[console]` 은 join 하지
// 않는다. 표준 입력 읽기를 밖에서 취소하는 수단을 쓰지 않으므로 join 하면 사용자가 한 줄을
// 더 칠 때까지 종료가 멈춘다.
//
// join 하지 않는다는 것은 그 스레드가 `main` 이 빠져나간 뒤에도 깨어날 수 있다는 뜻이다.
// 그때 건드릴 것이 스택 위에 있으면 해제된 것을 쓴다. 그래서 큐와 이벤트와 중단 표시를
// 여기 모으고, 스레드가 `shared_ptr` 를 값으로 들고 가 스스로 수명을 붙든다.
class ConsoleSession {
public:
    // 자동 리셋 이벤트를 만든다. 실패하면 널을 돌려준다.
    [[nodiscard]] static std::shared_ptr<ConsoleSession> create();

    ~ConsoleSession();

    ConsoleSession(const ConsoleSession&) = delete;
    ConsoleSession& operator=(const ConsoleSession&) = delete;
    ConsoleSession(ConsoleSession&&) = delete;
    ConsoleSession& operator=(ConsoleSession&&) = delete;

    [[nodiscard]] ConsoleQueue& queue() noexcept { return queue_; }

    // `[loop]` 가 대기 집합에 넣는 핸들. 소유는 이 객체가 한다.
    [[nodiscard]] void* event() const noexcept { return event_; }

    void request_stop() noexcept { stop_.store(true, std::memory_order_relaxed); }
    [[nodiscard]] bool stopped() const noexcept {
        return stop_.load(std::memory_order_relaxed);
    }

private:
    ConsoleSession() = default;

    ConsoleQueue queue_;
    std::atomic<bool> stop_{false};
    void* event_ = nullptr;
};

}  // namespace sangtachi
