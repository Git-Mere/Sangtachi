#pragma once

// 호스트의 `ROSTER` 송신 (protocol.md 5.7 ROSTER, 11장 타이머의 `ROSTER` 두 행).
//
// 터널이 들고, 호스트일 때만 켠다(`start`). 플레이어는 보내지 않는다(5.7).
//
// ## 명부
//
// 방의 전원은 호스트 자신과, 마지막으로 받은 `host_report` 응답의 `peers` 다(`set_peers`). 세션 목록에서
// 만들지 않는다. 비트 0 은 그 멤버와의 세션이 `CONNECTED` 일 때만 1 이고 호스트 자신은 늘 1 이다.
// `generation` 은 이 객체에 하나다. `start` 때 0 이고, 실을 내용(멤버, 이름, 비트 0)이 직전과 달라질
// 때마다 1 오른다(5.7 "명부가 바뀐 것은 실을 내용이 바뀐 것이다").
//
// ## 시점 (5.7 의 표와 11장)
//
// | 계기 | 동작 |
// |------|------|
// | 명부가 바뀜 | 모든 `CONNECTED` 세션에 즉시 1회. 그 세션마다 5초 간격 재송 2회를 건다. 진행 중이던 재송은 새로 건 것으로 바뀐다 |
// | 세션이 `CONNECTED` 에 들어감 | 비트 0 이 바뀌므로 위 행이다. 그 세션에 따로 또 보내지 않는다. 멤버가 아니어서 내용이 그대로면 그 세션에만 1회 |
// | 송신마다 | 그 세션의 15초 주기를 다시 건다(11장 "초기화 계기: 매 송신") |
// | 세션이 `CONNECTED` 를 떠남 | 그 세션의 두 타이머를 지운다. 내용이 바뀌면 위 첫 행이다 |
//
// ## 이 코드가 정한 것
//
// | 자리 | 정한 것 | 근거 |
// |------|---------|------|
// | "즉시" 의 자리 | 계기(상태 전이, `set_peers`)는 표시만 하고, 같은 바퀴의 `run` 이 보낸다. 수신 처리 안에서 다시 보내지 않는다 | 루프는 수신 뒤 같은 바퀴에 세션 타이머를 돈다(11장) |
// | 한 바퀴 안의 여러 변화 | 한 번으로 묶는다. `generation` 은 1 오르고 한 장이 나간다. 멤버가 나갔다 같은 바퀴에 돌아와 내용이 그대로 보여도 변화로 센다. 떠난 세션의 타이머는 그 자리에서 지운다 | 받는 쪽이 보는 것은 마지막 내용과 값이 커졌다는 것뿐이다(5.7 수신 검증 3번) |
// | 타이머 이름 | `roster.period.<peer_id>`, `roster.resend.<peer_id>`. 세션 타이머와 같은 꼴 | architecture.md 9장 `timer.tick` |
// | 멤버가 다섯을 넘는 입력 | 앞 MAX_PEERS 개만 싣지 않고 아예 보내지 않는다. `encode_roster` 가 거절한다 | 5.7 의 `count` 상한. 방은 다섯을 넘지 않는다 |

#include "sangtachi/peer/session.hpp"
#include "sangtachi/timer.hpp"
#include "sangtachi/tunnel/packet.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace sangtachi::tunnel {

// `host_report` 응답의 `peers` 원소 하나 (control_plane.md 4.6).
struct RosterPeer {
    std::uint32_t peer_id = 0;
    std::uint32_t virtual_ip = 0;
    std::string name;
};

class RosterPublisher {
public:
    // 지금 CONNECTED 인 세션들. 터널이 넘긴다.
    using Connected = std::vector<peer::Session*>;

    // 방을 세웠다. 호스트 자신의 항목을 정하고 `generation` 을 0 으로 둔다.
    void start(const RosterPeer& self);
    // 방을 떠났다. 다시 start 할 때까지 아무것도 보내지 않는다.
    void stop();
    [[nodiscard]] bool active() const noexcept { return self_.has_value(); }

    void set_peers(std::vector<RosterPeer> peers);
    // 세션이 CONNECTED 에 들어가거나 떠났다. 떠나면 그 세션의 타이머를 바로 지운다. 다음 `run` 이 명부를
    // 다시 보고 보낸다.
    void on_connected_change(std::uint32_t peer_id, bool entered);

    // 가장 이른 마감. 다시 볼 것이 있으면 지금(0)이다.
    [[nodiscard]] std::optional<Millis> earliest() const noexcept;
    void run(Millis now, const Connected& connected);

    [[nodiscard]] std::uint32_t generation() const noexcept { return generation_; }

private:
    struct Schedule {
        std::optional<Millis> period_due;
        Millis period_since = 0;
        std::optional<Millis> resend_due;
        Millis resend_since = 0;
        std::size_t resends_left = 0;
    };

    [[nodiscard]] Roster build(const Connected& connected) const;
    void refresh(Millis now, const Connected& connected);
    void send(peer::Session& session, Schedule& schedule, Millis now);

    std::optional<RosterPeer> self_;
    std::vector<RosterPeer> peers_;
    std::uint32_t generation_ = 0;
    std::optional<Roster> last_;           // 마지막으로 정한 명부. generation 은 0 으로 두고 비교한다
    bool forced_change_ = false;           // 멤버의 비트 0 이 바뀌었다 (on_connected_change)
    std::map<std::uint32_t, Schedule> schedules_;  // CONNECTED 세션마다. 키는 상대 peer_id
    bool dirty_ = false;
};

}  // namespace sangtachi::tunnel
