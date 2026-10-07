#include "sangtachi/control/runner.hpp"

#include "sangtachi/control/channel.hpp"
#include "sangtachi/control/exchange.hpp"
#include "sangtachi/control/http.hpp"
#include "sangtachi/control/lobby.hpp"
#include "sangtachi/control/ops.hpp"
#include "sangtachi/log.hpp"
#include "sangtachi/network/stun_client.hpp"

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
ControlReply interpret(const ControlResponse& response) {
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
            out.reply = got ? interpret_get_peers(raw, response.self_peer_id)
                            : transport_failure<Peers>();
            break;
        case http::Op::kHostReport:
            out.reply = got ? interpret_host_report(raw, response.self_peer_id)
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

ControlRunner::ControlRunner(RunnerDeps deps, TimerSet& timers, Counters& counters)
    : deps_(std::move(deps)), timers_(timers), counters_(counters), lobby_(deps_.nonce) {}

ControlRunner::~ControlRunner() {
    // 루프의 타이머 집합은 이 객체보다 오래 산다. 남긴 STUN 타이머가 버린 객체를 부르지 않게 한다.
    drop_stun();
}

void ControlRunner::on_command(const LobbyCommand& command) {
    run(lobby_.on_command(command));
}

void ControlRunner::on_response(const ControlResponse& response) {
    if (!response.result.complete()) {
        emit_transport(response);
    }
    run(lobby_.on_reply(interpret(response)));
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

void ControlRunner::run(LobbyActions actions) {
    for (auto& action : actions) {
        queue_.push_back(std::move(action));
    }
    if (running_) {
        return;  // 바깥 run 이 이어서 한다. Lobby 를 다시 들어가지 않는다
    }
    running_ = true;
    while (!queue_.empty()) {
        LobbyAction action = std::move(queue_.front());
        queue_.pop_front();
        execute(action);
    }
    running_ = false;
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
                if (!timers_.add_once(std::string(a.name), a.delay_ms, deps_.clock())) {
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
            } else if constexpr (std::is_same_v<T, LogLine>) {
                emit(a.level, a.event, a.fields);
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
