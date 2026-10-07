#include "sangtachi/control/lobby.hpp"

#include "sangtachi/control/http.hpp"
#include "sangtachi/control/json.hpp"
#include "sangtachi/control/ops.hpp"
#include "sangtachi/log.hpp"
#include "sangtachi/network/endpoint.hpp"
#include "sangtachi/network/stun_client.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// 시험 케이스 이름은 ASCII 로만 적는다. 이유는 network/wsa_test.cpp 머리에 있다.
//
// 규칙의 출처는 client/include/sangtachi/control/lobby.hpp 머리와 그 표가 가리키는 문서다.
// 판정 기준은 roadmap.md Phase 3 검증의 "로비" 와 "클라이언트 쪽 계약" 묶음 가운데 순수 상태
// 기계로 판정할 수 있는 것이다.
//
// 시험은 Sim 하나로 돈다. Sim 은 [loop] 이 할 일을 흉내 낸다. 타이머를 가상 시계로 돌리고, 요청을
// 미결로 들고 있다가 시험이 준 응답을 돌려주고, STUN 의 시작과 취소를 센다. 행동 목록마다 통합
// 계약(SubmitRequest 는 하나이고 마지막, 미결은 하나, 이미 걸린 타이머를 다시 걸지 않음)과
// 비밀(로그에 room_id, peer_token, client_nonce 없음)을 Sim 이 매번 본다.

using namespace sangtachi::control;
using sangtachi::LogLevel;
using sangtachi::Millis;
using sangtachi::Role;
using sangtachi::network::Endpoint;
using sangtachi::network::parse_ipv4;
using sangtachi::network::StunMapping;

namespace {

constexpr std::string_view kToken = "0123456789abcdef0123456789abcdef";
constexpr std::string_view kRoom = "ABCDEF";
constexpr std::uint32_t kSelf = 11;
constexpr std::uint32_t kHostId = 7;
constexpr Millis kHostReportOpenMs = Millis{kHostReportOpenS} * 1000;
constexpr Millis kHostReportFullMs = Millis{kHostReportFullS} * 1000;

std::uint32_t ip(std::string_view text) {
    const auto value = parse_ipv4(text);
    REQUIRE(value.has_value());
    return *value;
}

Candidate cand(std::string_view address, std::uint16_t port) {
    return Candidate{Endpoint(ip(address), port), CandidateKind::kReflexive};
}

// ---------------------------------------------------------------- 응답 만들기

template <class T>
Reply<T> err(Outcome outcome, Failure failure) {
    return Reply<T>{outcome, failure, std::nullopt};
}

Reply<Issued> issued(std::string_view room = kRoom, std::uint32_t peer_id = kSelf) {
    const auto token = PeerToken::parse(kToken);
    REQUIRE(token.has_value());
    return Reply<Issued>{Outcome::kSuccess, Failure::kNone,
                         Issued{std::string(room), peer_id, *token, ip("10.100.0.2"), 120}};
}

Reply<Registered> registered(std::uint32_t accepted = 2, std::uint32_t rejected = 0) {
    return Reply<Registered>{Outcome::kSuccess, Failure::kNone, Registered{accepted, rejected}};
}

PeerView unready(std::uint32_t peer_id) {
    PeerView v;
    v.peer_id = peer_id;
    v.virtual_ip = ip("10.100.0.1");
    v.ready = false;
    return v;
}

PeerView ready(std::uint32_t peer_id, std::vector<Candidate> candidates) {
    PeerView v;
    v.peer_id = peer_id;
    v.virtual_ip = ip("10.100.0.3");
    v.ready = true;
    v.punch_delay_ms = 1000;
    v.elapsed_since_ready_ms = 10;
    v.candidates = std::move(candidates);
    return v;
}

Reply<Peers> peers(std::vector<PeerView> list) {
    bool any = false;
    for (const auto& p : list) {
        any = any || p.ready;
    }
    return Reply<Peers>{Outcome::kSuccess, Failure::kNone, Peers{any, std::move(list)}};
}

Reply<HostReport> report(std::vector<PeerView> list = {}) {
    return Reply<HostReport>{Outcome::kSuccess, Failure::kNone,
                             HostReport{120, {}, {}, std::move(list)}};
}

std::vector<StunMapping> two_mappings() {
    return {StunMapping{"stun.a", Endpoint(ip("203.0.113.5"), 40000)},
            StunMapping{"stun.b", Endpoint(ip("203.0.113.5"), 40001)}};
}

// ---------------------------------------------------------------- 본문 읽기

json::Value parse_body(const std::string& body) {
    auto parsed = json::parse(body);
    REQUIRE(parsed.value.has_value());
    return std::move(*parsed.value);
}

std::string body_string(const std::string& body, std::string_view key) {
    const json::Value v = parse_body(body);
    const json::Value* item = v.find(key);
    REQUIRE(item != nullptr);
    REQUIRE(item->as_string() != nullptr);
    return *item->as_string();
}

std::vector<std::int64_t> body_ids(const std::string& body, std::string_view key) {
    const json::Value v = parse_body(body);
    const json::Value* item = v.find(key);
    REQUIRE(item != nullptr);
    REQUIRE(item->as_array() != nullptr);
    std::vector<std::int64_t> out;
    for (const auto& e : *item->as_array()) {
        REQUIRE(e.as_integer().has_value());
        out.push_back(*e.as_integer());
    }
    return out;
}

// ---------------------------------------------------------------- Sim

struct Sent {
    SubmitRequest request;
    Millis at = 0;
};

class Sim {
public:
    Sim() : lobby([this] { return next_nonce(); }) { secrets.emplace_back(kToken); }

    Lobby lobby;
    Millis now = 0;
    std::map<std::string, Millis, std::less<>> timers;  // 이름 -> 마감
    std::optional<SubmitRequest> in_flight;
    std::vector<Sent> sent;
    std::vector<std::string> printed;
    std::vector<LogLine> logs;
    int stun_starts = 0;
    int stun_cancels = 0;
    bool stun_running = false;
    bool fail_nonce = false;
    bool drop_next = false;
    int dropped = 0;
    std::vector<std::string> secrets;  // 로그에 나오면 안 되는 문자열

    void command(std::string_view line) {
        const auto parsed = parse_lobby_command(line);
        REQUIRE(parsed.has_value());
        apply(lobby.on_command(*parsed));
    }

    template <class T>
    void reply(const Reply<T>& r) {
        REQUIRE(in_flight.has_value());
        const http::Op op = in_flight->op;
        in_flight.reset();
        apply(lobby.on_reply(ControlReply{op, r}));
    }

    // 실제 [loop] 은 StunClient 가 끝나면 그 객체를 버리고 결과를 넘긴다.
    void stun_done(bool ok, const std::vector<StunMapping>& mappings) {
        stun_running = false;
        apply(lobby.on_stun_done(ok, mappings));
    }

    // 시계를 ms 만큼 돌린다. 마감이 온 타이머를 마감 순서대로 넘긴다.
    void advance(Millis ms) {
        const Millis target = now + ms;
        for (;;) {
            auto earliest = timers.end();
            for (auto it = timers.begin(); it != timers.end(); ++it) {
                if (it->second <= target && (earliest == timers.end() || it->second < earliest->second)) {
                    earliest = it;
                }
            }
            if (earliest == timers.end()) {
                break;
            }
            now = earliest->second;
            const std::string name = earliest->first;
            timers.erase(earliest);
            apply(lobby.on_timer(name));
        }
        now = target;
    }

    [[nodiscard]] bool armed(std::string_view name) const { return timers.find(name) != timers.end(); }

    [[nodiscard]] std::size_t count_log(std::string_view event) const {
        std::size_t n = 0;
        for (const auto& l : logs) {
            n += l.event == event ? 1 : 0;
        }
        return n;
    }

