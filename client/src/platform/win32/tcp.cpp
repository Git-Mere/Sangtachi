#include "sangtachi/platform/tcp.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace sangtachi::platform {
namespace {

constexpr std::uintptr_t kInvalidHandle = static_cast<std::uintptr_t>(INVALID_SOCKET);

SOCKET as_socket(std::uintptr_t handle) noexcept {
    return static_cast<SOCKET>(handle);
}

// 소켓 하나를 스코프 끝에서 닫는다. 넘겨줄 때만 release 한다.
//
// connect_tcp 의 실패 경로가 여럿이라 닫기를 손으로 맞추면 하나를 흘린다. 흘린 소켓은
// 요청마다 하나씩 쌓인다 (요청마다 새 소켓이다. control_plane.md 8.2).
class SocketGuard {
public:
    explicit SocketGuard(SOCKET sock) noexcept : sock_(sock) {}
    ~SocketGuard() {
        if (sock_ != INVALID_SOCKET) {
            ::closesocket(sock_);
        }
    }
    SocketGuard(const SocketGuard&) = delete;
    SocketGuard& operator=(const SocketGuard&) = delete;
    SocketGuard(SocketGuard&&) = delete;
    SocketGuard& operator=(SocketGuard&&) = delete;

    [[nodiscard]] SOCKET get() const noexcept { return sock_; }
    [[nodiscard]] SOCKET release() noexcept {
        const SOCKET out = sock_;
        sock_ = INVALID_SOCKET;
        return out;
    }

private:
    SOCKET sock_;
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
    return error_result(TcpStatus::kError, static_cast<std::uint32_t>(::WSAGetLastError()), op);
}

// 중단 이벤트가 신호됐는가. 널이면 볼 것이 없다.
//
// 대기 자체가 실패하면 **중단으로 본다.** 그 핸들로는 종료를 알 수 없게 된 것이고, 모르는
// 채로 요청을 이어 가면 종료가 그 요청의 상한만큼 늦는다.
bool aborted(WaitHandle abort) noexcept {
    if (abort == nullptr) {
        return false;
    }
    const WaitHandle handles[] = {abort};
    const WaitResult r = wait_any(std::span<const WaitHandle>(handles, 1), 0);
    return r.status != WaitStatus::Timeout;
}

enum class Readiness : std::uint8_t {
    kReady,      // 지켜보는 쪽이 됐다
    kException,  // exceptfds 에 올랐다. connect 에서는 실패다
    kTimedOut,
    kAborted,
    kError,
};

// select 를 kAbortPollMs 조각으로 나눠 부른다. 조각마다 중단 이벤트를 본다.
// for_write 가 참이면 쓰기 가능(connect 완료)을, 거짓이면 읽기 가능을 기다린다.
Readiness wait_socket(SOCKET sock, bool for_write, std::uint32_t timeout_ms,
                      WaitHandle abort) noexcept {
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
        const auto slice = static_cast<long>(std::min<WaitMillis>(remaining, kAbortPollMs));

        fd_set watch;
        fd_set except;
        FD_ZERO(&watch);
        FD_ZERO(&except);
        FD_SET(sock, &watch);
        FD_SET(sock, &except);
        timeval tv{};
        tv.tv_sec = slice / 1000;
        tv.tv_usec = (slice % 1000) * 1000;

        const int r = for_write ? ::select(0, nullptr, &watch, &except, &tv)
                                : ::select(0, &watch, nullptr, &except, &tv);
        if (r == SOCKET_ERROR) {
            return Readiness::kError;
        }
        if (r == 0) {
            continue;
        }
        if (FD_ISSET(sock, &except)) {
            return Readiness::kException;
        }
        if (FD_ISSET(sock, &watch)) {
            return Readiness::kReady;
        }
    }
}

// 연결 실패의 이유. connect 가 exceptfds 로 끝나면 SO_ERROR 가 그 코드다.
std::uint32_t socket_error(SOCKET sock) noexcept {
    int value = 0;
    int length = sizeof(value);
    if (::getsockopt(sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&value), &length) != 0) {
        return static_cast<std::uint32_t>(::WSAGetLastError());
    }
    return static_cast<std::uint32_t>(value);
}

}  // namespace

struct TcpStreamOpener {
    static TcpStream make(SOCKET sock) noexcept {
        return TcpStream(static_cast<std::uintptr_t>(sock));
    }
};

TcpStream::TcpStream(TcpStream&& other) noexcept : handle_(other.handle_) {
    other.handle_ = kInvalidHandle;
}

TcpStream::~TcpStream() {
    if (handle_ != kInvalidHandle) {
        ::closesocket(as_socket(handle_));
    }
}

