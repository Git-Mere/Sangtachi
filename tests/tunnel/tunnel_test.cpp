#include "sangtachi/tunnel/tunnel.hpp"

#include "sangtachi/tunnel/data_sender.hpp"

#include "../log_capture.hpp"
#include "../session_fixture.hpp"

#include "sangtachi/console.hpp"
#include "sangtachi/loop.hpp"
#include "sangtachi/network/stun.hpp"
#include "sangtachi/network/udp_socket.hpp"
#include "sangtachi/network/wsa.hpp"
#include "sangtachi/peer/candidates.hpp"
#include "sangtachi/platform/wait.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <vector>

// 시험 케이스 이름은 ASCII 로만 적는다. 이유는 network/wsa_test.cpp 머리에 있다.
//
// protocol.md 8장 수신 검증 파이프라인(8.1 검사 7·8, 9.4.1 의 자리), 8.4 내부 검증 10~15,
// 5.6 종료 상태 세션 보존, 4.3 폐기 목록의 파이프라인 확인, 7장 출발지 위생 (b) 층을 본다.
// 시계, 송신, 난수원은 주입한다.

using namespace sangtachi;
using namespace sangtachi::testing;
using peer::SessionState;
using tunnel::InnerError;
using tunnel::PacketType;
using tunnel::Tunnel;

namespace {

constexpr Millis kT0 = 2000000;

struct TunnelFixture {
    Counters counters;
    std::vector<SentPacket> sent;
    Millis now = kT0;
    bool send_ok = true;
    std::uint8_t next_random = 1;
    bool random_ok = true;
    std::size_t random_bytes_left = static_cast<std::size_t>(-1);  // 이만큼 내준 뒤 실패한다
    bool with_sink = false;
    std::vector<std::vector<std::byte>> sunk;
    std::vector<std::uint32_t> terminal_peers;
    Tunnel tunnel;

    explicit TunnelFixture(bool sink = false) : with_sink(sink), tunnel(counters, deps()) {
        tunnel.set_transition_handler(
            [this](const peer::Session& s, peer::SessionState, peer::SessionState to) {
                if (peer::is_terminal(to)) {
                    terminal_peers.push_back(s.peer_id());
                }
            });
    }

    tunnel::TunnelDeps deps() {
        tunnel::TunnelDeps d;
        d.send = [this](const network::Endpoint& to, std::span<const std::byte> bytes) {
            const auto check = tunnel::check_common(bytes);
            SentPacket p;
            p.to = to;
            p.header = check.header;
            p.payload.assign(check.payload.begin(), check.payload.end());
            sent.push_back(std::move(p));
            return send_ok;
        };
        d.clock = [this] { return now; };
        d.rtt_clock_us = [] { return std::uint64_t{0}; };
        // 결정적인 난수원. 바이트마다 1 씩 올라 0 이 되지 않는다.
        d.random = [this](std::span<std::byte> out) {
            if (!random_ok || random_bytes_left < out.size()) {
                return false;
            }
            random_bytes_left -= out.size();
            for (auto& b : out) {
                b = static_cast<std::byte>(next_random);
                next_random = static_cast<std::uint8_t>(next_random == 255 ? 1 : next_random + 1);
            }
            return true;
        };
        if (with_sink) {
            d.data_sink = [this](std::span<const std::byte> bytes) {
                sunk.emplace_back(bytes.begin(), bytes.end());
            };
        }
        return d;
    }

    // 상대마다 다른 라우팅 칸 (8.5). 자신은 10.100.0.2, kPeerId 는 10.100.0.1, 나머지는 .3~.5 다.
    static std::uint32_t vip_of(std::uint32_t peer_id) {
        return peer_id == kPeerId ? kPeerVip : 0x0A640003u + peer_id % 3;
    }

    peer::Session* create(std::uint32_t peer_id = kPeerId) { return create(peer_id, vip_of(peer_id)); }
    peer::Session* create(std::uint32_t peer_id, std::uint32_t peer_vip) {
        tunnel::NewSession r;
        r.local_peer_id = kLocalPeerId;
        r.local_virtual_ip = kLocalVip;
        r.peer_id = peer_id;
        r.peer_virtual_ip = peer_vip;
        r.candidates = {kCandA, kCandB};
        r.peer_is_host = true;
        r.peer_name = "Alice";
        return tunnel.create_session(r);
    }

    void feed(const Rx& rx) { tunnel.on_datagram(rx.from, rx.wire()); }
    void feed_bytes(std::vector<std::byte> bytes, network::Endpoint from = kCandA) {
        tunnel.on_datagram(from, bytes);
    }

    // 상대 seq 0, 1 로 CONNECTED 를 만든다.
    peer::Session* connect(std::uint32_t peer_id = kPeerId, std::uint32_t epoch = kPeerEpoch) {
        peer::Session* s = create(peer_id);
        REQUIRE(s != nullptr);
        s->start_punch(now);
        Rx h = hello(epoch, 0, kCandA, vip_of(peer_id));
        h.header.peer_id = peer_id;
        feed(h);
        Rx a = hello_ack(epoch, 1, kCandA, vip_of(peer_id), s->attempt_nonce());
        a.header.peer_id = peer_id;
        feed(a);
        REQUIRE(s->state() == SessionState::kConnected);
        return s;
    }

    // 프로세스 표.
    [[nodiscard]] std::uint64_t count(Counter c) const { return counters.value(c); }
    // 그 세션의 표.
    [[nodiscard]] std::uint64_t scount(Counter c, std::uint32_t peer_id = kPeerId) {
        peer::Session* s = tunnel.find(peer_id);
        REQUIRE(s != nullptr);
        return s->counters().value(c);
    }
};

Rx with_peer(Rx rx, std::uint32_t peer_id) {
    rx.header.peer_id = peer_id;
    return rx;
}

}  // namespace

// ---------------------------------------------------------------- 카운터 대응

TEST_CASE("tunnel: every packet error maps to the counter its name gives", "[tunnel]") {
    using tunnel::PacketError;
    for (const auto e :
         {PacketError::kShort, PacketError::kMagic, PacketError::kVersion, PacketError::kUnknownType,
          PacketError::kLength, PacketError::kTypeLength, PacketError::kBadReason,
          PacketError::kRosterDirection, PacketError::kRosterStale, PacketError::kRosterFlags,
          PacketError::kRosterName}) {
        const auto counter = tunnel::to_counter(e);
        REQUIRE(counter.has_value());
        REQUIRE(to_token(*counter) == tunnel::counter_name(e));
    }
    REQUIRE_FALSE(tunnel::to_counter(PacketError::kNone).has_value());
}

// ---------------------------------------------------------------- 8.4 내부 검증 10~15

namespace {

std::vector<std::byte> fix_checksum(std::vector<std::byte> p, std::size_t ihl_bytes = 20) {
    p[10] = std::byte{0};
    p[11] = std::byte{0};
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i < ihl_bytes; i += 2) {
        sum += (std::to_integer<std::uint32_t>(p[i]) << 8) | std::to_integer<std::uint32_t>(p[i + 1]);
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    const auto c = static_cast<std::uint16_t>(~sum & 0xFFFF);
    p[10] = static_cast<std::byte>(c >> 8);
    p[11] = static_cast<std::byte>(c & 0xFF);
    return p;
}

InnerError inner(const std::vector<std::byte>& p) {
    return tunnel::check_inner_ipv4(p, kPeerVip, kLocalVip);
}

}  // namespace

TEST_CASE("tunnel: inner IPv4 case table", "[tunnel][inner]") {
    const auto good = inner_ipv4(kPeerVip, kLocalVip);
    REQUIRE(inner(good) == InnerError::kNone);
    REQUIRE(inner(inner_ipv4(kPeerVip, kLocalVip, 20)) == InnerError::kNone);
    REQUIRE(inner(inner_ipv4(kPeerVip, kLocalVip, 1452)) == InnerError::kNone);

    // 10. 버전 니블
    auto v6 = good;
    v6[0] = std::byte{0x65};
    REQUIRE(inner(fix_checksum(v6)) == InnerError::kNotIpv4);
    REQUIRE(tunnel::check_inner_ipv4({}, kPeerVip, kLocalVip) == InnerError::kNotIpv4);

    // 11. IHL 하한과 상한
    auto ihl4 = good;
    ihl4[0] = std::byte{0x44};
    REQUIRE(inner(ihl4) == InnerError::kIhl);
    auto ihl6_short = inner_ipv4(kPeerVip, kLocalVip, 20);
    ihl6_short[0] = std::byte{0x46};  // 24바이트 헤더인데 페이로드가 20
    REQUIRE(inner(ihl6_short) == InnerError::kIhl);
    // 정확히 IHL*4 == 길이는 통과한다 (24바이트 헤더, 24바이트 페이로드)
    auto ihl6_exact = inner_ipv4(kPeerVip, kLocalVip, 24);
    ihl6_exact[0] = std::byte{0x46};
    REQUIRE(inner(fix_checksum(ihl6_exact, 24)) == InnerError::kNone);
    // IHL 15 = 60바이트 헤더
    auto ihl15 = inner_ipv4(kPeerVip, kLocalVip, 60);
    ihl15[0] = std::byte{0x4F};
    REQUIRE(inner(fix_checksum(ihl15, 60)) == InnerError::kNone);

    // 12. 체크섬. 옵션까지 덮는지 본다: IHL 6 에서 옵션 바이트를 바꾸면 깨진다
    auto bad_sum = good;
    bad_sum[11] = static_cast<std::byte>(std::to_integer<int>(bad_sum[11]) ^ 0x01);
    REQUIRE(inner(bad_sum) == InnerError::kChecksum);
    auto opt = fix_checksum(ihl6_exact, 24);
    opt[22] = std::byte{0x01};
    REQUIRE(inner(opt) == InnerError::kChecksum);

    // 13. total_length
    auto len = good;
    len[3] = std::byte{99};
    REQUIRE(inner(fix_checksum(len)) == InnerError::kLength);

    // 14, 15
    REQUIRE(inner(inner_ipv4(kPeerVip + 1, kLocalVip)) == InnerError::kSrc);
    REQUIRE(inner(inner_ipv4(kPeerVip, kLocalVip + 1)) == InnerError::kDst);

    // 순서: 체크섬과 출발지가 둘 다 틀리면 체크섬이 이긴다
    auto both = inner_ipv4(kPeerVip + 1, kLocalVip);
    both[10] = std::byte{0};
    REQUIRE(inner(both) == InnerError::kChecksum);
}

TEST_CASE("tunnel: inner errors map to the 8.4 counters", "[tunnel][inner]") {
    REQUIRE(tunnel::to_counter(InnerError::kNotIpv4) == Counter::DropInnerNotIpv4);
    REQUIRE(tunnel::to_counter(InnerError::kIhl) == Counter::DropInnerIhl);
    REQUIRE(tunnel::to_counter(InnerError::kChecksum) == Counter::DropInnerChecksum);
    REQUIRE(tunnel::to_counter(InnerError::kLength) == Counter::DropInnerLength);
    REQUIRE(tunnel::to_counter(InnerError::kSrc) == Counter::DropInnerSrc);
    REQUIRE(tunnel::to_counter(InnerError::kDst) == Counter::DropInnerDst);
    REQUIRE_FALSE(tunnel::to_counter(InnerError::kNone).has_value());
}

// ---------------------------------------------------------------- 8.1 공통 검사

TEST_CASE("tunnel: checks 1 to 6 run before the session lookup", "[tunnel]") {
    TunnelFixture f;
    f.feed_bytes(std::vector<std::byte>(19, std::byte{0x53}));
    REQUIRE(f.count(Counter::DropShort) == 1);
    auto magic = keepalive(kPeerEpoch, 0, kCandA).wire();
    magic[3] = std::byte{0x00};
    f.feed_bytes(magic);
    REQUIRE(f.count(Counter::DropMagic) == 1);
    auto version = keepalive(kPeerEpoch, 0).wire();
    version[4] = std::byte{0x02};
    f.feed_bytes(version);
    REQUIRE(f.count(Counter::DropVersion) == 1);
    auto type = keepalive(kPeerEpoch, 0).wire();
    type[5] = std::byte{0x09};
    f.feed_bytes(type);
    REQUIRE(f.count(Counter::DropUnknownType) == 1);
    auto length = keepalive(kPeerEpoch, 0).wire();
    length.push_back(std::byte{0});
    f.feed_bytes(length);
    REQUIRE(f.count(Counter::DropLength) == 1);
    f.feed(make_rx(PacketType::kPing, {std::byte{0}}, kPeerEpoch, 0));
    REQUIRE(f.count(Counter::DropTypeLength) == 1);
    REQUIRE(f.count(Counter::DropUnknownPeer) == 0);
}

