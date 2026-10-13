#pragma once

// 세션 상태 기계 (protocol.md 9장 세션, 11장 타이머, 4.3~4.5, 5.5 PING/PONG, 5.6 CLOSE,
// 10.4 엔드포인트 학습).
//
// 세션 하나는 상대 하나와의 시도 하나다. **`IDLE` 에는 세션 객체가 없다** (9.1). 객체는 상대
// 후보를 받는 순간 `PUNCHING` 으로 생기고, 종료 상태(`FAILED`, `CLOSED`)에 닿은 뒤에도 터널이
// 2분 동안 들고 있다 (5.6, tunnel/tunnel.hpp).
//
// **소유는 `[loop]` 하나다** (concurrency.md 5장 상태 소유). 뮤텍스가 없다.
//
// ## 무엇을 주입하는가
//
// | 무엇 | 어떻게 | 왜 |
// |------|--------|-----|
// | 타이머 시계 | 모든 메서드가 `now` 를 인자로 받는다 | 11장 마감을 시계 주입으로 시험한다 (roadmap.md Phase 4 검증의 keepalive 주기, 종료 상태 보존) |
// | RTT 시계 | `SessionIo::rtt_clock_us` | concurrency.md 4장. RTT 만 QueryPerformanceCounter 다. 두 시계를 비교하지 않는다 |
// | 송신 | `SessionIo::send`. 성공이면 참 | 9.2 `sent_ack` 는 `sendto` 성공 뒤에만 선다. 실패면 `tx_err_ack` (6장) |
// | 난수원 | `SessionIo::random`. 세션은 probe nonce 만 뽑는다. epoch 와 시도 nonce 는 만든 쪽(터널)이 뽑아 `SessionConfig` 로 넘긴다 | 4.3, 5.1. 뽑는 함수는 peer/epoch 다 |
//
// `send` 의 계약은 EventLoop::send_datagram_uncounted 와 같다. `socket.error` 줄은 그 함수가 내고
// **`tx_err_send` 는 세션이 자기 표에 올린다** (6장). `HELLO_ACK` 이면 `tx_err_ack` 도 같이 올린다.
//
// ## 카운터 표 (counters.hpp 의 두 범위)
//
// 세션마다 표 하나를 가진다. 세션이 정해진 자리, 즉 이 객체가 내리는 모든 수신 판정과 이 세션의
// 송신, 타이머, 재협상, 엔드포인트 학습은 이 표에 오른다. 터널이 8.1 검사 7 을 지난 뒤 내리는
// 판정(검사 8, 9.4.1)도 이 표다. 표는 객체와 함께 사라진다. 그 전에 종료 전이에서 한 번 낸다.
//
// ## 상태는 플래그에서 나온다 (9.2)
//
// | 종료 표시 | `got_ack` | `sent_ack` | 상태 |
// |-----------|-----------|------------|------|
// | `FAILED` | - | - | `FAILED` |
// | `CLOSED` | - | - | `CLOSED` |
// | 없음 | 참 | 참 | `CONNECTED` |
// | 없음 | 하나만 참 | | `HANDSHAKING` |
// | 없음 | 거짓 | 거짓 | `PUNCHING` |
//
// 상태를 따로 저장하지 않는다. 9.5 7번이 "상태는 플래그 개수가 정한다" 고 적었다. 저장하면 같은
// 패킷 하나가 두 상태를 가리킬 수 있다.
//
// ## 수신 (`receive`)
//
// 터널이 8.1 공통 검사 1~8 과 9.4.1 종료 상태 폐기를 마친 패킷을 넘긴다. 여기서는 그 뒤를
// 문서 순서 그대로 한다 (8장 머리, ADR 0021).
//
// 1. 8.2 epoch 및 출발지 검사. `HELLO`/`HELLO_ACK` 은 `virtual_ip` 를 epoch 보다 먼저 본다.
//    `HELLO_ACK` 은 probe nonce 행을 시도 nonce 행보다 먼저 본다
// 2. 9.5 재협상. 8.2 가 다른 epoch 의 `HELLO` 를 보내는 자리다. **8.3 보다 먼저 도는 유일한 부수
//    효과다** (4.5, 8.3). 1번이 probe 행을 보기 전에 5초가 찬 잠정 경로를 치우는 것은 이 패킷의
//    부수 효과가 아니라 항목 수명이다. `poll` 이 먼저 돌았어도 같은 결과다
// 3. 8.3 중복 억제. 창은 하나다. 그 패킷이 실은 epoch 의 창이 곧 이 창이다. 이 자리에 오는
//    패킷의 epoch 는 고정값이거나(재협상이 방금 새로 고정한 것 포함), 미고정일 때 오는
//    `HELLO`/`HELLO_ACK` 이고 그때 창은 비어 있다 (8.3)
// 4. 타입별 검사. 8.4 `DATA` 9~15, 5.5 `PONG` 매칭, 5.6 reason, 5.7 `ROSTER` 다섯
// 5. 부수 효과. epoch 고정, 출발지 학습, 유휴 타이머 갱신, 9.4 전이표의 동작
//
// ## 이 코드가 정한 것
//
// 문서가 동작은 정했지만 자리를 적지 않은 곳이다. 근거 절을 같이 적는다.
//
// | 자리 | 정한 것 | 근거 |
// |------|---------|------|
// | 펀치 시작 전(10.2 펀치 지연 중) 재협상 | 9.5 1~6·8·9 만 한다. 7번 펀치 마감과 10번 `HELLO` 재전송은 `start_punch` 가 시작한다 | 9.1, 10.3: 펀치 마감은 실제 로컬 펀치 시작부터 센다 |
// | 재협상 뒤 첫 `HELLO` | 그 자리에서 한 바퀴 보내고 200ms 마다 | 11장: 재협상이 "시작" 계기다. 펀치 시작과 같은 뜻으로 읽었다. 10.3 "펀치 시작부터 보낸다" |
// | 사용자 종료 때 학습 전 활성 세션 | `CLOSE` 를 보내지 않고 `CLOSED` 로 둔다 | 5.6 "보내는가" 표와 "보낸 세션도 `CLOSED`" |
// | `endpoint_learned` | 값이 바뀔 때마다 1. 처음 학습(없음에서 있음)도 센다 | 10.4 "엔드포인트가 바뀌면" |
// | 출발지 학습을 하는 타입 | 10.4 (a)(b)(c) 를 만족하면 타입과 무관하다. 재협상을 일으킨 `HELLO` 만 빠진다 | 10.4 (a) 가 `DATA`, `PONG`, `CLOSE`, `ROSTER` 를 적었다. 9.5 6번 |
// | 8.4 를 통과했는데 갈 곳이 없는 `DATA` | 유휴 타이머 갱신과 학습은 하고 `drop_no_sink` | 9.4 "모든 검증을 통과한 패킷". `drop_no_sink` 는 검증이 아니라 출구다 (concurrency.md 3장) |
// | 만료한 `pending_pings` 항목에 온 `PONG` | 조회 전에 만료 항목을 치우므로 `drop_unmatched_pong` | 5.5 "5초를 넘긴 항목은 표본을 버리고 제거한다" |
// | 재협상과 `next_ping_id` | 되돌리지 않는다 | 5.5 "세션 시도마다 0에서". 재협상은 새 시도가 아니다(9.5 5번) |
// | 받아들인 `ROSTER` | `roster` 줄 하나(`peer_id`, `generation`, `members`). 버린 것은 내지 않는다 | 5.7 "받아들인 명부마다 `roster` 로그 줄", architecture.md 9장 |
// | 잠정 경로를 올리는 계기 | 위 머리의 학습 조건에서 (c) 만 못 한 패킷, 그리고 재협상을 일으킨 `HELLO`. `CLOSE` 는 세션을 끝내므로 올리지 않는다 | 10.4 (c), 9.5 9번 |
// | 이미 잠정 경로인 주소 | 다시 올리지 않는다. 토큰도 쓰지 않는다 | 10.4 "경로당 검증 `HELLO` 1회", roadmap.md Phase 4 의 토큰 셈 |
// | 회신과 등록의 순서 | 9.4 전이표의 동작(회신)이 먼저, 등록이 뒤다. 토큰이 하나 남았으면 `HELLO_ACK` 이 쓴다 | 9.4 `HELLO` 행의 순서 |
// | probe nonce 를 뽑지 못함 | 등록하지 않고 토큰도 쓰지 않는다. 카운터는 없다 | 5.1. 실패를 고정값으로 대신하지 않는다(peer/epoch) |
// | 버킷의 보충 | 1ms 마다 토큰 5/1000 개씩 잇달아 채운다. 처음에는 가득 차 있다 | 9.4 "초당 5개, 순간 10패킷". 고정 창으로 세지 않는다 |
// | 잠정 경로 자리의 만료 | `등록 + 5초 <= now`. 수신 조회 전과 `poll` 에서 치운다. `next_deadline` 에 들지만 `timer.tick` 은 내지 않는다 | 11장 만료 값 표. 타이머가 아니라 항목 수명이다 |
// | echo 를 받은 경로 | 자리가 빌 때까지 같은 nonce 의 echo 를 다시 받아도 같은 판정이다 | 5.1 "5초간 별도 표에 보관한다" |
// | 종료 상태 진입 | 잠정 경로 표를 비운다. 아직 echo 가 없던 경로는 `path_probe_failed` 로 세지 않는다 | 10.4 "`path_probe_failed` 는 6번에서만 오른다" |
// | 재협상과 잠정 경로 표, 예산 | 되돌리지 않는다 | 9.5 3번 목록에 없다 |
//
// ## 타이머 이름 (architecture.md 9장 timer.tick)
//
// 11장 세션 타이머 다섯이 만료할 때마다 `timer.tick name=<이름> elapsed_ms=<직전 만료 또는 건 시각부터>`
// 을 낸다. 이름은 STUN 의 `stun.retry.<자리>` 와 같은 꼴이다. `<peer_id>` 는 상대의 10진 값이다.
//
// | 11장 타이머 | 이름 |
// |-------------|------|
// | `HELLO` 재전송 | `session.hello.<peer_id>` |
// | 펀치 마감 | `session.punch.<peer_id>` |
// | `KEEPALIVE` | `session.keepalive.<peer_id>` |
// | `PING` | `session.ping.<peer_id>` |
// | 유휴 타임아웃 | `session.idle.<peer_id>` |
//
// 11장 "만료 값" 표의 항목 수명(`pending_pings`, 종료 세션 보존 등)은 타이머가 아니라 내지 않는다.
// 유휴의 기준 시각은 마지막 갱신이다.
//
// ## 후보 집합 밖 (10.4 (c) 경로 검증, 9.4 무검증 목적지 공통 예산)
//
// **검증된 주소**는 승인된 후보 집합에 있거나 지금의 `peer_endpoint` 인 주소다. 그 밖이 9.4 의 무검증
// 목적지다. 무검증 목적지로 나가는 송신 셋(`HELLO_ACK`, `PONG`, 검증 `HELLO`)은 세션당 토큰 버킷
// 하나를 쓴다. 토큰이 없으면 보내지 않고 `drop_unverified_tx` 다. `HELLO_ACK` 을 보내지 않았으면
// `sent_ack` 도 서지 않고, 송신 실패가 아니므로 `tx_err_ack` 도 아니다.
//
// 10.4 (a)(b) 를 만족했는데 출발지가 검증된 주소가 아니면 학습하지 않고 그 주소를 잠정 경로로
// 올린다. 10.4 (c) 1~4 번 순서다. 잠정 경로마다 probe nonce 하나를 뽑아 검증 `HELLO` 를 1회 보낸다.
// 그 nonce 를 같은 주소에서 echo 한 `HELLO_ACK` 은 8.2 의 probe 행이다. (b) 도 만족하면 그 주소로
// 전환한다. 자리는 등록부터 5초가 지나야 빈다. 그때까지 echo 가 없었으면 `path_probe_failed` 다.

