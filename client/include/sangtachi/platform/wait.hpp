#pragma once

// 플랫폼 이음새: 단조 시계, 이벤트 핸들, 여러 핸들 대기 (concurrency.md 2장 대기,
// 3장 루프 한 바퀴, 4장 타이머).
//
// **이 헤더는 OS 헤더를 들이지 않는다.** 구현만 `client/src/platform/win32/` 아래에 있다
// (decisions/0010). 핸들은 OS 가 준 값을 그대로 나르는 불투명 포인터다. 이 헤더를 읽는
// 쪽은 그 값을 해석하지 않는다.

#include <cstddef>
#include <cstdint>
#include <span>

namespace sangtachi::platform {

// 단조 밀리초. timer.hpp 의 Millis 와 같은 폭이다.
using WaitMillis = std::uint64_t;

// OS 이벤트 핸들. 이 이름이 나르는 값은 플랫폼 구현만 해석한다.
using WaitHandle = void*;

// 무한 대기를 뜻하는 타임아웃. 이 값의 출처는 여기다. timer.hpp 의 kInfiniteTimeout 이
// 이것을 가리킨다.
inline constexpr std::uint32_t kInfiniteWaitMs = 0xFFFFFFFFu;

// 이벤트가 신호된 뒤 스스로 돌아가는지 (concurrency.md 2장 대기의 표).
//
// Manual  신호되면 기다리는 모든 스레드가 함께 깨어나고 신호 상태로 남는다. 종료 이벤트.
// Auto    깨어난 스레드 하나가 신호를 가져가고 이벤트는 비신호로 돌아간다. 콘솔 이벤트.
enum class ResetMode {
    Manual,
    Auto,
};

// 대기 한 번의 결과.
enum class WaitStatus {
    Signaled,  // index 가 신호된 핸들의 배열 인덱스다
    Timeout,   // 아무 핸들도 신호되지 않은 채 타임아웃이 지났다
    Failed,    // 대기 자체가 실패했다. error 가 OS 오류 코드다
};

struct WaitResult {
    WaitStatus status = WaitStatus::Failed;
    std::size_t index = 0;    // Signaled 일 때만 뜻이 있다
    std::uint32_t error = 0;  // Failed 일 때만 뜻이 있다
};

// 단조 밀리초 시계. 벽시계가 아니다 (concurrency.md 4장 타이머).
[[nodiscard]] WaitMillis monotonic_ms() noexcept;

// 이벤트를 만든다. 비신호 상태로 시작한다. 실패하면 널을 돌려준다.
[[nodiscard]] WaitHandle create_event(ResetMode mode) noexcept;

// 이벤트를 신호한다. 널이거나 실패하면 거짓이다.
bool signal_event(WaitHandle handle) noexcept;

// 이벤트를 닫는다. 널은 아무것도 하지 않는다.
void close_event(WaitHandle handle) noexcept;

// 같은 이벤트 객체를 가리키는 핸들을 하나 더 만든다. 실패하거나 널을 받으면 널이다.
//
// 다른 스레드가 기다리는 핸들을 소유자가 먼저 닫으면 그 대기는 정의되지 않는다. 그래서
// 소유자보다 오래 살 수 있는 쪽이 자기 몫을 복제해 들고 간다. 객체는 마지막 핸들이 닫힐
// 때까지 산다. 복제본은 close_event 로 닫는다.
[[nodiscard]] WaitHandle duplicate_event(WaitHandle handle) noexcept;

// 핸들 하나가 신호될 때까지, 또는 타임아웃까지 기다린다.
//
// 여럿이 동시에 신호되어 있으면 **가장 낮은 인덱스 하나**를 돌려준다. 루프가 반환값으로
// 분기하지 않는 이유가 그것이다 (concurrency.md 3장).
//
// 빈 배열이나 OS 상한을 넘는 배열은 기다리지 않고 Failed 를 돌려준다.
[[nodiscard]] WaitResult wait_any(std::span<const WaitHandle> handles,
                                  std::uint32_t timeout_ms) noexcept;

}  // namespace sangtachi::platform
