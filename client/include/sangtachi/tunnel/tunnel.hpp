#pragma once

// 수신 검증 파이프라인과 세션 표 (protocol.md 8장 수신 검증 파이프라인, 8.4 DATA 내부 검증,
// 9.4.1 종료 상태의 수신, 5.6 종료 상태 세션 보존, 4.3 폐기 목록).
//
// 7장 수신 분류가 터널 후보로 넘긴 데이터그램이 `Tunnel::on_datagram` 으로 들어온다
// (loop.hpp 의 EventLoop::receive_datagram). 순서는 8장 머리 그대로다.
//
// | 단계 | 무엇 | 어디 | 실패 카운터 | 표 |
// |------|------|------|-------------|----|
// | 1 | 8.1 공통 검사 1~6 | tunnel/packet 의 check_common | `drop_short` ... `drop_type_length` | 프로세스 |
// | 2 | 8.1 검사 7. 대조 집합은 세션 표다 (5.7: 명부가 아니다) | 여기 | `drop_unknown_peer` | 프로세스 |
// | 3 | 8.1 검사 8. 폐기 목록 | 여기 | `drop_retired_epoch` | 그 세션 |
// | 4 | 9.4.1 종료 상태 | 여기 | `drop_terminal_state` | 그 세션 |
// | 5 | 8.2, 9.5, 8.3, 타입별 검사, 부수 효과 | peer/session 의 Session::receive | 그 머리의 표 | 그 세션 |
//
// 어느 카운터 표에 올리는지는 이름이 아니라 오르는 자리가 정한다 (counters.hpp). 검사 7 을 지나면
// 세션이 정해졌으므로 그 뒤는 전부 그 세션의 표다.
//
// ## 이 코드가 정한 것
//
// | 자리 | 정한 것 | 근거 |
// |------|---------|------|
// | 9.4.1 종료 상태 폐기의 자리 | 8.1 전체(1~8) 뒤, 8.2 앞. 종료 세션 앞으로 온 폐기 epoch 패킷은 `drop_retired_epoch` 다 | 8.1 은 공통 검사다. 종료 세션도 검사 7 의 "세션이 있는 피어" 에 든다(5.6 이 그 패킷을 `drop_unknown_peer` 가 아니라 `drop_terminal_state` 로 적었다) |
// | 폐기 목록의 개수 | 프로세스(터널)에 하나. 상한 `MAX_RETIRED_EPOCHS` | 4.3 이 "폐기 목록" 을 단수로 적었다 |
// | 4.3 등록 표의 첫 행과 5.1 시도 nonce 폐기 | **다음 `create_session` 때** 등록한다. 그때 남아 있는 종료 세션의 고정 상대 epoch 와, 끝난 시도의 nonce 다 | 4.3 "우리가 `IDLE`에서 새 시도를 시작할 때", 3장 "항목은 우리가 새 시도를 시작할 때만 생긴다". 지워진 세션의 epoch 는 4.3 대로 등록할 값이 없다 |
// | 같은 `peer_id` 의 활성 세션이 있는데 또 만든다 | 만들지 않고 널 | 8.5 "비어 있지 않으면 구현 결함" 과 같은 종류다 |
// | 상대 가상 IP 가 라우팅 칸으로 갈 수 없다 | 만들지 않고 널. `10.100.0.1`~`.5` 밖이거나, 자신의 가상 IP 이거나, 그 칸에 활성 세션이 있다 | 8.5 "자신의 칸도 비어 있다", "비어 있지 않으면 구현 결함이다" |
// | 8.5 검사 4번의 자신의 가상 IP | 마지막으로 만든 세션의 `local_virtual_ip`. 방을 떠나면 잊는다. 모르면 출발지가 맞을 수 없으므로 `tx_drop_bad_src` | 한 방에서 자신의 가상 IP 는 하나다(control_plane.md 4장 연산) |
// | 송신 실패 | `sendto` 가 실패하면 `kSendFailed`. 학습 전이라 후보 전체로 나갈 때는 실패한 자리에서 멈춘다 | roadmap.md Phase 4 `send` "중간에 `sendto` 가 실패하면 거기서 멈춘다" |
//
// ## 송신 (8.5 송신 측 검증, 라우팅 표)
//
// `send_data` 가 내부 패킷 하나를 8.5 의 순서대로 본다. 1~5 의 폐기는 프로세스 표, 6 은 고른 세션의
// 표다(세션을 고르기 전과 뒤, counters.hpp). 통과하면 송신 관찰 칸(시험 빌드의 `data.tx`)을 부르고 그
// 세션이 `DATA` 로 캡슐화해 보낸다(10.4 목적지).
//
// 라우팅 표(tunnel/router)의 칸은 세션을 만들 때 채우고 세션이 종료 상태에 닿으면 **기록 줄과
// 카운터보다 먼저** 비운다(8.5 순서 표). 종료 상태로 남긴 세션은 칸이 없다.
//
// ## 3차·4차의 자리
//
// - 세션을 실제로 만드는 랑데부(10.2)와 로비 연결은 4차 J 다. 그때까지 제품 경로에는 세션이
//   없으므로 터널 패킷은 전부 `drop_unknown_peer` 로 끝난다. 그것이 맞다 (9.1)
// - 종료 전이 때의 기록 줄(architecture.md 9장)은 `set_terminal_handler` 자리다

