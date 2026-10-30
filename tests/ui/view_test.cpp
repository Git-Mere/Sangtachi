#include "sangtachi/ui/view.hpp"

#include "sangtachi/control/lobby.hpp"
#include "sangtachi/tunnel/packet.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// 시험 케이스 이름은 ASCII 로만 적는다. 이유는 network/wsa_test.cpp 머리에 있다.
//
// 규칙의 출처는 client/include/sangtachi/ui/view.hpp 머리의 표와 그 표가 가리키는 문서다. 판정 함수마다 케이스
// 표를 먼저 두고 그 표를 그대로 돈다.

using sangtachi::control::FailCode;
using sangtachi::control::LobbyRole;
using namespace sangtachi::ui;

namespace {

constexpr std::uint32_t kHostId = 0x11111111u;
constexpr std::uint32_t kAliceId = 0x22222222u;

sangtachi::tunnel::RosterMember member(std::uint32_t peer_id, std::uint32_t vip, std::string_view name,
                                       bool connected) {
    sangtachi::tunnel::RosterMember m;
    m.peer_id = peer_id;
    m.virtual_ip = vip;
    m.connected = connected;
    m.name_len = static_cast<std::uint8_t>(name.size());
    for (std::size_t i = 0; i < name.size(); ++i) {
        m.name[i] = name[i];
    }
    return m;
}

ScreenState ready_lobby() {
    ScreenState s;
    s.phase = Phase::kReady;
    s.name.assign("Alice");
    return s;
}

// 호스트 하나와 플레이어 하나가 있는 방. 자기 peer_id 는 부르는 쪽이 정한다.
ScreenState in_room(std::uint32_t own) {
    ScreenState s = ready_lobby();
    s.in_lobby = false;
    s.role = own == kHostId ? LobbyRole::kHost : LobbyRole::kPlayer;
    s.room_id.assign("ABCDEF");
    s.own_peer_id = own;
    s.own_virtual_ip = own == kHostId ? 0x0A640001u : 0x0A640002u;
    sangtachi::tunnel::Roster r;
    r.count = 2;
    r.members[0] = member(kHostId, 0x0A640001u, "Host", true);
    r.members[1] = member(kAliceId, 0x0A640002u, "Alice", false);
    set_members(s, &r);
    return s;
}

}  // namespace

// ---------------------------------------------------------------- 버튼과 이름 칸

TEST_CASE("view: controls follow the case table", "[ui][view]") {
    // architecture.md 3.5 GUI 표: 방 만들기와 참가는 로비이고 미결 요청이 없을 때, 방 나가기는 방에 있을 때, 이름 칸은
    // 로비일 때. Loading..., Closing... 과 그 뒤로는 모두 끈다.
    struct Case {
        const char* what;
        Phase phase;
        bool in_lobby;
        bool pending;
        bool room;  // 방 코드가 있다
        bool host;  // join 도 같다
        bool leave;
        bool name;
        bool copy;
    };
    const Case cases[] = {
        {"ready lobby", Phase::kReady, true, false, false, true, false, true, false},
        {"ready lobby, request pending", Phase::kReady, true, true, false, false, false, true, false},
        {"ready, attempt started, no room code yet", Phase::kReady, false, true, false, false, true, false, false},
        {"ready, in room", Phase::kReady, false, false, true, false, true, false, true},
        {"ready, in room, host_report pending", Phase::kReady, false, true, true, false, true, false, true},
        {"loading", Phase::kLoading, true, false, false, false, false, false, false},
        {"closing from lobby", Phase::kClosing, true, false, false, false, false, false, false},
        {"closing from room", Phase::kClosing, false, false, true, false, false, false, false},
        {"closed", Phase::kClosed, true, false, false, false, false, false, false},
        {"startup failed", Phase::kStartupFailed, true, false, false, false, false, false, false},
        {"ended by itself in room", Phase::kEndedByItself, false, false, true, false, false, false, false},
    };
    for (const auto& c : cases) {
        INFO(c.what);
        ScreenState s;
        s.phase = c.phase;
        s.in_lobby = c.in_lobby;
        s.request_pending = c.pending;
        if (c.room) {
            s.room_id.assign("ABCDEF");
        }
        const Controls got = controls(s);
        CHECK(got.host == c.host);
        CHECK(got.join == c.host);
        CHECK(got.leave == c.leave);
        CHECK(got.name_edit == c.name);
        CHECK(got.copy_room == c.copy);
    }
}