TEST_CASE("tunnel: IDLE has no session so an early HELLO is drop_unknown_peer", "[tunnel]") {
    // roadmap.md Phase 4 "IDLE 에는 세션이 없다". 상대 peer_id 를 알아도 후보가 오기 전에는
    // 세션을 만들지 않는다.
    TunnelFixture f;
    f.feed(hello(kPeerEpoch, 0));
    REQUIRE(f.count(Counter::DropUnknownPeer) == 1);
    REQUIRE(f.sent.empty());
    REQUIRE(f.tunnel.session_count() == 0);
}

TEST_CASE("tunnel: a peer_id without a session is drop_unknown_peer", "[tunnel]") {
    TunnelFixture f;
    f.connect();
    f.sent.clear();
    f.feed(with_peer(hello(kPeerEpoch, 5), 0x33333333u));
    REQUIRE(f.count(Counter::DropUnknownPeer) == 1);
    REQUIRE(f.sent.empty());
}

TEST_CASE("tunnel: a retired epoch is dropped even as a HELLO", "[tunnel]") {
    TunnelFixture f;
    peer::Session* s = f.connect();
    f.tunnel.retired_epochs().add(0xEEEE0001u, f.now);
    f.sent.clear();
    f.feed(hello(0xEEEE0001u, 9));
    REQUIRE(f.scount(Counter::DropRetiredEpoch) == 1);
    REQUIRE(f.count(Counter::DropRetiredEpoch) == 0);  // 검사 7 을 지났으므로 세션의 표다
    REQUIRE(s->renegotiations() == 0);
    REQUIRE(f.sent.empty());
}

// ---------------------------------------------------------------- 재협상 뒤 지연 패킷 (roadmap.md 상태 머신 셋째)

TEST_CASE("tunnel: a late packet of the old epoch after renegotiation is drop_retired_epoch",
          "[tunnel]") {
    TunnelFixture f;
    peer::Session* s = f.connect();
    // 재협상 전의 지연 패킷은 고정된 epoch 와 같으므로 수용된다
    f.feed(keepalive(kPeerEpoch, 7));
    REQUIRE(f.scount(Counter::DropRetiredEpoch) == 0);
    REQUIRE(f.scount(Counter::DropStaleEpoch) == 0);
    f.now += 2000;
    f.feed(hello(kPeerEpoch2, 0));
    REQUIRE(s->renegotiations() == 1);
    f.feed(keepalive(kPeerEpoch, 6));
    REQUIRE(f.scount(Counter::DropRetiredEpoch) == 1);
    REQUIRE(f.scount(Counter::DropStaleEpoch) == 0);
}

TEST_CASE("tunnel: an epoch pushed out of the full retired list falls to drop_stale_epoch",
          "[tunnel]") {
    // 4.3 두 번째 방어선. 고정된 세션에 비-HELLO 를 넣는다. 미고정이면 drop_no_epoch 라 다른
    // 것을 본 것이 된다 (roadmap.md 폐기 목록 상한).
    TunnelFixture f;
    f.connect();
    for (std::uint32_t i = 0; i < 17; ++i) {
        f.tunnel.retired_epochs().add(0xF0000000u + i, f.now);
    }
    REQUIRE(f.tunnel.retired_epochs().size() == 16);
    f.feed(keepalive(0xF0000000u, 10));  // 밀려난 첫 항목
    REQUIRE(f.scount(Counter::DropStaleEpoch) == 1);
    REQUIRE(f.scount(Counter::DropRetiredEpoch) == 0);
    f.feed(keepalive(0xF0000010u, 11));  // 마지막으로 넣은 항목은 남아 있다
    REQUIRE(f.scount(Counter::DropRetiredEpoch) == 1);
}

// ---------------------------------------------------------------- 9.4.1 종료 상태, 5.6 보존

TEST_CASE("tunnel: after CLOSE a KEEPALIVE only raises drop_terminal_state and gets no reply",
          "[tunnel]") {
    TunnelFixture f;
    peer::Session* s = f.connect();
    f.feed(close_rx(kPeerEpoch, 2, 0x00));
    REQUIRE(s->state() == SessionState::kClosed);
    REQUIRE(f.terminal_peers == std::vector<std::uint32_t>{kPeerId});
    f.sent.clear();
    const Counters process_before = f.counters;
    const Counters session_before = s->counters();
    f.feed(keepalive(kPeerEpoch, 3));
    f.feed(hello(kPeerEpoch, 4));
    f.feed(ping(kPeerEpoch, 5, 1));
    REQUIRE(s->counters().value(Counter::DropTerminalState) == 3);
    REQUIRE(f.sent.empty());
    // 다른 카운터는 어느 표에서도 오르지 않았다
    for (std::size_t i = 0; i < kCounterCount; ++i) {
        const auto c = static_cast<Counter>(i);
        REQUIRE(f.counters.value(c) == process_before.value(c));
        if (c != Counter::DropTerminalState) {
            REQUIRE(s->counters().value(c) == session_before.value(c));
        }
    }
}

TEST_CASE("tunnel: a retired epoch to a terminal session is drop_retired_epoch", "[tunnel]") {
    // 9.4.1 의 자리는 8.1 전체 뒤다 (tunnel.hpp 머리 표).
    TunnelFixture f;
    peer::Session* s = f.connect();
    s->close_by_user(f.now);
    f.tunnel.retired_epochs().add(0xEEEE0002u, f.now);
    f.feed(keepalive(0xEEEE0002u, 9));
    REQUIRE(s->counters().value(Counter::DropRetiredEpoch) == 1);
    REQUIRE(s->counters().value(Counter::DropTerminalState) == 0);
}

TEST_CASE("tunnel: a terminal session is kept for two minutes then forgotten", "[tunnel]") {
    TunnelFixture f;
    peer::Session* s = f.connect();
    s->close_by_user(f.now);
    const Millis closed_at = f.now;
    REQUIRE(f.tunnel.earliest_deadline() == closed_at + 120000);
    f.now = closed_at + 119999;
    f.tunnel.run_timers(f.now);
    f.feed(keepalive(kPeerEpoch, 3));
    REQUIRE(s->counters().value(Counter::DropTerminalState) == 1);
    f.now = closed_at + 120000;
    f.tunnel.run_timers(f.now);
    REQUIRE(f.tunnel.session_count() == 0);
    f.feed(keepalive(kPeerEpoch, 4));
    REQUIRE(f.count(Counter::DropUnknownPeer) == 1);
}

TEST_CASE("tunnel: at most MAX_PEERS - 1 terminal sessions are kept, oldest goes first",
          "[tunnel]") {
    TunnelFixture f;
    for (std::uint32_t i = 0; i < 5; ++i) {
        peer::Session* s = f.create(0x40000000u + i, kPeerVip);
        REQUIRE(s != nullptr);
        s->close_by_user(f.now);
        f.tunnel.run_timers(f.now);
        f.now += 1000;
    }
    REQUIRE(f.tunnel.session_count() == 4);
    REQUIRE(f.tunnel.find(0x40000000u) == nullptr);
    for (std::uint32_t i = 1; i < 5; ++i) {
        REQUIRE(f.tunnel.find(0x40000000u + i) != nullptr);
    }
}

TEST_CASE("tunnel: the cap counts only terminal sessions", "[tunnel]") {
    TunnelFixture f;
    for (std::uint32_t i = 0; i < 4; ++i) {
        f.create(0x40000000u + i, kPeerVip)->close_by_user(f.now);
        f.now += 1000;
    }
    f.create(0x50000000u, kPeerVip);  // 활성 하나
    f.tunnel.run_timers(f.now);
    REQUIRE(f.tunnel.session_count() == 5);
}

TEST_CASE("tunnel: leaving the room forgets terminal sessions before two minutes", "[tunnel]") {
    TunnelFixture f;
    peer::Session* s = f.connect();
    s->close_by_user(f.now);
    f.connect(0x60000000u, 0xCAFE0001u);  // 새 시도가 앞 세션의 epoch 를 폐기했다
    f.sent.clear();
    f.tunnel.leave_room(f.now + 10);
    REQUIRE(f.tunnel.session_count() == 0);
    // 활성 세션은 사용자 종료로 CLOSE 를 1회 보냈다
    REQUIRE(f.sent.size() == 1);
    REQUIRE(f.sent[0].type() == PacketType::kClose);
    f.feed(keepalive(kPeerEpoch, 3));
    REQUIRE(f.count(Counter::DropUnknownPeer) == 1);
}

// ---------------------------------------------------------------- 세션 생성, 폐기 등록 (4.3, 5.1)

TEST_CASE("tunnel: a second active session for the same peer is refused", "[tunnel]") {
    TunnelFixture f;
    REQUIRE(f.create() != nullptr);
    REQUIRE(f.create() == nullptr);
    REQUIRE(f.tunnel.session_count() == 1);
}

TEST_CASE("tunnel: a failing random source creates no session", "[tunnel]") {
    TunnelFixture f;
    f.random_ok = false;
    REQUIRE(f.create() == nullptr);
    REQUIRE(f.tunnel.session_count() == 0);
}

TEST_CASE("tunnel: sessions draw probe nonces from the tunnel random source", "[tunnel][probe]") {
    TunnelFixture f;
    peer::Session* s = f.create();
    REQUIRE(s != nullptr);
    f.random_ok = false;  // 10.4 (c) 의 probe nonce 를 뽑지 못한다
    f.tunnel.on_datagram(kOutside, hello(kPeerEpoch, 0, kOutside).wire());
    REQUIRE(f.sent.size() == 1);
    REQUIRE(f.sent[0].type() == PacketType::kHelloAck);
    REQUIRE(s->probe_paths() == 0);
    f.random_ok = true;
    f.tunnel.on_datagram(kOutside, keepalive(kPeerEpoch, 1, kOutside).wire());
    REQUIRE(s->probe_paths() == 1);
}

TEST_CASE("tunnel: the next attempt retires the peer epoch and nonce of a kept terminal session",
          "[tunnel]") {
    TunnelFixture f;
    peer::Session* old = f.connect();
    const auto old_nonce = old->attempt_nonce();
    const auto old_epoch = old->local_epoch();
    old->close_by_user(f.now);
    REQUIRE_FALSE(f.tunnel.retired_epochs().contains(kPeerEpoch, f.now));  // 아직 새 시도 전
    f.now += 10;
    peer::Session* fresh = f.create();  // 같은 상대, 종료 세션을 대신한다
    REQUIRE(fresh != nullptr);
    REQUIRE(fresh->state() == SessionState::kPunching);  // old 는 지워졌다. 주소는 재사용될 수 있다
    REQUIRE(f.tunnel.retired_epochs().contains(kPeerEpoch, f.now));   // 상대의 epoch
    REQUIRE_FALSE(f.tunnel.retired_epochs().contains(old_epoch, f.now));  // 우리 epoch 가 아니다
    REQUIRE(f.tunnel.retired_nonces().contains(old_nonce, f.now));
    REQUIRE_FALSE(fresh->attempt_nonce() == old_nonce);
    REQUIRE(f.tunnel.session_count() == 1);
}

// ---------------------------------------------------------------- DATA 출구 (concurrency.md 3장)

TEST_CASE("tunnel: DATA passing 8.4 reaches the payload hook byte for byte", "[tunnel]") {
    TunnelFixture f(true);
    f.connect();
    const auto payload = inner_ipv4(kPeerVip, kLocalVip, 333);
    f.feed(data_rx(kPeerEpoch, 2, payload));
    REQUIRE(f.sunk.size() == 1);
    REQUIRE(f.sunk[0] == payload);
}

TEST_CASE("tunnel: without a hook DATA passing 8.4 is drop_no_sink", "[tunnel]") {
    TunnelFixture f(false);
    f.connect();
    f.feed(data_rx(kPeerEpoch, 2, inner_ipv4(kPeerVip, kLocalVip)));
    REQUIRE(f.scount(Counter::DropNoSink) == 1);
}

TEST_CASE("tunnel: DATA with wrong inner addresses never reaches the hook", "[tunnel]") {
    TunnelFixture f(true);
    f.connect();
    f.feed(data_rx(kPeerEpoch, 2, inner_ipv4(kLocalVip, kLocalVip)));
    f.feed(data_rx(kPeerEpoch, 3, inner_ipv4(kPeerVip, kPeerVip)));
    REQUIRE(f.scount(Counter::DropInnerSrc) == 1);
    REQUIRE(f.scount(Counter::DropInnerDst) == 1);
    REQUIRE(f.sunk.empty());
}

// ---------------------------------------------------------------- 5.7 ROSTER 수신 (파이프라인 주입)

namespace {

std::vector<std::byte> roster_bytes(std::uint32_t generation, std::vector<std::vector<std::uint8_t>> names) {
    std::vector<std::byte> out;
    auto put32 = [&](std::uint32_t v) {
        for (int s = 24; s >= 0; s -= 8) {
            out.push_back(static_cast<std::byte>((v >> s) & 0xFF));
        }
    };
    put32(generation);
    out.push_back(static_cast<std::byte>(names.size()));
    std::uint32_t id = 1;
    for (const auto& name : names) {
        put32(id++);
        put32(0x0A640000u + id);
        out.push_back(std::byte{0});
        out.push_back(static_cast<std::byte>(name.size()));
        for (const auto ch : name) {
            out.push_back(static_cast<std::byte>(ch));
        }
    }
    return out;
}

std::vector<std::uint8_t> name_of(std::size_t len, std::uint8_t ch = 'a') {
    return std::vector<std::uint8_t>(len, ch);
}

}  // namespace

