#include "sangtachi/peer/session.hpp"

#include "../log_capture.hpp"
#include "../session_fixture.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// 시험 케이스 이름은 ASCII 로만 적는다. 이유는 network/wsa_test.cpp 머리에 있다.
//
// protocol.md 9장 세션, 11장 타이머, 5.5, 5.6, 4.4, 10.4 (후보 집합 안까지) 를 본다. 시계와 송신은
// 주입한다. 와이어 캡처가 필요한 roadmap.md Phase 4 검증 항목은 여기서 송신 기록으로 같은 사실을
// 본다. 캡처 자체는 4차 K 의 체크리스트다.

using namespace sangtachi;
using namespace sangtachi::testing;
using peer::Session;
using peer::SessionFailure;
using peer::SessionState;
using tunnel::PacketType;

namespace {

constexpr Millis kT0 = 1000000;

struct Fixture {
    FakeIo io;
    Session session;

    explicit Fixture(bool sink = false) : io(sink), session(session_config(), io.io(), kT0) {}

    peer::RxResult rx(const Rx& packet, Millis now) { return session.receive(packet.view(), now); }
    // 이 세션의 표 (counters.hpp 의 두 범위).
    [[nodiscard]] std::uint64_t count(Counter c) const { return session.counters().value(c); }

    // 양쪽 증거를 세워 CONNECTED 로 만든다. 상대 seq 0, 1 을 쓴다.
    void connect(Millis now = kT0) {
        session.start_punch(now);
        REQUIRE(rx(hello(kPeerEpoch, 0), now).accepted());
        REQUIRE(rx(hello_ack(kPeerEpoch, 1), now).accepted());
        REQUIRE(session.state() == SessionState::kConnected);
    }
};

}  // namespace

// ---------------------------------------------------------------- 생성과 펀치 (9.1, 10.3)

TEST_CASE("session: a new session is PUNCHING with no timers and no sends", "[peer][session]") {
    Fixture f;
    REQUIRE(f.session.state() == SessionState::kPunching);
    REQUIRE_FALSE(f.session.got_ack());
    REQUIRE_FALSE(f.session.sent_ack());
    REQUIRE(f.io.sent.empty());
    REQUIRE_FALSE(f.session.next_deadline().has_value());
    REQUIRE_FALSE(f.session.pinned_peer_epoch().has_value());
}

TEST_CASE("session: punch start sends HELLO to every candidate and arms both timers",
          "[peer][session]") {
    Fixture f;
    REQUIRE(f.session.start_punch(kT0));
    const auto hellos = f.io.of_type(PacketType::kHello);
    REQUIRE(hellos.size() == 2);
    REQUIRE(hellos[0].to == kCandA);
    REQUIRE(hellos[1].to == kCandB);
    for (const auto& h : hellos) {
        REQUIRE(h.header.peer_id == kLocalPeerId);
        REQUIRE(h.header.session_epoch == kLocalEpoch);
        const auto decoded = tunnel::decode_hello(h.payload);
        REQUIRE(decoded.ok());
        REQUIRE(decoded.value.nonce == wire_nonce(kLocalNonce));
        REQUIRE(decoded.value.virtual_ip == kLocalVip);
    }
    // 4.4: 첫 송신이 0, 재전송과 후보마다 새 번호
    REQUIRE(hellos[0].header.sequence == 0);
    REQUIRE(hellos[1].header.sequence == 1);
    REQUIRE(f.session.hello_due() == kT0 + 200);
    REQUIRE(f.session.punch_deadline() == kT0 + 10000);
    REQUIRE_FALSE(f.session.start_punch(kT0 + 1));
}

TEST_CASE("session: HELLO retransmits every 200ms with the same nonce and new sequences",
          "[peer][session]") {
    Fixture f;
    f.session.start_punch(kT0);
    f.session.poll(kT0 + 199);
    REQUIRE(f.io.sent.size() == 2);
    f.session.poll(kT0 + 200);
    REQUIRE(f.io.sent.size() == 4);
    REQUIRE(f.io.sent[2].header.sequence == 2);
    REQUIRE(f.io.sent[3].header.sequence == 3);
    REQUIRE(tunnel::decode_hello(f.io.sent[3].payload).value.nonce == wire_nonce(kLocalNonce));
    REQUIRE(f.session.hello_due() == kT0 + 400);
}

// ---------------------------------------------------------------- 9.2 이중 플래그, 9.4 전이표

TEST_CASE("session: HELLO_ACK arriving first in PUNCHING is accepted", "[peer][session]") {
    // roadmap.md Phase 4 상태 머신 1번. 9.4 "PUNCHING 에서 HELLO_ACK 이 먼저 오는 것은 정상이다".
    Fixture f;
    f.session.start_punch(kT0);
    const auto r = f.rx(hello_ack(kPeerEpoch, 0), kT0 + 10);
    REQUIRE(r.accepted());
    REQUIRE(f.session.got_ack());
    REQUIRE_FALSE(f.session.sent_ack());
    REQUIRE(f.session.state() == SessionState::kHandshaking);
    REQUIRE(f.session.pinned_peer_epoch() == kPeerEpoch);  // 8.2 nonce 유효 HELLO_ACK 의 epoch 고정
    REQUIRE(f.session.peer_endpoint() == kCandA);
}

TEST_CASE("session: HELLO is answered to the datagram source and sets sent_ack",
          "[peer][session]") {
    Fixture f;
    f.session.start_punch(kT0);
    f.io.sent.clear();
    REQUIRE(f.rx(hello(kPeerEpoch, 0, kCandB), kT0 + 10).accepted());
    const auto acks = f.io.of_type(PacketType::kHelloAck);
    REQUIRE(acks.size() == 1);
    REQUIRE(acks[0].to == kCandB);
    const auto decoded = tunnel::decode_hello(acks[0].payload);
    REQUIRE(decoded.value.nonce == wire_nonce(kPeerNonce));  // echo_nonce
    REQUIRE(decoded.value.virtual_ip == kLocalVip);
    REQUIRE(f.session.sent_ack());
    REQUIRE(f.session.state() == SessionState::kHandshaking);
    REQUIRE(f.session.pinned_peer_epoch() == kPeerEpoch);
}

TEST_CASE("session: sent_ack is not set when sending HELLO_ACK fails", "[peer][session]") {
    // 9.2 "sendto 가 성공을 반환한 뒤에만 세운다. 실패하면 tx_err_ack".
    Fixture f;
    f.io.send_ok = false;
    REQUIRE(f.rx(hello(kPeerEpoch, 0), kT0).accepted());
    REQUIRE_FALSE(f.session.sent_ack());
    REQUIRE(f.session.state() == SessionState::kPunching);
    REQUIRE(f.count(Counter::TxErrAck) == 1);
    REQUIRE(f.count(Counter::TxErrSend) == 1);  // 세션의 송신 실패는 그 세션의 표에 센다
}

TEST_CASE("session: both orders reach CONNECTED", "[peer][session]") {
    // 9.2 "어느 쪽이 먼저 서는지는 정해져 있지 않다".
    {
        Fixture f;
        f.session.start_punch(kT0);
        REQUIRE(f.rx(hello(kPeerEpoch, 0), kT0).accepted());
        REQUIRE(f.rx(hello_ack(kPeerEpoch, 1), kT0).accepted());
        REQUIRE(f.session.state() == SessionState::kConnected);
    }
    {
        Fixture f;
        f.session.start_punch(kT0);
        REQUIRE(f.rx(hello_ack(kPeerEpoch, 0), kT0).accepted());
        REQUIRE(f.rx(hello(kPeerEpoch, 1), kT0).accepted());
        REQUIRE(f.session.state() == SessionState::kConnected);
    }
}

TEST_CASE("session: only one side of evidence never becomes CONNECTED", "[peer][session]") {
    // roadmap.md Phase 4 "한쪽만 HELLO 를 받은 상태에서는 CONNECTED 로 전이하지 않음".
    Fixture f;
    f.session.start_punch(kT0);
    for (std::uint32_t seq = 0; seq < 20; ++seq) {
        REQUIRE(f.rx(hello(kPeerEpoch, seq), kT0 + seq * 200).accepted());
        f.session.poll(kT0 + seq * 200);
    }
    REQUIRE(f.session.state() == SessionState::kHandshaking);
    REQUIRE_FALSE(f.session.got_ack());
}

TEST_CASE("session: entering CONNECTED sends one KEEPALIVE and switches timers",
          "[peer][session]") {
    Fixture f;
    f.connect(kT0 + 300);
    const auto ka = f.io.of_type(PacketType::kKeepalive);
    REQUIRE(ka.size() == 1);
    REQUIRE(ka[0].to == kCandA);  // 학습한 peer_endpoint (10.4)
    REQUIRE_FALSE(f.session.hello_due().has_value());
    REQUIRE_FALSE(f.session.punch_deadline().has_value());
    REQUIRE(f.session.keepalive_due() == kT0 + 300 + 15000);
    REQUIRE(f.session.idle_deadline() == kT0 + 300 + 50000);
    REQUIRE(f.session.ping_due() == kT0 + 300 + 5000);  // 진입 즉시 보내지 않는다
    REQUIRE(f.io.of_type(PacketType::kPing).empty());
    REQUIRE(f.io.transitions.back() ==
            std::pair{SessionState::kHandshaking, SessionState::kConnected});
}

TEST_CASE("session: HELLO retransmit in CONNECTED is answered again", "[peer][session]") {
    // roadmap.md Phase 4 상태 머신 2번. 9.4: 무시하면 상대가 타임아웃한다.
    Fixture f;
    f.connect();
    f.io.sent.clear();
    REQUIRE(f.rx(hello(kPeerEpoch, 2), kT0 + 200).accepted());
    REQUIRE(f.io.of_type(PacketType::kHelloAck).size() == 1);
    REQUIRE(f.session.state() == SessionState::kConnected);
    REQUIRE(f.session.renegotiations() == 0);
}

TEST_CASE("session: HELLO_ACK in CONNECTED counts dup_ack and refreshes idle", "[peer][session]") {
    Fixture f;
    f.connect();
    REQUIRE(f.rx(hello_ack(kPeerEpoch, 2), kT0 + 7000).accepted());
    REQUIRE(f.count(Counter::DupAck) == 1);
    REQUIRE(f.session.state() == SessionState::kConnected);
    REQUIRE(f.session.idle_deadline() == kT0 + 7000 + 50000);
}

