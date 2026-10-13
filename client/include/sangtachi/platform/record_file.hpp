#pragma once

// 플랫폼 이음새: 로컬 기록 파일의 열기와 쓰기, UTC 벽시계, PID (architecture.md 9장 로컬 기록 파일의
// 경로, 쓰기와 실패).
//
// 줄의 모양과 실패 뒤 정지는 record.hpp 가 갖는다. 여기는 OS 호출만 한다.
//
// 9장이 정한 것을 옮긴다.
//   - 폴더는 `GetModuleFileNameW` 가 준 실행 파일 경로에서 마지막 구성 요소를 뺀 것이다. 링크를 풀거나
//     정규화하지 않는다. 경로를 끝까지 얻지 못하면(잘림 포함) 열기 실패다
//   - 그 아래 `records` 폴더가 없으면 만든다. 있으면 그대로 쓴다
//   - 같은 이름의 파일이 이미 있으면 열기 실패다 (`CREATE_NEW`)
//   - 경로는 와이드 문자 API 로 다룬다
//   - 한 줄은 `WriteFile` 한 번이다. 런타임 버퍼를 거치지 않고 디스크 캐시를 비우는 호출도 하지 않는다.
//     일부만 쓰인 것도 실패다
//
// **이 헤더는 OS 헤더를 들이지 않는다.** 구현은 `client/src/platform/win32/` 에 있다 (decisions/0010).
//
// ## 이 코드가 정한 것
//
// | 자리 | 정한 것 | 근거 |
// |------|---------|------|
// | 실행 파일 경로의 버퍼 | 32768 와이드 문자로 한 번 부른다. 돌려준 길이가 버퍼를 다 채웠으면 잘린 것이다. `GetModuleFileNameW` 실패, 코드 122(`ERROR_INSUFFICIENT_BUFFER`) | 9장 "버퍼가 모자라 잘린 경우" 는 열기 실패다. 32767 자가 Windows 확장 경로의 상한이라 다시 키워 부를 일이 없다 |
// | 마지막 구성 요소 | 마지막 `\` 앞까지. `\` 가 없으면 `GetModuleFileNameW` 실패, 코드 0 | `GetModuleFileNameW` 는 `\` 로 구분한 전체 경로를 준다 |
// | 여는 권한 | `FILE_APPEND_DATA` 하나. 공유는 `FILE_SHARE_READ` | 9장 추가 전용. 실행 중에 사람과 시험이 읽을 수 있고 다른 쓰기 열기는 막는다 |
// | 실패의 보고 | 실패한 호출 이름과 `GetLastError` 값을 돌려준다. 줄은 부르는 쪽이 낸다 | main 의 기동 실패 관례(`socket.error op=<호출> code=<오류>`) |
// | 1970 이전의 벽시계 | 0 | 기록 시각으로 의미가 없다. 일어나지 않는 값이다 |

#include <cstdint>
#include <string_view>

namespace sangtachi::platform {

// OS 파일 핸들을 그대로 나르는 불투명 값.
using FileHandle = void*;

// UTC 벽시계 unix 밀리초. 단조 시계가 아니다. 기록 줄의 `ts` 와 파일 이름의 기동 시각에만 쓴다.
[[nodiscard]] std::uint64_t wall_clock_unix_ms() noexcept;

// 이 프로세스의 PID.
[[nodiscard]] std::uint32_t current_process_id() noexcept;

// bytes 를 쓰기 호출 한 번으로 넘긴다. 전부 쓰였을 때만 참이다. 일부만 쓰였으면 거짓이고 남은 것을
// 다시 쓰지 않는다. 시험이 파이프 핸들을 넘겨 일부 쓰기를 본다.
bool write_whole(FileHandle handle, std::string_view bytes) noexcept;

// 열린 기록 파일 하나. 옮길 수만 있고 사라질 때 닫는다.
class RecordFile {
public:
    RecordFile() noexcept = default;
    explicit RecordFile(FileHandle handle) noexcept : handle_(handle) {}
    ~RecordFile();
    RecordFile(const RecordFile&) = delete;
    RecordFile& operator=(const RecordFile&) = delete;
    RecordFile(RecordFile&& other) noexcept;
    RecordFile& operator=(RecordFile&& other) noexcept;

    [[nodiscard]] bool is_open() const noexcept { return handle_ != nullptr; }
    // write_whole 과 같다. 닫힌 파일이면 거짓이다.
    bool write(std::string_view bytes) noexcept { return write_whole(handle_, bytes); }

private:
    FileHandle handle_ = nullptr;
};

struct RecordFileOpen {
    RecordFile file;              // 실패면 닫혀 있다
    std::string_view failed_op;   // 실패한 호출 이름. 성공이면 비어 있다
    std::uint32_t error = 0;      // 그 호출의 GetLastError

    [[nodiscard]] bool ok() const noexcept { return file.is_open(); }
};

// `<실행 파일 폴더>\records\<file_name>` 을 새로 만든다. file_name 은 ASCII 다 (record::file_name).
[[nodiscard]] RecordFileOpen open_record_file(std::string_view file_name) noexcept;

}  // namespace sangtachi::platform
