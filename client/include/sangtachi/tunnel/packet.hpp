#pragma once

// 터널 패킷의 와이어 코덱 (protocol.md 4.1 와이어 레이아웃, 5장 패킷 타입, 8.1 공통 검사).
//
// **순수 함수만 둔다.** 소켓도 시계도 세션도 여기 없다. 카운터를 올리지 않고 로그도 내지
// 않는다. 판정 함수는 실패의 종류만 돌려주고, 그 종류가 가리키는 카운터를 올리는 것은
// 수신 파이프라인의 일이다. 종류마다 그 절이 정한 카운터 이름이 하나씩 붙어 있다
// (`counter_name`).
//
// 값의 출처. 헤더 오프셋과 바이트 오더는 4.1 이다. 모든 다중 바이트 정수는 빅엔디안이다
// (1장). 타입 값과 페이로드 길이는 5장 표, 페이로드 배치는 5.1~5.7 이다. 3장 상수는
// protocol_constants.hpp 를 쓰고 여기서 새로 짓지 않는다. 표시 이름의 문자 집합은
// control_plane.md 2.7 이다.
//
// **구조체를 memcpy 하지 않는다** (4.1). 필드를 하나씩 바이트로 옮긴다.
//
// ## 어디까지가 이 모듈인가
//
// 8.1 공통 검사 여덟 가운데 1~6 은 데이터그램 바이트만 보고 판정한다. 그래서 여기 있다
// (`check_common`). 7(`drop_unknown_peer`)과 8(`drop_retired_epoch`)은 세션 표와 폐기
// 목록을 봐야 하므로 수신 파이프라인이 한다. 8.2 의 `virtual_ip` 대조, epoch, nonce 도
// 세션 상태라 여기 없다. 8.4 `DATA` 내부 검증도 여기 없다. 이 모듈은 `DATA` 페이로드를
// 해석하지 않고 그대로 넘긴다.
//
// 타입별 디코더가 보는 것은 그 절이 페이로드 바이트에 대해 정한 규칙뿐이다.
//
// | 타입 | 디코더가 보는 것 | 실패 |
// |------|------------------|------|
// | `HELLO`, `HELLO_ACK` | 길이 20 (8.1 6번과 같은 규칙) | `kTypeLength` |
// | `PING`, `PONG` | 길이 8 | `kTypeLength` |
// | `CLOSE` | 길이 1, 그다음 reason 이 `0x00`~`0x03` (5.6) | `kTypeLength`, `kBadReason` |
// | `ROSTER` | 5.7 수신 검증 1~5번. 아래 표 | 그 표의 카운터 |
//
// 디코더도 길이를 다시 본다. 8.1 6번을 이미 통과한 페이로드라면 걸리지 않는다. 디코더를
// 따로 불러도 버퍼 밖을 읽지 않게 하려는 것이고, 규칙은 `payload_length_ok` 하나를 같이
// 쓴다.
//
// ### 8.1 공통 검사 1~6 (`check_common`)
//
// 위에서부터 보고 처음 걸린 자리에서 멈춘다. 표의 순서가 곧 8.1 의 순서다.
//
// | # | 검사 | 어긋나면 | 카운터 |
// |---|------|----------|--------|
// | 1 | 데이터그램 길이 >= `HEADER_SIZE` | `kShort` | `drop_short` |
// | 2 | magic == `TUNNEL_MAGIC` | `kMagic` | `drop_magic` |
// | 3 | version == `TUNNEL_VERSION` | `kVersion` | `drop_version` |
// | 4 | type 이 5장 목록에 있다 | `kUnknownType` | `drop_unknown_type` |
// | 5 | `payload_length == 길이 - HEADER_SIZE` | `kLength` | `drop_length` |
// | 6 | 타입별 길이 (5장 표) | `kTypeLength` | `drop_type_length` |
//
// ### 5.7 ROSTER 수신 검증 (`decode_roster`)
//
// 그 절의 다섯 검사를 그 순서대로 본다. 1번과 3번은 세션 상태(상대가 호스트인가, 그 세션이
// 마지막으로 받아들인 `generation`)를 봐야 하므로 그 값을 인자로 받는다. 인자로 받는 이유는
// 순서다. 3번을 파이프라인이 따로 보면 4번과 5번이 3번보다 먼저 돌아 같은 입력이 다른
// 카운터를 올린다.
//
// | # | 검사 | 어긋나면 | 카운터 |
// |---|------|----------|--------|
// | 1 | 보낸 쪽이 호스트다 | `kRosterDirection` | `drop_roster_direction` |
// | 2 | 구조. `1 <= count <= MAX_PEERS`, 멤버마다 `1 <= name_len <= MAX_NAME_LEN` 이고 페이로드 끝을 넘지 않는다, 다 읽은 위치가 페이로드 끝과 같다 | `kTypeLength` | `drop_type_length` |
// | 3 | `generation` 이 마지막으로 받아들인 값보다 크다 | `kRosterStale` | `drop_roster_stale` |
// | 4 | 모든 멤버의 `flags` 비트 1~7 이 0 | `kRosterFlags` | `drop_roster_flags` |
// | 5 | 모든 멤버의 `name` 이 control_plane.md 2.7 의 63자 안에 있다 | `kRosterName` | `drop_roster_name` |
//
// 4번과 5번은 "모든 멤버" 의 검사라 4번을 전 멤버에 대해 마친 뒤 5번을 본다. 앞 멤버의
// 이름이 틀리고 뒤 멤버의 flags 가 틀리면 4번이 이긴다.
//
// ### 이 코드가 정한 것
//
// 문서가 값을 정했지만 표현을 정하지 않은 자리다. 표 밖의 규칙을 만들지 않는다.
//
// | 자리 | 정한 것 | 왜 |
// |------|---------|-----|
// | 마지막으로 받아들인 `generation` 이 없다 | 어떤 값이든 3번을 통과한다 | 5.7 "세션이 새로 서면 수신자는 0 부터 받아들인다". 부호 없는 32비트에 0 보다 작은 값이 없다 |
// | `generation` 비교 | 부호 없는 32비트 `>` 다. 순환을 고려하지 않는다 | 5.7 은 "크다" 라고 적었고 4.4 `is_newer` 를 가리키지 않았다. 같은 방에서 값이 뒤로 갈 일이 없다고 적었다 |
// | 호스트 자신의 항목의 비트 0 | 보지 않는다 | 5.7 이 송신 규칙으로만 적었고 수신 검사 다섯에 없다 |
// | 인코더가 받는 값이 규칙을 어긴다 | 아무것도 쓰지 않고 0 을 돌려준다 | 우리가 보내는 패킷이 상대의 같은 검사에 걸리면 안 된다. 잘라서 보내지 않는다 |
// | 디코더에 들어온 `DATA`, `KEEPALIVE` | 디코더가 없다 | `KEEPALIVE` 는 페이로드가 없고 `DATA` 는 8.4 가 보는 내부 패킷 그대로다 |