TEST_CASE("session: HELLO_ACK in HANDSHAKING does not count dup_ack", "[peer][session]") {
    Fixture f;
    f.session.start_punch(kT0);
    REQUIRE(f.rx(hello_ack(kPeerEpoch, 0), kT0).accepted());
    REQUIRE(f.rx(hello_ack(kPeerEpoch, 1), kT0).accepted());
    REQUIRE(f.count(Counter::DupAck) == 0);
}

// ---------------------------------------------------------------- 8.2 epoch 및 출발지 검사

TEST_CASE("session: a HELLO with the wrong virtual_ip is dropped before it can pin",
          "[peer][session]") {
    Fixture f;
    const auto r = f.rx(hello(kPeerEpoch, 0, kCandA, kPeerVip + 1), kT0);
    REQUIRE(r.dropped == Counter::DropVipMismatch);
    REQUIRE_FALSE(f.session.pinned_peer_epoch().has_value());
    REQUIRE(f.io.sent.empty());
    REQUIRE_FALSE(f.session.window_initialized());
}

TEST_CASE("session: a pinned session drops a wrong-vip HELLO before renegotiation",
          "[peer][session]") {
    Fixture f;
    f.connect();
    const auto r = f.rx(hello(kPeerEpoch2, 0, kCandA, kPeerVip + 1), kT0 + 5000);
    REQUIRE(r.dropped == Counter::DropVipMismatch);
    REQUIRE(f.session.renegotiations() == 0);
}

TEST_CASE("session: HELLO_ACK checks virtual_ip, then nonce, then epoch", "[peer][session]") {
    Fixture f;
    f.session.start_punch(kT0);
    REQUIRE(f.rx(hello_ack(kPeerEpoch, 0, kCandA, kPeerVip + 1), kT0).dropped ==
            Counter::DropVipMismatch);
    REQUIRE(f.rx(hello_ack(kPeerEpoch, 1, kCandA, kPeerVip, nonce_fill(0x33)), kT0).dropped ==
            Counter::DropBadNonce);
    REQUIRE_FALSE(f.session.pinned_peer_epoch().has_value());
    REQUIRE_FALSE(f.session.got_ack());
    // 고정 뒤
    REQUIRE(f.rx(hello(kPeerEpoch, 2), kT0).accepted());
    REQUIRE(f.rx(hello_ack(kPeerEpoch2, 0), kT0).dropped == Counter::DropStaleEpoch);
    // nonce 가 틀리고 epoch 도 다르면 nonce 가 먼저다 (8.2 표의 행 순서)
    REQUIRE(f.rx(hello_ack(kPeerEpoch2, 1, kCandA, kPeerVip, nonce_fill(0x33)), kT0).dropped ==
            Counter::DropBadNonce);
    REQUIRE_FALSE(f.session.got_ack());
}

TEST_CASE("session: other types need a pinned and matching epoch", "[peer][session]") {
    Fixture f;
    REQUIRE(f.rx(keepalive(kPeerEpoch, 0), kT0).dropped == Counter::DropNoEpoch);
    REQUIRE(f.rx(ping(kPeerEpoch, 1, 7), kT0).dropped == Counter::DropNoEpoch);
    REQUIRE(f.io.sent.empty());
    REQUIRE(f.rx(hello(kPeerEpoch, 2), kT0).accepted());
    REQUIRE(f.rx(keepalive(kPeerEpoch2, 3), kT0).dropped == Counter::DropStaleEpoch);
    REQUIRE(f.rx(keepalive(kPeerEpoch, 3), kT0).accepted());
}

// ---------------------------------------------------------------- 8.3 중복 억제

TEST_CASE("session: duplicates and too-old packets change nothing", "[peer][session]") {
    Fixture f;
    f.connect();
    REQUIRE(f.rx(keepalive(kPeerEpoch, 100), kT0 + 1000).accepted());
    REQUIRE(f.session.idle_deadline() == kT0 + 1000 + 50000);
    REQUIRE(f.rx(keepalive(kPeerEpoch, 100), kT0 + 2000).dropped == Counter::DropDuplicate);
    REQUIRE(f.rx(keepalive(kPeerEpoch, 30), kT0 + 2000).dropped == Counter::DropTooOld);
    REQUIRE(f.session.idle_deadline() == kT0 + 1000 + 50000);
    f.io.sent.clear();
    REQUIRE(f.rx(hello(kPeerEpoch, 100), kT0 + 2000).dropped == Counter::DropDuplicate);
    REQUIRE(f.io.sent.empty());
}

TEST_CASE("session: a packet failing a type check after 8.3 is still recorded in the window",
          "[peer][session]") {
    // 8.3 "8.3 을 통과한 패킷은 뒤의 타입별 검사에서 버려져도 수용 창에 기록된다".
    Fixture f;
    f.connect();
    REQUIRE(f.rx(close_rx(kPeerEpoch, 5, 0x04), kT0).dropped == Counter::DropBadReason);
    REQUIRE(f.session.state() == SessionState::kConnected);
    REQUIRE(f.rx(close_rx(kPeerEpoch, 5, 0x00), kT0).dropped == Counter::DropDuplicate);
    REQUIRE(f.session.state() == SessionState::kConnected);
}

// ---------------------------------------------------------------- 8.4 DATA

TEST_CASE("session: DATA before CONNECTED is drop_data_early", "[peer][session]") {
    Fixture f(true);
    f.session.start_punch(kT0);
    REQUIRE(f.rx(hello(kPeerEpoch, 0), kT0).accepted());
    REQUIRE(f.rx(data_rx(kPeerEpoch, 1, inner_ipv4(kPeerVip, kLocalVip)), kT0).dropped ==
            Counter::DropDataEarly);
    REQUIRE(f.io.sunk.empty());
}

TEST_CASE("session: valid DATA reaches the sink byte for byte", "[peer][session]") {
    Fixture f(true);
    f.connect();
    const auto inner = inner_ipv4(kPeerVip, kLocalVip);
    REQUIRE(f.rx(data_rx(kPeerEpoch, 2, inner), kT0 + 3000).accepted());
    REQUIRE(f.io.sunk.size() == 1);
    REQUIRE(f.io.sunk[0] == inner);
    REQUIRE(f.session.idle_deadline() == kT0 + 3000 + 50000);
}

TEST_CASE("session: valid DATA without a sink is drop_no_sink", "[peer][session]") {
    Fixture f(false);
    f.connect();
    REQUIRE(f.rx(data_rx(kPeerEpoch, 2, inner_ipv4(kPeerVip, kLocalVip)), kT0).dropped ==
            Counter::DropNoSink);
}

TEST_CASE("session: DATA with the wrong inner source or destination is dropped",
          "[peer][session]") {
    // roadmap.md Phase 4 DATA 왕복의 둘째 항목.
    Fixture f(true);
    f.connect();
    REQUIRE(f.rx(data_rx(kPeerEpoch, 2, inner_ipv4(kPeerVip + 2, kLocalVip)), kT0 + 100).dropped ==
            Counter::DropInnerSrc);
    REQUIRE(f.rx(data_rx(kPeerEpoch, 3, inner_ipv4(kPeerVip, kLocalVip + 2)), kT0 + 100).dropped ==
            Counter::DropInnerDst);
    REQUIRE(f.io.sunk.empty());
    REQUIRE(f.session.idle_deadline() == kT0 + 50000);  // 버려진 패킷은 갱신하지 않는다
}

// ---------------------------------------------------------------- 5.5 PING / PONG

TEST_CASE("session: PING is answered with PONG to the source in any active state",
          "[peer][session]") {
    Fixture f;
    REQUIRE(f.rx(hello(kPeerEpoch, 0), kT0).accepted());
    f.io.sent.clear();
    REQUIRE(f.rx(ping(kPeerEpoch, 1, 0x0102030405060708ull, kCandB), kT0).accepted());
    const auto pongs = f.io.of_type(PacketType::kPong);
    REQUIRE(pongs.size() == 1);
    REQUIRE(pongs[0].to == kCandB);
    REQUIRE(tunnel::decode_ping(pongs[0].payload).ping_id == 0x0102030405060708ull);
}

TEST_CASE("session: PING starts 5s after CONNECTED and PONG yields one RTT sample",
          "[peer][session]") {
    Fixture f;
    f.connect();
    f.session.poll(kT0 + 4999);
    REQUIRE(f.io.of_type(PacketType::kPing).empty());
    f.io.us = 1000000;
    f.session.poll(kT0 + 5000);
    const auto pings = f.io.of_type(PacketType::kPing);
    REQUIRE(pings.size() == 1);
    REQUIRE(pings[0].to == kCandA);
    REQUIRE(tunnel::decode_ping(pings[0].payload).ping_id == 0);
    REQUIRE(f.session.pending_pings() == 1);
    REQUIRE(f.session.ping_due() == kT0 + 10000);

    f.io.us = 1023410;
    testing::LogCapture logs;
    REQUIRE(f.rx(pong(kPeerEpoch, 2, 0), kT0 + 5020).accepted());
    REQUIRE(f.session.last_rtt_us() == 23410u);
    // architecture.md 9장 session.rtt. 표본마다 한 줄
    const auto rtt = logs.with_event("session.rtt");
    REQUIRE(rtt.size() == 1);
    REQUIRE(rtt[0].find(" rtt_us=23410") != std::string::npos);
    REQUIRE(f.session.rtt_samples() == 1);
    REQUIRE(f.session.pending_pings() == 0);
    // 각 ping_id 는 한 번만 유효하다
    REQUIRE(f.rx(pong(kPeerEpoch, 3, 0), kT0 + 5030).dropped == Counter::DropUnmatchedPong);
    REQUIRE(f.rx(pong(kPeerEpoch, 4, 99), kT0 + 5030).dropped == Counter::DropUnmatchedPong);
    REQUIRE(f.session.rtt_samples() == 1);
}

TEST_CASE("session: PONG before CONNECTED never makes a sample", "[peer][session]") {
    Fixture f;
    REQUIRE(f.rx(hello(kPeerEpoch, 0), kT0).accepted());
    REQUIRE(f.rx(pong(kPeerEpoch, 1, 0), kT0).dropped == Counter::DropUnmatchedPong);
    REQUIRE(f.session.rtt_samples() == 0);
}