#include "sangtachi/counters.hpp"
#include "sangtachi/network/endpoint.hpp"
#include "sangtachi/peer/epoch.hpp"
#include "sangtachi/timer.hpp"
#include "sangtachi/tunnel/packet.hpp"
#include "sangtachi/tunnel/replay.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sangtachi::peer {

// 9.1 상태 가운데 객체가 있는 다섯. `IDLE` 은 객체가 없다.
enum class SessionState : std::uint8_t {
    kPunching,
    kHandshaking,
    kConnected,
    kFailed,
    kClosed,
};

// 9.1 의 이름 문자열 그대로 (architecture.md 9장 `session.state` 의 `from`, `to`).
[[nodiscard]] std::string_view to_token(SessionState state) noexcept;
inline constexpr std::string_view kIdleToken = "IDLE";

[[nodiscard]] constexpr bool is_terminal(SessionState state) noexcept {
    return state == SessionState::kFailed || state == SessionState::kClosed;
}

// 9.6 실패 전이 가운데 세션이 내는 셋. 문자열은 architecture.md 8장 코드 그대로다.
enum class SessionFailure : std::uint8_t {
    kHolePunchTimeout,
    kPeerHandshakeFailed,
    kTunnelDropped,
};

[[nodiscard]] std::string_view to_token(SessionFailure failure) noexcept;

