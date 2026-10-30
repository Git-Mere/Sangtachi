#include "sangtachi/ui/view.hpp"

#include "sangtachi/control/ops.hpp"
#include "sangtachi/network/endpoint.hpp"

#include <utility>

namespace sangtachi::ui {

void set_members(ScreenState& state, const tunnel::Roster* roster) noexcept {
    state.members = {};
    state.member_count = 0;
    if (roster == nullptr) {
        return;
    }
    // 호스트가 정한 명부는 MAX_PEERS 를 넘을 수 있다(roster_publisher.hpp 머리 표). 배열에 든 것만 싣는다.
    const std::size_t count = std::min<std::size_t>(roster->count, roster->members.size());
    for (std::size_t i = 0; i < count; ++i) {
        state.members[i] = roster->members[i];
    }
    state.member_count = static_cast<std::uint8_t>(count);
}

bool ScreenQueue::publish(const ScreenState& state) {
    if (!has_last_ || !(state == last_)) {
        last_ = state;
        has_last_ = true;
        held_ = true;
    }
    // 마지막 kEndSlots 칸은 루프가 끝난 뒤의 몫이다. size 는 소비자가 비우는 중이면 실제보다 클 수 있다. 그때는
    // 덜 넣을 뿐이다.
    if (held_ && ring_.size() + kEndSlots < kCapacity) {
        ScreenState copy = last_;
        held_ = !ring_.try_push(std::move(copy));
    }
    return held_;
}

bool ScreenQueue::publish_end(const ScreenState& state) {
    last_ = state;
    has_last_ = true;
    ScreenState copy = state;
    held_ = !ring_.try_push(std::move(copy));
    return !held_;
}

std::optional<ScreenState> ScreenQueue::take_latest() {
    std::optional<ScreenState> latest;
    while (auto item = ring_.try_pop()) {
        latest = *item;
    }
    return latest;
}

std::vector<MemberRow> member_rows(const ScreenState& state) {
    std::vector<MemberRow> rows;
    const std::size_t count = std::min<std::size_t>(state.member_count, state.members.size());
    for (std::size_t i = 0; i < count; ++i) {
        const auto& m = state.members[i];
        MemberRow row;
        row.name = std::string(m.name_view());
        if (m.peer_id == state.own_peer_id) {
            row.name += " (you)";
        }
        row.virtual_ip = network::format_ipv4(m.virtual_ip);
        // 연결 상태는 비트 0 의 두 값뿐이다 (architecture.md 3.5 멤버 목록).
        row.state = m.connected ? std::string_view("Connected") : std::string_view("Not connected");
        rows.push_back(std::move(row));
    }
    return rows;
}

Controls controls(const ScreenState& state) {
    Controls c;
    if (state.phase != Phase::kReady) {
        return c;
    }
    c.host = state.in_lobby && !state.request_pending;
    c.join = c.host;
    c.leave = !state.in_lobby;
    c.name_edit = state.in_lobby;
    c.copy_room = !state.room_id.view().empty();
    return c;
}

std::string own_virtual_ip_text(const ScreenState& state) {
    return state.own_virtual_ip == 0 ? std::string() : network::format_ipv4(state.own_virtual_ip);
}

std::string_view phase_text(const ScreenState& state) {
    switch (state.phase) {
        case Phase::kLoading: return "Loading...";
        case Phase::kClosing: return "Closing...";
        case Phase::kReady:
        case Phase::kClosed:
        case Phase::kStartupFailed:
        case Phase::kEndedByItself:
            break;
    }
    return {};
}

std::optional<std::string> fail_text(const ScreenState& state) {
    if (state.fail_seq == 0) {
        return std::nullopt;
    }
    return control::fail_line(state.fail_code, state.fail_peer.view());
}

bool fail_is_new(std::uint32_t shown_seq, const ScreenState& state) noexcept {
    // 번호는 FAIL 줄마다 오르고 0 은 "없음" 이다. [gui] 는 0 에서 시작하므로 실패가 없으면 같다.
    return state.fail_seq != shown_seq;
}

std::optional<std::string> dialog_text(const ScreenState& state) {
    std::string text;
    if (state.phase == Phase::kStartupFailed) {
        text = "Could not start.";
    } else if (state.phase == Phase::kEndedByItself) {
        text = "Stopped.";
    } else {
        return std::nullopt;
    }
    if (!state.error_line.view().empty()) {
        text += "\n";
        text += state.error_line.view();
    }
    if (!state.log_name.view().empty()) {
        text += "\nFull log: logs\\";
        text += state.log_name.view();
    }
    return text;
}

namespace {

// 콘솔이 같은 사유로 내는 안내와 같은 문장이다. 앞의 들여쓰기만 뺀다.
std::string_view reason_text(std::string_view reason) noexcept {
    std::string_view text = control::rejected_guide(reason);
    while (text.starts_with(' ')) {
        text.remove_prefix(1);
    }
    return text;
}

// command 앞에 이름이 바뀌었으면 name 줄을 붙인다.
Submit with_name(const ScreenState& state, std::string_view name_field, std::string command) {
    Submit submit;
    if (name_field != state.name.view()) {
        submit.lines.push_back("name " + std::string(name_field));
    }
    submit.lines.push_back(std::move(command));
    return submit;
}

}  // namespace

Submit host_submit(const ScreenState& state, std::string_view name_field) {
    if (!tunnel::is_valid_name(name_field)) {
        return Submit{{}, reason_text("bad_name")};
    }
    return with_name(state, name_field, "host");
}

Submit join_submit(const ScreenState& state, std::string_view name_field, std::string_view room_field) {
    if (!tunnel::is_valid_name(name_field)) {
        return Submit{{}, reason_text("bad_name")};
    }
    if (!control::is_room_id_input(room_field)) {
        return Submit{{}, reason_text("bad_room")};
    }
    return with_name(state, name_field, "join " + std::string(room_field));
}

}  // namespace sangtachi::ui
