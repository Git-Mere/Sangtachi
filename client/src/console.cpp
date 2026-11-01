#include "sangtachi/console.hpp"

#include "sangtachi/platform/wait.hpp"

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
    session->event_ = platform::create_event(platform::ResetMode::Auto);
    if (session->event_ == nullptr) {
        return nullptr;
    }
    return session;
}

ConsoleSession::~ConsoleSession() {
    platform::close_event(event_);
}

void ConsolePrompt::line(std::string_view text) {
    clear_status();
    std::string out = "\r";
    out += text;
    write_(out, true);
    showing_ = false;
    show();
}

void ConsolePrompt::show() {
    clear_status();
    if (showing_ || closed_) {
        return;
    }
    write_("> ", false);
    showing_ = true;
}

void ConsolePrompt::close() {
    clear_status();
    if (showing_) {
        write_("\r  \r", false);
        showing_ = false;
    }
    closed_ = true;
}

void ConsolePrompt::status(std::string_view text) {
    write_(text, false);
    status_width_.store(text.size(), std::memory_order_relaxed);
}

void ConsolePrompt::clear_status() {
    // 지우는 쪽이 하나가 되게 꺼내면서 비운다.
    const std::size_t width = status_width_.exchange(0, std::memory_order_relaxed);
    if (width == 0) {
        return;
    }
    std::string out = "\r";
    out.append(width, ' ');
    out += '\r';
    write_(out, false);
}

}  // namespace sangtachi
