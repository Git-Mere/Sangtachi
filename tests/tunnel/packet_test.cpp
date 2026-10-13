#include "sangtachi/tunnel/packet.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// 시험 케이스 이름은 ASCII 로만 적는다. 이유는 network/wsa_test.cpp 머리에 있다.
//
// 케이스 표가 먼저다. packet.hpp 머리의 두 검사 표와 "이 코드가 정한 것" 표의 행마다 여기에
// 케이스가 있다. 값은 코드에서 가져오지 않고 이 파일에 따로 적는다. 구현과 시험이 같은 상수를
// 보면 그 상수가 틀려도 둘 다 같이 틀린다. 숫자의 출처는 protocol.md 4.1, 5장 표, 5.1~5.7 이다.

using sangtachi::tunnel::build_packet;
using sangtachi::tunnel::check_common;
using sangtachi::tunnel::CloseReason;
using sangtachi::tunnel::counter_name;
using sangtachi::tunnel::decode_close;
using sangtachi::tunnel::decode_hello;
using sangtachi::tunnel::decode_ping;
using sangtachi::tunnel::decode_roster;
using sangtachi::tunnel::deserialize_header;
using sangtachi::tunnel::encode_close;
using sangtachi::tunnel::encode_hello;
using sangtachi::tunnel::encode_ping;
using sangtachi::tunnel::encode_roster;
using sangtachi::tunnel::Header;
using sangtachi::tunnel::HeaderFields;
using sangtachi::tunnel::HelloPayload;
using sangtachi::tunnel::is_known_type;
using sangtachi::tunnel::is_valid_name;
using sangtachi::tunnel::Nonce;
using sangtachi::tunnel::PacketError;
using sangtachi::tunnel::PacketType;
using sangtachi::tunnel::payload_length_ok;
using sangtachi::tunnel::Roster;
using sangtachi::tunnel::RosterContext;
using sangtachi::tunnel::RosterMember;
using sangtachi::tunnel::serialize_header;

namespace {

using Bytes = std::vector<std::byte>;

// 4.1 와이어 레이아웃과 3장 상수. 여기 따로 적는다.
constexpr std::uint32_t kMagic = 0x53414E47u;  // "SANG"
constexpr unsigned kVersion = 0x01u;
constexpr std::size_t kHeader = 20;

// 5장 타입 값.
constexpr unsigned kHello = 0x01u;
constexpr unsigned kHelloAck = 0x02u;
constexpr unsigned kKeepalive = 0x03u;
constexpr unsigned kData = 0x04u;
constexpr unsigned kPing = 0x05u;
constexpr unsigned kPong = 0x06u;
constexpr unsigned kClose = 0x07u;
constexpr unsigned kRoster = 0x08u;

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

Bytes hex(std::initializer_list<unsigned> values) {
    Bytes out;
    for (const unsigned v : values) {
        put_u8(out, v);
    }
    return out;
}

// 헤더를 손으로 짓는다. 구현의 serialize_header 를 쓰지 않는다.
struct RawHeader {
    std::uint32_t magic = kMagic;
    unsigned version = kVersion;
    unsigned type = kKeepalive;
    std::optional<unsigned> payload_length;  // 없으면 실제 페이로드 길이를 적는다
    std::uint32_t peer_id = 0x11223344u;
    std::uint32_t session_epoch = 0x55667788u;
    std::uint32_t sequence = 0x99AABBCCu;
};

Bytes datagram(const RawHeader& h, const Bytes& payload) {
    Bytes out;
    put_u32(out, h.magic);
    put_u8(out, h.version);
    put_u8(out, h.type);
    put_u16(out, h.payload_length.value_or(static_cast<unsigned>(payload.size())));
    put_u32(out, h.peer_id);
    put_u32(out, h.session_epoch);
    put_u32(out, h.sequence);
    put_all(out, payload);
    return out;
}

Bytes typed(unsigned type, const Bytes& payload) {
    RawHeader h;
    h.type = type;
    return datagram(h, payload);
}

std::span<const std::byte> view(const Bytes& b) {
    return std::span<const std::byte>(b.data(), b.size());
}

// --- ROSTER 짓기 ---

struct MemberSpec {
    std::uint32_t peer_id = 0x0A0B0C0Du;
    std::uint32_t virtual_ip = 0x0A640001u;  // 10.100.0.1
    unsigned flags = 0x01u;
    std::string name = "Alice";
    std::optional<unsigned> name_len;  // 없으면 name 의 길이를 적는다
};

Bytes member(const MemberSpec& m) {
    Bytes out;
    put_u32(out, m.peer_id);
    put_u32(out, m.virtual_ip);
    put_u8(out, m.flags);
    put_u8(out, m.name_len.value_or(static_cast<unsigned>(m.name.size())));
    for (const char c : m.name) {
        put_u8(out, static_cast<unsigned char>(c));
    }
    return out;
}

Bytes roster_payload(std::uint32_t generation, unsigned count, std::initializer_list<Bytes> members) {
    Bytes out;
    put_u32(out, generation);
    put_u8(out, count);
    for (const auto& m : members) {
        put_all(out, m);
    }
    return out;
}

Bytes simple_roster(std::uint32_t generation, std::initializer_list<MemberSpec> specs) {
    Bytes out;
    put_u32(out, generation);
    put_u8(out, static_cast<unsigned>(specs.size()));
    for (const auto& s : specs) {
        put_all(out, member(s));
    }
    return out;
}

MemberSpec named(std::string name, unsigned flags = 0x01u) {
    MemberSpec m;
    m.name = std::move(name);
    m.flags = flags;
    return m;
}

const RosterContext kFromHostFresh{true, std::nullopt};

RosterMember make_member(std::uint32_t peer_id, std::uint32_t vip, bool connected,
                         std::string_view name) {
    RosterMember m;
    m.peer_id = peer_id;
    m.virtual_ip = vip;
    m.connected = connected;
    for (std::size_t i = 0; i < name.size() && i < m.name.size(); ++i) {
        m.name[i] = name[i];
    }
    m.name_len = static_cast<std::uint8_t>(name.size());
    return m;
}

}  // namespace

