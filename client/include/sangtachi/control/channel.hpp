#pragma once

// `[control]` 스레드와 두 큐 (concurrency.md 8장 `[control]` 스레드, control_plane.md 8장
// 클라이언트 쪽 계약).
//
// 8장의 표를 그대로 옮긴다.
//
// | 항목 | 이 파일 |
// |------|---------|
// | 큐 | 요청 링, 응답 링. 각각 SpscRing 8 항목 (spsc_ring.hpp) |
// | 가득 찼을 때 | submit 이 거짓을 돌려주고 control_queue_dropped 를 올린다. 시도를 끝내는 것은 부르는 쪽이다 |
// | 미결 요청 | 한 번에 하나는 `[loop]` 쪽 규칙이다. 이 객체는 강제하지 않는다. 강제하면 위 행의 경로가 죽는다 |
// | 깨우기 | `[control]` 은 종료 이벤트와 요청 이벤트(자동 리셋)를 기다린다. `[loop]` 는 응답 이벤트(자동 리셋, 2장 대기의 순위 4)로 깨어난다 |
// | 소켓 | 요청마다 새 TCP 소켓. control/exchange.hpp |
// | DNS | 스레드가 시작하자마자 한 번. 결과 IPv4 하나가 이 스레드의 유일한 상태다 |
// | 종료 | 종료 이벤트를 보면 진행 중인 요청을 버리고 소켓을 닫고 돌아온다 |
//
// **`[control]` 은 세션 상태를 읽지도 쓰지도 않는다.** 요청 큐에서 꺼낸 바이트를 보내고 받은
// 바이트를 응답 큐에 넣는다. 해석은 `[loop]` 가 ops.hpp 로 한다.
//
// ## 스레드 경계
//
// | 함수 | 부르는 스레드 |
// |------|---------------|
// | start, wait_resolved | 기동 중의 주 스레드. `[loop]` 가 돌기 전이다 |
// | submit, try_pop, response_event | `[loop]` 만 |
// | join_for, 소멸자 | 종료 중의 주 스레드. `[loop]` 가 끝난 뒤다 |
//
// **Winsock 참조를 하나 따로 든다.** concurrency.md 7장 종료는 정리 완료 신호(7) 뒤에 join(8)
// 을 둔다. main 의 Winsock 은 정리 완료보다 먼저 해제되므로, 이 객체가 자기 참조를 들지 않으면
// join 을 기다리는 동안 `[control]` 의 소켓 밑에서 Winsock 이 사라진다. WSAStartup 은 참조를
// 세므로 둘이 들면 마지막 것이 해제할 때 끝난다.
//
// **종료 이벤트를 복제해 든다.** 원본은 EventLoop 가 소유하고 루프가 먼저 소멸한다. 다른
// 스레드가 기다리는 핸들을 닫으면 그 대기는 정의되지 않는다 (platform/wait.hpp duplicate_event).

#include "sangtachi/control/exchange.hpp"
#include "sangtachi/control/http.hpp"
#include "sangtachi/counters.hpp"
#include "sangtachi/network/endpoint.hpp"
#include "sangtachi/network/wsa.hpp"
#include "sangtachi/platform/wait.hpp"
#include "sangtachi/spsc_ring.hpp"

#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace sangtachi::control {

// concurrency.md 8장 `[control]` 스레드의 큐 행. 두 큐가 같은 크기다.
inline constexpr std::size_t kControlQueueCapacity = 8;

// `[loop]` 가 넣는 요청. bytes 는 http::build_request 가 만든 것 그대로다.
//
// 바이트를 `[loop]` 가 만드는 이유는 본문에 세션 상태(peer_id, peer_token)가 들기 때문이다.
// 그 상태는 `[loop]` 가 갖는다 (concurrency.md 5장 상태 소유).
struct ControlRequest {
    http::Op op = http::Op::kCreateRoom;
    std::string bytes;
    // 응답 해석(ops::interpret_get_peers, interpret_host_report)의 인자. `[control]` 은 보지 않고
    // 응답에 그대로 되실어 보낸다. 그래서 `[loop]` 는 미결 요청을 따로 기억하지 않아도 된다.
    std::uint32_t self_peer_id = 0;
};

// `[control]` 이 돌려주는 응답. result 가 complete 이면 response 에 응답 바이트가 있고, 아니면
// 전송 오류다 (control_plane.md 3.5, 8.3). 부르는 쪽은 그때 ops::transport_failure 를 쓴다.
struct ControlResponse {
    http::Op op = http::Op::kCreateRoom;
    std::uint32_t self_peer_id = 0;  // 요청의 값 그대로
    ExchangeResult result;
};