using SendFn = std::function<bool(const network::Endpoint&, std::span<const std::byte>)>;
using MicrosFn = std::function<std::uint64_t()>;
// concurrency.md 3장의 페이로드 훅. 8.4 를 통과한 `DATA` 의 페이로드 바이트열을 그대로 받는다.
using DataSink = std::function<void(std::span<const std::byte>)>;

class Session;
using TransitionFn = std::function<void(const Session&, SessionState from, SessionState to)>;

struct SessionConfig {
    std::uint32_t local_peer_id = 0;
    std::uint32_t local_virtual_ip = 0;  // 호스트 바이트 순서
    std::uint32_t peer_id = 0;
    std::uint32_t peer_virtual_ip = 0;   // 제어 평면이 알려 준 값 (5.1, 14장 계약 6)
    std::vector<network::Endpoint> candidates;  // 받은 후보 위생(10.1)을 지난 상대 후보
    bool peer_is_host = false;           // 5.7 수신 검증 1번
    std::string peer_name = "-";         // control_plane.md 2.7 표시 이름. 모르면 `-`. counter 줄이 싣는다
    std::uint32_t local_epoch = 0;       // generate_session_epoch. 0 이 아니다 (4.3)
    Nonce attempt_nonce;                 // generate_attempt_nonce (5.1)
};