#include "sangtachi/counters.hpp"
#include "sangtachi/network/endpoint.hpp"
#include "sangtachi/peer/epoch.hpp"
#include "sangtachi/peer/session.hpp"
#include "sangtachi/timer.hpp"
#include "sangtachi/tunnel/packet.hpp"
#include "sangtachi/tunnel/router.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace sangtachi::tunnel {

// tunnel/packet 의 판정 종류가 가리키는 카운터. `kNone` 은 비어 있다.
// to_token(to_counter(e)) 는 counter_name(e) 와 같다 (packet.hpp 머리).
[[nodiscard]] std::optional<Counter> to_counter(PacketError error) noexcept;

// --- 8.4 DATA 내부 검증 10~15 ---
//
// 9번(상태가 `CONNECTED`)은 세션 상태라 Session::receive 가 먼저 본다. 여기는 페이로드
// 바이트와 두 가상 IP 만 본다. 위에서부터 보고 처음 걸린 자리에서 멈춘다.
//
// | # | 검사 | 어긋나면 | 카운터 |
// |---|------|----------|--------|
// | 10 | 버전 니블 == 4 | `kNotIpv4` | `drop_inner_not_ipv4` |
// | 11 | IHL 5~15 이고 `IHL*4 <= payload_length` | `kIhl` | `drop_inner_ihl` |
// | 12 | IPv4 헤더 체크섬 유효 (IHL*4 바이트의 1의 보수 합이 0xFFFF) | `kChecksum` | `drop_inner_checksum` |
// | 13 | `total_length == payload_length` | `kLength` | `drop_inner_length` |
// | 14 | 출발지 == 그 세션의 상대 가상 IP | `kSrc` | `drop_inner_src` |
// | 15 | 목적지 == 자신의 가상 IP | `kDst` | `drop_inner_dst` |
//
// 빈 페이로드는 10번에서 걸린다. 8.1 6번이 20바이트 하한을 보장하지만 판정 함수는 그것을
// 가정하지 않는다 (8.5 의 같은 이유). 14·15번이 읽는 오프셋 12~20 은 11번이 보장한다.
enum class InnerError : std::uint8_t {
    kNone = 0,
    kNotIpv4,
    kIhl,
    kChecksum,
    kLength,
    kSrc,
    kDst,
};

[[nodiscard]] InnerError check_inner_ipv4(std::span<const std::byte> payload,
                                          std::uint32_t peer_virtual_ip,
                                          std::uint32_t local_virtual_ip) noexcept;
[[nodiscard]] std::optional<Counter> to_counter(InnerError error) noexcept;

// 시계, 송신, 난수원, RTT 시계, 페이로드 훅을 주입한다.
struct TunnelDeps {
    peer::SendFn send;                         // EventLoop::send_datagram 과 같은 계약
    std::function<Millis()> clock;             // 타이머 시계 (platform::monotonic_ms)
    peer::MicrosFn rtt_clock_us;               // RTT 시계 (platform::monotonic_us)
    peer::RandomFn random;                     // epoch, 시도 nonce, 세션의 probe nonce. 비우면 platform::random_bytes (peer/epoch)
    // concurrency.md 3장 페이로드 훅. **제품 경로는 비워 둔다.** 그러면 8.4 를 통과한 DATA 가
    // drop_no_sink 로 끝난다. 시험 빌드의 M-5 비교와 에코 응답기(3차 H)가 여기에 붙는다.
    peer::DataSink data_sink;
};

// 8.5 의 결과. 폐기면 카운터는 이미 올렸다.
enum class TxOutcome : std::uint8_t {
    kSent,        // 통과했고 보냈다
    kDropped,     // 8.5 에서 버렸다
    kSendFailed,  // 통과했지만 sendto 가 실패했다. tx_err_send 는 세션이 올렸다
};

// 새 세션의 입력. epoch 와 nonce 는 터널이 뽑는다.
struct NewSession {
    std::uint32_t local_peer_id = 0;
    std::uint32_t local_virtual_ip = 0;
    std::uint32_t peer_id = 0;
    std::uint32_t peer_virtual_ip = 0;
    std::vector<network::Endpoint> candidates;
    bool peer_is_host = false;
    std::string peer_name;  // control_plane.md 2.7. 형식에 어긋나거나 비면 `-` 로 싣는다
};

class Tunnel {
public:
    Tunnel(Counters& counters, TunnelDeps deps);

