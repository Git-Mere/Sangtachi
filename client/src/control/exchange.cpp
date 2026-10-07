#include "sangtachi/control/exchange.hpp"

#include "sangtachi/control/constants.hpp"
#include "sangtachi/control/http.hpp"
#include "sangtachi/platform/tcp.hpp"
#include "sangtachi/platform/wait.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace sangtachi::control {
namespace {

using platform::TcpStatus;

// 한 번에 받는 양의 상한. 응답 전체가 머리 2048 + 본문 4096 이하라 몇 번이면 끝난다.
constexpr std::size_t kChunkBytes = 2048;

ExchangeResult fail(ExchangeStatus status, std::uint32_t os_error = 0) {
    ExchangeResult r;
    r.status = status;
    r.os_error = os_error;
    return r;
}

}  // namespace

std::string_view to_token(ExchangeStatus status) noexcept {
    switch (status) {
        case ExchangeStatus::kComplete:       return "complete";
        case ExchangeStatus::kSocketError:    return "socket_error";
        case ExchangeStatus::kConnectFailed:  return "connect_failed";
        case ExchangeStatus::kConnectTimeout: return "connect_timeout";
        case ExchangeStatus::kSendFailed:     return "send_failed";
        case ExchangeStatus::kSendTimeout:    return "send_timeout";
        case ExchangeStatus::kRecvFailed:     return "recv_failed";
        case ExchangeStatus::kRecvTimeout:    return "recv_timeout";
        case ExchangeStatus::kClosedEarly:    return "closed_early";
        case ExchangeStatus::kBadResponse:    return "bad_response";
        case ExchangeStatus::kAborted:        return "aborted";
    }
    return "unknown";
}

ExchangeResult exchange(const network::Endpoint& server, std::string_view request,
                        platform::WaitHandle abort, const ExchangeTimeouts& timeouts) {
    // 1. connect. 실패한 경로는 connect_tcp 가 소켓을 닫고 돌아온다.
    auto connected = platform::connect_tcp(server, timeouts.connect_ms, timeouts.io_ms, abort);
    if (!connected.stream) {
        switch (connected.result.status) {
            case TcpStatus::kRefused:  return fail(ExchangeStatus::kConnectFailed, connected.result.error);
            case TcpStatus::kTimedOut: return fail(ExchangeStatus::kConnectTimeout);
            case TcpStatus::kAborted:  return fail(ExchangeStatus::kAborted);
            case TcpStatus::kOk:
            case TcpStatus::kClosed:
            case TcpStatus::kError:
                break;
        }
        return fail(ExchangeStatus::kSocketError, connected.result.error);
    }
    // 4. 이 뒤의 모든 경로에서 stream 의 소멸자가 소켓을 닫는다.
    platform::TcpStream& stream = *connected.stream;

    // 2. 송신.
    const auto sent = stream.send_all(std::as_bytes(std::span(request.data(), request.size())));
    if (sent.status == TcpStatus::kTimedOut) {
        return fail(ExchangeStatus::kSendTimeout);
    }
    if (sent.status != TcpStatus::kOk) {
        return fail(ExchangeStatus::kSendFailed, sent.error);
    }

    // 3. 수신. 단계 전체가 io_ms 안이다.
    const platform::WaitMillis start = platform::monotonic_ms();
    std::string received;
    std::array<std::byte, kChunkBytes> chunk{};
    while (true) {
        const http::Frame frame = http::frame_response(received);
        if (frame.state == http::FrameState::kComplete) {
            // 경계 뒤의 바이트는 보지 않는다 (3.5).
            received.resize(frame.head_size + frame.body_size);
            ExchangeResult ok;
            ok.status = ExchangeStatus::kComplete;
            ok.response = std::move(received);
            return ok;
        }
        if (frame.state == http::FrameState::kInvalid) {
            ExchangeResult bad = fail(ExchangeStatus::kBadResponse);
            bad.frame_error = frame.error;
            return bad;
        }

        // 머리가 다 왔으면 남은 본문만큼, 아니면 머리 상한까지만 읽는다.
        const std::size_t want = frame.needed != 0
                                     ? frame.needed
                                     : (received.size() < kMaxHeaderBytes
                                            ? kMaxHeaderBytes - received.size()
                                            : 0);
        if (want == 0) {
            // frame_response 가 상한에 닿은 머리를 kInvalid 로 돌려주므로 오지 않는 자리다.
            // 와도 더 읽지 않는다.
            ExchangeResult bad = fail(ExchangeStatus::kBadResponse);
            bad.frame_error = http::ResponseError::kHeaderTooLarge;
            return bad;
        }

        const platform::WaitMillis now = platform::monotonic_ms();
        const platform::WaitMillis elapsed = now >= start ? now - start : 0;
        if (elapsed >= timeouts.io_ms) {
            return fail(ExchangeStatus::kRecvTimeout);
        }
        const auto remaining = static_cast<std::uint32_t>(timeouts.io_ms - elapsed);

        const auto ready = stream.wait_readable(remaining, abort);
        if (ready.status == TcpStatus::kAborted) {
            return fail(ExchangeStatus::kAborted);
        }
        if (ready.status == TcpStatus::kTimedOut) {
            return fail(ExchangeStatus::kRecvTimeout);
        }
        if (ready.status != TcpStatus::kOk) {
            return fail(ExchangeStatus::kRecvFailed, ready.error);
        }

        const std::size_t take = std::min(want, chunk.size());
        const auto got = stream.recv_some(std::span<std::byte>(chunk.data(), take));
        switch (got.status) {
            case TcpStatus::kOk:
                received.append(reinterpret_cast<const char*>(chunk.data()), got.bytes);
                break;
            case TcpStatus::kClosed:
                return fail(ExchangeStatus::kClosedEarly);
            case TcpStatus::kTimedOut:
                return fail(ExchangeStatus::kRecvTimeout);
            case TcpStatus::kAborted:
                return fail(ExchangeStatus::kAborted);
            case TcpStatus::kRefused:
            case TcpStatus::kError:
                return fail(ExchangeStatus::kRecvFailed, got.error);
        }
    }
}

}  // namespace sangtachi::control
