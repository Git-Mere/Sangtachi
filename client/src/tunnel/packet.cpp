#include "sangtachi/tunnel/packet.hpp"

#include "sangtachi/network/endpoint.hpp"
#include "sangtachi/protocol_constants.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace sangtachi::tunnel {
namespace {

// 4.1 와이어 레이아웃의 오프셋.
constexpr std::size_t kOffMagic = 0;
constexpr std::size_t kOffVersion = 4;
constexpr std::size_t kOffType = 5;
constexpr std::size_t kOffPayloadLength = 6;
constexpr std::size_t kOffPeerId = 8;
constexpr std::size_t kOffSessionEpoch = 12;
constexpr std::size_t kOffSequence = 16;

// 바이트 오더 변환을 한 자리에 둔다. 전부 빅엔디안이다 (protocol.md 1장).
[[nodiscard]] constexpr std::uint8_t read_u8(std::span<const std::byte> bytes,
                                             std::size_t offset) noexcept {
    return std::to_integer<std::uint8_t>(bytes[offset]);
}

[[nodiscard]] constexpr std::uint16_t read_u16(std::span<const std::byte> bytes,
                                               std::size_t offset) noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(read_u8(bytes, offset)) << 8) |
                                      static_cast<std::uint16_t>(read_u8(bytes, offset + 1)));
}

[[nodiscard]] constexpr std::uint32_t read_u32(std::span<const std::byte> bytes,
                                               std::size_t offset) noexcept {
    return (static_cast<std::uint32_t>(read_u8(bytes, offset)) << 24) |
           (static_cast<std::uint32_t>(read_u8(bytes, offset + 1)) << 16) |
           (static_cast<std::uint32_t>(read_u8(bytes, offset + 2)) << 8) |
           static_cast<std::uint32_t>(read_u8(bytes, offset + 3));
}

[[nodiscard]] constexpr std::uint64_t read_u64(std::span<const std::byte> bytes,
                                               std::size_t offset) noexcept {
    return (static_cast<std::uint64_t>(read_u32(bytes, offset)) << 32) |
           static_cast<std::uint64_t>(read_u32(bytes, offset + 4));
}

constexpr void write_u8(std::span<std::byte> bytes, std::size_t offset, std::uint8_t value) noexcept {
    bytes[offset] = static_cast<std::byte>(value);
}

constexpr void write_u16(std::span<std::byte> bytes, std::size_t offset,
                         std::uint16_t value) noexcept {
    bytes[offset] = static_cast<std::byte>((value >> 8) & 0xFFu);
    bytes[offset + 1] = static_cast<std::byte>(value & 0xFFu);
}

constexpr void write_u32(std::span<std::byte> bytes, std::size_t offset,
                         std::uint32_t value) noexcept {
    bytes[offset] = static_cast<std::byte>((value >> 24) & 0xFFu);
    bytes[offset + 1] = static_cast<std::byte>((value >> 16) & 0xFFu);
    bytes[offset + 2] = static_cast<std::byte>((value >> 8) & 0xFFu);
    bytes[offset + 3] = static_cast<std::byte>(value & 0xFFu);
}

constexpr void write_u64(std::span<std::byte> bytes, std::size_t offset,
                         std::uint64_t value) noexcept {
    write_u32(bytes, offset, static_cast<std::uint32_t>(value >> 32));
    write_u32(bytes, offset + 4, static_cast<std::uint32_t>(value & 0xFFFFFFFFu));
}

// control_plane.md 2.7 의 63자. 표준 라이브러리의 문자 분류(isalnum 따위)를 쓰지 않는다.
// 로캘을 따라 ASCII 밖의 바이트를 글자로 볼 수 있다.
[[nodiscard]] constexpr bool is_name_byte(std::uint8_t c) noexcept {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

}  // namespace

std::string_view counter_name(PacketError error) noexcept {
    switch (error) {
        case PacketError::kNone: return {};
        case PacketError::kShort: return "drop_short";
        case PacketError::kMagic: return "drop_magic";
        case PacketError::kVersion: return "drop_version";
        case PacketError::kUnknownType: return "drop_unknown_type";
        case PacketError::kLength: return "drop_length";
        case PacketError::kTypeLength: return "drop_type_length";
        case PacketError::kBadReason: return "drop_bad_reason";
        case PacketError::kRosterDirection: return "drop_roster_direction";
        case PacketError::kRosterStale: return "drop_roster_stale";
        case PacketError::kRosterFlags: return "drop_roster_flags";
        case PacketError::kRosterName: return "drop_roster_name";
    }
    return {};
}

