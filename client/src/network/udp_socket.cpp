#include "hamychi/network/udp_socket.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>

#include <climits>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>

namespace hamychi::network {
namespace {

constexpr std::uintptr_t kInvalidHandle = static_cast<std::uintptr_t>(INVALID_SOCKET);

SOCKET as_socket(std::uintptr_t handle) noexcept {
    return static_cast<SOCKET>(handle);
}

// sockaddr_in 은 네트워크 바이트 순서다. Endpoint 는 호스트 순서로 들고 있다.
sockaddr_in to_sockaddr(const Endpoint& endpoint) noexcept {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = ::htons(endpoint.port());
    addr.sin_addr.s_addr = ::htonl(endpoint.address());
    return addr;
}

Endpoint from_sockaddr(const sockaddr_in& addr) noexcept {
    return Endpoint(::ntohl(addr.sin_addr.s_addr), ::ntohs(addr.sin_port));
}

// 실패한 열기의 뒷정리. 이미 만든 것만 되돌린다.
void close_partial(SOCKET sock, WSAEVENT event) noexcept {
    if (event != WSA_INVALID_EVENT) {
        ::WSACloseEvent(event);
    }
    if (sock != INVALID_SOCKET) {
        ::closesocket(sock);
    }
}

OpenResult open_failure(SOCKET sock, WSAEVENT event, std::string_view op) noexcept {
    // WSAGetLastError 를 먼저 읽는다. 정리 호출이 그 값을 덮어쓴다.
    const int error = ::WSAGetLastError();
    close_partial(sock, event);
    OpenResult result;
    result.failed_op = op;
    result.error = error;
    return result;
}

}  // namespace

struct UdpSocketOpener {
    static UdpSocket make(SOCKET sock, WSAEVENT event, Endpoint local, int rcvbuf) noexcept {
        return UdpSocket(static_cast<std::uintptr_t>(sock), event, local, rcvbuf);
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
    other.event_ = WSA_INVALID_EVENT;
}

UdpSocket::~UdpSocket() {
    if (event_ != WSA_INVALID_EVENT) {
        ::WSACloseEvent(event_);
    }
    if (handle_ != kInvalidHandle) {
        ::closesocket(as_socket(handle_));
    }
}

bool UdpSocket::enumerate_events() noexcept {
    WSANETWORKEVENTS events{};
    return ::WSAEnumNetworkEvents(as_socket(handle_), event_, &events) == 0;
}

SendResult UdpSocket::send_to(const Endpoint& to, std::span<const std::byte> payload) noexcept {
    // sendto 의 길이 인자가 int 다. 좁히기 전에 걸러야 크기가 감싸 돌아 엉뚱한 길이로
    // 나가지 않는다. MAX_DATAGRAM 으로 자르지 않는 이유는 시험이 일부러 그보다 큰
    // 데이터그램을 보내 수신 쪽의 과대 경로를 확인하기 때문이다.
    if (payload.size() > static_cast<std::size_t>(INT_MAX)) {
        return SendResult{false, WSAEMSGSIZE};
    }
    const sockaddr_in addr = to_sockaddr(to);
    const int sent = ::sendto(as_socket(handle_),
                              reinterpret_cast<const char*>(payload.data()),
                              static_cast<int>(payload.size()), 0,
                              reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    if (sent == SOCKET_ERROR) {
        // protocol.md 6장: 타입과 무관하게 버리고 재전송하지 않는다. 카운터는 호출자가 올린다.
        return SendResult{false, ::WSAGetLastError()};
    }
    return SendResult{true, 0};
}

RecvResult UdpSocket::recv_from(std::span<std::byte> buffer) noexcept {
    // 읽는 길이를 kRecvBufferSize 로 자른다. 이유가 둘이다.
    //
    // 하나, recvfrom 의 길이 인자가 int 라 size_t 를 그대로 좁히면 감싸 돌 수 있다.
    // 둘, 호출자가 더 큰 버퍼를 주면 architecture.md 3.2.3 이 설계한 두 경로가 무너진다.
    // 버퍼가 1473 일 때만 1474바이트 이상이 WSAEMSGSIZE 로 올라온다. 더 큰 버퍼에서는
    // 그 데이터그램이 그냥 담겨 길이 비교 경로로만 걸리고, 그러면 그 분기를 도는 시험이
    // 아무것도 확인하지 못한다.
    const auto want = static_cast<int>(
        buffer.size() < kRecvBufferSize ? buffer.size() : kRecvBufferSize);

    sockaddr_in addr{};
    int addr_len = sizeof(addr);
    const int received = ::recvfrom(as_socket(handle_),
                                    reinterpret_cast<char*>(buffer.data()),
                                    want, 0,
                                    reinterpret_cast<sockaddr*>(&addr), &addr_len);
    if (received == SOCKET_ERROR) {
        const int error = ::WSAGetLastError();
        if (error == WSAEWOULDBLOCK) {
            return RecvResult{RecvStatus::WouldBlock, 0, Endpoint(), 0};
        }
        if (error == WSAEMSGSIZE) {
            // 1473바이트 버퍼로도 모자랐다. 데이터그램은 이미 소비됐다
            // (architecture.md 3.2.3). 버리고 계속 비운다.
            return RecvResult{RecvStatus::Oversize, 0, Endpoint(), error};
        }
        return RecvResult{RecvStatus::Error, 0, Endpoint(), error};
    }

    const auto length = static_cast<std::size_t>(received);
    if (length > protocol::kMaxDatagram) {
        // 정확히 1473바이트가 왔다. 길이 비교가 판정이다.
        return RecvResult{RecvStatus::Oversize, 0, Endpoint(), 0};
    }
    return RecvResult{RecvStatus::Received, length, from_sockaddr(addr), 0};
}

OpenResult open_udp_socket() {
    SOCKET sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        return open_failure(INVALID_SOCKET, WSA_INVALID_EVENT, "socket");
    }

    // SO_REUSEADDR 를 쓰지 않는다. Windows 에서는 다른 소켓이 같은 엔드포인트를 bind 해
    // 배달이 비결정적이 된다 (protocol.md 6장).
    BOOL exclusive = TRUE;
    if (::setsockopt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                     reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) != 0) {
        return open_failure(sock, WSA_INVALID_EVENT, "setsockopt.SO_EXCLUSIVEADDRUSE");
    }

