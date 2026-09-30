#pragma once

// STUN 메시지 구성과 파싱 (protocol.md 13장 STUN 사용 범위).
//
// 그 절이 범위를 정한다. 메시지는 Binding Request(`0x0001`), Binding Success
// Response(`0x0101`), Binding Error Response(`0x0111`) 셋이다. Binding Request 는 헤더만
// 보내고 속성을 싣지 않는다. 해석하는 속성은 `XOR-MAPPED-ADDRESS`(`0x0020`) 하나이고
// `ERROR-CODE` 는 값만 돌려준다 (로그용). 나머지 속성은 건너뛴다. FINGERPRINT,
// MESSAGE-INTEGRITY, 인증, TURN, ICE 는 없다.
//
// **순수 함수만 둔다.** 소켓도 타이머도 난수도 여기 없다. 트랜잭션 ID 는 인자로 받는다.
// 그래야 시험이 값을 고정할 수 있다. 재시도와 마감(protocol.md 11장 타이머), 서버 선택
// (architecture.md 3.5 기동 입력), 카운터와 로그는 부르는 쪽의 일이다. 이 모듈은 카운터를
// 올리지 않고 로그도 내지 않는다.
//
// 값의 출처는 둘이다. 메시지 종류 셋과 `XOR-MAPPED-ADDRESS` 의 속성 번호, IPv4 family
// `0x01` 은 protocol.md 13장이 적었다. 속성 헤더 4바이트, 4바이트 정렬, `ERROR-CODE` 의
// 속성 번호와 class/number 인코딩은 그 절이 "RFC 5389 중" 이라고 가리킨 RFC 5389 다.
// magic cookie 는 protocol_constants.hpp 의 `kStunCookie` 를 쓴다. 여기서 새로 짓지 않는다.
//
// ## 판정 자세
//
// 바깥에서 온 바이트를 판정하는 코드다. 검증을 먼저 하고 확인하지 못한 것은 받지 않는다.
// 실패는 이유를 구분해 돌려준다. 부르는 쪽이 그 이유로 카운터와 로그를 가른다.
//
// **모호한 입력은 고르지 않는다.** 성공 응답에 `XOR-MAPPED-ADDRESS` 가 두 개 이상이면
// 폐기한다. 둘이 같은 주소인지도 보지 않는다 (protocol.md 13장 "같은 속성이 두 번 온 성공
// 응답"). 이 값은 로그용이 아니라 우리가 상대에게 알릴 공인 엔드포인트가 되므로, 틀린
// 것을 고르면 그 뒤의 홀펀칭이 통째로 엉뚱한 곳을 향한다. RFC 5389 는 첫 번째를 쓰라고
// 적지만 그 기본값을 여기서는 쓰지 않는다.
//
// ### 검사 순서
//
// 위에서부터 보고 처음 걸린 자리에서 멈춘다. 순서를 정해 두어야 같은 입력이 늘 같은
// 이유를 낸다. 3, 4, 7, 8 은 13장의 "응답 검증" 네 가지이고 나머지는 그 넷을 돌리는 데
// 필요한 자리다.
//
// | # | 검사 | 어긋나면 |
// |---|------|----------|
// | 1 | 받은 바이트가 20 이상 | `kTooShort` |
// | 2 | 첫 바이트의 상위 2비트가 `00` (protocol.md 7장 분류) | `kNotStun` |
// | 3 | magic cookie 가 `kStunCookie` | `kBadCookie` |
// | 4 | 트랜잭션 ID 가 기대값과 같음 | `kTransactionMismatch` |
// | 5 | 메시지 종류가 `0x0101` 또는 `0x0111` | `kUnknownType` |
// | 6 | 길이 필드가 4의 배수 | `kLengthNotAligned` |
// | 7 | `20 + 길이 필드` 가 받은 바이트 수와 같음 | `kLengthMismatch` |
// | 8 | 속성 순회가 경계를 넘지 않음 | `kAttributeTruncated` |
// | 9 | (성공 응답) `XOR-MAPPED-ADDRESS` 의 family 가 IPv4 | `kMappedAddressFamily` |
// | 10 | (성공 응답) 그 속성의 값 길이가 8 | `kMappedAddressMalformed` |
// | 11 | (성공 응답) 그 속성이 하나 이상 있었음 | `kMissingMappedAddress` |
// | 12 | (성공 응답) 그 속성이 **하나뿐이었음** | `kDuplicateMappedAddress` |
//
// 8~10 은 순회 안에서 일어나므로 **앞쪽 속성에서 걸린 것이 이긴다.** 11 과 12 는 순회를
// 마친 뒤에 본다. 둘은 같은 축의 양끝이라(0개와 2개 이상) 함께 걸리지 않는다.
//
// **12 가 순회 중에 즉시 실패하지 않는 이유.** 두 번째 속성을 만난 자리에서 돌아서면 그
// 뒤의 바이트가 경계 검사를 받지 못한다. 표시만 해 두고 순회는 끝까지 한다. 그래서 중복
// 뒤에 잘린 속성이 있으면 `kAttributeTruncated` 가 이긴다. 구조가 깨진 것을 먼저 말하는
// 편이 낫고, 어느 쪽이든 폐기라 카운터는 같다 (protocol.md 13장 폐기 카운터).
//
// ### 이 코드가 정한 것
//
// 13장이 이름만 적고 넘어간 자리다. 표 밖의 규칙을 만들지 않는다.
//
// | 자리 | 정한 것 | 왜 |
// |------|---------|-----|
// | 길이 필드가 4의 배수가 아니다 | 폐기 | 속성은 4바이트 경계에 놓이므로 그 길이로 끝나는 속성 열이 없다. 순회를 시작하기 전에 거른다 |
// | 속성 헤더 4바이트가 안 남았다 | 폐기 | 13장 "속성 순회 중 경계 초과 시 중단" |
// | 속성 값이 패딩까지 남은 바이트를 넘는다 | 폐기 | 같은 규칙. 길이 필드는 패딩을 포함한다 |
// | 값 길이가 4의 배수가 아니다 | 4의 배수로 올린 만큼 건너뛴다 | RFC 5389 의 4바이트 정렬. 패딩 바이트의 내용은 보지 않는다 |
// | 값 길이가 0 이다 | 헤더 4바이트만 건너뛴다 | 경계를 넘지 않는다. 늘 4 이상 전진하므로 순회가 멈추지 않는 일이 없다 |
// | `ERROR-CODE` 가 두 번 온다 | 처음 것만 쓰고 뒤엣것은 건너뛴다. 순회는 끝까지 한다 | protocol.md 13장이 정했다. 그 값은 로그에만 쓰이고 판정에 들어가지 않는다. 끝까지 도는 덕분에 뒤쪽 바이트도 경계 검사를 받는다 |
// | 오류 응답에 `XOR-MAPPED-ADDRESS` 가 있다 | 해석하지 않고 건너뛴다 | 거절당한 요청에서 주소를 얻지 않는다. 13장이 오류 응답에 요구하는 것은 `ERROR-CODE` 값뿐이다 |
// | 성공 응답에 `ERROR-CODE` 가 있다 | 건너뛴다 | 위의 짝이다. 성공 응답의 오류 코드는 뜻이 없다 |
// | `ERROR-CODE` 가 없거나 형식이 어긋난다 | 응답은 통과시키고 코드만 비운다 | 그 값은 로그용이다. 읽지 못하면 모른다고 두는 것이 지어내는 것보다 낫다 |
// | `ERROR-CODE` 의 class 가 3~6 밖이거나 number 가 99 를 넘는다 | 같다. 코드를 비운다 | RFC 5389 가 정한 범위 밖이다 |
// | `ERROR-CODE` 의 class 바이트 상위 5비트 | 보지 않는다 | RFC 5389 의 class 는 그 바이트의 하위 3비트다 |
// | 요청(`0x0001`)이 그대로 돌아왔다 | 폐기 (`kUnknownType`) | 쿠키와 트랜잭션 ID 가 맞아도 응답이 아니다 |

