#include "sangtachi/network/stun.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// 시험 케이스 이름은 ASCII 로만 적는다. 이유는 network/wsa_test.cpp 머리에 있다.
//
// 케이스 표가 먼저다. stun.hpp 머리의 "검사 순서" 와 "이 코드가 정한 것" 두 표의 행마다
// 여기에 케이스가 있다. 값은 코드에서 가져오지 않고 이 파일에 따로 적는다. 구현과 시험이
// 같은 상수를 보면 그 상수가 틀려도 둘 다 같이 틀린다.

using sangtachi::network::build_binding_request;
using sangtachi::network::Endpoint;
using sangtachi::network::kStunHeaderSize;
using sangtachi::network::parse_binding_response;
using sangtachi::network::peek_transaction_id;
using sangtachi::network::StunMessageType;
using sangtachi::network::StunParseError;
using sangtachi::network::TransactionId;

namespace {

using Bytes = std::vector<std::byte>;

constexpr std::uint32_t kCookie = 0x2112A442u;  // RFC 5389
constexpr std::uint16_t kSuccess = 0x0101u;
constexpr std::uint16_t kErrorResponse = 0x0111u;
constexpr std::uint16_t kRequest = 0x0001u;

constexpr std::uint16_t kXorMappedAddress = 0x0020u;
constexpr std::uint16_t kErrorCode = 0x0009u;
constexpr std::uint16_t kSoftware = 0x8022u;  // 해석하지 않는 속성의 보기

// 203.0.113.7:51000. 203.0.113.0/24 는 문서용 대역이다 (RFC 5737).
constexpr std::uint32_t kMappedAddress = 0xCB007107u;
constexpr std::uint16_t kMappedPort = 51000u;

// 198.51.100.9:1. 중복 속성 케이스에서 "뒤엣것" 으로 쓴다.
constexpr std::uint32_t kOtherAddress = 0xC6336409u;
constexpr std::uint16_t kOtherPort = 1u;

const TransactionId kTxid = {
    std::byte{0xA1}, std::byte{0xB2}, std::byte{0xC3}, std::byte{0xD4},
    std::byte{0xE5}, std::byte{0xF6}, std::byte{0x07}, std::byte{0x18},
    std::byte{0x29}, std::byte{0x3A}, std::byte{0x4B}, std::byte{0x5C},
};

// 인자는 넓은 부호 없는 정수로 받고 여기서 자른다. 호출 자리마다 static_cast 를 적으면
// 표가 읽히지 않는다.
void put_u8(Bytes& out, unsigned value) {
    out.push_back(static_cast<std::byte>(value & 0xFFu));
}

void put_u16(Bytes& out, unsigned value) {
    put_u8(out, (value >> 8) & 0xFFu);
    put_u8(out, value & 0xFFu);
}

void put_u32(Bytes& out, std::uint32_t value) {
    put_u16(out, (value >> 16) & 0xFFFFu);
    put_u16(out, value & 0xFFFFu);
}

void put_all(Bytes& out, const Bytes& more) {
    out.insert(out.end(), more.begin(), more.end());
}

Bytes filler(std::size_t count, unsigned value = 0xEEu) {
    Bytes out;
    for (std::size_t i = 0; i < count; ++i) {
        put_u8(out, value);
    }
    return out;
}

// 속성 하나. 값 길이는 실제 값의 길이로 적고 4바이트 경계까지 채운다.
Bytes attribute(std::uint16_t type, const Bytes& value) {
    Bytes out;
    put_u16(out, type);
    put_u16(out, static_cast<std::uint16_t>(value.size()));
    put_all(out, value);
    while ((out.size() % 4u) != 0u) {
        put_u8(out, 0x00);
    }
    return out;
}

// 값 길이를 거짓으로 적는 속성. 잘린 속성 케이스에 쓴다. 채우지 않는다.
Bytes attribute_raw(std::uint16_t type, unsigned declared_length, const Bytes& value) {
    Bytes out;
    put_u16(out, type);
    put_u16(out, declared_length);
    put_all(out, value);
    return out;
}

// `XOR-MAPPED-ADDRESS` 값. 포트는 쿠키의 상위 16비트와, 주소는 쿠키 전체와 XOR 한다.
Bytes xor_mapped_value(unsigned family, std::uint32_t address, std::uint16_t port) {
    Bytes out;
    put_u8(out, 0x00);  // 예약
    put_u8(out, family);
    put_u16(out, static_cast<std::uint16_t>(port ^ static_cast<std::uint16_t>(kCookie >> 16)));
    put_u32(out, address ^ kCookie);
    return out;
}

Bytes mapped_ipv4(std::uint32_t address, std::uint16_t port) {
    return attribute(kXorMappedAddress, xor_mapped_value(0x01u, address, port));
}

Bytes error_code_value(unsigned error_class, unsigned number, std::string_view reason) {
    Bytes out;
    put_u8(out, 0x00);
    put_u8(out, 0x00);
    put_u8(out, error_class);
    put_u8(out, number);
    for (const char c : reason) {
        put_u8(out, static_cast<unsigned char>(c));
    }
    return out;
}

struct MessageSpec {
    std::uint16_t type = kSuccess;
    std::uint32_t cookie = kCookie;
    TransactionId txid = kTxid;
    Bytes body;
    std::optional<std::uint16_t> declared_length;  // 없으면 body 의 길이를 적는다
    std::optional<unsigned> first_byte;           // 없으면 type 의 상위 바이트
};

Bytes build_message(const MessageSpec& spec) {
    Bytes out;
    put_u16(out, spec.type);
    put_u16(out, spec.declared_length.value_or(static_cast<std::uint16_t>(spec.body.size())));
    put_u32(out, spec.cookie);
    for (const std::byte b : spec.txid) {
        out.push_back(b);
    }
    put_all(out, spec.body);
    if (spec.first_byte) {
        out[0] = static_cast<std::byte>(*spec.first_byte & 0xFFu);
    }
    return out;
}

Bytes success_with(const Bytes& body) {
    MessageSpec spec;
    spec.body = body;
    return build_message(spec);
}

Bytes error_with(const Bytes& body) {
    MessageSpec spec;
    spec.type = kErrorResponse;
    spec.body = body;
    return build_message(spec);
}

Bytes concat(std::initializer_list<Bytes> parts) {
    Bytes out;
    for (const auto& part : parts) {
        put_all(out, part);
    }
    return out;
}

struct ParseCase {
    std::string_view why;
    Bytes datagram;
    StunParseError expected;
    std::optional<Endpoint> mapped;        // 통과할 때만 본다
    std::optional<std::uint16_t> code;     // 통과할 때만 본다
    StunMessageType type = StunMessageType::kBindingSuccessResponse;
};

ParseCase ok_case(std::string_view why, Bytes datagram, std::optional<Endpoint> mapped) {
    return ParseCase{why, std::move(datagram), StunParseError::kNone, mapped, std::nullopt,
                     StunMessageType::kBindingSuccessResponse};
}

ParseCase error_response_case(std::string_view why, Bytes datagram,
                              std::optional<std::uint16_t> code) {
    return ParseCase{why, std::move(datagram), StunParseError::kNone, std::nullopt, code,
                     StunMessageType::kBindingErrorResponse};
}

ParseCase bad_case(std::string_view why, Bytes datagram, StunParseError expected) {
    return ParseCase{why, std::move(datagram), expected, std::nullopt, std::nullopt,
                     StunMessageType::kBindingSuccessResponse};
}

const Endpoint kMapped{kMappedAddress, kMappedPort};

std::vector<ParseCase> make_parse_cases() {
    std::vector<ParseCase> cases;

    // --- 통과하는 것 ---
    cases.push_back(ok_case("only XOR-MAPPED-ADDRESS", success_with(mapped_ipv4(kMappedAddress, kMappedPort)), kMapped));
    cases.push_back(ok_case("lowest address and port",
                            success_with(mapped_ipv4(0x00000000u, 0u)), Endpoint{0x00000000u, 0u}));
    cases.push_back(ok_case("highest address and port", success_with(mapped_ipv4(0xFFFFFFFFu, 65535u)),
                            Endpoint{0xFFFFFFFFu, 65535u}));
    cases.push_back(ok_case("cookie-valued address and port decodes to zero after xor",
                            success_with(mapped_ipv4(kCookie, 0x2112u)), Endpoint{kCookie, 0x2112u}));
    cases.push_back(ok_case("an unread attribute with 3 padding bytes comes first",
                            success_with(concat({attribute(kSoftware, filler(5)),
                                                 mapped_ipv4(kMappedAddress, kMappedPort)})),
                            kMapped));
    cases.push_back(ok_case("an unread attribute of length 0 comes last",
                            success_with(concat({mapped_ipv4(kMappedAddress, kMappedPort),
                                                 attribute(kSoftware, Bytes{})})),
                            kMapped));
    cases.push_back(ok_case("a zero-length attribute between two read attributes",
                            success_with(concat({attribute(kSoftware, Bytes{}),
                                                 mapped_ipv4(kMappedAddress, kMappedPort)})),
                            kMapped));
    cases.push_back(ok_case("ERROR-CODE in a success response is skipped",
                            success_with(concat({mapped_ipv4(kMappedAddress, kMappedPort),
                                                 attribute(kErrorCode, error_code_value(4, 0, "Bad Request"))})),
                            kMapped));

    cases.push_back(error_response_case("ERROR-CODE 400",
                                        error_with(attribute(kErrorCode, error_code_value(4, 0, "Bad Request"))), std::uint16_t{400}));
    cases.push_back(error_response_case("ERROR-CODE 420 with a reason that needs padding",
                                        error_with(attribute(kErrorCode, error_code_value(4, 20, "Unknown Attribute"))), std::uint16_t{420}));
    cases.push_back(error_response_case("ERROR-CODE 300 is the lowest class",
                                        error_with(attribute(kErrorCode, error_code_value(3, 0, ""))), std::uint16_t{300}));
    cases.push_back(error_response_case("ERROR-CODE 699 is the highest accepted",
                                        error_with(attribute(kErrorCode, error_code_value(6, 99, ""))), std::uint16_t{699}));
    cases.push_back(error_response_case("the reserved bits above the class are ignored",
                                        error_with(attribute(kErrorCode, error_code_value(0xF4, 1, ""))), std::uint16_t{401}));
    cases.push_back(error_response_case("no ERROR-CODE at all", error_with(Bytes{}), std::nullopt));
    cases.push_back(error_response_case("ERROR-CODE shorter than its fixed part",
                                        error_with(attribute(kErrorCode, filler(3, 0x00))), std::nullopt));
    cases.push_back(error_response_case("class 2 is below the RFC range",
                                        error_with(attribute(kErrorCode, error_code_value(2, 0, ""))), std::nullopt));
    cases.push_back(error_response_case("class 7 is above the RFC range",
                                        error_with(attribute(kErrorCode, error_code_value(7, 0, ""))), std::nullopt));
    cases.push_back(error_response_case("number 100 is out of range",
                                        error_with(attribute(kErrorCode, error_code_value(4, 100, ""))), std::nullopt));
    cases.push_back(error_response_case("the first ERROR-CODE wins",
                                        error_with(concat({attribute(kErrorCode, error_code_value(4, 1, "")),
                                                           attribute(kErrorCode, error_code_value(5, 0, ""))})), std::uint16_t{401}));
    cases.push_back(error_response_case("a malformed first ERROR-CODE is not replaced by the second",
                                        error_with(concat({attribute(kErrorCode, filler(3, 0x00)),
                                                           attribute(kErrorCode, error_code_value(5, 0, ""))})),
                                        std::nullopt));
    cases.push_back(error_response_case("XOR-MAPPED-ADDRESS in an error response is not decoded",
                                        error_with(mapped_ipv4(kMappedAddress, kMappedPort)), std::nullopt));
    cases.push_back(error_response_case("a bad family in an error response is not judged",
                                        error_with(attribute(kXorMappedAddress,
                                                             xor_mapped_value(0x02, kOtherAddress, kOtherPort))),
                                        std::nullopt));

    // --- 1~2. 헤더가 있는지, STUN 인지 ---
    cases.push_back(bad_case("empty datagram", Bytes{}, StunParseError::kTooShort));
    {
        Bytes short_one = success_with(Bytes{});
        short_one.pop_back();  // 19 바이트
        cases.push_back(bad_case("one byte short of a header", short_one, StunParseError::kTooShort));
    }
    {
        MessageSpec spec;
        spec.body = mapped_ipv4(kMappedAddress, kMappedPort);
        spec.first_byte = 0x80u;  // 상위 2비트 10
        cases.push_back(bad_case("leading bits 10", build_message(spec), StunParseError::kNotStun));
        spec.first_byte = 0x40u;  // 상위 2비트 01. TUNNEL_MAGIC 의 첫 바이트가 여기다
        cases.push_back(bad_case("leading bits 01", build_message(spec), StunParseError::kNotStun));
        spec.first_byte = 0xC0u;
        cases.push_back(bad_case("leading bits 11", build_message(spec), StunParseError::kNotStun));
    }

    // --- 3~4. 쿠키와 트랜잭션 ID ---
    {
        MessageSpec spec;
        spec.body = mapped_ipv4(kMappedAddress, kMappedPort);
        spec.cookie = 0x2112A443u;  // 마지막 비트만 다르다
        cases.push_back(bad_case("cookie off by one bit", build_message(spec), StunParseError::kBadCookie));
        spec.cookie = 0x00000000u;
        cases.push_back(bad_case("zero cookie", build_message(spec), StunParseError::kBadCookie));
    }
    {
        MessageSpec spec;
        spec.body = mapped_ipv4(kMappedAddress, kMappedPort);
        spec.txid = kTxid;
        spec.txid[11] = std::byte{0x5D};  // 마지막 바이트만 다르다
        cases.push_back(bad_case("transaction id differs in the last byte", build_message(spec),
                                 StunParseError::kTransactionMismatch));
        spec.txid = kTxid;
        spec.txid[0] = std::byte{0xA0};
        cases.push_back(bad_case("transaction id differs in the first byte", build_message(spec),
                                 StunParseError::kTransactionMismatch));
    }

    // --- 5. 종류 ---
    {
        MessageSpec spec;
        spec.type = kRequest;
        cases.push_back(bad_case("our own request echoed back", build_message(spec),
                                 StunParseError::kUnknownType));
        spec.type = std::uint16_t{0x0011};  // Binding Indication
        cases.push_back(bad_case("binding indication", build_message(spec), StunParseError::kUnknownType));
        spec.type = std::uint16_t{0x0102};
        cases.push_back(bad_case("a response to some other method", build_message(spec),
                                 StunParseError::kUnknownType));
    }

    // --- 6. 길이 정렬 ---
    {
        MessageSpec spec;
        // 몸통 10 바이트. 길이 필드와 실제 길이는 맞지만 4의 배수가 아니다.
        spec.body = concat({attribute_raw(kXorMappedAddress, 8, filler(6))});
        cases.push_back(bad_case("length field is not a multiple of 4", build_message(spec),
                                 StunParseError::kLengthNotAligned));
    }

    // --- 7. 길이 일치 ---
    {
        MessageSpec spec;
        spec.body = mapped_ipv4(kMappedAddress, kMappedPort);  // 12 바이트
        spec.declared_length = std::uint16_t{16};
        cases.push_back(bad_case("length field is larger than the datagram", build_message(spec),
                                 StunParseError::kLengthMismatch));
        spec.declared_length = std::uint16_t{8};
        cases.push_back(bad_case("length field is smaller than the datagram", build_message(spec),
                                 StunParseError::kLengthMismatch));
        spec.declared_length = std::uint16_t{0};
        cases.push_back(bad_case("length field says there are no attributes", build_message(spec),
                                 StunParseError::kLengthMismatch));
    }

    // --- 8. 속성 경계 ---
    {
        // 값 길이 8 이라고 적고 4 바이트만 싣는다.
        cases.push_back(bad_case("attribute value runs past the end",
                                 success_with(attribute_raw(kXorMappedAddress, 8, filler(4))),
                                 StunParseError::kAttributeTruncated));
        // 값 길이 5 는 패딩까지 8 바이트를 요구하는데 4 바이트뿐이다.
        cases.push_back(bad_case("padding of the last attribute is missing",
                                 success_with(attribute_raw(kSoftware, 5, filler(4))),
                                 StunParseError::kAttributeTruncated));
        // 앞 속성은 멀쩡하고 뒤 속성이 잘렸다.
        cases.push_back(bad_case("a later attribute is truncated",
                                 success_with(concat({mapped_ipv4(kMappedAddress, kMappedPort),
                                                      attribute_raw(kSoftware, 0xFFFFu, filler(4))})),
                                 StunParseError::kAttributeTruncated));
    }

    // --- 9~10. XOR-MAPPED-ADDRESS 의 주소군과 길이 ---
    {
        Bytes ipv6_value;
        put_u8(ipv6_value, 0x00);
        put_u8(ipv6_value, 0x02);  // IPv6
        put_u16(ipv6_value, 0x1234u);
        put_all(ipv6_value, filler(16));
        cases.push_back(bad_case("family is IPv6",
                                 success_with(attribute(kXorMappedAddress, ipv6_value)),
                                 StunParseError::kMappedAddressFamily));

        cases.push_back(bad_case("family is IPv6 with an IPv4-sized value",
                                 success_with(attribute(kXorMappedAddress,
                                                        xor_mapped_value(0x02, kMappedAddress, kMappedPort))),
                                 StunParseError::kMappedAddressFamily));
        cases.push_back(bad_case("family is 0",
                                 success_with(attribute(kXorMappedAddress,
                                                        xor_mapped_value(0x00, kMappedAddress, kMappedPort))),
                                 StunParseError::kMappedAddressFamily));
        cases.push_back(bad_case("family is unassigned",
                                 success_with(attribute(kXorMappedAddress,
                                                        xor_mapped_value(0x03, kMappedAddress, kMappedPort))),
                                 StunParseError::kMappedAddressFamily));

        Bytes short_ipv4;
        put_u8(short_ipv4, 0x00);
        put_u8(short_ipv4, 0x01);
        put_u16(short_ipv4, 0x0000u);
        cases.push_back(bad_case("IPv4 value holds no address",
                                 success_with(attribute(kXorMappedAddress, short_ipv4)),
                                 StunParseError::kMappedAddressMalformed));

        Bytes long_ipv4 = xor_mapped_value(0x01, kMappedAddress, kMappedPort);
        put_all(long_ipv4, filler(4));
        cases.push_back(bad_case("IPv4 value has four bytes too many",
                                 success_with(attribute(kXorMappedAddress, long_ipv4)),
                                 StunParseError::kMappedAddressMalformed));

        cases.push_back(bad_case("the attribute is empty so the family cannot be read",
                                 success_with(attribute(kXorMappedAddress, Bytes{})),
                                 StunParseError::kMappedAddressMalformed));
        cases.push_back(bad_case("only the reserved byte is present",
                                 success_with(attribute(kXorMappedAddress, filler(1, 0x00))),
                                 StunParseError::kMappedAddressMalformed));
    }

    // --- 11. 성공 응답에 주소가 없다 ---
    cases.push_back(bad_case("success response with no attributes", success_with(Bytes{}),
                             StunParseError::kMissingMappedAddress));
    cases.push_back(bad_case("success response with only an unread attribute",
                             success_with(attribute(kSoftware, filler(8))),
                             StunParseError::kMissingMappedAddress));
    cases.push_back(bad_case("success response carrying only an ERROR-CODE",
                             success_with(attribute(kErrorCode, error_code_value(4, 0, "Bad Request"))),
                             StunParseError::kMissingMappedAddress));

    // --- 12. 성공 응답에 XOR-MAPPED-ADDRESS 가 두 개 이상이다 ---
    //
    // protocol.md 13장: 폐기한다. 둘이 같은 주소인지 보지 않는다. 이 값은 로그용이 아니라
    // 우리가 상대에게 알릴 공인 엔드포인트가 되므로 모호하면 고르지 않는다.
    {
        cases.push_back(bad_case("two XOR-MAPPED-ADDRESS with different addresses",
                                 success_with(concat({mapped_ipv4(kMappedAddress, kMappedPort),
                                                      mapped_ipv4(kOtherAddress, kOtherPort)})),
                                 StunParseError::kDuplicateMappedAddress));
        // 같은 값이 두 번 와도 마찬가지다. 값을 비교해 봐주지 않는다.
        cases.push_back(bad_case("two XOR-MAPPED-ADDRESS with the same address",
                                 success_with(concat({mapped_ipv4(kMappedAddress, kMappedPort),
                                                      mapped_ipv4(kMappedAddress, kMappedPort)})),
                                 StunParseError::kDuplicateMappedAddress));
        // 두 번째를 해석하지 않으므로 그것이 망가져 있어도 이유는 중복이다.
        cases.push_back(bad_case("the second XOR-MAPPED-ADDRESS has a bad family",
                                 success_with(concat({mapped_ipv4(kMappedAddress, kMappedPort),
                                                      attribute(kXorMappedAddress,
                                                                xor_mapped_value(0x02, kOtherAddress, kOtherPort))})),
                                 StunParseError::kDuplicateMappedAddress));
        cases.push_back(bad_case("three XOR-MAPPED-ADDRESS",
                                 success_with(concat({mapped_ipv4(kMappedAddress, kMappedPort),
                                                      mapped_ipv4(kOtherAddress, kOtherPort),
                                                      mapped_ipv4(kMappedAddress, kMappedPort)})),
                                 StunParseError::kDuplicateMappedAddress));
        // 사이에 다른 속성이 끼어도 센다.
        cases.push_back(bad_case("two XOR-MAPPED-ADDRESS with another attribute between them",
                                 success_with(concat({mapped_ipv4(kMappedAddress, kMappedPort),
                                                      attribute(kSoftware, filler(4)),
                                                      mapped_ipv4(kOtherAddress, kOtherPort)})),
                                 StunParseError::kDuplicateMappedAddress));
        // **첫 속성의 결함이 중복보다 먼저다.** 앞쪽에서 걸린 것이 이긴다 (stun.hpp 검사 순서).
        cases.push_back(bad_case("a bad first XOR-MAPPED-ADDRESS beats the duplicate",
                                 success_with(concat({attribute(kXorMappedAddress,
                                                                xor_mapped_value(0x02, kMappedAddress, kMappedPort)),
                                                      mapped_ipv4(kOtherAddress, kOtherPort)})),
                                 StunParseError::kMappedAddressFamily));
        // **구조가 깨진 것이 중복보다 먼저다.** 순회를 끝까지 하므로 뒤쪽도 검사를 받는다.
        cases.push_back(bad_case("a truncated attribute after the duplicate wins",
                                 success_with(concat({mapped_ipv4(kMappedAddress, kMappedPort),
                                                      mapped_ipv4(kOtherAddress, kOtherPort),
                                                      attribute_raw(kSoftware, 0xFFFFu, filler(4))})),
                                 StunParseError::kAttributeTruncated));
        // 오류 응답에서는 이 속성을 해석하지 않으므로 두 개여도 폐기하지 않는다.
        cases.push_back(error_response_case("two XOR-MAPPED-ADDRESS in an error response",
                                            error_with(concat({mapped_ipv4(kMappedAddress, kMappedPort),
                                                               mapped_ipv4(kOtherAddress, kOtherPort),
                                                               attribute(kErrorCode, error_code_value(4, 0, ""))})),
                                            std::uint16_t{400}));
    }

    return cases;
}

// 손으로 적은 정상 응답. 203.0.113.7:51000 이고 XOR 값을 손으로 계산했다.
//   포트 51000 = 0xC738, 0xC738 ^ 0x2112 = 0xE62A
//   주소 203.0.113.7 = 0xCB007107, 0xCB007107 ^ 0x2112A442 = 0xEA12D545
constexpr std::uint8_t kGolden[] = {
    0x01, 0x01,                          // Binding Success Response
    0x00, 0x0C,                          // 길이 12 (속성 하나)
    0x21, 0x12, 0xA4, 0x42,              // magic cookie
    0xA1, 0xB2, 0xC3, 0xD4,              // 트랜잭션 ID
    0xE5, 0xF6, 0x07, 0x18,
    0x29, 0x3A, 0x4B, 0x5C,
    0x00, 0x20,                          // XOR-MAPPED-ADDRESS
    0x00, 0x08,                          // 값 8 바이트
    0x00, 0x01,                          // 예약, family IPv4
    0xE6, 0x2A,                          // X-Port
    0xEA, 0x12, 0xD5, 0x45,              // X-Address
};

Bytes bytes_of(std::span<const std::uint8_t> raw) {
    Bytes out;
    for (const std::uint8_t value : raw) {
        put_u8(out, value);
    }
    return out;
}

}  // namespace

