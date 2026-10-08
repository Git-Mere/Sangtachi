#include "sangtachi/peer/epoch.hpp"

#include "sangtachi/platform/random.hpp"
#include "sangtachi/protocol_constants.hpp"

#include <algorithm>
#include <utility>

namespace sangtachi::peer {

namespace {

// 비워 둔 난수원의 기본값을 고르는 자리는 여기 하나다. 제품 경로는 이것을 탄다.
// platform::random_bytes 는 BCryptGenRandom 이다 (platform/win32/random.cpp).
// 4.3 과 5.1 이 정한 출처가 그것이다.
bool draw(const RandomFn& random, std::span<std::byte> out) {
    if (random) {
        return random(out);
    }
    return platform::random_bytes(out);
}

// 11장 타이머의 마감 비교와 같은 부등호다. 정확히 2분이 된 순간 만료한다.
// 시계가 등록 시각보다 뒤로 가 있으면 만료로 보지 않는다. 단조 시계에서는 일어나지 않는다.
constexpr bool expired(Millis retired_at, Millis ttl, Millis now) noexcept {
    return now >= retired_at && now - retired_at >= ttl;
}

}  // namespace

std::optional<std::uint32_t> generate_session_epoch(const RandomFn& random) {
    for (;;) {
        std::array<std::byte, 4> bytes{};
        if (!draw(random, bytes)) {
            return std::nullopt;
        }
        const std::uint32_t value = (std::to_integer<std::uint32_t>(bytes[0]) << 24) |
                                    (std::to_integer<std::uint32_t>(bytes[1]) << 16) |
                                    (std::to_integer<std::uint32_t>(bytes[2]) << 8) |
                                    std::to_integer<std::uint32_t>(bytes[3]);
        // 0 은 쓰지 않는다 (4.3). 다시 뽑는다.
        if (value != 0) {
            return value;
        }
    }
}

// ---------------------------------------------------------------- RetiredEpochs

void RetiredEpochs::add(std::uint32_t epoch, Millis now) {
    expire(now);

    // 이미 있는 값은 지우고 맨 뒤에 새로 넣는다 (epoch.hpp 의 add 설명).
    const auto same = std::find_if(entries_.begin(), entries_.end(),
                                   [epoch](const Entry& e) { return e.epoch == epoch; });
    if (same != entries_.end()) {
        entries_.erase(same);
    }

    // 상한이 2분 보관보다 먼저다 (4.3). 가득 차 있으면 가장 오래된 것을 밀어낸다.
    // 새 항목을 거부하지 않는다.
    while (entries_.size() >= protocol::kMaxRetiredEpochs) {
        entries_.pop_front();
    }
    entries_.push_back(Entry{epoch, now});
}

bool RetiredEpochs::contains(std::uint32_t epoch, Millis now) const noexcept {
    return std::any_of(entries_.begin(), entries_.end(), [&](const Entry& e) {
        return e.epoch == epoch && !expired(e.retired_at, kRetiredEpochTtlMs, now);
    });
}

void RetiredEpochs::expire(Millis now) {
    std::erase_if(entries_, [now](const Entry& e) {
        return expired(e.retired_at, kRetiredEpochTtlMs, now);
    });
}

// ---------------------------------------------------------------- RetiredNonces

void RetiredNonces::add(const Nonce& nonce, Millis now) {
    expire(now);
    // 상한이 없다 (3장). 같은 값이 이미 있어도 그대로 하나 더 둔다. 우리가 뽑은 nonce 는
    // generate_attempt_nonce 가 겹치지 않게 하므로 겹친 등록은 부르는 쪽의 결함이고,
    // 둘 다 같은 판정(contains)을 내므로 해가 없다.
    entries_.push_back(Entry{nonce, now});
}

bool RetiredNonces::contains(const Nonce& nonce, Millis now) const noexcept {
    return std::any_of(entries_.begin(), entries_.end(), [&](const Entry& e) {
        return e.nonce == nonce && !expired(e.retired_at, kRetiredNonceTtlMs, now);
    });
}

void RetiredNonces::expire(Millis now) {
    std::erase_if(entries_, [now](const Entry& e) {
        return expired(e.retired_at, kRetiredNonceTtlMs, now);
    });
}

std::optional<Nonce> generate_attempt_nonce(const RetiredNonces& retired, Millis now,
                                            const RandomFn& random) {
    for (;;) {
        Nonce nonce;
        if (!draw(random, nonce.bytes)) {
            return std::nullopt;
        }
        // 이전 시도의 nonce 를 다시 쓰지 않는다 (5.1).
        if (!retired.contains(nonce, now)) {
            return nonce;
        }
    }
}

}  // namespace sangtachi::peer
