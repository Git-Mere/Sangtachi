#include "sangtachi/telemetry/uploader.hpp"

#include "sangtachi/control/exchange.hpp"
#include "sangtachi/control/http.hpp"
#include "sangtachi/log.hpp"
#include "sangtachi/platform/resolve.hpp"
#include "sangtachi/platform/wait.hpp"

#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace sangtachi::telemetry {

Uploader::Uploader(std::string host, std::uint16_t port, std::string host_header, Counters& counters,
                   const control::ExchangeTimeouts& timeouts)
    : host_(std::move(host)),
      port_(port),
      host_header_(std::move(host_header)),
      counters_(counters),
      timeouts_(timeouts) {}

std::unique_ptr<Uploader> Uploader::start(std::string host, std::uint16_t port,
                                          platform::WaitHandle shutdown_event, Counters& counters,
                                          const control::ExchangeTimeouts& timeouts) {
    auto host_header = control::http::host_header(host, port);
    if (!host_header) {
        return nullptr;
    }
    std::unique_ptr<Uploader> uploader(
        new Uploader(std::move(host), port, std::move(*host_header), counters, timeouts));
    uploader->shutdown_event_ = platform::duplicate_event(shutdown_event);
    uploader->cleanup_event_ = platform::create_event(platform::ResetMode::Manual);
    uploader->done_event_ = platform::create_event(platform::ResetMode::Manual);
    if (uploader->shutdown_event_ == nullptr || uploader->cleanup_event_ == nullptr ||
        uploader->done_event_ == nullptr) {
        return nullptr;
    }
    try {
        Uploader* self = uploader.get();
        uploader->thread_ = std::thread([self] { self->run(); });
    } catch (const std::system_error&) {
        return nullptr;
    }
    return uploader;
}

Uploader::~Uploader() {
    if (thread_.joinable()) {
        // 시험은 여기로 끝낸다. 두 이벤트가 없으면 `[telemetry]` 가 끝나지 않는다.
        stop();
        thread_.join();
    }
    platform::close_event(done_event_);
    platform::close_event(cleanup_event_);
    platform::close_event(shutdown_event_);
}

bool Uploader::push(QueuedRecord record) {
    if (!queue_.try_push(std::move(record))) {
        counters_.increment(Counter::TelemetryQueueDropped);
        return false;
    }
    return true;
}

void Uploader::stop() {
    (void)platform::signal_event(shutdown_event_);
    (void)platform::signal_event(cleanup_event_);
}

void Uploader::cleanup_done() {
    (void)platform::signal_event(cleanup_event_);
}

bool Uploader::join_for(std::uint32_t timeout_ms) {
    if (!thread_.joinable()) {
        return true;
    }
    const platform::WaitHandle handles[] = {done_event_};
    const auto r = platform::wait_any(std::span<const platform::WaitHandle>(handles, 1), timeout_ms);
    if (r.status != platform::WaitStatus::Signaled) {
        return false;
    }
    thread_.join();
    return true;
}

bool Uploader::shutdown_signaled() const noexcept {
    const platform::WaitHandle handles[] = {shutdown_event_};
    const auto r = platform::wait_any(std::span<const platform::WaitHandle>(handles, 1), 0);
    return r.status != platform::WaitStatus::Timeout;
}

void Uploader::run() {
    try {
        const platform::WaitHandle shutdown[] = {shutdown_event_};
        // 종료 이벤트는 수동 리셋이라 한 번 신호되면 이 대기가 곧바로 돌아온다 (concurrency.md 7장).
        while (platform::wait_any(std::span<const platform::WaitHandle>(shutdown, 1), kWakeMs).status ==
               platform::WaitStatus::Timeout) {
            while (!shutdown_signaled()) {
                auto record = queue_.try_pop();
                if (!record) {
                    break;
                }
                upload(*record);
            }
        }
        // 종료 절차 (3)이 넣은 마지막 레코드는 (7) 정리 완료로 확정된다. 그 뒤 남은 것을 비운다. 얼마나 보내는지는
        // 주 스레드의 join 상한이 자른다 (concurrency.md 7장).
        const platform::WaitHandle cleanup[] = {cleanup_event_};
        (void)platform::wait_any(std::span<const platform::WaitHandle>(cleanup, 1), platform::kInfiniteWaitMs);
        while (auto record = queue_.try_pop()) {
            upload(*record);
        }
    } catch (const std::exception&) {
        const LogField fields[] = {field("reason", std::string_view("exception"))};
        emit(LogLevel::Error, "telemetry.thread", fields);
    }
    (void)platform::signal_event(done_event_);
}

void Uploader::upload(const QueuedRecord& record) {
    std::string_view reason;
    // 서비스 주소는 보낼 때마다 이 스레드에서 해석한다 (concurrency.md 6장).
    if (const auto server = platform::resolve_ipv4(host_, port_); !server) {
        reason = "resolve_failed";
    } else if (const auto request = control::http::build_request(host_header_, "report", record.body);
               !request) {
        reason = "bad_request";
    } else {
        const control::ExchangeResult result = control::exchange(*server, *request, nullptr, timeouts_);
        if (!result.complete()) {
            reason = control::to_token(result.status);
        } else {
            // control_plane.md 3.5 의 5, 6. 상태 코드로 가르지 않는다.
            const control::http::Response response = control::http::parse_response(result.response);
            const control::json::Value* ok = response.body ? response.body->find("ok") : nullptr;
            if (!response.ok()) {
                reason = "bad_response";
            } else if (ok == nullptr || ok->as_bool() != std::optional<bool>(true)) {
                reason = "not_ok";
            } else {
                return;
            }
        }
    }
    upload_failed_.fetch_add(1, std::memory_order_relaxed);
    const LogField fields[] = {
        field("reason", reason),
        field("kind", to_token(record.kind)),
    };
    emit(LogLevel::Warn, "telemetry.upload_failed", fields);
}

}  // namespace sangtachi::telemetry
