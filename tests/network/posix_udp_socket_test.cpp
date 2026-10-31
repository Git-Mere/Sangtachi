#include "sangtachi/network/udp_socket.hpp"

#include "sangtachi/platform/wait.hpp"

#include <catch2/catch_test_macros.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <vector>

// POSIX UDP 소켓의 크기 경계와 수준형 읽기 핸들 (ADR 0035, protocol.md 6장, concurrency.md 3장).
// 시험 케이스 이름은 ASCII 로만 적는다.

using sangtachi::network::Endpoint;
using sangtachi::network::kRecvBufferSize;
using sangtachi::network::open_udp_socket;
using sangtachi::network::RecvResult;
using sangtachi::network::RecvStatus;
using sangtachi::network::UdpSocket;
using sangtachi::platform::wait_any;
using sangtachi::platform::WaitHandle;
using sangtachi::platform::WaitStatus;

namespace {

// 127.0.0.1 이 아니라 127.0.0.2 다. WSL 미러 네트워크는 UDP 127.0.0.1 을 MTU 1500 인 loopback0 으로 Windows
// 쪽에 넘기고, 1473바이트부터 조각난 데이터그램이 돌아오지 않는다. 127/8 의 나머지는 lo 로 간다.
constexpr std::uint32_t kLoopback = 0x7F000002u;

Endpoint loopback(const UdpSocket& socket) {
    return Endpoint(kLoopback, socket.local().port());
}

// 보낸 것이 도착할 때까지 읽기 핸들로 기다린다.
bool wait_readable(const UdpSocket& socket) {
    const std::array<WaitHandle, 1> handles{socket.read_event()};
    return wait_any(handles, 2000).status == WaitStatus::Signaled;
}

RecvResult send_and_receive(UdpSocket& socket, std::size_t size) {
    const std::vector<std::byte> payload(size, std::byte{0x5A});
    const auto sent = socket.send_to(loopback(socket), payload);
    REQUIRE(sent.ok);
    REQUIRE(sent.bytes == size);
    REQUIRE(wait_readable(socket));
    std::array<std::byte, kRecvBufferSize> buffer{};
    return socket.recv_from(buffer);
}

}  // namespace

TEST_CASE("posix udp: 1472 bytes is received", "[network][udp][posix]") {
    auto opened = open_udp_socket();
    REQUIRE(opened.ok());
    const RecvResult r = send_and_receive(*opened.socket, 1472);
    REQUIRE(r.status == RecvStatus::Received);
    REQUIRE(r.length == 1472);
    REQUIRE(r.from.port() == opened.socket->local().port());
}

TEST_CASE("posix udp: exactly 1473 bytes is oversize with its length", "[network][udp][posix]") {
    auto opened = open_udp_socket();
    REQUIRE(opened.ok());
    const RecvResult r = send_and_receive(*opened.socket, 1473);
    REQUIRE(r.status == RecvStatus::Oversize);
    REQUIRE(r.length == 1473);
    REQUIRE(r.error == 0);
}

TEST_CASE("posix udp: 1474 bytes is oversize with EMSGSIZE and no length", "[network][udp][posix]") {
    // Windows 의 WSAEMSGSIZE 경로와 같다. length 0 이라 rx_bytes 에 세지 않는다 (loop.cpp).
    auto opened = open_udp_socket();
    REQUIRE(opened.ok());
    const RecvResult r = send_and_receive(*opened.socket, 1474);
    REQUIRE(r.status == RecvStatus::Oversize);
    REQUIRE(r.length == 0);
    REQUIRE(r.error == EMSGSIZE);
}

TEST_CASE("posix udp: the oversize datagram is consumed", "[network][udp][posix]") {
    auto opened = open_udp_socket();
    REQUIRE(opened.ok());
    UdpSocket& socket = *opened.socket;
    REQUIRE(send_and_receive(socket, 4000).status == RecvStatus::Oversize);
    std::array<std::byte, kRecvBufferSize> buffer{};
    REQUIRE(socket.recv_from(buffer).status == RecvStatus::WouldBlock);
}

