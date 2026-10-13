#include "sangtachi/platform/record_file.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

// 시험 케이스 이름은 ASCII 로만 적는다. 이유는 network/wsa_test.cpp 머리에 있다.
//
// architecture.md 9장 로컬 기록 파일의 경로, 쓰기와 실패. 실행 파일 폴더 아래 `records` 에 이 시험만의
// 이름으로 파일을 만들고 끝나면 지운다. `records` 폴더는 남긴다(있으면 그대로 쓰는 폴더다).

namespace {

// 시험 실행 파일의 `records` 폴더. 구현과 따로 구한다.
std::wstring records_dir() {
    std::wstring path(32768, L'\0');
    const DWORD n = ::GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    REQUIRE(n > 0);
    REQUIRE(n < path.size());
    path.resize(n);
    path.resize(path.rfind(L'\\') + 1);
    return path + L"records\\";
}

std::wstring widen(std::string_view ascii) {
    return std::wstring(ascii.begin(), ascii.end());
}

std::string unique_name() {
    return "unit-" + std::to_string(::GetCurrentProcessId()) + "-" +
           std::to_string(::GetTickCount64()) + ".log";
}

std::string read_all(const std::wstring& path) {
    // 쓰는 쪽이 열어 둔 채로 읽는다. 쓰는 쪽은 읽기 공유를 허용한다.
    const HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(h != INVALID_HANDLE_VALUE);
    std::string got;
    char buf[512];
    DWORD n = 0;
    while (::ReadFile(h, buf, sizeof(buf), &n, nullptr) != 0 && n > 0) {
        got.append(buf, n);
    }
    ::CloseHandle(h);
    return got;
}

}  // namespace

TEST_CASE("record_file: a new file in records next to the executable, appended line by line",
          "[platform][record_file]") {
    const std::string name = unique_name();
    const std::wstring path = records_dir() + widen(name);
    {
        auto opened = sangtachi::platform::open_record_file(name);
        REQUIRE(opened.ok());
        REQUIRE(opened.failed_op.empty());
        REQUIRE(read_all(path).empty());

        REQUIRE(opened.file.write("v=1 a\n"));
        REQUIRE(read_all(path) == "v=1 a\n");  // 버퍼에 갇히지 않고 바로 OS 에 간다
        REQUIRE(opened.file.write("v=1 b\n"));
        REQUIRE(read_all(path) == "v=1 a\nv=1 b\n");

        // 같은 이름이 이미 있으면 새로 만들지 않고 실패다. 덮거나 잇지 않는다.
        auto again = sangtachi::platform::open_record_file(name);
        REQUIRE_FALSE(again.ok());
        REQUIRE(again.failed_op == "CreateFileW");
        REQUIRE(again.error == ERROR_FILE_EXISTS);
    }
    REQUIRE(read_all(path) == "v=1 a\nv=1 b\n");
    REQUIRE(::DeleteFileW(path.c_str()) != 0);
}

TEST_CASE("record_file: a whole write is true", "[platform][record_file]") {
    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    REQUIRE(::CreatePipe(&read_end, &write_end, nullptr, 0) != 0);
    const bool ok = sangtachi::platform::write_whole(write_end, "v=1 x\n");
    ::CloseHandle(write_end);
    std::string got;
    char buf[64];
    DWORD n = 0;
    while (::ReadFile(read_end, buf, sizeof(buf), &n, nullptr) != 0 && n > 0) {
        got.append(buf, n);
    }
    ::CloseHandle(read_end);
    REQUIRE(ok);
    REQUIRE(got == "v=1 x\n");
}

TEST_CASE("record_file: a short write reported as success is a failure", "[platform][record_file]") {
    // 9장: 일부만 쓰인 경우도 실패다. WriteFile 이 성공을 돌려주면서 넘긴 것보다 적게 쓴 경우가 그것이다.
    // 자리가 모자란 논블로킹 바이트 파이프가 그렇게 돌려준다. 이 기기에서는 다 들어가지 않으면 0 바이트였다.
    const auto full_pipe = [](HANDLE& read_end, HANDLE& write_end) {
        REQUIRE(::CreatePipe(&read_end, &write_end, nullptr, 4096) != 0);
        DWORD mode = PIPE_NOWAIT;
        REQUIRE(::SetNamedPipeHandleState(write_end, &mode, nullptr, nullptr) != 0);
    };
    const std::string big(1 << 20, 'x');

    // 전제: 이 파이프에서 WriteFile 은 성공을 돌려주고 다 쓰지 못한다. 아니면 이 케이스는 일부 쓰기를 보지 않는다.
    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    full_pipe(read_end, write_end);
    DWORD written = 0;
    const BOOL raw_ok = ::WriteFile(write_end, big.data(), static_cast<DWORD>(big.size()), &written, nullptr);
    ::CloseHandle(write_end);
    ::CloseHandle(read_end);
    REQUIRE(raw_ok != 0);
    REQUIRE(written < big.size());

    full_pipe(read_end, write_end);
    const bool ok = sangtachi::platform::write_whole(write_end, big);
    ::CloseHandle(write_end);
    ::CloseHandle(read_end);
    REQUIRE_FALSE(ok);
}

TEST_CASE("record_file: a closed file does not write", "[platform][record_file]") {
    sangtachi::platform::RecordFile closed;
    REQUIRE_FALSE(closed.is_open());
    REQUIRE_FALSE(closed.write("x\n"));
    REQUIRE_FALSE(sangtachi::platform::write_whole(nullptr, "x\n"));
}

TEST_CASE("record_file: wall clock is UTC unix milliseconds and PID is this process",
          "[platform][record_file]") {
    using namespace std::chrono;
    const auto before = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    const auto got = sangtachi::platform::wall_clock_unix_ms();
    const auto after = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    REQUIRE(static_cast<std::int64_t>(got) >= before - 1);
    REQUIRE(static_cast<std::int64_t>(got) <= after + 1);
    REQUIRE(sangtachi::platform::current_process_id() == ::GetCurrentProcessId());
}
