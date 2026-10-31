#include "sangtachi/platform/wait.hpp"

#include "fd_handle.hpp"

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <span>

// monotonic_ms, monotonic_us 는 OS 마다 시계가 달라 여기 두지 않는다 (linux/clock.cpp).

namespace sangtachi::platform {
namespace {

// Windows MAXIMUM_WAIT_OBJECTS 와 같은 상한을 둔다. 대기 집합의 계약이 OS 마다 갈리지 않게 한다.
constexpr std::size_t kMaxWaitHandles = 64;

bool set_flags(int fd) noexcept {
    const int status = ::fcntl(fd, F_GETFL);
    if (status < 0 || ::fcntl(fd, F_SETFL, status | O_NONBLOCK) < 0) {
        return false;
    }
    const int fd_flags = ::fcntl(fd, F_GETFD);
    return fd_flags >= 0 && ::fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC) >= 0;
}

// 자동 리셋 이벤트의 신호를 가져간다. 여러 번 신호된 것도 한 번의 깨움으로 합친다 (Windows 의
// 자동 리셋과 같다). 가져간 것이 없으면 다른 대기자가 먼저 가져간 것이다.
bool consume(int read_fd) noexcept {
    bool consumed = false;
    std::array<unsigned char, 64> sink{};
    while (true) {
        const ssize_t n = ::read(read_fd, sink.data(), sink.size());
        if (n > 0) {
            consumed = true;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        // 0 은 쓰는 쪽이 모두 닫힌 것이다. 다시 기다리면 poll 이 곧바로 돌아와 돌기만 하므로 깨운 것으로 친다.
        return consumed || n == 0;
    }
}

WaitResult failed(int error) noexcept {
    return WaitResult{WaitStatus::Failed, 0, static_cast<std::uint32_t>(error)};
}

}  // namespace

WaitHandle create_event(ResetMode mode) noexcept {
    // pipe2 를 쓰지 않는다. macOS 에 없다 (ADR 0035 결정 2).
    int fds[2] = {-1, -1};
    if (::pipe(fds) != 0) {
        return nullptr;
    }
    if (!set_flags(fds[0]) || !set_flags(fds[1])) {
        ::close(fds[0]);
        ::close(fds[1]);
        return nullptr;
    }
    return posix::encode_event(fds[0], fds[1], mode == ResetMode::Auto);
}

bool signal_event(WaitHandle handle) noexcept {
    if (handle == nullptr) {
        return false;
    }
    const posix::EventFds fds = posix::decode_event(handle);
    if (fds.write_fd < 0) {
        return false;
    }
    const unsigned char one = 1;
    while (true) {
        const ssize_t n = ::write(fds.write_fd, &one, 1);
        if (n == 1) {
            return true;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        // 파이프가 찼다 = 이미 신호된 상태다.
        return n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
    }
}

void close_event(WaitHandle handle) noexcept {
    if (handle == nullptr) {
        return;
    }
    const posix::EventFds fds = posix::decode_event(handle);
    if (fds.read_fd >= 0) {
        ::close(fds.read_fd);
    }
    if (fds.write_fd >= 0) {
        ::close(fds.write_fd);
    }
}

WaitHandle duplicate_event(WaitHandle handle) noexcept {
    if (handle == nullptr) {
        return nullptr;
    }
    const posix::EventFds fds = posix::decode_event(handle);
    if (fds.read_fd < 0 || fds.write_fd < 0) {
        return nullptr;
    }
    const int read_fd = ::fcntl(fds.read_fd, F_DUPFD_CLOEXEC, 0);
    if (read_fd < 0) {
        return nullptr;
    }
    const int write_fd = ::fcntl(fds.write_fd, F_DUPFD_CLOEXEC, 0);
    if (write_fd < 0) {
        ::close(read_fd);
        return nullptr;
    }
    return posix::encode_event(read_fd, write_fd, fds.auto_reset);
}

WaitResult wait_any(std::span<const WaitHandle> handles, std::uint32_t timeout_ms) noexcept {
    if (handles.empty() || handles.size() > kMaxWaitHandles) {
        return failed(EINVAL);
    }

    std::array<pollfd, kMaxWaitHandles> fds{};
    std::array<bool, kMaxWaitHandles> auto_reset{};
    for (std::size_t i = 0; i < handles.size(); ++i) {
        // 널은 Windows 에서도 대기 전체의 실패다 (concurrency.md 2장 대기).
        if (handles[i] == nullptr) {
            return failed(EINVAL);
        }
        const posix::EventFds decoded = posix::decode_event(handles[i]);
        // poll 은 음수 fd 를 조용히 건너뛴다. 그대로 두면 무한 대기가 돌아오지 않는다.
        if (decoded.read_fd < 0) {
            return failed(EINVAL);
        }
        fds[i].fd = decoded.read_fd;
        fds[i].events = POLLIN;
        auto_reset[i] = decoded.auto_reset;
    }

    const bool infinite = timeout_ms == kInfiniteWaitMs;
    const WaitMillis start = monotonic_ms();
    while (true) {
        int slice = -1;
        if (!infinite) {
            const WaitMillis now = monotonic_ms();
            const WaitMillis elapsed = now >= start ? now - start : 0;
            const WaitMillis remaining = elapsed >= timeout_ms ? 0 : timeout_ms - elapsed;
            slice = remaining > static_cast<WaitMillis>(INT_MAX) ? INT_MAX
                                                                  : static_cast<int>(remaining);
        }

        const int ready = ::poll(fds.data(), static_cast<nfds_t>(handles.size()), slice);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            return failed(errno);
        }
        if (ready == 0) {
            if (infinite || slice < INT_MAX) {
                return WaitResult{WaitStatus::Timeout, 0, 0};
            }
            continue;  // INT_MAX 조각 하나가 끝났을 뿐이다
        }

        // 무효 핸들이 하나라도 있으면 대기 전체가 실패다. WaitForMultipleObjects 와 같다.
        for (std::size_t i = 0; i < handles.size(); ++i) {
            if ((fds[i].revents & POLLNVAL) != 0) {
                return failed(EBADF);
            }
        }
        for (std::size_t i = 0; i < handles.size(); ++i) {
            if ((fds[i].revents & (POLLIN | POLLERR | POLLHUP)) == 0) {
                continue;
            }
            // 깨어난 하나만 소비한다. 수동 리셋, 소켓, TUN 은 신호를 그대로 둔다.
            if (!auto_reset[i] || consume(fds[i].fd)) {
                return WaitResult{WaitStatus::Signaled, i, 0};
            }
            break;  // 다른 대기자가 먼저 가져갔다. 남은 시간으로 다시 기다린다
        }
    }
}

}  // namespace sangtachi::platform