TEST_CASE("tunnel: ROSTER names are checked through the receive path", "[tunnel][roster]") {
    // roadmap.md Phase 4 표시 이름의 ROSTER 항목. 수신 경로 주입으로 돌린다.
    TunnelFixture f;
    peer::Session* s = f.connect();
    std::uint32_t seq = 10;
    auto feed_roster = [&](std::vector<std::byte> payload) {
        f.feed(make_rx(PacketType::kRoster, std::move(payload), kPeerEpoch, seq++));
    };
    // 길이 0 과 17 은 구조 결함
    feed_roster(roster_bytes(1, {name_of(0), name_of(5)}));
    feed_roster(roster_bytes(1, {name_of(17)}));
    REQUIRE(f.scount(Counter::DropTypeLength) == 2);
    // 마지막 멤버가 페이로드 끝을 넘는다
    auto over = roster_bytes(1, {name_of(3), name_of(3)});
    over.pop_back();
    feed_roster(over);
    // count 개를 읽고 바이트가 남는다
    auto extra = roster_bytes(1, {name_of(3)});
    extra.push_back(std::byte{'x'});
    feed_roster(extra);
    REQUIRE(f.scount(Counter::DropTypeLength) == 4);
    // 공백과 제어 바이트
    feed_roster(roster_bytes(1, {{'a', ' ', 'b'}}));
    feed_roster(roster_bytes(1, {{'a', 0x07}}));
    REQUIRE(f.scount(Counter::DropRosterName) == 2);
    REQUIRE_FALSE(s->last_roster_generation().has_value());
    // 1 과 16 은 받는다
    feed_roster(roster_bytes(1, {name_of(1), name_of(16, 'Z')}));
    REQUIRE(s->last_roster_generation() == 1u);
    feed_roster(roster_bytes(1, {name_of(1)}));  // 같은 값은 재송신이다
    REQUIRE(f.scount(Counter::DropRosterStale) == 0);
    feed_roster(roster_bytes(0, {name_of(1)}));
    REQUIRE(f.scount(Counter::DropRosterStale) == 1);
}

// ---------------------------------------------------------------- 타이머와 종료 처리기

TEST_CASE("tunnel: run_timers drives session timers and the terminal handler", "[tunnel]") {
    sangtachi::testing::LogCapture logs;
    TunnelFixture f;
    peer::Session* s = f.create();
    s->start_punch(f.now);
    REQUIRE(f.tunnel.earliest_deadline() == f.now + 200);
    f.now += 10000;
    f.tunnel.run_timers(f.now);
    REQUIRE(s->failure() == peer::SessionFailure::kHolePunchTimeout);
    REQUIRE(f.terminal_peers == std::vector<std::uint32_t>{kPeerId});
    // 종료 전이 때 카운터 전량 (architecture.md 9장)
    const auto counter_lines = logs.with_event("counter");
    REQUIRE(counter_lines.size() == kCounterCount);
    for (const auto& line : counter_lines) {
        REQUIRE(line.find(" peer_id=572662306") != std::string::npos);  // 0x22222222
        REQUIRE(line.find(" peer_name=Alice") != std::string::npos);
    }
    REQUIRE(logs.with_event("session.failed").size() == 1);
    REQUIRE(f.tunnel.earliest_deadline() == f.now + 120000);
}

TEST_CASE("tunnel: session.loss through the receive entry point", "[tunnel][loop][loss]") {
    // roadmap.md Phase 5 손실 항목을 EventLoop::receive_datagram 으로 한 번에 본다. 누락, 재정렬, 첫
    // 패킷보다 앞선 번호, 32비트 경계, 재협상 두 구간의 합이다 (protocol.md 4.5).
    const network::WsaContext wsa;
    auto opened = network::open_udp_socket();
    REQUIRE(opened.ok());
    Counters loop_counters;
    auto console = ConsoleSession::create();
    REQUIRE(console != nullptr);
    EventLoop loop(std::move(*opened.socket), loop_counters, console->queue(), console->event(),
                   LoopOptions{});
    REQUIRE(loop.valid());
    TunnelFixture f;
    loop.set_tunnel_handler([&](const network::Endpoint& from, std::span<const std::byte> bytes) {
        f.tunnel.on_datagram(from, bytes);
    });
    peer::Session* s = f.create();
    REQUIRE(s != nullptr);
    s->start_punch(f.now);
    const auto inject = [&loop](const Rx& rx) { loop.receive_datagram(rx.from, rx.wire()); };

    // 첫 구간. 32비트 경계 앞에서 시작한다. baseline 은 0xFFFFFFF6 을 처리한 뒤의 위치다.
    inject(hello(kPeerEpoch, 0xFFFFFFF5u));
    inject(hello_ack(kPeerEpoch, 0xFFFFFFF6u, kCandA, kPeerVip, s->attempt_nonce()));
    REQUIRE(s->state() == SessionState::kConnected);
    inject(keepalive(kPeerEpoch, 0xFFFFFFF3u));  // 첫 패킷보다 앞선 번호. 세지 않는다
    inject(keepalive(kPeerEpoch, 0xFFFFFFF8u));  // F7 이 빠졌다
    inject(keepalive(kPeerEpoch, 0xFFFFFFFAu));
    inject(keepalive(kPeerEpoch, 0xFFFFFFF9u));  // 재정렬
    inject(keepalive(kPeerEpoch, 0x00000001u));  // 경계를 넘는다. FB~FF, 0 이 빠졌다
    REQUIRE(s->loss() == peer::LossStats{11, 4});

    // 재협상 뒤 둘째 구간. baseline 은 seq 1 을 처리한 뒤다.
    f.now += 2000;
    inject(hello(kPeerEpoch2, 0));
    REQUIRE(s->renegotiations() == 1);
    inject(hello_ack(kPeerEpoch2, 1, kCandA, kPeerVip, s->attempt_nonce()));
    REQUIRE(s->state() == SessionState::kConnected);
    inject(keepalive(kPeerEpoch2, 3));  // 2 가 빠졌다

    sangtachi::testing::LogCapture logs;
    f.tunnel.emit_session_counters();
    const auto lines = logs.with_event("session.loss");
    REQUIRE(lines.size() == 1);
    REQUIRE(lines[0] == "INFO session.loss peer_id=572662306 expected=13 accepted=5 lost=8");
}

// ---------------------------------------------------------------- 퍼즈 (roadmap.md Phase 5)
//
// 두 단계다. 1단계는 분류와 8.1 을 소켓으로, 2단계는 8.2 뒤의 파이프라인을 수신 주입 진입점으로 본다.
// 메모리 오류 판정은 ASan 빌드(scripts/test.ps1 -Asan)가 한다. 일반 빌드에서도 같은 시험이 돌고 그때는
// 크래시만 본다. seed, 개수, 길이 범위는 시작할 때 표준 오류에 한 줄로 낸다. 크래시해도 남는다.

namespace {

// 진단 줄 10만 개를 내지 않는다. 판정은 카운터로 한다.
class QuietLog final : public LogSink {
public:
    QuietLog() noexcept : previous_(set_log_sink(this)) {}
    ~QuietLog() override { (void)set_log_sink(previous_); }
    QuietLog(const QuietLog&) = delete;
    QuietLog& operator=(const QuietLog&) = delete;
    void write(std::string_view) override {}

private:
    LogSink* previous_;
};

void fill_random(std::mt19937& rng, std::span<std::byte> out) {
    for (auto& b : out) {
        b = static_cast<std::byte>(rng() & 0xFF);
    }
}

// 5장 타입마다 8.1 6번을 통과하는 페이로드 길이 하나 (5.1~5.7).
std::size_t valid_length(std::uint8_t type, std::mt19937& rng) {
    switch (type) {
        case 0x01:
        case 0x02:
            return 20;
        case 0x03:
            return 0;
        case 0x04:
            return 20 + rng() % (protocol::kMaxInner - 20 + 1);
        case 0x05:
        case 0x06:
            return 8;
        case 0x07:
            return 1;
        default:
            return 16 + rng() % (135 - 16 + 1);  // 0x08 ROSTER
    }
}

}  // namespace

TEST_CASE("fuzz: stage 1 random datagrams through the socket", "[tunnel][loop][fuzz]") {
    constexpr std::uint32_t kSeed = 0x5A4E4701u;
    constexpr std::size_t kCount = 100000;
    constexpr std::size_t kMinLen = 1;
    constexpr std::size_t kMaxLen = 2000;
    constexpr std::size_t kBatch = 32;  // 2000 x 32 바이트는 SO_RCVBUF 256KB 안이다. 커널이 버리지 않는다
    std::fprintf(stderr, "fuzz stage=1 seed=0x%08X count=%zu len=%zu..%zu\n", kSeed, kCount, kMinLen,
                 kMaxLen);
    const QuietLog quiet;

    const network::WsaContext wsa;
    auto sender = network::open_udp_socket();
    auto receiver = network::open_udp_socket();
    REQUIRE(sender.ok());
    REQUIRE(receiver.ok());
    const network::Endpoint to(0x7F000001u, receiver.socket->local().port());
    Counters loop_counters;
    auto console = ConsoleSession::create();
    REQUIRE(console != nullptr);
    EventLoop loop(std::move(*receiver.socket), loop_counters, console->queue(), console->event(),
                   LoopOptions{});
    REQUIRE(loop.valid());

    // 8.1 검사 7 을 지나 세션까지 가는 상대가 하나 있다.
    TunnelFixture f;
    REQUIRE(f.connect() != nullptr);
    std::size_t tunnel_calls = 0;
    std::size_t stun_calls = 0;
    loop.set_tunnel_handler([&](const network::Endpoint& from, std::span<const std::byte> bytes) {
        ++tunnel_calls;
        f.tunnel.on_datagram(from, bytes);
    });
    // STUN 으로 분류된 것은 응답 해석기까지 태운다. 트랜잭션 ID 는 데이터그램의 것을 쓴다.
    loop.set_stun_handler([&](const network::Endpoint&, std::span<const std::byte> bytes) {
        ++stun_calls;
        if (const auto id = network::peek_transaction_id(bytes)) {
            (void)network::parse_binding_response(bytes, *id);
        }
    });
    // 대기가 무한이 되지 않게 마감을 늘 지난 것으로 둔다. run_once 가 기다리지 않고 돌아온다.
    loop.set_deadline_source({[] { return std::optional<Millis>(Millis{0}); }, [](Millis) {}});

    const auto accounted = [&] {
        return loop_counters.value(Counter::DropOversizeDatagram) +
               loop_counters.value(Counter::DropBadSource) +
               loop_counters.value(Counter::DropUnclassified) + tunnel_calls + stun_calls;
    };

    std::mt19937 rng(kSeed);
    std::uniform_int_distribution<std::size_t> length(kMinLen, kMaxLen);
    std::vector<std::byte> datagram;
    std::size_t sent = 0;
    while (sent < kCount) {
        const std::size_t batch = std::min(kBatch, kCount - sent);
        for (std::size_t i = 0; i < batch; ++i) {
            datagram.resize(length(rng));
            fill_random(rng, datagram);
            // 무작위 바이트가 STUN 분류(상위 2비트 00, magic cookie)에 닿을 확률은 2^-34 다. 여덟에 하나는
            // 그 두 자리를 맞춰 STUN 갈래와 응답 해석기도 밟게 한다.
            if (datagram.size() >= 20 && rng() % 8 == 0) {
                datagram[0] &= std::byte{0x3F};
                datagram[4] = std::byte{0x21};
                datagram[5] = std::byte{0x12};
                datagram[6] = std::byte{0xA4};
                datagram[7] = std::byte{0x42};
            }
            REQUIRE(sender.socket->send_to(to, datagram).ok);
        }
        sent += batch;
        for (int spins = 0; accounted() < sent && spins < 10000; ++spins) {
            REQUIRE(loop.run_once());
        }
        INFO("sent " << sent);
        REQUIRE(accounted() == sent);
    }
    REQUIRE(loop_counters.value(Counter::RxErrRecv) == 0);
    // 세 갈래가 다 밟혔다
    REQUIRE(loop_counters.value(Counter::DropOversizeDatagram) > 0);
    REQUIRE(stun_calls > 0);
    REQUIRE(f.count(Counter::DropMagic) > 0);
}

