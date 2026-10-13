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
// | OpenSession | 터널의 create_session. 널이면 Lobby::on_session_open_failed. 만들었으면 그 쌍의 기록 줄을 맡는다(아래) |
// | StartPunch | 그 상대의 세션이 있으면 start_punch |
// | LeaveRoom | 터널의 leave_room |
// | StartRoster / SetRosterPeers | 터널의 start_roster / set_roster_peers |
// | RecordAttempt | 기록 함수의 수립 줄 |
//
// ## 세션의 전이 (tunnel/tunnel.hpp set_transition_handler)
//
// `on_session_transition` 은 터널의 전이 콜백이다. **값을 베껴 큐에 넣기만 한다.** 그 자리는 세션의
// 메서드 안이라 거기서 Lobby 의 행동(LeaveRoom 등)을 하면 지우는 중인 세션을 다시 만진다. 큐는
// `flush` 가 비운다. [loop] 은 터널을 부른 뒤마다(수신, 타이머, 종료) `flush` 를 부른다. 이 객체가 행동을
// 하다 생긴 전이(LeaveRoom)는 그 행동 목록을 다 한 뒤에 이 객체가 스스로 비운다.
//
// 전이 하나마다 기록 줄을 먼저 쓰고 Lobby::on_session_event 를 부른다.
//
// ## 기록 줄 (architecture.md 9장 로컬 기록 파일)
//
// OpenSession 으로 만든 세션마다 쌍 하나를 기억한다. 세션 없이 끝난 시도의 줄은 Lobby 가 RecordAttempt 로 낸다.
//
// | 전이 | 줄 |
// |------|-----|
// | 처음 `CONNECTED` 에 들어감 | 수립 줄 `OK`. `setup_ms` 는 전이 시각에서 OpenSession 의 기점을 뺀 값. `rtt_us` 는 그때의 마지막 표본 |
// | 종료 상태. `CONNECTED` 에 닿은 적이 있다 | 종료 줄. 그 수립 줄의 시도 번호 |
// | 종료 상태. 닿은 적이 없다 | 수립 줄 하나 |
//
// 결과 값은 `record_result` 의 표다. 다시 `CONNECTED` 에 들어가도(재협상 뒤) 수립 줄을 다시 쓰지 않는다.
//
// - 행동은 받은 순서대로 한다. 실행 중에 생긴 사건(STUN 이 그 자리에서 끝남, 요청 큐가 참)의
//   행동은 지금 목록의 뒤에 붙는다. Lobby 의 함수를 다시 들어가지 않는다
// - STUN 이 끝났는지는 StunClient 를 부른 뒤마다 본다. 끝난 것을 처음 본 때 한 번
//   Lobby::on_stun_done 을 부른다. 끝난 객체는 다음 StartStun 이나 CancelStun 까지 둔다. 재시도가
//   부른 늦은 응답을 그 객체가 세지 않고 버리게 하려는 것이다 (stun_client.hpp 의 표)
// - 제어 응답은 op 로 interpret_* 를 고르고, get_peers 와 host_report 에는 요청에 실었던
//   self_peer_id 와 응답을 해석하는 시점의 자기 서브넷 집합(RunnerDeps::own_subnets)을 넣는다.
//   전송 오류면 ops::transport_failure 다
// - **`control.result` 는 Lobby 가 LogLine 으로 낸다.** 여기서 내지 않는다. 여기서 내는 것은 전송
//   오류의 이유 한 줄(`control.transport`)뿐이다. 그 줄은 architecture.md 9장의 고정 키가 아니다

#include "sangtachi/control/channel.hpp"
#include "sangtachi/control/lobby.hpp"
#include "sangtachi/counters.hpp"
#include "sangtachi/network/endpoint.hpp"
#include "sangtachi/network/stun_client.hpp"
#include "sangtachi/peer/candidates.hpp"
#include "sangtachi/peer/session.hpp"
#include "sangtachi/timer.hpp"
#include "sangtachi/tunnel/tunnel.hpp"

#include <cstddef>
#include <deque>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
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

// 자기 서브넷 집합을 읽는다. 돌려준 참조는 그 호출 안에서만 쓴다.
using OwnSubnetsFn = std::function<const peer::OwnSubnets&()>;