TEST_CASE("session: a PONG for an entry older than 5s is unmatched", "[peer][session]") {
    Fixture f;
    f.connect();
    f.session.poll(kT0 + 5000);  // ping 0
    REQUIRE(f.rx(pong(kPeerEpoch, 2, 0), kT0 + 10000).dropped == Counter::DropUnmatchedPong);
    REQUIRE(f.session.rtt_samples() == 0);
}

TEST_CASE("session: a matched PONG one ms before expiry still counts", "[peer][session]") {
    Fixture f;
    f.connect();
    f.session.poll(kT0 + 5000);
    REQUIRE(f.rx(pong(kPeerEpoch, 2, 0), kT0 + 9999).accepted());
}

// ---------------------------------------------------------------- 11장 KEEPALIVE, 유휴

TEST_CASE("session: KEEPALIVE goes out at entry and every 15s", "[peer][session]") {
    // roadmap.md Phase 4 "keepalive 주기". 캡처 대신 시계 주입과 송신 기록으로 본다.
    Fixture f;
    f.connect();
    std::vector<Millis> times;
    for (Millis t = kT0; t <= kT0 + 45000; t += 500) {
        const auto before = f.io.of_type(PacketType::kKeepalive).size();
        f.session.poll(t);
        // 유휴로 끝나지 않게 상대 패킷을 넣는다
        if (t % 10000 == 0) {
            (void)f.rx(keepalive(kPeerEpoch, static_cast<std::uint32_t>(10 + (t - kT0) / 500)), t);
        }
        if (f.io.of_type(PacketType::kKeepalive).size() != before) {
            times.push_back(t - kT0);
        }
    }
    REQUIRE(times == std::vector<Millis>{15000, 30000, 45000});
    REQUIRE(f.io.of_type(PacketType::kKeepalive).size() == 4);  // 진입 1회 + 셋
}

TEST_CASE("session: idle timeout fails CONNECTED as TUNNEL_DROPPED and sends CLOSE 0x03",
          "[peer][session]") {
    Fixture f;
    f.connect();
    f.session.poll(kT0 + 49999);
    REQUIRE(f.session.state() == SessionState::kConnected);
    f.io.sent.clear();
    f.session.poll(kT0 + 50000);
    REQUIRE(f.session.state() == SessionState::kFailed);
    REQUIRE(f.session.failure() == SessionFailure::kTunnelDropped);
    const auto closes = f.io.of_type(PacketType::kClose);
    REQUIRE(closes.size() == 1);
    REQUIRE(closes[0].to == kCandA);
    REQUIRE(tunnel::decode_close(closes[0].payload).reason == tunnel::CloseReason::kSetupFailed);
    REQUIRE(f.io.sent.size() == 1);  // 같은 바퀴에 KEEPALIVE·PING 이 더 나가지 않는다
    REQUIRE_FALSE(f.session.next_deadline().has_value());
}

TEST_CASE("session: a validated packet refreshes the idle timer", "[peer][session]") {
    Fixture f;
    f.connect();
    REQUIRE(f.rx(keepalive(kPeerEpoch, 2), kT0 + 40000).accepted());
    f.session.poll(kT0 + 50000);
    REQUIRE(f.session.state() == SessionState::kConnected);
    f.session.poll(kT0 + 89999);
    REQUIRE(f.session.state() == SessionState::kConnected);
    f.session.poll(kT0 + 90000);
    REQUIRE(f.session.failure() == SessionFailure::kTunnelDropped);
}

// ---------------------------------------------------------------- 9.6 실패 전이

TEST_CASE("session: punch deadline without evidence is HOLE_PUNCH_TIMEOUT and stops HELLO",
          "[peer][session]") {
    Fixture f;
    f.session.start_punch(kT0);
    f.session.poll(kT0 + 9999);
    REQUIRE(f.session.active());
    const auto before = f.io.sent.size();
    f.session.poll(kT0 + 10000);
    REQUIRE(f.session.state() == SessionState::kFailed);
    REQUIRE(f.session.failure() == SessionFailure::kHolePunchTimeout);
    REQUIRE(f.io.sent.size() == before);  // 학습 전이라 CLOSE 도 HELLO 도 없다
    for (Millis t = kT0 + 10000; t < kT0 + 20000; t += 100) {
        f.session.poll(t);
    }
    REQUIRE(f.io.sent.size() == before);
    REQUIRE_FALSE(f.session.hello_due().has_value());
}

TEST_CASE("session: punch deadline with one flag is PEER_HANDSHAKE_FAILED with CLOSE 0x03",
          "[peer][session]") {
    Fixture f;
    f.session.start_punch(kT0);
    REQUIRE(f.rx(hello(kPeerEpoch, 0, kCandB), kT0 + 100).accepted());
    f.io.sent.clear();
    f.session.poll(kT0 + 10000);
    REQUIRE(f.session.failure() == SessionFailure::kPeerHandshakeFailed);
    const auto closes = f.io.of_type(PacketType::kClose);
    REQUIRE(closes.size() == 1);
    REQUIRE(closes[0].to == kCandB);
    REQUIRE(tunnel::decode_close(closes[0].payload).reason == tunnel::CloseReason::kSetupFailed);
    REQUIRE(f.io.of_type(PacketType::kHello).empty());
}

// ---------------------------------------------------------------- 5.6 CLOSE

TEST_CASE("session: user close of CONNECTED sends one CLOSE 0x00 to the learned endpoint",
          "[peer][session]") {
    Fixture f;
    f.connect();
    f.io.sent.clear();
    f.session.close_by_user(kT0 + 100);
    REQUIRE(f.session.state() == SessionState::kClosed);
    REQUIRE(f.io.sent.size() == 1);
    REQUIRE(f.io.sent[0].to == kCandA);
    REQUIRE(tunnel::decode_close(f.io.sent[0].payload).reason == tunnel::CloseReason::kNormal);
    REQUIRE_FALSE(f.session.next_deadline().has_value());
    f.session.close_by_user(kT0 + 200);
    REQUIRE(f.io.sent.size() == 1);
}

TEST_CASE("session: user close while establishing sends 0x01 only when learned",
          "[peer][session]") {
    {
        Fixture f;
        f.session.start_punch(kT0);
        REQUIRE(f.rx(hello(kPeerEpoch, 0), kT0).accepted());
        f.io.sent.clear();
        f.session.close_by_user(kT0 + 1);
        REQUIRE(f.io.sent.size() == 1);
        REQUIRE(tunnel::decode_close(f.io.sent[0].payload).reason ==
                tunnel::CloseReason::kUserCancel);
        REQUIRE(f.session.state() == SessionState::kClosed);
    }
    {
        // 학습 전이면 한 개도 나가지 않는다. 후보 전체로 뿌리는 구현이 여기서 걸린다.
        Fixture f;
        f.session.start_punch(kT0);
        f.io.sent.clear();
        f.session.close_by_user(kT0 + 1);
        REQUIRE(f.io.sent.empty());
        REQUIRE(f.session.state() == SessionState::kClosed);
    }
}

TEST_CASE("session: user close goes CLOSED even if sending fails", "[peer][session]") {
    Fixture f;
    f.connect();
    f.io.send_ok = false;
    f.session.close_by_user(kT0);
    REQUIRE(f.session.state() == SessionState::kClosed);
}

TEST_CASE("session: user close of FAILED does nothing", "[peer][session]") {
    Fixture f;
    f.session.start_punch(kT0);
    f.session.poll(kT0 + 10000);
    const auto before = f.io.sent.size();
    f.session.close_by_user(kT0 + 10001);
    REQUIRE(f.session.state() == SessionState::kFailed);
    REQUIRE(f.io.sent.size() == before);
}

TEST_CASE("session: received CLOSE moves to CLOSED and records the reason", "[peer][session]") {
    for (const std::uint8_t reason : std::vector<std::uint8_t>{0x00, 0x01, 0x02, 0x03}) {
        Fixture f;
        f.session.start_punch(kT0);
        REQUIRE(f.rx(hello(kPeerEpoch, 0), kT0).accepted());
        REQUIRE(f.rx(close_rx(kPeerEpoch, 1, reason), kT0).accepted());
        REQUIRE(f.session.state() == SessionState::kClosed);
        REQUIRE(f.session.peer_close_reason() == static_cast<tunnel::CloseReason>(reason));
        REQUIRE_FALSE(f.session.next_deadline().has_value());
        REQUIRE(f.session.terminal_since() == kT0);
    }
}

TEST_CASE("session: a terminal session drops everything as drop_terminal_state",
          "[peer][session]") {
    Fixture f;
    f.connect();
    f.session.close_by_user(kT0);
    f.io.sent.clear();
    REQUIRE(f.rx(keepalive(kPeerEpoch, 9), kT0).dropped == Counter::DropTerminalState);
    REQUIRE(f.rx(hello(kPeerEpoch2, 0), kT0).dropped == Counter::DropTerminalState);
    REQUIRE(f.io.sent.empty());
}

// ---------------------------------------------------------------- 9.5 재협상