TEST_CASE("fuzz: stage 2 valid headers with random bodies through the receive entry point",
          "[tunnel][loop][fuzz]") {
    constexpr std::uint32_t kSeed = 0x5A4E4702u;
    constexpr std::size_t kCount = 100000;
    constexpr std::size_t kMinLen = protocol::kHeaderSize;
    constexpr std::size_t kMaxLen = protocol::kMaxDatagram;
    std::fprintf(stderr, "fuzz stage=2 seed=0x%08X count=%zu len=%zu..%zu\n", kSeed, kCount, kMinLen,
                 kMaxLen);
    const QuietLog quiet;

    const network::WsaContext wsa;
    auto opened = network::open_udp_socket();
    REQUIRE(opened.ok());
    Counters loop_counters;
    auto console = ConsoleSession::create();
    REQUIRE(console != nullptr);
    EventLoop loop(std::move(*opened.socket), loop_counters, console->queue(), console->event(),
                   LoopOptions{});
    REQUIRE(loop.valid());
    TunnelFixture f;
    loop.set_tunnel_handler([&f](const network::Endpoint& from, std::span<const std::byte> bytes) {
        f.tunnel.on_datagram(from, bytes);
    });

    // 세션이 CLOSE 로 끝나면 새 epoch 로 다시 세운다. 이전 epoch 는 터널이 폐기 목록에 넣는다 (4.3).
    // 끝난 세션의 표는 합쳐 둔다.
    std::uint32_t epoch = 0xC0DE0000u;
    peer::Session* s = f.connect(kPeerId, epoch);
    Counters session_total;
    std::size_t reconnects = 0;
    const auto fold = [&session_total](const peer::Session& done) {
        for (std::size_t c = 0; c < kCounterCount; ++c) {
            session_total.add(static_cast<Counter>(c), done.counters().value(static_cast<Counter>(c)));
        }
    };

    std::mt19937 rng(kSeed);
    std::uint32_t last_seq = 1;
    std::vector<std::byte> datagram;
    for (std::size_t i = 0; i < kCount; ++i) {
        if (!s->active()) {
            fold(*s);
            ++epoch;
            ++reconnects;
            s = f.connect(kPeerId, epoch);
            last_seq = 1;
        }
        tunnel::Header h;
        h.peer_id = kPeerId;
        h.session_epoch = epoch;
        // 타입은 대개 5장 목록 안이고, 가끔 밖이다 (8.1 4번).
        h.type = (rng() % 8 != 0) ? static_cast<std::uint8_t>(1 + rng() % 8)
                                  : static_cast<std::uint8_t>(rng() & 0xFF);
        // 본문 길이는 반은 그 타입이 받는 길이, 반은 아무 길이다 (8.1 6번).
        const std::size_t body = (rng() % 2 == 0)
                                     ? valid_length(h.type, rng)
                                     : rng() % (kMaxLen - kMinLen + 1);
        // payload_length 는 대개 실제 길이이고 가끔 아무 값이다 (8.1 5번).
        h.payload_length = (rng() % 8 != 0) ? static_cast<std::uint16_t>(body)
                                            : static_cast<std::uint16_t>(rng() & 0xFFFF);
        // sequence 는 반은 직전 근처(중복, 재정렬, 창 밖), 반은 아무 값이다 (8.3).
        if (rng() % 2 == 0) {
            last_seq += static_cast<std::uint32_t>(static_cast<std::int32_t>(rng() % 80) - 70);
            h.sequence = last_seq;
        } else {
            h.sequence = static_cast<std::uint32_t>(rng());
        }
        const auto header = tunnel::serialize_header(h);
        datagram.assign(header.begin(), header.end());
        datagram.resize(protocol::kHeaderSize + body);
        const std::span<std::byte> payload(datagram.data() + protocol::kHeaderSize, body);
        if (h.type == 0x04 && body >= 20 && rng() % 2 == 0) {
            // 8.4 를 깊이 밟게 그럴듯한 내부 IPv4 를 넣고 몇 바이트를 흔든다.
            const auto inner = inner_ipv4(kPeerVip, kLocalVip, body);
            std::copy(inner.begin(), inner.end(), payload.begin());
            for (auto k = rng() % 3; k > 0; --k) {
                payload[rng() % body] = static_cast<std::byte>(rng() & 0xFF);
            }
        } else {
            fill_random(rng, payload);
        }
        loop.receive_datagram(kCandA, datagram);
        if (f.sent.size() > 1024) {
            f.sent.clear();  // 회신 기록이 10만 개로 자라지 않게 한다
        }
    }
    fold(*s);
    INFO("reconnects " << reconnects);

    // 8.1 을 넘었다. magic 은 늘 맞으므로 drop_magic 은 0 이다.
    REQUIRE(f.count(Counter::DropMagic) == 0);
    REQUIRE(f.count(Counter::DropUnknownType) > 0);
    REQUIRE(f.count(Counter::DropLength) > 0);
    REQUIRE(f.count(Counter::DropTypeLength) > 0);
    // 8.3, 8.4, 타입별 검사까지 갔다 (세션 표).
    REQUIRE(session_total.value(Counter::DropDuplicate) > 0);
    REQUIRE(session_total.value(Counter::DropTooOld) > 0);
    REQUIRE(session_total.value(Counter::DropInnerNotIpv4) > 0);
    REQUIRE(session_total.value(Counter::DropInnerChecksum) > 0);
    REQUIRE(session_total.value(Counter::DropNoSink) > 0);
    REQUIRE(session_total.value(Counter::DropBadReason) > 0);
    REQUIRE(session_total.value(Counter::DropUnmatchedPong) > 0);
    REQUIRE(session_total.value(Counter::DropTypeLength) > 0);  // 5.7 ROSTER 구조
}

// ---------------------------------------------------------------- 7장 출발지 위생 (b) 층

TEST_CASE("tunnel: rejected sources stop at drop_bad_source through the receive entry point",
          "[tunnel][loop]") {
    // roadmap.md Phase 4 출발지 위생 (b). EventLoop::receive_datagram 은 drain_udp 가 recvfrom
    // 뒤에 부르는 그 함수다. 세션이 있어 터널까지 가면 HELLO_ACK 이 나가는 데이터그램을 쓴다.
    const network::WsaContext wsa;
    auto opened = network::open_udp_socket();
    REQUIRE(opened.ok());
    Counters loop_counters;
    auto console = ConsoleSession::create();
    REQUIRE(console != nullptr);
    EventLoop loop(std::move(*opened.socket), loop_counters, console->queue(), console->event(),
                   LoopOptions{});
    REQUIRE(loop.valid());
    // 자기 서브넷 192.168.1.0/24. directed broadcast 는 192.168.1.255 다.
    loop.set_own_subnets(peer::OwnSubnets({peer::OwnSubnet{0xC0A8010Au, 24}}));

    TunnelFixture f;
    peer::Session* s = f.create();
    REQUIRE(s != nullptr);
    std::size_t tunnel_calls = 0;
    std::size_t stun_calls = 0;
    loop.set_tunnel_handler([&](const network::Endpoint& from, std::span<const std::byte> bytes) {
        ++tunnel_calls;
        f.tunnel.on_datagram(from, bytes);
    });
    loop.set_stun_handler([&](const network::Endpoint&, std::span<const std::byte>) { ++stun_calls; });

    // STUN 으로 분류될 바이트열 (첫 바이트 상위 2비트 00, cookie 일치)
    std::vector<std::byte> stun(20, std::byte{0});
    stun[1] = std::byte{0x01};
    stun[4] = std::byte{0x21};
    stun[5] = std::byte{0x12};
    stun[6] = std::byte{0xA4};
    stun[7] = std::byte{0x42};

    const network::Endpoint rejected[] = {
        {0xE0000001u, 5000},  // 멀티캐스트 224.0.0.1
        {0xEFFFFFFFu, 5000},  // 멀티캐스트 끝 239.255.255.255
        {0xFFFFFFFFu, 5000},  // 제한 브로드캐스트
        {0x00000000u, 5000},  // 미지정
        {0x01020304u, 0},     // 포트 0
        {0xC0A801FFu, 5000},  // 자기 서브넷의 directed broadcast
    };
    std::uint32_t seq = 0;
    for (const auto& from : rejected) {
        INFO("from=" << from.to_string());
        const auto before = loop_counters.value(Counter::DropBadSource);
        loop.receive_datagram(from, hello(kPeerEpoch, seq++, from).wire());
        loop.receive_datagram(from, stun);
        REQUIRE(loop_counters.value(Counter::DropBadSource) == before + 2);
    }
    REQUIRE(tunnel_calls == 0);
    REQUIRE(stun_calls == 0);
    REQUIRE(f.sent.empty());
    REQUIRE_FALSE(s->pinned_peer_epoch().has_value());

    // 통과하는 행: 루프백과 일반 유니캐스트, 원격 서브넷의 브로드캐스트 모양. 응답이 나간다.
    const network::Endpoint passed[] = {
        {0x7F000001u, 5000},  // 루프백
        {0x01020304u, 5000},  // 일반 유니캐스트
        {0xC0A802FFu, 5000},  // 원격 서브넷 192.168.2.255 (넷마스크를 모른다)
    };
    for (const auto& from : passed) {
        INFO("from=" << from.to_string());
        const auto sent_before = f.sent.size();
        loop.receive_datagram(from, hello(kPeerEpoch, seq++, from).wire());
        // HELLO_ACK 이 그 출발지로 나간다. 후보 밖 출발지면 그 뒤에 검증 HELLO 도 나간다 (10.4 (c)).
        REQUIRE(f.sent.size() > sent_before);
        REQUIRE(f.sent[sent_before].type() == PacketType::kHelloAck);
        REQUIRE(f.sent[sent_before].to == from);
    }
    REQUIRE(tunnel_calls == 3);
    REQUIRE(loop_counters.value(Counter::DropBadSource) == 12);
}

// ---------------------------------------------------------------- 루프의 세션 타이머 자리 (concurrency.md 4장)

TEST_CASE("tunnel: the loop waits for the session deadline and runs it", "[tunnel][loop]") {
    const network::WsaContext wsa;
    auto opened = network::open_udp_socket();
    REQUIRE(opened.ok());
    Counters loop_counters;
    auto console = ConsoleSession::create();
    REQUIRE(console != nullptr);
    EventLoop loop(std::move(*opened.socket), loop_counters, console->queue(), console->event(),
                   LoopOptions{});
    REQUIRE(loop.valid());

    const Millis start = static_cast<Millis>(platform::monotonic_ms());
    // 세션 마감이 대기 타임아웃에 들어가지 않는 구현이 영원히 매달리지 않게 2초 상한을 둔다.
    REQUIRE(loop.timers().add_once("guard", 2000, start));
    const Millis deadline = start + 100;
    std::vector<Millis> runs;
    loop.set_deadline_source(EventLoop::DeadlineSource{
        [&] { return runs.empty() ? std::optional<Millis>(deadline) : std::nullopt; },
        [&](Millis now) {
            if (now >= deadline) {
                runs.push_back(now);
            }
        },
    });
    // 대기의 타임아웃과 GetTickCount64 의 해상도(약 15ms)가 어긋나 첫 바퀴가 마감 몇 ms 전에 깰 수
    // 있다. 그때는 다음 바퀴가 타임아웃 0 근처로 다시 돈다. 그래서 바퀴 수가 아니라 걸린 시간을 본다.
    for (int i = 0; i < 50 && runs.empty(); ++i) {
        REQUIRE(loop.run_once());
    }
    const Millis elapsed = static_cast<Millis>(platform::monotonic_ms()) - start;
    REQUIRE(runs.size() == 1);
    REQUIRE(runs[0] >= deadline);
    REQUIRE(elapsed < 1000);  // 마감이 대기에 들어가지 않으면 2초 상한에서야 깬다
}

// ---------------------------------------------------------------- 카운터 표의 두 범위 (counters.hpp)

TEST_CASE("tunnel: drops before the session is known go to the process table only",
          "[tunnel][counters]") {
    TunnelFixture f;
    peer::Session* s = f.connect();
    f.feed_bytes(std::vector<std::byte>(5, std::byte{0x53}));           // 8.1 1번
    f.feed(make_rx(PacketType::kPing, {std::byte{0}}, kPeerEpoch, 9));  // 8.1 6번 (세션 앞)
    f.feed(with_peer(keepalive(kPeerEpoch, 9), 0x77777777u));           // 8.1 7번
    REQUIRE(f.count(Counter::DropShort) == 1);
    REQUIRE(f.count(Counter::DropTypeLength) == 1);
    REQUIRE(f.count(Counter::DropUnknownPeer) == 1);
    REQUIRE(s->counters().value(Counter::DropShort) == 0);
    REQUIRE(s->counters().value(Counter::DropTypeLength) == 0);
    REQUIRE(s->counters().value(Counter::DropUnknownPeer) == 0);
}

