#pragma once

// session_epoch, 시도 nonce, 두 폐기 목록 (protocol.md 4.3 session_epoch, 5.1 HELLO,
// 3장 상수, 11장 타이머의 만료 표).
//
// 이 파일은 값을 뽑고 목록을 들고 있을 뿐이다. 언제 뽑고 언제 등록하는지는 세션이 정한다
// (4.3 의 등록 표, 9.5 재협상 1번과 5번, 9.6 실패 전이).
//
// probe nonce(10.4 (c)) 와 그 표는 여기에 없다. 5.1 이 두 종류를 같은 표에 섞지 말라고
// 정했으므로 시도 nonce 의 폐기 목록(RetiredNonces)을 probe 쪽에서 쓰지 않는다.

#include "sangtachi/protocol_constants.hpp"
#include "sangtachi/timer.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace sangtachi::peer {

// 난수원. 참이면 out 전체를 채웠다.
//
// **주입하는 이유는 둘이다** (roadmap.md Phase 4 검증의 session_epoch 난수원). (1) 0 을
// 돌려주는 난수원으로 다시 뽑기를 확률이 아니라 값으로 본다. (2) 실패를 돌려주는 난수원으로
// 실패 경로를 본다. 비워 두면 platform::random_bytes(BCryptGenRandom) 다. 제품 경로는 비워
// 둔다. 기본값을 고르는 자리는 epoch.cpp 의 한 곳이다.
using RandomFn = std::function<bool(std::span<std::byte>)>;

// 폐기 목록 항목의 보관 시간 (4.3, 5.1, 11장 만료 표). 값은 protocol_constants.hpp 의 11장
// 블록이 갖는다. 같은 값을 두 곳에 두지 않는다.
inline constexpr Millis kRetiredEpochTtlMs = protocol::kRetiredEpochTtlMs;
inline constexpr Millis kRetiredNonceTtlMs = protocol::kRetiredNonceTtlMs;

// 새 session_epoch 를 뽑는다 (4.3). 0 이 나오면 다시 뽑는다. 0 은 쓰지 않는다.
//
// 난수원이 실패하면 nullopt 다. 0 이나 고정값으로 대신하지 않는다. 예측 가능한 epoch 는
// 2장의 위협 전제를 그만큼 낮춘다. 실패를 어떻게 처리할지는 문서가 정하지 않았으므로
// 부르는 쪽에 넘긴다.
//
// **다시 뽑는 횟수에 상한을 두지 않는다.** 문서가 상한을 정하지 않았다. 정상 난수원에서
// 0 이 나올 확률은 2^-32 다. 늘 0 만 성공으로 돌려주는 난수원은 이 함수를 끝내지 못한다.
[[nodiscard]] std::optional<std::uint32_t> generate_session_epoch(const RandomFn& random = {});

// 폐기된 상대 epoch 목록 (4.3). 8.1 검사 8번이 조회한다.
//
// **넣는 것은 상대의 epoch 다.** 검사 8번이 보는 것은 수신 패킷의 session_epoch, 즉
// 송신자의 값이다. 우리 자신의 지난 epoch 를 넣으면 아무것도 거르지 못한다. 이 클래스는
// 값만 받으므로 그것을 막지 못한다. 등록 자리 둘(4.3 의 표)이 상대 값을 넘겨야 한다.
//
// **상한이 2분 보관보다 먼저다.** 가득 찬 상태에서 새 항목이 오면 가장 오래 등록된 것을
// 밀어낸다. 새 항목을 거부하지 않는다. 그래서 2분은 best-effort 다 (4.3).
class RetiredEpochs {
public:
    // 등록한다. 만료한 항목을 먼저 치우고, 그래도 MAX_RETIRED_EPOCHS 개면 가장 오래된
    // 것을 밀어낸 뒤 넣는다.
    //
    // 이미 살아 있는 값을 다시 넣으면 그 항목을 지우고 맨 뒤에 새로 넣는다. 보관 2분이
    // 그 시각부터 다시 센다. 같은 값이 두 칸을 차지하면 상한 16칸이 그만큼 줄어든다.
    // **이 동작은 문서가 정하지 않았다.**
    void add(std::uint32_t epoch, Millis now);

    // 살아 있는 항목에 epoch 가 있는가. 만료 비교는 `등록 시각 + 2분 <= now` 다 (11장
    // 타이머의 마감 비교와 같은 부등호). 상태를 바꾸지 않는다.
    [[nodiscard]] bool contains(std::uint32_t epoch, Millis now) const noexcept;

    // 만료한 항목을 치운다. add 가 스스로 부르므로 따로 부르지 않아도 된다.
    void expire(Millis now);

    // 들고 있는 항목 수. 만료했지만 아직 치우지 않은 항목도 센다.
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

private:
    struct Entry {
        std::uint32_t epoch = 0;
        Millis retired_at = 0;
    };
    // 등록 순서대로다. 앞이 가장 오래된 것이다.
    std::deque<Entry> entries_;
};

// HELLO 의 nonce 필드 하나 (5.1). 128비트다.
struct Nonce {
    static constexpr std::size_t kBytes = 16;
    std::array<std::byte, kBytes> bytes{};

    friend bool operator==(const Nonce&, const Nonce&) = default;
};

// 이전 시도의 nonce 목록 (5.1). 새 시도의 nonce 가 이전 것을 다시 쓰지 않게 한다.
//
// **상한이 없다.** 3장이 정한 대로 항목은 우리가 새 시도를 시작할 때만 생기고 2분 뒤
// 사라지므로 상대가 키울 수 없다. 상한을 걸 대상이 아니다.
class RetiredNonces {
public:
    // 등록한다. 만료한 항목을 먼저 치운다.
    void add(const Nonce& nonce, Millis now);

    // 살아 있는 항목에 있는가. 만료 비교는 RetiredEpochs::contains 와 같다.
    [[nodiscard]] bool contains(const Nonce& nonce, Millis now) const noexcept;

    void expire(Millis now);

    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

private:
    struct Entry {
        Nonce nonce;
        Millis retired_at = 0;
    };
    std::vector<Entry> entries_;
};

// 새 시도의 nonce 를 뽑는다 (5.1). 시도당 한 번 부르고, 그 시도의 HELLO 재전송은 이 값을
// 그대로 쓴다. 9.5 재협상은 다시 부르지 않는다 (9.5 5번).
//
// 뽑은 값이 retired 에 살아 있으면 다시 뽑는다. 5.1 의 "재사용하지 않는다" 다. 정상
// 난수원에서 겹칠 확률은 사실상 0 이지만, 규칙을 확률에 맡기지 않는다.
//
// 난수원이 실패하면 nullopt 다. 다시 뽑는 횟수에 상한이 없는 것은 generate_session_epoch 와
// 같은 이유다.
[[nodiscard]] std::optional<Nonce> generate_attempt_nonce(const RetiredNonces& retired, Millis now,
                                                          const RandomFn& random = {});

}  // namespace sangtachi::peer
