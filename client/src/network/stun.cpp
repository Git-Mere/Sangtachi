#include "sangtachi/network/stun.hpp"

#include "sangtachi/protocol_constants.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace sangtachi::network {
namespace {

// 속성 헤더는 종류 2 + 값 길이 2 다 (RFC 5389).
constexpr std::size_t kAttrHeaderSize = 4;

// IPv4 `XOR-MAPPED-ADDRESS` 의 값 길이. 예약 1 + family 1 + X-Port 2 + X-Address 4.
constexpr std::size_t kMappedIpv4ValueSize = 8;

// `ERROR-CODE` 값의 앞머리. 예약 2 + class 1 + number 1. 뒤는 이유 문구이고 보지 않는다.
constexpr std::size_t kErrorCodePrefixSize = 4;

[[nodiscard]] constexpr std::uint8_t byte_at(std::span<const std::byte> bytes,
                                             std::size_t offset) noexcept {
    return std::to_integer<std::uint8_t>(bytes[offset]);
}

[[nodiscard]] constexpr std::uint16_t read_u16(std::span<const std::byte> bytes,
                                               std::size_t offset) noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(byte_at(bytes, offset)) << 8) |
                                      static_cast<std::uint16_t>(byte_at(bytes, offset + 1)));
}

[[nodiscard]] constexpr std::uint32_t read_u32(std::span<const std::byte> bytes,
                                               std::size_t offset) noexcept {
    return (static_cast<std::uint32_t>(byte_at(bytes, offset)) << 24) |
           (static_cast<std::uint32_t>(byte_at(bytes, offset + 1)) << 16) |
           (static_cast<std::uint32_t>(byte_at(bytes, offset + 2)) << 8) |
           static_cast<std::uint32_t>(byte_at(bytes, offset + 3));
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

// 상위 2비트가 00 이어야 STUN 이다 (protocol.md 7장 분류).
[[nodiscard]] constexpr bool has_stun_leading_bits(std::span<const std::byte> datagram) noexcept {
    return (byte_at(datagram, 0) & 0xC0u) == 0x00u;
}

[[nodiscard]] constexpr bool has_stun_cookie(std::span<const std::byte> datagram) noexcept {
    return read_u32(datagram, 4) == protocol::kStunCookie;
}

[[nodiscard]] TransactionId read_transaction_id(std::span<const std::byte> datagram) noexcept {
    TransactionId id{};
    for (std::size_t i = 0; i < id.size(); ++i) {
        id[i] = datagram[8 + i];
    }
    return id;
}

// 값 길이를 4의 배수로 올린다. 메시지 길이 필드는 이 패딩을 포함한다 (RFC 5389).
[[nodiscard]] constexpr std::size_t padded_size(std::uint16_t value_length) noexcept {
    return (static_cast<std::size_t>(value_length) + 3u) & ~static_cast<std::size_t>(3u);
}

// IPv4 `XOR-MAPPED-ADDRESS` 값 하나. 실패하면 이유를 남기고 값이 없다.
[[nodiscard]] std::optional<Endpoint> decode_mapped_address(std::span<const std::byte> value,
                                                            StunParseError& error) noexcept {
    // family 를 읽기 전에 그 자리가 있는지 본다. 없으면 주소군을 모르므로 판정하지 않는다.
    if (value.size() < 2) {
        error = StunParseError::kMappedAddressMalformed;
        return std::nullopt;
    }
    if (byte_at(value, 1) != kAddressFamilyIpv4) {
        error = StunParseError::kMappedAddressFamily;  // protocol.md 13장 주소군
        return std::nullopt;
    }
    if (value.size() != kMappedIpv4ValueSize) {
        error = StunParseError::kMappedAddressMalformed;
        return std::nullopt;
    }

    // 포트는 쿠키의 상위 16비트와, 주소는 쿠키 전체와 XOR 한다 (RFC 5389).
    const std::uint16_t cookie_high =
        static_cast<std::uint16_t>((protocol::kStunCookie >> 16) & 0xFFFFu);
    const std::uint16_t port = static_cast<std::uint16_t>(read_u16(value, 2) ^ cookie_high);
    const std::uint32_t address = read_u32(value, 4) ^ protocol::kStunCookie;
    return Endpoint(address, port);
}

// `ERROR-CODE` 의 숫자 값. 읽지 못하면 비운다. 이 값은 로그용이고 판정에 쓰지 않는다.
[[nodiscard]] std::optional<std::uint16_t> decode_error_code(
    std::span<const std::byte> value) noexcept {
    if (value.size() < kErrorCodePrefixSize) {
        return std::nullopt;
    }
    const std::uint8_t error_class = byte_at(value, 2) & 0x07u;
    const std::uint8_t number = byte_at(value, 3);
    if (error_class < 3u || error_class > 6u || number > 99u) {
        return std::nullopt;  // RFC 5389 가 정한 범위 밖이다
    }
    return static_cast<std::uint16_t>(error_class * 100u + number);
}

[[nodiscard]] StunParseResult failure(StunParseError error) noexcept {
    StunParseResult result;
    result.error = error;
    return result;
}

}  // namespace

std::array<std::byte, kStunHeaderSize> build_binding_request(
    const TransactionId& transaction_id) noexcept {
    std::array<std::byte, kStunHeaderSize> message{};
    const std::span<std::byte> bytes(message);

    write_u16(bytes, 0, static_cast<std::uint16_t>(StunMessageType::kBindingRequest));
    write_u16(bytes, 2, 0);  // 속성이 없다 (protocol.md 13장 송신 속성)
    write_u32(bytes, 4, protocol::kStunCookie);
    for (std::size_t i = 0; i < transaction_id.size(); ++i) {
        message[8 + i] = transaction_id[i];
    }
    return message;
}