TEST_CASE("tunnel: drops after the session is known go to that session's table only",
          "[tunnel][counters]") {
    TunnelFixture f;
    peer::Session* a = f.connect();
    peer::Session* b = f.connect(0x60000001u, 0xCAFE0002u);
    const Counters process_before = f.counters;
    f.feed(keepalive(kPeerEpoch, 1));                                  // 8.3 drop_duplicate (a)
    f.feed(keepalive(kPeerEpoch2, 2));                                 // 8.2 drop_stale_epoch (a)
    f.feed(make_rx(PacketType::kRoster, {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{1},
                                         std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0},
                                         std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0},
                                         std::byte{2}, std::byte{0}, std::byte{0}, std::byte{'a'}},
                   kPeerEpoch, 3));                                   // 5.7 구조 drop_type_length (a)
    REQUIRE(a->counters().value(Counter::DropDuplicate) == 1);
    REQUIRE(a->counters().value(Counter::DropStaleEpoch) == 1);
    REQUIRE(a->counters().value(Counter::DropTypeLength) == 1);
    REQUIRE(b->counters().value(Counter::DropDuplicate) == 0);
    REQUIRE(b->counters().value(Counter::DropStaleEpoch) == 0);
    for (std::size_t i = 0; i < kCounterCount; ++i) {
        REQUIRE(f.counters.value(static_cast<Counter>(i)) ==
                process_before.value(static_cast<Counter>(i)));
    }
}

TEST_CASE("tunnel: session counter dump has one block per kept session with peer fields",
          "[tunnel][counters]") {
    TunnelFixture f;
    f.connect();
    f.connect(0x60000001u, 0xCAFE0002u)->close_by_user(f.now);  // 종료 상태로 보존된 것도 낸다
    sangtachi::testing::LogCapture logs;
    f.tunnel.emit_session_counters();
    const auto lines = logs.with_event("counter");
    REQUIRE(lines.size() == 2 * kCounterCount);
    std::size_t first = 0;
    std::size_t second = 0;
    for (const auto& line : lines) {
        REQUIRE(line.find(" peer_name=Alice") != std::string::npos);
        first += line.find(" peer_id=572662306 ") != std::string::npos ? 1 : 0;
        second += line.find(" peer_id=1610612737 ") != std::string::npos ? 1 : 0;
    }
    REQUIRE(first == kCounterCount);
    REQUIRE(second == kCounterCount);
}

TEST_CASE("tunnel: session.loss goes out with each session table", "[tunnel][counters][loss]") {
    // architecture.md 9장: session.loss 는 세션 표와 같은 때에 그 세션마다 한 줄이다.
    TunnelFixture f;
    peer::Session* s = f.connect();
    f.feed(keepalive(kPeerEpoch, 4));  // 2, 3 이 빠졌다
    f.connect(0x60000001u, 0xCAFE0002u);
    {
        sangtachi::testing::LogCapture logs;
        f.tunnel.emit_session_counters();
        const auto lines = logs.with_event("session.loss");
        REQUIRE(lines.size() == 2);
        REQUIRE(lines[0] == "INFO session.loss peer_id=572662306 expected=3 accepted=1 lost=2");
        REQUIRE(lines[1] == "INFO session.loss peer_id=1610612737 expected=0 accepted=0 lost=0");
    }
    sangtachi::testing::LogCapture logs;
    s->close_by_user(f.now);  // 종료 상태 도달 때 그 세션 하나
    const auto lines = logs.with_event("session.loss");
    REQUIRE(lines.size() == 1);
    REQUIRE(lines[0] == "INFO session.loss peer_id=572662306 expected=3 accepted=1 lost=2");
}

TEST_CASE("tunnel: a malformed peer name is logged as a dash", "[tunnel][counters]") {
    TunnelFixture f;
    tunnel::NewSession r;
    r.local_peer_id = kLocalPeerId;
    r.local_virtual_ip = kLocalVip;
    r.peer_id = kPeerId;
    r.peer_virtual_ip = kPeerVip;
    r.candidates = {kCandA};
    r.peer_name = "bad name";
    peer::Session* s = f.tunnel.create_session(r);
    REQUIRE(s != nullptr);
    REQUIRE(s->counter_peer().peer_name == "-");
}

// ---------------------------------------------------------------- 8.5 라우팅 표와 송신 측 검증

namespace {

constexpr std::uint32_t kOtherPeer = 0x60000000u;  // vip_of: 10.100.0.3

std::vector<std::byte> inner_to(std::uint32_t dst, std::uint32_t src = kLocalVip) {
    return tunnel::make_synthetic(100, src, dst, 0, 0);
}

std::size_t data_count(const TunnelFixture& f) {
    std::size_t n = 0;
    for (const auto& p : f.sent) {
        n += p.type() == PacketType::kData ? 1 : 0;
    }
    return n;
}

}  // namespace

TEST_CASE("tunnel: the 8.5 routing case table", "[tunnel][route]") {
    TunnelFixture f;
    peer::Session* host = f.connect();  // 10.100.0.1
    REQUIRE(f.tunnel.route(kPeerVip) == host);
    for (const std::uint32_t vip : {0x0A640003u, kLocalVip, 0x0A640000u, 0x0A640006u, 0x0A6400FFu,
                                    0x08080808u, 0xE0000001u, 0xFFFFFFFFu}) {
        INFO("vip=" << vip);
        REQUIRE(f.tunnel.route(vip) == nullptr);
        REQUIRE(f.tunnel.send_data(inner_to(vip)) == tunnel::TxOutcome::kDropped);
    }
    REQUIRE(f.count(Counter::TxDropNoRoute) == 8);
    REQUIRE(f.scount(Counter::TxDropNotConnected) == 0);
}

TEST_CASE("tunnel: a session is created only into an empty slot of its own", "[tunnel][route]") {
    TunnelFixture f;
    REQUIRE(f.create() != nullptr);
    REQUIRE(f.create(kOtherPeer, kPeerVip) == nullptr);      // 활성 세션이 든 칸
    REQUIRE(f.create(kOtherPeer, kLocalVip) == nullptr);     // 자신의 칸
    REQUIRE(f.create(kOtherPeer, 0x0A640006u) == nullptr);   // 1단계 밖
    REQUIRE(f.create(kOtherPeer, 0x0A640000u) == nullptr);   // 네트워크 주소. 인덱스 0 칸은 영구히 빈다
    REQUIRE(f.create(kOtherPeer, 0x08080808u) == nullptr);
    REQUIRE(f.create(kOtherPeer) != nullptr);                // 10.100.0.3
}

TEST_CASE("tunnel: a terminal session empties its slot before the terminal handler",
          "[tunnel][route]") {
    TunnelFixture f;
    peer::Session* s = f.connect();
    bool slot_was_empty = false;
    f.tunnel.set_transition_handler([&](const peer::Session&, peer::SessionState,
                                        peer::SessionState to) {
        if (peer::is_terminal(to)) {
            slot_was_empty = f.tunnel.route(kPeerVip) == nullptr;
        }
    });
    s->close_by_user(f.now);
    REQUIRE(slot_was_empty);
    // 회수된 가상 IP 를 받은 새 피어는 그 칸을 다시 쓴다. 종료 세션은 칸이 없다
    peer::Session* fresh = f.create(kOtherPeer, kPeerVip);
    REQUIRE(fresh != nullptr);
    REQUIRE(f.tunnel.route(kPeerVip) == fresh);
}

TEST_CASE("tunnel: 8.5 checks run in order and count in the right table", "[tunnel][route]") {
    SECTION("before any session the own address is unknown") {
        TunnelFixture f;
        REQUIRE(f.tunnel.send_data(inner_to(kPeerVip)) == tunnel::TxOutcome::kDropped);
        REQUIRE(f.count(Counter::TxDropBadSrc) == 1);
    }
    TunnelFixture f;
    f.connect();
    peer::Session* other = f.create(kOtherPeer);  // PUNCHING
    REQUIRE(other != nullptr);
    f.sent.clear();
    std::vector<std::byte> short_packet(19, std::byte{0x45});
    REQUIRE(f.tunnel.send_data(short_packet) == tunnel::TxOutcome::kDropped);
    auto v6 = inner_to(kPeerVip);
    v6[0] = std::byte{0x65};
    REQUIRE(f.tunnel.send_data(v6) == tunnel::TxOutcome::kDropped);
    auto oversize = tunnel::make_synthetic(protocol::kMaxInner, kLocalVip, kPeerVip, 0, 0);
    oversize.push_back(std::byte{0xA5});
    REQUIRE(f.tunnel.send_data(oversize) == tunnel::TxOutcome::kDropped);
    REQUIRE(f.tunnel.send_data(inner_to(kPeerVip, 0x0A640004u)) == tunnel::TxOutcome::kDropped);
    REQUIRE(f.tunnel.send_data(inner_to(0x0A640005u)) == tunnel::TxOutcome::kDropped);
    REQUIRE(f.tunnel.send_data(inner_to(TunnelFixture::vip_of(kOtherPeer))) ==
            tunnel::TxOutcome::kDropped);
    REQUIRE(f.count(Counter::TxDropShort) == 1);
    REQUIRE(f.count(Counter::TxDropNotIpv4) == 1);
    REQUIRE(f.count(Counter::TxDropOversize) == 1);
    REQUIRE(f.count(Counter::TxDropBadSrc) == 1);
    REQUIRE(f.count(Counter::TxDropNoRoute) == 1);
    // 결함이 겹치면 앞의 검사가 센다. 순서가 바뀌면 다른 카운터가 오른다
    std::vector<std::byte> short_v6(19, std::byte{0x65});
    REQUIRE(f.tunnel.send_data(short_v6) == tunnel::TxOutcome::kDropped);           // 1 > 2
    auto oversize_v6 = oversize;
    oversize_v6[0] = std::byte{0x65};
    REQUIRE(f.tunnel.send_data(oversize_v6) == tunnel::TxOutcome::kDropped);        // 2 > 3
    auto oversize_bad_src = tunnel::make_synthetic(protocol::kMaxInner, 0x0A640004u, 0x0A640005u, 0, 0);
    oversize_bad_src.push_back(std::byte{0xA5});
    REQUIRE(f.tunnel.send_data(oversize_bad_src) == tunnel::TxOutcome::kDropped);   // 3 > 4, 5
    REQUIRE(f.tunnel.send_data(inner_to(0x0A640005u, 0x0A640004u)) ==
            tunnel::TxOutcome::kDropped);                                            // 4 > 5
    REQUIRE(f.count(Counter::TxDropShort) == 2);
    REQUIRE(f.count(Counter::TxDropNotIpv4) == 2);
    REQUIRE(f.count(Counter::TxDropOversize) == 2);
    REQUIRE(f.count(Counter::TxDropBadSrc) == 2);
    REQUIRE(f.count(Counter::TxDropNoRoute) == 1);
    // 세션을 고른 뒤의 폐기는 그 세션의 표다
    REQUIRE(f.count(Counter::TxDropNotConnected) == 0);
    REQUIRE(other->counters().value(Counter::TxDropNotConnected) == 1);
    REQUIRE(f.sent.empty());
    // 통과하면 그 바이트열 그대로 DATA 로 나간다
    const auto inner = inner_to(kPeerVip);
    REQUIRE(f.tunnel.send_data(inner) == tunnel::TxOutcome::kSent);
    REQUIRE(f.sent.size() == 1);
    REQUIRE(f.sent[0].type() == PacketType::kData);
    REQUIRE(f.sent[0].to == kCandA);  // 학습한 peer_endpoint (10.4)
    REQUIRE(f.sent[0].payload == inner);
    f.send_ok = false;
    REQUIRE(f.tunnel.send_data(inner) == tunnel::TxOutcome::kSendFailed);
    REQUIRE(f.scount(Counter::TxErrSend) == 1);
}

TEST_CASE("tunnel: the Phase 5 boundary sizes", "[tunnel][route]") {
    // roadmap.md Phase 5 경계 크기 표. 19 는 수신 쪽 drop_type_length, 20·576·1452 는 페이로드 훅에 바이트 그대로,
    // 1453 은 송신 쪽 tx_drop_oversize 다. 왕복은 session-check 의 sizes 장면이 프로세스 둘로 본다.
    TunnelFixture f(true);
    peer::Session* s = f.connect();
    std::uint32_t seq = 2;
    {
        const auto before = f.count(Counter::DropTypeLength);
        f.feed(data_rx(kPeerEpoch, seq++, std::vector<std::byte>(19, std::byte{0x45})));
        REQUIRE(f.count(Counter::DropTypeLength) == before + 1);  // 8.1 6번이라 프로세스 표다
        REQUIRE(f.sunk.empty());
    }
    for (const std::size_t size : {std::size_t{20}, std::size_t{576}, protocol::kMaxInner}) {
        INFO("size " << size);
        const auto inner = tunnel::make_synthetic(size, kPeerVip, kLocalVip, 0, 0);
        f.feed(data_rx(kPeerEpoch, seq++, inner));
        REQUIRE(f.sunk.size() >= 1);
        REQUIRE(f.sunk.back() == inner);
    }
    REQUIRE(f.sunk.size() == 3);
    auto oversize = tunnel::make_synthetic(protocol::kMaxInner, kLocalVip, kPeerVip, 0, 0);
    oversize.push_back(std::byte{0xA5});
    f.sent.clear();
    REQUIRE(f.tunnel.send_data(oversize) == tunnel::TxOutcome::kDropped);
    REQUIRE(f.count(Counter::TxDropOversize) == 1);
    REQUIRE(f.sent.empty());
    REQUIRE(s->state() == SessionState::kConnected);
}