#include "sangtachi/network/endpoint.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace sangtachi::network {

// 96비트 트랜잭션 ID.
using TransactionId = std::array<std::byte, 12>;

// STUN 헤더. 종류 2 + 길이 2 + 쿠키 4 + 트랜잭션 ID 12.
inline constexpr std::size_t kStunHeaderSize = 20;

// protocol.md 13장이 적은 메시지 종류 셋.
enum class StunMessageType : std::uint16_t {
    kBindingRequest = 0x0001,
    kBindingSuccessResponse = 0x0101,
    kBindingErrorResponse = 0x0111,
};

// 해석하는 속성 (protocol.md 13장). 번호는 RFC 5389 의 등록값이다.
inline constexpr std::uint16_t kAttrXorMappedAddress = 0x0020;
inline constexpr std::uint16_t kAttrErrorCode = 0x0009;

// `XOR-MAPPED-ADDRESS` 의 주소군. IPv4 가 아니면 폐기한다 (protocol.md 13장).
inline constexpr std::uint8_t kAddressFamilyIpv4 = 0x01;

// 파싱 실패의 이유. 위 "검사 순서" 표의 오른쪽 칸이다.
enum class StunParseError : std::uint8_t {
    kNone = 0,               // 오류 없음
    kTooShort,               // 받은 바이트가 헤더보다 작다
    kNotStun,                // 첫 바이트의 상위 2비트가 00 이 아니다
    kBadCookie,              // magic cookie 가 다르다
    kTransactionMismatch,    // 트랜잭션 ID 가 기대값과 다르다
    kUnknownType,            // Binding 성공/오류 응답이 아니다
    kLengthNotAligned,       // 길이 필드가 4의 배수가 아니다
    kLengthMismatch,         // 길이 필드가 실제 바이트 수와 어긋난다
    kAttributeTruncated,     // 속성 순회가 경계를 넘었다
    kMappedAddressFamily,    // XOR-MAPPED-ADDRESS 의 family 가 IPv4 가 아니다
    kMappedAddressMalformed, // IPv4 인데 값 길이가 8 이 아니다
    kMissingMappedAddress,   // 성공 응답인데 그 속성이 없다
    kDuplicateMappedAddress, // 성공 응답에 그 속성이 두 개 이상이다
};

