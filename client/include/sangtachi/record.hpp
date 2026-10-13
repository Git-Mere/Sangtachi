#pragma once

// 로컬 기록 파일 (architecture.md 9장 로컬 기록 파일). spec.md M-6 판정의 입력이다.
//
// 여기 있는 것은 OS 를 모르는 셋이다. 줄 하나의 모양(format_line), 파일 이름(file_name), 그리고 쓰기와
// 실패 뒤 정지(RecordWriter). 파일을 여는 것과 OS 에 쓰는 것은 platform/record_file 이고, 쓰기 함수와
// 벽시계는 주입받는다. 그래서 단위 시험이 실패를 주입할 수 있다.
//
// 9장이 정한 것을 옮긴다.
//   - 줄은 `<이름>=<값>` 아홉 쌍을 공백 하나로 이은 것이다. 순서는 v, kind, ts, attempt, role, peer,
//     result, rtt_us, setup_ms 로 고정이고 끝은 LF 하나다
//   - 값이 없으면 `-` 다. 빈 문자열이나 0 을 쓰지 않는다
//   - `ts` 는 UTC 벽시계 밀리초를 `2026-10-07T12:34:56.789Z` 꼴로 적는다
//   - 파일 이름은 `<UTC 기동 시각 YYYYMMDDTHHMMSSZ>-<PID 10진>.log` 다
//   - 시도 번호는 수립 줄을 쓸 때 1 부터 하나씩 붙이고, 종료 줄은 그 세션의 수립 줄 번호를 쓴다
//   - 한 줄은 쓰기 호출 하나다. 쓰기가 실패하면(일부만 쓰인 것 포함) `WARN` 한 줄과
//     `record_write_failed` 를 올리고 그 뒤로는 아무것도 쓰지 않는다. 터널은 그대로 둔다
//
// **이 객체는 `[loop]` 가 단독 소유한다** (concurrency.md 5장 상태 소유). 올리는 카운터 표는 프로세스
// 표다. 쓰기 실패는 세션이 아니라 파일의 일이다 (counters.hpp 의 두 범위).
//
// ## 이 코드가 정한 것
//
// | 자리 | 정한 것 | 근거 |
// |------|---------|------|
// | 실패 뒤 카운터와 `WARN` | 실패한 쓰기 한 번에 `record_write_failed` 1 과 `WARN` 한 줄. 그 뒤 쓰지 않고 넘긴 줄은 세지도 알리지도 않는다 | 9장은 쓰기가 실패하면 올린다고 했고 그 뒤로는 쓰지 않는다고 했다. 넘긴 줄은 쓰기 시도가 아니다. 9장 "판정할 수 있는 실행" 은 그 값이 0 인지만 본다 |
// | `WARN` 의 이벤트 키 | `record.write_failed`. 필드는 `kind`(`establish`/`end`)와 `attempt` | 9장 고정 키 표에 없는 진단이다. 다른 실패 `WARN` 의 `<모듈>.<무엇>_failed` 꼴을 따른다 (`candidates.enumerate_failed`) |
// | 실패 뒤의 시도 번호 | 줄을 쓰지 않아도 수립마다 하나 오른다 | 번호는 시도를 센다. 줄이 남았는지와 무관하다. 부르는 쪽이 종료 줄을 그 번호로 묶는다 |
// | 벽시계를 읽는 때 | 줄을 쓰기 직전에 한 번. 실패 뒤에는 읽지 않는다 | 9장 `ts` 는 "줄을 쓴 시각" 이다 |
// | 값의 검사 | `role` 과 `result` 는 받은 그대로 싣는다. `setup_ms` 가 `OK` 일 때만 있는지도 부르는 쪽이 지킨다 | 값의 출처는 부르는 쪽의 상태다. 여기서 고쳐 적으면 그 결함이 기록에서 숨는다 |
// | 파일 이름의 기동 시각 | 부르는 쪽이 준 벽시계 밀리초의 초 아래를 버린다 | 9장 "기동 시각은 `YYYYMMDDTHHMMSSZ` 꼴의 UTC" 에 초 아래 자리가 없다 |

#include "sangtachi/counters.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace sangtachi::record {

enum class LineKind : std::uint8_t { kEstablish, kEnd };

// 줄 하나의 값. 필드 이름과 순서는 format_line 이 정한다.
struct RecordLine {
    LineKind kind = LineKind::kEstablish;
    std::uint64_t attempt = 0;
    std::string_view role;                // "host" | "player"
    std::optional<std::uint32_t> peer;    // 상대 peer_id. 없으면 "-"
    std::string_view result;              // OK, FR-13 코드, ABORTED, PEER_FAILED, CLOSED
    std::optional<std::uint64_t> rtt_us;  // 마이크로초
    std::optional<std::uint64_t> setup_ms;
};

// `2026-10-07T12:34:56.789Z`. unix_ms 는 UTC 벽시계 밀리초다.
[[nodiscard]] std::string format_timestamp(std::uint64_t unix_ms);

// 줄 하나. 끝의 LF 하나까지 붙인다. unix_ms 가 `ts` 다.
[[nodiscard]] std::string format_line(const RecordLine& line, std::uint64_t unix_ms);

// `20261007T123456Z-4812.log`. 폴더는 붙이지 않는다.
[[nodiscard]] std::string file_name(std::uint64_t start_unix_ms, std::uint32_t pid);

class RecordWriter {
public:
    // 전부 썼을 때만 참. 한 번 부르는 것이 쓰기 호출 하나다.
    using WriteFn = std::function<bool(std::string_view bytes)>;
    // UTC unix 밀리초.
    using WallClockFn = std::function<std::uint64_t()>;

    RecordWriter(WriteFn write, WallClockFn wall_clock, Counters& counters);

    // 수립 줄. 새 시도 번호(1부터)를 붙여 쓰고 그 번호를 돌려준다. 쓰기가 실패했거나 이미 실패한 뒤에도
    // 번호는 하나 오른다.
    std::uint64_t establish(std::string_view role, std::optional<std::uint32_t> peer,
                            std::string_view result, std::optional<std::uint64_t> rtt_us,
                            std::optional<std::uint64_t> setup_ms);

    // 종료 줄. setup_ms 는 늘 "-" 다.
    void end(std::uint64_t attempt, std::string_view role, std::optional<std::uint32_t> peer,
             std::string_view result, std::optional<std::uint64_t> rtt_us);

    // 쓰기가 한 번이라도 실패했다. 그 뒤로는 아무것도 쓰지 않는다.
    [[nodiscard]] bool failed() const noexcept { return failed_; }

private:
    void write_line(const RecordLine& line);

    WriteFn write_;
    WallClockFn wall_clock_;
    Counters& counters_;
    std::uint64_t last_attempt_ = 0;
    bool failed_ = false;
};

}  // namespace sangtachi::record
