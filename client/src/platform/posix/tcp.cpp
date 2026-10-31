#include "sangtachi/platform/tcp.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace sangtachi::platform {
namespace {

constexpr std::uintptr_t kInvalidHandle = UINTPTR_MAX;

int as_fd(std::uintptr_t handle) noexcept {
    return static_cast<int>(handle);
}

// 소켓 하나를 스코프 끝에서 닫는다. 넘겨줄 때만 release 한다 (win32 판과 같은 이유).
class SocketGuard {
public:
    explicit SocketGuard(int fd) noexcept : fd_(fd) {}
    ~SocketGuard() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }
    SocketGuard(const SocketGuard&) = delete;
    SocketGuard& operator=(const SocketGuard&) = delete;
    SocketGuard(SocketGuard&&) = delete;
    SocketGuard& operator=(SocketGuard&&) = delete;

    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] int release() noexcept {
        const int out = fd_;
        fd_ = -1;
        return out;
    }

private:
    int fd_;
};

TcpResult ok_result(std::size_t bytes = 0) noexcept {
    TcpResult r;
    r.status = TcpStatus::kOk;
    r.bytes = bytes;
    return r;
}

TcpResult status_result(TcpStatus status) noexcept {
    TcpResult r;
    r.status = status;
    return r;
}

TcpResult error_result(TcpStatus status, std::uint32_t error, std::string_view op) noexcept {
    TcpResult r;
    r.status = status;
    r.error = error;
    r.failed_op = op;
    return r;
}

TcpResult last_error(std::string_view op) noexcept {
    return error_result(TcpStatus::kError, static_cast<std::uint32_t>(errno), op);
}

// 중단 이벤트가 신호됐는가. 대기 자체가 실패하면 중단으로 본다 (win32 판과 같은 이유).
bool aborted(WaitHandle abort) noexcept {
    if (abort == nullptr) {
        return false;
    }
    const WaitHandle handles[] = {abort};
    const WaitResult r = wait_any(std::span<const WaitHandle>(handles, 1), 0);
    return r.status != WaitStatus::Timeout;
}

enum class Readiness : std::uint8_t {
    kReady,  // 지켜보는 쪽이 됐거나 오류, 닫힘이 났다
    kTimedOut,
    kAborted,
    kError,
};

// poll 을 kAbortPollMs 조각으로 나눠 부른다. 조각마다 중단 이벤트를 본다.
// select 를 쓰지 않는 이유는 FD_SETSIZE 보다 큰 fd 를 담지 못하기 때문이다.
Readiness wait_socket(int fd, bool for_write, std::uint32_t timeout_ms, WaitHandle abort) noexcept {
    const WaitMillis start = monotonic_ms();
    while (true) {
        if (aborted(abort)) {
            return Readiness::kAborted;
        }
        const WaitMillis now = monotonic_ms();
        const WaitMillis elapsed = now >= start ? now - start : 0;
        if (elapsed >= timeout_ms) {
            return Readiness::kTimedOut;
        }
        const WaitMillis remaining = timeout_ms - elapsed;
        const int slice = static_cast<int>(std::min<WaitMillis>(remaining, kAbortPollMs));

        pollfd pfd{};
        pfd.fd = fd;
        pfd.events = for_write ? POLLOUT : POLLIN;
        const int r = ::poll(&pfd, 1, slice);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            return Readiness::kError;
        }
        if (r == 0) {
            continue;
        }
        if ((pfd.revents & POLLNVAL) != 0) {
            errno = EBADF;
            return Readiness::kError;
        }
        return Readiness::kReady;
    }
}

// 연결의 결과. Linux 는 실패를 exceptfds 가 아니라 쓰기 가능과 SO_ERROR 로 알린다.
std::uint32_t socket_error(int fd) noexcept {
    int value = 0;
    socklen_t length = sizeof(value);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &value, &length) != 0) {
        return static_cast<std::uint32_t>(errno);
    }
    return static_cast<std::uint32_t>(value);
}

bool set_nonblocking(int fd, bool on) noexcept {
    const int flags = ::fcntl(fd, F_GETFL);
    if (flags < 0) {
        return false;
    }
    const int next = on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return ::fcntl(fd, F_SETFL, next) == 0;
}

bool is_timeout(int error) noexcept {
    return error == EAGAIN || error == EWOULDBLOCK;
}

}  // namespace

struct TcpStreamOpener {
    static TcpStream make(int fd) noexcept {
        return TcpStream(static_cast<std::uintptr_t>(fd));
    }
};

TcpStream::TcpStream(TcpStream&& other) noexcept : handle_(other.handle_) {
    other.handle_ = kInvalidHandle;
}

TcpStream::~TcpStream() {
    if (handle_ != kInvalidHandle) {
        ::close(as_fd(handle_));
    }
}