// ============================================================================
// 4.1 헤더
// ============================================================================

TEST_CASE("packet: header golden bytes match the 4.1 offsets in big-endian", "[packet]") {
    Header h;
    h.type = 0x04;
    h.payload_length = 0x0164;  // 356
    h.peer_id = 0x11223344u;
    h.session_epoch = 0xA1B2C3D4u;
    h.sequence = 0x00000102u;

    const auto bytes = serialize_header(h);
    const Bytes expected = hex({0x53, 0x41, 0x4E, 0x47,  // magic "SANG"
                                0x01,                    // version
                                0x04,                    // type
                                0x01, 0x64,              // payload_length
                                0x11, 0x22, 0x33, 0x44,  // peer_id
                                0xA1, 0xB2, 0xC3, 0xD4,  // session_epoch
                                0x00, 0x00, 0x01, 0x02});
    REQUIRE(bytes.size() == kHeader);
    CHECK(Bytes(bytes.begin(), bytes.end()) == expected);
}

TEST_CASE("packet: the first four bytes of a default header are 53 41 4E 47", "[packet]") {
    const auto bytes = serialize_header(Header{});
    CHECK(bytes[0] == std::byte{0x53});
    CHECK(bytes[1] == std::byte{0x41});
    CHECK(bytes[2] == std::byte{0x4E});
    CHECK(bytes[3] == std::byte{0x47});
    CHECK(bytes[4] == std::byte{0x01});  // 기본 version 도 3장 값이다
}

TEST_CASE("packet: header round-trips through serialize and deserialize", "[packet]") {
    // 필드마다 경계와 바이트가 모두 다른 값을 넣는다. 바이트 순서를 뒤집은 구현이나
    // 필드 자리를 바꾼 구현은 이 중 하나에서 원본과 달라진다.
    const std::vector<Header> table = {
        Header{},
        Header{0x00000000u, 0x00, 0x00, 0x0000, 0x00000000u, 0x00000000u, 0x00000000u},
        Header{0xFFFFFFFFu, 0xFF, 0xFF, 0xFFFF, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu},
        Header{0x01020304u, 0x05, 0x06, 0x0708, 0x090A0B0Cu, 0x0D0E0F10u, 0x11121314u},
        Header{kMagic, 0x01, 0x08, 0x0087, 0x80000000u, 0x00000001u, 0x7FFFFFFFu},
    };
    for (const auto& h : table) {
        const auto bytes = serialize_header(h);
        const auto back = deserialize_header(std::span<const std::byte>(bytes.data(), bytes.size()));
        REQUIRE(back.has_value());
        CHECK(*back == h);
    }
}

TEST_CASE("packet: deserialize_header needs 20 bytes and ignores what follows", "[packet]") {
    const Bytes full = datagram(RawHeader{}, filler(5));
    CHECK_FALSE(deserialize_header(std::span<const std::byte>(full.data(), 19)).has_value());
    CHECK_FALSE(deserialize_header(std::span<const std::byte>()).has_value());

    const auto h = deserialize_header(view(full));
    REQUIRE(h.has_value());
    CHECK(h->magic == kMagic);
    CHECK(h->version == kVersion);
    CHECK(h->type == kKeepalive);
    CHECK(h->payload_length == 5);
    CHECK(h->peer_id == 0x11223344u);
    CHECK(h->session_epoch == 0x55667788u);
    CHECK(h->sequence == 0x99AABBCCu);
}

// ============================================================================
// 8.1 공통 검사 1~6
// ============================================================================

