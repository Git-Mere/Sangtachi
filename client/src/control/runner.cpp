#include "sangtachi/control/runner.hpp"

#include "sangtachi/control/channel.hpp"
#include "sangtachi/control/exchange.hpp"
#include "sangtachi/control/http.hpp"
#include "sangtachi/control/lobby.hpp"
#include "sangtachi/control/ops.hpp"
#include "sangtachi/log.hpp"
#include "sangtachi/network/stun_client.hpp"
#include "sangtachi/peer/candidates.hpp"
#include "sangtachi/peer/session.hpp"
#include "sangtachi/tunnel/packet.hpp"
#include "sangtachi/tunnel/tunnel.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace sangtachi::control {
namespace {

// 응답 하나를 op 에 맞는 해석으로 돌린다. 받지 못한 응답은 전송 오류다 (control_plane.md 3.5, 8.3).
// own_subnets 는 상대 후보 위생의 자기 서브넷 행이 쓴다 (protocol.md 10.1).
ControlReply interpret(const ControlResponse& response, const peer::OwnSubnets& own_subnets) {
    ControlReply out;
    out.op = response.op;
    const bool got = response.result.complete();
    const std::string_view raw = response.result.response;
    switch (response.op) {
        case http::Op::kCreateRoom:
            out.reply = got ? interpret_create_room(raw) : transport_failure<Issued>();
            break;
        case http::Op::kJoinRoom:
            out.reply = got ? interpret_join_room(raw) : transport_failure<Issued>();
            break;
        case http::Op::kRegisterCandidate:
            out.reply = got ? interpret_register_candidate(raw) : transport_failure<Registered>();
            break;
        case http::Op::kGetPeers:
            out.reply = got ? interpret_get_peers(raw, response.self_peer_id, own_subnets)
                            : transport_failure<Peers>();
            break;
        case http::Op::kHostReport:
            out.reply = got ? interpret_host_report(raw, response.self_peer_id, own_subnets)
                            : transport_failure<HostReport>();
            break;
    }
    return out;
}

void emit_transport(const ControlResponse& response) {
    const LogField fields[] = {
        field("op", http::op_name(response.op)),
        field("status", to_token(response.result.status)),
        field("code", static_cast<std::uint64_t>(response.result.os_error)),
        field("frame", http::to_token(response.result.frame_error)),
    };
    emit(LogLevel::Warn, "control.transport", fields);
}

}  // namespace

std::string_view record_result(const SessionEvent& event, bool connected) noexcept {
    if (event.to == peer::SessionState::kFailed) {
        return peer::to_token(event.failure.value_or(peer::SessionFailure::kTunnelDropped));
    }
    if (event.peer_close_reason == tunnel::CloseReason::kConfigError ||
        event.peer_close_reason == tunnel::CloseReason::kSetupFailed) {
        return "PEER_FAILED";
    }
    return connected ? "CLOSED" : "ABORTED";
}

ControlRunner::ControlRunner(RunnerDeps deps, TimerSet& timers, Counters& counters)
    : deps_(std::move(deps)),
      timers_(timers),
      counters_(counters),
      lobby_(deps_.nonce, deps_.name, deps_.clock,
             [this](std::span<const network::Endpoint> reflexive) { return collect(reflexive); }) {}

std::vector<Candidate> ControlRunner::collect(std::span<const network::Endpoint> reflexive) {
    const auto& sources = deps_.candidate_sources;
    if (!sources.query_interfaces || !sources.query_routes) {
        return peer::registration_list(reflexive, {}, std::nullopt, deps_.local_port);
    }
    static const peer::OwnSubnets kNoSubnets;
    const peer::OwnSubnets& previous = deps_.own_subnets ? deps_.own_subnets() : kNoSubnets;
    // 자기 Wintun 식별자 (protocol.md 10.1 제외 단계). `--no-adapter` 면 없다.
    peer::Collection collection =
        peer::collect(sources, reflexive, deps_.local_port, deps_.own_wintun, previous);
    peer::emit_collection_warnings(collection);
    if (deps_.set_own_subnets) {
        deps_.set_own_subnets(std::move(collection.own_subnets));
    }
    return std::move(collection.registration);
}

ControlRunner::~ControlRunner() {
    // 루프의 타이머 집합은 이 객체보다 오래 산다. 남긴 STUN 타이머가 버린 객체를 부르지 않게 한다.
    drop_stun();
}

void ControlRunner::start() {
    run(lobby_.on_start());
}

void ControlRunner::on_command(const LobbyCommand& command) {
    run(lobby_.on_command(command));
}

void ControlRunner::on_response(const ControlResponse& response) {
    if (!response.result.complete()) {
        emit_transport(response);
    }
    // 집합을 주지 않은 실행기는 빈 집합이다 (runner.hpp 의 RunnerDeps::own_subnets).
    static const peer::OwnSubnets kNoSubnets;
    const peer::OwnSubnets& own = deps_.own_subnets ? deps_.own_subnets() : kNoSubnets;
    run(lobby_.on_reply(interpret(response, own)));
}