struct SessionIo {
    SendFn send;                         // 위 머리의 계약
    RetiredEpochs* retired = nullptr;    // 9.5 1번의 등록 자리. 널이 아니다
    MicrosFn rtt_clock_us;               // 비면 RTT 표본을 만들지 않는다
    DataSink data_sink;                  // 비면 drop_no_sink (concurrency.md 3장 제품 빌드)
    RandomFn random;                     // probe nonce (5.1). 비우면 platform::random_bytes (peer/epoch)
    TransitionFn on_transition;          // 상태가 바뀔 때마다. 비어도 된다
};

// 수신 패킷 하나. 8.1 공통 검사를 통과했고 그 세션이 활성 상태다.
struct RxPacket {
    tunnel::Header header;
    std::span<const std::byte> payload;
    network::Endpoint from;
};

// 수신 판정. 폐기면 올린 카운터다. 카운터는 세션이 이미 올렸다.
struct RxResult {
    std::optional<Counter> dropped;
    [[nodiscard]] bool accepted() const noexcept { return !dropped.has_value(); }
};

class Session {
public:
    // `PUNCHING` 으로 생긴다 (9.1). `session.state from=IDLE to=PUNCHING` 을 낸다.
    Session(SessionConfig config, SessionIo io, Millis now);

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // 실제 로컬 펀치 시작 (10.2 의 펀치 지연 만료, 10.3). 후보 전체로 `HELLO` 한 바퀴를 보내고
    // 200ms 재전송과 10초 마감을 건다. 활성이 아니거나 이미 시작했으면 아무것도 하지 않고 거짓.
    // 이미 CONNECTED 면 시작한 것으로 적되 보내지도 걸지도 않고 거짓이다.
    bool start_punch(Millis now);