    int rcvbuf = kRecvBufferRequest;
    if (::setsockopt(sock, SOL_SOCKET, SO_RCVBUF,
                     reinterpret_cast<const char*>(&rcvbuf), sizeof(rcvbuf)) != 0) {
        return open_failure(sock, WSA_INVALID_EVENT, "setsockopt.SO_RCVBUF");
    }

    // 요청값을 그대로 주지 않을 수 있다. 적용값을 읽어 기록한다 (protocol.md 6장).
    int applied = 0;
    int applied_len = sizeof(applied);
    if (::getsockopt(sock, SOL_SOCKET, SO_RCVBUF,
                     reinterpret_cast<char*>(&applied), &applied_len) != 0) {
        return open_failure(sock, WSA_INVALID_EVENT, "getsockopt.SO_RCVBUF");
    }

    // 응답 없는 후보가 보낸 ICMP port unreachable 이 WSAECONNRESET 으로 올라오는 것을
    // 막는다. 끄지 않으면 홀펀칭에서 수신 루프가 그 오류로 끝난다 (protocol.md 6장).
    BOOL conn_reset = FALSE;
    DWORD returned = 0;
    if (::WSAIoctl(sock, SIO_UDP_CONNRESET, &conn_reset, sizeof(conn_reset),
                   nullptr, 0, &returned, nullptr, nullptr) == SOCKET_ERROR) {
        return open_failure(sock, WSA_INVALID_EVENT, "WSAIoctl.SIO_UDP_CONNRESET");
    }

    // INADDR_ANY, 포트 0. 포트를 고정하면 같은 기기에서 두 프로세스를 띄울 수 없다.
    sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = ::htonl(INADDR_ANY);
    bind_addr.sin_port = 0;
    if (::bind(sock, reinterpret_cast<const sockaddr*>(&bind_addr), sizeof(bind_addr)) != 0) {
        return open_failure(sock, WSA_INVALID_EVENT, "bind");
    }

    sockaddr_in actual{};
    int actual_len = sizeof(actual);
    if (::getsockname(sock, reinterpret_cast<sockaddr*>(&actual), &actual_len) != 0) {
        return open_failure(sock, WSA_INVALID_EVENT, "getsockname");
    }

    WSAEVENT event = ::WSACreateEvent();
    if (event == WSA_INVALID_EVENT) {
        return open_failure(sock, WSA_INVALID_EVENT, "WSACreateEvent");
    }

    // 이 호출이 소켓을 논블로킹으로 바꾼다. ioctlsocket(FIONBIO) 을 따로 부르지 않는다.
    if (::WSAEventSelect(sock, event, FD_READ) != 0) {
        return open_failure(sock, event, "WSAEventSelect");
    }

    OpenResult result;
    result.socket.emplace(
        UdpSocketOpener::make(sock, event, from_sockaddr(actual), applied));
    return result;
}

}  // namespace hamychi::network