    [[nodiscard]] std::size_t count_sent(http::Op op) const {
        std::size_t n = 0;
        for (const auto& s : sent) {
            n += s.request.op == op ? 1 : 0;
        }
        return n;
    }

    [[nodiscard]] std::size_t count_fail() const {
        std::size_t n = 0;
        for (const auto& p : printed) {
            n += p.rfind("FAIL ", 0) == 0 ? 1 : 0;
        }
        return n;
    }

    [[nodiscard]] const LogLine* last_log(std::string_view event) const {
        for (auto it = logs.rbegin(); it != logs.rend(); ++it) {
            if (it->event == event) {
                return &*it;
            }
        }
        return nullptr;
    }

    // ---- 흐름 지름길

    // host 를 쳐서 create_room 이 성공하고 STUN 이 돌기 시작한 상태.
    void host_to_stun() {
        command("host");
        REQUIRE(in_flight.has_value());
        reply(issued());
        REQUIRE(lobby.stage() == LobbyStage::kStun);
    }

    // host 가 방을 세운 상태 (register_candidate 성공).
    void host_settled() {
        host_to_stun();
        stun_done(true, two_mappings());
        REQUIRE(in_flight.has_value());
        REQUIRE(in_flight->op == http::Op::kRegisterCandidate);
        reply(registered());
        REQUIRE(lobby.stage() == LobbyStage::kSettled);
    }

    void player_to_stun(std::string_view room = kRoom) {
        command("join " + std::string(room));
        REQUIRE(in_flight.has_value());
        reply(issued());
        REQUIRE(lobby.stage() == LobbyStage::kStun);
    }

    void player_polling() {
        player_to_stun();
        stun_done(true, two_mappings());
        REQUIRE(in_flight.has_value());
        reply(registered());
        REQUIRE(lobby.stage() == LobbyStage::kPolling);
    }

private:
    std::optional<ClientNonce> next_nonce() {
        if (fail_nonce) {
            return std::nullopt;
        }
        ++nonce_counter_;
        std::array<std::byte, ClientNonce::kBytes> bytes{};
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            bytes[i] = static_cast<std::byte>(nonce_counter_ * 16 + static_cast<int>(i));
        }
        ClientNonce nonce = ClientNonce::from_bytes(bytes);
        secrets.emplace_back(nonce.reveal_for_request());
        return nonce;
    }

public:
    void apply(sangtachi::control::LobbyActions actions) { apply_impl(std::move(actions)); }

private:
    void check_log(const LogLine& line) {
        for (const auto& f : line.fields) {
            for (const auto& s : secrets) {
                INFO("event=" << line.event << " field=" << f.name);
                CHECK(f.value.find(s) == std::string::npos);
            }
        }
    }

    void apply_impl(sangtachi::control::LobbyActions actions) {
        // 통합 계약: SubmitRequest 는 많아야 하나이고 마지막이다.
        for (std::size_t i = 0; i < actions.size(); ++i) {
            if (std::holds_alternative<SubmitRequest>(actions[i])) {
                REQUIRE(i + 1 == actions.size());
            }
        }
        for (auto& action : actions) {
            if (auto* s = std::get_if<SubmitRequest>(&action)) {
                REQUIRE_FALSE(in_flight.has_value());  // 미결은 하나
                if (drop_next) {
                    drop_next = false;
                    ++dropped;
                    apply(lobby.on_submit_dropped());
                    return;
                }
                in_flight = *s;
                sent.push_back(Sent{*s, now});
            } else if (std::holds_alternative<StartStun>(action)) {
                REQUIRE_FALSE(stun_running);
                stun_running = true;
                ++stun_starts;
            } else if (std::holds_alternative<CancelStun>(action)) {
                REQUIRE(stun_running);
                stun_running = false;
                ++stun_cancels;
            } else if (auto* a = std::get_if<ArmTimer>(&action)) {
                REQUIRE(Lobby::owns_timer(a->name));
                REQUIRE_FALSE(armed(a->name));  // TimerSet 은 같은 이름을 거부한다
                timers.emplace(std::string(a->name), now + a->delay_ms);
            } else if (auto* c = std::get_if<CancelTimer>(&action)) {
                REQUIRE(armed(c->name));
                timers.erase(timers.find(c->name));
            } else if (auto* p = std::get_if<PrintLine>(&action)) {
                const bool room = p->text.rfind("ROOM ", 0) == 0;
                const bool fail = p->text.rfind("FAIL ", 0) == 0;
                REQUIRE((room || fail));
                if (fail) {
                    for (const auto& secret : secrets) {
                        CHECK(p->text.find(secret) == std::string::npos);
                    }
                }
                printed.push_back(p->text);
            } else if (auto* l = std::get_if<LogLine>(&action)) {
                check_log(*l);
                logs.push_back(*l);
            }
        }
        // STUN 결과가 오면 Sim 은 STUN 을 끝난 것으로 본다.
        CHECK(lobby.request_pending() == in_flight.has_value());
    }

    int nonce_counter_ = 0;
};

const std::string kCpefLine = fail_line(FailCode::kControlPlaneExchangeFailed);

}  // namespace

// ---------------------------------------------------------------- 표준 출력 줄

TEST_CASE("lobby: fail lines carry the code and the sentence of architecture 8") {
    struct Row {
        FailCode code;
        std::string_view line;
    };
    const Row rows[] = {
        {FailCode::kStunDiscoveryFailed,
         "FAIL STUN_DISCOVERY_FAILED 인터넷에서 내 주소를 확인하지 못했습니다. 네트워크 연결을 확인하고 다시 시도하세요."},
        {FailCode::kControlPlaneExchangeFailed,
         "FAIL CONTROL_PLANE_EXCHANGE_FAILED 방 정보를 주고받지 못했습니다. 방 코드가 맞는지, 방이 아직 열려 있는지 확인하고 다시 시도하세요."},
        {FailCode::kHolePunchTimeout,
         "FAIL HOLE_PUNCH_TIMEOUT 상대와 직접 연결을 만들지 못했습니다. 둘 중 한 명이 다른 네트워크에서 다시 시도해 보세요."},
        {FailCode::kPeerHandshakeFailed,
         "FAIL PEER_HANDSHAKE_FAILED 연결이 한쪽 방향만 열렸습니다. 방화벽 설정을 확인하고 다시 시도하세요."},
        {FailCode::kTunnelDropped,
         "FAIL TUNNEL_DROPPED 상대와의 연결이 끊어졌습니다. 상대가 접속을 종료했거나 네트워크가 불안정합니다."},
    };
    for (const auto& row : rows) {
        CHECK(fail_line(row.code) == row.line);
    }
    CHECK(room_line("ABCDEF") == "ROOM ABCDEF");
}

// ---------------------------------------------------------------- 명령 읽기

TEST_CASE("lobby: parse_lobby_command table") {
    struct Row {
        std::string_view line;
        bool lobby;
        LobbyCommandKind kind;
        std::string_view argument;
        bool extra;
    };
    using K = LobbyCommandKind;
    const Row rows[] = {
        {"host", true, K::kHost, "", false},
        {"  host  ", true, K::kHost, "", false},
        {"host now", true, K::kHost, "", true},
        {"join ABCDEF", true, K::kJoin, "ABCDEF", false},
        {"join\tabcdef", true, K::kJoin, "abcdef", false},
        {"join", true, K::kJoin, "", false},
        {"join   ", true, K::kJoin, "", false},
        {"join ABCDEF GHJKLM", true, K::kJoin, "ABCDEF", true},
        {"leave", true, K::kLeave, "", false},
        {"leave now", true, K::kLeave, "", true},
        {"quit", false, K::kHost, "", false},
        {"counters", false, K::kHost, "", false},
        {"hostx", false, K::kHost, "", false},
        {"Host", false, K::kHost, "", false},
        {"joinABCDEF", false, K::kHost, "", false},
        {"", false, K::kHost, "", false},
        {"   ", false, K::kHost, "", false},
    };
    for (const auto& row : rows) {
        INFO("line=[" << row.line << "]");
        const auto got = parse_lobby_command(row.line);
        REQUIRE(got.has_value() == row.lobby);
        if (got) {
            CHECK(got->kind == row.kind);
            CHECK(got->argument == row.argument);
            CHECK(got->extra == row.extra);
        }
    }
}