#include "sangtachi/protocol_constants.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace sangtachi::tunnel {

// 5장 패킷 타입 표.
enum class PacketType : std::uint8_t {
    kHello = 0x01,
    kHelloAck = 0x02,
    kKeepalive = 0x03,
    kData = 0x04,
    kPing = 0x05,
    kPong = 0x06,
    kClose = 0x07,
    kRoster = 0x08,
};

// 5장 표의 페이로드 길이와 5.1~5.7 의 배치.
inline constexpr std::size_t kNonceSize = 16;                    // 5.1 nonce, 5.2 echo_nonce
inline constexpr std::size_t kHelloPayloadSize = kNonceSize + 4;  // + virtual_ip. HELLO, HELLO_ACK
inline constexpr std::size_t kPingPayloadSize = 8;                // ping_id. PING, PONG
inline constexpr std::size_t kClosePayloadSize = 1;               // reason
inline constexpr std::size_t kDataMinPayload = 20;
inline constexpr std::size_t kDataMaxPayload = protocol::kMaxInner;

// 5.7 ROSTER. 머리는 generation 4 + count 1, 멤버 하나는 peer_id 4 + virtual_ip 4 +
// flags 1 + name_len 1 + name.
inline constexpr std::size_t kRosterHeadSize = 5;
inline constexpr std::size_t kRosterMemberFixedSize = 10;
inline constexpr std::size_t kRosterMinPayload = kRosterHeadSize + kRosterMemberFixedSize + 1;
inline constexpr std::size_t kRosterMaxPayload =
    kRosterHeadSize + protocol::kMaxPeers * (kRosterMemberFixedSize + protocol::kMaxNameLen);

static_assert(kHelloPayloadSize == 20, "protocol.md 5장 HELLO 20");
static_assert(kRosterMinPayload == 16, "protocol.md 5.7 ROSTER 16");
static_assert(kRosterMaxPayload == 135, "protocol.md 5.7 ROSTER 135");
static_assert(protocol::kHeaderSize + kDataMaxPayload == protocol::kMaxDatagram);

// 5.7 멤버 flags. 비트 0 = 호스트와 연결됨, 비트 1~7 은 예약이다.
inline constexpr std::uint8_t kRosterFlagConnected = 0x01;
inline constexpr std::uint8_t kRosterFlagsReserved = 0xFE;

