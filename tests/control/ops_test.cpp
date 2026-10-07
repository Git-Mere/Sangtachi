#include "sangtachi/control/ops.hpp"

#include "sangtachi/control/http.hpp"
#include "sangtachi/log.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// 시험 케이스 이름은 ASCII 로만 적는다. 이유는 network/wsa_test.cpp 머리에 있다.
//
// 규칙의 출처는 client/include/sangtachi/control/ops.hpp 머리의 두 표(결과 분류, 응답 필드의
// 형 규칙)다. 응답 행은 좋은 본문 하나에서 한 자리만 바꾼 것이라 어느 규칙이 행을 떨어뜨렸는지
// 보인다.

using namespace sangtachi::control;
using sangtachi::format_line;
using sangtachi::LogLevel;
using sangtachi::network::Endpoint;
using sangtachi::network::parse_ipv4;

namespace {

constexpr std::string_view kToken = "0123456789abcdef0123456789abcdef";
constexpr std::string_view kRoom = "ABCDEF";
constexpr std::uint32_t kSelf = 9;

std::string respond(std::string_view body) {
    return "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
           std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + std::string(body);
}

// base 에서 from 을 to 로 한 번 바꾼다. from 이 없으면 시험이 틀린 것이다.
std::string patch(std::string_view base, std::string_view from, std::string_view to) {
    std::string out(base);
    const auto at = out.find(from);
    REQUIRE(at != std::string::npos);
    out.replace(at, from.size(), to);
    return out;
}

struct Row {
    std::string_view from;  // 비어 있으면 base 그대로
    std::string_view to;
    Outcome outcome;
    Failure failure;
    std::string_view why;
};

constexpr auto S = Outcome::kSuccess;
constexpr auto T = Outcome::kTransient;
constexpr auto D = Outcome::kDefinite;
constexpr auto kOk = Failure::kNone;
constexpr auto kTr = Failure::kTransport;

template <class Interpret>
void run_rows(std::string_view base, std::initializer_list<Row> rows, Interpret interpret) {
    for (const auto& row : rows) {
        INFO("why=" << row.why);
        const std::string body = row.from.empty() ? std::string(base) : patch(base, row.from, row.to);
        INFO("body=" << body);
        const auto got = interpret(respond(body));
        CHECK(got.outcome == row.outcome);
        CHECK(got.failure == row.failure);
        CHECK(got.value.has_value() == (row.outcome == Outcome::kSuccess));
    }
}

// 서버는 연산 필드 뒤에 ok 를 붙인다 (server.py 의 dict(fields, ok=True)).
constexpr std::string_view kIssued =
    "{\"room_id\":\"ABCDEF\",\"peer_id\":123,\"peer_token\":\"0123456789abcdef0123456789abcdef\","
    "\"virtual_ip\":\"10.100.0.2\",\"expires_in_s\":120,\"ok\":true}";

constexpr std::string_view kRegistered = "{\"accepted\":2,\"rejected\":0,\"ok\":true}";

constexpr std::string_view kNotReady =
    "{\"ready\":false,\"peers\":[{\"peer_id\":7,\"virtual_ip\":\"10.100.0.1\",\"ready\":false}],\"ok\":true}";

constexpr std::string_view kReady =
    "{\"ready\":true,\"peers\":[{\"peer_id\":7,\"virtual_ip\":\"10.100.0.1\",\"ready\":true,"
    "\"punch_delay_ms\":1000,\"elapsed_since_ready_ms\":250,\"candidates\":["
    "{\"ip\":\"192.168.0.10\",\"port\":51234,\"kind\":\"local\"},"
    "{\"ip\":\"203.0.113.5\",\"port\":40000,\"kind\":\"reflexive\"}]}],\"ok\":true}";

constexpr std::string_view kHostReport =
    "{\"expires_in_s\":120,\"released\":[5],\"confirmed\":[7],\"peers\":["
    "{\"peer_id\":7,\"virtual_ip\":\"10.100.0.2\",\"ready\":true,"
    "\"punch_delay_ms\":1000,\"elapsed_since_ready_ms\":0,\"candidates\":["
    "{\"ip\":\"198.51.100.7\",\"port\":3478,\"kind\":\"reflexive\"}]},"
    "{\"peer_id\":8,\"virtual_ip\":\"10.100.0.3\",\"ready\":false}],\"ok\":true}";

PeerToken token() {
    return *PeerToken::parse(kToken);
}

ClientNonce nonce() {
    std::array<std::byte, ClientNonce::kBytes> bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<std::byte>(i);
    }
    return ClientNonce::from_bytes(bytes);
}