// ---------------------------------------------------------------- 멤버 줄

TEST_CASE("view: member rows follow the case table", "[ui][view]") {
    struct Case {
        const char* what;
        ScreenState state;
        std::size_t rows;
        std::size_t you;  // (you) 가 붙는 줄. rows 이상이면 없다
    };
    const Case cases[] = {
        {"lobby has no rows", ready_lobby(), 0, 0},
        {"host sees itself first", in_room(kHostId), 2, 0},
        {"player sees itself second", in_room(kAliceId), 2, 1},
        {"own peer_id not in the roster marks nobody", in_room(0x33333333u), 2, 2},
    };
    for (const auto& c : cases) {
        INFO(c.what);
        const auto rows = member_rows(c.state);
        REQUIRE(rows.size() == c.rows);
        for (std::size_t i = 0; i < rows.size(); ++i) {
            const bool you = rows[i].name.ends_with(" (you)");
            CHECK(you == (i == c.you));
        }
    }
}

TEST_CASE("view: a member row shows the name, the virtual IP and bit 0", "[ui][view]") {
    const auto rows = member_rows(in_room(kAliceId));
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].name == "Host");
    CHECK(rows[0].virtual_ip == "10.100.0.1");
    CHECK(rows[0].state == "Connected");
    CHECK(rows[1].name == "Alice (you)");
    CHECK(rows[1].virtual_ip == "10.100.0.2");
    CHECK(rows[1].state == "Not connected");
}

TEST_CASE("view: MAX_PEERS members with 16 byte names all show, more are cut", "[ui][view]") {
    // 경계: 다섯 명과 16 바이트 이름은 그대로. 호스트가 정한 명부가 다섯을 넘으면(roster_publisher) 다섯만 싣는다.
    sangtachi::tunnel::Roster r;
    r.count = 6;  // 배열은 다섯 칸이다
    for (std::uint32_t i = 0; i < 5; ++i) {
        r.members[i] = member(100 + i, 0x0A640001u + i, "abcdefghijklmno" + std::to_string(i), i % 2 == 0);
    }
    ScreenState s = ready_lobby();
    s.in_lobby = false;
    s.own_peer_id = 104;
    set_members(s, &r);
    REQUIRE(s.member_count == 5);
    const auto rows = member_rows(s);
    REQUIRE(rows.size() == 5);
    CHECK(rows[0].name == "abcdefghijklmno0");
    CHECK(rows[4].name == "abcdefghijklmno4 (you)");
    CHECK(rows[4].virtual_ip == "10.100.0.5");
    CHECK(rows[4].state == "Connected");
    CHECK(rows[3].state == "Not connected");
    // 명부가 없으면 비운다
    set_members(s, nullptr);
    CHECK(s.member_count == 0);
    CHECK(member_rows(s).empty());
    CHECK(s == [] {
        ScreenState e = ready_lobby();
        e.in_lobby = false;
        e.own_peer_id = 104;
        return e;
    }());
}

// ---------------------------------------------------------------- 상태 줄과 실패 줄

TEST_CASE("view: phase text and own virtual IP", "[ui][view]") {
    struct Case {
        Phase phase;
        std::string_view text;
    };
    const Case cases[] = {
        {Phase::kLoading, "Loading..."},     {Phase::kReady, ""},         {Phase::kClosing, "Closing..."},
        {Phase::kClosed, ""},                {Phase::kStartupFailed, ""}, {Phase::kEndedByItself, ""},
    };
    for (const auto& c : cases) {
        ScreenState s;
        s.phase = c.phase;
        CHECK(phase_text(s) == c.text);
    }
    CHECK(own_virtual_ip_text(ready_lobby()).empty());
    CHECK(own_virtual_ip_text(in_room(kHostId)) == "10.100.0.1");
}

