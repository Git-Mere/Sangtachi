#pragma once

// 화면 큐의 항목과, 그 항목에서 창에 보일 것 (architecture.md 3.1 의 ui/view, 3.5 GUI 실행 파일과 멤버 목록,
// concurrency.md 6장 화면 큐).
//
// Qt 를 쓰지 않는다. sangtachi_core 에 들어 시험 실행 파일이 링크한다 (ADR 0034).
//
// ## 이 코드가 정한 것
//
// | 자리 | 정한 것 | 근거 |
// |------|---------|------|
// | 화면 큐의 크기 | 4 칸. 루프는 2 칸까지만 채우고, 남은 2 칸은 루프가 끝난 뒤의 두 항목(정리 중, 끝) 몫이다 | 6장은 크기를 정하지 않았다. 루프가 끝나면 다시 넣을 바퀴가 없으므로 마지막 상태를 버리지 않으려면 자리가 남아 있어야 한다 |
// | 넣지 못한 항목 | 들고 있다가 다음 publish 때 다시 넣는다. 루프가 GUI_POLL_MS 안에 다시 돌게 마감을 하나 둔다(client_process.cpp) | 6장 "다음 바퀴에 다시 넣는다". 마감이 없으면 로비에서 다음 사건까지 루프가 깨지 않는다 |
// | 글자 칸 | 고정 길이. 넘치면 자른다. ERROR 줄은 512 바이트, 로그 파일 이름은 64 바이트 | 항목을 복사만으로 나른다 |
// | 버튼과 이름 칸 | 준비 단계(kReady)에서만 켠다. 방 만들기와 참가는 로비이고 미결 요청이 없을 때, 방 나가기는 방에 있을 때, 이름 칸은 로비일 때다 | architecture.md 3.5 의 GUI 표와 로비 명령의 "받는 때". `Loading...` 과 `Closing...` 동안은 끈다 |
// | 창을 닫은 뒤 | `[gui]` 는 자기 사본의 단계를 kClosing 으로 바꿔 다시 그린다. 작업 스레드의 항목을 기다리지 않는다 | concurrency.md 7장 GUI 빌드의 종료 "블록하지 않는다" |
// | 대화 상자의 머리 줄 | `Could not start.` 와 `Stopped.` | 3.5 "안내의 문구는 계약이 아니다" |

#include "sangtachi/control/lobby.hpp"
#include "sangtachi/protocol_constants.hpp"
#include "sangtachi/spsc_ring.hpp"
#include "sangtachi/timer.hpp"
#include "sangtachi/tunnel/packet.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace sangtachi::ui {

// `[gui]` 가 화면 큐를 읽는 간격 (concurrency.md 6장의 GUI_POLL_MS).
inline constexpr Millis kGuiPollMs = 100;

// 고정 길이 글자 칸. 넘치는 글자는 자른다.
template <std::size_t N>
struct FixedText {
    std::array<char, N> bytes{};
    std::size_t size = 0;

    void assign(std::string_view text) noexcept {
        size = std::min(text.size(), N);
        std::copy_n(text.data(), size, bytes.data());
    }
    [[nodiscard]] std::string_view view() const noexcept { return std::string_view(bytes.data(), size); }

    friend bool operator==(const FixedText& a, const FixedText& b) noexcept { return a.view() == b.view(); }
};

enum class Phase : std::uint8_t {
    kLoading,        // 준비 중. 첫 항목이 오기 전 `[gui]` 가 스스로 보이는 단계다
    kReady,          // 루프가 돈다
    kClosing,        // 루프가 끝나 정리 중이다
    kClosed,         // 사람이 닫았고 정리가 끝났다
    kStartupFailed,  // 기동 실패로 끝났다. 대화 상자를 보인다
    kEndedByItself,  // 사람이 닫지 않았는데 끝났다. 기동 실패와 같은 대화 상자다 (concurrency.md 7장 GUI 빌드의 종료)
};

// 화면 큐의 항목 하나. 창에 보일 상태 전체다 (concurrency.md 6장).
struct ScreenState {
    Phase phase = Phase::kLoading;
    bool in_lobby = true;
    control::LobbyRole role = control::LobbyRole::kNone;
    bool request_pending = false;
    FixedText<protocol::kMaxNameLen> name;
    FixedText<8> room_id;              // 방 코드. 받기 전과 로비에서는 비었다
    std::uint32_t own_virtual_ip = 0;  // 0 이면 없다
    std::uint32_t own_peer_id = 0;     // 자기 줄을 가린다. 0 이면 없다 (control_plane.md 2.2: 0 은 발급하지 않는다)
    std::uint8_t member_count = 0;     // members 의 앞 몇 개가 뜻이 있나
    std::array<tunnel::RosterMember, protocol::kMaxPeers> members{};
    std::uint32_t fail_seq = 0;  // 낸 FAIL 줄의 수. 0 이면 실패 줄이 없다
    control::FailCode fail_code = control::FailCode::kControlPlaneExchangeFailed;
    FixedText<protocol::kMaxNameLen> fail_peer;  // 비면 `-`
    FixedText<512> error_line;  // 대화 상자의 ERROR 줄
    FixedText<64> log_name;     // 대화 상자의 로그 파일 이름

