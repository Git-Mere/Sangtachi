#pragma once

// 로비와 시도의 상태 기계 (concurrency.md 7장 로비, 8장 [control] 스레드,
// architecture.md 3.5 기동 입력, control_plane.md 8.3 오류 분류와 재시도, 8.4 기동부터 펀치까지).
//
// **순수 코드다.** 스레드도 소켓도 시계도 OS 호출도 없다. 사건(명령, 제어 응답, STUN 결과,
// 타이머 만료)을 받아 할 일(LobbyAction)의 목록을 돌려준다. 할 일을 실제로 하는 것은 [loop] 다.
// 그래서 시험이 사건을 차례로 넣고 돌아온 목록만 보고 판정한다.
//
// ## 통합 계약 ([loop] 이 지킬 것)
//
// | 행동 | [loop] 이 하는 일 |
// |------|------------------|
// | `SubmitRequest` | 요청 큐에 넣는다. 큐가 차서 넣지 못하면 control_queue_dropped 를 올리고 곧바로 `on_submit_dropped()` 를 부른다 (concurrency.md 8장의 "가득 찼을 때") |
// | `StartStun` | **새** StunClient 를 만들어 start 한다. StunClient::start 는 끝난 뒤 다시 부르면 아무것도 하지 않으므로 시도마다 새 객체다 (8.4 "5번 STUN 도 다시 돈다"). 끝나면 `on_stun_done(phase == Succeeded, mappings())` |
// | `CancelStun` | 그 StunClient 의 타이머(stun.retry.N, stun.deadline.N)를 지우고 객체를 버린다. StunClient 는 소멸자에서 타이머를 지우지 않는다 |
// | `ArmTimer` | `TimerSet::add_once(name, delay_ms, now)`. 만료하면 `on_timer(name)` |
// | `CancelTimer` | `TimerSet::cancel(name)` |
// | `PrintLine` | 표준 출력에 그 줄과 줄바꿈 (architecture.md 3.5 의 `ROOM`, `FAIL`) |
// | `LogLine` | `emit(level, event, fields)` (architecture.md 9장 로그 출력) |
//
// - 행동은 **돌려준 순서대로** 한다
// - **`SubmitRequest` 는 한 목록에 많아야 하나이고 언제나 마지막이다.** 그래서 큐가 차서
//   on_submit_dropped 를 부를 때 같은 목록에 남은 행동이 없다
// - 응답은 [control] 이 SubmitRequest 의 op 와 self_peer_id 로 interpret_* 를 불러 만든 값을
//   그대로 `on_reply` 에 넣는다. 미결 요청은 늘 하나이므로 응답도 늘 그 하나의 것이다
//   (concurrency.md 8장 "미결 요청 | 한 번에 하나")
// - 이 객체의 타이머 이름은 `owns_timer` 가 참인 넷뿐이다. 그 밖의 이름은 넘겨도 무시한다
//
// ## 비밀
//
// `peer_token` 과 `client_nonce` 는 SubmitRequest::body 밖으로 나가지 않는다. `room_id` 는
// 그 밖에 `ROOM` 줄 하나에만 실린다. LogLine 은 셋 중 어느 것도 싣지 않는다
// (architecture.md 3.5 "peer_token 은 프로세스 밖으로 나가지 않는다", "방 코드도 표준 출력이다").
// 형식이 틀린 `join` 의 WARN 에도 친 문자열을 싣지 않는다. 한 글자 틀린 방 코드일 수 있다.
//
// ## 상태
//
// | 단계 | 뜻 | 로비인가 |
// |------|-----|:---:|
// | kLobby | 방에 속하지 않았다 (concurrency.md 7장 로비) | 예 |
// | kIssuing | create_room 또는 join_room 이 미결이거나 재시도를 기다린다 (8.4 의 4번) | 아니오 |
// | kStun | STUN 이 돈다 (8.4 의 5번) | 아니오 |
// | kRegistering | register_candidate 가 미결이거나 재시도·차례를 기다린다 (8.4 의 6번) | 아니오 |
// | kPolling | 플레이어가 get_peers 를 폴링한다 (8.4 의 7번) | 아니오 |
// | kSettled | 플레이어는 상대를 얻고 폴링을 멈췄다. 호스트는 방을 세웠다 (register_candidate 성공) | 아니오 |
//
// 호스트의 host_report 주기는 단계와 따로 돈다. create_room 성공에서 시작하고(protocol.md
// 11장 타이머, 8.4 "호스트는 4번 직후부터") 시도가 끝날 때 멈춘다.
//
// **미결 요청은 시도와 따로 산다.** 시도가 끝나도(`leave`, `FAIL`) 미결 요청은 끝까지 가고, 그
// 응답은 반영하지 않고 버린다. 그 응답이 오기 전에는 host 와 join 을 받지 않는다
// (concurrency.md 8장 "시도가 끝나도 미결 요청은 끝까지 간다").
//
// ## 경계에서의 거동
//
// | 자리 | 거동 | 출처 |
// |------|------|------|
// | client_nonce 를 뽑지 못함 (CSPRNG 실패) | 요청을 보내지 않고 `FAIL CONTROL_PLANE_EXCHANGE_FAILED` 뒤 로비. 예측 가능한 값으로 대신하지 않는다 | control_plane.md 2.4 |
// | 요청 본문 생성기가 거부함 | 같다. 입력을 먼저 검사하므로 정상 경로에서는 생기지 않는다 | 이 코드 |
// | 미결 중에 host_report 주기나 register_candidate 재시도 차례가 옴 | 미결 응답 직후에 보낸다. 건너뛰는 것은 get_peers 폴링뿐이다. 재시도 횟수는 실제로 보낸 횟수다. host_report 주기는 그 송신에서 다시 센다 | concurrency.md 8장 |
// | 호스트의 confirm 대상 | 직전 host_report 응답에서 `ready: false` 인 상대 전부. 양쪽 후보는 서버가 확인한다 | control_plane.md 8.4 의 7번 |
// | 플레이어의 get_peers 성공 응답에 상대가 정확히 하나가 아님 | 4.5 응답 표 위반이라 형이 틀린 성공이고 전송 오류다(3.5 의 7번). `control.result error=transport` 를 내고 다음 폴링이 재시도다. 마감에 닿으면 폴링 마감 초과로 CPEF | control_plane.md 3.5, 4.5, 8.3 |
// | 요청 큐가 가득 차 버린 요청 | 시도를 `CONTROL_PLANE_EXCHANGE_FAILED` 로 끝낸다. 방을 세운 호스트의 host_report 면 4.6 첫 행처럼 다룬다. Phase 3 에는 세션이 없으므로 로비로 간다 | concurrency.md 8장 |
// | 응답의 op 가 미결 요청과 다름 | 통합 결함이다. 그 응답을 미결 요청의 것으로 소비하고 시도를 `CONTROL_PLANE_EXCHANGE_FAILED` 로 끝낸다 | 이 코드 |
// | 성공한 STUN 이 관측을 하나도 주지 않음 | `STUN_DISCOVERY_FAILED` | 이 코드 |
// | register_candidate 의 후보 | 반사 후보뿐이다 | control_plane.md 8.4 의 Phase 3 범위 |
// | 명령 뒤에 남은 낱말, 대소문자 | 형식이 틀린 것이다. 명령은 대소문자를 가린다 | architecture.md 3.5 |
//
// ## 로그
//
// | 이벤트 | 수준 | 필드 | 언제 |
// |--------|------|------|------|
// | `control.result` | 성공 INFO, 그 밖 WARN | op, ok, error (architecture.md 9장) | 반영하는 응답마다 |
// | `control.discarded` | INFO | op | 끝난 시도의 늦은 응답을 버릴 때 |
// | `control.peers` | INFO | peer_id, virtual_ip, candidates (architecture.md 9장) | 8.4 의 Phase 3 규칙 |
// | `control.candidates_rejected` | WARN | rejected | register_candidate 응답의 rejected 가 0 이 아님 (4.4) |
// | `session.failed` | ERROR | code | `FAIL CONTROL_PLANE_EXCHANGE_FAILED` 를 낼 때. STUN 실패의 줄은 StunClient 가 이미 낸다 |
// | `console.rejected` | WARN | reason, command | 받지 않는 명령 (architecture.md 3.5 로비 명령) |
// | `attempt.start` | INFO | role | host 나 join 을 받았다 |
// | `attempt.end` | INFO | reason (`leave`, `failed`) | 로비로 간다 |
// | `control.reply_mismatch`, `control.unexpected_reply` | WARN | op | 통합 결함 |