class ControlChannel {
public:
    // `[control]` 을 띄운다. 그 스레드는 먼저 host 를 한 번 해석한다 (platform::resolve_ipv4.
    // 숫자 모양의 이름을 묻지 않는 규칙이 거기 있다). 결과는 wait_resolved 로 받는다.
    //
    // shutdown_event 는 복제해서 든다. 이벤트나 스레드를 만들지 못하면 널이다.
    // Winsock 초기화가 실패하면 network::WsaStartupError 를 던진다.
    [[nodiscard]] static std::unique_ptr<ControlChannel> start(std::string host,
                                                               std::uint16_t port,
                                                               platform::WaitHandle shutdown_event,
                                                               Counters& counters,
                                                               const ExchangeTimeouts& timeouts = {});

    ~ControlChannel();

    ControlChannel(const ControlChannel&) = delete;
    ControlChannel& operator=(const ControlChannel&) = delete;
    ControlChannel(ControlChannel&&) = delete;
    ControlChannel& operator=(ControlChannel&&) = delete;

    // 해석이 끝날 때까지 기다린다. 값이 없으면 해석에 실패한 것이고 그것은 기동 실패다
    // (control_plane.md 8.2). 그때 `[control]` 은 이미 끝났다. 한 번만 부른다.
    //
    // DNS 에는 별도 시간 제한을 걸지 않는다 (8.2). OS 기본만큼 기다린다.
    [[nodiscard]] std::optional<network::Endpoint> wait_resolved();

    // 요청을 넣고 요청 이벤트를 신호한다. `[loop]` 만 부른다.
    //
    // 링이 가득 차면 버리고 control_queue_dropped 를 올린 뒤 거짓을 돌려준다. 그 시도를
    // CONTROL_PLANE_EXCHANGE_FAILED 로 끝내는 것은 부르는 쪽이다 (concurrency.md 8장).
    [[nodiscard]] bool submit(ControlRequest request);

    // 응답을 하나 꺼낸다. 없으면 값이 없다. `[loop]` 만 부른다.
    [[nodiscard]] std::optional<ControlResponse> try_pop();

    // `[loop]` 의 대기 집합 순위 4 (concurrency.md 2장 대기). 자동 리셋이다. 소유는 이 객체가 한다.
    [[nodiscard]] platform::WaitHandle response_event() const noexcept { return response_event_; }

    // `[control]` 이 끝나기를 timeout_ms 까지 기다린다. 끝났으면 join 하고 참이다.
    //
    // 종료 이벤트가 신호된 뒤에 부른다. 거짓이면 그 스레드는 아직 요청 하나를 붙들고 있다.
    // concurrency.md 7장 종료는 그때 `_exit` 로 끝낸다. 그 판단은 main 이 한다.
    [[nodiscard]] bool join_for(std::uint32_t timeout_ms);

private:
    ControlChannel(Counters& counters, const ExchangeTimeouts& timeouts);

    void run(std::string host, std::uint16_t port);
    void serve(const network::Endpoint& server);
    // 응답 링이 차 있으면 자리가 날 때까지 기다린다. 종료를 보면 거짓이다.
    bool deliver(ControlResponse response);
    [[nodiscard]] bool shutdown_signaled() const noexcept;

    // 소멸 순서가 뜻을 갖는다. Winsock 참조가 맨 먼저 생기고 맨 나중에 사라진다.
    network::WsaContext wsa_;
    Counters& counters_;  // `[loop]` 소유. submit 에서만 만진다
    ExchangeTimeouts timeouts_;

    platform::WaitHandle shutdown_event_ = nullptr;  // 복제본. 이 객체가 닫는다
    platform::WaitHandle request_event_ = nullptr;   // 자동 리셋. `[loop]` 가 신호한다
    platform::WaitHandle response_event_ = nullptr;  // 자동 리셋. `[control]` 이 신호한다
    platform::WaitHandle done_event_ = nullptr;      // 수동 리셋. `[control]` 이 끝날 때 신호한다

    SpscRing<ControlRequest, kControlQueueCapacity> requests_;
    SpscRing<ControlResponse, kControlQueueCapacity> responses_;

    std::promise<std::optional<network::Endpoint>> resolved_promise_;
    std::future<std::optional<network::Endpoint>> resolved_;

    std::thread thread_;
};

}  // namespace sangtachi::control