void ControlRunner::on_stun_datagram(const network::Endpoint& from,
                                     std::span<const std::byte> payload) {
    if (!stun_) {
        // 시도 밖이다. 기다리는 트랜잭션이 없으므로 ID 가 맞을 수 없다 (protocol.md 13장의 응답
        // 검증, stun_client.hpp 의 drop_stun_parse 와 같은 판정).
        counters_.increment(Counter::DropStunParse);
        return;
    }
    stun_->on_datagram(from, payload, deps_.clock());
    check_stun_done();
}

void ControlRunner::on_timer(std::string_view name) {
    if (Lobby::owns_timer(name)) {
        run(lobby_.on_timer(name));
        return;
    }
    if (stun_ && network::StunClient::owns_timer(name)) {
        stun_->on_timer(name, deps_.clock());
        check_stun_done();
    }
}

void ControlRunner::on_session_transition(const peer::Session& session, peer::SessionState from,
                                          peer::SessionState to) {
    SessionEvent event;
    event.peer_id = session.peer_id();
    event.from = from;
    event.to = to;
    event.failure = session.failure();
    event.peer_close_reason = session.peer_close_reason();
    event.last_rtt_us = session.last_rtt_us();
    events_.push_back(QueuedEvent{std::move(event), deps_.clock()});
}

void ControlRunner::flush() {
    run({});
}

void ControlRunner::shutdown() {
    run(lobby_.on_shutdown());
}

void ControlRunner::run(LobbyActions actions) {
    for (auto& action : actions) {
        queue_.push_back(std::move(action));
    }
    if (running_) {
        return;  // 바깥 run 이 이어서 한다. Lobby 를 다시 들어가지 않는다
    }
    running_ = true;
    // 행동을 먼저 다 하고 그 사이 쌓인 전이를 하나씩 처리한다. 전이가 낸 행동은 다시 큐로 간다.
    while (!queue_.empty() || !events_.empty()) {
        if (!queue_.empty()) {
            LobbyAction action = std::move(queue_.front());
            queue_.pop_front();
            execute(action);
            continue;
        }
        const QueuedEvent queued = std::move(events_.front());
        events_.pop_front();
        record(queued.event, queued.at);
        for (auto& action : lobby_.on_session_event(queued.event)) {
            queue_.push_back(std::move(action));
        }
    }
    running_ = false;
}

void ControlRunner::record(const SessionEvent& event, Millis at) {
    const auto it = tracks_.find(event.peer_id);
    if (it == tracks_.end()) {
        return;
    }
    Track& track = it->second;
    const std::string_view role = to_token(track.role);
    const RecordSink& sink = deps_.records;
    if (event.to == peer::SessionState::kConnected && !track.connected) {
        track.connected = true;
        const std::uint64_t setup = at >= track.setup_base_ms ? at - track.setup_base_ms : 0;
        track.attempt =
            sink.establish ? sink.establish(role, event.peer_id, "OK", event.last_rtt_us, setup) : 0;
        return;
    }
    if (!peer::is_terminal(event.to)) {
        return;
    }
    const std::string_view result = record_result(event, track.connected);
    if (track.connected) {
        if (sink.end) {
            sink.end(track.attempt, role, event.peer_id, result, event.last_rtt_us);
        }
    } else if (sink.establish) {
        (void)sink.establish(role, event.peer_id, result, event.last_rtt_us, std::nullopt);
    }
    tracks_.erase(it);
}

void ControlRunner::open_session(const OpenSession& open) {
    const std::uint32_t peer_id = open.request.peer_id;
    peer::Session* session = deps_.tunnel ? deps_.tunnel->create_session(open.request) : nullptr;
    if (session == nullptr) {
        run(lobby_.on_session_open_failed(peer_id));
        return;
    }
    tracks_[peer_id] = Track{open.role, open.setup_base_ms, false, 0};
}

