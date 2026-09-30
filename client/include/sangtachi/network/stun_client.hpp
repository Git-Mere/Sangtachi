#pragma once

// STUN 클라이언트. 재시도와 마감의 상태 기계다 (protocol.md 11장 타이머,
// architecture.md 3.5 기동 입력의 서버 선택).
//
// 메시지 구성과 파싱은 network/stun.hpp 가 갖는다. 이 파일은 **언제 보내고 언제 포기하고
// 어느 서버로 바꾸는가**만 정한다. 소켓도 시계도 여기 없다. 송신은 주입받은 함수로 하고
// 시각은 호출마다 인자로 받는다. 그래야 시험이 500ms 를 실제로 기다리지 않는다.
//
// ## 소켓을 직접 읽지 않는다
//
// protocol.md 6장 소켓 소유권: 소켓 하나를 수신 루프 하나가 배타적으로 소유하고, STUN
// 클라이언트는 recvfrom 을 부르지 않는다. 그래서 이 객체는 루프에게 두 가지를 받는다.
//
//   1. 송신 함수 (StunSendFn). 루프의 소켓으로 나간다
//   2. 수신 전달. 루프가 protocol.md 7장 수신 분류에서 STUN 으로 가른 데이터그램을
//      on_datagram 으로 넘긴다
//
// 타이머도 루프의 집합(TimerSet)을 빌려 쓴다. 만료는 루프가 on_timer 로 넘긴다. 타이머를
// 따로 두지 않는 이유는 concurrency.md 3장 루프 한 바퀴가 타이머를 한 곳에서 돌리기
// 때문이다.
//
// ## 값의 출처
//
// | 값 | 출처 |
// |----|------|
// | 재시도 500ms, 1s, 2s (3회) | protocol.md 11장 타이머 |
// | 마감 5s, **서버별** | 같은 표 |
// | 앞 두 서버에 동시 질의 | architecture.md 3.5 기동 입력의 서버 선택 |
// | 서로 다른 두 서버의 응답을 못 얻으면 실패 | 같은 절 |
// | 단계 전체 상한 `5s x ceil(목록 길이 / 2)` | protocol.md 11장 |
// | 실패 코드 `STUN_DISCOVERY_FAILED` | architecture.md 8장 실패 진단 |
// | 로그 `stun.result` 의 필드 `server`, `mapped` | architecture.md 9장 |
// | 폐기 카운터 `drop_stun_parse` | protocol.md 13장 STUN 사용 범위 |
//
// ## 이 코드가 정한 것
//
// 문서가 이름만 적고 넘어간 자리다. 근거를 함께 적는다.
//
// | 자리 | 정한 것 | 왜 |
// |------|---------|-----|
// | 재시도 세 값의 뜻 | **간격이다.** 첫 요청을 0 에 보내고 500, 1500, 3500 에 다시 보낸다 | 500ms, 1s, 2s 는 RFC 5389 7.2.1 의 RTO 배증 수열이다. 절대 시각으로 읽으면 간격이 500, 500, 1000 이 되어 배증이 아니다. 어느 쪽이든 마감 5s 안에 3회가 들어간다 |
// | 타이머 이름 | `stun.retry.<서버 인덱스>` 와 `stun.deadline.<서버 인덱스>` | 문서에 없다. 서버마다 갈라야 한다 (timer.hpp 의 이름 유일성). 인덱스는 목록 안의 자리이고 한 자리는 많아야 한 번 질의하므로 이름이 겹치지 않는다 |
// | 자리를 다시 채우는 시점 | 마감 만료뿐 아니라 **응답으로 그 자리가 끝났을 때도** 채운다. 아직 응답이 둘이 안 됐고 남은 서버가 있으면 늘 둘을 띄운다 | 마감에서만 채우면 상한이 깨진다. 목록 4개에서 앞 서버가 1s 에 응답하고 뒤 서버가 5s 에 마감하면, 남은 둘을 한 자리로 차례로 쓰게 되어 끝이 15s 다. 상한은 10s 다 |
// | 오류 응답(`0x0111`) | 그 서버의 시도를 거기서 끝낸다. 재시도하지 않고 자리를 다음 서버에 넘긴다 | 서버가 요청을 거절한 것이므로 같은 요청을 다시 보내도 같은 답이다. 그 서버는 매핑을 주지 않는다 |
// | 대기 중인 요청에 없는 트랜잭션 ID | `drop_stun_parse` 로 센다 | protocol.md 13장 응답 검증의 "트랜잭션 ID 일치" 에 걸린 것이다. 이유를 코드별로 나누지 않는다 |
// | 이미 끝난 트랜잭션의 응답 | 세지 않고 버린다. **마감으로 끝난 자리도 같다** | 재시도를 보냈으면 응답이 여러 통 오는 것이 정상이고, 마감 직후에 도착하는 것도 정상이다. 그것을 폐기로 세면 정상 경로가 카운터를 올린다. 자리가 끝나는 경로 셋이 모두 `release` 를 지나게 해서 한 자리에서 기록한다 |
// | 응답과 요청을 잇는 것 | 트랜잭션 ID 하나 | protocol.md 7장 분류가 그렇게 적었다. 출발지 주소를 함께 보지 않는다. 96비트를 맞히지 못하면 끼어들 수 없고, 출발지는 어차피 위조할 수 있다 |
// | 목록이 두 개 미만 | 질의하지 않고 그 자리에서 실패한다 | 서로 다른 두 서버의 응답이 나올 수 없으므로 결과가 이미 정해졌다. 마감 5초를 기다려도 답이 바뀌지 않는다. architecture.md 3.5 기동 입력이 해석 실패로 목록이 줄어든 경우를 같은 규칙으로 적었다 |
// | `platform::random_bytes` 실패 | 그 자리에서 단계 전체를 `STUN_DISCOVERY_FAILED` 로 끝낸다 | 문서에 없다. 트랜잭션 ID 를 뽑지 못하면 보낼 수 있는 요청이 없다. 예측 가능한 값으로 대신하지 않는다 |
// | 송신 함수가 거짓을 돌려줌 | 자리를 유지하고 재시도 일정을 그대로 둔다 | 한 번의 sendto 실패는 그 서버의 실패가 아니다. 카운터와 로그는 루프가 이미 냈다 (protocol.md 6장) |
//
// ## 상한 산술
//
// `stun_stage_cap_ms` 가 그 값을 낸다. 자리가 둘이고 한 자리가 한 서버에 5s 를 쓰므로
// 서버는 늦어도 0s, 5s, 10s ... 에 짝으로 시작한다. 마지막 짝의 마감이 단계의 끝이다.