TEST_CASE("session: renegotiation from CONNECTED follows the ten steps", "[peer][session]") {
    Fixture f;
    f.connect();
    f.session.poll(kT0 + 5000);  // ping 하나를 걸어 둔다
    REQUIRE(f.session.pending_pings() == 1);
    REQUIRE(f.rx(keepalive(kPeerEpoch, 500), kT0 + 6000).accepted());  // 창을 멀리 민다
    f.io.sent.clear();

    const Millis t = kT0 + 7000;
    REQUIRE(f.rx(hello(kPeerEpoch2, 0, kOutside), t).accepted());
    REQUIRE(f.session.renegotiations() == 1);
    // 1. 상대의 이전 epoch 를 폐기 목록에
    REQUIRE(f.io.retired.contains(kPeerEpoch, t));
    REQUIRE_FALSE(f.io.retired.contains(kLocalEpoch, t));
    // 4. 새 epoch 고정
    REQUIRE(f.session.pinned_peer_epoch() == kPeerEpoch2);
    // 3. pending_pings 초기화
    REQUIRE(f.session.pending_pings() == 0);
    // 6. 그 데이터그램의 출발지로 HELLO_ACK, peer_endpoint 는 그대로
    const auto acks = f.io.of_type(PacketType::kHelloAck);
    REQUIRE(acks.size() == 1);
    REQUIRE(acks[0].to == kOutside);
    REQUIRE(f.session.peer_endpoint() == kCandA);
    // 7. sent_ack 만 선 HANDSHAKING
    REQUIRE(f.session.sent_ack());
    REQUIRE_FALSE(f.session.got_ack());
    REQUIRE(f.session.state() == SessionState::kHandshaking);
    REQUIRE(f.session.punch_deadline() == t + 10000);
    // 9, 10. 후보 전체로 HELLO 재전송을 다시 시작한다. 유발한 출발지는 재전송 대상이 아니라 잠정
    // 경로다. 그 주소로는 probe nonce 를 담은 검증 HELLO 하나만 나간다
    const auto hellos = f.io.of_type(PacketType::kHello);
    REQUIRE(hellos.size() == 3);
    REQUIRE(hellos[0].to == kCandA);
    REQUIRE(hellos[1].to == kCandB);
    REQUIRE(hellos[2].to == kOutside);
    REQUIRE(tunnel::decode_hello(hellos[2].payload).value.nonce != wire_nonce(kLocalNonce));
    REQUIRE(f.session.probe_paths() == 1);
    REQUIRE(f.session.hello_due() == t + 200);
    f.io.sent.clear();
    f.session.poll(t + 200);
    for (const auto& h : f.io.of_type(PacketType::kHello)) {
        REQUIRE(h.to != kOutside);
    }
    // 10. KEEPALIVE, PING, 유휴를 멈춘다
    REQUIRE_FALSE(f.session.keepalive_due().has_value());
    REQUIRE_FALSE(f.session.ping_due().has_value());
    REQUIRE_FALSE(f.session.idle_deadline().has_value());
    // 5. 우리 epoch 와 nonce 는 그대로
    for (const auto& p : f.io.sent) {
        REQUIRE(p.header.session_epoch == kLocalEpoch);
    }
    REQUIRE(tunnel::decode_hello(hellos[0].payload).value.nonce == wire_nonce(kLocalNonce));
    REQUIRE(f.session.local_epoch() == kLocalEpoch);
}

TEST_CASE("session: renegotiation resets the window so seq 0 of the new epoch is accepted",
          "[peer][session]") {
    Fixture f;
    f.connect();
    REQUIRE(f.rx(keepalive(kPeerEpoch, 500), kT0).accepted());
    REQUIRE(f.rx(hello(kPeerEpoch2, 0), kT0 + 2000).accepted());
    REQUIRE(f.rx(keepalive(kPeerEpoch2, 1), kT0 + 2001).accepted());
    REQUIRE(f.rx(keepalive(kPeerEpoch2, 1), kT0 + 2002).dropped == Counter::DropDuplicate);
}

TEST_CASE("session: renegotiation with a failed HELLO_ACK lands in PUNCHING", "[peer][session]") {
    Fixture f;
    f.connect();
    f.io.send_ok = false;
    REQUIRE(f.rx(hello(kPeerEpoch2, 0), kT0 + 2000).accepted());
    REQUIRE(f.session.state() == SessionState::kPunching);
    REQUIRE_FALSE(f.session.sent_ack());
    REQUIRE(f.count(Counter::TxErrAck) == 1);
}

TEST_CASE("session: after renegotiation the same new epoch is a plain HELLO", "[peer][session]") {
    Fixture f;
    f.connect();
    REQUIRE(f.rx(hello(kPeerEpoch2, 0), kT0 + 2000).accepted());
    for (std::uint32_t i = 1; i <= 5; ++i) {
        REQUIRE(f.rx(hello(kPeerEpoch2, i), kT0 + 2000 + i * 200).accepted());
    }
    REQUIRE(f.session.renegotiations() == 1);
    REQUIRE(f.rx(hello_ack(kPeerEpoch2, 6), kT0 + 3300).accepted());
    REQUIRE(f.session.state() == SessionState::kConnected);
}

TEST_CASE("session: renegotiations within 1s are drop_reneg_rate and do not move the basis",
          "[peer][session]") {
    Fixture f;
    f.connect();
    const Millis t = kT0 + 5000;
    REQUIRE(f.rx(hello(kPeerEpoch2, 0), t).accepted());
    REQUIRE(f.rx(hello(0xCC000001u, 0), t + 500).dropped == Counter::DropRenegRate);
    REQUIRE(f.rx(hello(0xCC000002u, 0), t + 999).dropped == Counter::DropRenegRate);
    // 기준은 수용한 재협상의 시각이다. 폐기한 것이 갱신했다면 여기서도 걸린다.
    REQUIRE(f.rx(hello(0xCC000003u, 0), t + 1000).accepted());
    REQUIRE(f.session.renegotiations() == 2);
    REQUIRE(f.session.pinned_peer_epoch() == 0xCC000003u);
}

TEST_CASE("session: the ninth renegotiation is drop_reneg_limit", "[peer][session]") {
    Fixture f;
    f.connect();
    Millis t = kT0 + 5000;
    for (std::uint32_t i = 0; i < 8; ++i) {
        REQUIRE(f.rx(hello(0xD0000000u + i, 0), t).accepted());
        t += 1000;
    }
    REQUIRE(f.session.renegotiations() == 8);
    REQUIRE(f.rx(hello(0xD0000100u, 0), t).dropped == Counter::DropRenegLimit);
    REQUIRE(f.rx(hello(0xD0000101u, 0), t + 5000).dropped == Counter::DropRenegLimit);
    REQUIRE(f.session.renegotiations() == 8);
    REQUIRE(f.session.pinned_peer_epoch() == 0xD0000007u);
}

TEST_CASE("session: renegotiation stops the idle timer so it cannot fire during the new punch",
          "[peer][session]") {
    // roadmap.md Phase 4 "재협상 뒤 타이머" 셋째. 유휴를 45초까지 올린 뒤 재협상한다.
    Fixture f;
    f.connect();
    for (Millis t = kT0; t <= kT0 + 45000; t += 1000) {
        f.session.poll(t);
    }
    REQUIRE(f.rx(hello(kPeerEpoch2, 0), kT0 + 45000).accepted());
    const auto keepalives = f.io.of_type(PacketType::kKeepalive).size();
    const auto pings = f.io.of_type(PacketType::kPing).size();
    for (Millis t = kT0 + 45000; t < kT0 + 55000; t += 100) {
        f.session.poll(t);
        REQUIRE(f.session.state() == SessionState::kHandshaking);
    }
    REQUIRE(f.io.of_type(PacketType::kKeepalive).size() == keepalives);
    REQUIRE(f.io.of_type(PacketType::kPing).size() == pings);
    // 재전송은 200ms 간격으로 다시 나간다 (첫 바퀴 + 49 바퀴, 후보 둘)
    REQUIRE(f.io.of_type(PacketType::kHello).size() >= 2 * 50);
    f.session.poll(kT0 + 55000);
    REQUIRE(f.session.failure() == SessionFailure::kPeerHandshakeFailed);
}

TEST_CASE("session: renegotiation before punch start leaves both punch timers unarmed",
          "[peer][session]") {
    Fixture f;
    REQUIRE(f.rx(hello(kPeerEpoch, 0), kT0).accepted());
    REQUIRE(f.rx(hello(kPeerEpoch2, 0), kT0 + 100).accepted());
    REQUIRE(f.session.renegotiations() == 1);
    REQUIRE(f.session.state() == SessionState::kHandshaking);  // 9.5 7번: 플래그가 정한다 (6번 성공)
    REQUIRE_FALSE(f.session.hello_due().has_value());
    REQUIRE_FALSE(f.session.punch_deadline().has_value());
    REQUIRE(f.io.of_type(PacketType::kHello).empty());
    REQUIRE(f.session.start_punch(kT0 + 500));
    REQUIRE(f.session.punch_deadline() == kT0 + 500 + 10000);
}

TEST_CASE("session: a late packet of the old epoch after renegotiation is stale here",
          "[peer][session]") {
    // 세션 안에서는 drop_stale_epoch 다. 파이프라인에서는 8.1 검사 8번이 먼저 걸러
    // drop_retired_epoch 가 된다 (tunnel_test).
    Fixture f;
    f.connect();
    REQUIRE(f.rx(hello(kPeerEpoch2, 0), kT0 + 2000).accepted());
    REQUIRE(f.rx(keepalive(kPeerEpoch, 50), kT0 + 2001).dropped == Counter::DropStaleEpoch);
}

// ---------------------------------------------------------------- 10.4 엔드포인트 학습 (후보 집합 안)

TEST_CASE("session: learning needs an advancing packet from a candidate", "[peer][session]") {
    Fixture f;
    testing::LogCapture logs;
    f.session.start_punch(kT0);
    REQUIRE(f.rx(hello(kPeerEpoch, 10, kOutside), kT0).accepted());
    REQUIRE_FALSE(f.session.peer_endpoint().has_value());  // (c) 후보 밖
    REQUIRE(logs.with_event("session.endpoint").empty());
    REQUIRE(f.rx(keepalive(kPeerEpoch, 11, kCandB), kT0).accepted());
    REQUIRE(f.session.peer_endpoint() == kCandB);
    REQUIRE(f.count(Counter::EndpointLearned) == 1);
    // architecture.md 9장 session.endpoint. 바뀔 때마다 한 줄, 학습한 주소를 싣는다
    REQUIRE(logs.with_event("session.endpoint").size() == 1);
    REQUIRE(logs.with_event("session.endpoint")[0].find(" endpoint=5.6.7.8:6000") != std::string::npos);
    // (b) 재정렬 수용은 학습하지 않는다
    REQUIRE(f.rx(keepalive(kPeerEpoch, 5, kCandA), kT0).accepted());
    REQUIRE(f.session.peer_endpoint() == kCandB);
    // 가장 최근의 전진 패킷
    REQUIRE(f.rx(keepalive(kPeerEpoch, 12, kCandA), kT0).accepted());
    REQUIRE(f.session.peer_endpoint() == kCandA);
    REQUIRE(f.count(Counter::EndpointLearned) == 2);
    // 같은 값이면 세지 않는다
    REQUIRE(f.rx(keepalive(kPeerEpoch, 13, kCandA), kT0).accepted());
    REQUIRE(f.count(Counter::EndpointLearned) == 2);
    REQUIRE(logs.with_event("session.endpoint").size() == 2);
}