    // 위 머리 "수신". 8.2 부터 부수 효과까지.
    RxResult receive(const RxPacket& packet, Millis now);

    // 가장 이른 세션 타이머 마감. 없으면 비어 있다 (concurrency.md 4장 선형 주사).
    [[nodiscard]] std::optional<Millis> next_deadline() const noexcept;

    // 마감이 지난 타이머를 돌린다. 비교는 `deadline <= now` 다 (11장). 마감(펀치, 유휴)을
    // 주기 송신보다 먼저 본다. 같은 바퀴에 둘이 지나면 끝난 세션이 한 번 더 쏘지 않는다.
    void poll(Millis now);

    // 8.5 를 통과한 내부 패킷 하나를 `DATA` 로 보낸다. 목적지는 10.4 다(학습했으면 그 하나, 아니면
    // 후보 전체). 후보 전체로 나갈 때는 sendto 가 실패한 자리에서 멈추고 거짓이다. 상태는 부르는
    // 쪽(8.5 검사 6번)이 봤다.
    bool send_data(std::span<const std::byte> inner);

    // 호스트의 `ROSTER` 한 장 (5.7). 목적지는 10.4 다. 언제 보내는지는 tunnel/roster_publisher 가 정한다.
    void send_roster(std::span<const std::byte> payload) { send_to_peer(tunnel::PacketType::kRoster, payload); }

    // 사용자 종료(quit, leave). 5.6 의 "보내는가" 와 "어느 값" 표. 활성이 아니면 아무것도
    // 하지 않는다. 학습했으면 `CLOSE` 를 1회 보내고(`CONNECTED` 면 0x00, 아니면 0x01), 보냈든
    // 못 보냈든 `CLOSED` 다.
    void close_by_user(Millis now);

    // --- 읽기 ---
    [[nodiscard]] SessionState state() const noexcept;
    [[nodiscard]] bool active() const noexcept { return !is_terminal(state()); }
    [[nodiscard]] bool got_ack() const noexcept { return got_ack_; }
    [[nodiscard]] bool sent_ack() const noexcept { return sent_ack_; }
    [[nodiscard]] std::uint32_t peer_id() const noexcept { return config_.peer_id; }
    [[nodiscard]] std::uint32_t peer_virtual_ip() const noexcept { return config_.peer_virtual_ip; }
    [[nodiscard]] std::uint32_t local_epoch() const noexcept { return config_.local_epoch; }
    [[nodiscard]] const Nonce& attempt_nonce() const noexcept { return config_.attempt_nonce; }
    [[nodiscard]] std::optional<std::uint32_t> pinned_peer_epoch() const noexcept { return pinned_; }
    [[nodiscard]] std::optional<network::Endpoint> peer_endpoint() const noexcept { return peer_endpoint_; }
    [[nodiscard]] std::uint32_t next_sequence() const noexcept { return next_seq_; }
    [[nodiscard]] std::size_t renegotiations() const noexcept { return renegotiations_; }
    [[nodiscard]] std::size_t pending_pings() const noexcept { return pending_pings_.size(); }
    [[nodiscard]] std::size_t probe_paths() const noexcept { return probes_.size(); }
    [[nodiscard]] std::uint64_t next_ping_id() const noexcept { return next_ping_id_; }
    [[nodiscard]] std::optional<std::uint64_t> last_rtt_us() const noexcept { return last_rtt_us_; }
    [[nodiscard]] std::size_t rtt_samples() const noexcept { return rtt_samples_; }
    [[nodiscard]] std::optional<std::uint32_t> last_roster_generation() const noexcept {
        return last_generation_;
    }
    [[nodiscard]] std::optional<SessionFailure> failure() const noexcept { return failure_; }
    [[nodiscard]] std::optional<tunnel::CloseReason> peer_close_reason() const noexcept {
        return peer_close_reason_;
    }
    [[nodiscard]] std::optional<Millis> terminal_since() const noexcept { return terminal_since_; }
    [[nodiscard]] bool punch_started() const noexcept { return punch_started_; }
    [[nodiscard]] bool window_initialized() const noexcept { return window_.initialized(); }