void ControlRunner::execute(LobbyAction& action) {
    std::visit(
        [this](auto& a) {
            using T = std::decay_t<decltype(a)>;
            if constexpr (std::is_same_v<T, SubmitRequest>) {
                submit(a);
            } else if constexpr (std::is_same_v<T, StartStun>) {
                start_stun();
            } else if constexpr (std::is_same_v<T, CancelStun>) {
                drop_stun();
            } else if constexpr (std::is_same_v<T, ArmTimer>) {
                if (!timers_.add_once(a.name, a.delay_ms, deps_.clock())) {
                    // 같은 이름이 이미 걸려 있다. 조용히 넘기면 그 마감이 영영 오지 않는다.
                    const LogField fields[] = {field("name", a.name)};
                    emit(LogLevel::Warn, "timer.rejected", fields);
                }
            } else if constexpr (std::is_same_v<T, CancelTimer>) {
                (void)timers_.cancel(a.name);
            } else if constexpr (std::is_same_v<T, PrintLine>) {
                if (deps_.print) {
                    deps_.print(a.text);
                }
            } else if constexpr (std::is_same_v<T, GuideLine>) {
                if (deps_.print_guide) {
                    deps_.print_guide(a.text);
                }
            } else if constexpr (std::is_same_v<T, LogLine>) {
                emit(a.level, a.event, a.fields);
            } else if constexpr (std::is_same_v<T, OpenSession>) {
                open_session(a);
            } else if constexpr (std::is_same_v<T, StartPunch>) {
                if (deps_.tunnel != nullptr) {
                    if (peer::Session* s = deps_.tunnel->find(a.peer_id); s != nullptr) {
                        (void)s->start_punch(deps_.clock());
                    }
                }
            } else if constexpr (std::is_same_v<T, LeaveRoom>) {
                if (deps_.tunnel != nullptr) {
                    deps_.tunnel->leave_room(deps_.clock());
                }
                if (deps_.clear_address) {
                    deps_.clear_address();
                }
            } else if constexpr (std::is_same_v<T, AssignAddress>) {
                // 8.5 의 자신의 가상 IP 는 방에 든 때부터 안다. 세션을 기다리면 호스트 혼자인 동안 어댑터에서 읽은
                // 패킷이 모두 tx_drop_bad_src 로 잘못 세진다.
                if (deps_.tunnel != nullptr) {
                    deps_.tunnel->set_local_virtual_ip(a.virtual_ip);
                }
                if (deps_.assign_address) {
                    deps_.assign_address(a.virtual_ip);
                }
            } else if constexpr (std::is_same_v<T, StartRoster>) {
                if (deps_.tunnel != nullptr) {
                    deps_.tunnel->start_roster(a.self);
                }
            } else if constexpr (std::is_same_v<T, SetRosterPeers>) {
                if (deps_.tunnel != nullptr) {
                    deps_.tunnel->set_roster_peers(std::move(a.peers));
                }
            } else if constexpr (std::is_same_v<T, RecordAttempt>) {
                if (deps_.records.establish) {
                    (void)deps_.records.establish(to_token(a.role), a.peer, a.result, std::nullopt,
                                                  std::nullopt);
                }
            }
        },
        action);
}

void ControlRunner::submit(SubmitRequest& request) {
    auto bytes = http::build_request(deps_.host_header, request.op, request.body);
    bool queued = false;
    if (bytes) {
        queued = deps_.submit(ControlRequest{request.op, std::move(*bytes), request.self_peer_id});
    } else {
        // Host 헤더나 본문이 build_request 에 걸렸다. 보낼 수 없으므로 큐가 찬 것과 같이 그
        // 시도를 끝낸다. 기동 시 host_header 를 확인하므로 정상 경로에서는 오지 않는다.
        const LogField fields[] = {field("op", http::op_name(request.op))};
        emit(LogLevel::Error, "control.request_rejected", fields);
    }
    if (!queued) {
        // 요청 큐가 찼다 (concurrency.md 8장 "가득 찼을 때"). 카운터는 채널이 이미 올렸다.
        // 계약상 SubmitRequest 는 목록의 마지막이라 남은 행동이 없다.
        run(lobby_.on_submit_dropped());
    }
}

void ControlRunner::start_stun() {
    drop_stun();
    // 시도마다 새 객체다. StunClient::start 는 끝난 뒤 다시 부르면 아무것도 하지 않는다
    // (control_plane.md 8.4 "5번 STUN 도 다시 돈다").
    stun_ = std::make_unique<network::StunClient>(deps_.stun_servers, deps_.send, timers_, counters_,
                                                  deps_.stun_random);
    stun_reported_ = false;
    (void)stun_->start(deps_.clock());
    // 목록이 두 개 미만이거나 난수를 얻지 못하면 그 자리에서 끝난다.
    check_stun_done();
}

void ControlRunner::drop_stun() {
    if (!stun_) {
        return;
    }
    // StunClient 는 소멸자에서 타이머를 지우지 않는다 (lobby.hpp 의 통합 계약). 이름은
    // `stun.retry.<인덱스>` 와 `stun.deadline.<인덱스>` 이고 인덱스는 목록 안의 자리다
    // (stun_client.hpp 의 표).
    for (std::size_t i = 0; i < deps_.stun_servers.size(); ++i) {
        (void)timers_.cancel("stun.retry." + std::to_string(i));
        (void)timers_.cancel("stun.deadline." + std::to_string(i));
    }
    stun_.reset();
    stun_reported_ = false;
}

void ControlRunner::check_stun_done() {
    if (!stun_ || stun_reported_ || !stun_->done()) {
        return;
    }
    stun_reported_ = true;
    const bool ok = stun_->phase() == network::StunPhase::Succeeded;
    // mappings 를 복사한다. on_stun_done 의 행동이 이 객체를 버릴 수 있다.
    const std::vector<network::StunMapping> mappings = stun_->mappings();
    run(lobby_.on_stun_done(ok, mappings));
}

}  // namespace sangtachi::control