TEST_CASE("session: a dropped packet does not teach the endpoint", "[peer][session]") {
    Fixture f;
    f.connect();
    REQUIRE(f.rx(pong(kPeerEpoch, 2, 77, kCandB), kT0).dropped == Counter::DropUnmatchedPong);
    REQUIRE(f.session.peer_endpoint() == kCandA);
}

TEST_CASE("session: without a learned endpoint KEEPALIVE and PING go to every candidate",
          "[peer][session]") {
    Fixture f;
    f.session.start_punch(kT0);
    REQUIRE(f.rx(hello(kPeerEpoch, 0, kOutside), kT0).accepted());
    REQUIRE(f.rx(hello_ack(kPeerEpoch, 1, kOutside), kT0).accepted());
    REQUIRE(f.session.state() == SessionState::kConnected);
    REQUIRE_FALSE(f.session.peer_endpoint().has_value());
    const auto ka = f.io.of_type(PacketType::kKeepalive);
    REQUIRE(ka.size() == 2);
    REQUIRE(ka[0].to == kCandA);
    REQUIRE(ka[1].to == kCandB);
    f.session.poll(kT0 + 5000);
    const auto pings = f.io.of_type(PacketType::kPing);
    REQUIRE(pings.size() == 2);
    REQUIRE(f.session.pending_pings() == 1);
    // 학습 전이므로 사용자 종료는 CLOSE 를 보내지 않는다
    f.io.sent.clear();
    f.session.close_by_user(kT0 + 5001);
    REQUIRE(f.io.sent.empty());
}

TEST_CASE("session: DATA goes to every candidate before learning and stops at a failed send",
          "[peer][session]") {
    Fixture f;
    f.session.start_punch(kT0);
    REQUIRE(f.rx(hello(kPeerEpoch, 0, kOutside), kT0).accepted());
    REQUIRE(f.rx(hello_ack(kPeerEpoch, 1, kOutside), kT0).accepted());
    REQUIRE(f.session.state() == SessionState::kConnected);
    REQUIRE_FALSE(f.session.peer_endpoint().has_value());
    const auto inner = inner_ipv4(kLocalVip, kPeerVip);
    f.io.sent.clear();
    REQUIRE(f.session.send_data(inner));
    REQUIRE(f.io.sent.size() == 2);
    REQUIRE(f.io.sent[0].to == kCandA);
    REQUIRE(f.io.sent[1].to == kCandB);
    REQUIRE(f.io.sent[0].payload == inner);
    f.io.sent.clear();
    f.io.send_ok = false;
    REQUIRE_FALSE(f.session.send_data(inner));
    REQUIRE(f.io.sent.size() == 1);  // 첫 후보에서 멈췄다
    REQUIRE(f.count(Counter::TxErrSend) == 1);
}


// ---------------------------------------------------------------- 5.7 ROSTER 수신

TEST_CASE("session: ROSTER records the generation and rejects stale ones", "[peer][session]") {
    Fixture f;
    f.connect();
    tunnel::Roster roster;
    roster.generation = 5;
    roster.count = 1;
    roster.members[0].peer_id = kPeerId;
    roster.members[0].virtual_ip = kPeerVip;
    roster.members[0].name_len = 4;
    roster.members[0].name = {'h', 'o', 's', 't'};
    roster.members[0].connected = true;
    std::vector<std::byte> buf(200);
    buf.resize(tunnel::encode_roster(roster, buf));
    sangtachi::testing::LogCapture logs;
    REQUIRE(f.rx(make_rx(PacketType::kRoster, buf, kPeerEpoch, 2), kT0).accepted());
    REQUIRE(f.session.last_roster_generation() == 5u);
    REQUIRE(f.rx(make_rx(PacketType::kRoster, buf, kPeerEpoch, 3), kT0).dropped ==
            Counter::DropRosterStale);
    // 받아들인 명부마다 roster 줄 하나 (5.7, architecture.md 9장). 버린 것은 내지 않는다
    auto lines = logs.with_event("roster");
    REQUIRE(lines.size() == 1);
    REQUIRE(lines[0].find(" generation=5 ") != std::string::npos);
    REQUIRE(lines[0].find(" members=" + std::to_string(kPeerId) + "/10.100.0.1/1/host") !=
            std::string::npos);
    // 더 큰 값은 다시 받아들이고 다시 낸다. 이름이 어긋난 것은 버리고 내지 않는다
    roster.generation = 6;
    buf.assign(200, std::byte{0});
    buf.resize(tunnel::encode_roster(roster, buf));
    REQUIRE(f.rx(make_rx(PacketType::kRoster, buf, kPeerEpoch, 4), kT0).accepted());
    auto bad = buf;
    bad[3] = std::byte{7};    // generation 7
    bad[5 + 10] = std::byte{' '};  // 이름 첫 바이트를 공백으로
    REQUIRE(f.rx(make_rx(PacketType::kRoster, bad, kPeerEpoch, 5), kT0).dropped ==
            Counter::DropRosterName);
    lines = logs.with_event("roster");
    REQUIRE(lines.size() == 2);
    REQUIRE(lines[1].find(" generation=6 ") != std::string::npos);
}

TEST_CASE("session: ROSTER goes to every candidate before learning", "[peer][session]") {
    Fixture f;
    f.session.start_punch(kT0);
    REQUIRE(f.rx(hello(kPeerEpoch, 0, kOutside), kT0).accepted());
    REQUIRE(f.rx(hello_ack(kPeerEpoch, 1, kOutside), kT0).accepted());
    REQUIRE_FALSE(f.session.peer_endpoint().has_value());
    f.io.sent.clear();
    tunnel::Roster roster;
    roster.count = 1;
    roster.members[0].name_len = 1;
    roster.members[0].name = {'h'};
    std::vector<std::byte> payload(200);
    payload.resize(tunnel::encode_roster(roster, payload));
    f.session.send_roster(payload);
    REQUIRE(f.io.sent.size() == 2);
    REQUIRE(f.io.sent[0].to == kCandA);
    REQUIRE(f.io.sent[1].to == kCandB);
    REQUIRE(f.io.sent[0].type() == PacketType::kRoster);
    REQUIRE(f.io.sent[0].payload == payload);
    REQUIRE(f.io.sent[0].header.sequence != f.io.sent[1].header.sequence);
}

TEST_CASE("session: ROSTER from a non-host peer is drop_roster_direction", "[peer][session]") {
    FakeIo io;
    auto config = session_config();
    config.peer_is_host = false;
    Session s(config, io.io(), kT0);
    REQUIRE(s.receive(hello(kPeerEpoch, 0).view(), kT0).accepted());
    tunnel::Roster roster;
    roster.generation = 1;
    roster.count = 1;
    roster.members[0].name_len = 1;
    roster.members[0].name = {'a'};
    std::vector<std::byte> buf(200);
    buf.resize(tunnel::encode_roster(roster, buf));
    REQUIRE(s.receive(make_rx(PacketType::kRoster, buf, kPeerEpoch, 1).view(), kT0).dropped ==
            Counter::DropRosterDirection);
}

// ---------------------------------------------------------------- 4.4 송신 sequence

TEST_CASE("session: every sent packet takes the next sequence", "[peer][session]") {
    Fixture f;
    f.connect();
    f.session.poll(kT0 + 5000);
    f.session.close_by_user(kT0 + 5001);
    for (std::size_t i = 0; i < f.io.sent.size(); ++i) {
        REQUIRE(f.io.sent[i].header.sequence == i);
    }
    REQUIRE(f.session.next_sequence() == f.io.sent.size());
}

// ---------------------------------------------------------------- 회신은 받은 데이터그램의 출발지로 (9.4)

TEST_CASE("session: replies go to the datagram source even when an endpoint is learned",
          "[peer][session]") {
    Fixture f;
    f.connect();
    REQUIRE(f.session.peer_endpoint() == kCandA);
    f.io.sent.clear();
    REQUIRE(f.rx(hello(kPeerEpoch, 2, kOutside), kT0 + 10).accepted());
    REQUIRE(f.rx(ping(kPeerEpoch, 3, 5, kOutside), kT0 + 10).accepted());
    // 회신 둘과, HELLO 가 올린 잠정 경로의 검증 HELLO 하나. PING 은 같은 주소라 다시 올리지 않는다
    REQUIRE(f.io.sent.size() == 3);
    REQUIRE(f.io.sent[0].type() == PacketType::kHelloAck);
    REQUIRE(f.io.sent[0].to == kOutside);
    REQUIRE(f.io.sent[1].type() == PacketType::kHello);
    REQUIRE(f.io.sent[1].to == kOutside);
    REQUIRE(f.io.sent[2].type() == PacketType::kPong);
    REQUIRE(f.io.sent[2].to == kOutside);
    REQUIRE(f.session.peer_endpoint() == kCandA);  // 회신은 전환이 아니다
}

// ---------------------------------------------------------------- 10.4 (c) 경로 검증과 9.4 공통 예산

namespace {

constexpr network::Endpoint outside_port(std::uint16_t port) {
    return network::Endpoint{0x09090909u, port};
}

// sent 안에서 to 로 간 검증 HELLO 들 (시도 nonce 가 아닌 HELLO).
std::vector<SentPacket> probes_to(const FakeIo& io, const network::Endpoint& to) {
    std::vector<SentPacket> out;
    for (const auto& p : io.of_type(PacketType::kHello)) {
        if (p.to == to && tunnel::decode_hello(p.payload).value.nonce != wire_nonce(kLocalNonce)) {
            out.push_back(p);
        }
    }
    return out;
}

std::size_t count_to(const FakeIo& io, PacketType type, const network::Endpoint& to) {
    std::size_t n = 0;
    for (const auto& p : io.of_type(type)) {
        n += p.to == to ? 1 : 0;
    }
    return n;
}

// 그 검증 HELLO 의 probe nonce 를 echo 하는 HELLO_ACK.
Rx probe_echo(const SentPacket& probe, std::uint32_t epoch, std::uint32_t seq,
              network::Endpoint from) {
    return hello_ack(epoch, seq, from, kPeerVip,
                     peer_nonce(tunnel::decode_hello(probe.payload).value.nonce));
}

}  // namespace