// --- 헤더 ---

std::array<std::byte, protocol::kHeaderSize> serialize_header(const Header& header) noexcept {
    std::array<std::byte, protocol::kHeaderSize> out{};
    const std::span<std::byte> bytes(out);
    write_u32(bytes, kOffMagic, header.magic);
    write_u8(bytes, kOffVersion, header.version);
    write_u8(bytes, kOffType, header.type);
    write_u16(bytes, kOffPayloadLength, header.payload_length);
    write_u32(bytes, kOffPeerId, header.peer_id);
    write_u32(bytes, kOffSessionEpoch, header.session_epoch);
    write_u32(bytes, kOffSequence, header.sequence);
    return out;
}

std::optional<Header> deserialize_header(std::span<const std::byte> bytes) noexcept {
    if (bytes.size() < protocol::kHeaderSize) {
        return std::nullopt;
    }
    Header header;
    header.magic = read_u32(bytes, kOffMagic);
    header.version = read_u8(bytes, kOffVersion);
    header.type = read_u8(bytes, kOffType);
    header.payload_length = read_u16(bytes, kOffPayloadLength);
    header.peer_id = read_u32(bytes, kOffPeerId);
    header.session_epoch = read_u32(bytes, kOffSessionEpoch);
    header.sequence = read_u32(bytes, kOffSequence);
    return header;
}

bool is_known_type(std::uint8_t type) noexcept {
    return type >= static_cast<std::uint8_t>(PacketType::kHello) &&
           type <= static_cast<std::uint8_t>(PacketType::kRoster);
}

bool payload_length_ok(PacketType type, std::size_t length) noexcept {
    switch (type) {
        case PacketType::kHello:
        case PacketType::kHelloAck: return length == kHelloPayloadSize;
        case PacketType::kKeepalive: return length == 0;
        case PacketType::kData: return length >= kDataMinPayload && length <= kDataMaxPayload;
        case PacketType::kPing:
        case PacketType::kPong: return length == kPingPayloadSize;
        case PacketType::kClose: return length == kClosePayloadSize;
        case PacketType::kRoster: return length >= kRosterMinPayload && length <= kRosterMaxPayload;
    }
    return false;  // 5장 목록 밖
}

CommonCheck check_common(std::span<const std::byte> datagram) noexcept {
    CommonCheck result;

    // 1. 길이 >= HEADER_SIZE. 이것이 아래의 모든 읽기를 버퍼 안에 둔다.
    const auto header = deserialize_header(datagram);
    if (!header) {
        result.error = PacketError::kShort;
        return result;
    }
    // 2~4.
    if (header->magic != protocol::kTunnelMagic) {
        result.error = PacketError::kMagic;
        return result;
    }
    if (header->version != protocol::kTunnelVersion) {
        result.error = PacketError::kVersion;
        return result;
    }
    if (!is_known_type(header->type)) {
        result.error = PacketError::kUnknownType;
        return result;
    }
    // 5. payload_length == len - HEADER_SIZE. 1번을 통과했으므로 뺄셈이 언더플로하지 않는다.
    const std::size_t actual = datagram.size() - protocol::kHeaderSize;
    if (static_cast<std::size_t>(header->payload_length) != actual) {
        result.error = PacketError::kLength;
        return result;
    }
    // 6. 타입별 길이.
    if (!payload_length_ok(static_cast<PacketType>(header->type), actual)) {
        result.error = PacketError::kTypeLength;
        return result;
    }

    result.header = *header;
    result.payload = datagram.subspan(protocol::kHeaderSize);
    return result;
}