// ---------------------------------------------------------------- 15장 대조에서 시험이 비었던 자리 (plan.md)

TEST_CASE("tunnel: a FAILED session drops everything as drop_terminal_state", "[tunnel]") {
    // 9.4.1 은 종료 상태 둘(FAILED, CLOSED)에 같다. CLOSED 쪽은 위 시험들이 본다.
    TunnelFixture f;
    peer::Session* s = f.create();
    s->start_punch(f.now);
    f.now += 10000;
    f.tunnel.run_timers(f.now);
    REQUIRE(s->state() == SessionState::kFailed);
    f.sent.clear();
    f.feed(keepalive(kPeerEpoch, 0));
    f.feed(hello(kPeerEpoch, 1));
    REQUIRE(f.scount(Counter::DropTerminalState) == 2);
    REQUIRE(f.sent.empty());
}

TEST_CASE("tunnel: a received CLOSE empties the routing slot", "[tunnel][route]") {
    // 5.6: 받은 쪽도 라우팅 칸만 비운다. 보낸 쪽은 "a terminal session empties its slot before the terminal handler" 가 본다.
    TunnelFixture f;
    peer::Session* s = f.connect();
    REQUIRE(f.tunnel.route(kPeerVip) == s);
    f.feed(close_rx(kPeerEpoch, 2, 0x00));
    REQUIRE(s->state() == SessionState::kClosed);
    REQUIRE(f.tunnel.route(kPeerVip) == nullptr);
    REQUIRE(f.tunnel.find(kPeerId) == s);  // 세션 객체는 2분 남는다
}

TEST_CASE("tunnel: every new attempt draws its own session epoch", "[tunnel]") {
    // 4.3: session_epoch 는 세션 시도 단위다. 같은 상대의 다음 시도가 앞 시도의 값을 다시 쓰지 않는다.
    TunnelFixture f;
    peer::Session* first = f.connect();
    const std::uint32_t old_epoch = first->local_epoch();
    first->close_by_user(f.now);
    peer::Session* second = f.create();
    REQUIRE(second != nullptr);
    REQUIRE(second->local_epoch() != 0);
    REQUIRE(second->local_epoch() != old_epoch);
}

TEST_CASE("tunnel: a player has no route to another player even after a ROSTER names it", "[tunnel][route]") {
    // 8.5 케이스 표 4행. ROSTER 는 표시용이고 라우팅 칸을 채우지 않는다 (5.7). 플레이어의 칸은 호스트 하나다.
    TunnelFixture f;
    f.connect();  // 상대는 호스트다 (TunnelFixture 의 peer_is_host)
    tunnel::Roster roster;
    roster.generation = 1;
    roster.count = 2;
    roster.members[0].peer_id = kPeerId;
    roster.members[0].virtual_ip = kPeerVip;
    roster.members[0].name_len = 1;
    roster.members[0].name = {'H'};
    roster.members[0].connected = true;
    roster.members[1].peer_id = 0x60000001u;
    roster.members[1].virtual_ip = 0x0A640003u;
    roster.members[1].name_len = 1;
    roster.members[1].name = {'P'};
    roster.members[1].connected = true;
    std::vector<std::byte> buf(200);
    buf.resize(tunnel::encode_roster(roster, buf));
    REQUIRE_FALSE(buf.empty());
    f.feed(make_rx(PacketType::kRoster, buf, kPeerEpoch, 2));
    REQUIRE(f.tunnel.find(kPeerId)->last_roster_generation() == 1u);
    f.sent.clear();
    REQUIRE(f.tunnel.send_data(inner_to(0x0A640003u)) == tunnel::TxOutcome::kDropped);
    REQUIRE(f.count(Counter::TxDropNoRoute) == 1);
    REQUIRE(f.sent.empty());
}

// ---------------------------------------------------------------- 콘솔 send (roadmap.md Phase 4)

namespace {

struct SenderFixture {
    TunnelFixture f;
    tunnel::DataSender sender{f.tunnel, [this] { return f.now; }};
};

std::uint32_t number_of(const SentPacket& p) {
    std::uint32_t v = 0;
    for (std::size_t i = 28; i < 32; ++i) {
        v = (v << 8) | std::to_integer<std::uint32_t>(p.payload[i]);
    }
    return v;
}

}  // namespace

TEST_CASE("send: n packets to the only CONNECTED session, a budget per round", "[tunnel][send]") {
    SenderFixture s;
    s.f.connect();
    s.f.sent.clear();
    std::size_t observed = 0;
    s.f.tunnel.set_tx_observer([&](std::span<const std::byte>) { ++observed; });
    LogCapture logs;
    s.sender.on_command(" 100");
    REQUIRE(s.sender.busy());
    REQUIRE(s.sender.pump(64));
    REQUIRE(data_count(s.f) == 64);
    REQUIRE_FALSE(s.sender.pump(64));
    REQUIRE(data_count(s.f) == 100);
    REQUIRE(observed == 100);
    for (std::size_t i = 0; i < s.f.sent.size(); ++i) {
        REQUIRE(number_of(s.f.sent[i]) == i);  // 0 부터 세는 패킷 번호
        REQUIRE(s.f.sent[i].payload.size() == 100);
    }
    const auto done = logs.with_event("console.send");
    REQUIRE(done.size() == 1);
    REQUIRE(done[0].find(" sent=100 requested=100 ") != std::string::npos);
}

TEST_CASE("send: the command stage rejects and sends nothing", "[tunnel][send]") {
    // 거른 명령은 작업을 만들지 않는다. 앞 작업이 도는 경우만 busy 가 남는다.
    auto rejected = [](SenderFixture& s, std::string_view rest, std::string_view reason,
                       bool busy_before = false) {
        LogCapture logs;
        s.sender.on_command(rest);
        const auto lines = logs.with_event("console.rejected");
        REQUIRE(lines.size() == 1);
        REQUIRE(lines[0].find("reason=" + std::string(reason)) != std::string::npos);
        REQUIRE(s.sender.busy() == busy_before);
        if (!busy_before) {
            s.f.sent.clear();
            REQUIRE_FALSE(s.sender.pump(64));
            REQUIRE(s.f.sent.empty());
        }
    };
    SECTION("no CONNECTED session") {
        SenderFixture s;
        rejected(s, " 5", "not_connected");
    }
    SECTION("the chosen session is not CONNECTED") {
        SenderFixture s;
        s.f.connect();
        REQUIRE(s.f.create(kOtherPeer) != nullptr);
        rejected(s, " 5 10.100.0.3", "not_connected");
    }
    SECTION("a session that is not CONNECTED does not count as a destination") {
        SenderFixture s;
        s.f.connect();
        REQUIRE(s.f.create(kOtherPeer) != nullptr);  // PUNCHING
        s.f.sent.clear();
        s.sender.on_command(" 2");
        REQUIRE_FALSE(s.sender.pump(64));
        REQUIRE(data_count(s.f) == 2);  // 하나뿐인 CONNECTED 세션으로 갔다
    }
    SECTION("two CONNECTED sessions and no destination") {
        SenderFixture s;
        s.f.connect();
        s.f.connect(kOtherPeer);
        rejected(s, " 5", "ambiguous_destination");
    }
    SECTION("bad arguments") {
        SenderFixture s;
        s.f.connect();
        rejected(s, " 0", "bad_send_args");
    }
    SECTION("a job is still running") {
        SenderFixture s;
        s.f.connect();
        s.sender.on_command(" 5");
        rejected(s, " 5", "send_busy", true);
        REQUIRE_FALSE(s.sender.pump(64));
        REQUIRE(data_count(s.f) == 5);  // 앞 작업의 다섯뿐이다
    }
}

TEST_CASE("send: with a destination it picks that session, an empty slot is 8.5 no_route",
          "[tunnel][send]") {
    SenderFixture s;
    s.f.connect();
    s.f.connect(kOtherPeer);  // 10.100.0.3
    s.f.sent.clear();
    s.sender.on_command(" 3 10.100.0.3");
    REQUIRE_FALSE(s.sender.pump(64));
    REQUIRE(data_count(s.f) == 3);
    const peer::Session* other = s.f.tunnel.find(kOtherPeer);
    for (const auto& p : s.f.sent) {
        REQUIRE(p.header.session_epoch == other->local_epoch());  // 그 세션이 캡슐화했다
        REQUIRE(p.payload[19] == std::byte{3});                    // 목적지 10.100.0.3
    }
    REQUIRE(s.f.scount(Counter::TxDropNoRoute, kOtherPeer) == 0);
    LogCapture logs;
    s.sender.on_command(" 2 10.100.0.4");
    REQUIRE_FALSE(s.sender.pump(64));
    REQUIRE(data_count(s.f) == 3);
    REQUIRE(s.f.count(Counter::TxDropNoRoute) == 2);
    REQUIRE(logs.with_event("console.send")[0].find(" sent=0 requested=2 ") != std::string::npos);
}

TEST_CASE("send: a failing sendto stops the job and reports the count", "[tunnel][send]") {
    SenderFixture s;
    s.f.connect();
    s.f.sent.clear();
    LogCapture logs;
    s.sender.on_command(" 10");
    REQUIRE(s.sender.pump(3));
    s.f.send_ok = false;
    REQUIRE_FALSE(s.sender.pump(64));
    REQUIRE(data_count(s.f) == 4);  // 넷째가 실패했다
    const auto done = logs.with_event("console.send");
    REQUIRE(done.size() == 1);
    REQUIRE(done[0].rfind("WARN ", 0) == 0);
    REQUIRE(done[0].find(" sent=3 requested=10 reason=send_failed") != std::string::npos);
}

TEST_CASE("send: queued echoes go first and share the budget", "[tunnel][send]") {
    SenderFixture s;
    s.f.connect();
    s.f.sent.clear();
    s.sender.on_command(" 2");
    auto echo = tunnel::make_synthetic(100, kLocalVip, kPeerVip, 9, 0);
    echo[40] = std::byte{0x5A};
    s.sender.enqueue(echo);
    REQUIRE(s.sender.pump(2));
    REQUIRE(s.f.sent.size() == 2);
    REQUIRE(s.f.sent[0].payload == echo);
    REQUIRE(number_of(s.f.sent[1]) == 0);
    REQUIRE_FALSE(s.sender.pump(2));
}

TEST_CASE("send: the size and the clock of the command reach the bytes", "[tunnel][send]") {
    SenderFixture s;
    s.f.connect();
    s.f.sent.clear();
    s.sender.on_command(" 2 576");
    REQUIRE(s.sender.pump(1));
    s.f.now += 7;
    REQUIRE_FALSE(s.sender.pump(1));
    REQUIRE(s.f.sent.size() == 2);
    for (std::size_t i = 0; i < 2; ++i) {
        const auto& p = s.f.sent[i].payload;
        REQUIRE(p.size() == 576);
        std::uint64_t t = 0;
        for (std::size_t k = 32; k < 40; ++k) {
            t = (t << 8) | std::to_integer<std::uint64_t>(p[k]);
        }
        REQUIRE(t == kT0 + 7 * i);  // 패킷마다 그 순간의 단조 시계
    }
}

TEST_CASE("send: echoes keep their order, a failed one does not stop the rest", "[tunnel][send]") {
    SenderFixture s;
    s.f.connect();
    s.f.sent.clear();
    auto echo = [](std::uint32_t n) {
        auto e = tunnel::make_synthetic(100, kLocalVip, kPeerVip, n, 0);
        e[40] = std::byte{0x5A};
        tunnel::fill_ipv4_checksum(e);
        return e;
    };
    s.sender.enqueue(echo(1));
    s.sender.enqueue(echo(2));
    s.sender.enqueue(echo(3));
    REQUIRE(s.sender.busy());  // 작업 없이 큐만 있어도 남은 일이다
    s.f.send_ok = false;
    REQUIRE(s.sender.pump(1));
    s.f.send_ok = true;
    REQUIRE_FALSE(s.sender.pump(64));
    REQUIRE(s.f.sent.size() == 3);
    REQUIRE(number_of(s.f.sent[0]) == 1);  // 실패한 첫째도 한 번 나갔다
    REQUIRE(number_of(s.f.sent[1]) == 2);
    REQUIRE(number_of(s.f.sent[2]) == 3);
}

