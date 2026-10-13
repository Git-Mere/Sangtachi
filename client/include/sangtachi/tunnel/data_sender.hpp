#pragma once

// 콘솔 `send <n> [size] [to]` 과 합성 내부 패킷 (roadmap.md Phase 4 의 콘솔 명령과 100바이트
// 레이아웃, Phase 5 크기 표의 단서).
//
// 명령 하나가 작업 하나를 만들고, 루프가 바퀴마다 `pump` 로 MAX_DRAIN 개씩 나눠 보낸다
// (concurrency.md 3장 "콘솔이 만든 송신 잔량은 busy 에 포함한다"). 패킷마다 터널의 8.5 를 지난다.
//
// ## 명령 단계에서 거르는 것 (`console.rejected` WARN 한 줄, 아무것도 보내지 않는다)
//
// | 입력 | reason |
// |------|--------|
// | `n` 이 1~1000 정수가 아님, `size` 가 20~MAX_INNER 정수가 아님, `to` 가 점 십진 IPv4 가 아님, 낱말이 셋을 넘음 | `bad_send_args` |
// | `to` 를 생략했는데 `CONNECTED` 세션이 둘 이상 | `ambiguous_destination` |
// | 고른 세션이 `CONNECTED` 가 아님. `to` 를 생략했고 `CONNECTED` 세션이 없는 것도 이것이다 | `not_connected` |
// | 앞 작업이 아직 끝나지 않음 | `send_busy` |
//
// `to` 로 고른 칸에 세션이 없으면 거르지 않는다. 8.5 의 `tx_drop_no_route` 가 센다.
//
// ## 이 코드가 정한 것
//
// | 자리 | 정한 것 | 근거 |
// |------|---------|------|
// | `[size]` 와 `[to]` 가르기 | 점이 든 낱말은 `to`, 아니면 `size`. 순서는 `size` 다음 `to` 다 | roadmap.md 의 문법 `send <n> [size] [to]` |
// | 100 미만의 크기 | 100바이트 레이아웃의 앞에서 자른다. 그 안에 든 칸만 쓴다 | Phase 5 단서 "20바이트는 IPv4 헤더뿐이다" |
// | 작업 중의 새 `send` | 받지 않는다 | 두 작업의 패킷 번호가 섞이면 비교 스크립트가 줄을 맞출 수 없다 |
// | 끝맺음 줄 | `console.send` 에 `sent`, `requested`. 송신 실패로 멈추면 WARN 과 `reason=send_failed` | roadmap.md "보낸 개수를 남긴다" |
// | 에코 응답 | `enqueue` 로 받아 작업 패킷보다 먼저 보낸다. 실패해도 작업을 멈추지 않는다 | 에코는 시험 빌드 장치다(ADR 0023) |

#include "sangtachi/timer.hpp"
#include "sangtachi/tunnel/tunnel.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace sangtachi::tunnel {

inline constexpr std::size_t kSyntheticSize = 100;      // 레이아웃 표의 크기
inline constexpr std::size_t kSyntheticMinSize = 20;
inline constexpr std::uint32_t kSendMaxCount = 1000;
inline constexpr std::uint16_t kSyntheticPort = 25565;
inline constexpr std::size_t kMarkerOffset = 40;        // 방향 표시
inline constexpr std::byte kMarkerRequest{0xA5};
inline constexpr std::byte kMarkerEcho{0x5A};

// 레이아웃 표대로 size 바이트를 만든다. size 는 20 이상 MAX_INNER 이하여야 한다.
[[nodiscard]] std::vector<std::byte> make_synthetic(std::size_t size, std::uint32_t src,
                                                    std::uint32_t dst, std::uint32_t number,
                                                    std::uint64_t sent_ms);

// 앞 20바이트(IHL 5)의 IPv4 헤더 체크섬을 다시 채운다.
void fill_ipv4_checksum(std::span<std::byte> packet) noexcept;

struct SendArgs {
    std::uint32_t count = 0;
    std::size_t size = kSyntheticSize;
    std::optional<std::uint32_t> to;
};

// `send` 다음의 나머지. 위 머리 표의 `bad_send_args` 면 비어 있다.
[[nodiscard]] std::optional<SendArgs> parse_send_args(std::string_view rest);

class DataSender {
public:
    DataSender(Tunnel& tunnel, std::function<Millis()> clock);

    void on_command(std::string_view rest);
    void enqueue(std::vector<std::byte> inner);
    // 많아야 budget 개를 보낸다. 남은 것이 있으면 참이다.
    bool pump(std::size_t budget);
    [[nodiscard]] bool busy() const noexcept { return job_.has_value() || !queued_.empty(); }

private:
    struct Job {
        SendArgs args;
        std::uint32_t to = 0;
        std::uint32_t next = 0;  // 패킷 번호. 0 부터
        std::uint32_t sent = 0;
    };
    void finish(bool failed);

    Tunnel& tunnel_;
    std::function<Millis()> clock_;
    std::optional<Job> job_;
    std::deque<std::vector<std::byte>> queued_;
};

}  // namespace sangtachi::tunnel
