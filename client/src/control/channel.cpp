#include "sangtachi/control/channel.hpp"

#include "sangtachi/control/exchange.hpp"
#include "sangtachi/counters.hpp"
#include "sangtachi/log.hpp"
#include "sangtachi/platform/resolve.hpp"
#include "sangtachi/platform/tcp.hpp"
#include "sangtachi/platform/wait.hpp"

#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

namespace sangtachi::control {

ControlChannel::ControlChannel(Counters& counters, const ExchangeTimeouts& timeouts)
    : counters_(counters), timeouts_(timeouts), resolved_(resolved_promise_.get_future()) {}

std::unique_ptr<ControlChannel> ControlChannel::start(std::string host, std::uint16_t port,
                                                      platform::WaitHandle shutdown_event,
                                                      Counters& counters,
                                                      const ExchangeTimeouts& timeouts) {
    // make_unique 를 쓰지 않는다. 생성자가 비공개다.
    std::unique_ptr<ControlChannel> channel(new ControlChannel(counters, timeouts));

    channel->shutdown_event_ = platform::duplicate_event(shutdown_event);
    // 자동 리셋. 깨어난 쪽이 신호를 가져가므로 따로 리셋하지 않는다 (concurrency.md 2장, 8장).
    channel->request_event_ = platform::create_event(platform::ResetMode::Auto);
    channel->response_event_ = platform::create_event(platform::ResetMode::Auto);
    // 수동 리셋. 끝났다는 사실은 한 번 서면 그대로 남아야 한다.
    channel->done_event_ = platform::create_event(platform::ResetMode::Manual);
    if (channel->shutdown_event_ == nullptr || channel->request_event_ == nullptr ||
        channel->response_event_ == nullptr || channel->done_event_ == nullptr) {
        return nullptr;  // 소멸자가 만든 것만 닫는다. 스레드는 아직 없다
    }

    try {
        ControlChannel* self = channel.get();
        channel->thread_ = std::thread([self, host = std::move(host), port]() mutable {
            self->run(std::move(host), port);
        });
    } catch (const std::system_error&) {
        return nullptr;
    }
    return channel;
}

ControlChannel::~ControlChannel() {
    if (thread_.joinable()) {
        // 종료 이벤트를 신호하지 않으면 `[control]` 이 요청 이벤트를 영영 기다린다. main 의
        // 경로에서는 이미 신호돼 있다. 시험은 이 자리로 끝낸다.
        (void)platform::signal_event(shutdown_event_);
        thread_.join();
    }
    platform::close_event(done_event_);
    platform::close_event(response_event_);
    platform::close_event(request_event_);
    platform::close_event(shutdown_event_);
}

std::optional<network::Endpoint> ControlChannel::wait_resolved() {
    if (!resolved_.valid()) {
        return std::nullopt;
    }
    return resolved_.get();
}

bool ControlChannel::submit(ControlRequest request) {
    if (!requests_.try_push(std::move(request))) {
        // 정상 경로에서는 차지 않는다. 차면 `[control]` 이 멈춘 것이다 (concurrency.md 8장).
        counters_.increment(Counter::ControlQueueDropped);
        return false;
    }
    (void)platform::signal_event(request_event_);
    return true;
}

std::optional<ControlResponse> ControlChannel::try_pop() {
    return responses_.try_pop();
}

bool ControlChannel::join_for(std::uint32_t timeout_ms) {
    if (!thread_.joinable()) {
        return true;
    }
    const platform::WaitHandle handles[] = {done_event_};
    const auto r = platform::wait_any(std::span<const platform::WaitHandle>(handles, 1), timeout_ms);
    if (r.status != platform::WaitStatus::Signaled) {
        return false;
    }
    // done 은 run 의 마지막 줄이다. 남은 것은 스레드가 돌아가는 일뿐이라 곧 끝난다.
    thread_.join();
    return true;
}

bool ControlChannel::shutdown_signaled() const noexcept {
    const platform::WaitHandle handles[] = {shutdown_event_};
    const auto r = platform::wait_any(std::span<const platform::WaitHandle>(handles, 1), 0);
    // 대기가 실패하면 종료로 본다. 그 핸들로는 종료를 알 수 없게 됐다.
    return r.status != platform::WaitStatus::Timeout;
}

void ControlChannel::run(std::string host, std::uint16_t port) {
    bool resolved_set = false;
    try {
        // DNS 는 기동 시 한 번이다 (control_plane.md 8.2). 결과가 이 스레드의 유일한
        // 상태다 (concurrency.md 8장).
        const std::optional<network::Endpoint> server = platform::resolve_ipv4(host, port);
        resolved_promise_.set_value(server);
        resolved_set = true;
        if (server) {
            serve(*server);
        }
    } catch (const std::exception&) {
        // 예외로 끝나면 ERROR 한 줄을 내고 종료 이벤트를 신호한다. 문서가 정했다
        // (concurrency.md 8장 `[control]` 스레드). 잡지 않으면 정리 없이 프로세스가 끝난다.
        if (!resolved_set) {
            try {
                resolved_promise_.set_value(std::nullopt);
            } catch (const std::exception&) {
            }
        }
        const LogField fields[] = {field("reason", std::string_view("exception"))};
        emit(LogLevel::Error, "control.thread", fields);
        (void)platform::signal_event(shutdown_event_);
    }
    // 마지막 줄이다. 이 뒤에 이 객체를 건드리지 않는다.
    (void)platform::signal_event(done_event_);
}

void ControlChannel::serve(const network::Endpoint& server) {
    // 순서가 곧 우선순위다. 둘이 같이 신호돼 있으면 종료가 먼저 보인다.
    const platform::WaitHandle handles[] = {shutdown_event_, request_event_};
    constexpr std::size_t kShutdownIndex = 0;

    while (true) {
        const auto r = platform::wait_any(std::span<const platform::WaitHandle>(handles, 2),
                                          platform::kInfiniteWaitMs);
        if (r.status == platform::WaitStatus::Failed) {
            // 대기가 실패하면 ERROR 한 줄을 내고 종료 이벤트를 신호한다. 문서가 정했다
            // (concurrency.md 8장 `[control]` 스레드). 조용히 끝내면 `[loop]` 의 시도가 마감까지 멈춘다.
            const LogField fields[] = {
                field("op", std::string_view("WaitForMultipleObjects")),
                field("code", static_cast<std::uint64_t>(r.error)),
            };
            emit(LogLevel::Error, "socket.error", fields);
            (void)platform::signal_event(shutdown_event_);
            return;
        }
        if (r.status == platform::WaitStatus::Signaled && r.index == kShutdownIndex) {
            return;
        }

        // 요청 이벤트는 자동 리셋이다. 깨어난 김에 링을 비운다.
        while (auto request = requests_.try_pop()) {
            if (shutdown_signaled()) {
                return;
            }
            ExchangeResult result = exchange(server, request->bytes, shutdown_event_, timeouts_);
            if (result.status == ExchangeStatus::kAborted) {
                // 종료다. 응답을 기다리지 않고 돌아간다. 소켓은 exchange 가 닫았다.
                return;
            }
            if (!deliver(ControlResponse{request->op, request->self_peer_id, std::move(result)})) {
                return;
            }
        }
    }
}

bool ControlChannel::deliver(ControlResponse response) {
    // 응답 링이 차면 버리지 않고 자리가 날 때까지 기다린다. 기다리는 동안에도 종료를 본다.
    // 문서가 정했다 (concurrency.md 8장 `[control]` 스레드). 미결 요청이 늘 하나라 정상 경로에서는
    // 차지 않는다. 그동안 요청 링이 차면 `[loop]` 의 submit 이 control_queue_dropped 로 드러낸다.
    while (!responses_.try_push(std::move(response))) {
        const platform::WaitHandle handles[] = {shutdown_event_};
        const auto r = platform::wait_any(std::span<const platform::WaitHandle>(handles, 1),
                                          platform::kAbortPollMs);
        if (r.status != platform::WaitStatus::Timeout) {
            return false;
        }
    }
    (void)platform::signal_event(response_event_);
    return true;
}

}  // namespace sangtachi::control