TEST_CASE("lobby: the cli role is the lobby command right after startup") {
    CHECK_FALSE(startup_command(Role::None, std::nullopt).has_value());
    CHECK_FALSE(startup_command(Role::None, std::string("ABCDEF")).has_value());
    const auto host = startup_command(Role::Host, std::nullopt);
    REQUIRE(host.has_value());
    CHECK(host->kind == LobbyCommandKind::kHost);
    const auto player = startup_command(Role::Player, std::string("ABCDEF"));
    REQUIRE(player.has_value());
    CHECK(player->kind == LobbyCommandKind::kJoin);
    CHECK(player->argument == "ABCDEF");
    const auto bare = startup_command(Role::Player, std::nullopt);
    REQUIRE(bare.has_value());
    CHECK(bare->argument.empty());
}

TEST_CASE("lobby: no role startup sends nothing and waits in the lobby") {
    Sim sim;
    CHECK(sim.lobby.in_lobby());
    CHECK_FALSE(startup_command(Role::None, std::nullopt).has_value());
    sim.advance(600000);
    CHECK(sim.sent.empty());
    CHECK(sim.printed.empty());
    CHECK(sim.timers.empty());
    CHECK(sim.stun_starts == 0);
}

TEST_CASE("lobby: a timer that is not armed does nothing") {
    Sim sim;
    sim.command("host");
    // 재시도를 기다리지 않는데 재시도 이름이 온다. 무시한다.
    CHECK(sim.lobby.on_timer(kRetryTimer).empty());
    CHECK(sim.lobby.on_timer(kDeadlineTimer).empty());
    CHECK(sim.lobby.on_timer("stun.retry.0").empty());
    sim.reply(issued());
    CHECK(sim.count_sent(http::Op::kCreateRoom) == 1);
    CHECK_FALSE(sim.in_flight.has_value());
    CHECK(sim.lobby.stage() == LobbyStage::kStun);
}

TEST_CASE("lobby: timer values match the documents") {
    // 시험의 다른 케이스는 같은 상수로 시각을 계산하므로 값이 바뀌어도 통과한다. 값 자체는 여기서 본다.
    CHECK(kHostReportOpenS == 5);       // control_plane.md 2.6 HOST_REPORT_OPEN_S
    CHECK(kHostReportFullS == 30);      // HOST_REPORT_FULL_S
    CHECK(kRetryIntervalMs == 1000);    // control_plane.md 8.3
    CHECK(kMaxTries == 3);              // control_plane.md 8.3
    CHECK(kGetPeersPollMs == 500);      // protocol.md 11장
    CHECK(kGetPeersDeadlineMs == 60000);  // protocol.md 11장
}

TEST_CASE("lobby: owns_timer is an exact allow list") {
    CHECK(Lobby::owns_timer("control.retry"));
    CHECK(Lobby::owns_timer("control.poll"));
    CHECK(Lobby::owns_timer("control.deadline"));
    CHECK(Lobby::owns_timer("control.host_report"));
    CHECK_FALSE(Lobby::owns_timer("control."));
    CHECK_FALSE(Lobby::owns_timer("control.retry2"));
    CHECK_FALSE(Lobby::owns_timer("control.pol"));
    CHECK_FALSE(Lobby::owns_timer("stun.retry.0"));
    CHECK_FALSE(Lobby::owns_timer("probe200"));
    CHECK_FALSE(Lobby::owns_timer(""));
}

// ---------------------------------------------------------------- 명령을 받는 때 (architecture.md 3.5)

namespace {

enum class Setup {
    kLobby,           // 로비, 미결 없음
    kLobbyPending,    // 로비, 끝난 시도의 요청이 미결
    kIssuing,         // create_room 미결
    kRetryWait,       // create_room 일시 오류 뒤 재시도 대기
    kStun,
    kRegistering,
    kPolling,
    kPlayerSettled,
    kHostSettled,
};

void setup(Sim& sim, Setup s) {
    switch (s) {
        case Setup::kLobby:
            break;
        case Setup::kLobbyPending:
            sim.command("host");
            sim.command("leave");
            break;
        case Setup::kIssuing:
            sim.command("host");
            break;
        case Setup::kRetryWait:
            sim.command("host");
            sim.reply(err<Issued>(Outcome::kTransient, Failure::kInternal));
            break;
        case Setup::kStun:
            sim.host_to_stun();
            break;
        case Setup::kRegistering:
            sim.host_to_stun();
            sim.stun_done(true, two_mappings());
            break;
        case Setup::kPolling:
            sim.player_polling();
            break;
        case Setup::kPlayerSettled:
            sim.player_polling();
            sim.advance(kGetPeersPollMs);
            sim.reply(peers({ready(kHostId, {cand("198.51.100.7", 5000)})}));
            REQUIRE(sim.lobby.stage() == LobbyStage::kSettled);
            break;
        case Setup::kHostSettled:
            sim.host_settled();
            break;
    }
}

}  // namespace