std::size_t build_packet(PacketType type, const HeaderFields& fields,
                         std::span<const std::byte> payload, std::span<std::byte> out) noexcept {
    if (!payload_length_ok(type, payload.size())) {
        return 0;
    }
    const std::size_t total = protocol::kHeaderSize + payload.size();
    if (out.size() < total) {
        return 0;
    }

    Header header;
    header.type = static_cast<std::uint8_t>(type);
    // payload_length_ok 가 최대 1452 로 묶었으므로 16비트에 들어간다.
    header.payload_length = static_cast<std::uint16_t>(payload.size());
    header.peer_id = fields.peer_id;
    header.session_epoch = fields.session_epoch;
    header.sequence = fields.sequence;

    const auto head = serialize_header(header);
    for (std::size_t i = 0; i < head.size(); ++i) {
        out[i] = head[i];
    }
    for (std::size_t i = 0; i < payload.size(); ++i) {
        out[protocol::kHeaderSize + i] = payload[i];
    }
    return total;
}

// --- 5.1 HELLO, 5.2 HELLO_ACK ---

std::array<std::byte, kHelloPayloadSize> encode_hello(const HelloPayload& payload) noexcept {
    std::array<std::byte, kHelloPayloadSize> out{};
    for (std::size_t i = 0; i < kNonceSize; ++i) {
        out[i] = payload.nonce[i];
    }
    write_u32(std::span<std::byte>(out), kNonceSize, payload.virtual_ip);
    return out;
}

HelloDecode decode_hello(std::span<const std::byte> payload) noexcept {
    HelloDecode result;
    if (!payload_length_ok(PacketType::kHello, payload.size())) {
        result.error = PacketError::kTypeLength;
        return result;
    }
    for (std::size_t i = 0; i < kNonceSize; ++i) {
        result.value.nonce[i] = payload[i];
    }
    result.value.virtual_ip = read_u32(payload, kNonceSize);
    return result;
}

// --- 5.5 PING / PONG ---

std::array<std::byte, kPingPayloadSize> encode_ping(std::uint64_t ping_id) noexcept {
    std::array<std::byte, kPingPayloadSize> out{};
    write_u64(std::span<std::byte>(out), 0, ping_id);
    return out;
}

PingDecode decode_ping(std::span<const std::byte> payload) noexcept {
    PingDecode result;
    if (!payload_length_ok(PacketType::kPing, payload.size())) {
        result.error = PacketError::kTypeLength;
        return result;
    }
    result.ping_id = read_u64(payload, 0);
    return result;
}

// --- 5.6 CLOSE ---

std::array<std::byte, kClosePayloadSize> encode_close(CloseReason reason) noexcept {
    return {static_cast<std::byte>(reason)};
}

CloseDecode decode_close(std::span<const std::byte> payload) noexcept {
    CloseDecode result;
    if (!payload_length_ok(PacketType::kClose, payload.size())) {
        result.error = PacketError::kTypeLength;
        return result;
    }
    const std::uint8_t reason = read_u8(payload, 0);
    if (reason > static_cast<std::uint8_t>(CloseReason::kSetupFailed)) {
        result.error = PacketError::kBadReason;  // 0x00~0x03 밖 (5.6)
        return result;
    }
    result.reason = static_cast<CloseReason>(reason);
    return result;
}

// --- 5.7 ROSTER ---

bool is_valid_name(std::string_view name) noexcept {
    if (name.empty() || name.size() > protocol::kMaxNameLen) {
        return false;
    }
    for (const char c : name) {
        if (!is_name_byte(static_cast<std::uint8_t>(c))) {
            return false;
        }
    }
    return true;
}

