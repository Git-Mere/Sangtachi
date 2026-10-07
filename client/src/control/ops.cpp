#include "sangtachi/control/ops.hpp"

#include "sangtachi/control/http.hpp"
#include "sangtachi/control/json.hpp"
#include "sangtachi/log.hpp"
#include "sangtachi/network/endpoint.hpp"
#include "sangtachi/platform/random.hpp"
#include "sangtachi/protocol_constants.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sangtachi::control {
namespace {

using json::Value;

constexpr std::string_view kRoomAlphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";  // 2.1
constexpr std::size_t kRoomLength = 6;
constexpr std::size_t kHex32 = 32;
constexpr std::uint32_t kUint32Max = 0xFFFFFFFFu;
// 4.6 departed·confirm 과 응답의 peers·released·confirmed 의 상한. MAX_PEERS - 1.
constexpr std::size_t kMaxOthers = protocol::kMaxPeers - 1;

bool is_lower_hex32(std::string_view text) noexcept {
    if (text.size() != kHex32) {
        return false;
    }
    for (const char c : text) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

// 응답의 room_id. 서버가 정규화한 대문자만 받는다.
bool is_room_id_exact(std::string_view text) noexcept {
    if (text.size() != kRoomLength) {
        return false;
    }
    for (const char c : text) {
        if (kRoomAlphabet.find(c) == std::string_view::npos) {
            return false;
        }
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------- 비밀

std::optional<PeerToken> PeerToken::parse(std::string_view text) {
    if (!is_lower_hex32(text)) {
        return std::nullopt;
    }
    return PeerToken(std::string(text));
}

ClientNonce ClientNonce::from_bytes(std::span<const std::byte, kBytes> bytes) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(kBytes * 2);
    for (const std::byte b : bytes) {
        const auto v = static_cast<unsigned>(b);
        hex.push_back(kHex[v >> 4]);
        hex.push_back(kHex[v & 0x0Fu]);
    }
    return ClientNonce(std::move(hex));
}

std::optional<ClientNonce> generate_client_nonce() {
    std::array<std::byte, ClientNonce::kBytes> bytes{};
    if (!platform::random_bytes(bytes)) {
        return std::nullopt;
    }
    return ClientNonce::from_bytes(bytes);
}

std::string_view to_token(CandidateKind kind) noexcept {
    switch (kind) {
        case CandidateKind::kLocal:     return "local";
        case CandidateKind::kReflexive: return "reflexive";
    }
    return "local";
}

bool is_room_id_input(std::string_view text) noexcept {
    if (text.size() != kRoomLength) {
        return false;
    }
    for (char c : text) {
        // ASCII 소문자만 대문자로 본다. 그 밖의 바이트는 알파벳 검사에서 걸린다 (2.1).
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
        if (kRoomAlphabet.find(c) == std::string_view::npos) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------- 요청 본문

namespace {

Value auth_fields(std::string_view room_id, std::uint32_t peer_id, const PeerToken& token) {
    Value body = Value::object();
    body.insert("room_id", Value::string(std::string(room_id)));
    body.insert("peer_id", Value::integer(peer_id));
    body.insert("peer_token", Value::string(std::string(token.reveal_for_request())));
    return body;
}

Value peer_id_array(std::span<const std::uint32_t> ids) {
    Value arr = Value::array();
    for (const std::uint32_t id : ids) {
        arr.push_back(Value::integer(id));
    }
    return arr;
}

}  // namespace

std::optional<std::string> create_room_body(const ClientNonce& nonce) {
    Value body = Value::object();
    body.insert("client_nonce", Value::string(std::string(nonce.reveal_for_request())));
    return json::serialize(body);
}

std::optional<std::string> join_room_body(std::string_view room_id, const ClientNonce& nonce) {
    if (!is_room_id_input(room_id)) {
        return std::nullopt;
    }
    Value body = Value::object();
    body.insert("room_id", Value::string(std::string(room_id)));
    body.insert("client_nonce", Value::string(std::string(nonce.reveal_for_request())));
    return json::serialize(body);
}

std::optional<std::string> register_candidate_body(std::string_view room_id,
                                                   std::uint32_t peer_id,
                                                   const PeerToken& token,
                                                   std::span<const Candidate> candidates) {
    if (!is_room_id_input(room_id)) {
        return std::nullopt;
    }
    if (candidates.empty() || candidates.size() > protocol::kMaxCandidates) {
        return std::nullopt;
    }
    Value body = auth_fields(room_id, peer_id, token);
    Value list = Value::array();
    for (const auto& c : candidates) {
        Value item = Value::object();
        item.insert("ip", Value::string(network::format_ipv4(c.endpoint.address())));
        item.insert("port", Value::integer(c.endpoint.port()));
        item.insert("kind", Value::string(std::string(to_token(c.kind))));
        list.push_back(std::move(item));
    }
    body.insert("candidates", std::move(list));
    return json::serialize(body);
}

std::optional<std::string> get_peers_body(std::string_view room_id, std::uint32_t peer_id,
                                          const PeerToken& token) {
    if (!is_room_id_input(room_id)) {
        return std::nullopt;
    }
    return json::serialize(auth_fields(room_id, peer_id, token));
}

std::optional<std::string> host_report_body(std::string_view room_id, std::uint32_t peer_id,
                                            const PeerToken& token,
                                            std::span<const std::uint32_t> departed,
                                            std::span<const std::uint32_t> confirm) {
    if (!is_room_id_input(room_id)) {
        return std::nullopt;
    }
    if (departed.size() > kMaxOthers || confirm.size() > kMaxOthers) {
        return std::nullopt;
    }
    // 빈 배열도 적는다. 4.6 은 빈 배열과 생략을 같은 뜻으로 받는다.
    Value body = auth_fields(room_id, peer_id, token);
    body.insert("departed", peer_id_array(departed));
    body.insert("confirm", peer_id_array(confirm));
    return json::serialize(body);
}

// ---------------------------------------------------------------- 결과 분류

std::string_view to_token(Failure failure) noexcept {
    switch (failure) {
        case Failure::kNone:             return "-";
        case Failure::kTransport:        return "transport";
        case Failure::kBadRequest:       return "bad_request";
        case Failure::kMethodNotAllowed: return "method_not_allowed";
        case Failure::kUnknownOp:        return "unknown_op";
        case Failure::kLengthRequired:   return "length_required";
        case Failure::kTooLarge:         return "too_large";
        case Failure::kRoomNotFound:     return "room_not_found";
        case Failure::kRoomExpired:      return "room_expired";
        case Failure::kRoomFull:         return "room_full";
        case Failure::kUnauthorized:     return "unauthorized";
        case Failure::kRateLimited:      return "rate_limited";
        case Failure::kInternal:         return "internal";
        case Failure::kUnavailable:      return "unavailable";
        case Failure::kUnknownCode:      return "unknown_code";
        case Failure::kSelfInPeers:      return "self_in_peers";
    }
    return "transport";
}

namespace {

struct CodeRow {
    std::string_view code;
    Failure failure;
};

// 4.1 공통 봉투의 오류 코드 표. 허용 목록이다.
constexpr CodeRow kCodes[] = {
    {"bad_request",        Failure::kBadRequest},
    {"method_not_allowed", Failure::kMethodNotAllowed},
    {"unknown_op",         Failure::kUnknownOp},
    {"length_required",    Failure::kLengthRequired},
    {"too_large",          Failure::kTooLarge},
    {"room_not_found",     Failure::kRoomNotFound},
    {"room_expired",       Failure::kRoomExpired},
    {"room_full",          Failure::kRoomFull},
    {"unauthorized",       Failure::kUnauthorized},
    {"rate_limited",       Failure::kRateLimited},
    {"internal",           Failure::kInternal},
    {"unavailable",        Failure::kUnavailable},
};

}  // namespace

Envelope classify_envelope(const json::Value& body) {
    const Value* ok = body.find("ok");
    const auto ok_value = ok ? ok->as_bool() : std::nullopt;
    if (!ok_value) {
        return {Outcome::kTransient, Failure::kTransport};
    }
    if (*ok_value) {
        return {Outcome::kSuccess, Failure::kNone};
    }
    const Value* error = body.find("error");
    const std::string* code = error ? error->as_string() : nullptr;
    if (code == nullptr) {
        return {Outcome::kTransient, Failure::kTransport};
    }
    for (const auto& row : kCodes) {
        if (row.code == *code) {
            // 8.3 일시 오류는 internal, unavailable 둘과 전송 오류뿐이다.
            const bool transient =
                row.failure == Failure::kInternal || row.failure == Failure::kUnavailable;
            return {transient ? Outcome::kTransient : Outcome::kDefinite, row.failure};
        }
    }
    return {Outcome::kDefinite, Failure::kUnknownCode};
}

// ---------------------------------------------------------------- 응답 필드

namespace {

std::optional<std::uint32_t> get_uint32(const Value* v, std::uint32_t min,
                                        std::uint32_t max) noexcept {
    if (v == nullptr) {
        return std::nullopt;
    }
    const auto i = v->as_integer();
    if (!i || *i < static_cast<std::int64_t>(min) || *i > static_cast<std::int64_t>(max)) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(*i);
}

std::optional<std::int64_t> get_nonnegative(const Value* v) noexcept {
    if (v == nullptr) {
        return std::nullopt;
    }
    const auto i = v->as_integer();
    if (!i || *i < 0) {
        return std::nullopt;
    }
    return *i;
}

std::optional<std::uint32_t> get_peer_id(const Value* v) noexcept {
    return get_uint32(v, 1, kUint32Max);  // 2.2 0 제외
}

std::optional<std::uint32_t> get_ipv4(const Value* v) {
    const std::string* s = v ? v->as_string() : nullptr;
    if (s == nullptr) {
        return std::nullopt;
    }
    return network::parse_ipv4(*s);
}

std::optional<Candidate> decode_candidate(const Value& v) {
    if (v.kind() != json::Kind::kObject) {
        return std::nullopt;
    }
    const auto ip = get_ipv4(v.find("ip"));
    // 포트 0 은 형 위반이 아니라 위생 거부다 (4.4). 형은 0~65535 의 정수다.
    const auto port = get_uint32(v.find("port"), 0, 65535);
    const Value* kind_v = v.find("kind");
    const std::string* kind = kind_v ? kind_v->as_string() : nullptr;
    if (!ip || !port || kind == nullptr) {
        return std::nullopt;
    }
    Candidate c;
    c.endpoint = network::Endpoint(*ip, static_cast<std::uint16_t>(*port));
    if (*kind == "local") {
        c.kind = CandidateKind::kLocal;
    } else if (*kind == "reflexive") {
        c.kind = CandidateKind::kReflexive;
    } else {
        return std::nullopt;
    }
    return c;
}

std::optional<PeerView> decode_peer(const Value& v) {
    if (v.kind() != json::Kind::kObject) {
        return std::nullopt;
    }
    PeerView p;
    const auto id = get_peer_id(v.find("peer_id"));
    const auto vip = get_ipv4(v.find("virtual_ip"));
    const Value* ready_v = v.find("ready");
    const auto ready = ready_v ? ready_v->as_bool() : std::nullopt;
    if (!id || !vip || !ready) {
        return std::nullopt;
    }
    p.peer_id = *id;
    p.virtual_ip = *vip;
    p.ready = *ready;

    const Value* delay_v = v.find("punch_delay_ms");
    const Value* elapsed_v = v.find("elapsed_since_ready_ms");
    const Value* cands_v = v.find("candidates");
    if (!p.ready) {
        // 4.5 원소의 필드 집합. 준비 전에는 셋 다 없어야 한다.
        if (delay_v != nullptr || elapsed_v != nullptr || cands_v != nullptr) {
            return std::nullopt;
        }
        return p;
    }
    const auto delay = get_uint32(delay_v, 0, kUint32Max);
    const auto elapsed = get_nonnegative(elapsed_v);
    const auto* cands = cands_v ? cands_v->as_array() : nullptr;
    if (!delay || !elapsed || cands == nullptr) {
        return std::nullopt;
    }
    p.punch_delay_ms = *delay;
    p.elapsed_since_ready_ms = *elapsed;
    std::vector<Candidate> typed;
    for (const auto& item : *cands) {
        auto c = decode_candidate(item);
        if (!c) {
            return std::nullopt;  // 형 위반은 응답 전체의 문제다 (protocol.md 10.1, 3.5 의 7)
        }
        typed.push_back(*c);
    }
    p.candidates = sanitize_received_candidates(typed);
    return p;
}

std::optional<std::vector<PeerView>> decode_peers(const Value* v) {
    const auto* arr = v ? v->as_array() : nullptr;
    if (arr == nullptr || arr->size() > kMaxOthers) {
        return std::nullopt;
    }
    std::vector<PeerView> out;
    for (const auto& item : *arr) {
        auto p = decode_peer(item);
        if (!p) {
            return std::nullopt;
        }
        for (const auto& seen : out) {
            if (seen.peer_id == p->peer_id) {
                return std::nullopt;  // 2.2 방 안 유일
            }
        }
        out.push_back(std::move(*p));
    }
    return out;
}

std::optional<std::vector<std::uint32_t>> decode_id_list(const Value* v) {
    const auto* arr = v ? v->as_array() : nullptr;
    if (arr == nullptr || arr->size() > kMaxOthers) {
        return std::nullopt;
    }
    std::vector<std::uint32_t> out;
    for (const auto& item : *arr) {
        const auto id = get_peer_id(&item);
        if (!id) {
            return std::nullopt;
        }
        out.push_back(*id);
    }
    return out;
}

bool contains_peer(const std::vector<PeerView>& peers, std::uint32_t id) noexcept {
    for (const auto& p : peers) {
        if (p.peer_id == id) {
            return true;
        }
    }
    return false;
}

std::optional<Issued> decode_issued(const Value& body) {
    const Value* room_v = body.find("room_id");
    const std::string* room = room_v ? room_v->as_string() : nullptr;
    const auto id = get_peer_id(body.find("peer_id"));
    const Value* token_v = body.find("peer_token");
    const std::string* token_s = token_v ? token_v->as_string() : nullptr;
    const auto vip = get_ipv4(body.find("virtual_ip"));
    const auto expires = get_nonnegative(body.find("expires_in_s"));
    if (room == nullptr || !is_room_id_exact(*room) || !id || token_s == nullptr || !vip ||
        !expires) {
        return std::nullopt;
    }
    auto token = PeerToken::parse(*token_s);
    if (!token) {
        return std::nullopt;
    }
    return Issued{*room, *id, std::move(*token), *vip, *expires};
}

std::optional<Registered> decode_registered(const Value& body) {
    const auto accepted =
        get_uint32(body.find("accepted"), 1, static_cast<std::uint32_t>(protocol::kMaxCandidates));
    const auto rejected =
        get_uint32(body.find("rejected"), 0, static_cast<std::uint32_t>(protocol::kMaxCandidates));
    if (!accepted || !rejected || *accepted + *rejected > protocol::kMaxCandidates) {
        return std::nullopt;
    }
    return Registered{*accepted, *rejected};
}

// protocol.md 10.1 위생 거부. 4.4 의 서버 판정과 같은 대역이다.
bool rejected_by_hygiene(const Candidate& c) noexcept {
    const std::uint32_t a = c.endpoint.address();
    if (c.endpoint.port() == 0) {
        return true;
    }
    if (a == 0xFFFFFFFFu) {
        return true;  // 브로드캐스트. 255.255.255.255 하나
    }
    if (a == 0x00000000u) {
        return true;  // 미지정. 0.0.0.0 하나
    }
    if ((a >> 28) == 0xEu) {
        return true;  // 멀티캐스트 224.0.0.0/4
    }
    if ((a >> 24) == 127u) {
        return true;  // 루프백 127.0.0.0/8
    }
    return false;
}

// 성공 봉투 뒤의 공통 절차. decode 가 nullopt 면 전송 오류다.
template <class T, class Decode>
Reply<T> interpret(std::string_view raw, Decode decode) {
    Reply<T> out;
    const http::Response response = http::parse_response(raw);
    if (!response.ok()) {
        return out;  // 전송 오류
    }
    const Envelope env = classify_envelope(*response.body);
    out.outcome = env.outcome;
    out.failure = env.failure;
    if (env.outcome != Outcome::kSuccess) {
        return out;
    }
    auto value = decode(*response.body);
    if (!value) {
        out.outcome = Outcome::kTransient;
        out.failure = Failure::kTransport;
        return out;
    }
    out.value = std::move(value);
    return out;
}

template <class T>
Reply<T> self_in_peers() {
    Reply<T> out;
    out.outcome = Outcome::kDefinite;
    out.failure = Failure::kSelfInPeers;
    return out;
}

}  // namespace

Reply<Issued> interpret_create_room(std::string_view raw) {
    return interpret<Issued>(raw, decode_issued);
}

Reply<Issued> interpret_join_room(std::string_view raw) {
    return interpret<Issued>(raw, decode_issued);
}

Reply<Registered> interpret_register_candidate(std::string_view raw) {
    return interpret<Registered>(raw, decode_registered);
}

Reply<Peers> interpret_get_peers(std::string_view raw, std::uint32_t self_peer_id) {
    auto reply = interpret<Peers>(raw, [](const Value& body) -> std::optional<Peers> {
        const Value* ready_v = body.find("ready");
        const auto ready = ready_v ? ready_v->as_bool() : std::nullopt;
        auto peers = decode_peers(body.find("peers"));
        if (!ready || !peers) {
            return std::nullopt;
        }
        bool any_ready = false;
        for (const auto& p : *peers) {
            any_ready = any_ready || p.ready;
        }
        if (*ready != any_ready) {
            return std::nullopt;  // 4.5 ready 의 정의
        }
        return Peers{*ready, std::move(*peers)};
    });
    if (reply.value && contains_peer(reply.value->peers, self_peer_id)) {
        return self_in_peers<Peers>();
    }
    return reply;
}

Reply<HostReport> interpret_host_report(std::string_view raw, std::uint32_t self_peer_id) {
    auto reply = interpret<HostReport>(raw, [](const Value& body) -> std::optional<HostReport> {
        const auto expires = get_nonnegative(body.find("expires_in_s"));
        auto released = decode_id_list(body.find("released"));
        auto confirmed = decode_id_list(body.find("confirmed"));
        auto peers = decode_peers(body.find("peers"));
        if (!expires || !released || !confirmed || !peers) {
            return std::nullopt;
        }
        return HostReport{*expires, std::move(*released), std::move(*confirmed),
                          std::move(*peers)};
    });
    if (reply.value && contains_peer(reply.value->peers, self_peer_id)) {
        return self_in_peers<HostReport>();
    }
    return reply;
}

// ---------------------------------------------------------------- 위생 (protocol.md 10.1)

std::vector<Candidate> sanitize_received_candidates(std::span<const Candidate> received) {
    std::vector<Candidate> out;
    // 순서: 위생 거부 -> 중복 제거 -> 앞에서부터 MAX_CANDIDATES 개.
    for (const auto& c : received) {
        if (rejected_by_hygiene(c)) {
            continue;
        }
        bool duplicate = false;
        for (const auto& kept : out) {
            if (kept.endpoint == c.endpoint) {
                duplicate = true;  // 먼저 온 것을 남긴다. kind 도 그 원소의 것이다
                break;
            }
        }
        if (!duplicate) {
            out.push_back(c);
        }
    }
    if (out.size() > protocol::kMaxCandidates) {
        out.resize(protocol::kMaxCandidates);
    }
    return out;
}

bool is_punch_ready(const PeerView& peer) noexcept {
    return peer.ready && !peer.candidates.empty();
}

// ---------------------------------------------------------------- host_report 의 결과 (4.6)

HostReportAction host_report_action(Outcome outcome, Failure failure) noexcept {
    // 허용 목록. 다음 주기로 가는 것을 나열하고 나머지 확정 오류는 멈춘다.
    if (outcome == Outcome::kSuccess || outcome == Outcome::kTransient) {
        return HostReportAction::kNextCycle;
    }
    if (failure == Failure::kRateLimited) {
        return HostReportAction::kNextCycle;  // 4.6 둘째 행
    }
    return HostReportAction::kStop;
}

// ---------------------------------------------------------------- 로그 필드

std::vector<LogField> control_result_fields(http::Op op, Outcome outcome, Failure failure) {
    std::vector<LogField> fields;
    fields.push_back(field("op", http::op_name(op)));
    fields.push_back(field("ok", std::string_view(outcome == Outcome::kSuccess ? "true" : "false")));
    fields.push_back(field("error", to_token(failure)));
    return fields;
}

std::vector<LogField> control_peers_fields(const PeerView& peer) {
    std::string candidates;
    for (const auto& c : sanitize_received_candidates(peer.candidates)) {
        if (!candidates.empty()) {
            candidates.push_back(',');
        }
        candidates.append(c.endpoint.to_string());
    }
    std::vector<LogField> fields;
    fields.push_back(field("peer_id", std::uint64_t{peer.peer_id}));
    fields.push_back(field("virtual_ip", network::format_ipv4(peer.virtual_ip)));
    fields.push_back(field("candidates", candidates));
    return fields;
}

}  // namespace sangtachi::control
