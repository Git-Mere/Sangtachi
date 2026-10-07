#pragma once

// 제어 평면 요청 한 건을 주고받는다 (control_plane.md 3.3 HTTP 부분집합, 3.5 클라이언트가 하는
// 검사, 8.2 시간 제한).
//
// `[control]` 스레드가 부른다 (concurrency.md 8장 `[control]` 스레드). **블로킹이다.** `[loop]`
// 에서 부르지 않는다.
//
// ## 한 건의 순서
//
// | # | 하는 일 | 걸리면 |
// |---|---------|--------|
// | 1 | 새 TCP 소켓으로 connect. 논블로킹 + select, timeouts.connect_ms | kConnectFailed, kConnectTimeout |
// | 2 | 요청 바이트를 전부 보낸다. `SO_SNDTIMEO` = timeouts.io_ms | kSendFailed, kSendTimeout |
// | 3 | http::frame_response 가 kComplete 라고 할 때까지 받는다. 수신 단계 전체가 timeouts.io_ms 안이다 | kRecvFailed, kRecvTimeout, kClosedEarly, kBadResponse |
// | 4 | 소켓을 닫는다. 어느 경로든 닫는다 | - |
//
// 1 과 3 에서는 중단 이벤트를 보면 kAborted 로 끝난다. 2 의 송신은 중단 이벤트를 보지 않고
// send 호출마다 `SO_SNDTIMEO` 까지 늦을 수 있다 (platform/tcp.hpp, concurrency.md 8장의 종료 행).
//
// **3 은 EOF 를 기다리지 않는다.** `Content-Length` 만큼 오면 멈춘다(3.5 "서버가 연결을 닫기를
// 기다리지 않는다"). 머리가 다 오기 전에는 kMaxHeaderBytes 까지만, 머리가 온 뒤에는 남은 본문
// 만큼만 읽는다. 그래서 받는 양이 kMaxHeaderBytes + kMaxBodyBytes 를 넘지 않는다. 더 온 바이트가
// 같은 조각에 섞여 들어오면 경계에서 자른다.
//
// **수신 단계 전체를 io_ms 로 묶는다.** 문서가 정했다 (control_plane.md 8.2 시간 제한: "수신은
// 단계 전체에 3초 마감을 두고 남은 시간만큼 select 로 기다린다. SO_RCVTIMEO 도 건다"). recv 한
// 번에 걸리는 `SO_RCVTIMEO` 만으로는 바이트를 조금씩 흘리는 상대 앞에서 그 상한을 지키지 못한다.
// 송신은 `SO_SNDTIMEO` 라 호출별이다(같은 절).
//
// 결과는 바이트다. 해석(봉투, 연산별 형, 자신의 peer_id 검사)은 `[loop]` 가 ops.hpp 의
// interpret_* 로 한다. 해석에 세션 상태(자신의 peer_id)가 들고, 그 상태는 `[loop]` 가 갖는다
// (concurrency.md 5장 상태 소유).

#include "sangtachi/control/constants.hpp"
#include "sangtachi/control/http.hpp"
#include "sangtachi/network/endpoint.hpp"
#include "sangtachi/platform/wait.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace sangtachi::control {

// control_plane.md 8.2 시간 제한. 시험이 짧은 값을 넣을 수 있게 인자로 받는다.
struct ExchangeTimeouts {
    std::uint32_t connect_ms = kClientConnectTimeoutS * 1000;  // CLIENT_CONNECT_TIMEOUT_S
    std::uint32_t io_ms = kClientIoTimeoutS * 1000;            // CLIENT_IO_TIMEOUT_S
};

enum class ExchangeStatus : std::uint8_t {
    kComplete,        // 응답 하나가 경계까지 왔다
    kSocketError,     // 소켓을 만들거나 옵션을 걸지 못했다
    kConnectFailed,   // 거부, 도달 불가
    kConnectTimeout,
    kSendFailed,
    kSendTimeout,
    kRecvFailed,
    kRecvTimeout,
    kClosedEarly,     // 경계 전에 상대가 닫았다
    kBadResponse,     // 3.5 의 1~4 에 걸렸다. frame_error 가 이유다
    kAborted,         // 중단 이벤트를 봤다
};

// 로그가 싣는 고정 토큰. 문서에 없는 값이라 이 코드가 정했다. architecture.md 9장의
// control.result 는 이것을 싣지 않는다. 거기서는 전부 `transport` 다.
[[nodiscard]] std::string_view to_token(ExchangeStatus status) noexcept;

struct ExchangeResult {
    ExchangeStatus status = ExchangeStatus::kSocketError;
    std::uint32_t os_error = 0;                                  // OS 오류 코드가 있을 때만
    http::ResponseError frame_error = http::ResponseError::kNone;  // kBadResponse 일 때만
    std::string response;  // kComplete 일 때만. 머리와 본문, 그 뒤의 바이트는 없다

    [[nodiscard]] bool complete() const noexcept { return status == ExchangeStatus::kComplete; }
};

// 요청 한 건. request 는 http::build_request 가 만든 바이트 그대로다.
//
// abort 는 종료 이벤트다. 널이면 중단하지 않는다.
[[nodiscard]] ExchangeResult exchange(const network::Endpoint& server, std::string_view request,
                                      platform::WaitHandle abort,
                                      const ExchangeTimeouts& timeouts = {});

}  // namespace sangtachi::control
