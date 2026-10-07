#pragma once

// 표준 출력 한 줄 (architecture.md 3.5 기동 입력의 `ROOM`, `FAIL` 줄).
//
// 3.5 가 정한 것을 옮긴다.
//   - 표준 출력이 리다이렉트되면 **UTF-8 바이트 그대로** 쓰고 줄 끝은 **LF 하나**다
//   - 표준 출력이 콘솔이면 콘솔의 유니코드 쓰기로 낸다
//   - 콘솔 코드 페이지는 바꾸지 않는다
//
// CRT 의 stdout 을 거치지 않는다. 텍스트 모드가 LF 를 CRLF 로 바꾸고, 콘솔에서는 코드 페이지대로
// 바이트를 해석해 한국어 문장이 깨진다. 이 프로그램에서 표준 출력에 쓰는 것은 이 이음새 하나다.
//
// **이 헤더는 OS 헤더를 들이지 않는다.** 구현은 `client/src/platform/win32/` 에 있다 (decisions/0010).
// 부르는 스레드는 `[loop]` 하나다. 이 함수는 줄 사이의 순서를 스레드끼리 맞춰 주지 않는다.

#include <string_view>

namespace sangtachi::platform {

// OS 가 준 출력 핸들을 그대로 나르는 불투명 값.
using OutputHandle = void*;

// 이 프로세스의 표준 출력 핸들. 없으면 널이다.
[[nodiscard]] OutputHandle stdout_handle() noexcept;

// utf8_line 과 줄바꿈 하나를 쓴다. 다 썼으면 참이다. utf8_line 에 줄바꿈을 넣지 않는다.
//
// handle 이 콘솔이면 UTF-16 으로 바꿔 콘솔 쓰기로, 아니면(파일, 파이프) 바이트 그대로 쓴다.
// 시험이 파이프 핸들을 넘겨 리다이렉트 경로를 본다.
bool write_line(OutputHandle handle, std::string_view utf8_line) noexcept;

// write_line(stdout_handle(), utf8_line).
bool write_stdout_line(std::string_view utf8_line) noexcept;

}  // namespace sangtachi::platform