namespace {

struct CommonCase {
    std::string_view why;
    Bytes datagram;
    PacketError expected;
};

std::vector<CommonCase> make_common_cases() {
    std::vector<CommonCase> cases;
    auto ok = [&](std::string_view why, Bytes d) {
        cases.push_back({why, std::move(d), PacketError::kNone});
    };
    auto bad = [&](std::string_view why, Bytes d, PacketError e) {
        cases.push_back({why, std::move(d), e});
    };

    // --- 1. 길이 >= 20 ---
    bad("empty datagram", Bytes{}, PacketError::kShort);
    {
        Bytes d = typed(kKeepalive, {});
        d.pop_back();
        bad("19 bytes", d, PacketError::kShort);
    }
    {
        // 19바이트인데 magic 도 틀리다. 1번이 이긴다.
        RawHeader h;
        h.magic = 0;
        Bytes d = datagram(h, {});
        d.pop_back();
        bad("19 bytes with a bad magic is short first", d, PacketError::kShort);
    }
    ok("exactly 20 bytes keepalive", typed(kKeepalive, {}));

    // --- 2. magic ---
    for (const std::uint32_t magic : {0x00000000u, 0x53414E46u, 0x534E4147u, 0x474E4153u, 0xD3414E47u}) {
        RawHeader h;
        h.magic = magic;
        bad("magic differs", datagram(h, {}), PacketError::kMagic);
    }
    {
        RawHeader h;
        h.magic = 0x53414E48u;
        h.version = 0x02;
        bad("bad magic and bad version is magic first", datagram(h, {}), PacketError::kMagic);
    }

    // --- 3. version ---
    for (const unsigned version : {0x00u, 0x02u, 0xFFu}) {
        RawHeader h;
        h.version = version;
        bad("version differs", datagram(h, {}), PacketError::kVersion);
    }
    {
        RawHeader h;
        h.version = 0x00;
        h.type = 0x09;
        bad("bad version and unknown type is version first", datagram(h, {}), PacketError::kVersion);
    }

    // --- 4. type ---
    for (const unsigned type : {0x00u, 0x09u, 0x10u, 0x80u, 0xFFu}) {
        bad("type outside the 5 table", typed(type, {}), PacketError::kUnknownType);
    }
    {
        RawHeader h;
        h.type = 0x00;
        h.payload_length = 3;
        bad("unknown type and bad length is type first", datagram(h, {}), PacketError::kUnknownType);
    }

    // --- 5. payload_length == len - 20 ---
    {
        RawHeader h;
        h.payload_length = 1;
        bad("payload_length 1 with no payload", datagram(h, {}), PacketError::kLength);
    }
    {
        RawHeader h;
        h.type = kPing;
        h.payload_length = 7;
        bad("payload_length one short of the real payload", datagram(h, filler(8)), PacketError::kLength);
    }
    {
        RawHeader h;
        h.type = kPing;
        h.payload_length = 9;
        bad("payload_length one over the real payload", datagram(h, filler(8)), PacketError::kLength);
    }
    {
        RawHeader h;
        h.type = kPing;
        h.payload_length = 0xFFFF;
        bad("payload_length 65535", datagram(h, filler(8)), PacketError::kLength);
    }
    {
        // 길이 필드와 실제가 어긋나고 타입별 길이도 틀리다. 5번이 이긴다.
        RawHeader h;
        h.type = kHello;
        h.payload_length = 20;
        bad("bad length and bad type length is length first", datagram(h, filler(3)), PacketError::kLength);
    }

    // --- 6. 타입별 길이. 최소, 최대, 하나 모자람, 하나 넘침 ---
    for (const unsigned type : {kHello, kHelloAck}) {
        bad("hello-sized 19", typed(type, filler(19)), PacketError::kTypeLength);
        ok("hello-sized 20", typed(type, filler(20)));
        bad("hello-sized 21", typed(type, filler(21)), PacketError::kTypeLength);
        bad("hello-sized 0", typed(type, {}), PacketError::kTypeLength);
    }
    ok("keepalive 0", typed(kKeepalive, {}));
    bad("keepalive 1", typed(kKeepalive, filler(1)), PacketError::kTypeLength);
    bad("data 0", typed(kData, {}), PacketError::kTypeLength);
    bad("data 19", typed(kData, filler(19)), PacketError::kTypeLength);
    ok("data 20", typed(kData, filler(20)));
    ok("data 1452", typed(kData, filler(1452)));
    bad("data 1453", typed(kData, filler(1453)), PacketError::kTypeLength);
    for (const unsigned type : {kPing, kPong}) {
        bad("ping-sized 7", typed(type, filler(7)), PacketError::kTypeLength);
        ok("ping-sized 8", typed(type, filler(8)));
        bad("ping-sized 9", typed(type, filler(9)), PacketError::kTypeLength);
        bad("ping-sized 0", typed(type, {}), PacketError::kTypeLength);
    }
    bad("close 0", typed(kClose, {}), PacketError::kTypeLength);
    ok("close 1", typed(kClose, filler(1, 0x00)));
    bad("close 2", typed(kClose, filler(2, 0x00)), PacketError::kTypeLength);
    bad("roster 0", typed(kRoster, {}), PacketError::kTypeLength);
    bad("roster 15", typed(kRoster, filler(15)), PacketError::kTypeLength);
    ok("roster 16", typed(kRoster, filler(16)));  // 구조는 5.7 이 따로 본다
    ok("roster 135", typed(kRoster, filler(135)));
    bad("roster 136", typed(kRoster, filler(136)), PacketError::kTypeLength);

    // 6번은 1번이 보지 않는 큰 쪽도 막는다. MAX_DATAGRAM 을 넘는 것은 수신 루프가 먼저 버린다.
    bad("data 1453 with 1473 bytes in total", typed(kData, filler(1453)), PacketError::kTypeLength);

    // 헤더 필드가 판정에 끼지 않는 것. peer_id, epoch, sequence 가 0 이어도 통과한다.
    {
        RawHeader h;
        h.peer_id = 0;
        h.session_epoch = 0;
        h.sequence = 0;
        ok("zero peer_id epoch and sequence pass the common checks", datagram(h, {}));
    }
    return cases;
}

}  // namespace

TEST_CASE("packet: check_common case table", "[packet]") {
    for (const auto& c : make_common_cases()) {
        INFO(c.why);
        const auto result = check_common(view(c.datagram));
        CHECK(result.error == c.expected);
        if (c.expected == PacketError::kNone) {
            CHECK(result.ok());
            CHECK(result.payload.size() == c.datagram.size() - kHeader);
            CHECK(result.payload.data() == c.datagram.data() + kHeader);
            CHECK(result.header.payload_length == c.datagram.size() - kHeader);
        } else {
            CHECK_FALSE(result.ok());
        }
    }
}

