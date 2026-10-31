#include "sangtachi/network/udp_socket.hpp"

#include "fd_handle.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>

namespace sangtachi::network {
namespace {

constexpr std::uintptr_t kInvalidHandle = UINTPTR_MAX;

int as_fd(std::uintptr_t handle) noexcept {
    return static_cast<int>(handle);
}

// sockaddr_in 은 네트워크 바이트 순서다. Endpoint 는 호스트 순서로 들고 있다.
sockaddr_in to_sockaddr(const Endpoint& endpoint) noexcept {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(endpoint.port());
    addr.sin_addr.s_addr = htonl(endpoint.address());
    return addr;
}

Endpoint from_sockaddr(const sockaddr_in& addr) noexcept {
    return Endpoint(ntohl(addr.sin_addr.s_addr), ntohs(addr.sin_port));
}

OpenResult open_failure(int fd, std::string_view op) noexcept {
    // errno 를 먼저 읽는다. close 가 그 값을 덮어쓴다.
    const int error = errno;
    if (fd >= 0) {
        ::close(fd);
    }
    OpenResult result;
    result.failed_op = op;
    result.error = error;
    return result;
}

}  // namespace

struct UdpSocketOpener {
    static UdpSocket make(int fd, Endpoint local, int rcvbuf) noexcept {
        return UdpSocket(static_cast<std::uintptr_t>(fd), platform::posix::fd_read_handle(fd), local,
                         rcvbuf);
    }
};

UdpSocket::UdpSocket(std::uintptr_t handle, void* event, Endpoint local,
                     int rcvbuf_applied) noexcept
    : handle_(handle), event_(event), local_(local), rcvbuf_applied_(rcvbuf_applied) {}

UdpSocket::UdpSocket(UdpSocket&& other) noexcept
    : handle_(other.handle_),
      event_(other.event_),
      local_(other.local_),
      rcvbuf_applied_(other.rcvbuf_applied_) {
    other.handle_ = kInvalidHandle;
    other.event_ = nullptr;
}

UdpSocket::~UdpSocket() {
    // event_ 는 같은 fd 를 담은 값일 뿐이다. 닫을 것은 소켓 하나다.
    if (handle_ != kInvalidHandle) {
        ::close(as_fd(handle_));
    }
}

bool UdpSocket::enumerate_events() noexcept {
    // 읽기 핸들이 소켓 자체라 수준형이다. 다 비우면 poll 이 더는 깨우지 않는다. 리셋할 것이 없다.
    return true;
}

SendResult UdpSocket::send_to(const Endpoint& to, std::span<const std::byte> payload) noexcept {
    // win32 판과 같은 가드를 둔다. 크기 경계의 동작이 OS 마다 갈리지 않게 한다.
    if (payload.size() > static_cast<std::size_t>(INT_MAX)) {
        return SendResult{false, EMSGSIZE};
    }
    const sockaddr_in addr = to_sockaddr(to);
    const ssize_t sent = ::sendto(as_fd(handle_), payload.data(), payload.size(), 0,
                                  reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    if (sent < 0) {
        // protocol.md 6장: 타입과 무관하게 버리고 재전송하지 않는다. 카운터는 호출자가 올린다.
        return SendResult{false, errno};
    }
    return SendResult{true, 0, static_cast<std::size_t>(sent)};
}

RecvResult UdpSocket::recv_from(std::span<std::byte> buffer) noexcept {
    // 읽는 길이를 kRecvBufferSize 로 자르는 이유는 win32 판과 같다 (concurrency.md 3장의 두 경로).
    const std::size_t want = buffer.size() < kRecvBufferSize ? buffer.size() : kRecvBufferSize;

    sockaddr_in addr{};
    socklen_t addr_len = sizeof(addr);
    // MSG_TRUNC 면 잘린 데이터그램의 실제 길이가 돌아온다. 그것으로 Windows 의 WSAEMSGSIZE 경로를 가른다.
    const ssize_t received = ::recvfrom(as_fd(handle_), buffer.data(), want, MSG_TRUNC,
                                        reinterpret_cast<sockaddr*>(&addr), &addr_len);
    if (received < 0) {
        const int error = errno;
        if (error == EAGAIN || error == EWOULDBLOCK) {
            return RecvResult{RecvStatus::WouldBlock, 0, Endpoint(), 0};
        }
        return RecvResult{RecvStatus::Error, 0, Endpoint(), error};
    }

    const auto length = static_cast<std::size_t>(received);
    if (length > want) {
        // 1473바이트 버퍼로도 모자랐다. 데이터그램은 이미 소비됐다 (concurrency.md 3장). rx_bytes 에
        // 세지 않도록 길이는 0 이다.
        return RecvResult{RecvStatus::Oversize, 0, Endpoint(), EMSGSIZE};
    }
    if (length > protocol::kMaxDatagram) {
        // 정확히 1473바이트가 왔다. 길이 비교가 판정이다.
        return RecvResult{RecvStatus::Oversize, length, Endpoint(), 0};
    }
    return RecvResult{RecvStatus::Received, length, from_sockaddr(addr), 0};
}

OpenResult open_udp_socket() {
    const int fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        return open_failure(-1, "socket");
    }

    // 대기 집합이 읽기 준비를 알리고 루프가 WouldBlock 까지 비운다. 그래서 논블로킹이다.
    const int status = ::fcntl(fd, F_GETFL);
    if (status < 0 || ::fcntl(fd, F_SETFL, status | O_NONBLOCK) < 0) {
        return open_failure(fd, "fcntl");
    }
    const int fd_flags = ::fcntl(fd, F_GETFD);
    if (fd_flags < 0 || ::fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC) < 0) {
        return open_failure(fd, "fcntl");
    }

    // SO_REUSEADDR, SO_REUSEPORT 를 켜지 않는다. 기본이 배타라 SO_EXCLUSIVEADDRUSE 의 몫을 한다
    // (protocol.md 6장).

    int rcvbuf = kRecvBufferRequest;
    if (::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) != 0) {
        return open_failure(fd, "setsockopt.SO_RCVBUF");
    }

    // Linux 는 요청값의 두 배를 보고한다. 고치지 않고 읽은 값을 그대로 기록한다 (protocol.md 6장).
    int applied = 0;
    socklen_t applied_len = sizeof(applied);
    if (::getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &applied, &applied_len) != 0) {
        return open_failure(fd, "getsockopt.SO_RCVBUF");
    }

    // SIO_UDP_CONNRESET 에 해당하는 것이 없다. 연결하지 않은 UDP 소켓은 IP_RECVERR 를 켜지 않으면
    // ICMP 오류를 올리지 않는다.

    sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_addr.sin_port = 0;
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&bind_addr), sizeof(bind_addr)) != 0) {
        return open_failure(fd, "bind");
    }

    sockaddr_in actual{};
    socklen_t actual_len = sizeof(actual);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&actual), &actual_len) != 0) {
        return open_failure(fd, "getsockname");
    }

    OpenResult result;
    result.socket.emplace(UdpSocketOpener::make(fd, from_sockaddr(actual), applied));
    return result;
}

}  // namespace sangtachi::network