TEST_CASE("lobby: command acceptance table") {
    // expect_op: 그 명령이 보내는 요청. reason: WARN console.rejected 의 reason. 둘 다 비면
    // leave 를 받아 로비로 간 것이다.
    struct Row {
        Setup setup;
        std::string_view line;
        std::optional<http::Op> expect_op;
        std::string_view reason;
    };
    using S = Setup;
    constexpr auto kCreate = http::Op::kCreateRoom;
    constexpr auto kJoin = http::Op::kJoinRoom;
    const Row rows[] = {
        {S::kLobby, "host", kCreate, ""},
        {S::kLobby, "join ABCDEF", kJoin, ""},
        {S::kLobby, "join abcdef", kJoin, ""},
        {S::kLobby, "join", std::nullopt, "missing_room"},
        {S::kLobby, "join ABCDE0", std::nullopt, "bad_room"},
        {S::kLobby, "join ABCDEI", std::nullopt, "bad_room"},
        {S::kLobby, "join ABCDE", std::nullopt, "bad_room"},
        {S::kLobby, "join ABCDEFG", std::nullopt, "bad_room"},
        {S::kLobby, "host now", std::nullopt, "extra_argument"},
        {S::kLobby, "join ABCDEF GHJKLM", std::nullopt, "extra_argument"},
        {S::kLobby, "leave", std::nullopt, "in_lobby"},
        {S::kLobbyPending, "host", std::nullopt, "request_pending"},
        {S::kLobbyPending, "join ABCDEF", std::nullopt, "request_pending"},
        {S::kLobbyPending, "leave", std::nullopt, "in_lobby"},
        {S::kIssuing, "host", std::nullopt, "not_in_lobby"},
        {S::kIssuing, "join ABCDEF", std::nullopt, "not_in_lobby"},
        {S::kIssuing, "leave now", std::nullopt, "extra_argument"},
        {S::kIssuing, "leave", std::nullopt, ""},
        {S::kRetryWait, "host", std::nullopt, "not_in_lobby"},
        {S::kRetryWait, "leave", std::nullopt, ""},
        {S::kStun, "host", std::nullopt, "not_in_lobby"},
        {S::kStun, "join ABCDEF", std::nullopt, "not_in_lobby"},
        {S::kStun, "leave", std::nullopt, ""},
        {S::kRegistering, "join ABCDEF", std::nullopt, "not_in_lobby"},
        {S::kRegistering, "leave", std::nullopt, ""},
        {S::kPolling, "host", std::nullopt, "not_in_lobby"},
        {S::kPolling, "join ABCDEF", std::nullopt, "not_in_lobby"},
        {S::kPolling, "leave", std::nullopt, ""},
        {S::kPlayerSettled, "host", std::nullopt, "not_in_lobby"},
        {S::kPlayerSettled, "join ABCDEF", std::nullopt, "not_in_lobby"},
        {S::kPlayerSettled, "leave", std::nullopt, ""},
        {S::kHostSettled, "host", std::nullopt, "not_in_lobby"},
        {S::kHostSettled, "join ABCDEF", std::nullopt, "not_in_lobby"},
        {S::kHostSettled, "leave", std::nullopt, ""},
    };
    for (const auto& row : rows) {
        INFO("setup=" << static_cast<int>(row.setup) << " line=[" << row.line << "]");
        Sim sim;
        setup(sim, row.setup);
        const auto sent_before = sim.sent.size();
        const auto warn_before = sim.count_log("console.rejected");
        const auto end_before = sim.count_log("attempt.end");
        const auto printed_before = sim.printed.size();
        const LobbyStage stage_before = sim.lobby.stage();
        sim.command(row.line);

        CHECK(sim.printed.size() == printed_before);  // 명령이 FAIL 이나 ROOM 을 내지 않는다
        if (row.expect_op) {
            REQUIRE(sim.sent.size() == sent_before + 1);
            CHECK(sim.sent.back().request.op == *row.expect_op);
            CHECK(sim.count_log("console.rejected") == warn_before);
            CHECK_FALSE(sim.lobby.in_lobby());
        } else if (!row.reason.empty()) {
            CHECK(sim.sent.size() == sent_before);
            REQUIRE(sim.count_log("console.rejected") == warn_before + 1);
            const LogLine* warn = sim.last_log("console.rejected");
            REQUIRE(warn != nullptr);
            CHECK(warn->level == LogLevel::Warn);
            REQUIRE(!warn->fields.empty());
            CHECK(warn->fields[0].name == "reason");
            CHECK(warn->fields[0].value == row.reason);
            CHECK(sim.lobby.stage() == stage_before);  // 그 상태 그대로 남는다
        } else {
            CHECK(sim.sent.size() == sent_before);
            CHECK(sim.count_log("console.rejected") == warn_before);
            CHECK(sim.count_log("attempt.end") == end_before + 1);
            CHECK(sim.lobby.in_lobby());
            CHECK(sim.timers.empty());
            CHECK_FALSE(sim.stun_running);
        }
    }
}

TEST_CASE("lobby: a bad join warning does not log the typed text") {
    Sim sim;
    sim.command("join ZZZZZ0");
    const LogLine* warn = sim.last_log("console.rejected");
    REQUIRE(warn != nullptr);
    for (const auto& f : warn->fields) {
        CHECK(f.value.find("ZZZZZ") == std::string::npos);
    }
}

// ---------------------------------------------------------------- 로비 검증 (roadmap.md Phase 3 "로비")

TEST_CASE("lobby: a missing room gives one FAIL line and the next join works") {
    Sim sim;
    sim.command("join ABCDEF");
    REQUIRE(sim.in_flight.has_value());
    CHECK(sim.in_flight->op == http::Op::kJoinRoom);
    sim.reply(err<Issued>(Outcome::kDefinite, Failure::kRoomNotFound));

    REQUIRE(sim.printed.size() == 1);
    CHECK(sim.printed[0] == kCpefLine);
    const LogLine* result = sim.last_log("control.result");
    REQUIRE(result != nullptr);
    REQUIRE(result->fields.size() == 3);
    CHECK(result->fields[0].value == "join_room");
    CHECK(result->fields[1].value == "false");
    CHECK(result->fields[2].value == "room_not_found");
    CHECK(sim.count_log("session.failed") == 1);
    CHECK(sim.printed[0].rfind("FAIL CONTROL_PLANE_EXCHANGE_FAILED ", 0) == 0);
    CHECK(sim.lobby.in_lobby());
    CHECK_FALSE(sim.lobby.request_pending());
    sim.advance(10000);
    CHECK(sim.sent.size() == 1);  // 재시도하지 않는다
    CHECK(sim.printed.size() == 1);

    sim.command("join GHJKLM");
    REQUIRE(sim.sent.size() == 2);
    CHECK(sim.sent[1].request.op == http::Op::kJoinRoom);
    CHECK(body_string(sim.sent[1].request.body, "room_id") == "GHJKLM");
    sim.reply(issued("GHJKLM"));
    CHECK(sim.printed.back() == "ROOM GHJKLM");
    CHECK(sim.stun_running);
}

TEST_CASE("lobby: leave then rejoin uses a new nonce and reruns STUN") {
    Sim sim;
    setup(sim, Setup::kPlayerSettled);
    REQUIRE(sim.stun_starts == 1);
    const std::string first_nonce = body_string(sim.sent.front().request.body, "client_nonce");

    sim.command("leave");
    CHECK(sim.lobby.in_lobby());
    sim.command("join ABCDEF");
    REQUIRE(sim.in_flight.has_value());
    CHECK(sim.in_flight->op == http::Op::kJoinRoom);
    const std::string second_nonce = body_string(sim.in_flight->body, "client_nonce");
    CHECK(second_nonce != first_nonce);

    sim.reply(issued(kRoom, 99));
    CHECK(sim.stun_starts == 2);  // 앞 시도의 STUN 결과를 다시 쓰지 않는다
    CHECK(sim.count_sent(http::Op::kRegisterCandidate) == 1);
    sim.stun_done(true, two_mappings());
    CHECK(sim.count_sent(http::Op::kRegisterCandidate) == 2);
    CHECK(sim.in_flight->self_peer_id == 99);  // 새 참가의 peer_id 를 쓴다
}

TEST_CASE("lobby: leave while host is pending discards the late reply") {
    Sim sim;
    sim.command("host");
    const std::string first_nonce = body_string(sim.in_flight->body, "client_nonce");
    sim.command("leave");
    CHECK(sim.lobby.in_lobby());
    CHECK(sim.lobby.request_pending());

    // 응답이 오기 전의 host 는 WARN 이다.
    sim.command("host");
    CHECK(sim.sent.size() == 1);
    CHECK(sim.last_log("console.rejected")->fields[0].value == "request_pending");

    // 늦은 응답. ROOM 줄도 STUN 도 register_candidate 도 없다.
    sim.reply(issued());
    CHECK(sim.printed.empty());
    CHECK(sim.stun_starts == 0);
    CHECK(sim.count_log("control.discarded") == 1);
    CHECK(sim.count_log("control.result") == 0);
    CHECK(sim.timers.empty());  // host_report 를 시작하지 않는다
    sim.advance(60000);
    CHECK(sim.sent.size() == 1);

    // 응답이 온 뒤의 host 는 받는다. 새 nonce 다.
    sim.command("host");
    REQUIRE(sim.sent.size() == 2);
    CHECK(sim.sent[1].request.op == http::Op::kCreateRoom);
    CHECK(body_string(sim.sent[1].request.body, "client_nonce") != first_nonce);
}

