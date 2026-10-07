#include "sangtachi/control/lobby.hpp"

#include "sangtachi/protocol_constants.hpp"

#include <algorithm>
#include <utility>

namespace sangtachi::control {

// ---------------------------------------------------------------- 표준 출력 줄

std::string_view to_token(FailCode code) noexcept {
    switch (code) {
        case FailCode::kStunDiscoveryFailed:        return "STUN_DISCOVERY_FAILED";
        case FailCode::kControlPlaneExchangeFailed: return "CONTROL_PLANE_EXCHANGE_FAILED";
        case FailCode::kHolePunchTimeout:           return "HOLE_PUNCH_TIMEOUT";
        case FailCode::kPeerHandshakeFailed:        return "PEER_HANDSHAKE_FAILED";
        case FailCode::kTunnelDropped:              return "TUNNEL_DROPPED";
    }
    return "CONTROL_PLANE_EXCHANGE_FAILED";
}

// architecture.md 8장 실패 진단의 문장 표. 그 표가 출처다. 여기를 고치지 말고 그 표를 고친다.
std::string_view fail_sentence(FailCode code) noexcept {
    switch (code) {
        case FailCode::kStunDiscoveryFailed:
            return "인터넷에서 내 주소를 확인하지 못했습니다. 네트워크 연결을 확인하고 다시 시도하세요.";
        case FailCode::kControlPlaneExchangeFailed:
            return "방 정보를 주고받지 못했습니다. 방 코드가 맞는지, 방이 아직 열려 있는지 확인하고 다시 시도하세요.";
        case FailCode::kHolePunchTimeout:
            return "상대와 직접 연결을 만들지 못했습니다. 둘 중 한 명이 다른 네트워크에서 다시 시도해 보세요.";
        case FailCode::kPeerHandshakeFailed:
            return "연결이 한쪽 방향만 열렸습니다. 방화벽 설정을 확인하고 다시 시도하세요.";
        case FailCode::kTunnelDropped:
            return "상대와의 연결이 끊어졌습니다. 상대가 접속을 종료했거나 네트워크가 불안정합니다.";
    }
    return "방 정보를 주고받지 못했습니다. 방 코드가 맞는지, 방이 아직 열려 있는지 확인하고 다시 시도하세요.";
}

std::string fail_line(FailCode code) {
    std::string line = "FAIL ";
    line.append(to_token(code));
    line.push_back(' ');
    line.append(fail_sentence(code));
    return line;
}

std::string room_line(std::string_view room_id) {
    std::string line = "ROOM ";
    line.append(room_id);
    return line;
}

// ---------------------------------------------------------------- 로비 명령

namespace {

bool is_blank(char c) noexcept {
    return c == ' ' || c == '\t';
}

// line 의 낱말을 차례로 꺼낸다.
std::string_view next_word(std::string_view& rest) noexcept {
    std::size_t begin = 0;
    while (begin < rest.size() && is_blank(rest[begin])) {
        ++begin;
    }
    std::size_t end = begin;
    while (end < rest.size() && !is_blank(rest[end])) {
        ++end;
    }
    const std::string_view word = rest.substr(begin, end - begin);
    rest.remove_prefix(end);
    return word;
}

}  // namespace

std::optional<LobbyCommand> parse_lobby_command(std::string_view line) {
    std::string_view rest = line;
    const std::string_view head = next_word(rest);

    LobbyCommand command;
    if (head == "host") {
        command.kind = LobbyCommandKind::kHost;
    } else if (head == "join") {
        command.kind = LobbyCommandKind::kJoin;
        command.argument = std::string(next_word(rest));
    } else if (head == "leave") {
        command.kind = LobbyCommandKind::kLeave;
    } else {
        return std::nullopt;
    }
    command.extra = !next_word(rest).empty();
    return command;
}

std::optional<LobbyCommand> startup_command(Role role, const std::optional<std::string>& room) {
    switch (role) {
        case Role::Host:
            return LobbyCommand{LobbyCommandKind::kHost, {}, false};
        case Role::Player:
            return LobbyCommand{LobbyCommandKind::kJoin, room.value_or(std::string()), false};
        case Role::None:
            return std::nullopt;
    }
    return std::nullopt;
}

// ---------------------------------------------------------------- 행동 묶음

// 한 사건이 만든 행동. SubmitRequest 는 하나만 들고 있다가 맨 끝에 붙인다 (머리의 통합 계약).
class Lobby::Batch {
public:
    void push(LobbyAction action) { actions_.push_back(std::move(action)); }

