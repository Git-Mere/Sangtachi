#pragma once

// 플랫폼 내부 헤더: 실행 파일이 있는 폴더 (architecture.md 9장 로컬 기록 파일의 경로).
//
// 선언은 POSIX 공통 원본(record_file.cpp)이 쓰고, 정의는 OS 마다 다르다. Linux 는
// `client/src/platform/linux/exe_path.cpp` 가 `/proc/self/exe` 로 구한다.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace sangtachi::platform {

// 실행 파일 경로에서 마지막 '/' 앞까지를 돌려준다. 끝에 '/' 를 붙이지 않는다.
// 실패하면 nullopt 이고 failed_op 에 실패한 호출 이름(정적 문자열), error 에 errno 를 둔다.
// 경로가 잘렸으면 실패다(error = ENAMETOOLONG). '/' 가 없으면 error = 0 이다.
// 메모리가 모자라면 std::bad_alloc 을 던진다. 부르는 쪽이 잡는다.
[[nodiscard]] std::optional<std::string> executable_dir(std::uint32_t& error,
                                                        std::string_view& failed_op);

}  // namespace sangtachi::platform