TcpResult TcpStream::send_all(std::span<const std::byte> data) noexcept {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const std::size_t left = data.size() - sent;
        const int chunk = static_cast<int>(std::min<std::size_t>(left, INT_MAX));
        const int n = ::send(as_socket(handle_), reinterpret_cast<const char*>(data.data() + sent),
                             chunk, 0);
        if (n == SOCKET_ERROR) {
            const int error = ::WSAGetLastError();
            if (error == WSAETIMEDOUT) {
                return status_result(TcpStatus::kTimedOut);  // SO_SNDTIMEO (8.2)
            }
            return error_result(TcpStatus::kError, static_cast<std::uint32_t>(error), "send");
        }
        sent += static_cast<std::size_t>(n);
    }
    return ok_result(sent);
}

TcpResult TcpStream::wait_readable(std::uint32_t timeout_ms, WaitHandle abort) noexcept {
    switch (wait_socket(as_socket(handle_), false, timeout_ms, abort)) {
        case Readiness::kReady:
        case Readiness::kException:
            // 읽을 것이 있거나 오류가 있다. 어느 쪽인지는 recv 가 말한다.
            return ok_result();
        case Readiness::kTimedOut:
            return status_result(TcpStatus::kTimedOut);
        case Readiness::kAborted:
            return status_result(TcpStatus::kAborted);
        case Readiness::kError:
            break;
    }
    return last_error("select");
}

TcpResult TcpStream::recv_some(std::span<std::byte> buffer) noexcept {
    if (buffer.empty()) {
        return error_result(TcpStatus::kError, static_cast<std::uint32_t>(WSAEINVAL), "recv");
    }
    const int want = static_cast<int>(std::min<std::size_t>(buffer.size(), INT_MAX));
    const int n = ::recv(as_socket(handle_), reinterpret_cast<char*>(buffer.data()), want, 0);
    if (n == 0) {
        return status_result(TcpStatus::kClosed);
    }
    if (n == SOCKET_ERROR) {
        const int error = ::WSAGetLastError();
        if (error == WSAETIMEDOUT) {
            return status_result(TcpStatus::kTimedOut);  // SO_RCVTIMEO (8.2)
        }
        return error_result(TcpStatus::kError, static_cast<std::uint32_t>(error), "recv");
    }
    return ok_result(static_cast<std::size_t>(n));
}

TcpConnect connect_tcp(const network::Endpoint& to, std::uint32_t connect_timeout_ms,
                       std::uint32_t io_timeout_ms, WaitHandle abort) noexcept {
    TcpConnect out;

    SocketGuard sock(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (sock.get() == INVALID_SOCKET) {
        out.result = last_error("socket");
        return out;
    }

    // 논블로킹으로 connect 를 걸고 select 로 기다린다 (control_plane.md 8.2).
    u_long nonblocking = 1;
    if (::ioctlsocket(sock.get(), FIONBIO, &nonblocking) != 0) {
        out.result = last_error("ioctlsocket.FIONBIO");
        return out;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = ::htons(to.port());
    addr.sin_addr.s_addr = ::htonl(to.address());
    if (::connect(sock.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        const int error = ::WSAGetLastError();
        if (error != WSAEWOULDBLOCK) {
            out.result = error_result(TcpStatus::kRefused, static_cast<std::uint32_t>(error),
                                      "connect");
            return out;
        }
        switch (wait_socket(sock.get(), true, connect_timeout_ms, abort)) {
            case Readiness::kReady:
                break;
            case Readiness::kException:
                out.result =
                    error_result(TcpStatus::kRefused, socket_error(sock.get()), "connect");
                return out;
            case Readiness::kTimedOut:
                out.result = status_result(TcpStatus::kTimedOut);
                return out;
            case Readiness::kAborted:
                out.result = status_result(TcpStatus::kAborted);
                return out;
            case Readiness::kError:
                out.result = last_error("select");
                return out;
        }
    }

    // 블로킹으로 되돌린다. 송수신 시간 제한은 소켓 옵션이 건다 (control_plane.md 8.2).
    u_long blocking = 0;
    if (::ioctlsocket(sock.get(), FIONBIO, &blocking) != 0) {
        out.result = last_error("ioctlsocket.FIONBIO");
        return out;
    }
    const DWORD io_timeout = static_cast<DWORD>(io_timeout_ms);
    if (::setsockopt(sock.get(), SOL_SOCKET, SO_SNDTIMEO,
                     reinterpret_cast<const char*>(&io_timeout), sizeof(io_timeout)) != 0) {
        out.result = last_error("setsockopt.SO_SNDTIMEO");
        return out;
    }
    if (::setsockopt(sock.get(), SOL_SOCKET, SO_RCVTIMEO,
                     reinterpret_cast<const char*>(&io_timeout), sizeof(io_timeout)) != 0) {
        out.result = last_error("setsockopt.SO_RCVTIMEO");
        return out;
    }

    out.stream.emplace(TcpStreamOpener::make(sock.release()));
    out.result = ok_result();
    return out;
}

}  // namespace sangtachi::platform
