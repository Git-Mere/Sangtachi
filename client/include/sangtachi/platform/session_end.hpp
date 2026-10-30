#pragma once

// 로그오프와 시스템 종료의 창 메시지 (concurrency.md 7장 GUI 빌드의 종료).
//
// **이 헤더는 OS 헤더를 들이지 않는다.** 구현만 `client/src/platform/win32/` 아래에 있다 (decisions/0010).

namespace sangtachi::platform {

// message 는 OS 가 창에 넘긴 메시지(Windows 의 MSG)다. 세션이 정말 끝나는 WM_ENDSESSION(wParam 참)이면 참이다.
// 널이면 거짓이다.
[[nodiscard]] bool is_session_ending(const void* message) noexcept;

}  // namespace sangtachi::platform
