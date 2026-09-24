#include "hamychi/network/udp_socket.hpp"

#include "hamychi/network/endpoint.hpp"
#include "hamychi/network/wsa.hpp"
#include "hamychi/protocol_constants.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// 시험 케이스 이름은 ASCII 로만 적는다. 이유는 network/wsa_test.cpp 머리에 있다.
//
// 이 파일은 실제 소켓을 연다. 루프백으로만 보내고 외부로 나가지 않는다. 케이스마다
// 프로세스가 분리되므로 (catch_discover_tests) 소켓이 서로를 오염시키지 않는다.

using hamychi::network::Endpoint;
using hamychi::network::kRecvBufferSize;
using hamychi::network::open_udp_socket;
using hamychi::network::RecvStatus;
using hamychi::network::UdpSocket;
using hamychi::network::WsaContext;
using hamychi::protocol::kMaxDatagram;

namespace {

constexpr std::uint32_t kLoopback = 0x7F000001u;

// 읽을 것이 생길 때까지 기다린다. 루프백이라도 도착이 즉시는 아니다.
bool wait_readable(const UdpSocket& socket, unsigned timeout_ms = 2000) {
    const DWORD r = ::WaitForSingleObject(socket.read_event(), timeout_ms);
    return r == WAIT_OBJECT_0;
}

std::vector<std::byte> pattern(std::size_t size) {
    // architecture.md 3.5 의 `raw` 명령과 같은 규칙. 0x00 부터 1씩 증가한다.
    std::vector<std::byte> out(size);
    for (std::size_t i = 0; i < size; ++i) {
        out[i] = static_cast<std::byte>(i & 0xFF);
    }
    return out;
}

}  // namespace

TEST_CASE("protocol: the datagram limit is the header plus the inner limit", "[protocol]") {
    // protocol.md 3장이 MAX_DATAGRAM 을 합으로 정의한다. 옮겨 적다 틀리면 여기서 걸린다.
    REQUIRE(hamychi::protocol::kMaxDatagram ==
            hamychi::protocol::kHeaderSize + hamychi::protocol::kMaxInner);
    REQUIRE(hamychi::protocol::kMaxDatagram == 1472);
    REQUIRE(kRecvBufferSize == 1473);
}

TEST_CASE("udp: open binds to any address on an ephemeral port", "[udp]") {
    const WsaContext wsa;
    auto opened = open_udp_socket();
    INFO("failed_op=" << opened.failed_op << " error=" << opened.error);
    REQUIRE(opened.ok());

    // protocol.md 6장: INADDR_ANY 와 포트 0 으로 bind 하고 실제 포트를 읽는다.
    REQUIRE(opened.socket->local().address() == 0u);
    REQUIRE(opened.socket->local().port() != 0);
}

TEST_CASE("udp: two sockets can be open at once", "[udp]") {
    // 포트를 고정했다면 두 번째가 실패한다 (roadmap.md Phase 1 bind 계약).
    const WsaContext wsa;
    auto first = open_udp_socket();
    auto second = open_udp_socket();
    REQUIRE(first.ok());
    REQUIRE(second.ok());
    REQUIRE(first.socket->local().port() != second.socket->local().port());
}

TEST_CASE("udp: the receive buffer request and the applied value are both readable", "[udp]") {
    const WsaContext wsa;
    auto opened = open_udp_socket();
    REQUIRE(opened.ok());

    // protocol.md 6장: 요청값은 262144 다. 단위 접미사가 아니라 바이트 수다.
    REQUIRE(opened.socket->rcvbuf_requested() == 262144);

    // 적용값은 한 바퀴 예산 이상이어야 한다. MAX_DRAIN(64) x MAX_DATAGRAM(1472) = 94208.
    REQUIRE(opened.socket->rcvbuf_applied() >= 94208);
}