TEST_CASE("posix udp: the read handle is level triggered and clears when drained", "[network][udp][posix]") {
    // enumerate_events 가 할 일이 없는 이유다. 비우면 대기가 더는 깨지 않는다.
    auto opened = open_udp_socket();
    REQUIRE(opened.ok());
    UdpSocket& socket = *opened.socket;
    const std::array<WaitHandle, 1> handles{socket.read_event()};
    REQUIRE(wait_any(handles, 0).status == WaitStatus::Timeout);

    const std::array<std::byte, 8> payload{};
    REQUIRE(socket.send_to(loopback(socket), payload).ok);
    REQUIRE(socket.send_to(loopback(socket), payload).ok);
    REQUIRE(wait_readable(socket));
    REQUIRE(socket.enumerate_events());
    REQUIRE(wait_any(handles, 0).status == WaitStatus::Signaled);  // 소비하지 않는다

    std::array<std::byte, kRecvBufferSize> buffer{};
    REQUIRE(socket.recv_from(buffer).status == RecvStatus::Received);
    REQUIRE(socket.recv_from(buffer).status == RecvStatus::Received);
    REQUIRE(socket.recv_from(buffer).status == RecvStatus::WouldBlock);
    REQUIRE(wait_any(handles, 0).status == WaitStatus::Timeout);
}

TEST_CASE("posix udp: an ICMP port unreachable does not surface as an error", "[network][udp][posix]") {
    // SIO_UDP_CONNRESET 대응이 없어도 되는 근거 (protocol.md 6장). 닫은 포트로 보낸 뒤에도 수신은
    // WouldBlock 이고, 그 뒤의 정상 데이터그램도 받는다.
    auto victim = open_udp_socket();
    REQUIRE(victim.ok());
    const Endpoint closed_port = loopback(*victim.socket);
    victim.socket.reset();

    auto opened = open_udp_socket();
    REQUIRE(opened.ok());
    UdpSocket& socket = *opened.socket;
    const std::array<std::byte, 8> payload{};
    REQUIRE(socket.send_to(closed_port, payload).ok);
    const std::array<WaitHandle, 1> handles{socket.read_event()};
    REQUIRE(wait_any(handles, 200).status == WaitStatus::Timeout);
    std::array<std::byte, kRecvBufferSize> buffer{};
    REQUIRE(socket.recv_from(buffer).status == RecvStatus::WouldBlock);

    REQUIRE(send_and_receive(socket, 16).status == RecvStatus::Received);
}

TEST_CASE("posix udp: the receive buffer request is applied and read back", "[network][udp][posix]") {
    auto opened = open_udp_socket();
    REQUIRE(opened.ok());
    int value = 0;
    socklen_t length = sizeof(value);
    REQUIRE(::getsockopt(static_cast<int>(opened.socket->native_handle()), SOL_SOCKET, SO_RCVBUF,
                         &value, &length) == 0);
    REQUIRE(opened.socket->rcvbuf_applied() == value);
    REQUIRE(opened.socket->local().port() != 0);
}

TEST_CASE("posix udp: a bound port cannot be taken by a second socket", "[network][udp][posix]") {
    // SO_REUSEADDR 를 켜지 않았으므로 기본이 배타다 (protocol.md 6장 SO_EXCLUSIVEADDRUSE 의 몫).
    auto opened = open_udp_socket();
    REQUIRE(opened.ok());
    const int other = ::socket(AF_INET, SOCK_DGRAM, 0);
    REQUIRE(other >= 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(opened.socket->local().port());
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    const int bound = ::bind(other, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    const int error = errno;
    ::close(other);
    REQUIRE(bound != 0);
    REQUIRE(error == EADDRINUSE);
}