TEST_CASE("stun: golden binding success response decodes by hand-checked xor", "[stun]") {
    const Bytes datagram = bytes_of(kGolden);
    REQUIRE(datagram.size() == 32u);

    const auto result = parse_binding_response(datagram, kTxid);
    REQUIRE(result.ok());
    REQUIRE(result.response.type == StunMessageType::kBindingSuccessResponse);
    REQUIRE(result.response.mapped.has_value());
    REQUIRE(result.response.mapped->to_string() == "203.0.113.7:51000");
    REQUIRE(!result.response.error_code.has_value());
}

TEST_CASE("stun: the builder and the golden vector agree on the header", "[stun]") {
    const auto request = build_binding_request(kTxid);
    const Bytes golden = bytes_of(kGolden);

    // 종류와 길이만 다르고 쿠키와 트랜잭션 ID 는 같은 자리에 같은 값으로 놓인다.
    REQUIRE(request.size() == kStunHeaderSize);
    REQUIRE(request[0] == std::byte{0x00});
    REQUIRE(request[1] == std::byte{0x01});
    REQUIRE(request[2] == std::byte{0x00});
    REQUIRE(request[3] == std::byte{0x00});
    for (std::size_t i = 4; i < kStunHeaderSize; ++i) {
        INFO("offset=" << i);
        REQUIRE(request[i] == golden[i]);
    }
}

