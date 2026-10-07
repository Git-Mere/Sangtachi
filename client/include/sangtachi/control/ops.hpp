#pragma once

// 제어 평면 연산 다섯의 본문과 결과 분류 (control_plane.md 4장 연산, 8.3 오류 분류와 재시도).
//
// **순수 코드다.** 입출력이 없다. 요청 본문을 만들고, 응답 바이트를 판정해 8.3 의 결과 셋 중
// 하나로 돌려준다. 재시도 판단은 [loop] 의 일이다 (8.3 "재시도를 판단하는 것은 [loop] 다").
//
// ## 결과 분류 (8.3)
//
// | 받은 것 | 결과 | Failure |
// |---------|------|---------|
// | 3.5 의 1~5 에 걸린 응답, 받지 못한 응답 | 일시 오류 | `kTransport` |
// | `ok` 가 없거나 불이 아니다 | 일시 오류 | `kTransport` |
// | `ok: false` 인데 `error` 가 없거나 문자열이 아니다 | 일시 오류 | `kTransport` |
// | `ok: false`, `error` 가 `internal` 또는 `unavailable` | 일시 오류 | 그 코드 |
// | `ok: false`, `error` 가 4.1 표의 그 밖의 코드 | 확정 오류 | 그 코드 |
// | `ok: false`, `error` 가 4.1 표에 없는 문자열 (3.5 의 8) | 확정 오류 | `kUnknownCode` |
// | `ok: true` 이고 연산별 필드가 아래 형 규칙을 지킨다 | 성공 | `kNone` |
// | `ok: true` 인데 연산별 필드가 형 규칙을 어긴다 (3.5 의 7) | 일시 오류 | `kTransport` |
// | `ok: true` 이고 `peers` 에 자신의 `peer_id` 가 있다 (4.5, 8.4 의 8번) | 확정 오류 | `kSelfInPeers` |
//
// 4.1 표에 없는 코드가 확정 오류이고 형이 틀린 성공이 전송 오류인 것은 3.5 의 7, 8 이 정했다.
// 형이 틀린 성공에서 값을 짐작해 채우지 않는다. 로그 토큰 `unknown_code`, `self_in_peers` 는
// architecture.md 9장의 `error` 행이 정했다.
//
// **`message` 는 보지 않는다.** 사람용이고 내용을 못박지 않는다 (4.1). 로그에도 싣지 않는다.
//
// ## 응답 필드의 형 규칙
//
// 알 수 없는 키는 무시한다 (3.3 전방 호환). 아는 키는 아래를 지켜야 한다.
//
// | 필드 | 규칙 | 출처 |
// |------|------|------|
// | `room_id` | 6자, `ABCDEFGHJKLMNPQRSTUVWXYZ23456789` 의 글자만. 소문자 거부 | 2.1. 응답은 정규화된 값이다 (4.3) |
// | `peer_id`, `released`·`confirmed` 의 원소 | JSON 정수, 1 이상 4294967295 이하 | 3.3 uint32, 2.2 "0 제외" |
// | `peer_token` | 소문자 16진수 32자 | 2.3 |
// | `virtual_ip` | 점 십진 IPv4. network::parse_ipv4 의 규칙(inet_aton 류의 관대함 없음) | 4.2, 4.4 |
// | `expires_in_s` | JSON 정수, 0 이상 | 4.2, 5.1 |
// | `accepted` | JSON 정수, 1 이상 `MAX_CANDIDATES` 이하 | 4.4. 0개를 저장하면 `bad_request` 다 |
// | `rejected` | JSON 정수, 0 이상이고 `accepted + rejected <= MAX_CANDIDATES` | 4.4. 보낸 것이 8개 이하이고 중복 제거는 둘 다 줄인다 |
// | `ready` | JSON 불 | 4.5 |
// | `peers` | 배열, 길이 `MAX_PEERS - 1` 이하, `peer_id` 중복 없음 | 4.5, 2.2 방 안 유일 |
// | `peers` 원소의 필드 집합 | `ready` 가 거짓이면 `punch_delay_ms`·`elapsed_since_ready_ms`·`candidates` 가 없어야 하고, 참이면 셋 다 있어야 한다 | 4.5 "원소의 필드 집합은 `ready` 가 가른다" |
// | `get_peers` 의 `ready` | `peers` 에 `ready` 인 원소가 하나라도 있는 것과 같아야 한다 | 4.5 |
// | `punch_delay_ms` | JSON 정수, 0 이상 4294967295 이하 | 4.5 |
// | `elapsed_since_ready_ms` | JSON 정수, 0 이상 | 4.5 |
// | `candidates` | 배열. 형 검사에서는 길이를 보지 않는다. 빈 배열도 받는다 | 아래, protocol.md 10.1 |
// | 후보 원소 | `ip` 점 십진 IPv4, `port` JSON 정수 0~65535, `kind` 가 `"local"` 또는 `"reflexive"` | 4.4 형 위반 행 |
// | `released`, `confirmed` | 배열, 길이 `MAX_PEERS - 1` 이하 | 4.6 |
//
// **받은 후보 목록은 형 검사 뒤에 위생을 거친다.** protocol.md 10.1 후보 수집과 위생의 "수신한
// 목록은 이 순서로 다룬다" 그대로다. 형 검사(위 표, 하나라도 틀리면 응답 전체가 전송 오류),
// 위생 거부, 중복 제거, 앞에서부터 MAX_CANDIDATES 개. 위생은 sanitize_received_candidates 가 하고
// interpret_get_peers 와 interpret_host_report 가 상대 후보마다 부른다. 그래서 그 둘이 돌려주는
// `PeerView::candidates` 는 늘 위생 뒤의 목록이다. 포트 0 은 형이 아니라 위생에서 걸러진다(4.4 의
// 거부 정책). 위생 뒤 목록이 비면 그 쌍은 준비 완료가 아니다(8.4). 판정은 is_punch_ready 다.
//
// **`host_report` 응답에는 최상위 `ready` 가 없다.** 4.6 응답 표에 없다. 있어도 알 수 없는 키로
// 무시한다.
//
// ## 비밀
//
// `peer_token` 과 `client_nonce` 는 프로세스 밖으로 나가지 않는다 (architecture.md 3.5,
// control_plane.md 1.2). 그래서 둘은 문자열이 아니라 아래 클래스로 들고, 값을 꺼내는 함수는
// 이름이 그것을 말한다. 로그 필드를 만드는 함수(control_result_fields, control_peers_fields)는
// 이 둘과 `room_id` 를 받지 않는다. 받지 않으면 실을 수 없다.