TEST_CASE("session: an advancing packet from outside registers one probe path and one HELLO",
          "[peer][session][probe]") {
    Fixture f;
    f.connect();
    f.io.sent.clear();
    REQUIRE(f.rx(keepalive(kPeerEpoch, 2, kOutside), kT0 + 10).accepted());
    REQUIRE(f.session.peer_endpoint() == kCandA);  // 3. peer_endpoint 는 그대로
    REQUIRE(f.session.idle_deadline() == kT0 + 10 + 50000);  // 원 패킷은 그대로 처리된다
    REQUIRE(f.session.probe_paths() == 1);
    REQUIRE(f.io.sent.size() == 1);
    const auto& probe = f.io.sent[0];
    REQUIRE(probe.type() == PacketType::kHello);
    REQUIRE(probe.to == kOutside);
    REQUIRE(probe.header.session_epoch == kLocalEpoch);
    const auto decoded = tunnel::decode_hello(probe.payload);
    REQUIRE(decoded.value.nonce != wire_nonce(kLocalNonce));  // 시도 nonce 가 아니다 (5.1)
    REQUIRE(decoded.value.virtual_ip == kLocalVip);
    // 같은 주소는 다시 올리지 않는다
    REQUIRE(f.rx(keepalive(kPeerEpoch, 3, kOutside), kT0 + 20).accepted());
    REQUIRE(f.rx(keepalive(kPeerEpoch, 10, outside_port(7001)), kT0 + 30).accepted());
    // (b) 를 못 한 재정렬 수용 패킷은 올리지 않는다
    REQUIRE(f.rx(keepalive(kPeerEpoch, 8, outside_port(7002)), kT0 + 40).accepted());
    REQUIRE(f.session.probe_paths() == 2);
    REQUIRE(f.io.sent.size() == 2);
}

TEST_CASE("session: a probe echo switches the endpoint and leaves the handshake alone",
          "[peer][session][probe]") {
    SECTION("CONNECTED") {
        Fixture f;
        f.connect();
        REQUIRE(f.rx(keepalive(kPeerEpoch, 2, kOutside), kT0 + 10).accepted());
        const auto probe = probes_to(f.io, kOutside).at(0);
        REQUIRE(f.rx(probe_echo(probe, kPeerEpoch, 3, kOutside), kT0 + 20).accepted());
        REQUIRE(f.session.peer_endpoint() == kOutside);
        REQUIRE(f.count(Counter::EndpointLearned) == 2);
        REQUIRE(f.count(Counter::DupAck) == 0);  // 시도 nonce 의 중복 ACK 이 아니다
        REQUIRE(f.session.state() == SessionState::kConnected);
        REQUIRE(f.session.idle_deadline() == kT0 + 20 + 50000);
        // 전환 뒤에는 검증된 주소다. 그 주소의 회신은 토큰을 쓰지 않고 다시 올리지 않는다
        f.io.sent.clear();
        REQUIRE(f.rx(ping(kPeerEpoch, 4, 9, kOutside), kT0 + 30).accepted());
        REQUIRE(f.io.sent.size() == 1);
        REQUIRE(f.io.sent[0].type() == PacketType::kPong);
    }
    SECTION("PUNCHING: got_ack and the epoch stay as they were") {
        Fixture f;
        f.session.start_punch(kT0);
        REQUIRE(f.rx(hello(kPeerEpoch, 0, kOutside), kT0).accepted());
        REQUIRE(f.session.sent_ack());
        const auto probe = probes_to(f.io, kOutside).at(0);
        REQUIRE(f.rx(probe_echo(probe, kPeerEpoch, 1, kOutside), kT0 + 5).accepted());
        REQUIRE(f.session.peer_endpoint() == kOutside);
        REQUIRE_FALSE(f.session.got_ack());
        REQUIRE(f.session.state() == SessionState::kHandshaking);
        REQUIRE(f.session.pinned_peer_epoch() == kPeerEpoch);
    }
}

TEST_CASE("session: a probe echo must come from the probed address under the pinned epoch",
          "[peer][session][probe]") {
    Fixture f;
    f.connect();
    REQUIRE(f.rx(keepalive(kPeerEpoch, 2, kOutside), kT0 + 10).accepted());
    const auto probe = probes_to(f.io, kOutside).at(0);
    // 다른 주소의 echo 는 probe 행이 아니고 시도 nonce 도 아니다
    REQUIRE(f.rx(probe_echo(probe, kPeerEpoch, 3, kCandB), kT0 + 20).dropped ==
            Counter::DropBadNonce);
    // 다른 epoch 의 echo 는 drop_stale_epoch (8.2)
    REQUIRE(f.rx(probe_echo(probe, kPeerEpoch2, 0, kOutside), kT0 + 20).dropped ==
            Counter::DropStaleEpoch);
    REQUIRE(f.session.peer_endpoint() == kCandA);
    REQUIRE(f.session.pinned_peer_epoch() == kPeerEpoch);
}

TEST_CASE("session: a reordered probe echo validates without switching", "[peer][session][probe]") {
    Fixture f;
    f.connect();
    REQUIRE(f.rx(keepalive(kPeerEpoch, 10, kOutside), kT0 + 10).accepted());
    const auto probe = probes_to(f.io, kOutside).at(0);
    REQUIRE(f.rx(probe_echo(probe, kPeerEpoch, 5, kOutside), kT0 + 20).accepted());
    REQUIRE(f.session.peer_endpoint() == kCandA);  // (b) 를 못 했다
    // 자리가 남아 있는 동안 같은 nonce 의 전진 echo 는 전환한다
    REQUIRE(f.rx(probe_echo(probe, kPeerEpoch, 11, kOutside), kT0 + 30).accepted());
    REQUIRE(f.session.peer_endpoint() == kOutside);
    f.session.poll(kT0 + 10 + 5000);
    REQUIRE(f.session.probe_paths() == 0);
    REQUIRE(f.count(Counter::PathProbeFailed) == 0);  // echo 는 왔다
}

TEST_CASE("session: a probe slot lasts exactly 5s whatever the outcome", "[peer][session][probe]") {
    const Millis t = kT0 + 10;
    SECTION("unanswered: path_probe_failed, and a late echo is a bad nonce") {
        sangtachi::testing::LogCapture logs;
        Fixture f;  // 펀치 전이라 세션 타이머가 없다. 마감은 자리의 만료 하나다
        REQUIRE(f.rx(hello(kPeerEpoch, 0, kOutside), t).accepted());
        const auto probe = probes_to(f.io, kOutside).at(0);
        REQUIRE(f.session.next_deadline() == t + 5000);
        f.session.poll(t + 4999);
        REQUIRE(f.session.probe_paths() == 1);
        REQUIRE(f.rx(probe_echo(probe, kPeerEpoch, 3, kOutside), t + 5000).dropped ==
                Counter::DropBadNonce);
        REQUIRE(f.session.probe_paths() == 0);
        REQUIRE(f.count(Counter::PathProbeFailed) == 1);
        f.session.poll(t + 5000);
        REQUIRE(logs.with_event("timer.tick").empty());  // 항목 수명이지 타이머가 아니다 (11장)
        // 자리가 비었으니 같은 주소가 다시 올라간다
        REQUIRE(f.rx(keepalive(kPeerEpoch, 4, kOutside), t + 5001).accepted());
        REQUIRE(probes_to(f.io, kOutside).size() == 2);
    }
    SECTION("answered: the slot stays until 5s and is not a failure") {
        Fixture f;
        f.connect();
        REQUIRE(f.rx(keepalive(kPeerEpoch, 2, kOutside), t).accepted());
        const auto probe = probes_to(f.io, kOutside).at(0);
        REQUIRE(f.rx(probe_echo(probe, kPeerEpoch, 3, kOutside), t + 1).accepted());
        f.session.poll(t + 4999);
        REQUIRE(f.session.probe_paths() == 1);
        f.session.poll(t + 5000);
        REQUIRE(f.session.probe_paths() == 0);
        REQUIRE(f.count(Counter::PathProbeFailed) == 0);
    }
}

TEST_CASE("session: four probe paths at most, the fifth packet is still processed",
          "[peer][session][probe]") {
    Fixture f(/*sink=*/true);
    f.connect();
    f.io.sent.clear();
    for (std::uint16_t i = 0; i < 4; ++i) {
        REQUIRE(f.rx(keepalive(kPeerEpoch, 2 + i, outside_port(7100 + i)), kT0 + 10).accepted());
    }
    REQUIRE(f.session.probe_paths() == 4);
    const auto fifth = outside_port(7104);
    REQUIRE(f.rx(data_rx(kPeerEpoch, 6, inner_ipv4(kPeerVip, kLocalVip), fifth), kT0 + 20)
                .accepted());
    REQUIRE(f.count(Counter::DropProbeFull) == 1);
    REQUIRE(f.io.sunk.size() == 1);                           // DATA 는 주입됐다
    REQUIRE(f.session.idle_deadline() == kT0 + 20 + 50000);   // 유휴 타이머도 갱신됐다
    REQUIRE(f.session.probe_paths() == 4);
    REQUIRE(probes_to(f.io, fifth).empty());
    // 가장 오래된 경로가 밀려나지 않았다. 검증 HELLO 는 경로마다 하나다
    for (std::uint16_t i = 0; i < 4; ++i) {
        REQUIRE(probes_to(f.io, outside_port(7100 + i)).size() == 1);
    }
    // 가장 오래된 칸이 표에 그대로 있다. 그 echo 가 받아진다
    REQUIRE(f.rx(probe_echo(probes_to(f.io, outside_port(7100)).at(0), kPeerEpoch, 7,
                            outside_port(7100)),
                 kT0 + 20)
                .accepted());
    REQUIRE(f.session.peer_endpoint() == outside_port(7100));
    // 등록하지 못한 다섯째는 토큰을 쓰지 않았다. 남은 토큰은 10 - 4 = 6. 7100 은 이제
    // peer_endpoint 라 무료이므로 아직 잠정 경로인 7101 로 센다
    for (std::uint32_t i = 0; i < 7; ++i) {
        REQUIRE(f.rx(hello(kPeerEpoch, 8 + i, outside_port(7101)), kT0 + 20).accepted());
    }
    REQUIRE(count_to(f.io, PacketType::kHelloAck, outside_port(7101)) == 6);
    REQUIRE(f.count(Counter::DropUnverifiedTx) == 1);
}