TEST_CASE("stun: build_binding_request carries the transaction id it was given", "[stun]") {
    TransactionId other = kTxid;
    other[0] = std::byte{0x00};
    other[11] = std::byte{0xFF};

    const auto a = build_binding_request(kTxid);
    const auto b = build_binding_request(other);
    REQUIRE(a != b);

    const auto peeked = peek_transaction_id(std::span<const std::byte>(b));
    REQUIRE(peeked.has_value());
    REQUIRE(*peeked == other);
}

TEST_CASE("stun: parse_binding_response case table", "[stun]") {
    for (const auto& c : make_parse_cases()) {
        INFO("case=[" << c.why << "] size=" << c.datagram.size());
        const auto result = parse_binding_response(c.datagram, kTxid);
        REQUIRE(result.error == c.expected);
        if (result.ok()) {
            REQUIRE(result.response.type == c.type);
            REQUIRE(result.response.mapped.has_value() == c.mapped.has_value());
            if (c.mapped) {
                REQUIRE(*result.response.mapped == *c.mapped);
            }
            REQUIRE(result.response.error_code.has_value() == c.code.has_value());
            if (c.code) {
                REQUIRE(*result.response.error_code == *c.code);
            }
        } else {
            // 실패한 판정은 값을 내지 않는다.
            REQUIRE(!result.response.mapped.has_value());
            REQUIRE(!result.response.error_code.has_value());
        }
    }
}

