#pragma once

// UDP 소켓 (protocol.md 6장 소켓 소유권).
//
// 6장이 정한 것을 그대로 옮긴다.
//   - 소켓 하나를 수신 루프 하나가 배타적으로 소유한다
//   - INADDR_ANY, 포트 0 으로 한 번 bind 하고 종료까지 유지한다
//   - bind 직후 getsockname 으로 실제 포트를 읽는다
//   - SO_RCVBUF 를 262144 로 요청하고 getsockopt 로 적용값을 읽어 기록한다
//   - SO_SNDBUF 는 기본값을 쓴다
//   - SO_REUSEADDR 대신 SO_EXCLUSIVEADDRUSE 를 쓴다
//   - connect() 를 부르지 않는다
//   - SIO_UDP_CONNRESET 을 끈다
//
// WSAEventSelect 로 FD_READ 를 이벤트에 묶는다 (architecture.md 3.2.2 대기). 그 호출이
// 소켓을 논블로킹으로 바꾸므로 ioctlsocket(FIONBIO) 을 따로 부르지 않는다.

#include "sangtachi/network/endpoint.hpp"
#include "sangtachi/protocol_constants.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace sangtachi::network {

// protocol.md 6장의 SO_RCVBUF 요청값. 10진 바이트 수로 적는다.
inline constexpr int kRecvBufferRequest = 262144;

// 수신 버퍼는 MAX_DATAGRAM 보다 1 크다 (architecture.md 3.2.3 루프 한 바퀴).
// 1 크게 잡아야 길이 비교가 판정이 되고, 시험이 1473바이트로 그 경로를 재현할 수 있다.
inline constexpr std::size_t kRecvBufferSize = protocol::kMaxDatagram + 1;

enum class RecvStatus {
    Received,    // 데이터그램 하나를 받았다
    WouldBlock,  // 소스가 비었다 (WSAEWOULDBLOCK)
    Oversize,    // MAX_DATAGRAM 을 넘었다. 데이터그램은 이미 소비됐다
    Error,       // 그 밖의 오류
};

struct RecvResult {
    RecvStatus status = RecvStatus::Error;
    std::size_t length = 0;  // Received 일 때만 뜻이 있다
    Endpoint from;           // Received 일 때만 뜻이 있다
    int error = 0;           // WSAGetLastError. Received / WouldBlock 이면 0
};

struct SendResult {
    bool ok = false;
    int error = 0;  // ok 가 거짓일 때 WSAGetLastError
};

class UdpSocket {
public:
    UdpSocket() = delete;
    ~UdpSocket();

    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;
    UdpSocket(UdpSocket&& other) noexcept;
    UdpSocket& operator=(UdpSocket&&) = delete;

    // getsockname 이 돌려준 실제 엔드포인트. 포트는 0 이 아니다.
    [[nodiscard]] Endpoint local() const noexcept { return local_; }

    // architecture.md 9장의 socket.bind 가 싣는 두 값.
    [[nodiscard]] int rcvbuf_requested() const noexcept { return kRecvBufferRequest; }
    [[nodiscard]] int rcvbuf_applied() const noexcept { return rcvbuf_applied_; }

    // architecture.md 3.2.2 의 대기 집합에 넣는 핸들. 소유는 이 객체가 한다.
    [[nodiscard]] void* read_event() const noexcept { return event_; }

    // 원시 소켓 핸들. 소유권을 넘기지 않는다.
    //
    // 시험이 소켓 옵션을 독립적으로 다시 읽으려고 쓴다. 이것이 없으면 `rcvbuf_applied`
    // 가 정말 getsockopt 에서 온 값인지 요청값을 그대로 적은 것인지 구분할 수 없고,
    // 변이 시험에서 실제로 구분되지 않았다. protocol.md 6장이 적용값을 기록하라고 한
    // 이유가 "OS 가 요청값을 그대로 주지 않을 수 있다" 이므로 그 자리는 검증되어야 한다.
    [[nodiscard]] std::uintptr_t native_handle() const noexcept { return handle_; }

    // 이벤트를 리셋하고 네트워크 이벤트를 조회한다 (architecture.md 3.2.3).
    // 이 호출을 빠뜨리면 이벤트가 신호 상태로 남아 루프가 스핀한다.
    bool enumerate_events() noexcept;

    // 데이터그램 하나를 보낸다. 실패해도 재전송하지 않는다 (protocol.md 6장).
    [[nodiscard]] SendResult send_to(const Endpoint& to, std::span<const std::byte> payload) noexcept;

    // 데이터그램 하나를 받는다. buffer 는 kRecvBufferSize 이상이어야 한다.
    [[nodiscard]] RecvResult recv_from(std::span<std::byte> buffer) noexcept;

private:
    friend struct UdpSocketOpener;
    UdpSocket(std::uintptr_t handle, void* event, Endpoint local, int rcvbuf_applied) noexcept;

    std::uintptr_t handle_;
    void* event_;
    Endpoint local_;
    int rcvbuf_applied_;
};

struct OpenResult {
    std::optional<UdpSocket> socket;
    std::string_view failed_op;  // 실패한 호출 이름. architecture.md 9장 socket.error 의 op
    int error = 0;               // WSAGetLastError

    [[nodiscard]] bool ok() const noexcept { return socket.has_value(); }
};

// 6장 계약대로 소켓 하나를 연다. Winsock 이 초기화된 뒤에 부른다.
[[nodiscard]] OpenResult open_udp_socket();

}  // namespace sangtachi::network