TEST_CASE("udp: the applied value comes from the socket, not from the request", "[udp]") {
    // 변이 시험이 드러낸 자리다. getsockopt 를 지우고 요청값을 그대로 적어도 "적용값이
    // 94208 이상" 케이스는 통과한다. 두 값이 같기 때문이다.
    //
    // 그래서 소켓에 직접 물어본 값과 대조한다. protocol.md 6장이 적용값을 기록하라고 한
    // 이유가 OS 가 요청값을 그대로 주지 않을 수 있다는 것이므로, 그 출처가 검증되어야 한다.
    const WsaContext wsa;
    auto opened = open_udp_socket();
    REQUIRE(opened.ok());

    int queried = 0;
    int queried_len = sizeof(queried);
    const int rc = ::getsockopt(static_cast<SOCKET>(opened.socket->native_handle()),
                                SOL_SOCKET, SO_RCVBUF,
                                reinterpret_cast<char*>(&queried), &queried_len);
    REQUIRE(rc == 0);
    REQUIRE(opened.socket->rcvbuf_applied() == queried);
}

TEST_CASE("udp: a datagram survives the round trip byte for byte", "[udp]") {
    const WsaContext wsa;
    auto sender = open_udp_socket();
    auto receiver = open_udp_socket();
    REQUIRE(sender.ok());
    REQUIRE(receiver.ok());

    const auto payload = pattern(64);
    const Endpoint to(kLoopback, receiver.socket->local().port());
    const auto sent = sender.socket->send_to(to, payload);
    INFO("send error=" << sent.error);
    REQUIRE(sent.ok);

    REQUIRE(wait_readable(*receiver.socket));
    REQUIRE(receiver.socket->enumerate_events());

    std::array<std::byte, kRecvBufferSize> buffer{};
    const auto got = receiver.socket->recv_from(buffer);
    REQUIRE(got.status == RecvStatus::Received);
    REQUIRE(got.length == payload.size());
    REQUIRE(got.from.port() == sender.socket->local().port());
    REQUIRE(got.from.address() == kLoopback);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        INFO("byte " << i);
        REQUIRE(buffer[i] == payload[i]);
    }
}

TEST_CASE("udp: receiving from an empty socket reports would block", "[udp]") {
    const WsaContext wsa;
    auto opened = open_udp_socket();
    REQUIRE(opened.ok());

    std::array<std::byte, kRecvBufferSize> buffer{};
    const auto got = opened.socket->recv_from(buffer);
    REQUIRE(got.status == RecvStatus::WouldBlock);
    REQUIRE(got.error == 0);
}

TEST_CASE("udp: a datagram of exactly MAX_DATAGRAM still fits", "[udp]") {
    const WsaContext wsa;
    auto sender = open_udp_socket();
    auto receiver = open_udp_socket();
    REQUIRE(sender.ok());
    REQUIRE(receiver.ok());

    const auto payload = pattern(kMaxDatagram);
    const Endpoint to(kLoopback, receiver.socket->local().port());
    REQUIRE(sender.socket->send_to(to, payload).ok);
    REQUIRE(wait_readable(*receiver.socket));
    REQUIRE(receiver.socket->enumerate_events());

    std::array<std::byte, kRecvBufferSize> buffer{};
    const auto got = receiver.socket->recv_from(buffer);
    REQUIRE(got.status == RecvStatus::Received);
    REQUIRE(got.length == kMaxDatagram);
}

TEST_CASE("udp: one byte over the limit is caught by the length compare", "[udp]") {
    // architecture.md 3.2.3 (a): 정확히 1473바이트는 길이 비교로 걸린다.
    // 버퍼를 1 크게 잡았기 때문에 오류 코드가 아니라 길이로 판정된다.
    const WsaContext wsa;
    auto sender = open_udp_socket();
    auto receiver = open_udp_socket();
    REQUIRE(sender.ok());
    REQUIRE(receiver.ok());

    const auto payload = pattern(kMaxDatagram + 1);
    const Endpoint to(kLoopback, receiver.socket->local().port());
    REQUIRE(sender.socket->send_to(to, payload).ok);
    REQUIRE(wait_readable(*receiver.socket));
    REQUIRE(receiver.socket->enumerate_events());

    std::array<std::byte, kRecvBufferSize> buffer{};
    const auto got = receiver.socket->recv_from(buffer);
    REQUIRE(got.status == RecvStatus::Oversize);
    REQUIRE(got.error == 0);  // 길이 비교 경로다. 오류 코드가 아니다
}

