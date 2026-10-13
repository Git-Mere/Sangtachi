#include "sangtachi/tunnel/tunnel.hpp"

#include "sangtachi/log.hpp"
#include "sangtachi/protocol_constants.hpp"

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace sangtachi::tunnel {
namespace {

constexpr std::uint8_t at(std::span<const std::byte> bytes, std::size_t offset) noexcept {
    return std::to_integer<std::uint8_t>(bytes[offset]);
}

constexpr std::uint32_t be32_at(std::span<const std::byte> bytes, std::size_t offset) noexcept {
    return (static_cast<std::uint32_t>(at(bytes, offset)) << 24) |
           (static_cast<std::uint32_t>(at(bytes, offset + 1)) << 16) |
           (static_cast<std::uint32_t>(at(bytes, offset + 2)) << 8) |
           static_cast<std::uint32_t>(at(bytes, offset + 3));
}

}  // namespace

std::optional<Counter> to_counter(PacketError error) noexcept {
    switch (error) {
        case PacketError::kNone: return std::nullopt;
        case PacketError::kShort: return Counter::DropShort;
        case PacketError::kMagic: return Counter::DropMagic;
        case PacketError::kVersion: return Counter::DropVersion;
        case PacketError::kUnknownType: return Counter::DropUnknownType;
        case PacketError::kLength: return Counter::DropLength;
        case PacketError::kTypeLength: return Counter::DropTypeLength;
        case PacketError::kBadReason: return Counter::DropBadReason;
        case PacketError::kRosterDirection: return Counter::DropRosterDirection;
        case PacketError::kRosterStale: return Counter::DropRosterStale;
        case PacketError::kRosterFlags: return Counter::DropRosterFlags;
        case PacketError::kRosterName: return Counter::DropRosterName;
    }
    return std::nullopt;
}

InnerError check_inner_ipv4(std::span<const std::byte> payload, std::uint32_t peer_virtual_ip,
                            std::uint32_t local_virtual_ip) noexcept {
    // 10. 버전 니블
    if (payload.empty() || (at(payload, 0) >> 4) != 4) {
        return InnerError::kNotIpv4;
    }
    // 11. IHL 5~15 이고 IHL*4 <= payload_length. 이것이 아래 읽기(오프셋 0~20)의 경계를 보장한다.
    const std::size_t ihl = at(payload, 0) & 0x0F;
    if (ihl < 5 || ihl * 4 > payload.size()) {
        return InnerError::kIhl;
    }
    // 12. 헤더 체크섬. 체크섬 칸을 포함해 16비트 단어의 1의 보수 합이 0xFFFF 면 유효하다.
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i < ihl * 4; i += 2) {
        sum += (static_cast<std::uint32_t>(at(payload, i)) << 8) | at(payload, i + 1);
    }
    while ((sum >> 16) != 0) {
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }
    if (sum != 0xFFFFu) {
        return InnerError::kChecksum;
    }
    // 13. total_length == payload_length
    const std::size_t total = (static_cast<std::size_t>(at(payload, 2)) << 8) | at(payload, 3);
    if (total != payload.size()) {
        return InnerError::kLength;
    }
    // 14, 15. 위생 장치다. 공격 방어 수단이 아니다 (8.4, 2장).
    if (be32_at(payload, 12) != peer_virtual_ip) {
        return InnerError::kSrc;
    }
    if (be32_at(payload, 16) != local_virtual_ip) {
        return InnerError::kDst;
    }
    return InnerError::kNone;
}

std::optional<Counter> to_counter(InnerError error) noexcept {
    switch (error) {
        case InnerError::kNone: return std::nullopt;
        case InnerError::kNotIpv4: return Counter::DropInnerNotIpv4;
        case InnerError::kIhl: return Counter::DropInnerIhl;
        case InnerError::kChecksum: return Counter::DropInnerChecksum;
        case InnerError::kLength: return Counter::DropInnerLength;
        case InnerError::kSrc: return Counter::DropInnerSrc;
        case InnerError::kDst: return Counter::DropInnerDst;
    }
    return std::nullopt;
}