#include "sangtachi/args.hpp"
#include "sangtachi/control/constants.hpp"
#include "sangtachi/control/http.hpp"
#include "sangtachi/control/ops.hpp"
#include "sangtachi/log.hpp"
#include "sangtachi/network/stun_client.hpp"
#include "sangtachi/timer.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace sangtachi::control {

// ---------------------------------------------------------------- 값

// 재시도, 폴링, host_report 간격의 값은 control/constants.hpp 가 갖는다.

// 타이머 이름. 문서에 없다. timer.hpp 의 이름 유일성 때문에 접두를 붙였다.
inline constexpr std::string_view kRetryTimer = "control.retry";
inline constexpr std::string_view kPollTimer = "control.poll";
inline constexpr std::string_view kDeadlineTimer = "control.deadline";
inline constexpr std::string_view kHostReportTimer = "control.host_report";

// ---------------------------------------------------------------- 표준 출력 줄 (architecture.md 3.5)

// architecture.md 8장 실패 진단의 코드.
enum class FailCode : std::uint8_t {
    kStunDiscoveryFailed,
    kControlPlaneExchangeFailed,
    kHolePunchTimeout,
    kPeerHandshakeFailed,
    kTunnelDropped,
};

// 8장의 코드 문자열 그대로.
[[nodiscard]] std::string_view to_token(FailCode code) noexcept;
// 8장 문장 표의 문장 그대로. 문장의 출처는 그 표다.
[[nodiscard]] std::string_view fail_sentence(FailCode code) noexcept;
// `FAIL <코드> <문장>`. 줄바꿈은 붙이지 않는다.
[[nodiscard]] std::string fail_line(FailCode code);
// `ROOM <room_id>`. 줄바꿈은 붙이지 않는다.
[[nodiscard]] std::string room_line(std::string_view room_id);

