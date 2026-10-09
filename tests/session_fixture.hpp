#pragma once

// 세션과 터널 시험이 같이 쓰는 도구. 송신을 기록하는 가짜, 패킷 생성기, 합성 내부 IPv4 패킷.

#include "sangtachi/counters.hpp"
#include "sangtachi/network/endpoint.hpp"
#include "sangtachi/peer/epoch.hpp"
#include "sangtachi/peer/session.hpp"
#include "sangtachi/protocol_constants.hpp"
#include "sangtachi/tunnel/packet.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace sangtachi::testing {

inline constexpr std::uint32_t kLocalPeerId = 0x11111111u;
inline constexpr std::uint32_t kPeerId = 0x22222222u;
inline constexpr std::uint32_t kLocalVip = 0x0A640002u;  // 10.100.0.2
inline constexpr std::uint32_t kPeerVip = 0x0A640001u;   // 10.100.0.1
inline constexpr std::uint32_t kLocalEpoch = 0xAAAA0001u;
inline constexpr std::uint32_t kPeerEpoch = 0xBBBB0001u;
inline constexpr std::uint32_t kPeerEpoch2 = 0xBBBB0002u;

inline const network::Endpoint kCandA{0x01020304u, 5000};   // 1.2.3.4:5000
inline const network::Endpoint kCandB{0x05060708u, 6000};   // 5.6.7.8:6000
inline const network::Endpoint kOutside{0x09090909u, 7000}; // 후보 집합 밖

inline peer::Nonce nonce_fill(std::uint8_t value) {
    peer::Nonce n;
    n.bytes.fill(static_cast<std::byte>(value));
    return n;
}

inline tunnel::Nonce wire_nonce(const peer::Nonce& n) {
    tunnel::Nonce out{};
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = n.bytes[i];
    }
    return out;
}

inline const peer::Nonce kLocalNonce = nonce_fill(0x11);
inline const peer::Nonce kPeerNonce = nonce_fill(0x22);

// 보낸 데이터그램 하나를 헤더와 페이로드로 나눠 기록한다.
struct SentPacket {
    network::Endpoint to;
    tunnel::Header header;
    std::vector<std::byte> payload;

    [[nodiscard]] tunnel::PacketType type() const noexcept {
        return static_cast<tunnel::PacketType>(header.type);
    }
};

struct FakeIo {
    explicit FakeIo(bool sink = false) : with_sink(sink) {}
    FakeIo(const FakeIo&) = delete;
    FakeIo& operator=(const FakeIo&) = delete;

    peer::RetiredEpochs retired;
    std::vector<SentPacket> sent;
    bool send_ok = true;
    std::uint64_t us = 0;
    std::vector<std::pair<peer::SessionState, peer::SessionState>> transitions;
    std::vector<std::vector<std::byte>> sunk;
    bool with_sink = false;

    peer::SendFn send_fn() {
        return [this](const network::Endpoint& to, std::span<const std::byte> bytes) {
            const auto check = tunnel::check_common(bytes);
            SentPacket p;
            p.to = to;
            p.header = check.header;
            p.payload.assign(check.payload.begin(), check.payload.end());
            sent.push_back(std::move(p));
            return send_ok;
        };
    }

    peer::SessionIo io() {
        peer::SessionIo io;
        io.send = send_fn();
        io.retired = &retired;
        io.rtt_clock_us = [this] { return us; };
        if (with_sink) {
            io.data_sink = [this](std::span<const std::byte> bytes) {
                sunk.emplace_back(bytes.begin(), bytes.end());
            };
        }
        io.on_transition = [this](const peer::Session&, peer::SessionState from,
                                  peer::SessionState to) { transitions.emplace_back(from, to); };
        return io;
    }

    [[nodiscard]] std::vector<SentPacket> of_type(tunnel::PacketType type) const {
        std::vector<SentPacket> out;
        for (const auto& p : sent) {
            if (p.type() == type) {
                out.push_back(p);
            }
        }
        return out;
    }
};

inline peer::SessionConfig session_config() {
    peer::SessionConfig c;
    c.local_peer_id = kLocalPeerId;
    c.local_virtual_ip = kLocalVip;
    c.peer_id = kPeerId;
    c.peer_virtual_ip = kPeerVip;
    c.candidates = {kCandA, kCandB};
    c.peer_is_host = true;
    c.local_epoch = kLocalEpoch;
    c.attempt_nonce = kLocalNonce;
    return c;
}

// 받은 패킷 하나. 페이로드를 들고 있다.
struct Rx {
    tunnel::Header header;
    std::vector<std::byte> payload;
    network::Endpoint from;