#include "sangtachi/counters.hpp"
#include "sangtachi/network/endpoint.hpp"
#include "sangtachi/network/stun.hpp"
#include "sangtachi/timer.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sangtachi::network {

// protocol.md 11장 타이머의 "STUN 재시도". 값은 간격이다 (위 표).
inline constexpr std::array<Millis, 3> kStunRetryIntervalsMs = {500, 1000, 2000};

// protocol.md 11장 타이머의 "STUN 마감". 서버별이다.
inline constexpr Millis kStunDeadlineMs = 5000;

// 동시에 질의하는 서버 수 (architecture.md 3.5 기동 입력의 서버 선택).
inline constexpr std::size_t kStunConcurrency = 2;

// 성공에 필요한 서로 다른 서버의 응답 수 (같은 절). 하나뿐이면 실패다.
inline constexpr std::size_t kStunRequiredResponses = 2;

// architecture.md 8장 실패 진단의 코드 문자열. 로그에 그대로 싣는다 (9장).
inline constexpr std::string_view kStunDiscoveryFailed = "STUN_DISCOVERY_FAILED";

// architecture.md 3.5 기동 입력의 STUN 기본 목록.
//
// **출처는 tools/nat-probe/natprobe.py 의 DEFAULT_STUN_SERVERS 다.** 그 문서가 두 곳의
// 값이 같아야 한다고 적었고, 여기가 세 번째 자리다. 한쪽을 바꾸면 나머지도 바꾼다.
struct StunServerName {
    std::string_view host;
    std::uint16_t port;
};

inline constexpr std::array<StunServerName, 4> kDefaultStunServers = {{
    {"stun.l.google.com", 19302},
    {"stun1.l.google.com", 19302},
    {"stun.cloudflare.com", 3478},
    {"stun.nextcloud.com", 3478},
}};

// 질의 대상 하나. label 은 사람이 적은 그대로이고 endpoint 는 기동 시 해석한 값이다
// (architecture.md 3.5: 이름 해석은 [loop] 시작 전에 한 번).
struct StunServer {
    std::string label;
    Endpoint endpoint;
};

