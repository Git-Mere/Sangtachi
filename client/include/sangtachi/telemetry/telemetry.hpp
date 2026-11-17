#pragma once

// 텔레메트리 레코드 (architecture.md 6.3 텔레메트리 서비스 인터페이스, 9장 텔레메트리 업로드).
//
// **순수 코드다.** 레코드의 값, 세션마다 세는 값(바이트, RTT 표본, 지터), JSON 본문을 만든다. 큐와 업로드는
// telemetry/uploader.hpp 다. 여기 있는 것은 모두 `[loop]` 가 부른다 (concurrency.md 5장 상태 소유).
//
// | 자리 | 정한 것 | 근거 |
// |------|---------|------|
// | 지터의 정수 | 실수로 들고 레코드에 실을 때 가장 가까운 정수로 반올림한다 | 6.3 `jitter_us` 는 정수다. 식(9장)을 정수 나눗셈으로 돌리면 차이 16 미만이 0 으로 사라진다 |
// | 구간의 기점 | `start` 를 처음 부른 때. 세션이 처음 `CONNECTED` 에 들어간 때다 | 9장 "첫 구간은 처음 CONNECTED 에 들어간 때부터다" |
// | 기점 전의 바이트 | 구간에 들지 않고 세션 전체(`total_bytes_*`)에는 든다 | 9장 세션 바이트, 6.3 `end` 의 "그 세션의 바이트 전체" |

#include "sangtachi/timer.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sangtachi::telemetry {

// architecture.md 9장 텔레메트리 업로드의 TELEMETRY_INTERVAL_S.
inline constexpr Millis kIntervalMs = 60'000;
// 6.3 `rtt_us` 의 최대 개수 (9장 "RTT 표본은 구간마다 16개까지").
inline constexpr std::size_t kMaxRttSamples = 16;

enum class Kind : std::uint8_t { kEstablish, kInterval, kEnd };

[[nodiscard]] std::string_view to_token(Kind kind) noexcept;

// `interval` 과 `end` 의 구간 필드 (6.3).
struct IntervalValues {
    std::uint64_t interval_ms = 0;
    std::vector<std::uint64_t> rtt_us;
    std::uint64_t rtt_overflow = 0;
    std::optional<std::uint64_t> jitter_us;
    std::uint64_t expected = 0;
    std::uint64_t accepted = 0;
    std::uint64_t bytes_out = 0;
    std::uint64_t bytes_in = 0;
};

// `end` 에만 있는 필드 가운데 세션 전체의 값 (6.3).
struct SessionTotals {
    std::uint64_t duration_ms = 0;
    std::uint64_t total_expected = 0;
    std::uint64_t total_accepted = 0;
    std::uint64_t total_bytes_out = 0;
    std::uint64_t total_bytes_in = 0;
    std::uint64_t total_tx_err_send = 0;
};

// 레코드 하나. 프로세스마다 같은 필드(instance_id, os, ui, build_kind)는 Stamp 가 갖는다.
struct Record {
    Kind kind = Kind::kEstablish;
    std::uint64_t attempt = 0;
    std::uint64_t seq = 0;
    std::string_view role;                 // "host" | "player"
    std::string room_id;                   // 비면 null
    std::optional<std::uint32_t> peer_id;  // 자기 peer_id
    std::optional<std::uint32_t> peer;     // 상대 peer_id

    // establish, end. establish 는 수립 줄의 결과, end 는 종료 줄의 결과다.
    std::string_view result;

    // establish
    std::optional<std::uint64_t> setup_ms;
    std::optional<bool> local_differs_public;
    std::optional<bool> stun_same_endpoint;
    std::optional<bool> same_lan;

    // interval, end
    IntervalValues interval;
    // end
    SessionTotals totals;
};

struct Stamp {
    std::string instance_id;  // 16진수 32자 (make_instance_id)
    std::string_view os;      // "windows" | "linux"
    std::string_view ui;      // "gui" | "console"
    std::string_view build_kind;  // "release" | "test"
};

// 6.3 의 JSON 객체 하나. kind 의 표에 있는 필드만 싣는다.
[[nodiscard]] std::string to_json(const Record& record, const Stamp& stamp);

// 난수 바이트를 채운다. 다 채웠으면 참이다. 제품 경로는 platform::random_bytes 다.
using RandomFn = std::function<bool(std::span<std::byte>)>;

// 6.3 `instance_id`. 128비트를 소문자 16진수 32자로 적는다. 난수원이 실패하면 값이 없다.
[[nodiscard]] std::optional<std::string> make_instance_id(const RandomFn& random);

// 세션 하나가 세는 값 (9장 세션 바이트, 지터, RTT 표본). 세션 객체가 들고 `[loop]` 만 만진다.
class SessionMeter {
public:
    // 그 세션의 상대에게 sendto 가 성공한 UDP 페이로드 바이트.
    void add_out(std::uint64_t bytes) noexcept { bytes_out_ += bytes; }
    // protocol.md 8.1 검사 7번을 지나 그 세션으로 정해진 데이터그램의 UDP 페이로드 바이트.
    void add_in(std::uint64_t bytes) noexcept { bytes_in_ += bytes; }
    // RTT 표본 하나. 지터를 고치고 이 구간의 표본에 넣는다. 16개를 넘으면 버리고 센다.
    void add_rtt(std::uint64_t rtt_us);

    // 처음 CONNECTED 에 들어갔다. 구간의 기점을 잡는다. 두 번째부터는 아무것도 하지 않는다.
    // expected, accepted 는 그때의 protocol.md 4.5 세션 값이다.
    void start(Millis now, std::uint64_t expected, std::uint64_t accepted);
    [[nodiscard]] std::optional<Millis> started_at() const noexcept { return started_at_; }

    // 앞 레코드부터 지금까지의 구간 값을 내고 기점을 지금으로 옮긴다. start 전이면 길이 0 의 빈 구간이다.
    [[nodiscard]] IntervalValues take_interval(Millis now, std::uint64_t expected, std::uint64_t accepted);

    [[nodiscard]] std::uint64_t bytes_out() const noexcept { return bytes_out_; }
    [[nodiscard]] std::uint64_t bytes_in() const noexcept { return bytes_in_; }
    // 9장 지터. 세션의 표본이 둘보다 적으면 없다.
    [[nodiscard]] std::optional<std::uint64_t> jitter_us() const noexcept;

private:
    std::uint64_t bytes_out_ = 0;
    std::uint64_t bytes_in_ = 0;

    std::uint64_t samples_ = 0;
    std::uint64_t last_rtt_us_ = 0;
    double jitter_ = 0.0;

    std::optional<Millis> started_at_;
    Millis mark_ms_ = 0;
    std::uint64_t mark_expected_ = 0;
    std::uint64_t mark_accepted_ = 0;
    std::uint64_t mark_bytes_out_ = 0;
    std::uint64_t mark_bytes_in_ = 0;
    std::vector<std::uint64_t> rtt_;
    std::uint64_t rtt_overflow_ = 0;
};

}  // namespace sangtachi::telemetry