// ---------------------------------------------------------------- Tunnel

Tunnel::Tunnel(Counters& counters, TunnelDeps deps) : counters_(counters), deps_(std::move(deps)) {}

Millis Tunnel::now() const {
    return deps_.clock ? deps_.clock() : 0;
}

peer::Session* Tunnel::find(std::uint32_t peer_id) noexcept {
    const auto it = sessions_.find(peer_id);
    return it == sessions_.end() ? nullptr : it->second.session.get();
}

std::size_t Tunnel::terminal_count() const noexcept {
    std::size_t n = 0;
    for (const auto& [id, entry] : sessions_) {
        if (!entry.session->active()) {
            ++n;
        }
    }
    return n;
}

peer::Session* Tunnel::create_session(const NewSession& request) {
    const Millis t = now();
    const auto existing = sessions_.find(request.peer_id);
    if (existing != sessions_.end() && existing->second.session->active()) {
        return nullptr;  // 같은 상대의 활성 세션 위에 새 시도를 겹치지 않는다
    }
    // 8.5: 칸이 있어야 하고 비어 있어야 한다. 자신의 칸은 늘 비어 있다.
    if (!route_index(request.peer_virtual_ip) ||
        request.peer_virtual_ip == request.local_virtual_ip ||
        routes_.lookup(request.peer_virtual_ip) != nullptr) {
        return nullptr;
    }

    // 새 시도의 시작이다. 4.3 등록 표 첫 행과 5.1 시도 nonce 폐기를 여기서 한다 (머리 표).
    for (auto& [id, entry] : sessions_) {
        if (entry.session->active()) {
            continue;
        }
        if (!entry.epoch_retired) {
            if (const auto pinned = entry.session->pinned_peer_epoch()) {
                retired_.add(*pinned, t);  // 상대의 epoch 다 (4.3)
            }
            entry.epoch_retired = true;
        }
        if (!entry.nonce_retired) {
            retired_nonces_.add(entry.session->attempt_nonce(), t);
            entry.nonce_retired = true;
        }
    }
    for (const auto& nonce : ended_nonces_) {
        retired_nonces_.add(nonce, t);
    }
    ended_nonces_.clear();

    // 뽑기가 등록 뒤인 것은 새 nonce 가 방금 등록한 nonce 와 겹치지 않게 하려는 것이다 (5.1).
    // 실패하면 시도가 시작되지 않았으므로 남긴 종료 세션을 지우지 않는다 (5.6 보존은 그대로다).
    const auto epoch = peer::generate_session_epoch(deps_.random);
    if (!epoch) {
        return nullptr;
    }
    const auto nonce = peer::generate_attempt_nonce(retired_nonces_, t, deps_.random);
    if (!nonce) {
        return nullptr;
    }
    if (existing != sessions_.end()) {
        sessions_.erase(existing);  // 등록을 마친 종료 세션이다
    }

    peer::SessionConfig config;
    config.local_peer_id = request.local_peer_id;
    config.local_virtual_ip = request.local_virtual_ip;
    config.peer_id = request.peer_id;
    config.peer_virtual_ip = request.peer_virtual_ip;
    config.candidates = request.candidates;
    config.peer_is_host = request.peer_is_host;
    // counter 줄의 peer_name. 형식에 어긋난 값을 로그에 싣지 않는다 (control_plane.md 2.7).
    config.peer_name = is_valid_name(request.peer_name) ? request.peer_name : std::string("-");
    config.local_epoch = *epoch;
    config.attempt_nonce = *nonce;

    peer::SessionIo io;
    io.send = deps_.send;
    io.retired = &retired_;
    io.rtt_clock_us = deps_.rtt_clock_us;
    io.data_sink = deps_.data_sink;
    io.random = deps_.random;  // probe nonce (5.1)
    io.on_transition = [this](const peer::Session& session, peer::SessionState from,
                              peer::SessionState to) { on_transition(session, from, to); };

    Entry entry;
    entry.session = std::make_unique<peer::Session>(std::move(config), std::move(io), t);
    peer::Session* raw = entry.session.get();
    sessions_[request.peer_id] = std::move(entry);
    (void)routes_.assign(request.peer_virtual_ip, raw);  // 위에서 비어 있음을 봤다
    local_vip_ = request.local_virtual_ip;
    return raw;
}