TcpResult TcpStream::send_all(std::span<const std::byte> data) noexcept {
    std::size_t sent = 0;
    while (sent < data.size()) {
        // MSG_NOSIGNAL: 상대가 닫은 소켓에 써도 SIGPIPE 로 프로세스가 끝나지 않고 EPIPE 가 온다.
        const ssize_t n =
            ::send(as_fd(handle_), data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n < 0) {
            const int error = errno;
            if (error == EINTR) {
                continue;
            }
            if (is_timeout(error)) {
                return status_result(TcpStatus::kTimedOut);  // SO_SNDTIMEO (8.2)
            }
            return error_result(TcpStatus::kError, static_cast<std::uint32_t>(error), "send");
        }
        sent += static_cast<std::size_t>(n);
    }
    return ok_result(sent);
}

TcpResult TcpStream::wait_readable(std::uint32_t timeout_ms, WaitHandle abort) noexcept {
    switch (wait_socket(as_fd(handle_), false, timeout_ms, abort)) {
        case Readiness::kReady:
            // 읽을 것이 있거나 오류가 있다. 어느 쪽인지는 recv 가 말한다.
            return ok_result();
        case Readiness::kTimedOut:
            return status_result(TcpStatus::kTimedOut);
        case Readiness::kAborted:
            return status_result(TcpStatus::kAborted);
        case Readiness::kError:
            break;
    }
    return last_error("poll");
}

TcpResult TcpStream::recv_some(std::span<std::byte> buffer) noexcept {
    if (buffer.empty()) {
        return error_result(TcpStatus::kError, static_cast<std::uint32_t>(EINVAL), "recv");
    }
    while (true) {
        const ssize_t n = ::recv(as_fd(handle_), buffer.data(), buffer.size(), 0);
        if (n == 0) {
            return status_result(TcpStatus::kClosed);
        }
        if (n > 0) {
            return ok_result(static_cast<std::size_t>(n));
        }
        const int error = errno;
        if (error == EINTR) {
            continue;
        }
        if (is_timeout(error)) {
            return status_result(TcpStatus::kTimedOut);  // SO_RCVTIMEO (8.2)
        }
        return error_result(TcpStatus::kError, static_cast<std::uint32_t>(error), "recv");
    }
}

TcpConnect connect_tcp(const network::Endpoint& to, std::uint32_t connect_timeout_ms,
                       std::uint32_t io_timeout_ms, WaitHandle abort) noexcept {
    TcpConnect out;

    SocketGuard sock(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (sock.get() < 0) {
        out.result = last_error("socket");
        return out;
    }

    // 논블로킹으로 connect 를 걸고 poll 로 기다린다 (control_plane.md 8.2).
    if (!set_nonblocking(sock.get(), true)) {
        out.result = last_error("fcntl");
        return out;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(to.port());
    addr.sin_addr.s_addr = htonl(to.address());
    if (::connect(sock.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        const int error = errno;
        if (error != EINPROGRESS) {
            out.result = error_result(TcpStatus::kRefused, static_cast<std::uint32_t>(error),
                                      "connect");
            return out;
        }
        switch (wait_socket(sock.get(), true, connect_timeout_ms, abort)) {
            case Readiness::kReady:
                break;
            case Readiness::kTimedOut:
                out.result = status_result(TcpStatus::kTimedOut);
                return out;
            case Readiness::kAborted:
                out.result = status_result(TcpStatus::kAborted);
                return out;
            case Readiness::kError:
                out.result = last_error("poll");
                return out;
        }
        if (const std::uint32_t error_code = socket_error(sock.get()); error_code != 0) {
            out.result = error_result(TcpStatus::kRefused, error_code, "connect");
            return out;
        }
    }

    // 블로킹으로 되돌린다. 송수신 시간 제한은 소켓 옵션이 건다 (control_plane.md 8.2).
    if (!set_nonblocking(sock.get(), false)) {
        out.result = last_error("fcntl");
        return out;
    }
    timeval io_timeout{};
    io_timeout.tv_sec = static_cast<time_t>(io_timeout_ms / 1000);
    io_timeout.tv_usec = static_cast<suseconds_t>((io_timeout_ms % 1000) * 1000);
    if (::setsockopt(sock.get(), SOL_SOCKET, SO_SNDTIMEO, &io_timeout, sizeof(io_timeout)) != 0) {
        out.result = last_error("setsockopt.SO_SNDTIMEO");
        return out;
    }
    if (::setsockopt(sock.get(), SOL_SOCKET, SO_RCVTIMEO, &io_timeout, sizeof(io_timeout)) != 0) {
        out.result = last_error("setsockopt.SO_RCVTIMEO");
        return out;
    }

    out.stream.emplace(TcpStreamOpener::make(sock.release()));
    out.result = ok_result();
    return out;
}

}  // namespace sangtachi::platform