TEST_CASE("packet: check_common reports every common error somewhere in the table", "[packet]") {
    std::set<PacketError> seen;
    for (const auto& c : make_common_cases()) {
        seen.insert(c.expected);
    }
    for (const auto e : {PacketError::kNone, PacketError::kShort, PacketError::kMagic,
                         PacketError::kVersion, PacketError::kUnknownType, PacketError::kLength,
                         PacketError::kTypeLength}) {
        CHECK(seen.count(e) == 1);
    }
}

TEST_CASE("packet: check_common returns the parsed header fields", "[packet]") {
    RawHeader h;
    h.type = kPong;
    h.peer_id = 0xCAFEBABEu;
    h.session_epoch = 0x00000001u;
    h.sequence = 0xFFFFFFFFu;
    const Bytes d = datagram(h, filler(8, 0x42));
    const auto r = check_common(view(d));
    REQUIRE(r.ok());
    CHECK(r.type() == PacketType::kPong);
    CHECK(r.header.peer_id == 0xCAFEBABEu);
    CHECK(r.header.session_epoch == 0x00000001u);
    CHECK(r.header.sequence == 0xFFFFFFFFu);
    CHECK(r.header.payload_length == 8);
}

TEST_CASE("packet: is_known_type accepts exactly 0x01..0x08", "[packet]") {
    for (unsigned v = 0; v <= 0xFFu; ++v) {
        INFO(v);
        CHECK(is_known_type(static_cast<std::uint8_t>(v)) == (v >= 1u && v <= 8u));
    }
}

TEST_CASE("packet: payload_length_ok table over every type and length up to 1500", "[packet]") {
    struct Range {
        PacketType type;
        std::size_t lo;
        std::size_t hi;
    };
    const std::vector<Range> table = {
        {PacketType::kHello, 20, 20},  {PacketType::kHelloAck, 20, 20},
        {PacketType::kKeepalive, 0, 0}, {PacketType::kData, 20, 1452},
        {PacketType::kPing, 8, 8},     {PacketType::kPong, 8, 8},
        {PacketType::kClose, 1, 1},    {PacketType::kRoster, 16, 135},
    };
    for (const auto& r : table) {
        for (std::size_t len = 0; len <= 1500; ++len) {
            INFO(static_cast<unsigned>(r.type) << " len " << len);
            CHECK(payload_length_ok(r.type, len) == (len >= r.lo && len <= r.hi));
        }
    }
    // 목록 밖의 값으로 만든 타입은 어떤 길이도 허용하지 않는다.
    CHECK_FALSE(payload_length_ok(static_cast<PacketType>(0x09), 0));
    CHECK_FALSE(payload_length_ok(static_cast<PacketType>(0x00), 20));
}

// ============================================================================
// build_packet
// ============================================================================

TEST_CASE("packet: build_packet writes header then payload and passes check_common", "[packet]") {
    const Bytes payload = filler(8, 0x5A);
    std::array<std::byte, 64> out{};
    const HeaderFields fields{0x01020304u, 0x05060708u, 0x090A0B0Cu};
    const std::size_t n = build_packet(PacketType::kPing, fields, view(payload), out);
    REQUIRE(n == 28);

    RawHeader h;
    h.type = kPing;
    h.peer_id = 0x01020304u;
    h.session_epoch = 0x05060708u;
    h.sequence = 0x090A0B0Cu;
    const Bytes expected = datagram(h, payload);
    CHECK(Bytes(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(n)) == expected);

    const auto r = check_common(std::span<const std::byte>(out.data(), n));
    CHECK(r.ok());
}

TEST_CASE("packet: build_packet refuses a payload the type does not allow", "[packet]") {
    std::array<std::byte, 1600> out{};
    out.fill(std::byte{0x77});
    const HeaderFields fields{};
    CHECK(build_packet(PacketType::kHello, fields, view(filler(19)), out) == 0);
    CHECK(build_packet(PacketType::kKeepalive, fields, view(filler(1)), out) == 0);
    CHECK(build_packet(PacketType::kData, fields, view(filler(1453)), out) == 0);
    CHECK(build_packet(PacketType::kData, fields, view(filler(19)), out) == 0);
    CHECK(build_packet(static_cast<PacketType>(0x09), fields, view(Bytes{}), out) == 0);
    for (const auto b : out) {
        REQUIRE(b == std::byte{0x77});  // 아무것도 쓰지 않았다
    }
    CHECK(build_packet(PacketType::kData, fields, view(filler(1452)), out) == 1472);
    CHECK(build_packet(PacketType::kKeepalive, fields, view(Bytes{}), out) == 20);
}

TEST_CASE("packet: build_packet refuses an output buffer one byte too small", "[packet]") {
    std::array<std::byte, 27> small{};
    small.fill(std::byte{0x77});
    CHECK(build_packet(PacketType::kPing, HeaderFields{}, view(filler(8)), small) == 0);
    for (const auto b : small) {
        REQUIRE(b == std::byte{0x77});
    }
    std::array<std::byte, 28> exact{};
    CHECK(build_packet(PacketType::kPing, HeaderFields{}, view(filler(8)), exact) == 28);
}

// ============================================================================
// 5.1 HELLO, 5.2 HELLO_ACK
// ============================================================================

