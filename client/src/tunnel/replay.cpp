#include "sangtachi/tunnel/replay.hpp"

namespace sangtachi::tunnel {

namespace {

// 4.5 의사 코드의 리터럴 64 와 63 이다. 창과 비트맵 폭이 같이 움직인다 (replay.hpp 의
// static_assert).
constexpr std::uint32_t kWindow = static_cast<std::uint32_t>(protocol::kReplayWindow);

constexpr std::uint64_t bit_for_back(std::uint32_t back) noexcept {
    // back=b 는 비트 (b-1) 다. 부르는 쪽이 1 <= back <= 64 를 보장한다.
    // uint64_t 로 민다. 1 << 63 은 int 시프트라 미정의 동작이다 (4.5 경계 넷째).
    return std::uint64_t{1} << (back - 1);
}

}  // namespace

ReplayDecision ReplayWindow::check(std::uint32_t seq) const noexcept {
    ReplayDecision d;
    d.seq = seq;

    if (!initialized_) {
        // 첫 패킷은 무엇이든 수용하고 위치 REPLAY_WINDOW 에 둔다. back 은 창 크기 이하이므로
        // 아래 재정렬의 position - back 이 0 아래로 내려가지 않는다 (4.5).
        d.verdict = ReplayVerdict::kAdvance;
        d.distance = 0;
        d.pkt_pos = kWindow;
        return d;
    }

    // **seq == highest 를 먼저 거른다** (4.5 경계 첫째). is_newer 는 a == b 에서 거짓이라
    // 이 경우가 아래 오래된 쪽으로 내려가 back == 0 이 되고, 1 << (0-1) 이 미정의 동작이다.
    if (seq == highest_) {
        d.verdict = ReplayVerdict::kDuplicate;
        return d;
    }

    if (is_newer(seq, highest_)) {
        // 전진. shift 는 1 .. 2^31-1 이다.
        const std::uint32_t shift = static_cast<std::uint32_t>(seq - highest_);
        d.verdict = ReplayVerdict::kAdvance;
        d.distance = shift;
        d.pkt_pos = position_ + shift;
        return d;
    }

    // 오래됨. back 은 1 .. 2^31 이다.
    const std::uint32_t back = static_cast<std::uint32_t>(highest_ - seq);
    d.distance = back;
    if (back > kWindow) {
        d.verdict = ReplayVerdict::kTooOld;
        return d;
    }
    if ((seen_bits_ & bit_for_back(back)) != 0) {
        d.verdict = ReplayVerdict::kDuplicate;
        return d;
    }
    d.verdict = ReplayVerdict::kReorder;
    // 4.5 의 pkt_pos = position - back 그대로다. position 은 REPLAY_WINDOW 에서 시작하고
    // back 은 그 이하이므로 감싸 돌지 않는다. 손실 집계(Phase 5)가 이 값을 쓴다.
    d.pkt_pos = position_ - back;
    return d;
}

bool ReplayWindow::commit(const ReplayDecision& decision) noexcept {
    if (!decision.accepted()) {
        return false;
    }
    // 지금 상태로 다시 판정해 같을 때만 반영한다. 다른 상태에서 잰 shift 나 back 으로
    // 비트맵을 밀면 창이 조용히 망가진다 (replay.hpp 의 commit 설명).
    if (check(decision.seq) != decision) {
        return false;
    }

    if (!initialized_) {
        highest_ = decision.seq;
        position_ = kWindow;
        seen_bits_ = 0;
        initialized_ = true;
        return true;
    }

    if (decision.verdict == ReplayVerdict::kAdvance) {
        const std::uint32_t shift = decision.distance;
        // **세 분기는 상호 배타다** (4.5 경계 둘째). if / if / else 로 쓰면 shift > 64 가
        // 비트맵을 지운 뒤 else 로도 들어가 64 를 넘는 시프트를 한다.
        if (shift > kWindow) {
            seen_bits_ = 0;
        } else if (shift == kWindow) {
            // **지우지 않는다** (4.5 경계 셋째). 직전 highest 가 back == 64 가 되므로 비트
            // 63 을 세운다. seen_bits << 64 는 미정의 동작이라 따로 처리한다.
            seen_bits_ = bit_for_back(kWindow);
        } else {
            seen_bits_ = (seen_bits_ << shift) | bit_for_back(shift);
        }
        highest_ = decision.seq;
        position_ += shift;
        return true;
    }

    // 재정렬 수용. highest 와 position 은 그대로다.
    seen_bits_ |= bit_for_back(decision.distance);
    return true;
}

ReplayDecision ReplayWindow::accept(std::uint32_t seq) noexcept {
    const ReplayDecision d = check(seq);
    if (d.accepted()) {
        // 방금 같은 상태로 판정했으므로 반영이 거절될 일이 없다.
        (void)commit(d);
    }
    return d;
}

void ReplayWindow::reset() noexcept {
    highest_ = 0;
    position_ = 0;
    seen_bits_ = 0;
    initialized_ = false;
}

}  // namespace sangtachi::tunnel
