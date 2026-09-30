#pragma once

// 콘솔 제어 신호 핸들러 (concurrency.md 7장 종료).
//
// 7장이 정한 것을 그대로 옮긴다. 여기서 새 규칙을 만들지 않는다.
//   - `SetConsoleCtrlHandler` 가 종료 이벤트를 신호한다
//   - 핸들러는 `[loop]` 소유 상태를 절대 건드리지 않는다. 다른 스레드 컨텍스트에서
//     실행되기 때문이다. 이 이음새가 만지는 것은 이벤트 핸들 둘뿐이다
//   - `CTRL_C_EVENT` 와 `CTRL_BREAK_EVENT` 는 신호하고 즉시 반환한다
//   - `CTRL_CLOSE_EVENT`, `CTRL_LOGOFF_EVENT`, `CTRL_SHUTDOWN_EVENT` 는 신호한 뒤 정리
//     완료 이벤트를 OS 유예 시간 안쪽(3초)까지 기다렸다가 반환한다. 그 셋은 핸들러가
//     반환하는 순간 Windows 가 프로세스를 끝내므로, 기다리지 않으면 `CLOSE` 송신과
//     어댑터 정리가 실행되지 않는다
//
// **이 헤더는 OS 헤더를 들이지 않는다.** 구현만 `client/src/platform/win32/` 아래에 있다
// (decisions/0010). 핸들은 OS 가 준 값을 그대로 나르는 불투명 포인터다.

#include "sangtachi/platform/wait.hpp"

#include <cstdint>
#include <string_view>

namespace sangtachi::platform {

// 정리 완료 이벤트를 기다리는 상한 (concurrency.md 7장 종료의 표).
//
// OS 유예 시간 안쪽이라는 것이 이 값의 뜻이다. 더 키우면 유예가 먼저 끝나 기다린 만큼을
// 잃는다.
inline constexpr std::uint32_t kCleanupWaitMs = 3000;

// 콘솔 제어 신호 (concurrency.md 7장 종료의 표에 있는 다섯).
//
// Unknown 은 그 다섯 중 어느 것도 아닌 값이다. 문서가 정하지 않은 신호이므로 이 이음새는
// 그것을 처리했다고 말하지 않는다.
enum class CtrlSignal {
    Interrupt,  // CTRL_C_EVENT
    Break,      // CTRL_BREAK_EVENT
    Close,      // CTRL_CLOSE_EVENT
    Logoff,     // CTRL_LOGOFF_EVENT
    Shutdown,   // CTRL_SHUTDOWN_EVENT
    Unknown,
};

// 그 신호가 정리 완료를 기다려야 하는 것인가 (concurrency.md 7장 종료의 표).
//
// 순수 함수다. 신호 종류 하나만 보고 판정하므로 시험이 핸들도 콘솔도 없이 돌린다.
[[nodiscard]] constexpr bool waits_for_cleanup(CtrlSignal signal) noexcept {
    switch (signal) {
        case CtrlSignal::Close:
        case CtrlSignal::Logoff:
        case CtrlSignal::Shutdown:
            return true;
        case CtrlSignal::Interrupt:
        case CtrlSignal::Break:
        case CtrlSignal::Unknown:
            return false;
    }
    // 열거에 없는 값이 들어오면 기다리지 않는다. 모르는 신호에 3초를 쓰지 않는다.
    return false;
}

// OS 가 넘긴 제어 신호 값을 위 열거로 옮긴다. 다섯 밖의 값은 Unknown 이다.
[[nodiscard]] CtrlSignal classify_ctrl_signal(std::uint32_t os_event) noexcept;

// 핸들러 본체. 상태를 인자로 받는다.
//
// 설치된 핸들러가 이 함수를 모듈 상태와 kCleanupWaitMs 로 부른다. 시험은 자기 이벤트
// 둘과 짧은 상한으로 같은 함수를 부른다. 그래서 실제 경로와 시험 경로가 같은 코드다.
//
// 참을 돌려주면 그 신호를 처리한 것이다(OS 핸들러의 `TRUE`). Unknown 은 거짓을
// 돌려주어 다음 핸들러에게 넘긴다.
bool run_ctrl_handler(CtrlSignal signal, WaitHandle shutdown_event, WaitHandle cleanup_done,
                      std::uint32_t cleanup_wait_ms) noexcept;

struct CtrlHandlerInstall {
    bool ok = false;
    std::uint32_t error = 0;     // ok 가 거짓일 때 GetLastError
    std::string_view failed_op;  // 실패한 호출 이름 (architecture.md 9장 socket.error 의 op)
};

// 핸들러를 설치한다. 정리 완료 이벤트는 이 이음새가 만들어 들고 있는다.
//
// 종료 이벤트는 `[loop]` 가 소유한 것을 쓰되 **핸들을 복제해서** 들고 있는다. 이 두
// 핸들의 수명은 프로세스 수명이고 닫지 않는다. 근거는 구현 파일에 적었다.
[[nodiscard]] CtrlHandlerInstall install_console_ctrl_handler(WaitHandle shutdown_event) noexcept;

// 정리 완료를 알린다 (concurrency.md 7장 종료의 (7)).
//
// 설치 전이면 아무것도 하지 않고 거짓을 돌려준다.
bool signal_cleanup_done() noexcept;

}  // namespace sangtachi::platform