#include "sangtachi/control/http.hpp"
#include "sangtachi/control/json.hpp"
#include "sangtachi/log.hpp"
#include "sangtachi/network/endpoint.hpp"

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

// 2.3 peer_token. 소문자 16진수 32자.
class PeerToken {
public:
    [[nodiscard]] static std::optional<PeerToken> parse(std::string_view text);

    // 요청 본문에 넣을 때만 부른다. 로그, 표준 출력, 파일에 쓰지 않는다.
    [[nodiscard]] std::string_view reveal_for_request() const noexcept { return hex_; }

    friend bool operator==(const PeerToken&, const PeerToken&) = default;

private:
    explicit PeerToken(std::string hex) : hex_(std::move(hex)) {}
    std::string hex_;
};

// 2.4 client_nonce. 128비트 CSPRNG, 소문자 16진수 32자. room_id 와 같은 등급의 비밀이다 (1.2).
class ClientNonce {
public:
    inline static constexpr std::size_t kBytes = 16;

    [[nodiscard]] static ClientNonce from_bytes(std::span<const std::byte, kBytes> bytes);

    [[nodiscard]] std::string_view reveal_for_request() const noexcept { return hex_; }

    friend bool operator==(const ClientNonce&, const ClientNonce&) = default;

private:
    explicit ClientNonce(std::string hex) : hex_(std::move(hex)) {}
    std::string hex_;
};

// CSPRNG 로 새 값을 뽑는다 (platform::random_bytes). 실패하면 nullopt 다. 0 이나 고정값으로
// 대신하지 않는다. 한 참가에 한 값이고 재시도마다 바꾸지 않는다 (2.4). 그 수명은 부르는 쪽이 갖는다.
[[nodiscard]] std::optional<ClientNonce> generate_client_nonce();

// 4.4 후보 원소의 kind.
enum class CandidateKind : std::uint8_t {
    kLocal,
    kReflexive,
};

[[nodiscard]] std::string_view to_token(CandidateKind kind) noexcept;

struct Candidate {
    network::Endpoint endpoint;
    CandidateKind kind = CandidateKind::kLocal;

    friend bool operator==(const Candidate&, const Candidate&) = default;
};

// ---------------------------------------------------------------- 요청 본문 (4장 요청 표)
//
// JSON 텍스트를 돌려준다. 거부하면 nullopt 다. 거부하는 것은 서버가 형 위반으로 `bad_request`
// 를 돌려줄 것이 확실한 입력이다. 보내 봐야 확정 오류이고 그 사이 속도 제한 예산만 쓴다.
//
// - room_id 는 2.1 의 형식이어야 한다. 대소문자는 가리지 않는다 (서버가 정규화한다)
// - candidates 는 1개 이상 MAX_CANDIDATES(8) 이하. 넘으면 부르는 쪽이 먼저 자른다 (8.4 의 6번)
// - departed, confirm 은 MAX_PEERS - 1 이하
//
// 후보의 값(포트 0, 루프백)은 보지 않는다. 그것은 서버의 위생 거부이고 형 위반이 아니다 (4.4).