TEST_CASE("lobby: a late failure reply after leave prints nothing") {
    Sim sim;
    sim.command("join ABCDEF");
    sim.command("leave");
    sim.reply(err<Issued>(Outcome::kDefinite, Failure::kRoomNotFound));
    CHECK(sim.printed.empty());
    sim.command("join ABCDEF");
    sim.command("leave");
    sim.reply(err<Issued>(Outcome::kTransient, Failure::kInternal));
    CHECK(sim.timers.empty());  // 재시도 타이머를 걸지 않는다
    sim.advance(5000);
    CHECK(sim.sent.size() == 2);
}

TEST_CASE("lobby: leave clears a waiting retry timer") {
    Sim sim;
    sim.command("host");
    sim.reply(err<Issued>(Outcome::kTransient, Failure::kUnavailable));
    REQUIRE(sim.armed(kRetryTimer));
    sim.command("leave");
    CHECK_FALSE(sim.armed(kRetryTimer));
    sim.advance(5000);
    CHECK(sim.sent.size() == 1);
}

TEST_CASE("lobby: leave during STUN cancels it and a late STUN result does nothing") {
    Sim sim;
    sim.host_to_stun();
    sim.command("leave");
    CHECK(sim.stun_cancels == 1);
    CHECK_FALSE(sim.armed(kHostReportTimer));  // 방을 나가면 host_report 를 멈춘다
    sim.stun_done(true, two_mappings());
    CHECK(sim.count_sent(http::Op::kRegisterCandidate) == 0);
    sim.advance(60000);
    CHECK(sim.sent.size() == 1);
}

// ---------------------------------------------------------------- 재시도 (control_plane.md 8.3)

namespace {

// op 의 요청이 미결인 상태까지 간다.
void reach(Sim& sim, http::Op op) {
    switch (op) {
        case http::Op::kCreateRoom:
            sim.command("host");
            break;
        case http::Op::kJoinRoom:
            sim.command("join ABCDEF");
            break;
        case http::Op::kRegisterCandidate:
            sim.player_to_stun();
            sim.stun_done(true, two_mappings());
            break;
        case http::Op::kGetPeers:
            sim.player_polling();
            sim.advance(kGetPeersPollMs);
            break;
        case http::Op::kHostReport:
            sim.host_settled();
            sim.advance(kHostReportOpenMs);
            break;
    }
    REQUIRE(sim.in_flight.has_value());
    REQUIRE(sim.in_flight->op == op);
}

void reply_error(Sim& sim, Outcome outcome, Failure failure) {
    switch (sim.in_flight->op) {
        case http::Op::kCreateRoom:
        case http::Op::kJoinRoom:
            sim.reply(err<Issued>(outcome, failure));
            break;
        case http::Op::kRegisterCandidate:
            sim.reply(err<Registered>(outcome, failure));
            break;
        case http::Op::kGetPeers:
            sim.reply(err<Peers>(outcome, failure));
            break;
        case http::Op::kHostReport:
            sim.reply(err<HostReport>(outcome, failure));
            break;
    }
}

}  // namespace

TEST_CASE("lobby: transient failures retry three times in total with the same body") {
    const http::Op ops[] = {http::Op::kCreateRoom, http::Op::kJoinRoom, http::Op::kRegisterCandidate};
    const Failure failures[] = {Failure::kInternal, Failure::kUnavailable, Failure::kTransport};
    for (const auto op : ops) {
        for (const auto failure : failures) {
            INFO("op=" << http::op_name(op) << " failure=" << to_token(failure));
            Sim sim;
            reach(sim, op);
            const auto base = sim.count_sent(op);
            const std::string body = sim.in_flight->body;
            const auto printed_before = sim.printed.size();

            for (int attempt = 1; attempt <= 3; ++attempt) {
                REQUIRE(sim.in_flight.has_value());
                CHECK(sim.in_flight->op == op);
                CHECK(sim.in_flight->body == body);  // 같은 요청. client_nonce 도 같다
                const Millis answered = sim.now;
                reply_error(sim, Outcome::kTransient, failure);
                if (attempt < 3) {
                    CHECK(sim.printed.size() == printed_before);
                    REQUIRE(sim.armed(kRetryTimer));
                    CHECK(sim.timers.find(kRetryTimer)->second == answered + kRetryIntervalMs);
                    sim.advance(kRetryIntervalMs - 1);
                    CHECK_FALSE(sim.in_flight.has_value());
                    sim.advance(1);
                }
            }
            CHECK(sim.count_sent(op) == base + 2);  // reach 가 보낸 첫 시도 + 재시도 둘
            REQUIRE(sim.printed.size() == printed_before + 1);
            CHECK(sim.printed.back() == kCpefLine);
            CHECK(sim.lobby.in_lobby());
            sim.advance(10000);
            CHECK(sim.count_sent(op) == base + 2);
        }
    }
}

TEST_CASE("lobby: a transient failure then success continues the attempt") {
    Sim sim;
    sim.command("host");
    sim.reply(err<Issued>(Outcome::kTransient, Failure::kInternal));
    sim.advance(kRetryIntervalMs);
    sim.reply(err<Issued>(Outcome::kTransient, Failure::kTransport));
    sim.advance(kRetryIntervalMs);
    REQUIRE(sim.in_flight.has_value());
    sim.reply(issued());
    CHECK(sim.printed == std::vector<std::string>{"ROOM ABCDEF"});
    CHECK(sim.stun_running);
}

TEST_CASE("lobby: definite failures fail at once without retry") {
    const http::Op ops[] = {http::Op::kCreateRoom, http::Op::kJoinRoom,
                            http::Op::kRegisterCandidate, http::Op::kGetPeers};
    const Failure failures[] = {Failure::kRateLimited, Failure::kRoomFull,    Failure::kUnauthorized,
                                Failure::kBadRequest,  Failure::kRoomNotFound, Failure::kRoomExpired,
                                Failure::kUnknownCode, Failure::kSelfInPeers,  Failure::kTooLarge,
                                Failure::kUnknownOp};
    for (const auto op : ops) {
        for (const auto failure : failures) {
            INFO("op=" << http::op_name(op) << " failure=" << to_token(failure));
            Sim sim;
            reach(sim, op);
            const auto base = sim.count_sent(op);
            reply_error(sim, Outcome::kDefinite, failure);
            REQUIRE(sim.count_fail() == 1);
            CHECK(sim.printed.back() == kCpefLine);
            CHECK(sim.lobby.in_lobby());
            CHECK(sim.timers.empty());
            CHECK(sim.count_log("session.failed") == 1);
            sim.advance(120000);
            CHECK(sim.count_sent(op) == base);
        }
    }
}

// ---------------------------------------------------------------- STUN 과 등록 (8.4 의 5번, 6번)

TEST_CASE("lobby: register sends the reflexive candidates from STUN") {
    Sim sim;
    sim.player_to_stun();
    sim.stun_done(true, two_mappings());
    REQUIRE(sim.in_flight.has_value());
    const json::Value body = parse_body(sim.in_flight->body);
    CHECK(*body.find("room_id")->as_string() == "ABCDEF");
    CHECK(*body.find("peer_id")->as_integer() == kSelf);
    const auto* list = body.find("candidates")->as_array();
    REQUIRE(list != nullptr);
    REQUIRE(list->size() == 2);
    CHECK(*(*list)[0].find("ip")->as_string() == "203.0.113.5");
    CHECK(*(*list)[0].find("port")->as_integer() == 40000);
    CHECK(*(*list)[0].find("kind")->as_string() == "reflexive");
    CHECK(*(*list)[1].find("port")->as_integer() == 40001);
}