    [[nodiscard]] peer::RxPacket view() const {
        return peer::RxPacket{header, std::span<const std::byte>(payload), from};
    }
    // 와이어 바이트 (터널 시험이 on_datagram 에 넣는다).
    [[nodiscard]] std::vector<std::byte> wire() const {
        std::vector<std::byte> out(protocol::kHeaderSize + payload.size());
        tunnel::Header h = header;
        h.payload_length = static_cast<std::uint16_t>(payload.size());
        const auto head = tunnel::serialize_header(h);
        std::copy(head.begin(), head.end(), out.begin());
        std::copy(payload.begin(), payload.end(), out.begin() + protocol::kHeaderSize);
        return out;
    }
};

inline Rx make_rx(tunnel::PacketType type, std::vector<std::byte> payload, std::uint32_t epoch,
                  std::uint32_t seq, network::Endpoint from = kCandA,
                  std::uint32_t peer_id = kPeerId) {
    Rx rx;
    rx.header.type = static_cast<std::uint8_t>(type);
    rx.header.payload_length = static_cast<std::uint16_t>(payload.size());
    rx.header.peer_id = peer_id;
    rx.header.session_epoch = epoch;
    rx.header.sequence = seq;
    rx.payload = std::move(payload);
    rx.from = from;
    return rx;
}

inline std::vector<std::byte> hello_bytes(const peer::Nonce& nonce, std::uint32_t vip) {
    const auto b = tunnel::encode_hello(tunnel::HelloPayload{wire_nonce(nonce), vip});
    return {b.begin(), b.end()};
}

inline Rx hello(std::uint32_t epoch, std::uint32_t seq, network::Endpoint from = kCandA,
                std::uint32_t vip = kPeerVip, const peer::Nonce& nonce = kPeerNonce) {
    return make_rx(tunnel::PacketType::kHello, hello_bytes(nonce, vip), epoch, seq, from);
}

inline Rx hello_ack(std::uint32_t epoch, std::uint32_t seq, network::Endpoint from = kCandA,
                    std::uint32_t vip = kPeerVip, const peer::Nonce& echo = kLocalNonce) {
    return make_rx(tunnel::PacketType::kHelloAck, hello_bytes(echo, vip), epoch, seq, from);
}

inline Rx keepalive(std::uint32_t epoch, std::uint32_t seq, network::Endpoint from = kCandA) {
    return make_rx(tunnel::PacketType::kKeepalive, {}, epoch, seq, from);
}

inline Rx ping(std::uint32_t epoch, std::uint32_t seq, std::uint64_t id,
               network::Endpoint from = kCandA) {
    const auto b = tunnel::encode_ping(id);
    return make_rx(tunnel::PacketType::kPing, {b.begin(), b.end()}, epoch, seq, from);
}

inline Rx pong(std::uint32_t epoch, std::uint32_t seq, std::uint64_t id,
               network::Endpoint from = kCandA) {
    const auto b = tunnel::encode_ping(id);
    return make_rx(tunnel::PacketType::kPong, {b.begin(), b.end()}, epoch, seq, from);
}

inline Rx close_rx(std::uint32_t epoch, std::uint32_t seq, std::uint8_t reason,
                   network::Endpoint from = kCandA) {
    return make_rx(tunnel::PacketType::kClose, {static_cast<std::byte>(reason)}, epoch, seq, from);
}

// roadmap.md Phase 4 시험 장치의 100바이트 내부 패킷 (체크섬을 채운다).
inline std::vector<std::byte> inner_ipv4(std::uint32_t src, std::uint32_t dst,
                                         std::size_t size = 100) {
    std::vector<std::byte> p(size, std::byte{0xA5});
    auto put16 = [&](std::size_t at, std::uint16_t v) {
        p[at] = static_cast<std::byte>(v >> 8);
        p[at + 1] = static_cast<std::byte>(v & 0xFF);
    };
    auto put32 = [&](std::size_t at, std::uint32_t v) {
        put16(at, static_cast<std::uint16_t>(v >> 16));
        put16(at + 2, static_cast<std::uint16_t>(v & 0xFFFF));
    };
    p[0] = std::byte{0x45};
    p[1] = std::byte{0x00};
    put16(2, static_cast<std::uint16_t>(size));
    put16(4, 0);
    put16(6, 0);
    p[8] = std::byte{64};
    p[9] = std::byte{17};
    put16(10, 0);
    put32(12, src);
    put32(16, dst);
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i < 20; i += 2) {
        sum += (std::to_integer<std::uint32_t>(p[i]) << 8) | std::to_integer<std::uint32_t>(p[i + 1]);
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    put16(10, static_cast<std::uint16_t>(~sum & 0xFFFF));
    return p;
}

inline Rx data_rx(std::uint32_t epoch, std::uint32_t seq, std::vector<std::byte> inner,
                  network::Endpoint from = kCandA) {
    return make_rx(tunnel::PacketType::kData, std::move(inner), epoch, seq, from);
}

}  // namespace sangtachi::testing