TEST_CASE("udp: two bytes over the limit is caught by WSAEMSGSIZE", "[udp]") {
    // architecture.md 3.2.3 (b): 1474바이트 이상은 WSAEMSGSIZE 로 걸린다.
    // (a) 만 시험하면 이 경로에서 바퀴를 끝내는 구현이 통과한다.
    const WsaContext wsa;
    auto sender = open_udp_socket();
    auto receiver = open_udp_socket();
    REQUIRE(sender.ok());
    REQUIRE(receiver.ok());

    const auto payload = pattern(kMaxDatagram + 2);
    const Endpoint to(kLoopback, receiver.socket->local().port());
    REQUIRE(sender.socket->send_to(to, payload).ok);
    REQUIRE(wait_readable(*receiver.socket));
    REQUIRE(receiver.socket->enumerate_events());

    std::array<std::byte, kRecvBufferSize> buffer{};
    const auto got = receiver.socket->recv_from(buffer);
    REQUIRE(got.status == RecvStatus::Oversize);
    REQUIRE(got.error == WSAEMSGSIZE);
}

TEST_CASE("udp: an unreachable port does not break the receive loop", "[udp]") {
    // protocol.md 6장: SIO_UDP_CONNRESET 을 끄지 않으면 응답 없는 후보가 보낸 ICMP
    // port unreachable 이 WSAECONNRESET 으로 올라와 수신 루프를 끝낸다. 홀펀칭은 응답
    // 없는 후보로 계속 쏘는 것이 정상이므로 Phase 4 에서 반드시 터진다.
    const WsaContext wsa;
    auto sender = open_udp_socket();
    REQUIRE(sender.ok());

    // 닫힌 포트를 하나 확보한다. 열었다가 닫으면 그 포트는 비어 있다.
    //
    // 한계. 그 사이에 다른 프로세스가 같은 포트를 잡을 수 있다. 그래도 이 케이스가 틀린
    // 실패를 내지는 않는다. 누가 잡았으면 ICMP 가 돌아오지 않아 수신은 여전히 비어 있다.
    // 잃는 것은 그 실행에서 이 케이스가 아무것도 확인하지 못한다는 것뿐이고, 변이 시험이
    // SIO_UDP_CONNRESET 을 켠 변이를 실제로 죽이는 것으로 효력을 확인했다.
    std::uint16_t closed_port = 0;
    {
        auto victim = open_udp_socket();
        REQUIRE(victim.ok());
        closed_port = victim.socket->local().port();
    }

    const Endpoint to(kLoopback, closed_port);
    const auto payload = pattern(16);
    for (int i = 0; i < 4; ++i) {
        REQUIRE(sender.socket->send_to(to, payload).ok);
    }

    // ICMP 가 돌아올 시간을 준다. 돌아와도 수신은 비어 있어야 한다.
    ::Sleep(100);

    std::array<std::byte, kRecvBufferSize> buffer{};
    const auto got = sender.socket->recv_from(buffer);
    INFO("status=" << static_cast<int>(got.status) << " error=" << got.error);
    REQUIRE(got.error != WSAECONNRESET);
    REQUIRE(got.status == RecvStatus::WouldBlock);
}