TEST_CASE("view: fail text is the FAIL line with code, peer and sentence", "[ui][view]") {
    ScreenState s = ready_lobby();
    CHECK_FALSE(fail_text(s).has_value());
    s.fail_seq = 1;
    s.fail_code = FailCode::kHolePunchTimeout;
    s.fail_peer.assign("Bob");
    CHECK(fail_text(s) ==
          "FAIL HOLE_PUNCH_TIMEOUT Bob " + std::string(sangtachi::control::fail_sentence(FailCode::kHolePunchTimeout)));
    // 상대가 없으면 `-` (architecture.md 3.5)
    s.fail_code = FailCode::kControlPlaneExchangeFailed;
    s.fail_peer.assign("");
    CHECK(fail_text(s) == sangtachi::control::fail_line(FailCode::kControlPlaneExchangeFailed));
    CHECK(fail_text(s)->starts_with("FAIL CONTROL_PLANE_EXCHANGE_FAILED - Could not exchange"));
}

TEST_CASE("view: a fail line shows again only when its number changed", "[ui][view]") {
    struct Case {
        std::uint32_t shown;
        std::uint32_t seq;
        bool is_new;
    };
    const Case cases[] = {
        {0, 0, false},  // 실패가 없었다
        {0, 1, true},   // 첫 실패
        {1, 1, false},  // 이미 보였다. 다른 칸이 바뀐 항목이 와도 다시 띄우지 않는다
        {1, 2, true},   // 다음 실패. 내용이 같아도 새것이다
        {7, 3, true},   // 번호가 다르면 새것이다
    };
    for (const auto& c : cases) {
        ScreenState s;
        s.fail_seq = c.seq;
        INFO("shown=" << c.shown << " seq=" << c.seq);
        CHECK(fail_is_new(c.shown, s) == c.is_new);
    }
}

TEST_CASE("view: the dialog shows the ERROR line and the log file name", "[ui][view]") {
    ScreenState s;
    s.error_line.assign("ERROR adapter.error reason=not_elevated");
    s.log_name.assign("20261029T010203Z-1234.log");
    for (const Phase p : {Phase::kLoading, Phase::kReady, Phase::kClosing, Phase::kClosed}) {
        s.phase = p;
        CHECK_FALSE(dialog_text(s).has_value());
    }
    s.phase = Phase::kStartupFailed;
    CHECK(dialog_text(s) ==
          "Could not start.\nERROR adapter.error reason=not_elevated\nFull log: logs\\20261029T010203Z-1234.log");
    s.phase = Phase::kEndedByItself;
    CHECK(dialog_text(s) ==
          "Stopped.\nERROR adapter.error reason=not_elevated\nFull log: logs\\20261029T010203Z-1234.log");
    // 로그를 열기 전에 끝나면 이름이 없다
    s.phase = Phase::kStartupFailed;
    s.log_name.assign("");
    CHECK(dialog_text(s) == "Could not start.\nERROR adapter.error reason=not_elevated");
}

TEST_CASE("view: fixed text cuts at its size and compares by content", "[ui][view]") {
    FixedText<4> t;
    t.assign("abcdef");
    CHECK(t.view() == "abcd");
    FixedText<4> u;
    u.assign("abcd");
    CHECK(t == u);
    u.assign("ab");  // 뒤에 남은 바이트는 비교하지 않는다
    t.assign("ab");
    CHECK(t == u);
    u.assign("");
    CHECK(u.view().empty());
    CHECK_FALSE(t == u);
}

// ---------------------------------------------------------------- 화면 큐 (concurrency.md 6장)

TEST_CASE("screen queue: pushes only when the state changed", "[ui][screen_queue]") {
    ScreenQueue q;
    CHECK_FALSE(q.take_latest().has_value());
    ScreenState a = ready_lobby();
    CHECK_FALSE(q.publish(a));  // 첫 항목은 바뀐 것이다
    CHECK(q.take_latest() == a);
    CHECK_FALSE(q.publish(a));
    CHECK_FALSE(q.take_latest().has_value());  // 같으면 넣지 않는다
    a.request_pending = true;
    CHECK_FALSE(q.publish(a));
    CHECK(q.take_latest() == a);
}