// 해석한 응답. `error` 가 `kNone` 일 때만 값이 있다.
struct StunResponse {
    // 성공 응답이면 kBindingSuccessResponse, 오류 응답이면 kBindingErrorResponse 다.
    StunMessageType type = StunMessageType::kBindingSuccessResponse;

    // 성공 응답의 공인 엔드포인트. 오류 응답에서는 비어 있다.
    std::optional<Endpoint> mapped;

    // 오류 응답의 `ERROR-CODE` (class * 100 + number). 로그용이고, 읽지 못하면 비어 있다.
    std::optional<std::uint16_t> error_code;
};

struct StunParseResult {
    StunParseError error = StunParseError::kNone;
    StunResponse response;

    [[nodiscard]] constexpr bool ok() const noexcept { return error == StunParseError::kNone; }
};

// Binding Request 한 통. 헤더 20바이트뿐이고 속성이 없다 (protocol.md 13장 송신 속성).
//
// 트랜잭션 ID 를 뽑는 자리는 여기가 아니다. 13장이 OS CSPRNG 로 정했고 그 호출은
// platform/random.hpp 가 갖는다.
[[nodiscard]] std::array<std::byte, kStunHeaderSize> build_binding_request(
    const TransactionId& transaction_id) noexcept;

// 데이터그램에서 트랜잭션 ID 만 꺼낸다. 대기 중인 요청을 찾는 자리에 쓴다
// (protocol.md 7장 분류의 "트랜잭션 ID로 대기 중인 요청과 매칭").
//
// **검증이 아니다.** 길이 20 이상, 상위 2비트 `00`, magic cookie 만 보고 나머지는 보지
// 않는다. 찾은 요청의 트랜잭션 ID 로 parse_binding_response 를 다시 불러야 한다.
[[nodiscard]] std::optional<TransactionId> peek_transaction_id(
    std::span<const std::byte> datagram) noexcept;

// Binding Response 한 통을 판정한다. 검사와 순서는 이 파일 머리의 표가 갖는다.
//
// `expected` 는 이 응답을 기다리던 요청의 트랜잭션 ID 다.
[[nodiscard]] StunParseResult parse_binding_response(std::span<const std::byte> datagram,
                                                     const TransactionId& expected) noexcept;

}  // namespace sangtachi::network