    void log(LogLevel level, std::string_view event, std::vector<LogField> fields = {}) {
        actions_.push_back(LogLine{level, event, std::move(fields)});
    }

    // 미결 요청이 하나라서 한 사건이 둘을 보낼 일은 없다. Lobby::submit 이 미결을 먼저 본다.
    void set_submit(SubmitRequest request) { submit_ = std::move(request); }

    [[nodiscard]] LobbyActions finish() && {
        if (submit_) {
            actions_.push_back(std::move(*submit_));
        }
        return std::move(actions_);
    }

private:
    LobbyActions actions_;
    std::optional<SubmitRequest> submit_;
};

namespace {

std::string_view role_token(LobbyRole role) noexcept {
    switch (role) {
        case LobbyRole::kHost:   return "host";
        case LobbyRole::kPlayer: return "player";
        case LobbyRole::kNone:   return "none";
    }
    return "none";
}

std::string_view command_token(LobbyCommandKind kind) noexcept {
    switch (kind) {
        case LobbyCommandKind::kHost:  return "host";
        case LobbyCommandKind::kJoin:  return "join";
        case LobbyCommandKind::kLeave: return "leave";
    }
    return "host";
}

std::vector<LogField> rejected_fields(std::string_view reason, LobbyCommandKind kind) {
    std::vector<LogField> fields;
    fields.push_back(field("reason", reason));
    fields.push_back(field("command", command_token(kind)));
    return fields;
}

std::vector<LogField> op_fields(http::Op op) {
    std::vector<LogField> fields;
    fields.push_back(field("op", http::op_name(op)));
    return fields;
}

// op 가 어느 응답 형을 갖는가. ControlReply::reply 의 자리 번호다.
std::size_t reply_index(http::Op op) noexcept {
    switch (op) {
        case http::Op::kCreateRoom:
        case http::Op::kJoinRoom:          return 0;
        case http::Op::kRegisterCandidate: return 1;
        case http::Op::kGetPeers:          return 2;
        case http::Op::kHostReport:        return 3;
    }
    return 0;
}

constexpr std::size_t kMaxOthers = protocol::kMaxPeers - 1;
constexpr Millis kHostReportOpenMs = Millis{kHostReportOpenS} * 1000;
constexpr Millis kHostReportFullMs = Millis{kHostReportFullS} * 1000;

// 플레이어의 get_peers 성공 응답은 상대가 정확히 하나다 (4.5 응답 표). 아니면 형이 틀린
// 성공이고 전송 오류다 (3.5 의 7번). 호스트가 부르면 길이가 다르므로 ops 의 해석이 아니라 여기서 본다.
Reply<Peers> as_player_reply(const Reply<Peers>& reply) {
    if (reply.outcome == Outcome::kSuccess && reply.value && reply.value->peers.size() != 1) {
        return transport_failure<Peers>();
    }
    return reply;
}

}  // namespace

// ---------------------------------------------------------------- Lobby

Lobby::Lobby(NonceSource nonce) : nonce_(std::move(nonce)) {}

bool Lobby::owns_timer(std::string_view name) noexcept {
    return name == kRetryTimer || name == kPollTimer || name == kDeadlineTimer ||
           name == kHostReportTimer;
}

LobbyActions Lobby::on_command(const LobbyCommand& command) {
    Batch out;
    const LobbyCommandKind kind = command.kind;

    if (kind == LobbyCommandKind::kLeave) {
        // leave 는 로비가 아닐 때만 받는다 (architecture.md 3.5 로비 명령 표).
        if (in_lobby()) {
            out.log(LogLevel::Warn, "console.rejected", rejected_fields("in_lobby", kind));
            return std::move(out).finish();
        }
        if (command.extra) {
            out.log(LogLevel::Warn, "console.rejected", rejected_fields("extra_argument", kind));
            return std::move(out).finish();
        }
        end_attempt(out, "leave");
        return std::move(out).finish();
    }

    // host 와 join 은 로비에 있고 미결 제어 요청이 없을 때만 받는다.
    if (!in_lobby()) {
        out.log(LogLevel::Warn, "console.rejected", rejected_fields("not_in_lobby", kind));
        return std::move(out).finish();
    }
    if (pending_) {
        out.log(LogLevel::Warn, "console.rejected", rejected_fields("request_pending", kind));
        return std::move(out).finish();
    }
    if (command.extra) {
        out.log(LogLevel::Warn, "console.rejected", rejected_fields("extra_argument", kind));
        return std::move(out).finish();
    }

    if (kind == LobbyCommandKind::kHost) {
        start_attempt(out, LobbyRole::kHost, {});
        return std::move(out).finish();
    }

    // join. 방 코드의 형식 검사는 --room 과 같다 (control_plane.md 2.1). 친 값은 로그에 싣지 않는다.
    if (command.argument.empty()) {
        out.log(LogLevel::Warn, "console.rejected", rejected_fields("missing_room", kind));
        return std::move(out).finish();
    }
    if (!is_room_id_input(command.argument)) {
        out.log(LogLevel::Warn, "console.rejected", rejected_fields("bad_room", kind));
        return std::move(out).finish();
    }
    start_attempt(out, LobbyRole::kPlayer, command.argument);
    return std::move(out).finish();
}

void Lobby::start_attempt(Batch& out, LobbyRole role, std::string_view room) {
    role_ = role;
    stage_ = LobbyStage::kIssuing;
    {
        std::vector<LogField> fields;
        fields.push_back(field("role", role_token(role)));
        out.log(LogLevel::Info, "attempt.start", std::move(fields));
    }

    // 한 참가에 한 값이다. 재시도에는 같은 본문을 다시 보내므로 값이 바뀌지 않는다 (2.4).
    const std::optional<ClientNonce> nonce = nonce_ ? nonce_() : std::nullopt;
    if (!nonce) {
        fail(out, FailCode::kControlPlaneExchangeFailed);
        return;
    }
    std::optional<std::string> body;
    if (role == LobbyRole::kHost) {
        request_op_ = http::Op::kCreateRoom;
        body = create_room_body(*nonce);
    } else {
        request_op_ = http::Op::kJoinRoom;
        body = join_room_body(room, *nonce);
    }
    if (!body) {
        fail(out, FailCode::kControlPlaneExchangeFailed);
        return;
    }
    request_body_ = std::move(*body);
    tries_ = 0;
    send_or_defer_request(out);
}

void Lobby::submit(Batch& out, http::Op op, std::string body) {
    pending_ = Pending{op, false};
    out.set_submit(SubmitRequest{op, std::move(body), peer_id_});
}

void Lobby::send_or_defer_request(Batch& out) {
    if (pending_) {
        request_due_ = true;
        return;
    }
    request_due_ = false;
    ++tries_;
    submit(out, request_op_, request_body_);
}

void Lobby::send_host_report(Batch& out) {
    if (pending_) {
        report_due_ = true;
        return;
    }
    report_due_ = false;
    // Phase 3 에는 세션이 없으므로 departed 는 늘 비어 있다 (8.4 "Phase 3 에는 9번과 10번이 없다").
    std::optional<std::string> body = host_report_body(room_id_, peer_id_, *token_, {}, confirm_);
    if (!body) {
        fail(out, FailCode::kControlPlaneExchangeFailed);
        return;
    }
    // 주기는 매 송신에서 다시 센다 (protocol.md 11장 타이머). 간격은 직전 응답의 peers 길이로 정한다 (4.6).
    disarm(out, kHostReportTimer);
    arm(out, kHostReportTimer,
        last_peer_count_ >= kMaxOthers ? kHostReportFullMs : kHostReportOpenMs);
    submit(out, http::Op::kHostReport, std::move(*body));
}

void Lobby::run_due(Batch& out) {
    if (pending_ || in_lobby()) {
        return;
    }
    if (request_due_) {
        send_or_defer_request(out);
        return;
    }
    if (report_due_ && reporting_) {
        send_host_report(out);
    }
}

void Lobby::retry_or_fail(Batch& out) {
    if (tries_ < kMaxTries) {
        arm(out, kRetryTimer, kRetryIntervalMs);
        return;
    }
    fail(out, FailCode::kControlPlaneExchangeFailed);
}

void Lobby::fail(Batch& out, FailCode code) {
    out.push(PrintLine{fail_line(code)});
    if (code != FailCode::kStunDiscoveryFailed) {
        // STUN 실패의 session.failed 는 StunClient 가 이미 냈다 (network/stun_client.hpp).
        std::vector<LogField> fields;
        fields.push_back(field("code", to_token(code)));
        out.log(LogLevel::Error, "session.failed", std::move(fields));
    }
    end_attempt(out, "failed");
}

void Lobby::end_attempt(Batch& out, std::string_view reason) {
    // 시도의 타이머를 전부 지운다 (protocol.md 11장 "시도가 끝나면 그 시도의 타이머를 전부 지운다").
    while (!armed_.empty()) {
        disarm(out, armed_.back());
    }
    if (stage_ == LobbyStage::kStun) {
        out.push(CancelStun{});
    }
    // 미결 요청은 끝까지 간다. 그 응답은 버린다 (concurrency.md 8장).
    if (pending_) {
        pending_->orphaned = true;
    }
    request_due_ = false;
    report_due_ = false;
    reporting_ = false;
    request_body_.clear();
    tries_ = 0;
    room_id_.clear();
    peer_id_ = 0;
    token_.reset();
    last_peer_count_ = 0;
    confirm_.clear();
    announced_.clear();
    role_ = LobbyRole::kNone;
    stage_ = LobbyStage::kLobby;

    std::vector<LogField> fields;
    fields.push_back(field("reason", reason));
    out.log(LogLevel::Info, "attempt.end", std::move(fields));
}

void Lobby::arm(Batch& out, std::string_view name, Millis delay_ms) {
    if (armed(name)) {
        out.push(CancelTimer{name});
    } else {
        armed_.push_back(name);
    }
    out.push(ArmTimer{name, delay_ms});
}

void Lobby::disarm(Batch& out, std::string_view name) {
    if (!armed(name)) {
        return;
    }
    forget_armed(name);
    out.push(CancelTimer{name});
}

bool Lobby::armed(std::string_view name) const noexcept {
    return std::find(armed_.begin(), armed_.end(), name) != armed_.end();
}

void Lobby::forget_armed(std::string_view name) noexcept {
    armed_.erase(std::remove(armed_.begin(), armed_.end(), name), armed_.end());
}

LobbyActions Lobby::on_submit_dropped() {
    Batch out;
    if (!pending_) {
        return std::move(out).finish();
    }
    const bool orphaned = pending_->orphaned;
    pending_.reset();
    // 요청 큐가 차면 그 시도를 CONTROL_PLANE_EXCHANGE_FAILED 로 끝낸다 (concurrency.md 8장).
    if (!orphaned && !in_lobby()) {
        fail(out, FailCode::kControlPlaneExchangeFailed);
    }
    return std::move(out).finish();
}

LobbyActions Lobby::on_reply(const ControlReply& reply) {
    Batch out;
    if (!pending_) {
        out.log(LogLevel::Warn, "control.unexpected_reply", op_fields(reply.op));
        return std::move(out).finish();
    }
    const Pending pending = *pending_;
    pending_.reset();

    if (pending.op != reply.op || reply.reply.index() != reply_index(pending.op)) {
        out.log(LogLevel::Warn, "control.reply_mismatch", op_fields(pending.op));
        if (!pending.orphaned && !in_lobby()) {
            fail(out, FailCode::kControlPlaneExchangeFailed);
        }
        run_due(out);
        return std::move(out).finish();
    }

    if (pending.orphaned) {
        // 끝난 시도의 응답이다. 반영하지 않고 ROOM 줄도 내지 않는다 (concurrency.md 8장).
        out.log(LogLevel::Info, "control.discarded", op_fields(reply.op));
        return std::move(out).finish();
    }

    // 플레이어만 get_peers 를 부른다 (8.4 의 7번). 그 응답의 길이 규칙을 control.result 보다 먼저 본다.
    ControlReply checked = reply;
    if (reply.op == http::Op::kGetPeers) {
        checked.reply = as_player_reply(std::get<2>(reply.reply));
    }

    std::visit(
        [&](const auto& r) {
            out.log(r.outcome == Outcome::kSuccess ? LogLevel::Info : LogLevel::Warn,
                    "control.result", control_result_fields(reply.op, r.outcome, r.failure));
        },
        checked.reply);

    switch (reply.op) {
        case http::Op::kCreateRoom:
        case http::Op::kJoinRoom:
            on_issued(out, std::get<0>(checked.reply));
            break;
        case http::Op::kRegisterCandidate:
            on_registered(out, std::get<1>(checked.reply));
            break;
        case http::Op::kGetPeers:
            on_peers(out, std::get<2>(checked.reply));
            break;
        case http::Op::kHostReport:
            on_host_report(out, std::get<3>(checked.reply));
            break;
    }
    run_due(out);
    return std::move(out).finish();
}

void Lobby::on_issued(Batch& out, const Reply<Issued>& reply) {
    if (reply.outcome == Outcome::kTransient) {
        retry_or_fail(out);
        return;
    }
    if (reply.outcome != Outcome::kSuccess || !reply.value) {
        fail(out, FailCode::kControlPlaneExchangeFailed);
        return;
    }
    const Issued& issued = *reply.value;
    // 응답의 room_id 는 정규화된 값이다. 이후 이 값을 쓴다 (4.3).
    room_id_ = issued.room_id;
    peer_id_ = issued.peer_id;
    token_ = issued.peer_token;
    request_body_.clear();
    tries_ = 0;

    out.push(PrintLine{room_line(room_id_)});
    if (role_ == LobbyRole::kHost) {
        // host_report 는 create_room 성공 응답에서 시작한다 (protocol.md 11장, 8.4).
        reporting_ = true;
        arm(out, kHostReportTimer, kHostReportOpenMs);
    }
    stage_ = LobbyStage::kStun;
    out.push(StartStun{});
}

LobbyActions Lobby::on_stun_done(bool ok, std::span<const network::StunMapping> mappings) {
    Batch out;
    if (stage_ != LobbyStage::kStun) {
        return std::move(out).finish();  // 끝난 시도의 결과다
    }
    // STUN 단계는 끝났다. 이 뒤에 시도가 끝나도 CancelStun 을 내지 않는다.
    stage_ = LobbyStage::kRegistering;
    if (!ok || mappings.empty()) {
        fail(out, FailCode::kStunDiscoveryFailed);
        return std::move(out).finish();
    }

    // 반사 후보만 보낸다. 로컬 후보 수집은 Phase 4 다. 8개를 넘으면 먼저 자른다 (8.4 의 6번).
    std::vector<Candidate> candidates;
    for (const network::StunMapping& mapping : mappings) {
        if (candidates.size() >= protocol::kMaxCandidates) {
            break;
        }
        candidates.push_back(Candidate{mapping.mapped, CandidateKind::kReflexive});
    }
    std::optional<std::string> body =
        register_candidate_body(room_id_, peer_id_, *token_, candidates);
    if (!body) {
        fail(out, FailCode::kControlPlaneExchangeFailed);
        return std::move(out).finish();
    }
    request_op_ = http::Op::kRegisterCandidate;
    request_body_ = std::move(*body);
    tries_ = 0;
    send_or_defer_request(out);
    return std::move(out).finish();
}

void Lobby::on_registered(Batch& out, const Reply<Registered>& reply) {
    if (reply.outcome == Outcome::kTransient) {
        retry_or_fail(out);
        return;
    }
    if (reply.outcome != Outcome::kSuccess || !reply.value) {
        fail(out, FailCode::kControlPlaneExchangeFailed);
        return;
    }
    if (reply.value->rejected != 0) {
        std::vector<LogField> fields;
        fields.push_back(field("rejected", std::uint64_t{reply.value->rejected}));
        out.log(LogLevel::Warn, "control.candidates_rejected", std::move(fields));
    }
    request_body_.clear();
    tries_ = 0;

    if (role_ == LobbyRole::kHost) {
        // 방을 세웠다 (concurrency.md 7장 로비: 방을 세운 시점은 register_candidate 성공).
        stage_ = LobbyStage::kSettled;
        return;
    }
    // 폴링 간격과 마감은 이 성공 응답에서 센다 (protocol.md 11장, 8.4 의 7번).
    stage_ = LobbyStage::kPolling;
    arm(out, kPollTimer, kGetPeersPollMs);
    arm(out, kDeadlineTimer, kGetPeersDeadlineMs);
}

void Lobby::on_peers(Batch& out, const Reply<Peers>& reply) {
    if (reply.outcome == Outcome::kTransient) {
        // 재시도 규칙이 따로 없다. 다음 폴링이 곧 재시도다 (8.3).
        arm(out, kPollTimer, kGetPeersPollMs);
        return;
    }
    if (reply.outcome != Outcome::kSuccess || !reply.value) {
        fail(out, FailCode::kControlPlaneExchangeFailed);
        return;
    }
    // 길이는 on_reply 가 이미 1 로 확인했다 (as_player_reply).
    const std::vector<PeerView>& peers = reply.value->peers;
    if (is_punch_ready(peers.front())) {
        // Phase 3 은 8.4 의 8번에서 멈춘다. control.peers 한 줄을 내고 폴링을 멈춘다.
        out.log(LogLevel::Info, "control.peers", control_peers_fields(peers.front()));
        disarm(out, kDeadlineTimer);
        disarm(out, kPollTimer);
        stage_ = LobbyStage::kSettled;
        return;
    }
    // 준비 전이거나, 준비인데 위생 뒤 후보가 없다. 폴링을 계속한다 (4.5, 8.4).
    arm(out, kPollTimer, kGetPeersPollMs);
}

void Lobby::on_host_report(Batch& out, const Reply<HostReport>& reply) {
    if (host_report_action(reply) == HostReportAction::kStop) {
        // 4.6 오류 표의 첫 행. FAIL 한 번, host_report 를 멈춘다. Phase 3 에는 세션이 없으므로
        // 바로 로비로 간다 (concurrency.md 7장 로비).
        fail(out, FailCode::kControlPlaneExchangeFailed);
        return;
    }
    if (reply.outcome != Outcome::kSuccess || !reply.value) {
        return;  // rate_limited 와 일시 오류. 다음 주기에 다시 부른다 (4.6)
    }

    const std::vector<PeerView>& peers = reply.value->peers;
    last_peer_count_ = peers.size();
    confirm_.clear();
    std::vector<std::uint32_t> still_here;
    for (const PeerView& peer : peers) {
        if (!peer.ready) {
            confirm_.push_back(peer.peer_id);
        }
        const bool announced =
            std::find(announced_.begin(), announced_.end(), peer.peer_id) != announced_.end();
        if (announced) {
            still_here.push_back(peer.peer_id);
        } else if (is_punch_ready(peer)) {
            // 상대마다 첫 응답에서 한 줄 (8.4 "Phase 3 에는 9번과 10번이 없다").
            out.log(LogLevel::Info, "control.peers", control_peers_fields(peer));
            still_here.push_back(peer.peer_id);
        }
    }
    announced_ = std::move(still_here);
}

LobbyActions Lobby::on_timer(std::string_view name) {
    Batch out;
    if (!owns_timer(name) || !armed(name)) {
        return std::move(out).finish();
    }
    forget_armed(name);

    if (name == kRetryTimer) {
        if (stage_ == LobbyStage::kIssuing || stage_ == LobbyStage::kRegistering) {
            send_or_defer_request(out);
        }
    } else if (name == kPollTimer) {
        // 앞 요청이 미결이면 그 바퀴는 건너뛴다 (concurrency.md 8장).
        if (stage_ == LobbyStage::kPolling && !pending_) {
            std::optional<std::string> body = get_peers_body(room_id_, peer_id_, *token_);
            if (!body) {
                fail(out, FailCode::kControlPlaneExchangeFailed);
            } else {
                submit(out, http::Op::kGetPeers, std::move(*body));
            }
        }
    } else if (name == kDeadlineTimer) {
        if (stage_ == LobbyStage::kPolling) {
            // 미결 get_peers 가 있어도 지금 끝낸다. 그 응답은 버린다 (concurrency.md 8장).
            fail(out, FailCode::kControlPlaneExchangeFailed);
        }
    } else if (name == kHostReportTimer) {
        if (reporting_) {
            send_host_report(out);
        }
    }
    return std::move(out).finish();
}

}  // namespace sangtachi::control