void Tunnel::on_transition(const peer::Session& session, peer::SessionState from,
                           peer::SessionState to) {
    // 5.7: 비트 0 은 CONNECTED 여부다. 같은 바퀴의 run_timers 가 보낸다.
    if ((from == peer::SessionState::kConnected) != (to == peer::SessionState::kConnected)) {
        roster_.on_connected_change(session.peer_id(), to == peer::SessionState::kConnected);
    }
    if (!peer::is_terminal(to)) {
        if (on_transition_) {
            on_transition_(session, from, to);
        }
        return;
    }
    // 8.5: 라우팅 칸을 먼저 비운다. 기록 줄과 카운터는 그 뒤다.
    routes_.release(&session);
    if (on_transition_) {
        on_transition_(session, from, to);
    }
    // architecture.md 9장: 세션이 종료 상태에 닿을 때 그 세션의 표 전량.
    const CounterPeer peer = session.counter_peer();
    emit_all(session.counters(), &peer);
    // 여기서 세션을 지우지 않는다. 이 콜백은 그 세션의 메서드 안에서 불린다.
}

void Tunnel::erase(std::map<std::uint32_t, Entry>::iterator it) {
    if (!it->second.nonce_retired) {
        ended_nonces_.push_back(it->second.session->attempt_nonce());
    }
    sessions_.erase(it);
}

void Tunnel::enforce_retention_cap() {
    // 5.6: 남기는 것은 많아야 MAX_PEERS - 1 개. 넘으면 가장 오래 남긴 것부터 지운다.
    while (terminal_count() > protocol::kMaxPeers - 1) {
        auto oldest = sessions_.end();
        for (auto it = sessions_.begin(); it != sessions_.end(); ++it) {
            const auto since = it->second.session->terminal_since();
            if (!since) {
                continue;
            }
            if (oldest == sessions_.end() || *since < *oldest->second.session->terminal_since()) {
                oldest = it;
            }
        }
        if (oldest == sessions_.end()) {
            return;
        }
        erase(oldest);
    }
}

void Tunnel::on_datagram(const network::Endpoint& from, std::span<const std::byte> datagram) {
    // 8.1 공통 검사 1~6.
    const CommonCheck common = check_common(datagram);
    if (!common.ok()) {
        if (const auto counter = to_counter(common.error)) {
            counters_.increment(*counter);
        }
        return;
    }
    // 8.1 검사 7. 대조 집합은 세션 표다. 종료 상태로 남긴 세션도 든다 (5.6).
    const auto it = sessions_.find(common.header.peer_id);
    if (it == sessions_.end()) {
        counters_.increment(Counter::DropUnknownPeer);
        return;
    }
    const Millis t = now();
    // 여기부터 세션이 정해졌다. 판정은 전부 그 세션의 표에 오른다 (counters.hpp).
    peer::Session& session = *it->second.session;
    // 8.1 검사 8. 8.2 의 epoch 비교보다 먼저다 (roadmap.md Phase 4 재협상 뒤 지연 패킷).
    if (retired_.contains(common.header.session_epoch, t)) {
        session.counters().increment(Counter::DropRetiredEpoch);
        return;
    }
    // 9.4.1. 타입과 무관하게 전부 폐기. HELLO 도 받지 않는다.
    if (!session.active()) {
        session.counters().increment(Counter::DropTerminalState);
        return;
    }
    (void)session.receive(peer::RxPacket{common.header, common.payload, from}, t);
    enforce_retention_cap();
}