TEST_CASE("session: the unverified budget counts tokens as the roadmap does",
          "[peer][session][probe]") {
    const Millis t = kT0 + 10;
    SECTION("a new outside source: the first HELLO costs two, the tenth is blocked") {
        Fixture f;
        f.connect();
        f.io.sent.clear();
        for (std::uint32_t i = 0; i < 10; ++i) {
            REQUIRE(f.rx(hello(kPeerEpoch, 2 + i, kOutside), t).accepted());
        }
        REQUIRE(count_to(f.io, PacketType::kHelloAck, kOutside) == 9);
        REQUIRE(probes_to(f.io, kOutside).size() == 1);
        REQUIRE(f.count(Counter::DropUnverifiedTx) == 1);
        REQUIRE(f.count(Counter::TxErrAck) == 0);  // 보내지 않은 것은 송신 실패가 아니다
        // 200ms 에 토큰 하나
        REQUIRE(f.rx(hello(kPeerEpoch, 12, kOutside), t + 199).accepted());
        REQUIRE(f.count(Counter::DropUnverifiedTx) == 2);
        REQUIRE(f.rx(hello(kPeerEpoch, 13, kOutside), t + 200).accepted());
        REQUIRE(count_to(f.io, PacketType::kHelloAck, kOutside) == 10);
    }
    SECTION("with one token left the HELLO_ACK takes it before the new probe path") {
        Fixture f;
        f.connect();
        for (std::uint32_t i = 0; i < 8; ++i) {  // 검증 HELLO 하나와 HELLO_ACK 여덟
            REQUIRE(f.rx(hello(kPeerEpoch, 2 + i, outside_port(7200)), t).accepted());
        }
        f.io.sent.clear();
        REQUIRE(f.rx(hello(kPeerEpoch, 10, kOutside), t).accepted());
        REQUIRE(count_to(f.io, PacketType::kHelloAck, kOutside) == 1);
        REQUIRE(probes_to(f.io, kOutside).empty());
        REQUIRE(f.session.probe_paths() == 1);  // 7200 하나뿐이다. 2번에서 멈췄다
        REQUIRE(f.count(Counter::DropUnverifiedTx) == 1);
    }
    SECTION("an existing probe path: the eleventh is blocked") {
        Fixture f;
        f.connect();
        REQUIRE(f.rx(keepalive(kPeerEpoch, 2, kOutside), t).accepted());  // 토큰 하나
        f.io.sent.clear();
        // 1초 뒤 다시 가득 찬다. 용량을 넘겨 쌓이지 않는다
        for (std::uint32_t i = 0; i < 11; ++i) {
            REQUIRE(f.rx(hello(kPeerEpoch, 3 + i, kOutside), t + 1000).accepted());
        }
        REQUIRE(count_to(f.io, PacketType::kHelloAck, kOutside) == 10);
        REQUIRE(f.count(Counter::DropUnverifiedTx) == 1);
    }
    SECTION("HELLO_ACK, PONG and validation HELLO share one bucket") {
        Fixture f;
        f.connect();
        f.io.sent.clear();
        for (std::uint32_t i = 0; i < 20; ++i) {
            const Rx rx = i % 2 == 0 ? hello(kPeerEpoch, 2 + i, kOutside)
                                     : ping(kPeerEpoch, 2 + i, i, kOutside);
            REQUIRE(f.rx(rx, t).accepted());
        }
        // 회신 20개와 검증 HELLO 하나 가운데 10개만 나간다
        REQUIRE(f.io.sent.size() == 10);
        REQUIRE(f.count(Counter::DropUnverifiedTx) == 11);
    }
    SECTION("candidates are not limited") {
        Fixture f;
        f.connect();
        f.io.sent.clear();
        for (std::uint32_t i = 0; i < 30; ++i) {
            REQUIRE(f.rx(hello(kPeerEpoch, 2 + i, kCandB), t).accepted());
        }
        REQUIRE(count_to(f.io, PacketType::kHelloAck, kCandB) == 30);
        REQUIRE(f.count(Counter::DropUnverifiedTx) == 0);
    }
}

TEST_CASE("session: a 200ms resend from outside loses nothing when the probe is answered",
          "[peer][session][probe]") {
    Fixture f;
    f.connect();
    f.io.sent.clear();
    std::uint32_t seq = 2;
    bool answered = false;
    for (int k = 0; k < 150; ++k) {  // 30초
        const Millis now = kT0 + 10 + static_cast<Millis>(k) * 200;
        REQUIRE(f.rx(hello(kPeerEpoch, seq++, kOutside), now).accepted());
        const auto probes = probes_to(f.io, kOutside);
        if (!answered && !probes.empty()) {
            REQUIRE(f.rx(probe_echo(probes.back(), kPeerEpoch, seq++, kOutside), now).accepted());
            answered = true;
        }
        f.session.poll(now);
    }
    REQUIRE(count_to(f.io, PacketType::kHelloAck, kOutside) == 150);
    REQUIRE(f.count(Counter::DropUnverifiedTx) == 0);
    // 전환한 주소는 검증된 주소다. 자리가 빈 뒤에도 다시 올리지 않는다
    REQUIRE(probes_to(f.io, kOutside).size() == 1);
}

TEST_CASE("session: a HELLO_ACK withheld by the budget does not raise sent_ack",
          "[peer][session][probe]") {
    Fixture f;
    f.connect();
    const Millis t = kT0 + 2000;
    for (std::uint32_t i = 0; i < 9; ++i) {  // 검증 HELLO 하나와 PONG 아홉으로 버킷을 비운다
        REQUIRE(f.rx(ping(kPeerEpoch, 2 + i, i, kOutside), t).accepted());
    }
    REQUIRE(f.count(Counter::DropUnverifiedTx) == 0);
    // 9.5 6번의 HELLO_ACK 도 같은 예산이다
    REQUIRE(f.rx(hello(kPeerEpoch2, 0, kOutside), t).accepted());
    REQUIRE(f.session.renegotiations() == 1);
    REQUIRE(f.count(Counter::DropUnverifiedTx) == 1);
    REQUIRE(f.count(Counter::TxErrAck) == 0);
    REQUIRE_FALSE(f.session.sent_ack());
    REQUIRE(f.session.state() == SessionState::kPunching);
}

TEST_CASE("session: without a probe nonce nothing is registered and no token is spent",
          "[peer][session][probe]") {
    Fixture f;
    f.connect();
    f.io.random_ok = false;
    f.io.sent.clear();
    for (std::uint32_t i = 0; i < 11; ++i) {
        REQUIRE(f.rx(hello(kPeerEpoch, 2 + i, kOutside), kT0 + 10).accepted());
    }
    REQUIRE(f.session.probe_paths() == 0);
    REQUIRE(probes_to(f.io, kOutside).empty());
    REQUIRE(count_to(f.io, PacketType::kHelloAck, kOutside) == 10);
    REQUIRE(f.count(Counter::DropUnverifiedTx) == 1);
}

TEST_CASE("session: outside packets register a path only when every check passed",
          "[peer][session][probe]") {
    SECTION("rejected packets register nothing") {
        Fixture f;
        f.connect();
        f.io.sent.clear();
        REQUIRE(f.rx(keepalive(kPeerEpoch, 1, kOutside), kT0 + 10).dropped == Counter::DropDuplicate);
        REQUIRE(f.rx(pong(kPeerEpoch, 2, 77, kOutside), kT0 + 10).dropped ==
                Counter::DropUnmatchedPong);
        REQUIRE(f.rx(data_rx(kPeerEpoch, 3, inner_ipv4(kLocalVip, kLocalVip), kOutside), kT0 + 10)
                    .dropped == Counter::DropInnerSrc);
        REQUIRE(f.session.probe_paths() == 0);
        REQUIRE(f.io.sent.empty());
    }
    SECTION("an outside CLOSE ends the session without a validation HELLO") {
        Fixture f;
        f.connect();
        f.io.sent.clear();
        REQUIRE(f.rx(close_rx(kPeerEpoch, 2, 0x00, kOutside), kT0 + 10).accepted());
        REQUIRE(f.session.state() == SessionState::kClosed);
        REQUIRE(f.io.sent.empty());
    }
    SECTION("an outside DATA without a payload hook still registers") {
        Fixture f;  // 훅이 없다
        f.connect();
        f.io.sent.clear();
        REQUIRE(f.rx(data_rx(kPeerEpoch, 2, inner_ipv4(kPeerVip, kLocalVip), kOutside), kT0 + 10)
                    .dropped == Counter::DropNoSink);
        REQUIRE(f.session.probe_paths() == 1);
        REQUIRE(probes_to(f.io, kOutside).size() == 1);
        REQUIRE(f.session.idle_deadline() == kT0 + 10 + 50000);
    }
}

TEST_CASE("session: a failed validation HELLO still spends its token and holds its slot",
          "[peer][session][probe]") {
    Fixture f;
    f.connect();
    f.io.send_ok = false;
    REQUIRE(f.rx(keepalive(kPeerEpoch, 2, kOutside), kT0 + 10).accepted());
    REQUIRE(f.count(Counter::TxErrSend) == 1);
    REQUIRE(f.count(Counter::TxErrAck) == 0);
    REQUIRE(f.session.probe_paths() == 1);
    f.io.send_ok = true;
    f.io.sent.clear();
    // 토큰 하나를 썼다. 남은 아홉 가운데 둘은 7300 의 첫 PONG 과 검증 HELLO 이고 PONG 은 여덟이다
    for (std::uint32_t i = 0; i < 10; ++i) {
        REQUIRE(f.rx(ping(kPeerEpoch, 3 + i, i, outside_port(7300)), kT0 + 10).accepted());
    }
    REQUIRE(count_to(f.io, PacketType::kPong, outside_port(7300)) == 8);
    f.io.sent.clear();
    f.session.poll(kT0 + 10 + 4999);
    REQUIRE(probes_to(f.io, kOutside).empty());  // 재전송하지 않는다
    f.session.poll(kT0 + 10 + 5000);
    REQUIRE(f.count(Counter::PathProbeFailed) == 2);  // kOutside 와 7300 둘 다 echo 가 없었다
}

