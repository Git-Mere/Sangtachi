#pragma once

// 수용 창 (protocol.md 4.4 sequence, 4.5 중복 억제와 손실 집계, 8.3 중복 억제).
//
// 이 파일이 갖는 것은 **64칸 수용 창** 하나다. 4.5 의 손실 집계와 baseline 은 세션(peer/session)이
// 갖는다 (protocol.md 15장 구현 적합성 목록: "수용 창과 이 집계는 다른 물건이다"). 그래도 판정이
// pkt_pos 를 같이 내는 것은 그 값이 4.5 의사 코드의 분기마다 정해지기 때문이다.
//
// 상태 하나는 상대 epoch 하나에 대한 것이다. 어느 epoch 의 상태로 판정하는지, 9.5 재협상
// 3번에서 언제 reset 하는지는 부르는 쪽(세션)이 정한다 (protocol.md 8.3).

#include "sangtachi/protocol_constants.hpp"

#include <cstdint>

namespace sangtachi::tunnel {

// a 가 b 보다 엄격히 새로운가. a == b 는 거짓이다 (protocol.md 4.4 sequence).
//
// `a != b` 를 빼면 같은 번호가 새 패킷으로 분류되어 재생 방어가 무력화된다.
// 뺄셈은 uint32_t 에서 순환하므로 0xFFFFFFFF 다음의 0 이 새롭다.
[[nodiscard]] constexpr bool is_newer(std::uint32_t a, std::uint32_t b) noexcept {
    return a != b && static_cast<std::uint32_t>(a - b) < 0x80000000u;
}

// 판정 결과. 앞의 둘이 수용, 뒤의 둘이 폐기다. 폐기 둘의 카운터 이름은 8.3 이 정한다
// (drop_duplicate, drop_too_old). 카운터를 올리는 것은 부르는 쪽이다.
enum class ReplayVerdict : std::uint8_t {
    kAdvance,    // 수용. highest 를 전진시킨다. 첫 패킷도 여기다 (4.5)
    kReorder,    // 수용. 창 안의 처음 보는 오래된 번호. highest 는 그대로다
    kDuplicate,  // 폐기, drop_duplicate
    kTooOld,     // 폐기, drop_too_old
};

// check 가 돌려주는 판정 하나. commit 이 이것을 그대로 받는다.
struct ReplayDecision {
    ReplayVerdict verdict = ReplayVerdict::kDuplicate;
    std::uint32_t seq = 0;
    // kAdvance 이면 shift (1 .. 2^31-1. 첫 패킷이면 0), kReorder 이면 back (1 .. 64).
    // 폐기면 뜻이 없다.
    std::uint32_t distance = 0;
    // 이 패킷의 위치 (4.5 pkt_pos). 수용일 때만 뜻이 있다.
    std::uint64_t pkt_pos = 0;

    [[nodiscard]] constexpr bool accepted() const noexcept {
        return verdict == ReplayVerdict::kAdvance || verdict == ReplayVerdict::kReorder;
    }
    // 10.4 엔드포인트 학습 (b) 가 보는 값이다. 재정렬 수용은 전진이 아니다.
    [[nodiscard]] constexpr bool advanced() const noexcept {
        return verdict == ReplayVerdict::kAdvance;
    }

    friend constexpr bool operator==(const ReplayDecision&, const ReplayDecision&) = default;
};

// 4.5 의 창 상태.
//
// **판정과 상태 갱신을 둘로 나눈다.** check 는 상태를 바꾸지 않고, commit 이 바꾼다.
// 4.5 는 "이 판정은 어떤 부수 효과보다 먼저 온다" 고 정했다. 판정이 상태를 바꾸지 않아야
// 부르는 쪽이 판정만 보고 다른 검사를 끼울 수 있다. 문서가 정한 자리(8.3, 8.4 보다 앞)에서
// 곧바로 갱신하려면 accept 를 쓴다. accept 는 check 와 commit 을 이어 부른 것과 같다.
//
// 비트 연산은 전부 uint64_t 다 (4.5 경계 넷째). 창 크기와 비트맵 폭은 같이 움직인다.
class ReplayWindow {
public:
    // 4.5 의 판정. 상태를 바꾸지 않는다.
    [[nodiscard]] ReplayDecision check(std::uint32_t seq) const noexcept;

    // check 의 판정을 상태에 반영한다. 반영했으면 참이다.
    //
    // 거짓인 경우는 둘이다. 판정이 폐기였거나, 판정을 낸 뒤 상태가 바뀌어 그 판정이 더는
    // 지금 상태의 것이 아니다. 둘째를 그대로 반영하면 다른 상태에서 잰 shift 와 back 으로
    // 비트맵을 밀어 창이 조용히 망가진다. 그래서 지금 상태로 다시 판정해 같을 때만 반영한다.
    [[nodiscard]] bool commit(const ReplayDecision& decision) noexcept;

    // check 뒤 수용이면 바로 commit 한다. 8.3 자리의 정상 경로다.
    ReplayDecision accept(std::uint32_t seq) noexcept;

    // 처음 상태로 되돌린다. 9.5 재협상 3번 "중복 억제 상태 초기화" 가 부른다.
    void reset() noexcept;

    [[nodiscard]] bool initialized() const noexcept { return initialized_; }
    [[nodiscard]] std::uint32_t highest() const noexcept { return highest_; }
    [[nodiscard]] std::uint64_t position() const noexcept { return position_; }
    [[nodiscard]] std::uint64_t seen_bits() const noexcept { return seen_bits_; }

private:
    std::uint32_t highest_ = 0;    // 지금까지 받은 최대 sequence (32비트 순환)
    std::uint64_t position_ = 0;   // highest 의 순환 없는 64비트 확장 위치
    std::uint64_t seen_bits_ = 0;  // back=1..64 의 수신 비트맵. back=b 는 비트 (b-1)
    bool initialized_ = false;
};

// seen_bits 가 uint64_t 이므로 창은 64칸이어야 한다 (4.5 마지막 문단). 상수만 고치면
// 시프트가 미정의 동작이 되므로 여기서 빌드를 멈춘다.
static_assert(protocol::kReplayWindow == 64, "seen_bits 폭과 REPLAY_WINDOW 가 어긋났다");

}  // namespace sangtachi::tunnel