TEST_CASE("screen queue: a full queue holds the newest state and never drops it", "[ui][screen_queue]") {
    ScreenQueue q;
    ScreenState s = ready_lobby();
    // 루프 몫은 kCapacity - kEndSlots 칸이다
    for (std::uint32_t i = 1; i <= ScreenQueue::kCapacity - ScreenQueue::kEndSlots; ++i) {
        s.fail_seq = i;
        CHECK_FALSE(q.publish(s));
    }
    s.fail_seq = 10;
    CHECK(q.publish(s));   // 들고 있다
    s.fail_seq = 11;
    CHECK(q.publish(s));   // 들고 있던 것을 새 것으로 바꾼다
    CHECK(q.publish(s));   // 아직 자리가 없다
    // [gui] 가 비우면 루프 몫의 마지막 항목을 본다. 들고 있는 것은 아직 큐 밖이다
    REQUIRE(q.take_latest()->fail_seq == ScreenQueue::kCapacity - ScreenQueue::kEndSlots);
    // 다음 바퀴에 상태가 그대로여도 들고 있던 것을 넣는다
    CHECK_FALSE(q.publish(s));
    const auto latest = q.take_latest();
    REQUIRE(latest.has_value());
    CHECK(latest->fail_seq == 11);  // 10 은 바뀐 것으로 대신했다
    CHECK_FALSE(q.take_latest().has_value());
}

TEST_CASE("screen queue: the two end states fit even when the loop filled its share", "[ui][screen_queue]") {
    ScreenQueue q;
    ScreenState s = ready_lobby();
    for (std::uint32_t i = 1; i <= 5; ++i) {
        s.fail_seq = i;
        (void)q.publish(s);  // 루프 몫이 차고 마지막 것은 들고 있다
    }
    ScreenState closing = q.last();
    CHECK(closing.fail_seq == 5);
    closing.phase = Phase::kClosing;
    CHECK(q.publish_end(closing));
    ScreenState closed = closing;
    closed.phase = Phase::kClosed;
    CHECK(q.publish_end(closed));
    CHECK(q.take_latest() == closed);
    CHECK(q.last() == closed);
}

// ---------------------------------------------------------------- 방 만들기와 참가 버튼의 명령 줄

TEST_CASE("view: host and join submit follow the case table", "[ui][view]") {
    // architecture.md 3.5 GUI 표: 이름 칸과 방 코드 칸의 형식 검사는 --name, --room 과 같다. 틀리면 요청을 보내지 않고
    // 사유를 상태 줄에 보인다. 이름 칸이 지금 이름(Alice)과 다르면 name 줄이 먼저다.
    const std::string bad_name(sangtachi::control::rejected_guide("bad_name").substr(2));
    const std::string bad_room(sangtachi::control::rejected_guide("bad_room").substr(2));
    struct Case {
        const char* what;
        bool join;  // 거짓이면 host
        std::string_view name;
        std::string_view room;
        std::vector<std::string> lines;
        std::string error;
    };
    const Case cases[] = {
        {"host, name unchanged", false, "Alice", "", {"host"}, ""},
        {"host, name changed", false, "Bob_2", "", {"name Bob_2", "host"}, ""},
        {"host, empty name", false, "", "", {}, bad_name},
        {"host, name with a space", false, "Bo b", "", {}, bad_name},
        {"host, name 17 chars", false, "abcdefghijklmnopq", "", {}, bad_name},
        {"host, name 16 chars", false, "abcdefghijklmnop", "", {"name abcdefghijklmnop", "host"}, ""},
        {"host ignores the room field", false, "Alice", "bad", {"host"}, ""},
        {"join, name unchanged", true, "Alice", "ABCDEF", {"join ABCDEF"}, ""},
        {"join, lower case code", true, "Alice", "abcdef", {"join abcdef"}, ""},
        {"join, name changed", true, "Bob", "ABCDEF", {"name Bob", "join ABCDEF"}, ""},
        {"join, empty code", true, "Alice", "", {}, bad_room},
        {"join, 5 chars", true, "Alice", "ABCDE", {}, bad_room},
        {"join, 7 chars", true, "Alice", "ABCDEFG", {}, bad_room},
        {"join, code with a space", true, "Alice", "ABC DE", {}, bad_room},
        {"join, letter outside the set (O)", true, "Alice", "ABCDEO", {}, bad_room},
        {"join, bad name and good code", true, "", "ABCDEF", {}, bad_name},
    };
    const ScreenState s = ready_lobby();
    for (const auto& c : cases) {
        INFO(c.what);
        const Submit got = c.join ? join_submit(s, c.name, c.room) : host_submit(s, c.name);
        CHECK(got.lines == c.lines);
        CHECK(std::string(got.error) == c.error);
    }
    CHECK_FALSE(bad_name.starts_with(' '));
    CHECK_FALSE(bad_room.empty());
}