    // 이 세션의 카운터 표. 터널이 검사 8 과 9.4.1 의 판정을 여기에 올린다.
    [[nodiscard]] Counters& counters() noexcept { return counters_; }
    [[nodiscard]] const Counters& counters() const noexcept { return counters_; }
    // 세션 표의 counter 줄이 싣는 두 필드. peer_name 은 이 객체를 가리키므로 객체보다 오래 쓰지 않는다.
    [[nodiscard]] CounterPeer counter_peer() const noexcept {
        return CounterPeer{config_.peer_id, config_.peer_name};
    }
    [[nodiscard]] bool is_candidate(const network::Endpoint& endpoint) const noexcept;

    // 11장 타이머 각각의 다음 마감. 없으면 그 타이머가 멈춰 있다. 시험이 9.5 10번과 9.6 1번을
    // 본다.
    [[nodiscard]] std::optional<Millis> hello_due() const noexcept { return hello_due_; }
    [[nodiscard]] std::optional<Millis> punch_deadline() const noexcept { return punch_deadline_; }
    [[nodiscard]] std::optional<Millis> keepalive_due() const noexcept { return keepalive_due_; }
    [[nodiscard]] std::optional<Millis> ping_due() const noexcept { return ping_due_; }
    [[nodiscard]] std::optional<Millis> idle_deadline() const noexcept { return idle_deadline_; }

private:
    // 송신 하나. sequence 를 할당하고 1 올린 뒤 보낸다 (4.4). 실패해도 번호는 쓴 것이다.
    bool send_packet(tunnel::PacketType type, std::span<const std::byte> payload,
                     const network::Endpoint& to);
    // 후보 집합에 있거나 지금의 peer_endpoint 인가 (위 머리 "후보 집합 밖").
    [[nodiscard]] bool is_verified(const network::Endpoint& endpoint) const noexcept;
    // 9.4 무검증 목적지 공통 예산. 검증된 주소면 쓰지 않고 참. 아니면 토큰 하나를 쓰고 참, 없으면
    // drop_unverified_tx 를 올리고 거짓이다. 거짓이면 보내지 않는다.
    bool spend_for(const network::Endpoint& to, Millis now);
    // 10.4 (c) 1~4번. 이미 잠정 경로인 주소면 아무것도 하지 않는다.
    void start_path_probe(const network::Endpoint& from, Millis now);
    // 5초가 찬 잠정 경로를 비운다. echo 가 없던 것은 path_probe_failed (10.4 (c) 6, 7번).
    void expire_probes(Millis now);
    // `KEEPALIVE`, `PING`, `ROSTER` 의 목적지 (10.4). 학습했으면 그 하나, 아니면 후보 전체. `DATA` 는 send_data 다.
    void send_to_peer(tunnel::PacketType type, std::span<const std::byte> payload);
    void send_hello_round();
    void send_hello_ack(const tunnel::Nonce& echo, const network::Endpoint& to, Millis now);
    void send_keepalive(Millis now);
    void send_ping(Millis now);
    void renegotiate(const RxPacket& packet, const tunnel::Nonce& nonce, Millis now);
    void learn(const network::Endpoint& from);
    void enter_connected(Millis now);
    void fail(SessionFailure failure, Millis now);
    void stop_timers() noexcept;
    // 마감을 now + interval 로 걸고 그 기준 시각을 적는다 (timer.tick 의 elapsed_ms).
    static void arm(std::optional<Millis>& due, Millis& since, Millis now,
                    std::uint64_t interval) noexcept;
    // architecture.md 9장 timer.tick. 이름은 session.<종류>.<peer_id>.
    void emit_tick(std::string_view kind, Millis now, Millis since) const;
    void enter_terminal(SessionState terminal, Millis now);
    // 플래그가 바뀐 뒤 부른다. 상태가 바뀌었으면 알리고, `CONNECTED` 진입이면 그 타이머를 건다.
    void settle(SessionState before, Millis now);
    void notify(SessionState from, SessionState to);
    RxResult drop(Counter counter);
    void expire_pings(Millis now);