    Tunnel(const Tunnel&) = delete;
    Tunnel& operator=(const Tunnel&) = delete;

    // 상대 후보를 받았다 (9.1 `IDLE` -> `PUNCHING`). 위 머리 표의 등록을 먼저 하고, 새 epoch 와
    // nonce 를 뽑아 세션을 만든다. 난수원이 실패하거나 같은 peer_id 의 활성 세션이 있으면 널.
    // 같은 peer_id 의 종료 세션은 이 자리에서 지운다.
    peer::Session* create_session(const NewSession& request);

    // 7장이 터널 후보로 분류한 데이터그램 하나. 위 머리 표의 순서다.
    void on_datagram(const network::Endpoint& from, std::span<const std::byte> datagram);

    // 내부 패킷 하나를 8.5 대로 보고 보낸다 (위 머리 "송신").
    TxOutcome send_data(std::span<const std::byte> inner);

    // 8.5 라우팅 표의 두 단계 조회. 경로가 없으면 널.
    [[nodiscard]] peer::Session* route(std::uint32_t virtual_ip) const noexcept;
    // 라우팅 칸이 있는 세션 가운데 CONNECTED 인 것들의 상대 가상 IP. 칸 순서다.
    [[nodiscard]] std::vector<std::uint32_t> connected_peers() const;
    [[nodiscard]] std::optional<std::uint32_t> local_virtual_ip() const noexcept { return local_vip_; }

    // 세션 타이머와 종료 세션 보존 마감 가운데 가장 이른 것 (concurrency.md 4장).
    [[nodiscard]] std::optional<Millis> earliest_deadline() const noexcept;

    // 마감이 지난 세션 타이머를 돌리고, 2분이 지난 종료 세션을 지운다 (5.6).
    void run_timers(Millis now);

    // 활성 세션을 전부 사용자 종료로 닫는다 (5.6, quit). 세션은 남는다.
    void close_all(Millis now);

    // 방을 떠난다 (concurrency.md 7장 로비). 활성 세션을 닫고 **남긴 종료 세션까지 전부 지운다** (5.6).
    void leave_room(Millis now);

    [[nodiscard]] peer::Session* find(std::uint32_t peer_id) noexcept;
    [[nodiscard]] std::size_t session_count() const noexcept { return sessions_.size(); }
    [[nodiscard]] std::size_t terminal_count() const noexcept;

    // 남아 있는 세션(종료 상태로 보존된 것 포함)마다 그 세션의 표 전량을 낸다. 프로세스 종료와
    // 콘솔 `counters` 가 프로세스 표 뒤에 부른다 (architecture.md 9장).
    void emit_session_counters() const;
    [[nodiscard]] peer::RetiredEpochs& retired_epochs() noexcept { return retired_; }
    [[nodiscard]] const peer::RetiredNonces& retired_nonces() const noexcept { return retired_nonces_; }

    // 8.5 를 통과해 캡슐화하기 직전의 내부 패킷마다 (시험 빌드의 `data.tx`). 제품 빌드는 비워 둔다.
    void set_tx_observer(std::function<void(std::span<const std::byte>)> observer) {
        on_tx_ = std::move(observer);
    }

    // 세션이 종료 상태에 닿을 때마다. 라우팅 칸을 비운 뒤에 부른다 (기록 줄의 자리. 4차 J).
    void set_terminal_handler(std::function<void(const peer::Session&)> handler) {
        on_terminal_ = std::move(handler);
    }

private:
    struct Entry {
        std::unique_ptr<peer::Session> session;
        bool epoch_retired = false;  // 4.3 등록 표 첫 행을 이미 했다
        bool nonce_retired = false;  // 5.1 시도 nonce 를 이미 등록했다
    };

    Millis now() const;
    void on_transition(const peer::Session& session, peer::SessionState from, peer::SessionState to);
    // 종료 세션이 MAX_PEERS - 1 개를 넘으면 가장 오래 남긴 것부터 지운다 (5.6).
    void enforce_retention_cap();
    void erase(std::map<std::uint32_t, Entry>::iterator it);

    Counters& counters_;
    TunnelDeps deps_;
    std::map<std::uint32_t, Entry> sessions_;  // 키는 상대 peer_id (8.1 검사 7)
    peer::RetiredEpochs retired_;              // 4.3. 프로세스에 하나
    peer::RetiredNonces retired_nonces_;       // 5.1
    std::vector<peer::Nonce> ended_nonces_;    // 지워진 세션의 nonce. 다음 create_session 때 등록한다
    Router routes_;                            // 8.5 (tunnel/router)
    std::optional<std::uint32_t> local_vip_;   // 8.5 검사 4번 (위 머리 표)
    std::function<void(const peer::Session&)> on_terminal_;
    std::function<void(std::span<const std::byte>)> on_tx_;
};

}  // namespace sangtachi::tunnel