TEST_CASE("packet: hello golden bytes and round trip", "[packet]") {
    HelloPayload p;
    for (std::size_t i = 0; i < p.nonce.size(); ++i) {
        p.nonce[i] = static_cast<std::byte>(0xF0u + i);
    }
    p.virtual_ip = 0x0A640003u;  // 10.100.0.3
    const auto bytes = encode_hello(p);
    const Bytes expected = hex({0xF0, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA,
                                0xFB, 0xFC, 0xFD, 0xFE, 0xFF, 0x0A, 0x64, 0x00, 0x03});
    CHECK(Bytes(bytes.begin(), bytes.end()) == expected);

    const auto back = decode_hello(std::span<const std::byte>(bytes.data(), bytes.size()));
    REQUIRE(back.ok());
    CHECK(back.value == p);
}

TEST_CASE("packet: decode_hello rejects anything but 20 bytes", "[packet]") {
    for (const std::size_t len : {std::size_t{0}, std::size_t{16}, std::size_t{19}, std::size_t{21}}) {
        INFO(len);
        const Bytes b = filler(len);
        CHECK(decode_hello(view(b)).error == PacketError::kTypeLength);
    }
}

// ============================================================================
// 5.5 PING / PONG
// ============================================================================

TEST_CASE("packet: ping_id is 8 bytes big-endian and round-trips", "[packet]") {
    const auto bytes = encode_ping(0x0102030405060708ull);
    CHECK(Bytes(bytes.begin(), bytes.end()) == hex({0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08}));
    for (const std::uint64_t id : {0ull, 1ull, 0x00000000FFFFFFFFull, 0x0000000100000000ull,
                                   0x8000000000000000ull, 0xFFFFFFFFFFFFFFFFull}) {
        const auto b = encode_ping(id);
        const auto d = decode_ping(std::span<const std::byte>(b.data(), b.size()));
        REQUIRE(d.ok());
        CHECK(d.ping_id == id);
    }
}

TEST_CASE("packet: decode_ping rejects anything but 8 bytes", "[packet]") {
    for (const std::size_t len : {std::size_t{0}, std::size_t{7}, std::size_t{9}}) {
        INFO(len);
        CHECK(decode_ping(view(filler(len))).error == PacketError::kTypeLength);
    }
}

// ============================================================================
// 5.6 CLOSE
// ============================================================================

TEST_CASE("packet: close reason accepts 0x00..0x03 and drops every other byte", "[packet]") {
    for (unsigned v = 0; v <= 0xFFu; ++v) {
        INFO(v);
        const Bytes b = hex({v});
        const auto d = decode_close(view(b));
        if (v <= 3u) {
            REQUIRE(d.ok());
            CHECK(static_cast<unsigned>(d.reason) == v);
        } else {
            CHECK(d.error == PacketError::kBadReason);
        }
    }
}

TEST_CASE("packet: close length is checked before the reason", "[packet]") {
    CHECK(decode_close(view(Bytes{})).error == PacketError::kTypeLength);
    CHECK(decode_close(view(hex({0x00, 0x00}))).error == PacketError::kTypeLength);
    CHECK(decode_close(view(hex({0x09, 0x09}))).error == PacketError::kTypeLength);
}

TEST_CASE("packet: encode_close writes the reason byte", "[packet]") {
    CHECK(encode_close(CloseReason::kNormal)[0] == std::byte{0x00});
    CHECK(encode_close(CloseReason::kUserCancel)[0] == std::byte{0x01});
    CHECK(encode_close(CloseReason::kConfigError)[0] == std::byte{0x02});
    CHECK(encode_close(CloseReason::kSetupFailed)[0] == std::byte{0x03});
}

// ============================================================================
// control_plane.md 2.7 name
// ============================================================================

TEST_CASE("packet: is_valid_name follows control_plane 2.7", "[packet]") {
    struct NameCase {
        std::string_view why;
        std::string name;
        bool valid;
    };
    const std::vector<NameCase> table = {
        {"one letter", "a", true},
        {"sixteen letters", "ABCDEFGHIJKLMNOP", true},
        {"digits only", "0123456789", true},
        {"underscore only", "_", true},
        {"mixed case", "AliceBob", true},
        {"every allowed class", "Az09_", true},
        {"empty", "", false},
        {"seventeen letters", "ABCDEFGHIJKLMNOPQ", false},
        {"space in the middle", "Al ice", false},
        {"leading space", " Alice", false},
        {"trailing space", "Alice ", false},
        {"hyphen", "Al-ice", false},
        {"hangul", "\xEC\x95\x8C", false},          // "알" UTF-8
        {"fullwidth digit", "\xEF\xBC\x91", false},  // U+FF11
        {"trailing newline", "Alice\n", false},
        {"tab", "Al\tice", false},
        {"nul byte", std::string("Al\0ce", 5), false},
        {"DEL", "Al\x7F" "ce", false},
        {"0x80", "Al\x80" "ce", false},
        // 허용 범위 바로 바깥의 ASCII. 범위 끝을 하나씩 틀린 구현이 여기서 걸린다.
        {"slash 0x2F", "a/", false},
        {"colon 0x3A", "a:", false},
        {"at 0x40", "a@", false},
        {"bracket 0x5B", "a[", false},
        {"caret 0x5E", "a^", false},
        {"backtick 0x60", "a`", false},
        {"brace 0x7B", "a{", false},
        // 범위 끝 그 자체.
        {"range ends", "09AZaz_", true},
    };
    for (const auto& c : table) {
        INFO(c.why);
        CHECK(is_valid_name(c.name) == c.valid);
    }
}

// ============================================================================
// 5.7 ROSTER
// ============================================================================

