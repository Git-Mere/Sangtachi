#include "sangtachi/platform/stdout.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <climits>
#include <cstddef>
#include <new>
#include <string>
#include <string_view>

namespace sangtachi::platform {
namespace {

// 콘솔 쓰기. UTF-8 을 UTF-16 으로 바꾼다. 콘솔 코드 페이지는 보지도 바꾸지도 않는다.
bool write_console(HANDLE handle, std::string_view utf8_line) {
    std::wstring wide;
    if (!utf8_line.empty()) {
        if (utf8_line.size() > static_cast<std::size_t>(INT_MAX)) {
            return false;
        }
        const int in_len = static_cast<int>(utf8_line.size());
        const int needed = ::MultiByteToWideChar(CP_UTF8, 0, utf8_line.data(), in_len, nullptr, 0);
        if (needed <= 0) {
            return false;
        }
        wide.resize(static_cast<std::size_t>(needed));
        if (::MultiByteToWideChar(CP_UTF8, 0, utf8_line.data(), in_len, wide.data(), needed) !=
            needed) {
            return false;
        }
    }
    wide.push_back(L'\n');

    std::size_t done = 0;
    while (done < wide.size()) {
        DWORD written = 0;
        const DWORD chunk = static_cast<DWORD>(wide.size() - done);
        if (::WriteConsoleW(handle, wide.data() + done, chunk, &written, nullptr) == 0 ||
            written == 0) {
            return false;
        }
        done += written;
    }
    return true;
}

// 파일과 파이프. 바이트 그대로와 LF 하나. 한 번의 버퍼로 내 줄이 갈라지지 않게 한다.
bool write_bytes(HANDLE handle, std::string_view utf8_line) {
    std::string line;
    line.reserve(utf8_line.size() + 1);
    line.append(utf8_line);
    line.push_back('\n');

    std::size_t done = 0;
    while (done < line.size()) {
        DWORD written = 0;
        const std::size_t left = line.size() - done;
        const DWORD chunk = left > 0x7FFFFFFFu ? 0x7FFFFFFFu : static_cast<DWORD>(left);
        if (::WriteFile(handle, line.data() + done, chunk, &written, nullptr) == 0 || written == 0) {
            return false;
        }
        done += written;
    }
    return true;
}

}  // namespace

OutputHandle stdout_handle() noexcept {
    HANDLE handle = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (handle == INVALID_HANDLE_VALUE) {
        return nullptr;
    }
    return handle;
}

bool write_line(OutputHandle handle, std::string_view utf8_line) noexcept {
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    const HANDLE h = static_cast<HANDLE>(handle);
    try {
        // 콘솔 모드를 읽을 수 있으면 콘솔이다. 리다이렉트된 핸들(파일, 파이프)에서는 실패한다.
        DWORD mode = 0;
        if (::GetConsoleMode(h, &mode) != 0) {
            return write_console(h, utf8_line);
        }
        return write_bytes(h, utf8_line);
    } catch (const std::bad_alloc&) {
        return false;
    }
}

bool write_stdout_line(std::string_view utf8_line) noexcept {
    return write_line(stdout_handle(), utf8_line);
}

}  // namespace sangtachi::platform