// ---------------------------------------------------------------- 로비 명령 (architecture.md 3.5)

enum class LobbyCommandKind : std::uint8_t {
    kHost,
    kJoin,
    kLeave,
};

struct LobbyCommand {
    LobbyCommandKind kind = LobbyCommandKind::kHost;
    std::string argument;  // join 의 방 코드. 친 그대로. 없으면 빈 문자열
    bool extra = false;    // 받을 인자 뒤에 낱말이 더 있었다
};

// 콘솔 줄 하나. 첫 낱말이 정확히 `host`, `join`, `leave` 일 때만 값이 있다. 그 밖은 nullopt 이고
// 다른 콘솔 명령(quit, counters, raw)의 몫이다. 낱말은 ASCII 공백과 탭으로 가른다.
// 인자의 형식은 여기서 보지 않는다. Lobby::on_command 가 보고 WARN 을 낸다.
[[nodiscard]] std::optional<LobbyCommand> parse_lobby_command(std::string_view line);

// CLI 역할은 기동 직후의 로비 명령이다 (architecture.md 3.5). Role::None 이면 nullopt 이고 아무
// 요청도 나가지 않는다. Player 의 room 이 없으면 빈 인자의 join 이 되어 on_command 가 WARN 한다.
[[nodiscard]] std::optional<LobbyCommand> startup_command(Role role,
                                                          const std::optional<std::string>& room);

// ---------------------------------------------------------------- 행동

// 요청 하나. body 에 peer_token 과 client_nonce 가 들어 있다. 요청 큐 밖으로 내지 않는다.
struct SubmitRequest {
    http::Op op = http::Op::kCreateRoom;
    std::string body;
    std::uint32_t self_peer_id = 0;  // interpret_get_peers, interpret_host_report 의 인자
};

struct StartStun {};
struct CancelStun {};

struct ArmTimer {
    std::string_view name;  // 위 네 상수 가운데 하나. 수명은 프로그램 전체다
    Millis delay_ms = 0;
};

struct CancelTimer {
    std::string_view name;
};

struct PrintLine {
    std::string text;  // 줄바꿈 없음
};

struct LogLine {
    LogLevel level = LogLevel::Info;
    std::string_view event;  // 문자열 상수
    std::vector<LogField> fields;
};

using LobbyAction =
    std::variant<SubmitRequest, StartStun, CancelStun, ArmTimer, CancelTimer, PrintLine, LogLine>;
using LobbyActions = std::vector<LobbyAction>;

// ---------------------------------------------------------------- 사건

// [control] 이 돌려준 응답. op 는 그 요청의 op 이고 reply 의 형은 op 가 정한다.
// create_room, join_room 은 Reply<Issued>, register_candidate 는 Reply<Registered>,
// get_peers 는 Reply<Peers>, host_report 는 Reply<HostReport> 다.
struct ControlReply {
    http::Op op = http::Op::kCreateRoom;
    std::variant<Reply<Issued>, Reply<Registered>, Reply<Peers>, Reply<HostReport>> reply;
};