TEST_CASE("stun: every case in the table is reachable", "[stun]") {
    // 표가 비거나 한쪽으로 쏠리면 위 케이스가 아무것도 지키지 않는다.
    const auto cases = make_parse_cases();
    std::size_t accepted = 0;
    std::size_t rejected = 0;
    for (const auto& c : cases) {
        if (c.expected == StunParseError::kNone) {
            ++accepted;
        } else {
            ++rejected;
        }
    }
    REQUIRE(accepted >= 20u);
    REQUIRE(rejected >= 25u);
}

TEST_CASE("stun: peek_transaction_id looks at the header only", "[stun]") {
    const Bytes golden = bytes_of(kGolden);

    SECTION("a valid response gives its id") {
        const auto id = peek_transaction_id(golden);
        REQUIRE(id.has_value());
        REQUIRE(*id == kTxid);
    }
    SECTION("it does not judge the message type") {
        MessageSpec spec;
        spec.type = kRequest;
        const Bytes request = build_message(spec);
        const auto id = peek_transaction_id(request);
        REQUIRE(id.has_value());
        REQUIRE(*id == kTxid);
    }
    SECTION("it does not judge the length field") {
        MessageSpec spec;
        spec.body = mapped_ipv4(kMappedAddress, kMappedPort);
        spec.declared_length = std::uint16_t{0xFFFF};
        const auto id = peek_transaction_id(build_message(spec));
        REQUIRE(id.has_value());
        REQUIRE(*id == kTxid);
    }
    SECTION("a short datagram has no id") {
        Bytes shorter = golden;
        shorter.resize(19);
        REQUIRE(!peek_transaction_id(shorter).has_value());
        REQUIRE(!peek_transaction_id(Bytes{}).has_value());
    }
    SECTION("a bad cookie has no id") {
        Bytes bad = golden;
        bad[7] = std::byte{0x43};
        REQUIRE(!peek_transaction_id(bad).has_value());
    }
    SECTION("leading bits other than 00 have no id") {
        for (const unsigned first : {0x40u, 0x80u, 0xC0u}) {
            INFO("first byte=" << first);
            Bytes bad = golden;
            bad[0] = static_cast<std::byte>(first);
            REQUIRE(!peek_transaction_id(bad).has_value());
        }
    }
    SECTION("a datagram of exactly the header size is enough") {
        MessageSpec spec;
        spec.type = kRequest;
        const Bytes header_only = build_message(spec);
        REQUIRE(header_only.size() == kStunHeaderSize);
        REQUIRE(peek_transaction_id(header_only).has_value());
    }
}

TEST_CASE("stun: the xor decode round-trips every address and port in a table", "[stun]") {
    struct MappedCase {
        std::uint32_t address;
        std::uint16_t port;
        std::string_view text;
    };
    constexpr MappedCase kMappedCases[] = {
        {0x00000000u, 0u, "0.0.0.0:0"},
        {0xFFFFFFFFu, 65535u, "255.255.255.255:65535"},
        {0xCB007107u, 51000u, "203.0.113.7:51000"},
        {0xC0000201u, 3478u, "192.0.2.1:3478"},
        {0x2112A442u, 0x2112u, "33.18.164.66:8466"},   // 쿠키와 같은 값
        {0x0A640001u, 19302u, "10.100.0.1:19302"},
    };

    for (const auto& c : kMappedCases) {
        INFO("expected=" << c.text);
        const auto result =
            parse_binding_response(success_with(mapped_ipv4(c.address, c.port)), kTxid);
        REQUIRE(result.ok());
        REQUIRE(result.response.mapped.has_value());
        REQUIRE(result.response.mapped->to_string() == std::string(c.text));
    }
}
