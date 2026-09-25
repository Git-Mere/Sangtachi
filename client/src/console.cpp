#include "sangtachi/console.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <memory>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>

namespace sangtachi {

bool ConsoleQueue::try_push(std::string line) {
    const std::size_t tail = tail_.load(std::memory_order_relaxed);
    const std::size_t next = (tail + 1) % kSlots;
    if (next == head_.load(std::memory_order_acquire)) {
        // 가득 찼다. 새 줄을 버린다.
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    slots_[tail] = std::move(line);
    tail_.store(next, std::memory_order_release);
    return true;
}

std::optional<std::string> ConsoleQueue::try_pop() {
    const std::size_t head = head_.load(std::memory_order_relaxed);
    if (head == tail_.load(std::memory_order_acquire)) {
        return std::nullopt;
    }
    std::string line = std::move(slots_[head]);
    slots_[head].clear();
    head_.store((head + 1) % kSlots, std::memory_order_release);
    return line;
}

std::size_t ConsoleQueue::size() const noexcept {
    const std::size_t tail = tail_.load(std::memory_order_acquire);
    const std::size_t head = head_.load(std::memory_order_acquire);
    return (tail + kSlots - head) % kSlots;
}

std::shared_ptr<ConsoleSession> ConsoleSession::create() {
    // make_shared 를 쓰지 않는다. 생성자가 비공개다.
    std::shared_ptr<ConsoleSession> session(new ConsoleSession());
    // 자동 리셋이라 `[loop]` 가 별도 리셋 호출을 하지 않는다 (concurrency.md 3장).
    session->event_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (session->event_ == nullptr) {
        return nullptr;
    }
    return session;
}

ConsoleSession::~ConsoleSession() {
    if (event_ != nullptr) {
        ::CloseHandle(event_);
    }
}

}  // namespace sangtachi
