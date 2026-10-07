#pragma once

// 로비 상태 기계의 행동 실행기 (control/lobby.hpp 머리의 "통합 계약").
//
// Lobby 는 순수 코드라 할 일의 목록만 돌려준다. 이 객체가 그 목록을 `[loop]` 위에서 실제로 한다.
// **`[loop]` 만 부른다** (concurrency.md 5장 상태 소유). 요청 큐에 넣는 것과 STUN 송신과 타이머가
// 전부 `[loop]` 의 것이다.
//
// | 행동 | 이 객체가 하는 일 |
// |------|------------------|
// | SubmitRequest | http::build_request(Host 헤더, op, body) 를 만들어 submit 함수로 넣는다. 넣지 못하면 Lobby::on_submit_dropped. 카운터는 채널이 올린다 |
// | StartStun | 기동 시 해석한 서버 목록으로 **새** StunClient 를 만들어 start 한다. 앞 객체는 타이머를 지우고 버린다 |
// | CancelStun | 그 StunClient 의 타이머를 지우고 버린다 |
// | ArmTimer / CancelTimer | 루프의 TimerSet 에 add_once / cancel |
// | PrintLine | print 함수 (제품 경로는 platform::write_stdout_line) |
// | LogLine | emit |
//
// - 행동은 받은 순서대로 한다. 실행 중에 생긴 사건(STUN 이 그 자리에서 끝남, 요청 큐가 참)의
//   행동은 지금 목록의 뒤에 붙는다. Lobby 의 함수를 다시 들어가지 않는다
// - STUN 이 끝났는지는 StunClient 를 부른 뒤마다 본다. 끝난 것을 처음 본 때 한 번
//   Lobby::on_stun_done 을 부른다. 끝난 객체는 다음 StartStun 이나 CancelStun 까지 둔다. 재시도가
//   부른 늦은 응답을 그 객체가 세지 않고 버리게 하려는 것이다 (stun_client.hpp 의 표)
// - 제어 응답은 op 로 interpret_* 를 고르고, get_peers 와 host_report 에는 요청에 실었던
//   self_peer_id 를 넣는다. 전송 오류면 ops::transport_failure 다
// - **`control.result` 는 Lobby 가 LogLine 으로 낸다.** 여기서 내지 않는다. 여기서 내는 것은 전송
//   오류의 이유 한 줄(`control.transport`)뿐이다. 그 줄은 architecture.md 9장의 고정 키가 아니다

#include "sangtachi/control/channel.hpp"
#include "sangtachi/control/lobby.hpp"
#include "sangtachi/counters.hpp"
#include "sangtachi/network/endpoint.hpp"
#include "sangtachi/network/stun_client.hpp"
#include "sangtachi/timer.hpp"

#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sangtachi::control {

// 요청 큐에 넣는다. 넣었으면 참이다. 제품 경로는 ControlChannel::submit 이다.
using SubmitFn = std::function<bool(ControlRequest)>;

// 표준 출력 한 줄. 줄바꿈은 받는 쪽이 붙인다.
using PrintFn = std::function<void(std::string_view)>;

// 시각. 제품 경로는 platform::monotonic_ms 다.
using ClockFn = std::function<Millis()>;

struct RunnerDeps {
    std::string host_header;                         // http::host_header 가 만든 값
    std::vector<network::StunServer> stun_servers;   // 기동 시 한 번 해석한 목록
    NonceSource nonce;                               // 시도마다 새 client_nonce
    SubmitFn submit;
    network::StunSendFn send;                        // 루프의 소켓으로 보낸다
    PrintFn print;
    ClockFn clock;
    network::StunRandomFn stun_random;               // 비워 두면 platform::random_bytes
};

class ControlRunner {
public:
    ControlRunner(RunnerDeps deps, TimerSet& timers, Counters& counters);
    ~ControlRunner();

    ControlRunner(const ControlRunner&) = delete;
    ControlRunner& operator=(const ControlRunner&) = delete;
    ControlRunner(ControlRunner&&) = delete;
    ControlRunner& operator=(ControlRunner&&) = delete;

    // 로비 명령 하나 (콘솔이나 기동 시 역할 인자).
    void on_command(const LobbyCommand& command);

    // drain_control 이 꺼낸 응답 하나.
    void on_response(const ControlResponse& response);

    // 루프가 STUN 으로 가른 데이터그램 하나 (protocol.md 7장 수신 분류).
    void on_stun_datagram(const network::Endpoint& from, std::span<const std::byte> payload);

    // 타이머 만료 하나. 자기 이름(Lobby 의 넷, 지금 StunClient 의 것)이 아니면 무시한다.
    void on_timer(std::string_view name);

    [[nodiscard]] const Lobby& lobby() const noexcept { return lobby_; }
    [[nodiscard]] const network::StunClient* stun() const noexcept { return stun_.get(); }

private:
    void run(LobbyActions actions);
    void execute(LobbyAction& action);
    void submit(SubmitRequest& request);
    void start_stun();
    void drop_stun();
    // StunClient 를 부른 뒤에 부른다. 처음 끝난 것을 보면 Lobby 에 알린다.
    void check_stun_done();

    RunnerDeps deps_;
    TimerSet& timers_;
    Counters& counters_;
    Lobby lobby_;

    std::unique_ptr<network::StunClient> stun_;
    bool stun_reported_ = false;

    std::deque<LobbyAction> queue_;
    bool running_ = false;
};

}  // namespace sangtachi::control