TEST_CASE("tunnel: the tx observer sees a packet after 8.5 and before the send", "[tunnel][send]") {
    TunnelFixture f;
    f.connect();
    f.sent.clear();
    std::vector<std::size_t> sent_at_observe;
    f.tunnel.set_tx_observer([&](std::span<const std::byte>) { sent_at_observe.push_back(f.sent.size()); });
    REQUIRE(f.tunnel.send_data(inner_to(0x0A640005u)) == tunnel::TxOutcome::kDropped);  // 8.5 에서 버림
    REQUIRE(sent_at_observe.empty());
    f.send_ok = false;
    REQUIRE(f.tunnel.send_data(inner_to(kPeerVip)) == tunnel::TxOutcome::kSendFailed);
    REQUIRE(sent_at_observe == std::vector<std::size_t>{0});  // 송신 전에 불렸다. 실패해도 남는다
}

TEST_CASE("tunnel: leaving the room forgets the own address", "[tunnel][route]") {
    TunnelFixture f;
    f.connect();
    REQUIRE(f.tunnel.local_virtual_ip() == kLocalVip);
    f.tunnel.leave_room(f.now);
    REQUIRE_FALSE(f.tunnel.local_virtual_ip().has_value());
    REQUIRE(f.tunnel.send_data(inner_to(kPeerVip)) == tunnel::TxOutcome::kDropped);
    REQUIRE(f.count(Counter::TxDropBadSrc) == 1);
}

TEST_CASE("router: assign refuses a taken slot and release empties only its own",
          "[tunnel][route]") {
    tunnel::Router r;
    auto* a = reinterpret_cast<peer::Session*>(std::uintptr_t{0x1000});
    auto* b = reinterpret_cast<peer::Session*>(std::uintptr_t{0x2000});
    REQUIRE(r.assign(0x0A640001u, a));
    REQUIRE_FALSE(r.assign(0x0A640001u, b));
    REQUIRE(r.lookup(0x0A640001u) == a);
    REQUIRE_FALSE(r.assign(0x0A640000u, b));
    REQUIRE_FALSE(r.assign(0x0A640006u, b));
    REQUIRE(r.assign(0x0A640005u, b));
    r.release(a);
    REQUIRE(r.lookup(0x0A640001u) == nullptr);
    REQUIRE(r.lookup(0x0A640005u) == b);
}

// ---------------------------------------------------------------- 5.7 호스트의 ROSTER 송신 (3차 I)

namespace {

const tunnel::RosterPeer kMe{kLocalPeerId, kLocalVip, "Me"};

std::vector<SentPacket> rosters(const TunnelFixture& f) {
    std::vector<SentPacket> out;
    for (const auto& p : f.sent) {
        if (p.type() == PacketType::kRoster) {
            out.push_back(p);
        }
    }
    return out;
}

tunnel::Roster decoded(const SentPacket& p) {
    const auto d = tunnel::decode_roster(p.payload, tunnel::RosterContext{true, std::nullopt});
    REQUIRE(d.ok());
    return d.value;
}

// 방을 세우고(0 번), 상대 하나를 명부에 올린다(1 번). 아직 CONNECTED 세션이 없어 아무것도 안 나간다.
void host_with_alice(TunnelFixture& f) {
    f.tunnel.start_roster(kMe);
    f.tunnel.run_timers(f.now);
    REQUIRE(f.tunnel.roster().generation() == 0);
    f.tunnel.set_roster_peers({tunnel::RosterPeer{kPeerId, kPeerVip, "Alice"}});
    f.tunnel.run_timers(f.now);
    REQUIRE(f.tunnel.roster().generation() == 1);
    REQUIRE(rosters(f).empty());
}

}  // namespace

TEST_CASE("roster: entering CONNECTED is one change and one send", "[tunnel][roster]") {
    TunnelFixture f;
    host_with_alice(f);
    f.connect();
    // 상태 전이는 표시만 한다. 루프가 같은 바퀴에 돌도록 마감이 지금이다
    REQUIRE(f.tunnel.earliest_deadline() <= f.now);
    REQUIRE(rosters(f).empty());
    f.tunnel.run_timers(f.now);
    const auto sent = rosters(f);
    REQUIRE(sent.size() == 1);  // 11장의 진입 1회와 5.7 의 변경 1회가 같은 한 번이다
    REQUIRE(sent[0].to == kCandA);  // 10.4: 학습한 peer_endpoint
    const auto r = decoded(sent[0]);
    REQUIRE(r.generation == 2);
    REQUIRE(r.count == 2);
    REQUIRE(r.members[0].peer_id == kLocalPeerId);
    REQUIRE(r.members[0].connected);
    REQUIRE(r.members[0].name_view() == "Me");
    REQUIRE(r.members[1].peer_id == kPeerId);
    REQUIRE(r.members[1].virtual_ip == kPeerVip);
    REQUIRE(r.members[1].connected);
    REQUIRE(r.members[1].name_view() == "Alice");
}

TEST_CASE("roster: a change is resent twice 5s apart, then every 15s", "[tunnel][roster]") {
    TunnelFixture f;
    host_with_alice(f);
    f.connect();
    const Millis t = f.now;
    f.tunnel.run_timers(t);
    sangtachi::testing::LogCapture logs;
    auto count_at = [&](Millis now) {
        f.now = now;
        f.tunnel.run_timers(now);
        return rosters(f).size();
    };
    REQUIRE(count_at(t + 4999) == 1);
    REQUIRE(count_at(t + 5000) == 2);
    REQUIRE(count_at(t + 9999) == 2);
    REQUIRE(count_at(t + 10000) == 3);
    REQUIRE(count_at(t + 24999) == 3);   // 주기는 마지막 송신(재송)부터 15초다
    REQUIRE(count_at(t + 25000) == 4);
    REQUIRE(count_at(t + 40000) == 5);
    for (const auto& p : rosters(f)) {
        REQUIRE(decoded(p).generation == 2);  // 같은 내용이다
    }
    const auto ticks = logs.with_event("timer.tick");
    std::size_t resend = 0;
    std::size_t period = 0;
    for (const auto& line : ticks) {
        resend += line.find(" name=roster.resend." + std::to_string(kPeerId) + " ") != std::string::npos;
        period += line.find(" name=roster.period." + std::to_string(kPeerId) + " ") != std::string::npos;
    }
    REQUIRE(resend == 2);
    REQUIRE(period == 2);
}

TEST_CASE("roster: the same content does not count as a change", "[tunnel][roster]") {
    TunnelFixture f;
    host_with_alice(f);
    f.connect();
    f.tunnel.run_timers(f.now);
    f.sent.clear();
    f.tunnel.set_roster_peers({tunnel::RosterPeer{kPeerId, kPeerVip, "Alice"}});
    f.tunnel.run_timers(f.now + 1);
    REQUIRE(rosters(f).empty());
    REQUIRE(f.tunnel.roster().generation() == 2);
    // 비트 0 과 무관한 전이(PUNCHING -> HANDSHAKING)는 변화가 아니다
    f.tunnel.set_roster_peers({tunnel::RosterPeer{kPeerId, kPeerVip, "Alice"},
                               tunnel::RosterPeer{kOtherPeer, TunnelFixture::vip_of(kOtherPeer), "Bob"}});
    f.tunnel.run_timers(f.now + 1);
    REQUIRE(f.tunnel.roster().generation() == 3);  // Bob 이 명부에 들었다
    f.sent.clear();
    peer::Session* bob = f.create(kOtherPeer);
    bob->start_punch(f.now);
    f.feed(with_peer(hello(kPeerEpoch, 0, kCandA, TunnelFixture::vip_of(kOtherPeer)), kOtherPeer));
    REQUIRE(bob->state() == SessionState::kHandshaking);
    f.tunnel.run_timers(f.now + 2);
    REQUIRE(f.tunnel.roster().generation() == 3);
    REQUIRE(rosters(f).empty());
    // 이름이 바뀌면 바뀐 것이다
    f.tunnel.set_roster_peers({tunnel::RosterPeer{kPeerId, kPeerVip, "Alicia"},
                               tunnel::RosterPeer{kOtherPeer, TunnelFixture::vip_of(kOtherPeer), "Bob"}});
    f.tunnel.run_timers(f.now + 3);
    REQUIRE(rosters(f).size() == 1);
    REQUIRE(decoded(rosters(f)[0]).generation == 4);
    REQUIRE(decoded(rosters(f)[0]).members[1].name_view() == "Alicia");
}

TEST_CASE("roster: a CONNECTED session outside the member list gets one on entry",
          "[tunnel][roster]") {
    TunnelFixture f;
    f.tunnel.start_roster(kMe);
    f.tunnel.run_timers(f.now);
    f.connect();  // 명부에 없다. 내용은 그대로 호스트 혼자다
    f.tunnel.run_timers(f.now);
    REQUIRE(rosters(f).size() == 1);
    REQUIRE(decoded(rosters(f)[0]).generation == 0);
    REQUIRE(decoded(rosters(f)[0]).count == 1);
    f.tunnel.run_timers(f.now + 14999);
    REQUIRE(rosters(f).size() == 1);  // 변경이 아니므로 재송이 없다
    f.tunnel.run_timers(f.now + 15000);
    REQUIRE(rosters(f).size() == 2);
    // 재협상으로 CONNECTED 를 떠났다가 돌아오면 다시 진입이다. 같은 바퀴 안이어도 한 번
    f.now += 20000;
    f.feed(hello(kPeerEpoch2, 0));
    REQUIRE(f.tunnel.find(kPeerId)->state() != SessionState::kConnected);
    f.feed(hello_ack(kPeerEpoch2, 1, kCandA, kPeerVip, f.tunnel.find(kPeerId)->attempt_nonce()));
    REQUIRE(f.tunnel.find(kPeerId)->state() == SessionState::kConnected);
    f.tunnel.run_timers(f.now);
    REQUIRE(rosters(f).size() == 3);
}

TEST_CASE("roster: a session leaving CONNECTED stops its timers and the others hear of it",
          "[tunnel][roster]") {
    TunnelFixture f;
    f.tunnel.start_roster(kMe);
    f.tunnel.set_roster_peers({tunnel::RosterPeer{kPeerId, kPeerVip, "Alice"},
                               tunnel::RosterPeer{kOtherPeer, TunnelFixture::vip_of(kOtherPeer), "Bob"}});
    f.tunnel.run_timers(f.now);
    peer::Session* alice = f.connect();
    peer::Session* bob = f.connect(kOtherPeer);
    f.tunnel.run_timers(f.now);
    const std::uint32_t before = f.tunnel.roster().generation();
    f.sent.clear();
    alice->close_by_user(f.now);
    f.tunnel.run_timers(f.now);
    auto sent = rosters(f);
    REQUIRE(sent.size() == 1);  // bob 에게만
    REQUIRE(sent[0].header.session_epoch == bob->local_epoch());
    const auto r = decoded(sent[0]);
    REQUIRE(r.generation == before + 1);
    REQUIRE_FALSE(r.members[1].connected);  // alice
    REQUIRE(r.members[2].connected);        // bob
    // 끝난 세션에는 그 뒤로도 보내지 않는다
    f.sent.clear();
    f.tunnel.run_timers(f.now + 60000);
    for (const auto& p : rosters(f)) {
        REQUIRE(p.header.session_epoch == bob->local_epoch());
    }
}

TEST_CASE("roster: a player sends none and leaving the room stops the host", "[tunnel][roster]") {
    SECTION("player") {
        TunnelFixture f;
        f.connect();
        for (Millis dt = 0; dt <= 60000; dt += 5000) {
            f.tunnel.run_timers(f.now + dt);
        }
        REQUIRE(rosters(f).empty());
        REQUIRE_FALSE(f.tunnel.roster().active());
    }
    SECTION("host leaves") {
        TunnelFixture f;
        host_with_alice(f);
        f.tunnel.leave_room(f.now);
        REQUIRE_FALSE(f.tunnel.roster().active());
        REQUIRE_FALSE(f.tunnel.earliest_deadline().has_value());
    }
}

TEST_CASE("roster: more members than MAX_PEERS are not sent", "[tunnel][roster]") {
    TunnelFixture f;
    f.tunnel.start_roster(kMe);
    std::vector<tunnel::RosterPeer> peers{{kPeerId, kPeerVip, "Alice"}};
    for (std::uint32_t i = 0; i < 4; ++i) {
        peers.push_back({0x50000000u + i, 0x0A640003u, "P" + std::to_string(i)});
    }
    f.tunnel.set_roster_peers(peers);  // 호스트까지 여섯
    f.tunnel.run_timers(f.now);
    f.connect();
    f.tunnel.run_timers(f.now);
    REQUIRE(rosters(f).empty());
}

TEST_CASE("roster: peers set before the first run are already a change", "[tunnel][roster]") {
    TunnelFixture f;
    f.tunnel.start_roster(kMe);
    f.tunnel.set_roster_peers({tunnel::RosterPeer{kPeerId, kPeerVip, "Alice"}});
    f.tunnel.run_timers(f.now);
    REQUIRE(f.tunnel.roster().generation() == 1);  // 0 번은 호스트 혼자였다
}