TEST_CASE("lobby: rejected candidates leave one warning and the attempt goes on") {
    Sim sim;
    sim.player_to_stun();
    sim.stun_done(true, two_mappings());
    sim.reply(registered(1, 1));
    CHECK(sim.count_log("control.candidates_rejected") == 1);
    CHECK(sim.last_log("control.candidates_rejected")->level == LogLevel::Warn);
    CHECK(sim.lobby.stage() == LobbyStage::kPolling);
    Sim clean;
    clean.player_to_stun();
    clean.stun_done(true, two_mappings());
    clean.reply(registered(2, 0));
    CHECK(clean.count_log("control.candidates_rejected") == 0);
}

TEST_CASE("lobby: STUN failure prints one FAIL line and returns to the lobby") {
    struct Row {
        bool host;
        bool ok;
        bool empty;
    };
    const Row rows[] = {{false, false, false}, {true, false, false}, {false, true, true}, {true, true, true}};
    for (const auto& row : rows) {
        INFO("host=" << row.host << " ok=" << row.ok << " empty=" << row.empty);
        Sim sim;
        if (row.host) {
            sim.host_to_stun();
        } else {
            sim.player_to_stun();
        }
        sim.stun_done(row.ok, row.empty ? std::vector<StunMapping>{} : two_mappings());
        REQUIRE(sim.count_fail() == 1);
        CHECK(sim.printed.back() == fail_line(FailCode::kStunDiscoveryFailed));
        CHECK(sim.count_log("session.failed") == 0);  // StunClient 가 이미 냈다
        CHECK(sim.lobby.in_lobby());
        CHECK(sim.timers.empty());
        CHECK(sim.count_sent(http::Op::kRegisterCandidate) == 0);
        sim.advance(60000);
        CHECK(sim.sent.size() == 1);
    }
}

// ---------------------------------------------------------------- 플레이어 폴링 (protocol.md 11장)

TEST_CASE("lobby: player polls get_peers 500ms after each response") {
    Sim sim;
    sim.player_polling();
    const Millis registered_at = sim.now;
    REQUIRE(sim.armed(kPollTimer));
    CHECK(sim.timers.find(kPollTimer)->second == registered_at + kGetPeersPollMs);
    CHECK(sim.timers.find(kDeadlineTimer)->second == registered_at + kGetPeersDeadlineMs);

    sim.advance(kGetPeersPollMs - 1);
    CHECK(sim.count_sent(http::Op::kGetPeers) == 0);
    sim.advance(1);
    REQUIRE(sim.count_sent(http::Op::kGetPeers) == 1);
    CHECK(sim.in_flight->self_peer_id == kSelf);

    // 응답이 늦게 와도 다음 폴링은 응답에서 센다.
    sim.advance(2000);
    CHECK(sim.count_sent(http::Op::kGetPeers) == 1);
    sim.reply(peers({unready(kHostId)}));
    const Millis answered = sim.now;
    sim.advance(kGetPeersPollMs);
    REQUIRE(sim.count_sent(http::Op::kGetPeers) == 2);
    CHECK(sim.sent.back().at == answered + kGetPeersPollMs);

    // 일시 오류에는 재시도 규칙이 없다. 다음 폴링이 곧 재시도다.
    sim.reply(err<Peers>(Outcome::kTransient, Failure::kUnavailable));
    CHECK_FALSE(sim.armed(kRetryTimer));
    sim.advance(kGetPeersPollMs);
    REQUIRE(sim.count_sent(http::Op::kGetPeers) == 3);

    // 준비 완료인데 위생 뒤 후보가 없으면 준비 완료가 아니다.
    sim.reply(peers({ready(kHostId, {})}));
    CHECK(sim.count_log("control.peers") == 0);
    sim.advance(kGetPeersPollMs);
    REQUIRE(sim.count_sent(http::Op::kGetPeers) == 4);

    // 상대가 없는 응답도 폴링을 계속한다.
    sim.reply(peers({}));
    sim.advance(kGetPeersPollMs);
    REQUIRE(sim.count_sent(http::Op::kGetPeers) == 5);

    sim.reply(peers({ready(kHostId, {cand("198.51.100.7", 5000), cand("192.168.0.10", 5000)})}));
    REQUIRE(sim.count_log("control.peers") == 1);
    const LogLine* line = sim.last_log("control.peers");
    CHECK(line->fields[0].value == "7");
    CHECK(line->fields[2].value == "198.51.100.7:5000,192.168.0.10:5000");
    CHECK(sim.lobby.stage() == LobbyStage::kSettled);
    CHECK(sim.timers.empty());  // 폴링과 마감을 멈춘다

    // 그 뒤 leave 나 quit 까지 방에 남는다. 더 부르지 않고 실패하지도 않는다.
    sim.advance(600000);
    CHECK(sim.count_sent(http::Op::kGetPeers) == 5);
    CHECK(sim.count_fail() == 0);
    CHECK_FALSE(sim.lobby.in_lobby());
}

TEST_CASE("lobby: the get_peers deadline is 60s from the register response") {
    Sim sim;
    sim.player_polling();
    const Millis registered_at = sim.now;
    // 매 응답이 준비 전이다. 응답은 곧바로 온다.
    while (sim.count_fail() == 0) {
        sim.advance(kGetPeersPollMs);
        if (sim.in_flight) {
            sim.reply(peers({unready(kHostId)}));
        }
        REQUIRE(sim.now <= registered_at + kGetPeersDeadlineMs);
    }
    CHECK(sim.now == registered_at + kGetPeersDeadlineMs);
    CHECK(sim.printed.back() == kCpefLine);
    CHECK(sim.lobby.in_lobby());
}

TEST_CASE("lobby: a deadline with a pending get_peers fails now and drops the late reply") {
    Sim sim;
    sim.player_polling();
    const Millis registered_at = sim.now;
    sim.advance(kGetPeersPollMs);
    REQUIRE(sim.in_flight.has_value());
    sim.advance(kGetPeersDeadlineMs);  // 응답이 오지 않는다
    REQUIRE(sim.count_fail() == 1);
    CHECK(sim.lobby.in_lobby());
    CHECK(sim.lobby.request_pending());
    (void)registered_at;

    sim.command("join ABCDEF");
    CHECK(sim.count_sent(http::Op::kJoinRoom) == 1);  // 미결이라 받지 않는다

    sim.reply(peers({ready(kHostId, {cand("198.51.100.7", 5000)})}));
    CHECK(sim.count_log("control.peers") == 0);
    CHECK(sim.count_log("control.discarded") == 1);
    CHECK(sim.count_fail() == 1);

    sim.command("join ABCDEF");
    CHECK(sim.count_sent(http::Op::kJoinRoom) == 2);
}

TEST_CASE("lobby: a player reply without exactly one peer is a transport error") {
    // 4.5 응답 표가 플레이어의 peers 길이를 1 로 정한다. 다르면 형이 틀린 성공이고 전송 오류다(3.5 의 7번).
    // get_peers 는 재시도 규칙이 없으므로 다음 폴링이 재시도다.
    struct Row {
        std::string_view why;
        std::vector<PeerView> list;
    };
    const Row rows[] = {
        {"two ready peers", {ready(kHostId, {cand("198.51.100.7", 5000)}), ready(8, {cand("198.51.100.8", 5000)})}},
        {"no peer", {}},
        {"two unready peers", {unready(kHostId), unready(8)}},
    };
    for (const auto& row : rows) {
        INFO("why=" << row.why);
        Sim sim;
        sim.player_polling();
        sim.advance(kGetPeersPollMs);
        const Millis answered = sim.now;
        sim.reply(peers(row.list));
        const LogLine* result = sim.last_log("control.result");
        REQUIRE(result != nullptr);
        CHECK(result->level == LogLevel::Warn);
        CHECK(result->fields[0].value == "get_peers");
        CHECK(result->fields[1].value == "false");
        CHECK(result->fields[2].value == "transport");
        CHECK(sim.count_log("control.peers") == 0);
        CHECK(sim.count_fail() == 0);
        CHECK_FALSE(sim.armed(kRetryTimer));
        REQUIRE(sim.armed(kPollTimer));
        CHECK(sim.timers.find(kPollTimer)->second == answered + kGetPeersPollMs);
        sim.advance(kGetPeersPollMs);
        CHECK(sim.count_sent(http::Op::kGetPeers) == 2);
        // 같은 응답이 이어지면 폴링 마감 초과로 끝난다.
        while (sim.count_fail() == 0) {
            if (sim.in_flight) {
                sim.reply(peers(row.list));
            }
            sim.advance(kGetPeersPollMs);
        }
        CHECK(sim.printed.back() == kCpefLine);
        CHECK(sim.lobby.in_lobby());
    }
}