namespace {

struct RosterCase {
    std::string_view why;
    Bytes payload;
    RosterContext context;
    PacketError expected;
};

const std::string k16 = "ABCDEFGHIJKLMNOP";
const std::string k17 = "ABCDEFGHIJKLMNOPQ";

std::vector<RosterCase> make_roster_cases() {
    std::vector<RosterCase> cases;
    auto ok = [&](std::string_view why, Bytes p, RosterContext ctx = kFromHostFresh) {
        cases.push_back({why, std::move(p), ctx, PacketError::kNone});
    };
    auto bad = [&](std::string_view why, Bytes p, PacketError e, RosterContext ctx = kFromHostFresh) {
        cases.push_back({why, std::move(p), ctx, e});
    };

    // --- 통과 ---
    ok("one member with a one-letter name is 16 bytes", simple_roster(7, {named("a")}));
    ok("five members with 16-letter names is 135 bytes",
       simple_roster(7, {named(k16), named(k16), named(k16), named(k16), named(k16)}));
    ok("name length 16", simple_roster(7, {named(k16)}));
    ok("flags 0 is allowed", simple_roster(7, {named("Bob", 0x00)}));
    ok("generation 0 with no prior roster", simple_roster(0, {named("Bob")}));
    ok("generation one above the last", simple_roster(6, {named("Bob")}), RosterContext{true, 5u});
    ok("generation max above 0", simple_roster(0xFFFFFFFFu, {named("Bob")}), RosterContext{true, 0u});

    // --- 1. 방향 ---
    bad("sent by a player", simple_roster(7, {named("Bob")}), PacketError::kRosterDirection,
        RosterContext{false, std::nullopt});
    bad("sent by a player and malformed is direction first", roster_payload(7, 0, {}),
        PacketError::kRosterDirection, RosterContext{false, std::nullopt});

    // --- 2. 구조 ---
    bad("payload shorter than the 5-byte head", hex({0x00, 0x00, 0x00}), PacketError::kTypeLength);
    bad("count 0", roster_payload(7, 0, {member(named("Bob"))}), PacketError::kTypeLength);
    // count 0 이고 머리 5 바이트뿐이면 남는 바이트 검사로는 걸리지 않는다. 8.1 6번이 16 미만을
    // 먼저 막지만 디코더를 따로 불러도 count 하한이 지켜져야 한다.
    bad("count 0 with only the 5-byte head", roster_payload(7, 0, {}), PacketError::kTypeLength);
    bad("count 6", roster_payload(7, 6, {member(named("a")), member(named("b")), member(named("c")),
                                         member(named("d")), member(named("e")), member(named("f"))}),
        PacketError::kTypeLength);
    bad("count 255", roster_payload(7, 255, {member(named("a"))}), PacketError::kTypeLength);
    {
        MemberSpec m = named("");
        bad("name length 0", roster_payload(7, 1, {member(m), filler(1, 'x')}), PacketError::kTypeLength);
    }
    bad("name length 17", simple_roster(7, {named(k17)}), PacketError::kTypeLength);
    // 이름 길이 0 이 구조로는 앞뒤가 맞는 경우. 1 이상 검사가 없으면 5번(kRosterName)으로
    // 새어 나간다. 5.7 은 이것을 2번 구조 결함으로 정했다.
    bad("name length 0 with a consistent second member",
        roster_payload(7, 2, {member(named("")), member(named("Bob"))}), PacketError::kTypeLength);
    bad("name length 0 alone in 15 bytes", roster_payload(7, 1, {member(named(""))}), PacketError::kTypeLength);
    {
        // 이름 길이를 4 로 적고 3 바이트만 실었다. 마지막 멤버가 페이로드 끝을 1 바이트 넘는다.
        // 뒤에 아무것도 붙이지 않는다. 붙이면 그 바이트가 이름이 되어 끝을 넘지 않는다.
        MemberSpec m = named("Bob");
        m.name_len = 4;
        bad("last member runs one byte past the payload end", roster_payload(7, 1, {member(m)}),
            PacketError::kTypeLength);
    }
    {
        MemberSpec last = named("ABCDEFGHIJKLMNO");  // 15 바이트
        last.name_len = 16;
        bad("second member claims 16 but carries 15", roster_payload(7, 2, {member(named("Alice")), member(last)}),
            PacketError::kTypeLength);
    }
    {
        MemberSpec m = named("Bob");
        m.name_len = 255;
        bad("name length 255 runs past the end", roster_payload(7, 1, {member(m), filler(10, 'x')}),
            PacketError::kTypeLength);
    }
    bad("count says 2 but only one member", roster_payload(7, 2, {member(named("Alice")), filler(1, 'x')}),
        PacketError::kTypeLength);
    {
        // 둘째 멤버의 고정부 10바이트가 다 오지 않았다.
        Bytes p = roster_payload(7, 2, {member(named("Alice"))});
        put_all(p, filler(6, 0x00));
        bad("second member fixed part truncated", p, PacketError::kTypeLength);
    }
    bad("bytes remain after count members", roster_payload(7, 1, {member(named("Bob")), filler(1, 'x')}),
        PacketError::kTypeLength);
    bad("count 1 with two members encoded", roster_payload(7, 1, {member(named("Bob")), member(named("Eve"))}),
        PacketError::kTypeLength);
    {
        // 구조가 틀리고 generation 도 낡았다. 2번이 이긴다.
        bad("malformed and stale is structure first", roster_payload(3, 0, {member(named("Bob"))}),
            PacketError::kTypeLength, RosterContext{true, 5u});
    }
    {
        // 이름 길이 17 이면서 이름에 공백이 든다. 구조가 먼저다.
        bad("17 letters with a space is structure first", simple_roster(7, {named("ABCDEFGH IJKLMNOP")}),
            PacketError::kTypeLength);
    }

    // --- 3. generation ---
    bad("generation equal to the last", simple_roster(5, {named("Bob")}), PacketError::kRosterStale,
        RosterContext{true, 5u});
    bad("generation below the last", simple_roster(4, {named("Bob")}), PacketError::kRosterStale,
        RosterContext{true, 5u});
    bad("generation 0 after 0 was accepted", simple_roster(0, {named("Bob")}), PacketError::kRosterStale,
        RosterContext{true, 0u});
    bad("no wraparound: 0 after max is stale", simple_roster(0, {named("Bob")}), PacketError::kRosterStale,
        RosterContext{true, 0xFFFFFFFFu});
    bad("stale with bad flags is stale first", simple_roster(5, {named("Bob", 0x02)}),
        PacketError::kRosterStale, RosterContext{true, 5u});
    bad("stale with bad name is stale first", simple_roster(5, {named("B b")}),
        PacketError::kRosterStale, RosterContext{true, 5u});

    // --- 4. flags 비트 1~7 ---
    for (const unsigned flags : {0x02u, 0x03u, 0x04u, 0x08u, 0x10u, 0x20u, 0x40u, 0x80u, 0xFEu, 0xFFu}) {
        bad("reserved flag bit set", simple_roster(7, {named("Bob", flags)}), PacketError::kRosterFlags);
    }
    bad("reserved flag bit on the last of five members",
        simple_roster(7, {named("a"), named("b"), named("c"), named("d"), named("e", 0x81)}),
        PacketError::kRosterFlags);
    bad("bad name on member 1 and bad flags on member 2 is flags first",
        simple_roster(7, {named("B b"), named("Eve", 0x40)}), PacketError::kRosterFlags);

    // --- 5. 이름 문자 집합 ---
    bad("space in the middle", simple_roster(7, {named("Al ice")}), PacketError::kRosterName);
    bad("leading space", simple_roster(7, {named(" Alice")}), PacketError::kRosterName);
    bad("trailing space", simple_roster(7, {named("Alice ")}), PacketError::kRosterName);
    bad("one space only", simple_roster(7, {named(" ")}), PacketError::kRosterName);
    bad("nul byte", simple_roster(7, {named(std::string("A\0b", 3))}), PacketError::kRosterName);
    bad("control byte 0x1F", simple_roster(7, {named("A\x1F" "b")}), PacketError::kRosterName);
    bad("newline", simple_roster(7, {named("Ab\n")}), PacketError::kRosterName);
    bad("DEL", simple_roster(7, {named("A\x7F")}), PacketError::kRosterName);
    bad("hyphen", simple_roster(7, {named("A-b")}), PacketError::kRosterName);
    bad("non-ASCII byte", simple_roster(7, {named("A\xEA\xB0\x80")}), PacketError::kRosterName);
    bad("bad name on the last of five members",
        simple_roster(7, {named("a"), named("b"), named("c"), named("d"), named("e e")}),
        PacketError::kRosterName);
    bad("name with a char just past z", simple_roster(7, {named("a{")}), PacketError::kRosterName);
    bad("name with a char just before 0", simple_roster(7, {named("a/")}), PacketError::kRosterName);
    return cases;
}

}  // namespace