TEST_CASE("session: renegotiation keeps probe paths and their nonces", "[peer][session][probe]") {
    Fixture f;
    f.connect();
    REQUIRE(f.rx(keepalive(kPeerEpoch, 2, kOutside), kT0 + 10).accepted());
    const auto probe = probes_to(f.io, kOutside).at(0);
    REQUIRE(f.rx(hello(kPeerEpoch2, 0, kCandB), kT0 + 1000).accepted());
    REQUIRE(f.session.renegotiations() == 1);
    REQUIRE(f.session.probe_paths() == 1);
    // 새로 고정한 epoch 아래의 echo 를 받는다. 자리는 처음 등록한 시각부터 5초다
    REQUIRE(f.rx(probe_echo(probe, kPeerEpoch2, 1, kOutside), kT0 + 1010).accepted());
    REQUIRE(f.session.peer_endpoint() == kOutside);
    f.session.poll(kT0 + 10 + 5000);
    REQUIRE(f.session.probe_paths() == 0);
}

TEST_CASE("session: a terminal session drops its probe paths and their deadline",
          "[peer][session][probe]") {
    Fixture f;
    f.connect();
    REQUIRE(f.rx(keepalive(kPeerEpoch, 2, kOutside), kT0 + 10).accepted());
    REQUIRE(f.session.probe_paths() == 1);
    f.session.close_by_user(kT0 + 20);
    REQUIRE(f.session.probe_paths() == 0);
    REQUIRE_FALSE(f.session.next_deadline().has_value());
    f.session.poll(kT0 + 10 + 5000);
    REQUIRE(f.count(Counter::PathProbeFailed) == 0);
}

// ---------------------------------------------------------------- 같은 바퀴의 마감과 주기 송신 (9.6 1번)

TEST_CASE("session: when the punch deadline and a HELLO round fall together no HELLO goes out",
          "[peer][session]") {
    Fixture f;
    f.session.start_punch(kT0);
    for (Millis t = kT0; t < kT0 + 10000; t += 200) {
        f.session.poll(t);
    }
    REQUIRE(f.session.hello_due() == kT0 + 10000);
    const auto before = f.io.sent.size();
    f.session.poll(kT0 + 10000);
    REQUIRE(f.session.failure() == SessionFailure::kHolePunchTimeout);
    REQUIRE(f.io.sent.size() == before);
}

TEST_CASE("session: when idle timeout and KEEPALIVE fall together only CLOSE goes out",
          "[peer][session]") {
    Fixture f;
    f.connect();
    REQUIRE(f.rx(keepalive(kPeerEpoch, 2), kT0 + 10000).accepted());  // 유휴 마감 = +60000
    for (const Millis t : {Millis{15000}, Millis{30000}, Millis{45000}}) {
        f.session.poll(kT0 + t);
    }
    REQUIRE(f.session.keepalive_due() == kT0 + 60000);
    REQUIRE(f.session.idle_deadline() == kT0 + 60000);
    f.io.sent.clear();
    f.session.poll(kT0 + 60000);
    REQUIRE(f.session.failure() == SessionFailure::kTunnelDropped);
    REQUIRE(f.io.sent.size() == 1);
    REQUIRE(f.io.sent[0].type() == PacketType::kClose);
}

TEST_CASE("session: a terminal session has no deadline left", "[peer][session]") {
    // 남은 마감이 있으면 루프의 대기가 그 마감으로 계속 0 이 되어 돈다 (concurrency.md 4장).
    {
        Fixture f;
        f.connect();
        f.session.close_by_user(kT0 + 1);
        REQUIRE_FALSE(f.session.next_deadline().has_value());
    }
    {
        Fixture f;
        f.session.start_punch(kT0);
        f.session.poll(kT0 + 10000);
        REQUIRE_FALSE(f.session.next_deadline().has_value());
    }
    {
        Fixture f;
        f.connect();
        REQUIRE(f.rx(close_rx(kPeerEpoch, 2, 0x00), kT0 + 1).accepted());
        REQUIRE_FALSE(f.session.next_deadline().has_value());
    }
}

TEST_CASE("session: a failed send of any type counts tx_err_send in the session table",
          "[peer][session]") {
    Fixture f;
    f.io.send_ok = false;
    f.session.start_punch(kT0);  // HELLO 둘
    REQUIRE(f.count(Counter::TxErrSend) == 2);
    REQUIRE(f.count(Counter::TxErrAck) == 0);
}

TEST_CASE("session: punch start on a session already CONNECTED arms nothing", "[peer][session]") {
    // 상대 nonce 를 아는 쪽만 만들 수 있는 순서지만, 걸면 양쪽 증거가 선 세션이 10초 뒤 실패한다.
    Fixture f;
    REQUIRE(f.rx(hello(kPeerEpoch, 0), kT0).accepted());
    REQUIRE(f.rx(hello_ack(kPeerEpoch, 1), kT0).accepted());
    REQUIRE(f.session.state() == SessionState::kConnected);
    const auto sent = f.io.sent.size();
    REQUIRE_FALSE(f.session.start_punch(kT0 + 100));
    REQUIRE(f.io.sent.size() == sent);
    REQUIRE_FALSE(f.session.hello_due().has_value());
    REQUIRE_FALSE(f.session.punch_deadline().has_value());
    f.session.poll(kT0 + 20000);
    REQUIRE(f.session.state() == SessionState::kConnected);
}

TEST_CASE("session: sequence keeps counting across renegotiation and failed sends",
          "[peer][session]") {
    // 4.4 방향별, 시도별 카운터 하나. 재협상은 새 시도가 아니고 우리 epoch 도 그대로다(9.5 5번).
    Fixture f;
    f.connect();
    f.io.send_ok = false;
    f.session.poll(kT0 + 5000);  // PING 하나, 실패
    f.io.send_ok = true;
    REQUIRE(f.rx(hello(kPeerEpoch2, 0), kT0 + 6000).accepted());
    f.session.poll(kT0 + 6200);
    REQUIRE(f.io.sent.size() > 6);
    for (std::size_t i = 0; i < f.io.sent.size(); ++i) {
        INFO("i=" << i);
        REQUIRE(f.io.sent[i].header.sequence == i);
        REQUIRE(f.io.sent[i].header.session_epoch == kLocalEpoch);
    }
}

TEST_CASE("session: ping_id keeps counting after renegotiation", "[peer][session]") {
    // 5.5 "세션 시도마다 0에서 시작해 1씩 증가". 재협상은 새 시도가 아니다 (9.5 5번).
    Fixture f;
    f.connect();
    f.session.poll(kT0 + 5000);  // ping 0
    REQUIRE(f.rx(hello(kPeerEpoch2, 0), kT0 + 6000).accepted());
    REQUIRE(f.rx(hello_ack(kPeerEpoch2, 1), kT0 + 6000).accepted());
    REQUIRE(f.session.state() == SessionState::kConnected);
    f.io.sent.clear();
    f.session.poll(kT0 + 11000);
    const auto pings = f.io.of_type(PacketType::kPing);
    REQUIRE(pings.size() == 1);
    REQUIRE(tunnel::decode_ping(pings[0].payload).ping_id == 1);
}

// ---------------------------------------------------------------- timer.tick (architecture.md 9장)

namespace {

std::vector<std::string> ticks_named(const sangtachi::testing::LogCapture& logs, std::string_view name) {
    std::vector<std::string> out;
    for (const auto& line : logs.with_event("timer.tick")) {
        if (line.find(" name=" + std::string(name) + " ") != std::string::npos) {
            out.push_back(line);
        }
    }
    return out;
}

}  // namespace

TEST_CASE("session: punch timers emit timer.tick with name and elapsed_ms", "[peer][session]") {
    sangtachi::testing::LogCapture logs;
    Fixture f;
    f.session.start_punch(kT0);
    f.session.poll(kT0 + 250);  // 첫 만료는 건 시각부터 250
    f.session.poll(kT0 + 450);  // 다음은 직전 만료부터 200
    auto hello_ticks = ticks_named(logs, "session.hello.572662306");
    REQUIRE(hello_ticks.size() == 2);
    REQUIRE(hello_ticks[0].find(" elapsed_ms=250") != std::string::npos);
    REQUIRE(hello_ticks[1].find(" elapsed_ms=200") != std::string::npos);
    f.session.poll(kT0 + 10000);
    const auto punch_ticks = ticks_named(logs, "session.punch.572662306");
    REQUIRE(punch_ticks.size() == 1);
    REQUIRE(punch_ticks[0].find(" elapsed_ms=10000") != std::string::npos);
    // 실패로 끝난 바퀴에는 HELLO 가 돌지 않으므로 tick 도 없다
    REQUIRE(ticks_named(logs, "session.hello.572662306").size() == 2);
}

TEST_CASE("session: CONNECTED timers emit timer.tick with name and elapsed_ms", "[peer][session]") {
    sangtachi::testing::LogCapture logs;
    Fixture f;
    f.connect();
    f.session.poll(kT0 + 5000);
    f.session.poll(kT0 + 15000);
    REQUIRE(f.rx(keepalive(kPeerEpoch, 2), kT0 + 20000).accepted());  // 유휴 갱신
    f.session.poll(kT0 + 70000);
    const auto ping = ticks_named(logs, "session.ping.572662306");
    REQUIRE(ping.size() >= 1);
    REQUIRE(ping[0].find(" elapsed_ms=5000") != std::string::npos);
    const auto keepalive_ticks = ticks_named(logs, "session.keepalive.572662306");
    REQUIRE(keepalive_ticks.size() == 1);
    REQUIRE(keepalive_ticks[0].find(" elapsed_ms=15000") != std::string::npos);
    const auto idle = ticks_named(logs, "session.idle.572662306");
    REQUIRE(idle.size() == 1);
    REQUIRE(idle[0].find(" elapsed_ms=50000") != std::string::npos);  // 마지막 갱신(+20000)부터
    REQUIRE(f.session.failure() == SessionFailure::kTunnelDropped);
}