// 판정 실패의 종류. 각 줄의 카운터 이름은 `counter_name` 이 돌려주는 문자열이다.
enum class PacketError : std::uint8_t {
    kNone = 0,         // 오류 없음
    kShort,            // drop_short          8.1 1번
    kMagic,            // drop_magic          8.1 2번
    kVersion,          // drop_version        8.1 3번
    kUnknownType,      // drop_unknown_type   8.1 4번
    kLength,           // drop_length         8.1 5번
    kTypeLength,       // drop_type_length    8.1 6번, 5.7 2번
    kBadReason,        // drop_bad_reason     5.6
    kRosterDirection,  // drop_roster_direction  5.7 1번
    kRosterStale,      // drop_roster_stale      5.7 3번
    kRosterFlags,      // drop_roster_flags      5.7 4번
    kRosterName,       // drop_roster_name       5.7 5번
};

// 그 종류가 올릴 카운터의 이름 (architecture.md 9장 `counter` 이벤트의 `name`).
// `kNone` 은 빈 문자열이다.
//
// counters.hpp 의 열거값이 아니라 문자열을 돌려준다. `drop_roster_name` 이 아직 그 표에
// 없기 때문이다. 표에 들어오면 파이프라인이 이 문자열로 대조할 수 있다.
[[nodiscard]] std::string_view counter_name(PacketError error) noexcept;

// --- 헤더 (4.1) ---

// 헤더 20바이트의 필드. magic 과 version 도 필드로 둔다. 수신한 값을 그대로 담아야
// 8.1 2번과 3번이 판정할 수 있다. 보낼 때는 기본값이 3장 상수다.
//
// type 은 열거형이 아니라 바이트다. 5장 목록 밖의 값도 담아야 4번이 판정한다.
struct Header {
    std::uint32_t magic = protocol::kTunnelMagic;
    std::uint8_t version = protocol::kTunnelVersion;
    std::uint8_t type = 0;
    std::uint16_t payload_length = 0;
    std::uint32_t peer_id = 0;
    std::uint32_t session_epoch = 0;
    std::uint32_t sequence = 0;

    friend constexpr bool operator==(const Header&, const Header&) noexcept = default;
};

// 필드를 4.1 오프셋에 빅엔디안으로 쓴다. 검사하지 않는다. 받은 값을 그대로 쓴다.
[[nodiscard]] std::array<std::byte, protocol::kHeaderSize> serialize_header(
    const Header& header) noexcept;

// 앞 20바이트를 필드로 읽는다. 20 미만이면 비어 있다. **검증이 아니다.** 판정은
// `check_common` 이 한다. 20바이트 뒤는 보지 않는다.
[[nodiscard]] std::optional<Header> deserialize_header(std::span<const std::byte> bytes) noexcept;

// 8.1 4번. 5장 목록에 있는 값인가.
[[nodiscard]] bool is_known_type(std::uint8_t type) noexcept;

// 8.1 6번. 그 타입에 그 페이로드 길이가 허용되는가. ROSTER 는 16~135 범위만 본다.
// 구조는 5.7 수신 검증 2번이 따로 본다.
[[nodiscard]] bool payload_length_ok(PacketType type, std::size_t length) noexcept;

struct CommonCheck {
    PacketError error = PacketError::kNone;
    Header header;                       // 통과했을 때만 뜻이 있다
    std::span<const std::byte> payload;  // 데이터그램 안을 가리킨다. 통과했을 때만 있다

    [[nodiscard]] constexpr bool ok() const noexcept { return error == PacketError::kNone; }
    [[nodiscard]] constexpr PacketType type() const noexcept {
        return static_cast<PacketType>(header.type);
    }
};

// 8.1 공통 검사 1~6. 검사와 순서는 이 파일 머리의 표가 갖는다.
[[nodiscard]] CommonCheck check_common(std::span<const std::byte> datagram) noexcept;

// 보내는 패킷의 헤더 가운데 부르는 쪽이 정하는 셋. magic, version 은 3장 상수이고
// payload_length 는 페이로드 크기다.
struct HeaderFields {
    std::uint32_t peer_id = 0;
    std::uint32_t session_epoch = 0;
    std::uint32_t sequence = 0;
};

// 헤더와 페이로드를 `out` 에 잇고 쓴 바이트 수를 돌려준다. 페이로드 길이가 8.1 6번에
// 어긋나거나 `out` 이 모자라면 아무것도 쓰지 않고 0 이다.
[[nodiscard]] std::size_t build_packet(PacketType type, const HeaderFields& fields,
                                       std::span<const std::byte> payload,
                                       std::span<std::byte> out) noexcept;

// --- 5.1 HELLO, 5.2 HELLO_ACK ---

using Nonce = std::array<std::byte, kNonceSize>;

