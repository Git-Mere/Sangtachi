#include "sangtachi/tunnel/tunnel.hpp"

#include "../log_capture.hpp"
#include "../session_fixture.hpp"

#include "sangtachi/console.hpp"
#include "sangtachi/loop.hpp"
#include "sangtachi/network/udp_socket.hpp"
#include "sangtachi/network/wsa.hpp"
#include "sangtachi/peer/candidates.hpp"
#include "sangtachi/platform/wait.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
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
    std::uint8_t next_random = 1;
    bool random_ok = true;
    std::size_t random_bytes_left = static_cast<std::size_t>(-1);  // 이만큼 내준 뒤 실패한다
    bool with_sink = false;
    std::vector<std::vector<std::byte>> sunk;
    std::vector<std::uint32_t> terminal_peers;
    Tunnel tunnel;

    explicit TunnelFixture(bool sink = false) : with_sink(sink), tunnel(counters, deps()) {
        tunnel.set_terminal_handler(
            [this](const peer::Session& s) { terminal_peers.push_back(s.peer_id()); });
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
            return true;
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

    peer::Session* create(std::uint32_t peer_id = kPeerId, std::uint32_t peer_vip = kPeerVip) {
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
        Rx h = hello(epoch, 0);
        h.header.peer_id = peer_id;
        feed(h);
        Rx a = hello_ack(epoch, 1, kCandA, kPeerVip, s->attempt_nonce());
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
    feed_roster(roster_bytes(1, {name_of(1)}));
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
        REQUIRE(f.sent.size() == sent_before + 1);
        REQUIRE(f.sent.back().to == from);
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
        f.feed(with_peer(hello(0xA0000000u + i, 0), 0x70000001u));
        f.feed(with_peer(hello(0xB0000000u + i, 0), 0x70000002u));
    }
    REQUIRE(a->renegotiations() == 8);
    REQUIRE(b->renegotiations() == 8);
    auto& retired = f.tunnel.retired_epochs();
    REQUIRE(retired.size() == 16);
    REQUIRE(retired.contains(0xA0000000u, f.now));
    REQUIRE(retired.contains(0xB0000007u, f.now));
    // 셋째 세션의 재협상 하나가 17번째다. 가장 오래된 a 의 첫 epoch 가 밀려난다.
    f.now += 1000;
    f.feed(with_peer(hello(0xC0000001u, 0), 0x70000003u));
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