std::optional<Millis> Tunnel::earliest_deadline() const noexcept {
    std::optional<Millis> earliest;
    auto consider = [&earliest](std::optional<Millis> d) {
        if (d && (!earliest || *d < *earliest)) {
            earliest = d;
        }
    };
    for (const auto& [id, entry] : sessions_) {
        consider(entry.session->next_deadline());
        if (const auto since = entry.session->terminal_since()) {
            consider(*since + protocol::kTerminalSessionTtlMs);
        }
    }
    consider(roster_.earliest());
    return earliest;
}

void Tunnel::run_timers(Millis now) {
    for (auto& [id, entry] : sessions_) {
        entry.session->poll(now);
    }
    RosterPublisher::Connected connected;
    for (auto& [id, entry] : sessions_) {
        if (entry.session->state() == peer::SessionState::kConnected) {
            connected.push_back(entry.session.get());
        }
    }
    roster_.run(now, connected);
    // 5.6: 종료 상태 세션은 2분 남긴 뒤 지운다. 비교는 11장 마감과 같은 `<=` 다.
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        const auto since = it->second.session->terminal_since();
        if (since && *since + protocol::kTerminalSessionTtlMs <= now) {
            auto next = std::next(it);
            erase(it);
            it = next;
        } else {
            ++it;
        }
    }
    enforce_retention_cap();
}

void Tunnel::emit_session_counters() const {
    for (const auto& [id, entry] : sessions_) {
        const CounterPeer peer = entry.session->counter_peer();
        emit_all(entry.session->counters(), &peer);
    }
}

void Tunnel::close_all(Millis now) {
    for (auto& [id, entry] : sessions_) {
        entry.session->close_by_user(now);
    }
    enforce_retention_cap();
}

void Tunnel::leave_room(Millis now) {
    for (auto& [id, entry] : sessions_) {
        entry.session->close_by_user(now);
    }
    // 5.6: 방을 떠나면 남긴 세션도 함께 지운다. 2분이 안 됐어도.
    while (!sessions_.empty()) {
        erase(sessions_.begin());
    }
    local_vip_.reset();
    roster_.stop();
}

peer::Session* Tunnel::route(std::uint32_t virtual_ip) const noexcept {
    return routes_.lookup(virtual_ip);
}

std::vector<std::uint32_t> Tunnel::connected_peers() const {
    std::vector<std::uint32_t> out;
    for (const peer::Session* s : routes_.slots()) {
        if (s != nullptr && s->state() == peer::SessionState::kConnected) {
            out.push_back(s->peer_virtual_ip());
        }
    }
    return out;
}

TxOutcome Tunnel::send_data(std::span<const std::byte> inner) {
    // 8.5 의 표 순서. 길이가 맨 앞이다. 출발지와 목적지는 오프셋 12~20 에 있다.
    auto drop = [this](Counter c) {
        counters_.increment(c);
        return TxOutcome::kDropped;
    };
    if (inner.size() < 20) {
        return drop(Counter::TxDropShort);
    }
    if ((std::to_integer<std::uint8_t>(inner[0]) >> 4) != 4) {
        return drop(Counter::TxDropNotIpv4);
    }
    if (inner.size() > protocol::kMaxInner) {
        return drop(Counter::TxDropOversize);
    }
    if (!local_vip_ || be32_at(inner, 12) != *local_vip_) {
        return drop(Counter::TxDropBadSrc);
    }
    peer::Session* session = route(be32_at(inner, 16));
    if (session == nullptr) {
        return drop(Counter::TxDropNoRoute);  // 세션을 고르지 못했다. 전역이다
    }
    if (session->state() != peer::SessionState::kConnected) {
        session->counters().increment(Counter::TxDropNotConnected);  // 고른 세션의 표
        return TxOutcome::kDropped;
    }
    if (on_tx_) {
        on_tx_(inner);  // 캡슐화 직전 (roadmap.md Phase 4 M-5 의 송신 측 비교 지점)
    }
    return session->send_data(inner) ? TxOutcome::kSent : TxOutcome::kSendFailed;
}

}  // namespace sangtachi::tunnel