std::optional<TransactionId> peek_transaction_id(std::span<const std::byte> datagram) noexcept {
    if (datagram.size() < kStunHeaderSize) {
        return std::nullopt;
    }
    if (!has_stun_leading_bits(datagram) || !has_stun_cookie(datagram)) {
        return std::nullopt;
    }
    return read_transaction_id(datagram);
}

StunParseResult parse_binding_response(std::span<const std::byte> datagram,
                                       const TransactionId& expected) noexcept {
    // 1~2. 헤더가 있는지, STUN 인지.
    if (datagram.size() < kStunHeaderSize) {
        return failure(StunParseError::kTooShort);
    }
    if (!has_stun_leading_bits(datagram)) {
        return failure(StunParseError::kNotStun);
    }

    // 3~4. magic cookie 와 트랜잭션 ID (protocol.md 13장 응답 검증).
    if (!has_stun_cookie(datagram)) {
        return failure(StunParseError::kBadCookie);
    }
    if (read_transaction_id(datagram) != expected) {
        return failure(StunParseError::kTransactionMismatch);
    }

    // 5. 종류. 요청이 되돌아온 것도 여기서 걸린다.
    const std::uint16_t raw_type = read_u16(datagram, 0);
    const bool is_success =
        raw_type == static_cast<std::uint16_t>(StunMessageType::kBindingSuccessResponse);
    const bool is_error =
        raw_type == static_cast<std::uint16_t>(StunMessageType::kBindingErrorResponse);
    if (!is_success && !is_error) {
        return failure(StunParseError::kUnknownType);
    }

    // 6~7. 길이 필드. 속성은 4바이트 경계에 놓이므로 4의 배수가 아닌 길이는 속성 열이
    // 될 수 없다. 그 다음에 실제 바이트 수와 맞춘다 (13장 응답 검증).
    const std::uint16_t declared = read_u16(datagram, 2);
    if ((declared & 0x03u) != 0u) {
        return failure(StunParseError::kLengthNotAligned);
    }
    if (kStunHeaderSize + static_cast<std::size_t>(declared) != datagram.size()) {
        return failure(StunParseError::kLengthMismatch);
    }

    StunParseResult result;
    result.response.type = is_success ? StunMessageType::kBindingSuccessResponse
                                      : StunMessageType::kBindingErrorResponse;

    // 8~10. 속성 순회. 경계를 넘으면 중단한다 (13장 응답 검증).
    bool saw_mapped = false;
    bool duplicate_mapped = false;
    bool saw_error_code = false;
    for (std::size_t offset = kStunHeaderSize; offset < datagram.size();) {
        const std::size_t remaining = datagram.size() - offset;

        // 앞의 6~7 이 서 있는 한 여기는 닿지 않는다. 시작 오프셋이 20 이고 길이 필드가
        // 4의 배수이며 한 바퀴가 늘 4의 배수만큼 전진하므로 remaining 도 4의 배수다.
        // 남겨 두는 이유는 순회가 그 두 검사의 존재에 기대지 않게 하려는 것이다.
        // **시험이 닿지 못하는 줄이다.** 6 이나 7 을 빼거나 순서를 바꾸면 그때 닿는다.
        if (remaining < kAttrHeaderSize) {
            return failure(StunParseError::kAttributeTruncated);
        }
        const std::uint16_t attr_type = read_u16(datagram, offset);
        const std::uint16_t attr_length = read_u16(datagram, offset + 2);
        const std::size_t padded = padded_size(attr_length);
        if (remaining - kAttrHeaderSize < padded) {
            return failure(StunParseError::kAttributeTruncated);
        }
        const auto value = datagram.subspan(offset + kAttrHeaderSize, attr_length);

        // 성공 응답의 XOR-MAPPED-ADDRESS 가 두 개 이상이면 폐기한다 (13장). 여기서
        // 곧바로 돌아서지 않는 이유는 뒤쪽 바이트도 경계 검사를 받아야 하기 때문이다.
        // 표시만 하고 순회는 끝까지 한다. 두 번째 값은 해석하지 않는다. 해석해 봐야
        // 어느 것이 맞는지 판정할 근거가 없다.
        if (is_success && attr_type == kAttrXorMappedAddress && saw_mapped) {
            duplicate_mapped = true;
        } else if (is_success && attr_type == kAttrXorMappedAddress) {
            saw_mapped = true;
            StunParseError mapped_error = StunParseError::kNone;
            const auto mapped = decode_mapped_address(value, mapped_error);
            if (!mapped) {
                return failure(mapped_error);
            }
            result.response.mapped = mapped;
        } else if (is_error && attr_type == kAttrErrorCode && !saw_error_code) {
            saw_error_code = true;
            result.response.error_code = decode_error_code(value);
        }

        offset += kAttrHeaderSize + padded;
    }

    // 11. 성공 응답인데 주소가 없으면 얻은 것이 없다.
    if (is_success && !result.response.mapped) {
        return failure(StunParseError::kMissingMappedAddress);
    }
    // 12. 둘 이상이면 고르지 않는다 (13장 "같은 속성이 두 번 온 성공 응답").
    if (duplicate_mapped) {
        return failure(StunParseError::kDuplicateMappedAddress);
    }
    return result;
}

}  // namespace sangtachi::network