// 이름 하나를 엔드포인트로 바꾼다. 값이 없으면 해석 실패다.
//
// **주입하는 이유는 시험이다.** 실제 DNS 에 기대면 오프라인에서 깨지고, NXDOMAIN 을
// 가로채는 망에서는 없는 이름이 주소를 받아 온다. 제품 경로는 platform::resolve_ipv4 다.
using StunResolveFn = std::function<std::optional<Endpoint>(std::string_view host,
                                                            std::uint16_t port)>;

// 기동 시 목록 전체를 해석하고 쓸 수 있는 것만 남긴다 (architecture.md 3.5 기동 입력).
//
// 순서가 정해져 있다. 그 절이 정한 그대로다.
//   1. 해석한다. 실패한 항목은 목록에서 빼고 `stun.unresolved` WARN 한 줄
//   2. **해석 결과가 같은 `IP:포트` 면 앞의 것 하나만 둔다.** 뺄 때마다 `stun.duplicate`
//      WARN 한 줄. 이름이 다르다고 서버가 다른 것이 아니다
//   3. "남은 목록이 두 개 미만이면 STUN_DISCOVERY_FAILED" 는 **그 뒤에** 본다. 그 판정은
//      StunClient::start 가 한다
//
// > **왜 2 가 필요한가.** 두 서버에 묻는 이유는 목적지가 달라져도 매핑이 그대로인지 보는
// > 것이다. 같은 주소에 두 번 물으면 목적지가 하나여서 그 판정이 성립하지 않는데, 목록
// > 항목 수만 보면 두 개로 보인다. 기본 목록의 앞 두 이름이 실제로 한 주소로 풀린 실측이
// > `tools/nat-probe/records/` 에 있다.
//
// **포트가 다르면 다른 서버다.** 같은 호스트의 다른 포트는 다른 목적지다.
[[nodiscard]] std::vector<StunServer> resolve_stun_servers(std::span<const StunServerName> list,
                                                           const StunResolveFn& resolve);

// 얻은 관측 하나. architecture.md 9장 stun.result 의 두 필드가 그대로 들어간다.
struct StunMapping {
    std::string server;
    Endpoint mapped;
};

enum class StunPhase {
    Idle,       // start 전
    Running,    // 질의 중
    Succeeded,  // 서로 다른 두 서버의 응답을 얻었다
    Failed,     // STUN_DISCOVERY_FAILED
};

// 루프의 소켓으로 한 통 보낸다. 참이면 보냈다 (protocol.md 6장 소켓 소유권).
using StunSendFn = std::function<bool(const Endpoint&, std::span<const std::byte>)>;

// 트랜잭션 ID 를 뽑는다. 참이면 out 전체를 채웠다.
//
// **주입하는 이유는 둘이다.** (1) 시험이 ID 를 고정해 "어느 서버의 응답인가" 를 확률이
// 아니라 값으로 판정한다. 진짜 난수를 쓰면 두 ID 가 겹치지 않는다는 것이 확률 명제가
// 된다. (2) 실패를 돌려주는 난수원으로 아래 "random_bytes 실패" 규칙을 시험할 수 있다.
// 비워 두면 platform::random_bytes 다 (protocol.md 13장 트랜잭션 ID: OS CSPRNG 96비트).
using StunRandomFn = std::function<bool(std::span<std::byte>)>;

// 단계 전체의 상한. `5s x ceil(서버 수 / 2)` 다 (protocol.md 11장 타이머).
[[nodiscard]] Millis stun_stage_cap_ms(std::size_t server_count) noexcept;

// architecture.md 9장의 session.failed 한 줄. code 는 8장 문자열 그대로다.
//
// 자리가 둘이라 한 곳에 둔다. 목록이 두 개 미만이라 시작조차 못 한 경우는 기동 코드가
// 내고 (architecture.md 3.5), 질의하다 실패한 경우는 이 객체가 낸다.
void emit_stun_discovery_failed();

class StunClient {
public:
    // servers 는 해석이 끝난 목록이다. timers 와 counters 는 루프의 것을 빌린다.
    // 이 객체는 루프보다 오래 살지 않는다.
    //
    // random 을 비워 두면 platform::random_bytes 를 쓴다. 제품 경로는 비워 둔다.
    StunClient(std::vector<StunServer> servers, StunSendFn send, TimerSet& timers,
               Counters& counters, StunRandomFn random = {});