// 로컬 기록 파일 (architecture.md 9장). 제품 경로는 record 모듈의 RecordWriter 에 잇는다. 비우면 쓰지 않는다.
struct RecordSink {
    // 수립 줄. 새 시도 번호를 돌려준다.
    std::function<std::uint64_t(std::string_view role, std::optional<std::uint32_t> peer,
                                std::string_view result, std::optional<std::uint64_t> rtt_us,
                                std::optional<std::uint64_t> setup_ms)>
        establish;
    // 종료 줄. attempt 는 그 세션의 수립 줄 번호다.
    std::function<void(std::uint64_t attempt, std::string_view role, std::optional<std::uint32_t> peer,
                       std::string_view result, std::optional<std::uint64_t> rtt_us)>
        end;
};

// 세션 전이 하나의 기록 줄 `result` (architecture.md 9장 결과 값). connected 는 그 시도가 `CONNECTED` 에
// 한 번이라도 닿았는가다.
//
// | 전이 | 닿기 전 (수립 줄) | 닿은 뒤 (종료 줄) |
// |------|-------------------|-------------------|
// | `FAILED` | 그 실패 코드 | 그 실패 코드 |
// | 상대의 `CLOSE` 0x02, 0x03 | `PEER_FAILED` | `PEER_FAILED` |
// | 상대의 `CLOSE` 0x00, 0x01 | `ABORTED` | `CLOSED` |
// | 이쪽이 닫음 (`leave`, 종료 이벤트) | `ABORTED` | `CLOSED` |
[[nodiscard]] std::string_view record_result(const SessionEvent& event, bool connected) noexcept;

struct RunnerDeps {
    std::string host_header;                         // http::host_header 가 만든 값
    std::vector<network::StunServer> stun_servers;   // 기동 시 한 번 해석한 목록
    NonceSource nonce;                               // 시도마다 새 client_nonce
    std::string name;                                // 로비의 처음 표시 이름. 제품 경로는 --name (architecture.md 3.5)
    SubmitFn submit;
    network::StunSendFn send;                        // 루프의 소켓으로 보낸다
    PrintFn print;
    ClockFn clock;
    network::StunRandomFn stun_random;               // 비워 두면 platform::random_bytes
    // 지금의 자기 서브넷 집합 (protocol.md 10.1). get_peers 와 host_report 의 상대 후보 위생에 넘긴다.
    // 집합의 소유는 [loop] 다 (concurrency.md 5장). 제품 경로는 EventLoop::own_subnets 를 읽는다.
    // 비워 두면 빈 집합이다 (10.1 "첫 수집 전에는 비어 있다").
    OwnSubnetsFn own_subnets;

    // 로컬 후보 수집 (protocol.md 10.1). sources 의 두 함수가 다 있어야 수집한다. 없으면 반사 후보만
    // 등록한다. 결과의 자기 서브넷 집합은 set_own_subnets 로 [loop] 에 넘긴다.
    peer::CandidateSources candidate_sources;
    std::uint16_t local_port = 0;                            // 6장 getsockname 의 포트
    std::function<void(peer::OwnSubnets)> set_own_subnets;   // 제품 경로는 EventLoop::set_own_subnets

    tunnel::Tunnel* tunnel = nullptr;  // 세션을 여는 곳. 널이면 OpenSession 이 실패한다
    RecordSink records;
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

    // 타이머 만료 하나. 자기 이름(Lobby 의 것, 지금 StunClient 의 것)이 아니면 무시한다.
    void on_timer(std::string_view name);

    // 터널의 전이 콜백. 큐에 넣기만 한다 (위 "세션의 전이").
    void on_session_transition(const peer::Session& session, peer::SessionState from,
                               peer::SessionState to);

    // 쌓인 전이를 처리한다. 터널을 부른 뒤마다 부른다.
    void flush();

    // 종료 이벤트 (concurrency.md 7장 종료). Lobby::on_shutdown 을 한다. [loop] 은 이어서
    // Tunnel::close_all 과 flush 를 부른다.
    void shutdown();

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
    std::vector<Candidate> collect(std::span<const network::Endpoint> reflexive);
    void open_session(const OpenSession& open);
    void record(const SessionEvent& event, Millis at);

    struct Track {
        LobbyRole role = LobbyRole::kNone;
        Millis setup_base_ms = 0;
        bool connected = false;
        std::uint64_t attempt = 0;
    };

    struct QueuedEvent {
        SessionEvent event;
        Millis at = 0;
    };

    RunnerDeps deps_;
    TimerSet& timers_;
    Counters& counters_;
    Lobby lobby_;

    std::unique_ptr<network::StunClient> stun_;
    bool stun_reported_ = false;

    std::deque<LobbyAction> queue_;
    std::deque<QueuedEvent> events_;
    std::map<std::uint32_t, Track> tracks_;  // 기록 줄을 맡은 쌍. 키는 상대 peer_id
    bool running_ = false;
};

}  // namespace sangtachi::control