TEST_CASE("packet: decode_roster case table", "[packet]") {
    for (const auto& c : make_roster_cases()) {
        INFO(c.why);
        const auto r = decode_roster(view(c.payload), c.context);
        CHECK(r.error == c.expected);
    }
}

TEST_CASE("packet: decode_roster reaches every roster error", "[packet]") {
    std::set<PacketError> seen;
    for (const auto& c : make_roster_cases()) {
        seen.insert(c.expected);
    }
    for (const auto e : {PacketError::kNone, PacketError::kRosterDirection, PacketError::kTypeLength,
                         PacketError::kRosterStale, PacketError::kRosterFlags, PacketError::kRosterName}) {
        CHECK(seen.count(e) == 1);
    }
}

TEST_CASE("packet: decode_roster reads every field in big-endian", "[packet]") {
    MemberSpec host;
    host.peer_id = 0x01020304u;
    host.virtual_ip = 0x0A640001u;
    host.flags = 0x01;
    host.name = "Host_1";
    MemberSpec player;
    player.peer_id = 0xFFFFFFFEu;
    player.virtual_ip = 0x0A640005u;
    player.flags = 0x00;
    player.name = k16;
    const Bytes p = simple_roster(0x80000001u, {host, player});
    const auto r = decode_roster(view(p), kFromHostFresh);
    REQUIRE(r.ok());
    CHECK(r.value.generation == 0x80000001u);
    REQUIRE(r.value.count == 2);
    CHECK(r.value.members[0].peer_id == 0x01020304u);
    CHECK(r.value.members[0].virtual_ip == 0x0A640001u);
    CHECK(r.value.members[0].connected);
    CHECK(r.value.members[0].name_view() == "Host_1");
    CHECK(r.value.members[1].peer_id == 0xFFFFFFFEu);
    CHECK(r.value.members[1].virtual_ip == 0x0A640005u);
    CHECK_FALSE(r.value.members[1].connected);
    CHECK(r.value.members[1].name_view() == k16);
    // 쓰지 않은 칸은 기본값이다. 앞 디코드의 찌꺼기가 남지 않는다.
    CHECK(r.value.members[2] == RosterMember{});
}

