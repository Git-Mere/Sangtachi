#include "sangtachi/tunnel/roster_publisher.hpp"

#include "sangtachi/log.hpp"
#include "sangtachi/protocol_constants.hpp"

#include <algorithm>
#include <array>
#include <span>
#include <string_view>
#include <utility>

namespace sangtachi::tunnel {
namespace {

RosterMember member(const RosterPeer& p, bool connected) {
    RosterMember m;
    m.peer_id = p.peer_id;
    m.virtual_ip = p.virtual_ip;
    m.connected = connected;
    m.name_len = static_cast<std::uint8_t>(std::min(p.name.size(), m.name.size()));
    std::copy_n(p.name.begin(), m.name_len, m.name.begin());
    return m;
}

void emit_tick(std::string_view kind, std::uint32_t peer_id, Millis now, Millis since) {
    const std::string name =
        std::string("roster.") + std::string(kind) + "." + std::to_string(peer_id);
    const LogField fields[] = {
        field("name", name),
        field("elapsed_ms", static_cast<std::uint64_t>(now >= since ? now - since : 0)),
    };
    emit(LogLevel::Info, "timer.tick", fields);
}

}  // namespace

void RosterPublisher::start(const RosterPeer& self) {
    self_ = self;
    peers_.clear();
    generation_ = 0;
    schedules_.clear();
    last_ = build({});  // 0 번 명부는 호스트 혼자다 (5.7). 그 뒤의 set_peers 는 변화다
    forced_change_ = false;
    dirty_ = true;
}

void RosterPublisher::on_connected_change(std::uint32_t peer_id, bool entered) {
    if (!self_) {
        return;
    }
    if (!entered) {
        schedules_.erase(peer_id);  // 11장 취소: 세션 종료. 같은 바퀴에 돌아와도 진입으로 본다
    }
    // 멤버의 비트 0 이 바뀌었다. 같은 바퀴에 나갔다 들어와 내용이 그대로 보여도 변화다.
    if (std::any_of(peers_.begin(), peers_.end(),
                    [peer_id](const RosterPeer& p) { return p.peer_id == peer_id; })) {
        forced_change_ = true;
    }
    dirty_ = true;
}

void RosterPublisher::stop() {
    self_.reset();
    peers_.clear();
    schedules_.clear();
    last_.reset();
    forced_change_ = false;
    dirty_ = false;
}

void RosterPublisher::set_peers(std::vector<RosterPeer> peers) {
    peers_ = std::move(peers);
    dirty_ = true;
}

Roster RosterPublisher::build(const Connected& connected) const {
    auto is_connected = [&connected](std::uint32_t peer_id) {
        return std::any_of(connected.begin(), connected.end(),
                           [peer_id](const peer::Session* s) { return s->peer_id() == peer_id; });
    };
    std::vector<RosterMember> members;
    members.push_back(member(*self_, true));  // 호스트 자신은 비트 0 이 늘 1 (5.7)
    for (const auto& p : peers_) {
        members.push_back(member(p, is_connected(p.peer_id)));
    }
    Roster r;
    // 다섯을 넘으면 count 를 그대로 적는다. encode_roster 가 거절해 보내지 않는다 (머리 표).
    r.count = static_cast<std::uint8_t>(std::min<std::size_t>(members.size(), 255));
    for (std::size_t i = 0; i < members.size() && i < r.members.size(); ++i) {
        r.members[i] = members[i];
    }
    return r;
}

void RosterPublisher::send(peer::Session& session, Schedule& schedule, Millis now) {
    Roster r = *last_;
    r.generation = generation_;
    std::array<std::byte, 5 + protocol::kMaxPeers * (10 + protocol::kMaxNameLen)> buffer{};
    const std::size_t length = encode_roster(r, buffer);
    if (length != 0) {
        session.send_roster(std::span<const std::byte>(buffer.data(), length));
    }
    // 11장: 주기는 매 송신으로 다시 건다.
    schedule.period_due = now + protocol::kRosterIntervalMs;
    schedule.period_since = now;
}

void RosterPublisher::refresh(Millis now, const Connected& connected) {
    // CONNECTED 를 떠난 세션의 두 타이머를 지운다 (11장 취소: 세션 종료).
    std::erase_if(schedules_, [&connected](const auto& entry) {
        return std::none_of(connected.begin(), connected.end(), [&entry](const peer::Session* s) {
            return s->peer_id() == entry.first;
        });
    });
    const Roster roster = build(connected);
    const bool changed = forced_change_ || !(roster == *last_);
    forced_change_ = false;
    last_ = roster;
    if (changed) {
        ++generation_;
    }
    for (peer::Session* s : connected) {
        const auto [it, entered] = schedules_.try_emplace(s->peer_id());
        if (changed) {
            // 5.7 첫 행: 모든 CONNECTED 세션에 즉시 1회, 그 뒤 5초 간격 2회. 진입한 세션도 이 한 번이다.
            send(*s, it->second, now);
            it->second.resends_left = protocol::kRosterResendCount;
            it->second.resend_due = now + protocol::kRosterResendIntervalMs;
            it->second.resend_since = now;
        } else if (entered) {
            send(*s, it->second, now);  // 내용이 그대로인 진입. 11장 "진입 즉시 1회"
        }
    }
}

std::optional<Millis> RosterPublisher::earliest() const noexcept {
    if (!self_) {
        return std::nullopt;
    }
    if (dirty_) {
        return Millis{0};
    }
    std::optional<Millis> e;
    for (const auto& [id, s] : schedules_) {
        for (const auto& d : {s.period_due, s.resend_due}) {
            if (d && (!e || *d < *e)) {
                e = d;
            }
        }
    }
    return e;
}

void RosterPublisher::run(Millis now, const Connected& connected) {
    if (!self_) {
        return;
    }
    if (dirty_) {
        dirty_ = false;
        refresh(now, connected);
    }
    for (peer::Session* s : connected) {
        const auto it = schedules_.find(s->peer_id());
        if (it == schedules_.end()) {
            continue;  // 다음 refresh 가 넣는다
        }
        Schedule& sch = it->second;
        // 마감 비교는 11장과 같은 `<=` 다. 재송이 보내면 주기가 다시 걸리므로 같은 바퀴에 두 번 나가지 않는다.
        if (sch.resend_due && *sch.resend_due <= now) {
            emit_tick("resend", s->peer_id(), now, sch.resend_since);
            send(*s, sch, now);
            --sch.resends_left;
            sch.resend_since = now;
            sch.resend_due = sch.resends_left > 0
                                 ? std::optional<Millis>(now + protocol::kRosterResendIntervalMs)
                                 : std::nullopt;
        }
        if (sch.period_due && *sch.period_due <= now) {
            emit_tick("period", s->peer_id(), now, sch.period_since);
            send(*s, sch, now);
        }
    }
}

}  // namespace sangtachi::tunnel
