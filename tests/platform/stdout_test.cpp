#include "sangtachi/platform/stdout.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

// 시험 케이스 이름은 ASCII 로만 적는다. 이유는 network/wsa_test.cpp 머리에 있다.
//
// architecture.md 3.5: 표준 출력이 리다이렉트되면 UTF-8 바이트이고 줄 끝은 LF 하나다.

namespace {

// 파이프에 쓰고 읽은 바이트 전부.
std::string through_pipe(std::string_view line) {
    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    REQUIRE(::CreatePipe(&read_end, &write_end, nullptr, 0) != 0);
    const bool ok = sangtachi::platform::write_line(write_end, line);
    ::CloseHandle(write_end);
    std::string got;
    char buf[512];
    DWORD n = 0;
    while (::ReadFile(read_end, buf, sizeof(buf), &n, nullptr) != 0 && n > 0) {
        got.append(buf, n);
    }
    ::CloseHandle(read_end);
    REQUIRE(ok);
    return got;
}

}  // namespace

TEST_CASE("stdout: a redirected line is UTF-8 bytes and one LF", "[platform][stdout]") {
    // 8장 문장 표의 한국어 문장 하나. 바이트 그대로 지나가야 한다.
    const std::string line =
        "FAIL CONTROL_PLANE_EXCHANGE_FAILED \xEB\xB0\xA9 \xEC\xA0\x95\xEB\xB3\xB4\xEB\xA5\xBC";
    const std::string got = through_pipe(line);
    REQUIRE(got == line + "\n");
    REQUIRE(got.find("\r\n") == std::string::npos);
}

TEST_CASE("stdout: an ASCII line ends with exactly one LF", "[platform][stdout]") {
    REQUIRE(through_pipe("ROOM ABCDEF") == "ROOM ABCDEF\n");
    REQUIRE(through_pipe("") == "\n");
}

TEST_CASE("stdout: a null handle is reported as a failure", "[platform][stdout]") {
    REQUIRE_FALSE(sangtachi::platform::write_line(nullptr, "x"));
}
