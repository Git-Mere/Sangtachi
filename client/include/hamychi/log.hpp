#pragma once

// 로그 출력 (architecture.md 9장 로그 출력).
//
// 계약 넷을 그대로 옮긴 것이다.
//   출력처   표준 오류
//   수준     INFO, WARN, ERROR 셋
//   단위     한 줄에 이벤트 하나. 값에 공백과 제어문자를 싣지 않는다
//   실패     8장의 실패 코드 문자열을 그대로 쓴다
//
// 줄 모양은 `<수준> <이벤트키> <이름>=<값> ...` 이다.

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace hamychi {

enum class LogLevel {
    Info,
    Warn,
    Error,
};

[[nodiscard]] std::string_view to_token(LogLevel level) noexcept;

struct LogField {
    std::string_view name;
    std::string value;
};

// 진단 값 하나. 공백과 제어문자를 밑줄로 바꾼다.
[[nodiscard]] LogField field(std::string_view name, std::string_view value);
[[nodiscard]] LogField field(std::string_view name, std::uint64_t value);

// 값에서 공백과 제어문자를 걷어낸다.
//
// 되돌릴 수 없는 치환이다. 원문이 필요한 판정을 로그로 하지 않는다 (architecture.md 9장).
// 따옴표도 이스케이프도 쓰지 않는 이유는 읽는 쪽이 표기를 해석하지 않게 하려는 것이다.
//
// **판정은 바이트 단위다.** ASCII 공백과 제어 바이트만 바꾼다. 0x80 이상은 그대로 둔다.
// 유니코드 줄 구분자(U+2028 같은 것)는 바뀌지 않으므로 줄을 그것으로 나누는 도구를
// 판정에 쓰지 않는다. 같은 한계가 9장에 적혀 있다.
[[nodiscard]] std::string sanitize_value(std::string_view value);

// 줄 하나를 만든다. 줄바꿈은 붙이지 않는다.
//
// 순수 함수라 시험이 전역 상태 없이 돌릴 수 있다. 내보내는 것은 emit 이 한다.
[[nodiscard]] std::string format_line(LogLevel level, std::string_view event,
                                      std::span<const LogField> fields);

// 줄 하나를 표준 오류에 쓴다. 줄바꿈까지 한 번의 쓰기로 낸다.
void emit(LogLevel level, std::string_view event, std::span<const LogField> fields);

}  // namespace hamychi