TEST_CASE("udp: the read event is reset by enumerating events", "[udp]") {
    // architecture.md 3.2.3: 이 호출을 빠뜨리면 이벤트가 신호 상태로 남아 루프가 스핀한다.
    const WsaContext wsa;
    auto sender = open_udp_socket();
    auto receiver = open_udp_socket();
    REQUIRE(sender.ok());
    REQUIRE(receiver.ok());

    const Endpoint to(kLoopback, receiver.socket->local().port());
    REQUIRE(sender.socket->send_to(to, pattern(8)).ok);
    REQUIRE(wait_readable(*receiver.socket));

    std::array<std::byte, kRecvBufferSize> buffer{};
    REQUIRE(receiver.socket->enumerate_events());
    REQUIRE(receiver.socket->recv_from(buffer).status == RecvStatus::Received);

    // 다 읽었고 이벤트를 리셋했으므로 이제 기다리면 바로 돌아오지 않아야 한다.
    REQUIRE_FALSE(wait_readable(*receiver.socket, 100));
}

TEST_CASE("udp: a rejected send reports the error instead of swallowing it", "[udp]") {
    // protocol.md 6장: 실패는 카운터를 올리고 버린다. 재전송하지 않는다.
    // 여기서는 래퍼가 실패를 삼키지 않고 돌려주는지 본다.
    //
    // 브로드캐스트 주소를 고른 이유는 실패가 결정적이기 때문이다. SO_BROADCAST 를 켜지
    // 않은 소켓에서 그리로 보내면 WSAEACCES 가 난다. 이 소켓은 그 옵션을 켜지 않으며,
    // protocol.md 10.1 이 브로드캐스트를 후보에서 막는 것과 같은 방향이다.
    const WsaContext wsa;
    auto sender = open_udp_socket();
    REQUIRE(sender.ok());

    const Endpoint to(0xFFFFFFFFu, 9);
    const auto sent = sender.socket->send_to(to, pattern(8));
    REQUIRE_FALSE(sent.ok);
    REQUIRE(sent.error == WSAEACCES);
}

TEST_CASE("udp: a payload that cannot fit the length argument is rejected", "[udp]") {
    // sendto 의 길이 인자가 int 다. 좁히기 전에 걸러야 한다.
    //
    // 크기만 거짓인 span 을 만든다. 실제 메모리를 읽지 않는다. 가드가 먼저 돌려보내므로
    // sendto 에 닿지 않는다는 것이 이 케이스가 보는 것이다.
    const WsaContext wsa;
    auto sender = open_udp_socket();
    REQUIRE(sender.ok());

    std::array<std::byte, 8> small{};
    const std::span<const std::byte> oversized(
        small.data(), static_cast<std::size_t>(INT_MAX) + 1);

    const Endpoint to(kLoopback, sender.socket->local().port());
    const auto sent = sender.socket->send_to(to, oversized);
    REQUIRE_FALSE(sent.ok);
    REQUIRE(sent.error == WSAEMSGSIZE);
}

TEST_CASE("udp: a larger caller buffer does not defeat the oversize paths", "[udp]") {
    // 읽는 길이를 kRecvBufferSize 로 자르지 않으면, 큰 버퍼를 준 호출자에게는 1474바이트
    // 이상도 그냥 담겨 WSAEMSGSIZE 분기가 영영 돌지 않는다 (architecture.md 3.2.3).
    const WsaContext wsa;
    auto sender = open_udp_socket();
    auto receiver = open_udp_socket();
    REQUIRE(sender.ok());
    REQUIRE(receiver.ok());

    const auto payload = pattern(kMaxDatagram + 200);
    const Endpoint to(kLoopback, receiver.socket->local().port());
    REQUIRE(sender.socket->send_to(to, payload).ok);
    REQUIRE(wait_readable(*receiver.socket));
    REQUIRE(receiver.socket->enumerate_events());

    std::vector<std::byte> big(8192);
    const auto got = receiver.socket->recv_from(big);
    REQUIRE(got.status == RecvStatus::Oversize);
    REQUIRE(got.error == WSAEMSGSIZE);  // 자르지 않으면 여기가 0 이 된다
}