// ---------------------------------------------------------------- 호스트 host_report (4.6)

TEST_CASE("lobby: host_report starts at the create_room response and resets on each send") {
    Sim sim;
    sim.command("host");
    sim.advance(700);
    sim.reply(issued());
    const Millis created = sim.now;
    REQUIRE(sim.armed(kHostReportTimer));
    CHECK(sim.timers.find(kHostReportTimer)->second == created + kHostReportOpenMs);

    // STUN 이 끝나지 않아도 주기는 돈다.
    sim.advance(kHostReportOpenMs);
    REQUIRE(sim.count_sent(http::Op::kHostReport) == 1);
    CHECK(sim.sent.back().at == created + kHostReportOpenMs);
    CHECK(sim.in_flight->self_peer_id == kSelf);
    CHECK(sim.timers.find(kHostReportTimer)->second == sim.now + kHostReportOpenMs);
    sim.reply(report());
    sim.advance(kHostReportOpenMs);
    CHECK(sim.count_sent(http::Op::kHostReport) == 2);
}

TEST_CASE("lobby: a full room reports every 30s") {
    Sim sim;
    sim.host_settled();
    sim.advance(kHostReportOpenMs);
    sim.reply(report({unready(21), unready(22), unready(23), unready(24)}));
    sim.advance(kHostReportOpenMs);
    const Millis full_send = sim.now;
    REQUIRE(sim.count_sent(http::Op::kHostReport) == 2);
    CHECK(sim.timers.find(kHostReportTimer)->second == full_send + kHostReportFullMs);
    sim.reply(report({unready(21), unready(22), unready(23)}));
    sim.advance(kHostReportFullMs - 1);
    CHECK(sim.count_sent(http::Op::kHostReport) == 2);
    sim.advance(1);
    CHECK(sim.count_sent(http::Op::kHostReport) == 3);
    CHECK(sim.timers.find(kHostReportTimer)->second == sim.now + kHostReportOpenMs);
}

TEST_CASE("lobby: host requests wait for the pending one") {
    // register_candidate 가 미결인 동안 host_report 주기가 온다. 응답 직후에 보낸다.
    {
        Sim sim;
        sim.host_to_stun();
        sim.advance(kHostReportOpenMs - 100);
        sim.stun_done(true, two_mappings());
        REQUIRE(sim.in_flight->op == http::Op::kRegisterCandidate);
        sim.advance(200);
        CHECK(sim.count_sent(http::Op::kHostReport) == 0);
        sim.reply(registered());
        REQUIRE(sim.in_flight.has_value());
        CHECK(sim.in_flight->op == http::Op::kHostReport);
        CHECK(sim.timers.find(kHostReportTimer)->second == sim.now + kHostReportOpenMs);
    }
    // host_report 가 미결인 동안 STUN 이 끝난다. register_candidate 는 응답 직후에 보낸다.
    {
        Sim sim;
        sim.host_to_stun();
        sim.advance(kHostReportOpenMs);
        REQUIRE(sim.in_flight->op == http::Op::kHostReport);
        sim.stun_done(true, two_mappings());
        CHECK(sim.count_sent(http::Op::kRegisterCandidate) == 0);
        sim.reply(report());
        REQUIRE(sim.in_flight.has_value());
        CHECK(sim.in_flight->op == http::Op::kRegisterCandidate);
    }
    // register_candidate 재시도 타이머가 host_report 미결 중에 만료한다.
    {
        Sim sim;
        sim.host_to_stun();
        sim.advance(kHostReportOpenMs - 500);
        sim.stun_done(true, two_mappings());
        sim.reply(err<Registered>(Outcome::kTransient, Failure::kInternal));
        const std::string body = sim.sent.back().request.body;
        sim.advance(500);  // host_report 가 나간다
        REQUIRE(sim.in_flight->op == http::Op::kHostReport);
        sim.advance(kRetryIntervalMs);  // 재시도 차례지만 미결이 있다
        CHECK(sim.count_sent(http::Op::kRegisterCandidate) == 1);
        sim.reply(report());
        REQUIRE(sim.in_flight.has_value());
        CHECK(sim.in_flight->op == http::Op::kRegisterCandidate);
        CHECK(sim.in_flight->body == body);
    }
}

TEST_CASE("lobby: host_report first row errors stop reporting and go to the lobby") {
    const Failure failures[] = {Failure::kRoomExpired,   Failure::kRoomNotFound, Failure::kUnauthorized,
                                Failure::kBadRequest,    Failure::kRoomFull,     Failure::kMethodNotAllowed,
                                Failure::kUnknownOp,     Failure::kLengthRequired, Failure::kTooLarge,
                                Failure::kUnknownCode,   Failure::kSelfInPeers};
    for (const bool settled : {true, false}) {
        for (const auto failure : failures) {
            INFO("settled=" << settled << " failure=" << to_token(failure));
            Sim sim;
            if (settled) {
                sim.host_settled();
            } else {
                sim.host_to_stun();  // 방을 세우기 전. STUN 중이다
            }
            sim.advance(kHostReportOpenMs);
            REQUIRE(sim.in_flight->op == http::Op::kHostReport);
            sim.reply(err<HostReport>(Outcome::kDefinite, failure));
            REQUIRE(sim.count_fail() == 1);
            CHECK(sim.printed.back() == kCpefLine);
            CHECK(sim.lobby.in_lobby());
            CHECK_FALSE(sim.lobby.host_reporting());
            CHECK(sim.timers.empty());
            CHECK_FALSE(sim.stun_running);
            CHECK(sim.stun_cancels == (settled ? 0 : 1));
            const auto reports = sim.count_sent(http::Op::kHostReport);
            sim.advance(600000);
            CHECK(sim.count_sent(http::Op::kHostReport) == reports);
            CHECK(sim.count_fail() == 1);
            sim.command("host");  // 로비이므로 다음 방을 만들 수 있다
            CHECK(sim.in_flight->op == http::Op::kCreateRoom);
        }
    }
}

TEST_CASE("lobby: host_report rate_limited and transient errors keep the cycle") {
    struct Row {
        Outcome outcome;
        Failure failure;
    };
    const Row rows[] = {{Outcome::kDefinite, Failure::kRateLimited},
                        {Outcome::kTransient, Failure::kInternal},
                        {Outcome::kTransient, Failure::kUnavailable},
                        {Outcome::kTransient, Failure::kTransport}};
    for (const auto& row : rows) {
        INFO("failure=" << to_token(row.failure));
        Sim sim;
        sim.host_settled();
        sim.advance(kHostReportOpenMs);
        const Millis sent_at = sim.now;
        sim.reply(err<HostReport>(row.outcome, row.failure));
        CHECK(sim.count_fail() == 0);
        CHECK_FALSE(sim.armed(kRetryTimer));
        REQUIRE(sim.armed(kHostReportTimer));
        CHECK(sim.timers.find(kHostReportTimer)->second == sent_at + kHostReportOpenMs);
        sim.advance(kHostReportOpenMs);
        REQUIRE(sim.in_flight.has_value());
        CHECK(sim.count_sent(http::Op::kHostReport) == 2);
        sim.reply(report());
        CHECK(sim.count_fail() == 0);
        CHECK_FALSE(sim.lobby.in_lobby());
    }
}