    friend bool operator==(const ScreenState&, const ScreenState&) noexcept = default;
};
static_assert(std::is_trivially_copyable_v<ScreenState>);

// 명부를 항목의 멤버 칸에 옮긴다. 널이면 비운다. MAX_PEERS 를 넘는 멤버는 싣지 않는다.
void set_members(ScreenState& state, const tunnel::Roster* roster) noexcept;

// 화면 큐 (concurrency.md 6장). 생산자는 작업 스레드(`[loop]` 와 그 앞뒤의 기동과 정리) 하나, 소비자는 `[gui]` 하나다.
class ScreenQueue {
public:
    static constexpr std::size_t kCapacity = 4;
    static constexpr std::size_t kEndSlots = 2;  // 루프가 끝난 뒤의 두 항목 몫 (위 머리 표)

    // 생산자. 루프가 바퀴마다 부른다. 직전에 받은 것과 같고 들고 있는 것이 없으면 넣지 않는다. 넣지 못하면 들고
    // 있다가 다음에 부를 때 다시 넣고, 그사이 새 상태가 오면 그것으로 바꾼다. 기다리지 않는다. 들고 있는 것이
    // 남으면 참이다.
    bool publish(const ScreenState& state);

    // 생산자. 루프가 끝난 뒤에 부른다. 들고 있던 것을 버리고 이것을 넣는다. 루프가 남긴 kEndSlots 칸을 쓰므로
    // 루프 뒤에 kEndSlots 번까지는 넣는다. 넣었으면 참이다.
    bool publish_end(const ScreenState& state);

    // 생산자. 마지막으로 받은 상태. 아직 없으면 기본값이다.
    [[nodiscard]] const ScreenState& last() const noexcept { return last_; }

    // 소비자. 큐를 비우고 마지막 항목을 돌려준다. 비어 있었으면 값이 없다.
    [[nodiscard]] std::optional<ScreenState> take_latest();

private:
    SpscRing<ScreenState, kCapacity> ring_;
    ScreenState last_{};
    bool has_last_ = false;
    bool held_ = false;  // last_ 를 아직 넣지 못했다
};

// ---------------------------------------------------------------- 창에 보일 것

// 멤버 목록의 한 줄 (architecture.md 3.5 멤버 목록).
struct MemberRow {
    std::string name;        // 자기 줄이면 뒤에 " (you)"
    std::string virtual_ip;  // 점 네 개 표기
    std::string_view state;  // "Connected" 또는 "Not connected"
};

[[nodiscard]] std::vector<MemberRow> member_rows(const ScreenState& state);

// 켤 것.
struct Controls {
    bool host = false;       // 방 만들기
    bool join = false;       // 방 코드 칸과 참가
    bool leave = false;      // 방 나가기
    bool name_edit = false;  // 이름 칸
    bool copy_room = false;  // 방 코드 복사
};

[[nodiscard]] Controls controls(const ScreenState& state);

// 자기 가상 IP. 없으면 빈 문자열이다.
[[nodiscard]] std::string own_virtual_ip_text(const ScreenState& state);

// 단계의 상태 줄. `Loading...`, `Closing...` 이고 그 밖은 비었다.
[[nodiscard]] std::string_view phase_text(const ScreenState& state);

// 마지막 실패 줄 (architecture.md 3.5 의 FAIL 줄 형식). 실패가 없었으면 값이 없다.
[[nodiscard]] std::optional<std::string> fail_text(const ScreenState& state);

// 그 실패 줄을 새로 보여야 하나. shown_seq 는 `[gui]` 가 마지막으로 보인 번호(없으면 0)다.
[[nodiscard]] bool fail_is_new(std::uint32_t shown_seq, const ScreenState& state) noexcept;

// 대화 상자의 글. kStartupFailed 와 kEndedByItself 에만 값이 있다.
[[nodiscard]] std::optional<std::string> dialog_text(const ScreenState& state);

// 방 만들기와 참가 버튼이 명령 큐에 넣을 줄 (architecture.md 3.5 GUI 실행 파일의 표). 이름 칸이 지금 이름과 다르면
// `name <이름>` 이 먼저다. 칸의 형식 검사는 콘솔의 name, join 과 같고, 틀리면 줄 없이 사유(상태 줄의 문장)만 있다.
struct Submit {
    std::vector<std::string> lines;
    std::string_view error;  // 비어 있지 않으면 lines 가 비었다
};

[[nodiscard]] Submit host_submit(const ScreenState& state, std::string_view name_field);
[[nodiscard]] Submit join_submit(const ScreenState& state, std::string_view name_field,
                                 std::string_view room_field);

}  // namespace sangtachi::ui