    StunClient(const StunClient&) = delete;
    StunClient& operator=(const StunClient&) = delete;
    StunClient(StunClient&&) = delete;
    StunClient& operator=(StunClient&&) = delete;

    // 앞 두 서버에 질의를 시작한다. 이미 끝난 상태에서 다시 부르면 아무것도 하지 않는다.
    //
    // 참이면 질의가 돌기 시작했다. 거짓이면 그 자리에서 끝났다는 뜻이고 phase() 가
    // Failed 다 (목록이 비었거나 난수를 얻지 못했다).
    bool start(Millis now);

    // 루프가 STUN 으로 가른 데이터그램 하나 (protocol.md 7장 수신 분류).
    void on_datagram(const Endpoint& from, std::span<const std::byte> payload, Millis now);

    // 이 객체가 건 타이머의 만료. 이름이 자기 것이 아니면 아무것도 하지 않는다.
    void on_timer(std::string_view timer_name, Millis now);

    // 이 이름이 이 객체의 타이머인가. 루프가 만료를 나눠 줄 때 쓴다.
    //
    // **모양을 정확히 본다.** `stun.retry.<10진 인덱스>` 와 `stun.deadline.<10진 인덱스>`
    // 둘뿐이다. `stun.` 으로 시작하는 것을 전부 받으면 나중에 생길 다른 `stun.*` 타이머를
    // 이 객체가 가로챈다. 접두 일치는 배제 목록이고 모양 일치가 허용 목록이다.
    [[nodiscard]] static bool owns_timer(std::string_view timer_name) noexcept;

    [[nodiscard]] StunPhase phase() const noexcept { return phase_; }
    [[nodiscard]] bool done() const noexcept {
        return phase_ == StunPhase::Succeeded || phase_ == StunPhase::Failed;
    }

    // 얻은 관측. 성공이면 서로 다른 두 서버의 것이다.
    [[nodiscard]] const std::vector<StunMapping>& mappings() const noexcept { return mappings_; }

    // 이 목록으로 단계가 쓸 수 있는 최대 시간.
    [[nodiscard]] Millis stage_cap_ms() const noexcept {
        return stun_stage_cap_ms(servers_.size());
    }

private:
    // 질의 중인 자리 하나. 자리 수는 kStunConcurrency 다.
    struct Slot {
        bool active = false;
        std::size_t server_index = 0;
        TransactionId transaction{};
        std::size_t sent = 0;  // 보낸 요청 수. 1 이면 첫 요청만 보냈다
    };

    [[nodiscard]] std::size_t in_flight() const noexcept;
    [[nodiscard]] Slot* find_slot(std::size_t server_index) noexcept;
    [[nodiscard]] Slot* free_slot() noexcept;

    // 다음 서버를 빈 자리에 올린다. 더 올릴 것이 없으면 끝났는지 판정한다.
    void refill(Millis now);
    // 한 자리에 서버 하나를 올리고 첫 요청을 보낸다. 난수 실패면 거짓이다.
    bool begin_query(Slot& slot, std::size_t server_index, Millis now);
    void send_request(const Slot& slot);
    // 그 자리를 끝낸다. 타이머 둘을 지우고 그 트랜잭션을 resolved_ 에 넣는다.
    //
    // **자리가 끝나는 경로는 셋이다** (응답, 마감, 단계 종료). 셋 다 이 함수를 지난다.
    // 한 경로만 resolved_ 에 넣으면 나머지 경로 뒤에 온 정상 응답이 폐기로 세어진다.
    void release(Slot& slot);
    void fail();

    [[nodiscard]] std::string retry_timer(std::size_t server_index) const;
    [[nodiscard]] std::string deadline_timer(std::size_t server_index) const;

    std::vector<StunServer> servers_;
    StunSendFn send_;
    StunRandomFn random_;
    TimerSet& timers_;
    Counters& counters_;

    std::array<Slot, kStunConcurrency> slots_{};
    std::size_t next_server_ = 0;  // 아직 질의하지 않은 첫 서버
    std::vector<StunMapping> mappings_;
    // 끝난 트랜잭션. 재시도가 부른 뒤늦은 응답을 폐기로 세지 않으려는 것이다.
    std::vector<TransactionId> resolved_;
    StunPhase phase_ = StunPhase::Idle;
};

}  // namespace sangtachi::network