[[nodiscard]] std::optional<std::string> create_room_body(const ClientNonce& nonce);
[[nodiscard]] std::optional<std::string> join_room_body(std::string_view room_id,
                                                        const ClientNonce& nonce);
[[nodiscard]] std::optional<std::string> register_candidate_body(
    std::string_view room_id, std::uint32_t peer_id, const PeerToken& token,
    std::span<const Candidate> candidates);
[[nodiscard]] std::optional<std::string> get_peers_body(std::string_view room_id,
                                                        std::uint32_t peer_id,
                                                        const PeerToken& token);
[[nodiscard]] std::optional<std::string> host_report_body(
    std::string_view room_id, std::uint32_t peer_id, const PeerToken& token,
    std::span<const std::uint32_t> departed, std::span<const std::uint32_t> confirm);

// 2.1 room_id 형식. 대소문자를 가리지 않는다. 요청에 넣기 전 검사에 쓴다.
[[nodiscard]] bool is_room_id_input(std::string_view text) noexcept;

// ---------------------------------------------------------------- 결과 분류 (8.3)

enum class Outcome : std::uint8_t {
    kSuccess,
    kDefinite,   // 확정 오류. 재시도하지 않는다
    kTransient,  // 일시 오류. 재시도 여부는 연산마다 다르다 (8.3 표)
};

enum class Failure : std::uint8_t {
    kNone = 0,
    kTransport,          // 3.5 전송 오류. 받지 못했거나, 판정에 걸렸거나, 형이 틀렸다
    // 4.1 공통 봉투의 오류 코드
    kBadRequest,
    kMethodNotAllowed,
    kUnknownOp,
    kLengthRequired,
    kTooLarge,
    kRoomNotFound,
    kRoomExpired,
    kRoomFull,
    kUnauthorized,
    kRateLimited,
    kInternal,
    kUnavailable,
    // 4.1 표에 없는 코드
    kUnknownCode,
    // 4.5 응답에 자신의 peer_id 가 있다. 서버 결함이고 CONTROL_PLANE_EXCHANGE_FAILED 로 끝낸다
    kSelfInPeers,
};

// architecture.md 9장 control.result 의 `error` 값. 성공은 `-`, 전송 오류는 `transport`,
// 4.1 의 코드는 그 문자열 그대로, 4.1 표에 없는 코드는 `unknown_code`, 자신의 peer_id 가 든 응답은
// `self_in_peers` 다. 서버가 보낸 모르는 코드 문자열을 그대로 싣지 않는다 (같은 행).
[[nodiscard]] std::string_view to_token(Failure failure) noexcept;

template <class T>
struct Reply {
    Outcome outcome = Outcome::kTransient;
    Failure failure = Failure::kTransport;
    std::optional<T> value;  // 성공일 때만 있다
};

// 받지 못한 응답(connect 실패, 시간 초과, 상대가 먼저 닫음)을 나타내는 값. I/O 쪽이 쓴다.
template <class T>
[[nodiscard]] Reply<T> transport_failure() {
    return Reply<T>{};
}

// 4.1 공통 봉투만 본다. 성공이면 kSuccess/kNone 이고 연산별 필드는 보지 않는다.
struct Envelope {
    Outcome outcome = Outcome::kTransient;
    Failure failure = Failure::kTransport;
};
[[nodiscard]] Envelope classify_envelope(const json::Value& body);

// ---------------------------------------------------------------- 응답 (4장 응답 표)

// 4.2, 4.3. create_room 과 join_room 의 응답은 같은 모양이다.
struct Issued {
    std::string room_id;  // 표준 출력에만 낸다. 로그에 싣지 않는다 (architecture.md 3.5)
    std::uint32_t peer_id = 0;
    PeerToken peer_token;
    std::uint32_t virtual_ip = 0;  // 호스트 바이트 순서
    std::int64_t expires_in_s = 0;
};

// 4.4.
struct Registered {
    std::uint32_t accepted = 0;
    std::uint32_t rejected = 0;
};

// 4.5 peers 원소.
struct PeerView {
    std::uint32_t peer_id = 0;
    std::uint32_t virtual_ip = 0;
    bool ready = false;
    // 아래 셋은 ready 일 때만 뜻이 있다.
    std::uint32_t punch_delay_ms = 0;
    std::int64_t elapsed_since_ready_ms = 0;
    std::vector<Candidate> candidates;  // protocol.md 10.1 위생 뒤. 최대 MAX_CANDIDATES 개
};

// 4.5.
struct Peers {
    bool ready = false;
    std::vector<PeerView> peers;
};

// 4.6.
struct HostReport {
    std::int64_t expires_in_s = 0;
    std::vector<std::uint32_t> released;
    std::vector<std::uint32_t> confirmed;
    std::vector<PeerView> peers;
};

