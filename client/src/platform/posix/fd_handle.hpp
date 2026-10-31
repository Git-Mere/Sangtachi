#pragma once

// 플랫폼 내부 헤더: 파일 기술자를 공개 헤더의 void* 핸들에 담는 규칙 (ADR 0035 결정 3).
//
// 대기 핸들 = (wfd + 1) << 32 | (rfd + 1) << 1 | auto 비트.
//   - 이벤트는 파이프 한 쌍이라 두 fd 를 다 담는다. auto 비트가 서면 대기가 신호를 소비한다
//   - 소켓과 TUN 의 읽기 핸들은 wfd 자리가 0, auto 비트가 0 이다 (수준형)
// 널은 무효다. fd 0 도 +1 이라 널이 되지 않는다. 역참조하지 않으므로 가짜 값은 poll 이 거른다(음수 fd 는 wait_any 가 거른다).

#include <cstdint>

namespace sangtachi::platform::posix {

struct EventFds {
    int read_fd = -1;
    int write_fd = -1;  // 없으면 -1
    bool auto_reset = false;
};

[[nodiscard]] inline void* encode_event(int read_fd, int write_fd, bool auto_reset) noexcept {
    const std::uint64_t value = (static_cast<std::uint64_t>(write_fd + 1) << 32) |
                                (static_cast<std::uint64_t>(read_fd + 1) << 1) |
                                (auto_reset ? 1u : 0u);
    return reinterpret_cast<void*>(static_cast<std::uintptr_t>(value));
}

// 소켓, TUN 처럼 fd 하나로 읽기 준비를 알리는 것의 대기 핸들.
[[nodiscard]] inline void* fd_read_handle(int fd) noexcept {
    return encode_event(fd, -1, false);
}

[[nodiscard]] inline EventFds decode_event(const void* handle) noexcept {
    const auto value = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(handle));
    EventFds fds;
    fds.read_fd = static_cast<int>(((value >> 1) & 0x7FFFFFFFu)) - 1;
    fds.write_fd = static_cast<int>(value >> 32) - 1;
    fds.auto_reset = (value & 1u) != 0;
    return fds;
}

}  // namespace sangtachi::platform::posix
