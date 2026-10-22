#pragma once

// 로그 출력 (architecture.md 9장 로그 출력).
//
// 계약 넷을 그대로 옮긴 것이다.
//   출력처   로그 파일. 표준 오류에는 콘솔이면 ERROR 줄만, 리다이렉트면 전부 (아래 FileLogSink)
//   수준     INFO, WARN, ERROR 셋
//   단위     한 줄에 이벤트 하나. 값에 공백과 제어문자를 싣지 않는다
//   실패     8장의 실패 코드 문자열을 그대로 쓴다
//
// 줄 모양은 `<수준> <이벤트키> <이름>=<값> ...` 이다.

#include <atomic>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>

namespace sangtachi {

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
//
// 받는 곳(LogSink)이 설치돼 있으면 표준 오류 대신 그곳으로 간다.
void emit(LogLevel level, std::string_view event, std::span<const LogField> fields);

// emit 이 줄을 넘길 곳. **시험이 로그를 프로세스 안에서 보려는 주입 지점이다.**
//
// 진단 로그(`stun.timeout` 같은 것)는 반환값도 카운터도 남기지 않는 경로가 있다. 표준 오류를
// 프로세스 밖에서 긁지 않으면 단위 시험이 그 줄을 볼 수 없다.
//
// - 제품 경로는 로그 파일을 연 뒤 FileLogSink 를 설치한다. 설치된 것이 없으면 표준 오류다
// - write 는 줄바꿈을 붙이기 전의 한 줄을 받는다 (format_line 의 결과)
// - **설치와 해제는 다른 스레드가 emit 하지 않을 때만 한다.** 포인터 읽기는 원자적이지만,
//   해제한 받는 곳을 다른 스레드가 아직 부르고 있는지는 여기서 막지 않는다
class LogSink {
public:
    virtual ~LogSink() = default;
    virtual void write(std::string_view line) = 0;
};

// 받는 곳을 바꾸고 앞의 것을 돌려준다. nullptr 이면 표준 오류로 돌아간다.
LogSink* set_log_sink(LogSink* sink) noexcept;

// 제품 경로의 받는 곳 (architecture.md 9장 로그 출력). 모든 줄을 로그 파일에 쓴다. 화면(표준 오류)에는
// screen_all 이면 전부 쓴다. 아니면 기동 중의 ERROR 줄만 쓰고 end_startup 뒤로는 아무것도 쓰지 않는다. 표준
// 오류가 콘솔이면 screen_all 이 거짓이다. 사람이 보는 창에는 안내와 기동 실패의 이유만 남기고, 스크립트가 표준
// 오류를 받으면 지금처럼 전부 받는다.
//
// 두 함수는 줄바꿈까지 붙인 한 줄을 받아 한 번에 쓴다. 여러 스레드가 동시에 부른다. 파일 쓰기가 실패해도 다른
// 곳으로 돌리지 않는다. 이 받는 곳 안에서 emit 을 부르면 자기 자신으로 돌아온다.
class FileLogSink final : public LogSink {
public:
    using WriteFn = std::function<void(std::string_view line_with_newline)>;

    FileLogSink(WriteFn file, WriteFn screen, bool screen_all)
        : file_(std::move(file)), screen_(std::move(screen)), screen_all_(screen_all) {}

    void write(std::string_view line) override;

    // 기동이 끝났다. 그 뒤로 screen_all 이 거짓이면 화면에 ERROR 줄도 쓰지 않는다. 실패는 FAIL 줄과 안내가 알린다.
    void end_startup() noexcept { startup_.store(false, std::memory_order_relaxed); }

private:
    WriteFn file_;
    WriteFn screen_;
    bool screen_all_;
    std::atomic<bool> startup_{true};
};

// 표준 오류에 한 줄을 쓴다. 줄바꿈까지 붙은 것을 받아 한 번에 쓴다. FileLogSink 의 화면 함수가 쓴다.
void write_stderr(std::string_view line_with_newline);

}  // namespace sangtachi