TEST_CASE("packet: encode_roster golden bytes and round trip", "[packet]") {
    Roster roster;
    roster.generation = 0x00000102u;
    roster.count = 2;
    roster.members[0] = make_member(0x0A0B0C0Du, 0x0A640001u, true, "Alice");
    roster.members[1] = make_member(0x11223344u, 0x0A640002u, false, "b");

    std::array<std::byte, 135> out{};
    const std::size_t n = encode_roster(roster, out);
    REQUIRE(n == 5 + 15 + 11);
    Bytes want;
    put_u32(want, 0x00000102u);
    put_u8(want, 2);
    put_all(want, hex({0x0A, 0x0B, 0x0C, 0x0D, 0x0A, 0x64, 0x00, 0x01, 0x01, 0x05, 'A', 'l', 'i', 'c', 'e'}));
    put_all(want, hex({0x11, 0x22, 0x33, 0x44, 0x0A, 0x64, 0x00, 0x02, 0x00, 0x01, 'b'}));
    CHECK(Bytes(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(n)) == want);

    const auto back = decode_roster(std::span<const std::byte>(out.data(), n), kFromHostFresh);
    REQUIRE(back.ok());
    CHECK(back.value == roster);
}

TEST_CASE("packet: encode_roster at the 16 and 135 byte bounds", "[packet]") {
    std::array<std::byte, 200> out{};
    Roster one;
    one.count = 1;
    one.members[0] = make_member(1, 2, true, "a");
    CHECK(encode_roster(one, out) == 16);

    Roster five;
    five.count = 5;
    for (std::size_t i = 0; i < 5; ++i) {
        five.members[i] = make_member(static_cast<std::uint32_t>(i), 0, false, k16);
    }
    CHECK(encode_roster(five, out) == 135);
    std::array<std::byte, 134> small{};
    CHECK(encode_roster(five, small) == 0);
}

TEST_CASE("packet: encode_roster refuses what the receiver would drop", "[packet]") {
    std::array<std::byte, 200> out{};
    out.fill(std::byte{0x77});
    auto untouched = [&] {
        for (const auto b : out) {
            if (b != std::byte{0x77}) {
                return false;
            }
        }
        return true;
    };

    Roster zero;
    zero.count = 0;
    CHECK(encode_roster(zero, out) == 0);

    Roster six;
    six.count = 6;
    for (auto& m : six.members) {
        m = make_member(1, 1, true, "a");
    }
    CHECK(encode_roster(six, out) == 0);

    Roster empty_name;
    empty_name.count = 1;
    empty_name.members[0] = make_member(1, 1, true, "");
    CHECK(encode_roster(empty_name, out) == 0);

    Roster spaced;
    spaced.count = 1;
    spaced.members[0] = make_member(1, 1, true, "a b");
    CHECK(encode_roster(spaced, out) == 0);

    Roster too_long;
    too_long.count = 1;
    too_long.members[0] = make_member(1, 1, true, k16);
    too_long.members[0].name_len = 17;
    CHECK(encode_roster(too_long, out) == 0);

    // 뒤 멤버 하나만 틀려도 아무것도 쓰지 않는다.
    Roster late_bad;
    late_bad.count = 2;
    late_bad.members[0] = make_member(1, 1, true, "ok");
    late_bad.members[1] = make_member(2, 2, true, "no-no");
    CHECK(encode_roster(late_bad, out) == 0);

    CHECK(untouched());
}

// ============================================================================
// counter_name
// ============================================================================

TEST_CASE("packet: counter_name gives the counter each section names", "[packet]") {
    struct Row {
        PacketError error;
        std::string_view name;
    };
    const std::vector<Row> table = {
        {PacketError::kNone, ""},
        {PacketError::kShort, "drop_short"},
        {PacketError::kMagic, "drop_magic"},
        {PacketError::kVersion, "drop_version"},
        {PacketError::kUnknownType, "drop_unknown_type"},
        {PacketError::kLength, "drop_length"},
        {PacketError::kTypeLength, "drop_type_length"},
        {PacketError::kBadReason, "drop_bad_reason"},
        {PacketError::kRosterDirection, "drop_roster_direction"},
        {PacketError::kRosterStale, "drop_roster_stale"},
        {PacketError::kRosterFlags, "drop_roster_flags"},
        {PacketError::kRosterName, "drop_roster_name"},
    };
    for (const auto& r : table) {
        INFO(static_cast<unsigned>(r.error));
        CHECK(counter_name(r.error) == r.name);
    }
}

TEST_CASE("roster: the log value lists members as id/vip/bit/name joined by commas",
          "[tunnel][roster]") {
    sangtachi::tunnel::Roster r;
    r.count = 2;
    r.members[0].peer_id = 7;
    r.members[0].virtual_ip = 0x0A640001u;
    r.members[0].connected = true;
    r.members[0].name = {'H', 'o', 's', 't'};
    r.members[0].name_len = 4;
    r.members[1].peer_id = 4294967295u;
    r.members[1].virtual_ip = 0x0A640005u;
    r.members[1].connected = false;
    r.members[1].name = {'b'};
    r.members[1].name_len = 1;
    REQUIRE(sangtachi::tunnel::format_roster_members(r) == "7/10.100.0.1/1/Host,4294967295/10.100.0.5/0/b");
}