// protocol.md 10.1 의 수신 목록 처리에서 형 검사 뒤의 셋. 위생 거부, 중복 제거, 앞에서부터
// MAX_CANDIDATES 개. 이 순서를 바꾸지 않는다 (10.1 "왜 상한이 마지막인가").
//
// | 거부 | 판정 |
// |------|------|
// | 브로드캐스트 | `255.255.255.255` 하나. 서브넷 directed broadcast 는 판정하지 않는다 |
// | 멀티캐스트 | `224.0.0.0/4` |
// | 미지정 | `0.0.0.0` 하나 |
// | 루프백 | `127.0.0.0/8` |
// | 포트 0 | |
//
// 중복은 `ip:port` 가 같은 것이고 먼저 온 원소(그 kind 포함)를 남긴다. 4.4 의 서버 판정과 같은 대역이다.
// `0.0.0.0/8` 의 나머지와 `240.0.0.0/4` 는 목록에 없으므로 남긴다.
[[nodiscard]] std::vector<Candidate> sanitize_received_candidates(std::span<const Candidate> received);

// 8.4 의 7번과 8번. 그 쌍이 `ready` 이고 위생 뒤 상대 후보가 하나 이상인가. 펀치에 들어가거나
// control.peers 를 낼 수 있는 상대인지의 판정이다. 거짓이면 플레이어는 폴링을 계속한다.
[[nodiscard]] bool is_punch_ready(const PeerView& peer) noexcept;

// 받은 응답 바이트 전체를 판정한다. http::parse_response, 봉투, 연산별 필드 순서다.
// self_peer_id 는 4.5 의 "자신을 제외" 검사에 쓴다.
[[nodiscard]] Reply<Issued> interpret_create_room(std::string_view raw);
[[nodiscard]] Reply<Issued> interpret_join_room(std::string_view raw);
[[nodiscard]] Reply<Registered> interpret_register_candidate(std::string_view raw);
[[nodiscard]] Reply<Peers> interpret_get_peers(std::string_view raw, std::uint32_t self_peer_id);
[[nodiscard]] Reply<HostReport> interpret_host_report(std::string_view raw,
                                                      std::uint32_t self_peer_id);

// ---------------------------------------------------------------- host_report 의 결과 (4.6)

// 4.6 "호스트가 오류를 받으면 셋으로 나눠 다룬다" 의 행동. 8.3 의 일반 규칙과 다르다.
enum class HostReportAction : std::uint8_t {
    kNextCycle,  // 다음 주기에 다시 부른다. 성공도 여기다 (주기는 그대로 돈다)
    kStop,       // `FAIL CONTROL_PLANE_EXCHANGE_FAILED` 한 번, host_report 를 멈춘다
};

// | 받은 것 | 행동 |
// |---------|------|
// | 성공 | kNextCycle |
// | 확정 오류 가운데 `rate_limited` | kNextCycle |
// | 일시 오류 (`internal`, `unavailable`, 전송 오류, 형이 틀린 성공) | kNextCycle |
// | 그 밖의 확정 오류 (`room_expired`, `room_not_found`, `unauthorized`, `bad_request`, 4.1 의 나머지, `unknown_code`, `self_in_peers`) | kStop |
//
// 허용 목록이다. kStop 이 아닌 쪽을 나열하고 나머지 확정 오류는 전부 멈춘다. 확정 오류는 다시
// 보내도 같은 답이고, 계속 부르면 속도 제한 예산을 깎는다 (4.6 "왜 첫 행에서 멈추나").
[[nodiscard]] HostReportAction host_report_action(Outcome outcome, Failure failure) noexcept;

[[nodiscard]] inline HostReportAction host_report_action(const Reply<HostReport>& reply) noexcept {
    return host_report_action(reply.outcome, reply.failure);
}

// ---------------------------------------------------------------- 로그 필드 (architecture.md 9장)

// control.result 의 op, ok, error. room_id 와 peer_token 을 받지 않는다.
[[nodiscard]] std::vector<LogField> control_result_fields(http::Op op, Outcome outcome,
                                                          Failure failure);

// control.peers 의 peer_id, virtual_ip, candidates (`ip:port` 를 쉼표로 이은 것. 공백 없음).
// 피어 하나에 한 줄이다. 후보는 위생 뒤의 목록이다 (8.4 "control.peers 의 필드"). PeerView 는
// interpret_* 가 위생을 마친 것만 만들지만, 손으로 만든 PeerView 도 있을 수 있어 이 함수가
// sanitize_received_candidates 를 한 번 더 건다(같은 목록이면 결과가 같다). 부르는 쪽은
// is_punch_ready 가 참인 상대에만 낸다.
[[nodiscard]] std::vector<LogField> control_peers_fields(const PeerView& peer);

}  // namespace sangtachi::control