bool is_lower_hex32(std::string_view s) {
    if (s.size() != 32) {
        return false;
    }
    for (const char c : s) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------- 봉투 (8.3)

TEST_CASE("control_ops: envelope classification table", "[control][ops]") {
    struct Case {
        std::string_view body;
        Outcome outcome;
        Failure failure;
        std::string_view why;
    };
    const Case cases[] = {
        {"{\"ok\":true}", S, kOk, "success"},
        {"{\"ok\":false,\"error\":\"bad_request\",\"message\":\"x\"}", D, Failure::kBadRequest, "4.1"},
        {"{\"ok\":false,\"error\":\"method_not_allowed\"}", D, Failure::kMethodNotAllowed, "4.1"},
        {"{\"ok\":false,\"error\":\"unknown_op\"}", D, Failure::kUnknownOp, "4.1"},
        {"{\"ok\":false,\"error\":\"length_required\"}", D, Failure::kLengthRequired, "4.1"},
        {"{\"ok\":false,\"error\":\"too_large\"}", D, Failure::kTooLarge, "4.1"},
        {"{\"ok\":false,\"error\":\"room_not_found\"}", D, Failure::kRoomNotFound, "4.1"},
        {"{\"ok\":false,\"error\":\"room_expired\"}", D, Failure::kRoomExpired, "4.1"},
        {"{\"ok\":false,\"error\":\"room_full\"}", D, Failure::kRoomFull, "4.1"},
        {"{\"ok\":false,\"error\":\"unauthorized\"}", D, Failure::kUnauthorized, "4.1"},
        {"{\"ok\":false,\"error\":\"rate_limited\"}", D, Failure::kRateLimited, "8.3: rate_limited is definite"},
        {"{\"ok\":false,\"error\":\"internal\"}", T, Failure::kInternal, "8.3: transient"},
        {"{\"ok\":false,\"error\":\"unavailable\"}", T, Failure::kUnavailable, "8.3: transient"},
        {"{\"ok\":false,\"error\":\"brand_new_code\"}", D, Failure::kUnknownCode, "not in 4.1 -> definite"},
        {"{\"ok\":false,\"error\":\"Room_Full\"}", D, Failure::kUnknownCode, "codes are case sensitive"},
        {"{\"ok\":false,\"error\":\"\"}", D, Failure::kUnknownCode, "empty code"},
        {"{\"ok\":false}", T, kTr, "no error field"},
        {"{\"ok\":false,\"error\":5}", T, kTr, "error is not a string"},
        {"{\"ok\":false,\"error\":null}", T, kTr, "error is null"},
        {"{\"error\":\"room_full\"}", T, kTr, "no ok"},
        {"{\"ok\":1}", T, kTr, "ok is an integer, not a bool"},
        {"{\"ok\":\"true\"}", T, kTr, "ok is a string"},
        {"{\"ok\":null}", T, kTr, "ok is null"},
        {"{\"ok\":false,\"error\":\"room_full\",\"room_id\":\"ABCDEF\"}", D, Failure::kRoomFull,
         "success fields on an error do not matter"},
    };
    for (const auto& c : cases) {
        INFO("why=" << c.why << " body=" << c.body);
        const auto parsed = sangtachi::control::http::parse_response(respond(c.body));
        REQUIRE(parsed.ok());
        const auto env = classify_envelope(*parsed.body);
        CHECK(env.outcome == c.outcome);
        CHECK(env.failure == c.failure);
    }
}

TEST_CASE("control_ops: transport errors are transient", "[control][ops]") {
    // 3.5 의 검사에 걸린 응답은 일시 오류다 (8.3).
    const std::string_view broken[] = {
        "",
        "HTTP/1.1 200 OK\r\n\r\n{\"ok\":true}",
        "HTTP/1.1 200 OK\r\nContent-Length: 11\r\nContent-Length: 11\r\n\r\n{\"ok\":true}",
        "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n[]",
        "garbage",
    };
    for (const auto raw : broken) {
        INFO("raw=" << raw);
        const auto got = interpret_create_room(raw);
        CHECK(got.outcome == T);
        CHECK(got.failure == kTr);
        CHECK_FALSE(got.value.has_value());
    }
    const auto none = transport_failure<Issued>();
    CHECK(none.outcome == T);
    CHECK(none.failure == kTr);
}

TEST_CASE("control_ops: the http status does not decide", "[control][ops]") {
    // 3.5 의 4. 200 에 오류 봉투, 500 에 성공 봉투. 본문이 이긴다.
    const std::string err_200 = respond("{\"ok\":false,\"error\":\"room_full\"}");
    CHECK(interpret_join_room(err_200).failure == Failure::kRoomFull);

    std::string ok_500 = respond(kIssued);
    ok_500.replace(9, 6, "500 XX");
    const auto got = interpret_join_room(ok_500);
    CHECK(got.outcome == S);
}

// ---------------------------------------------------------------- create_room, join_room (4.2, 4.3)

TEST_CASE("control_ops: issued response table", "[control][ops]") {
    const std::initializer_list<Row> rows = {
        {"", "", S, kOk, "good"},
        {"\"peer_id\":123", "\"peer_id\":4294967295", S, kOk, "uint32 max"},
        {"\"expires_in_s\":120", "\"expires_in_s\":0", S, kOk, "expired-now is 0"},
        {"\"ok\":true", "\"ok\":true,\"future\":1.5,\"x\":{\"y\":[]}", S, kOk, "unknown keys are ignored"},

        {"\"room_id\":\"ABCDEF\"", "\"room_id\":\"abcdef\"", T, kTr, "response room_id is normalized"},
        {"\"room_id\":\"ABCDEF\"", "\"room_id\":\"ABCDE\"", T, kTr, "five chars"},
        {"\"room_id\":\"ABCDEF\"", "\"room_id\":\"ABCDEFG\"", T, kTr, "seven chars"},
        {"\"room_id\":\"ABCDEF\"", "\"room_id\":\"ABCDEI\"", T, kTr, "I is not in the alphabet"},
        {"\"room_id\":\"ABCDEF\"", "\"room_id\":\"ABCDE0\"", T, kTr, "0 is not in the alphabet"},
        {"\"room_id\":\"ABCDEF\"", "\"room_id\":123456", T, kTr, "number"},
        {"\"room_id\":\"ABCDEF\",", "", T, kTr, "missing room_id"},

        {"\"peer_id\":123", "\"peer_id\":0", T, kTr, "2.2: zero is never issued"},
        {"\"peer_id\":123", "\"peer_id\":-1", T, kTr, "negative"},
        {"\"peer_id\":123", "\"peer_id\":4294967296", T, kTr, "uint32 max + 1"},
        {"\"peer_id\":123", "\"peer_id\":true", T, kTr, "bool is not an integer"},
        {"\"peer_id\":123", "\"peer_id\":\"123\"", T, kTr, "string"},
        {"\"peer_id\":123", "\"peer_id\":123.0", T, kTr, "3.3: no decimal point"},
        {"\"peer_id\":123", "\"peer_id\":1.23e2", T, kTr, "exponent"},
        {"\"peer_id\":123,", "", T, kTr, "missing peer_id"},

        {"0123456789abcdef0123456789abcdef", "0123456789ABCDEF0123456789ABCDEF", T, kTr, "upper-case token"},
        {"0123456789abcdef0123456789abcdef", "0123456789abcdef0123456789abcde", T, kTr, "31 chars"},
        {"0123456789abcdef0123456789abcdef", "0123456789abcdef0123456789abcdef0", T, kTr, "33 chars"},
        {"0123456789abcdef0123456789abcdef", "0123456789abcdef0123456789abcdeg", T, kTr, "non-hex"},
        {"\"peer_token\":\"0123456789abcdef0123456789abcdef\"", "\"peer_token\":null", T, kTr, "null token"},

        {"\"virtual_ip\":\"10.100.0.2\"", "\"virtual_ip\":\"10.100.2\"", T, kTr, "three octets"},
        {"\"virtual_ip\":\"10.100.0.2\"", "\"virtual_ip\":\"010.100.0.2\"", T, kTr, "leading zero (octal)"},
        {"\"virtual_ip\":\"10.100.0.2\"", "\"virtual_ip\":\"10.100.0.256\"", T, kTr, "octet out of range"},
        {"\"virtual_ip\":\"10.100.0.2\"", "\"virtual_ip\":\" 10.100.0.2\"", T, kTr, "space"},
        {"\"virtual_ip\":\"10.100.0.2\"", "\"virtual_ip\":167772162", T, kTr, "integer address"},

        {"\"expires_in_s\":120", "\"expires_in_s\":-1", T, kTr, "negative"},
        {"\"expires_in_s\":120", "\"expires_in_s\":1.5", T, kTr, "fraction"},
        {"\"expires_in_s\":120", "\"expires_in_s\":\"120\"", T, kTr, "string"},
        {"\"expires_in_s\":120", "\"expires_in_s\":false", T, kTr, "bool"},
        {",\"expires_in_s\":120", "", T, kTr, "missing"},
    };
    run_rows(kIssued, rows, [](const std::string& raw) { return interpret_create_room(raw); });
    run_rows(kIssued, rows, [](const std::string& raw) { return interpret_join_room(raw); });
}

TEST_CASE("control_ops: issued values", "[control][ops]") {
    const auto got = interpret_join_room(respond(kIssued));
    REQUIRE(got.value);
    CHECK(got.value->room_id == "ABCDEF");
    CHECK(got.value->peer_id == 123u);
    CHECK(got.value->peer_token == token());
    CHECK(got.value->virtual_ip == 0x0A640002u);
    CHECK(got.value->expires_in_s == 120);
}

// ---------------------------------------------------------------- register_candidate (4.4)

TEST_CASE("control_ops: register response table", "[control][ops]") {
    run_rows(kRegistered,
             {
                 {"", "", S, kOk, "good"},
                 {"\"accepted\":2,\"rejected\":0", "\"accepted\":8,\"rejected\":0", S, kOk, "eight stored"},
                 {"\"accepted\":2,\"rejected\":0", "\"accepted\":1,\"rejected\":7", S, kOk, "sum is eight"},
                 {"\"accepted\":2", "\"accepted\":0", T, kTr, "4.4: zero stored is bad_request, not ok"},
                 {"\"accepted\":2", "\"accepted\":9", T, kTr, "over MAX_CANDIDATES"},
                 {"\"accepted\":2,\"rejected\":0", "\"accepted\":8,\"rejected\":1", T, kTr, "sum over eight"},
                 {"\"rejected\":0", "\"rejected\":9", T, kTr, "rejected over eight"},
                 {"\"rejected\":0", "\"rejected\":-1", T, kTr, "negative"},
                 {"\"accepted\":2", "\"accepted\":true", T, kTr, "bool"},
                 {"\"accepted\":2", "\"accepted\":2.0", T, kTr, "fraction"},
                 {",\"rejected\":0", "", T, kTr, "missing rejected"},
             },
             [](const std::string& raw) { return interpret_register_candidate(raw); });

    const auto got = interpret_register_candidate(respond("{\"accepted\":3,\"rejected\":2,\"ok\":true}"));
    REQUIRE(got.value);
    CHECK(got.value->accepted == 3u);
    CHECK(got.value->rejected == 2u);
}

// ---------------------------------------------------------------- get_peers (4.5)

TEST_CASE("control_ops: get_peers not-ready table", "[control][ops]") {
    run_rows(kNotReady,
             {
                 {"", "", S, kOk, "good"},
                 {"[{\"peer_id\":7,\"virtual_ip\":\"10.100.0.1\",\"ready\":false}]", "[]", S, kOk, "no peers yet"},
                 {"\"ready\":false}", "\"ready\":false,\"note\":1}", S, kOk, "unknown key in an element"},
                 {"{\"ready\":false,\"peers\"", "{\"ready\":true,\"peers\"", T, kTr, "4.5: top ready must match elements"},
                 {"\"ready\":false}", "\"ready\":false,\"candidates\":[]}", T, kTr, "4.5: field set; candidates before ready"},
                 {"\"ready\":false}", "\"ready\":false,\"punch_delay_ms\":1000}", T, kTr, "4.5: field set; delay before ready"},
                 {"\"ready\":false}", "\"ready\":false,\"elapsed_since_ready_ms\":0}", T, kTr, "4.5: field set; elapsed before ready"},
                 {"\"ready\":false}", "\"ready\":0}", T, kTr, "element ready is an integer"},
                 {"{\"ready\":false,", "{\"ready\":\"false\",", T, kTr, "top ready is a string"},
                 {"\"peer_id\":7", "\"peer_id\":0", T, kTr, "2.2: zero"},
                 {"\"peer_id\":7", "\"peer_id\":9", D, Failure::kSelfInPeers, "4.5: own peer_id in peers"},
                 {"\"virtual_ip\":\"10.100.0.1\"", "\"virtual_ip\":\"10.100.0.01\"", T, kTr, "leading zero"},
                 {"\"virtual_ip\":\"10.100.0.1\",", "", T, kTr, "missing virtual_ip"},
                 {"\"peers\":[", "\"peers\":{},\"x\":[", T, kTr, "peers is an object"},
                 {"[{\"peer_id\":7,\"virtual_ip\":\"10.100.0.1\",\"ready\":false}]",
                  "[{\"peer_id\":7,\"virtual_ip\":\"10.100.0.1\",\"ready\":false},"
                  "{\"peer_id\":7,\"virtual_ip\":\"10.100.0.2\",\"ready\":false}]",
                  T, kTr, "2.2: duplicate peer_id"},
                 {"[{\"peer_id\":7,\"virtual_ip\":\"10.100.0.1\",\"ready\":false}]",
                  "[{\"peer_id\":1,\"virtual_ip\":\"10.100.0.1\",\"ready\":false},"
                  "{\"peer_id\":2,\"virtual_ip\":\"10.100.0.2\",\"ready\":false},"
                  "{\"peer_id\":3,\"virtual_ip\":\"10.100.0.3\",\"ready\":false},"
                  "{\"peer_id\":4,\"virtual_ip\":\"10.100.0.4\",\"ready\":false}]",
                  S, kOk, "MAX_PEERS - 1 others"},
                 {"[{\"peer_id\":7,\"virtual_ip\":\"10.100.0.1\",\"ready\":false}]",
                  "[{\"peer_id\":1,\"virtual_ip\":\"10.100.0.1\",\"ready\":false},"
                  "{\"peer_id\":2,\"virtual_ip\":\"10.100.0.2\",\"ready\":false},"
                  "{\"peer_id\":3,\"virtual_ip\":\"10.100.0.3\",\"ready\":false},"
                  "{\"peer_id\":4,\"virtual_ip\":\"10.100.0.4\",\"ready\":false},"
                  "{\"peer_id\":5,\"virtual_ip\":\"10.100.0.5\",\"ready\":false}]",
                  T, kTr, "more than MAX_PEERS - 1"},
                 {"[{\"peer_id\":7,\"virtual_ip\":\"10.100.0.1\",\"ready\":false}]", "[7]", T, kTr,
                  "element is not an object"},
             },
             [](const std::string& raw) { return interpret_get_peers(raw, kSelf); });
}

TEST_CASE("control_ops: get_peers ready table", "[control][ops]") {
    run_rows(kReady,
             {
                 {"", "", S, kOk, "good"},
                 {"\"candidates\":[{\"ip\":\"192.168.0.10\",\"port\":51234,\"kind\":\"local\"},"
                  "{\"ip\":\"203.0.113.5\",\"port\":40000,\"kind\":\"reflexive\"}]",
                  "\"candidates\":[]", S, kOk, "4.5: empty list is the caller's business"},
                 {"\"port\":51234", "\"port\":0", S, kOk, "4.4: port 0 is hygiene, not type"},
                 {"\"ip\":\"192.168.0.10\"", "\"ip\":\"127.0.0.1\"", S, kOk, "loopback is hygiene, not type"},
                 {"\"port\":51234", "\"port\":65535", S, kOk, "highest port"},
                 {"\"punch_delay_ms\":1000", "\"punch_delay_ms\":0", S, kOk, "zero delay"},
                 {"\"kind\":\"local\"}", "\"kind\":\"local\",\"extra\":true}", S, kOk, "unknown key in a candidate"},

                 {"\"ready\":true,\"punch", "\"ready\":false,\"punch", T, kTr, "4.5: field set; ready-only fields on not ready"},
                 {"{\"ready\":true,\"peers\"", "{\"ready\":false,\"peers\"", T, kTr, "4.5: top ready must match"},
                 {"\"punch_delay_ms\":1000,", "", T, kTr, "missing punch_delay_ms"},
                 {"\"elapsed_since_ready_ms\":250,", "", T, kTr, "missing elapsed"},
                 {",\"candidates\":[{\"ip\":\"192.168.0.10\",\"port\":51234,\"kind\":\"local\"},"
                  "{\"ip\":\"203.0.113.5\",\"port\":40000,\"kind\":\"reflexive\"}]",
                  "", T, kTr, "missing candidates"},
                 {"\"punch_delay_ms\":1000", "\"punch_delay_ms\":-1", T, kTr, "negative delay"},
                 {"\"punch_delay_ms\":1000", "\"punch_delay_ms\":4294967296", T, kTr, "delay over uint32"},
                 {"\"punch_delay_ms\":1000", "\"punch_delay_ms\":1000.0", T, kTr, "delay with decimal point"},
                 {"\"elapsed_since_ready_ms\":250", "\"elapsed_since_ready_ms\":-1", T, kTr, "negative elapsed"},
                 {"\"elapsed_since_ready_ms\":250", "\"elapsed_since_ready_ms\":null", T, kTr, "null elapsed"},
                 {"\"candidates\":[", "\"candidates\":{},\"x\":[", T, kTr, "candidates is an object"},
                 {"\"port\":51234", "\"port\":65536", T, kTr, "4.4: port out of range is type"},
                 {"\"port\":51234", "\"port\":-1", T, kTr, "negative port"},
                 {"\"port\":51234", "\"port\":\"51234\"", T, kTr, "string port"},
                 {"\"port\":51234", "\"port\":true", T, kTr, "bool port"},
                 {"\"ip\":\"192.168.0.10\"", "\"ip\":\"10.0.5\"", T, kTr, "inet_aton would read 10.0.0.5"},
                 {"\"ip\":\"192.168.0.10\"", "\"ip\":\"010.0.0.5\"", T, kTr, "inet_aton would read octal"},
                 {"\"ip\":\"192.168.0.10\"", "\"ip\":\"::1\"", T, kTr, "ipv6"},
                 {"\"kind\":\"local\"", "\"kind\":\"relay\"", T, kTr, "4.4: kind allow-list"},
                 {"\"kind\":\"local\"", "\"kind\":\"LOCAL\"", T, kTr, "kind is case sensitive"},
                 {",\"kind\":\"local\"", "", T, kTr, "missing kind"},
                 {"{\"ip\":\"192.168.0.10\",\"port\":51234,\"kind\":\"local\"}", "\"192.168.0.10:51234\"", T, kTr,
                  "candidate is not an object"},
                 {"\"peer_id\":7", "\"peer_id\":9", D, Failure::kSelfInPeers, "4.5: own peer_id"},
             },
             [](const std::string& raw) { return interpret_get_peers(raw, kSelf); });
}

TEST_CASE("control_ops: get_peers values", "[control][ops]") {
    const auto got = interpret_get_peers(respond(kReady), kSelf);
    REQUIRE(got.value);
    CHECK(got.value->ready);
    REQUIRE(got.value->peers.size() == 1);
    const auto& p = got.value->peers[0];
    CHECK(p.peer_id == 7u);
    CHECK(p.virtual_ip == 0x0A640001u);
    CHECK(p.ready);
    CHECK(p.punch_delay_ms == 1000u);
    CHECK(p.elapsed_since_ready_ms == 250);
    REQUIRE(p.candidates.size() == 2);
    CHECK(p.candidates[0] == Candidate{*Endpoint::parse("192.168.0.10:51234"), CandidateKind::kLocal});
    CHECK(p.candidates[1] == Candidate{*Endpoint::parse("203.0.113.5:40000"), CandidateKind::kReflexive});

    // 9개 목록은 형 위반이 아니다.
    std::string nine = "{\"ready\":true,\"peers\":[{\"peer_id\":7,\"virtual_ip\":\"10.100.0.1\",\"ready\":true,"
                       "\"punch_delay_ms\":1000,\"elapsed_since_ready_ms\":0,\"candidates\":[";
    for (int i = 1; i <= 9; ++i) {
        nine += (i > 1 ? "," : "");
        nine += "{\"ip\":\"10.0.0." + std::to_string(i) + "\",\"port\":1000,\"kind\":\"local\"}";
    }
    nine += "]}],\"ok\":true}";
    const auto many = interpret_get_peers(respond(nine), kSelf);
    REQUIRE(many.value);
    // 형 검사는 길이를 보지 않고, 위생(protocol.md 10.1)이 앞에서부터 8개를 남긴다.
    REQUIRE(many.value->peers[0].candidates.size() == 8);
    CHECK(many.value->peers[0].candidates.front().endpoint == *Endpoint::parse("10.0.0.1:1000"));
    CHECK(many.value->peers[0].candidates.back().endpoint == *Endpoint::parse("10.0.0.8:1000"));
}

// ---------------------------------------------------------------- host_report (4.6)

TEST_CASE("control_ops: host_report table", "[control][ops]") {
    run_rows(kHostReport,
             {
                 {"", "", S, kOk, "good"},
                 {"\"released\":[5]", "\"released\":[]", S, kOk, "nothing released"},
                 {"\"expires_in_s\":120", "\"ready\":true,\"expires_in_s\":120", S, kOk,
                  "4.6 has no top-level ready; ignored as unknown"},
                 {"\"released\":[5]", "\"released\":[1,2,3,4]", S, kOk, "MAX_PEERS - 1 released"},
                 {"\"released\":[5]", "\"released\":[1,2,3,4,5]", T, kTr, "over MAX_PEERS - 1"},
                 {"\"confirmed\":[7]", "\"confirmed\":[1,2,3,4,5]", T, kTr, "over MAX_PEERS - 1"},
                 {"\"released\":[5]", "\"released\":[0]", T, kTr, "2.2: zero"},
                 {"\"released\":[5]", "\"released\":[\"5\"]", T, kTr, "string id"},
                 {"\"released\":[5]", "\"released\":[4294967296]", T, kTr, "id over uint32"},
                 {"\"released\":[5]", "\"released\":5", T, kTr, "not an array"},
                 {"\"confirmed\":[7],", "", T, kTr, "missing confirmed"},
                 {"\"expires_in_s\":120", "\"expires_in_s\":-1", T, kTr, "negative expires"},
                 {"{\"peer_id\":8,", "{\"peer_id\":9,", D, Failure::kSelfInPeers, "own peer_id"},
                 {"\"peer_id\":8,\"virtual_ip\":\"10.100.0.3\",\"ready\":false}",
                  "\"peer_id\":8,\"virtual_ip\":\"10.100.0.3\",\"ready\":false,\"candidates\":[]}",
                  T, kTr, "4.5 field set applies to host_report peers"},
             },
             [](const std::string& raw) { return interpret_host_report(raw, kSelf); });

    const auto got = interpret_host_report(respond(kHostReport), kSelf);
    REQUIRE(got.value);
    CHECK(got.value->expires_in_s == 120);
    CHECK(got.value->released == std::vector<std::uint32_t>{5});
    CHECK(got.value->confirmed == std::vector<std::uint32_t>{7});
    REQUIRE(got.value->peers.size() == 2);
    CHECK(got.value->peers[0].ready);
    CHECK_FALSE(got.value->peers[1].ready);
    CHECK(got.value->peers[1].candidates.empty());
}

// ---------------------------------------------------------------- 요청 본문 (4장 요청 표)

TEST_CASE("control_ops: request bodies", "[control][ops]") {
    CHECK(create_room_body(nonce()) == "{\"client_nonce\":\"000102030405060708090a0b0c0d0e0f\"}");
    CHECK(join_room_body("ABCDEF", nonce()) ==
          "{\"room_id\":\"ABCDEF\",\"client_nonce\":\"000102030405060708090a0b0c0d0e0f\"}");

    const Candidate cands[] = {
        {*Endpoint::parse("192.168.0.10:51234"), CandidateKind::kLocal},
        {*Endpoint::parse("203.0.113.5:40000"), CandidateKind::kReflexive},
    };
    CHECK(register_candidate_body(kRoom, 4294967295u, token(), cands) ==
          "{\"room_id\":\"ABCDEF\",\"peer_id\":4294967295,\"peer_token\":\"0123456789abcdef0123456789abcdef\","
          "\"candidates\":[{\"ip\":\"192.168.0.10\",\"port\":51234,\"kind\":\"local\"},"
          "{\"ip\":\"203.0.113.5\",\"port\":40000,\"kind\":\"reflexive\"}]}");

    CHECK(get_peers_body(kRoom, 7, token()) ==
          "{\"room_id\":\"ABCDEF\",\"peer_id\":7,\"peer_token\":\"0123456789abcdef0123456789abcdef\"}");

    const std::uint32_t departed[] = {5, 6};
    CHECK(host_report_body(kRoom, 7, token(), departed, {}) ==
          "{\"room_id\":\"ABCDEF\",\"peer_id\":7,\"peer_token\":\"0123456789abcdef0123456789abcdef\","
          "\"departed\":[5,6],\"confirm\":[]}");
}

TEST_CASE("control_ops: request bodies refuse what the server would reject", "[control][ops]") {
    // 2.1: 대소문자는 가리지 않는다. 알파벳과 길이는 본다.
    CHECK(join_room_body("abcdef", nonce()).has_value());
    CHECK_FALSE(join_room_body("ABCDE0", nonce()).has_value());
    CHECK_FALSE(join_room_body("ABCDEI", nonce()).has_value());
    CHECK_FALSE(join_room_body("ABCDE", nonce()).has_value());
    CHECK_FALSE(join_room_body("ABCDEFG", nonce()).has_value());
    CHECK_FALSE(join_room_body("ABCDE ", nonce()).has_value());
    CHECK_FALSE(join_room_body("ABCDE\"", nonce()).has_value());
    CHECK_FALSE(get_peers_body("", 7, token()).has_value());

    // 4.4: 1개 이상 MAX_CANDIDATES 이하.
    const Candidate one{*Endpoint::parse("10.0.0.1:1000"), CandidateKind::kLocal};
    const std::vector<Candidate> none;
    const std::vector<Candidate> eight(8, one);
    const std::vector<Candidate> nine(9, one);
    CHECK_FALSE(register_candidate_body(kRoom, 7, token(), none).has_value());
    CHECK(register_candidate_body(kRoom, 7, token(), eight).has_value());
    CHECK_FALSE(register_candidate_body(kRoom, 7, token(), nine).has_value());
    // 포트 0 은 서버의 위생 거부이고 형 위반이 아니다. 보낸다.
    const Candidate zero{Endpoint(0x0A000001u, 0), CandidateKind::kLocal};
    CHECK(register_candidate_body(kRoom, 7, token(), std::vector<Candidate>{zero}).has_value());

    // 4.6: MAX_PEERS - 1 이하.
    const std::uint32_t four[] = {1, 2, 3, 4};
    const std::uint32_t five[] = {1, 2, 3, 4, 5};
    CHECK(host_report_body(kRoom, 7, token(), four, four).has_value());
    CHECK_FALSE(host_report_body(kRoom, 7, token(), five, {}).has_value());
    CHECK_FALSE(host_report_body(kRoom, 7, token(), {}, five).has_value());
}

TEST_CASE("control_ops: peer_token format", "[control][ops]") {
    CHECK(PeerToken::parse(kToken).has_value());
    CHECK_FALSE(PeerToken::parse("0123456789ABCDEF0123456789abcdef").has_value());
    CHECK_FALSE(PeerToken::parse("0123456789abcdef0123456789abcde").has_value());
    CHECK_FALSE(PeerToken::parse("0123456789abcdef0123456789abcdef0").has_value());
    CHECK_FALSE(PeerToken::parse("0123456789abcdef0123456789abcdeg").has_value());
    CHECK_FALSE(PeerToken::parse("").has_value());
}

TEST_CASE("control_ops: client_nonce is 128-bit lowercase hex", "[control][ops]") {
    std::array<std::byte, ClientNonce::kBytes> bytes{};
    bytes.fill(std::byte{0xAB});
    bytes[15] = std::byte{0x0F};
    CHECK(ClientNonce::from_bytes(bytes).reveal_for_request() == "ababababababababababababababab0f");

    const auto a = generate_client_nonce();
    const auto b = generate_client_nonce();
    REQUIRE(a);
    REQUIRE(b);
    CHECK(is_lower_hex32(a->reveal_for_request()));
    CHECK(is_lower_hex32(b->reveal_for_request()));
    // 같을 확률은 2^-128 이다.
    CHECK_FALSE(*a == *b);
}

// ---------------------------------------------------------------- 로그 필드 (architecture.md 9장)

TEST_CASE("control_ops: control.result fields", "[control][ops]") {
    struct Case {
        sangtachi::control::http::Op op;
        Outcome outcome;
        Failure failure;
        std::string_view line;
    };
    using sangtachi::control::http::Op;
    const Case cases[] = {
        {Op::kJoinRoom, S, kOk, "INFO control.result op=join_room ok=true error=-"},
        {Op::kGetPeers, T, kTr, "INFO control.result op=get_peers ok=false error=transport"},
        {Op::kCreateRoom, D, Failure::kRoomFull, "INFO control.result op=create_room ok=false error=room_full"},
        {Op::kHostReport, T, Failure::kInternal, "INFO control.result op=host_report ok=false error=internal"},
        {Op::kRegisterCandidate, D, Failure::kUnknownCode,
         "INFO control.result op=register_candidate ok=false error=unknown_code"},
    };
    for (const auto& c : cases) {
        const auto fields = control_result_fields(c.op, c.outcome, c.failure);
        CHECK(format_line(LogLevel::Info, "control.result", fields) == c.line);
    }
}

TEST_CASE("control_ops: failure tokens are the 4.1 strings", "[control][ops]") {
    // 9장 control.result 의 error 는 4.1 의 코드 문자열 그대로다. 분류한 것을 다시 적으면 같다.
    const std::string_view codes[] = {
        "bad_request", "method_not_allowed", "unknown_op", "length_required", "too_large",
        "room_not_found", "room_expired", "room_full", "unauthorized", "rate_limited",
        "internal", "unavailable",
    };
    for (const auto code : codes) {
        INFO("code=" << code);
        const std::string body = "{\"ok\":false,\"error\":\"" + std::string(code) + "\"}";
        const auto parsed = sangtachi::control::http::parse_response(respond(body));
        REQUIRE(parsed.ok());
        CHECK(to_token(classify_envelope(*parsed.body).failure) == code);
    }
}

TEST_CASE("control_ops: control.peers fields", "[control][ops]") {
    const auto got = interpret_get_peers(respond(kReady), kSelf);
    REQUIRE(got.value);
    CHECK(format_line(LogLevel::Info, "control.peers", control_peers_fields(got.value->peers[0])) ==
          "INFO control.peers peer_id=7 virtual_ip=10.100.0.1 candidates=192.168.0.10:51234,203.0.113.5:40000");

    PeerView empty;
    empty.peer_id = 8;
    empty.virtual_ip = 0x0A640003u;
    CHECK(format_line(LogLevel::Info, "control.peers", control_peers_fields(empty)) ==
          "INFO control.peers peer_id=8 virtual_ip=10.100.0.3 candidates=");
}

TEST_CASE("control_ops: secrets never reach a log line or an error token", "[control][ops]") {
    // architecture.md 3.5: peer_token 은 프로세스 밖으로 나가지 않고 room_id 는 표준 출력에만 낸다.
    // 서버가 비밀을 message, 모르는 오류 코드, 모르는 키에 되돌려 보내도 로그 줄에 실리지 않는다.
    const std::string t(kToken);
    const std::string r(kRoom);
    const std::string bodies[] = {
        std::string(kIssued),
        "{\"ok\":false,\"error\":\"unauthorized\",\"message\":\"" + t + " " + r + "\"}",
        "{\"ok\":false,\"error\":\"" + t + "\"}",
        "{\"ok\":false,\"error\":\"" + r + "\"}",
        "{\"ok\":true,\"ready\":false,\"peers\":[],\"peer_token\":\"" + t + "\",\"room_id\":\"" + r + "\"}",
        "{\"ok\":true,\"peer_token\":\"" + t + "\"}",  // 형이 틀린 성공
    };
    using sangtachi::control::http::Op;
    for (const auto& body : bodies) {
        INFO("body=" << body);
        const std::string raw = respond(body);
        std::vector<std::string> lines;
        const auto issued = interpret_create_room(raw);
        lines.push_back(format_line(LogLevel::Info, "control.result",
                                    control_result_fields(Op::kCreateRoom, issued.outcome, issued.failure)));
        const auto peers = interpret_get_peers(raw, kSelf);
        lines.push_back(format_line(LogLevel::Info, "control.result",
                                    control_result_fields(Op::kGetPeers, peers.outcome, peers.failure)));
        if (peers.value) {
            for (const auto& p : peers.value->peers) {
                lines.push_back(format_line(LogLevel::Info, "control.peers", control_peers_fields(p)));
            }
        }
        lines.emplace_back(sangtachi::control::http::to_token(sangtachi::control::http::parse_response(raw).error));
        for (const auto& line : lines) {
            INFO("line=" << line);
            CHECK(line.find(t) == std::string::npos);
            CHECK(line.find(r) == std::string::npos);
        }
    }
}

// ---------------------------------------------------------------- 받은 후보 위생 (protocol.md 10.1)

namespace {

Candidate cand(std::string_view text, CandidateKind kind = CandidateKind::kLocal) {
    const auto ep = Endpoint::parse(text);
    REQUIRE(ep);
    return Candidate{*ep, kind};
}

struct HygieneRow {
    std::vector<std::string_view> in;   // 받은 순서
    std::vector<std::string_view> out;  // 남는 순서
    std::string_view why;
};

}  // namespace

TEST_CASE("control_ops: received candidate hygiene table", "[control][ops]") {
    const HygieneRow rows[] = {
        {{"192.168.0.10:5000", "203.0.113.5:40000"}, {"192.168.0.10:5000", "203.0.113.5:40000"}, "clean list keeps order"},
        {{}, {}, "empty stays empty"},
        {{"255.255.255.255:5000"}, {}, "limited broadcast"},
        {{"10.0.0.255:5000"}, {"10.0.0.255:5000"}, "directed broadcast is not judged"},
        {{"224.0.0.1:5000"}, {}, "multicast low"},
        {{"239.255.255.255:5000"}, {}, "multicast high"},
        {{"223.255.255.255:5000"}, {"223.255.255.255:5000"}, "just below multicast"},
        {{"240.0.0.1:5000"}, {"240.0.0.1:5000"}, "240/4 is not in the list"},
        {{"0.0.0.0:5000"}, {}, "unspecified"},
        {{"0.0.0.1:5000"}, {"0.0.0.1:5000"}, "rest of 0/8 is not in the list"},
        {{"127.0.0.1:5000"}, {}, "loopback"},
        {{"127.255.255.254:5000"}, {}, "loopback whole /8"},
        {{"126.255.255.255:5000"}, {"126.255.255.255:5000"}, "just below loopback"},
        {{"128.0.0.0:5000"}, {"128.0.0.0:5000"}, "just above loopback"},
        {{"10.0.0.5:0"}, {}, "port 0"},
        {{"10.0.0.5:1"}, {"10.0.0.5:1"}, "port 1"},
        {{"10.0.0.5:5000", "10.0.0.5:5000"}, {"10.0.0.5:5000"}, "duplicate keeps one"},
        {{"10.0.0.5:5000", "10.0.0.5:5001"}, {"10.0.0.5:5000", "10.0.0.5:5001"}, "same ip, different port is not a duplicate"},
        {{"10.0.0.6:1", "10.0.0.5:5000", "10.0.0.6:1"}, {"10.0.0.6:1", "10.0.0.5:5000"}, "first one stays in place"},
        // 순서: 위생 거부 -> 중복 제거 -> 앞에서 8개. 상한이 앞이면 걸러질 것이 자리를 차지한다.
        {{"127.0.0.1:1", "127.0.0.1:2", "10.0.0.1:1", "10.0.0.2:1", "10.0.0.3:1", "10.0.0.4:1",
          "10.0.0.5:1", "10.0.0.6:1", "10.0.0.7:1", "10.0.0.8:1"},
         {"10.0.0.1:1", "10.0.0.2:1", "10.0.0.3:1", "10.0.0.4:1", "10.0.0.5:1", "10.0.0.6:1",
          "10.0.0.7:1", "10.0.0.8:1"},
         "rejects do not take a slot under the limit"},
        {{"10.0.0.1:1", "10.0.0.1:1", "10.0.0.2:1", "10.0.0.3:1", "10.0.0.4:1", "10.0.0.5:1",
          "10.0.0.6:1", "10.0.0.7:1", "10.0.0.8:1"},
         {"10.0.0.1:1", "10.0.0.2:1", "10.0.0.3:1", "10.0.0.4:1", "10.0.0.5:1", "10.0.0.6:1",
          "10.0.0.7:1", "10.0.0.8:1"},
         "duplicates do not take a slot under the limit"},
        {{"10.0.0.1:1", "10.0.0.2:1", "10.0.0.3:1", "10.0.0.4:1", "10.0.0.5:1", "10.0.0.6:1",
          "10.0.0.7:1", "10.0.0.8:1", "10.0.0.9:1"},
         {"10.0.0.1:1", "10.0.0.2:1", "10.0.0.3:1", "10.0.0.4:1", "10.0.0.5:1", "10.0.0.6:1",
          "10.0.0.7:1", "10.0.0.8:1"},
         "limit keeps the first eight"},
    };
    for (const auto& row : rows) {
        INFO("why=" << row.why);
        std::vector<Candidate> in;
        for (const auto t : row.in) {
            in.push_back(cand(t));
        }
        std::vector<Candidate> want;
        for (const auto t : row.out) {
            want.push_back(cand(t));
        }
        CHECK(sanitize_received_candidates(in) == want);
    }
}

TEST_CASE("control_ops: a duplicate keeps the kind of the first", "[control][ops]") {
    const std::vector<Candidate> in = {
        cand("203.0.113.5:40000", CandidateKind::kReflexive),
        cand("203.0.113.5:40000", CandidateKind::kLocal),
    };
    const auto out = sanitize_received_candidates(in);
    REQUIRE(out.size() == 1);
    CHECK(out[0].kind == CandidateKind::kReflexive);
}

TEST_CASE("control_ops: type check covers every candidate before the limit", "[control][ops]") {
    // protocol.md 10.1 의 순서는 형 검사 -> 위생 -> 중복 제거 -> 상한이다. 유효한 8개 뒤의 9번째가
    // 형이 틀리면 응답 전체가 전송 오류다. 상한으로 먼저 자르는 구현은 9번째를 보지 못한다.
    auto list_with = [](std::string_view ninth) {
        std::string list;
        for (int i = 1; i <= 8; ++i) {
            list += "{\"ip\":\"10.0.0." + std::to_string(i) + "\",\"port\":1000,\"kind\":\"local\"},";
        }
        return list + std::string(ninth);
    };
    const std::string_view bad_ninth[] = {
        "{\"ip\":\"10.0.0.9\",\"port\":65536,\"kind\":\"local\"}",
        "{\"ip\":\"10.0.9\",\"port\":1000,\"kind\":\"local\"}",
        "{\"ip\":\"10.0.0.9\",\"port\":1000,\"kind\":\"relay\"}",
        "\"10.0.0.9:1000\"",
    };
    const std::string ok_ninth = "{\"ip\":\"10.0.0.9\",\"port\":1000,\"kind\":\"local\"}";
    for (const auto ninth : bad_ninth) {
        INFO("ninth=" << ninth);
        const std::string get = "{\"ready\":true,\"peers\":[{\"peer_id\":7,\"virtual_ip\":\"10.100.0.1\","
                                "\"ready\":true,\"punch_delay_ms\":1000,\"elapsed_since_ready_ms\":0,"
                                "\"candidates\":[" + list_with(ninth) + "]}],\"ok\":true}";
        const auto g = interpret_get_peers(respond(get), kSelf);
        CHECK(g.outcome == T);
        CHECK(g.failure == kTr);

        const std::string host = "{\"expires_in_s\":120,\"released\":[],\"confirmed\":[],\"peers\":["
                                 "{\"peer_id\":7,\"virtual_ip\":\"10.100.0.2\",\"ready\":true,"
                                 "\"punch_delay_ms\":1000,\"elapsed_since_ready_ms\":0,"
                                 "\"candidates\":[" + list_with(ninth) + "]}],\"ok\":true}";
        const auto h = interpret_host_report(respond(host), kSelf);
        CHECK(h.outcome == T);
        CHECK(h.failure == kTr);
    }
    // 대조군: 9번째가 형으로 맞으면 성공이고 위생이 앞의 8개를 남긴다.
    const std::string good = "{\"ready\":true,\"peers\":[{\"peer_id\":7,\"virtual_ip\":\"10.100.0.1\","
                             "\"ready\":true,\"punch_delay_ms\":1000,\"elapsed_since_ready_ms\":0,"
                             "\"candidates\":[" + list_with(ok_ninth) + "]}],\"ok\":true}";
    const auto g = interpret_get_peers(respond(good), kSelf);
    REQUIRE(g.value);
    CHECK(g.value->peers[0].candidates.size() == 8);
}

TEST_CASE("control_ops: interpret applies hygiene to both peer lists", "[control][ops]") {
    // get_peers: 포트 0 과 루프백이 빠진다. 형 위반이 아니므로 응답은 성공이다.
    const std::string bad_get = patch(patch(kReady, "\"port\":51234", "\"port\":0"),
                                      "\"ip\":\"203.0.113.5\"", "\"ip\":\"127.0.0.1\"");
    const auto g = interpret_get_peers(respond(bad_get), kSelf);
    REQUIRE(g.value);
    CHECK(g.value->peers[0].candidates.empty());
    CHECK_FALSE(is_punch_ready(g.value->peers[0]));  // 8.4: 위생 뒤 0개면 준비 완료가 아니다
    CHECK(g.value->ready);                           // 서버의 ready 는 그대로 넘긴다

    // host_report: 같은 위생이 상대 후보에 걸린다.
    const std::string bad_host = patch(kHostReport, "\"ip\":\"198.51.100.7\"", "\"ip\":\"224.0.0.9\"");
    const auto h = interpret_host_report(respond(bad_host), kSelf);
    REQUIRE(h.value);
    CHECK(h.value->peers[0].candidates.empty());
    CHECK_FALSE(is_punch_ready(h.value->peers[0]));

    const auto good = interpret_host_report(respond(kHostReport), kSelf);
    REQUIRE(good.value);
    CHECK(is_punch_ready(good.value->peers[0]));
    CHECK_FALSE(is_punch_ready(good.value->peers[1]));  // ready 가 아니다
}

TEST_CASE("control_ops: control.peers logs the sanitized list", "[control][ops]") {
    // 손으로 만든 PeerView 라도 위생 뒤의 목록만 줄에 실린다 (8.4 control.peers 의 필드).
    PeerView p;
    p.peer_id = 7;
    p.virtual_ip = *parse_ipv4("10.100.0.1");
    p.ready = true;
    p.candidates = {cand("127.0.0.1:5000"), cand("192.168.0.10:5000"), cand("192.168.0.10:5000"),
                    cand("10.0.0.1:0")};
    CHECK(format_line(LogLevel::Info, "control.peers", control_peers_fields(p)) ==
          "INFO control.peers peer_id=7 virtual_ip=10.100.0.1 candidates=192.168.0.10:5000");
}

// ---------------------------------------------------------------- host_report 의 행동 (4.6)

TEST_CASE("control_ops: host_report action table", "[control][ops]") {
    using A = HostReportAction;
    struct Case {
        std::string body;  // 응답 본문. 비어 있으면 받지 못한 응답이다
        A action;
        std::string_view why;
    };
    const Case cases[] = {
        {std::string(kHostReport), A::kNextCycle, "success keeps the cycle"},
        {"{\"ok\":false,\"error\":\"room_expired\"}", A::kStop, "4.6 row 1"},
        {"{\"ok\":false,\"error\":\"room_not_found\"}", A::kStop, "4.6 row 1"},
        {"{\"ok\":false,\"error\":\"unauthorized\"}", A::kStop, "4.6 row 1"},
        {"{\"ok\":false,\"error\":\"bad_request\"}", A::kStop, "4.6 row 1"},
        {"{\"ok\":false,\"error\":\"room_full\"}", A::kStop, "other definite: rest of 4.1"},
        {"{\"ok\":false,\"error\":\"method_not_allowed\"}", A::kStop, "other definite: rest of 4.1"},
        {"{\"ok\":false,\"error\":\"unknown_op\"}", A::kStop, "other definite: rest of 4.1"},
        {"{\"ok\":false,\"error\":\"length_required\"}", A::kStop, "other definite: rest of 4.1"},
        {"{\"ok\":false,\"error\":\"too_large\"}", A::kStop, "other definite: rest of 4.1"},
        {"{\"ok\":false,\"error\":\"brand_new\"}", A::kStop, "other definite: unknown_code"},
        {patch(kHostReport, "{\"peer_id\":8,", "{\"peer_id\":9,"), A::kStop, "other definite: self_in_peers"},
        {"{\"ok\":false,\"error\":\"rate_limited\"}", A::kNextCycle, "4.6 row 2"},
        {"{\"ok\":false,\"error\":\"internal\"}", A::kNextCycle, "4.6 row 3"},
        {"{\"ok\":false,\"error\":\"unavailable\"}", A::kNextCycle, "4.6 row 3"},
        {patch(kHostReport, "\"expires_in_s\":120", "\"expires_in_s\":-1"), A::kNextCycle,
         "4.6 row 3: malformed success is transport"},
        {"", A::kNextCycle, "4.6 row 3: nothing received"},
    };
    for (const auto& c : cases) {
        INFO("why=" << c.why << " body=" << c.body);
        const auto reply = c.body.empty() ? transport_failure<HostReport>()
                                          : interpret_host_report(respond(c.body), kSelf);
        CHECK(host_report_action(reply) == c.action);
    }
}