    SessionConfig config_;
    SessionIo io_;
    Counters counters_;  // 이 세션의 표

    bool got_ack_ = false;
    bool sent_ack_ = false;
    std::optional<SessionState> terminal_;
    std::optional<SessionFailure> failure_;
    std::optional<tunnel::CloseReason> peer_close_reason_;
    std::optional<Millis> terminal_since_;

    std::optional<std::uint32_t> pinned_;      // 고정한 상대 epoch (4.3, 9.3)
    tunnel::ReplayWindow window_;              // 고정한 epoch 의 창 (4.5, 8.3)
    std::uint32_t next_seq_ = 0;               // 4.4. 그 시도의 첫 송신이 0 이다
    std::optional<network::Endpoint> peer_endpoint_;  // 10.4

    std::size_t renegotiations_ = 0;           // 9.5. 시도가 끝날 때까지 회복되지 않는다
    std::optional<Millis> last_renegotiation_; // 수용한 재협상의 시각. 폐기한 HELLO 는 갱신하지 않는다

    struct PendingPing {
        Millis sent_ms = 0;          // 5초 만료를 잰다 (타이머 시계)
        std::uint64_t sent_us = 0;   // RTT 를 잰다 (RTT 시계)
    };
    std::map<std::uint64_t, PendingPing> pending_pings_;  // 5.5. MAX_PENDING_PINGS 이하
    std::uint64_t next_ping_id_ = 0;
    std::optional<std::uint64_t> last_rtt_us_;
    std::size_t rtt_samples_ = 0;

    std::optional<std::uint32_t> last_generation_;  // 5.7 수신 검증 3번

    struct ProbePath {
        network::Endpoint to;
        tunnel::Nonce nonce{};
        Millis registered = 0;
        bool answered = false;  // echo 가 왔다. 자리가 빌 때 path_probe_failed 가 아니다
    };
    std::vector<ProbePath> probes_;  // 10.4 (c). MAX_PROBE_PATHS 이하. 등록 순서다
    // 9.4 공통 예산. 단위는 토큰의 1/1000 이다. 보충이 1ms 에 5 라서 정수로 떨어진다.
    std::uint64_t budget_milli_;
    Millis budget_at_;  // 마지막으로 보충한 시각

    bool punch_started_ = false;
    std::optional<Millis> hello_due_;
    std::optional<Millis> punch_deadline_;
    std::optional<Millis> keepalive_due_;
    std::optional<Millis> ping_due_;
    std::optional<Millis> idle_deadline_;
    // 각 타이머를 마지막으로 건 시각(시작, 직전 만료, 유휴 갱신). timer.tick 의 elapsed_ms 기준.
    Millis hello_since_ = 0;
    Millis punch_since_ = 0;
    Millis keepalive_since_ = 0;
    Millis ping_since_ = 0;
    Millis idle_since_ = 0;
};

}  // namespace sangtachi::peer