// 시도마다 새 client_nonce 를 하나 준다. 제품 경로는 generate_client_nonce 다. 실패하면 nullopt.
using NonceSource = std::function<std::optional<ClientNonce>()>;

enum class LobbyStage : std::uint8_t {
    kLobby,
    kIssuing,
    kStun,
    kRegistering,
    kPolling,
    kSettled,
};

enum class LobbyRole : std::uint8_t {
    kNone,
    kHost,
    kPlayer,
};

class Lobby {
public:
    explicit Lobby(NonceSource nonce);

    // 로비 명령 하나 (architecture.md 3.5 로비 명령 표).
    [[nodiscard]] LobbyActions on_command(const LobbyCommand& command);

    // 미결 요청의 응답.
    [[nodiscard]] LobbyActions on_reply(const ControlReply& reply);

    // 방금 돌려준 SubmitRequest 를 요청 큐에 넣지 못했다.
    [[nodiscard]] LobbyActions on_submit_dropped();

    // StartStun 으로 시작한 STUN 단계가 끝났다. ok 는 서로 다른 두 서버의 응답을 얻었다는 뜻이다.
    [[nodiscard]] LobbyActions on_stun_done(bool ok, std::span<const network::StunMapping> mappings);

    // 이 객체가 건 타이머의 만료. 자기 이름이 아니거나 지금 걸려 있지 않으면 무시한다.
    [[nodiscard]] LobbyActions on_timer(std::string_view name);

    // 정확히 위 네 이름만 참이다. 접두 일치를 쓰지 않는다 (허용 목록).
    [[nodiscard]] static bool owns_timer(std::string_view name) noexcept;

    [[nodiscard]] LobbyStage stage() const noexcept { return stage_; }
    [[nodiscard]] LobbyRole role() const noexcept { return role_; }
    [[nodiscard]] bool in_lobby() const noexcept { return stage_ == LobbyStage::kLobby; }
    [[nodiscard]] bool request_pending() const noexcept { return pending_.has_value(); }
    [[nodiscard]] bool host_reporting() const noexcept { return reporting_; }

private:
    class Batch;

    struct Pending {
        http::Op op = http::Op::kCreateRoom;
        bool orphaned = false;  // 그 시도가 끝났다. 응답을 버린다
    };

    void start_attempt(Batch& out, LobbyRole role, std::string_view room);
    void submit(Batch& out, http::Op op, std::string body);
    void send_or_defer_request(Batch& out);
    void send_host_report(Batch& out);
    void run_due(Batch& out);
    void retry_or_fail(Batch& out);
    void fail(Batch& out, FailCode code);
    void end_attempt(Batch& out, std::string_view reason);
    void arm(Batch& out, std::string_view name, Millis delay_ms);
    void disarm(Batch& out, std::string_view name);
    [[nodiscard]] bool armed(std::string_view name) const noexcept;
    void forget_armed(std::string_view name) noexcept;

    void on_issued(Batch& out, const Reply<Issued>& reply);
    void on_registered(Batch& out, const Reply<Registered>& reply);
    void on_peers(Batch& out, const Reply<Peers>& reply);
    void on_host_report(Batch& out, const Reply<HostReport>& reply);

    NonceSource nonce_;

    LobbyStage stage_ = LobbyStage::kLobby;
    LobbyRole role_ = LobbyRole::kNone;
    std::optional<Pending> pending_;

    // 이 참가의 값. 시도가 끝나면 지운다.
    std::string room_id_;
    std::uint32_t peer_id_ = 0;
    std::optional<PeerToken> token_;

    // 재시도할 요청 (create_room, join_room, register_candidate). 같은 본문을 다시 보낸다 (8.3).
    http::Op request_op_ = http::Op::kCreateRoom;
    std::string request_body_;
    std::uint32_t tries_ = 0;
    bool request_due_ = false;  // 보낼 차례인데 다른 요청이 미결이었다

    // 호스트의 host_report.
    bool reporting_ = false;
    bool report_due_ = false;
    std::size_t last_peer_count_ = 0;
    std::vector<std::uint32_t> confirm_;
    std::vector<std::uint32_t> announced_;  // control.peers 를 이미 낸 상대

    std::vector<std::string_view> armed_;
};

}  // namespace sangtachi::control