// 두 타입의 배치가 같다. HELLO 에서는 `nonce`, HELLO_ACK 에서는 `echo_nonce` 다.
struct HelloPayload {
    Nonce nonce{};
    std::uint32_t virtual_ip = 0;  // 호스트 바이트 순서. 와이어에는 빅엔디안

    friend constexpr bool operator==(const HelloPayload&, const HelloPayload&) noexcept = default;
};

struct HelloDecode {
    PacketError error = PacketError::kNone;
    HelloPayload value;

    [[nodiscard]] constexpr bool ok() const noexcept { return error == PacketError::kNone; }
};

[[nodiscard]] std::array<std::byte, kHelloPayloadSize> encode_hello(
    const HelloPayload& payload) noexcept;
[[nodiscard]] HelloDecode decode_hello(std::span<const std::byte> payload) noexcept;

// --- 5.5 PING / PONG ---

struct PingDecode {
    PacketError error = PacketError::kNone;
    std::uint64_t ping_id = 0;

    [[nodiscard]] constexpr bool ok() const noexcept { return error == PacketError::kNone; }
};

[[nodiscard]] std::array<std::byte, kPingPayloadSize> encode_ping(std::uint64_t ping_id) noexcept;
[[nodiscard]] PingDecode decode_ping(std::span<const std::byte> payload) noexcept;

// --- 5.6 CLOSE ---

enum class CloseReason : std::uint8_t {
    kNormal = 0x00,       // 정상 종료
    kUserCancel = 0x01,   // 사용자 취소
    kConfigError = 0x02,  // 설정 오류
    kSetupFailed = 0x03,  // 수립 실패
};

struct CloseDecode {
    PacketError error = PacketError::kNone;
    CloseReason reason = CloseReason::kNormal;

    [[nodiscard]] constexpr bool ok() const noexcept { return error == PacketError::kNone; }
};

[[nodiscard]] std::array<std::byte, kClosePayloadSize> encode_close(CloseReason reason) noexcept;
[[nodiscard]] CloseDecode decode_close(std::span<const std::byte> payload) noexcept;

// --- 5.7 ROSTER ---

// control_plane.md 2.7 `name`. ASCII 영문자, 숫자, 밑줄로 된 1~MAX_NAME_LEN 바이트.
[[nodiscard]] bool is_valid_name(std::string_view name) noexcept;

struct RosterMember {
    std::uint32_t peer_id = 0;
    std::uint32_t virtual_ip = 0;  // 호스트 바이트 순서. 와이어에는 빅엔디안
    bool connected = false;        // flags 비트 0
    std::array<char, protocol::kMaxNameLen> name{};  // 앞 name_len 바이트만 뜻이 있고 나머지는 0
    std::uint8_t name_len = 0;

    [[nodiscard]] constexpr std::string_view name_view() const noexcept {
        // name_len 이 배열보다 크게 적혀 있어도 배열 밖을 가리키지 않는다.
        return std::string_view(name.data(),
                                name_len < name.size() ? std::size_t{name_len} : name.size());
    }

    friend constexpr bool operator==(const RosterMember&, const RosterMember&) noexcept = default;
};

struct Roster {
    std::uint32_t generation = 0;
    std::array<RosterMember, protocol::kMaxPeers> members{};  // 앞 count 개만 뜻이 있다
    std::uint8_t count = 0;

    friend constexpr bool operator==(const Roster&, const Roster&) noexcept = default;
};

// 5.7 수신 검증 1번과 3번이 보는 세션 상태.
struct RosterContext {
    bool sender_is_host = false;
    std::optional<std::uint32_t> last_generation;  // 그 세션이 마지막으로 받아들인 값. 없으면 비어 있다
};

struct RosterDecode {
    PacketError error = PacketError::kNone;
    Roster value;  // 통과했을 때만 뜻이 있다

    [[nodiscard]] constexpr bool ok() const noexcept { return error == PacketError::kNone; }
};

// 5.7 수신 검증 1~5. 검사와 순서는 이 파일 머리의 표가 갖는다.
//
// 통과해도 `last_generation` 을 고치지 않는다. 받아들인 값을 세션에 적는 것은 부르는 쪽이다.
[[nodiscard]] RosterDecode decode_roster(std::span<const std::byte> payload,
                                         const RosterContext& context) noexcept;

// 쓴 바이트 수를 돌려준다. count 가 1~MAX_PEERS 밖이거나, 이름이 `is_valid_name` 에
// 어긋나거나, `out` 이 모자라면 아무것도 쓰지 않고 0 이다. flags 는 비트 0 만 쓴다.
[[nodiscard]] std::size_t encode_roster(const Roster& roster, std::span<std::byte> out) noexcept;

}  // namespace sangtachi::tunnel