RosterDecode decode_roster(std::span<const std::byte> payload,
                           const RosterContext& context) noexcept {
    RosterDecode result;
    auto fail = [&result](PacketError error) {
        result.error = error;
        result.value = Roster{};
        return result;
    };

    // 1. 보낸 쪽이 호스트다.
    if (!context.sender_is_host) {
        return fail(PacketError::kRosterDirection);
    }

    // 2. 구조. 머리, count, 멤버를 앞에서부터, 끝에서 남는 바이트.
    if (payload.size() < kRosterHeadSize) {
        return fail(PacketError::kTypeLength);
    }
    Roster& roster = result.value;
    roster.generation = read_u32(payload, 0);
    const std::uint8_t count = read_u8(payload, 4);
    if (count < 1 || count > protocol::kMaxPeers) {
        return fail(PacketError::kTypeLength);
    }
    roster.count = count;

    // flags 는 4번이 보도록 원래 바이트를 따로 둔다. 구조체에는 비트 0 만 남는다.
    std::array<std::uint8_t, protocol::kMaxPeers> raw_flags{};
    std::size_t pos = kRosterHeadSize;
    for (std::size_t i = 0; i < count; ++i) {
        // 뺄셈으로 남은 길이를 잰다. pos 는 늘 payload.size() 이하라 언더플로하지 않는다.
        const std::size_t remaining = payload.size() - pos;
        if (remaining < kRosterMemberFixedSize) {
            return fail(PacketError::kTypeLength);
        }
        RosterMember& m = roster.members[i];
        m.peer_id = read_u32(payload, pos);
        m.virtual_ip = read_u32(payload, pos + 4);
        raw_flags[i] = read_u8(payload, pos + 8);
        const std::uint8_t name_len = read_u8(payload, pos + 9);
        if (name_len < 1 || name_len > protocol::kMaxNameLen) {
            return fail(PacketError::kTypeLength);
        }
        if (remaining - kRosterMemberFixedSize < name_len) {
            return fail(PacketError::kTypeLength);  // 멤버가 페이로드 끝을 넘는다
        }
        m.connected = (raw_flags[i] & kRosterFlagConnected) != 0;
        m.name_len = name_len;
        for (std::size_t k = 0; k < name_len; ++k) {
            m.name[k] = static_cast<char>(read_u8(payload, pos + kRosterMemberFixedSize + k));
        }
        pos += kRosterMemberFixedSize + name_len;
    }
    if (pos != payload.size()) {
        return fail(PacketError::kTypeLength);  // 남는 바이트
    }

    // 3. generation 이 마지막으로 받아들인 값보다 크다.
    if (context.last_generation && roster.generation <= *context.last_generation) {
        return fail(PacketError::kRosterStale);
    }

    // 4. 모든 멤버의 flags 비트 1~7 이 0.
    for (std::size_t i = 0; i < count; ++i) {
        if ((raw_flags[i] & kRosterFlagsReserved) != 0) {
            return fail(PacketError::kRosterFlags);
        }
    }

    // 5. 모든 멤버의 이름이 control_plane.md 2.7 의 문자 집합 안이다.
    for (std::size_t i = 0; i < count; ++i) {
        if (!is_valid_name(roster.members[i].name_view())) {
            return fail(PacketError::kRosterName);
        }
    }
    return result;
}

std::size_t encode_roster(const Roster& roster, std::span<std::byte> out) noexcept {
    if (roster.count < 1 || roster.count > protocol::kMaxPeers) {
        return 0;
    }
    // 먼저 전부 검사하고 크기를 잰다. 중간에 멈춰 반쯤 쓴 버퍼를 남기지 않는다.
    std::size_t total = kRosterHeadSize;
    for (std::size_t i = 0; i < roster.count; ++i) {
        const RosterMember& m = roster.members[i];
        // name_view 는 배열 크기에서 자르므로 name_len 을 따로 본다.
        if (m.name_len > protocol::kMaxNameLen || !is_valid_name(m.name_view())) {
            return 0;
        }
        total += kRosterMemberFixedSize + m.name_len;
    }
    if (out.size() < total) {
        return 0;
    }

    write_u32(out, 0, roster.generation);
    write_u8(out, 4, roster.count);
    std::size_t pos = kRosterHeadSize;
    for (std::size_t i = 0; i < roster.count; ++i) {
        const RosterMember& m = roster.members[i];
        write_u32(out, pos, m.peer_id);
        write_u32(out, pos + 4, m.virtual_ip);
        write_u8(out, pos + 8, m.connected ? kRosterFlagConnected : std::uint8_t{0});
        write_u8(out, pos + 9, m.name_len);
        for (std::size_t k = 0; k < m.name_len; ++k) {
            write_u8(out, pos + kRosterMemberFixedSize + k, static_cast<std::uint8_t>(m.name[k]));
        }
        pos += kRosterMemberFixedSize + m.name_len;
    }
    return total;
}

std::string format_roster_members(const Roster& roster) {
    std::string out;
    for (std::size_t i = 0; i < roster.count && i < roster.members.size(); ++i) {
        const auto& m = roster.members[i];
        if (i > 0) {
            out += ',';
        }
        out += std::to_string(m.peer_id) + "/" + network::format_ipv4(m.virtual_ip) + "/" +
               (m.connected ? "1" : "0") + "/" + std::string(m.name_view());
    }
    return out;
}

}  // namespace sangtachi::tunnel