TEST_CASE("lobby: host confirms unready peers and announces each ready peer once") {
    Sim sim;
    sim.host_settled();
    sim.advance(kHostReportOpenMs);
    CHECK(body_ids(sim.in_flight->body, "confirm").empty());
    CHECK(body_ids(sim.in_flight->body, "departed").empty());

    sim.reply(report({unready(21), ready(22, {cand("198.51.100.22", 6000)}), ready(23, {})}));
    REQUIRE(sim.count_log("control.peers") == 1);
    CHECK(sim.last_log("control.peers")->fields[0].value == "22");

    sim.advance(kHostReportOpenMs);
    CHECK(body_ids(sim.in_flight->body, "confirm") == std::vector<std::int64_t>{21});
    sim.reply(report({ready(21, {cand("198.51.100.21", 6000)}), ready(22, {cand("198.51.100.22", 6000)}),
                      ready(23, {cand("198.51.100.23", 6000)})}));
    CHECK(sim.count_log("control.peers") == 3);  // 21 과 23 이 새로 섰다. 22 는 다시 내지 않는다

    sim.advance(kHostReportOpenMs);
    CHECK(body_ids(sim.in_flight->body, "confirm").empty());
    sim.reply(report({ready(21, {cand("198.51.100.21", 6000)})}));
    CHECK(sim.count_log("control.peers") == 3);
    CHECK(sim.count_fail() == 0);
}

// ---------------------------------------------------------------- 요청 큐 (concurrency.md 8장)

TEST_CASE("lobby: a dropped request ends the attempt") {
    for (const auto op : {http::Op::kCreateRoom, http::Op::kJoinRoom, http::Op::kRegisterCandidate,
                          http::Op::kGetPeers, http::Op::kHostReport}) {
        INFO("op=" << http::op_name(op));
        Sim sim;
        switch (op) {
            case http::Op::kCreateRoom:
                sim.drop_next = true;
                sim.command("host");
                break;
            case http::Op::kJoinRoom:
                sim.drop_next = true;
                sim.command("join ABCDEF");
                break;
            case http::Op::kRegisterCandidate:
                sim.player_to_stun();
                sim.drop_next = true;
                sim.stun_done(true, two_mappings());
                break;
            case http::Op::kGetPeers:
                sim.player_polling();
                sim.drop_next = true;
                sim.advance(kGetPeersPollMs);
                break;
            case http::Op::kHostReport:
                sim.host_settled();
                sim.drop_next = true;
                sim.advance(kHostReportOpenMs);
                break;
        }
        CHECK(sim.dropped == 1);
        REQUIRE(sim.count_fail() == 1);
        CHECK(sim.printed.back() == kCpefLine);
        CHECK(sim.lobby.in_lobby());
        CHECK_FALSE(sim.lobby.request_pending());
        CHECK(sim.timers.empty());
        sim.command("host");  // 미결이 없으므로 받는다
        CHECK(sim.in_flight.has_value());
    }
}

// ---------------------------------------------------------------- 그 밖의 판정

TEST_CASE("lobby: a nonce failure sends nothing and fails") {
    Sim sim;
    sim.fail_nonce = true;
    sim.command("host");
    CHECK(sim.sent.empty());
    REQUIRE(sim.count_fail() == 1);
    CHECK(sim.printed.back() == kCpefLine);
    CHECK(sim.lobby.in_lobby());
    sim.fail_nonce = false;
    sim.command("join ABCDEF");
    CHECK(sim.sent.size() == 1);
}

TEST_CASE("lobby: the room line uses the normalized room id of the response") {
    Sim sim;
    sim.command("join abcdef");
    CHECK(body_string(sim.in_flight->body, "room_id") == "abcdef");
    sim.reply(issued("ABCDEF"));
    CHECK(sim.printed == std::vector<std::string>{"ROOM ABCDEF"});
    sim.stun_done(true, two_mappings());
    CHECK(body_string(sim.in_flight->body, "room_id") == "ABCDEF");
}

TEST_CASE("lobby: replies that do not match are not used") {
    // 미결이 없는데 온 응답.
    {
        Sim sim;
        const auto actions = sim.lobby.on_reply(ControlReply{http::Op::kCreateRoom, issued()});
        REQUIRE(actions.size() == 1);
        const auto* log = std::get_if<LogLine>(&actions[0]);
        REQUIRE(log != nullptr);
        CHECK(log->event == "control.unexpected_reply");
        CHECK(sim.lobby.in_lobby());
    }
    // op 가 미결 요청과 다른 응답.
    {
        Sim sim;
        sim.command("host");
        sim.in_flight.reset();
        const auto actions = sim.lobby.on_reply(ControlReply{http::Op::kGetPeers, peers({})});
        bool printed_fail = false;
        bool started_stun = false;
        for (const auto& a : actions) {
            if (const auto* p = std::get_if<PrintLine>(&a)) {
                printed_fail = printed_fail || p->text == kCpefLine;
            }
            started_stun = started_stun || std::holds_alternative<StartStun>(a);
        }
        CHECK(printed_fail);
        CHECK_FALSE(started_stun);
        CHECK(sim.lobby.in_lobby());
        CHECK_FALSE(sim.lobby.request_pending());
    }
    // op 는 같은데 응답 형이 다르다.
    {
        Sim sim;
        sim.command("host");
        sim.in_flight.reset();
        const auto actions = sim.lobby.on_reply(ControlReply{http::Op::kCreateRoom, registered()});
        CHECK(sim.lobby.in_lobby());
        CHECK_FALSE(sim.lobby.request_pending());
        for (const auto& a : actions) {
            CHECK_FALSE(std::holds_alternative<StartStun>(a));
        }
    }
}

TEST_CASE("lobby: secrets never reach a log line") {
    // Sim 이 행동마다 로그 값을 room_id, peer_token, client_nonce 로 찾는다. 여기서는 모든
    // 경로를 한 번씩 밟는다.
    Sim sim;
    sim.secrets.emplace_back("ABCDEF");
    sim.secrets.emplace_back("abcdef");
    sim.host_settled();
    sim.advance(kHostReportOpenMs);
    sim.reply(report({unready(21), ready(22, {cand("198.51.100.22", 6000)})}));
    sim.advance(kHostReportOpenMs);
    sim.reply(err<HostReport>(Outcome::kDefinite, Failure::kRoomExpired));
    sim.command("join abcdef");
    sim.reply(issued());
    sim.stun_done(true, two_mappings());
    sim.reply(err<Registered>(Outcome::kTransient, Failure::kInternal));
    sim.advance(kRetryIntervalMs);
    sim.reply(registered(1, 1));
    sim.advance(kGetPeersPollMs);
    sim.reply(peers({ready(kHostId, {cand("198.51.100.7", 5000)})}));
    sim.command("leave");
    sim.command("join ZZZZZ0");
    CHECK(sim.count_log("control.peers") == 2);
    // ROOM 줄만 room_id 를 싣는다.
    for (const auto& p : sim.printed) {
        if (p.rfind("FAIL ", 0) == 0) {
            CHECK(p.find("ABCDEF") == std::string::npos);
        }
    }
    // peer_token 과 client_nonce 는 요청 본문 밖으로 나가지 않는다.
    for (const auto& p : sim.printed) {
        CHECK(p.find(kToken) == std::string::npos);
    }
}