TEST_CASE("roster: a member leaving and coming back in one round is still a change",
          "[tunnel][roster]") {
    TunnelFixture f;
    host_with_alice(f);
    f.connect();
    f.tunnel.run_timers(f.now);
    const auto before = f.tunnel.roster().generation();
    f.sent.clear();
    f.now += 2000;
    // 재협상과 그 ACK 이 같은 바퀴에 온다. run_timers 사이에 CONNECTED 를 떠났다 돌아온다
    f.feed(hello(kPeerEpoch2, 0));
    f.feed(hello_ack(kPeerEpoch2, 1, kCandA, kPeerVip, f.tunnel.find(kPeerId)->attempt_nonce()));
    REQUIRE(f.tunnel.find(kPeerId)->state() == SessionState::kConnected);
    f.tunnel.run_timers(f.now);
    REQUIRE(f.tunnel.roster().generation() == before + 1);
    REQUIRE(rosters(f).size() == 1);
    REQUIRE(f.tunnel.roster().earliest() == f.now + 5000);  // 새 재송이 걸렸다
}

TEST_CASE("roster: a new change replaces the pending resends, for every CONNECTED session",
          "[tunnel][roster]") {
    TunnelFixture f;
    f.tunnel.start_roster(kMe);
    f.tunnel.set_roster_peers({tunnel::RosterPeer{kPeerId, kPeerVip, "Alice"},
                               tunnel::RosterPeer{kOtherPeer, TunnelFixture::vip_of(kOtherPeer), "Bob"}});
    f.tunnel.run_timers(f.now);
    f.connect();
    f.connect(kOtherPeer);
    const Millis t0 = f.now;
    f.tunnel.run_timers(t0);
    f.tunnel.run_timers(t0 + 5000);  // 첫 변경의 재송 하나
    f.sent.clear();
    // 둘째 변경. 이름을 바꾼다
    f.tunnel.set_roster_peers({tunnel::RosterPeer{kPeerId, kPeerVip, "Alicia"},
                               tunnel::RosterPeer{kOtherPeer, TunnelFixture::vip_of(kOtherPeer), "Bob"}});
    const Millis t1 = t0 + 6000;
    f.tunnel.run_timers(t1);
    REQUIRE(rosters(f).size() == 2);  // 두 세션에 즉시 하나씩
    for (const auto& p : rosters(f)) {
        REQUIRE(decoded(p).members[1].name_view() == "Alicia");
    }
    REQUIRE(f.tunnel.roster().earliest() == t1 + 5000);
    f.tunnel.run_timers(t0 + 10000);  // 옛 재송의 둘째 자리. 지워졌다
    REQUIRE(rosters(f).size() == 2);
    f.tunnel.run_timers(t1 + 5000);
    f.tunnel.run_timers(t1 + 10000);
    REQUIRE(rosters(f).size() == 6);  // 세션마다 즉시 1 + 재송 2
    f.tunnel.run_timers(t1 + 24999);
    REQUIRE(rosters(f).size() == 6);
}

TEST_CASE("roster: timer.tick carries the elapsed time of each roster timer", "[tunnel][roster]") {
    TunnelFixture f;
    host_with_alice(f);
    f.connect();
    const Millis t = f.now;
    f.tunnel.run_timers(t);
    sangtachi::testing::LogCapture logs;
    f.tunnel.run_timers(t + 5003);   // 늦게 돈 재송
    f.tunnel.run_timers(t + 10003);
    f.tunnel.run_timers(t + 25010);  // 주기. 마지막 송신(t + 10003)부터
    const auto ticks = logs.with_event("timer.tick");
    std::vector<std::string> roster_ticks;
    for (const auto& line : ticks) {
        if (line.find(" name=roster.") != std::string::npos) {
            roster_ticks.push_back(line);
        }
    }
    REQUIRE(roster_ticks.size() == 3);
    REQUIRE(roster_ticks[0].find("roster.resend.") != std::string::npos);
    REQUIRE(roster_ticks[0].find(" elapsed_ms=5003") != std::string::npos);
    REQUIRE(roster_ticks[1].find(" elapsed_ms=5000") != std::string::npos);
    REQUIRE(roster_ticks[2].find("roster.period.") != std::string::npos);
    REQUIRE(roster_ticks[2].find(" elapsed_ms=15007") != std::string::npos);
}

TEST_CASE("roster: a new room starts from zero with nothing left over", "[tunnel][roster]") {
    TunnelFixture f;
    host_with_alice(f);
    f.connect();
    f.tunnel.run_timers(f.now);
    REQUIRE(f.tunnel.roster().generation() == 2);
    f.tunnel.leave_room(f.now);
    REQUIRE_FALSE(f.tunnel.roster().earliest().has_value());
    f.tunnel.start_roster(kMe);
    f.tunnel.run_timers(f.now);
    REQUIRE(f.tunnel.roster().generation() == 0);
    f.sent.clear();
    f.tunnel.run_timers(f.now + 60000);
    REQUIRE(rosters(f).empty());
}

// ---------------------------------------------------------------- 폐기 목록은 프로세스에 하나 (4.3)

TEST_CASE("tunnel: renegotiations of different sessions share one retired list and its cap",
          "[tunnel]") {
    TunnelFixture f;
    peer::Session* a = f.connect(0x70000001u, 0xA0000000u);
    peer::Session* b = f.connect(0x70000002u, 0xB0000000u);
    peer::Session* c = f.connect(0x70000003u, 0xC0000000u);
    // a 와 b 가 8번씩 재협상한다. 등록되는 것은 각자 직전 상대 epoch 다 (9.5 1번).
    for (std::uint32_t i = 1; i <= 8; ++i) {
        f.now += 1000;
        f.feed(with_peer(hello(0xA0000000u + i, 0, kCandA, TunnelFixture::vip_of(0x70000001u)),
                         0x70000001u));
        f.feed(with_peer(hello(0xB0000000u + i, 0, kCandA, TunnelFixture::vip_of(0x70000002u)),
                         0x70000002u));
    }
    REQUIRE(a->renegotiations() == 8);
    REQUIRE(b->renegotiations() == 8);
    auto& retired = f.tunnel.retired_epochs();
    REQUIRE(retired.size() == 16);
    REQUIRE(retired.contains(0xA0000000u, f.now));
    REQUIRE(retired.contains(0xB0000007u, f.now));
    // 셋째 세션의 재협상 하나가 17번째다. 가장 오래된 a 의 첫 epoch 가 밀려난다.
    f.now += 1000;
    f.feed(with_peer(hello(0xC0000001u, 0, kCandA, TunnelFixture::vip_of(0x70000003u)), 0x70000003u));
    REQUIRE(c->renegotiations() == 1);
    REQUIRE(retired.size() == 16);
    REQUIRE_FALSE(retired.contains(0xA0000000u, f.now));
    REQUIRE(retired.contains(0xC0000000u, f.now));
    // 다른 세션이 등록한 epoch 도 이 세션 앞 패킷을 거른다. 목록이 하나이기 때문이다.
    f.feed(with_peer(keepalive(0xB0000001u, 50), 0x70000001u));
    REQUIRE(a->counters().value(Counter::DropRetiredEpoch) == 1);
}

// ---------------------------------------------------------------- 로비로 갈 때의 두 표

TEST_CASE("tunnel: leaving the room keeps the process table and drops the session tables",
          "[tunnel][counters]") {
    TunnelFixture f;
    peer::Session* s = f.connect();
    f.feed(with_peer(keepalive(kPeerEpoch, 9), 0x77777777u));  // 프로세스 표
    f.feed(keepalive(kPeerEpoch, 1));                          // 세션 표 drop_duplicate
    REQUIRE(s->counters().value(Counter::DropDuplicate) == 1);
    f.tunnel.leave_room(f.now);
    REQUIRE(f.tunnel.session_count() == 0);
    REQUIRE(f.count(Counter::DropUnknownPeer) == 1);
    REQUIRE(f.count(Counter::DropDuplicate) == 0);
    sangtachi::testing::LogCapture logs;
    f.tunnel.emit_session_counters();
    REQUIRE(logs.with_event("counter").empty());
}

TEST_CASE("tunnel: a failed replacement keeps the retained terminal session", "[tunnel]") {
    TunnelFixture f;
    peer::Session* old = f.connect();
    old->close_by_user(f.now);
    f.random_ok = false;
    REQUIRE(f.create() == nullptr);
    REQUIRE(f.tunnel.find(kPeerId) == old);
    f.feed(keepalive(kPeerEpoch2, 7));
    REQUIRE(old->counters().value(Counter::DropTerminalState) == 1);
    REQUIRE(f.count(Counter::DropUnknownPeer) == 0);
}

TEST_CASE("tunnel: the console counters command emits the process table then each session table",
          "[tunnel][loop][counters]") {
    const network::WsaContext wsa;
    auto opened = network::open_udp_socket();
    REQUIRE(opened.ok());
    Counters loop_counters;
    auto console = ConsoleSession::create();
    REQUIRE(console != nullptr);
    EventLoop loop(std::move(*opened.socket), loop_counters, console->queue(), console->event(),
                   LoopOptions{});
    REQUIRE(loop.valid());
    TunnelFixture f;
    f.connect();
    f.connect(0x60000001u, 0xCAFE0002u)->close_by_user(f.now);
    loop.set_counter_dump_handler([&f] { f.tunnel.emit_session_counters(); });

    sangtachi::testing::LogCapture logs;
    REQUIRE(console->queue().try_push("counters"));
    REQUIRE(platform::signal_event(console->event()));
    REQUIRE(loop.run_once());
    const auto lines = logs.with_event("counter");
    REQUIRE(lines.size() == 3 * kCounterCount);
    std::size_t with_peer = 0;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const bool has_peer = lines[i].find(" peer_id=") != std::string::npos;
        REQUIRE(has_peer == (i >= kCounterCount));  // 프로세스 표가 먼저, 줄에 peer 필드가 없다
        with_peer += has_peer ? 1 : 0;
    }
    REQUIRE(with_peer == 2 * kCounterCount);
}

TEST_CASE("tunnel: a replacement whose nonce draw fails keeps the retained session", "[tunnel]") {
    // epoch 4바이트는 뽑히고 nonce 16바이트에서 실패한다. 지우기는 둘 다 성공한 뒤다.
    TunnelFixture f;
    peer::Session* old = f.connect();
    old->close_by_user(f.now);
    f.random_bytes_left = 4;
    REQUIRE(f.create() == nullptr);
    REQUIRE(f.tunnel.find(kPeerId) == old);
}

// ---------------------------------------------------------------- 수신이 타이머보다 먼저 (protocol.md 11장)

TEST_CASE("tunnel: a valid packet queued at the idle deadline is handled before the session timers",
          "[tunnel][loop]") {
    // 11장 "같은 바퀴에 꺼낸 수신 이벤트가 그 바퀴의 타이머 만료보다 먼저 처리된다". 유휴 마감
    // 시각에 유효한 KEEPALIVE 가 큐에 있으면 그것이 유휴를 갱신하고 단절 판정은 나지 않는다.
    const network::WsaContext wsa;
    auto receiver = network::open_udp_socket();
    auto sender = network::open_udp_socket();
    REQUIRE(receiver.ok());
    REQUIRE(sender.ok());
    const network::Endpoint to(0x7F000001u, receiver.socket->local().port());
    Counters loop_counters;
    auto console = ConsoleSession::create();
    REQUIRE(console != nullptr);
    EventLoop loop(std::move(*receiver.socket), loop_counters, console->queue(), console->event(),
                   LoopOptions{});
    REQUIRE(loop.valid());

    // 터널은 주입 시계(f.now)로 돈다. 루프가 넘기는 실제 시각 대신 같은 주입 시계로 세션 타이머를
    // 돌린다. 그래야 수신과 타이머가 같은 시각을 본다.
    TunnelFixture f;
    peer::Session* s = f.connect();
    const Millis connected_at = f.now;
    REQUIRE(s->idle_deadline() == connected_at + 50000);
    loop.set_tunnel_handler([&f](const network::Endpoint& from, std::span<const std::byte> bytes) {
        f.tunnel.on_datagram(from, bytes);
    });
    const Millis real = static_cast<Millis>(platform::monotonic_ms());
    loop.set_deadline_source(EventLoop::DeadlineSource{
        [real] { return std::optional<Millis>(real + 2000); },  // 대기는 소켓 신호로 깬다
        [&f](Millis) { f.tunnel.run_timers(f.now); },
    });

    f.now = connected_at + 50000;  // 유휴 마감이 정확히 지금이다 (deadline <= now 면 만료)
    REQUIRE(sender.socket->send_to(to, keepalive(kPeerEpoch, 2).wire()).ok);
    REQUIRE(loop.run_once());

    REQUIRE(s->state() == SessionState::kConnected);
    REQUIRE_FALSE(s->failure().has_value());
    REQUIRE(s->idle_deadline() == connected_at + 50000 + 50000);
    REQUIRE(f.count(Counter::DropUnknownPeer) == 0);
}
