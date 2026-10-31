#include "sangtachi/platform/stdout.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <new>
#include <string>
#include <string_view>

// 핸들은 fd + 1 이다. fd 0 도 널이 되지 않는다 (ADR 0035 결정 3).

namespace sangtachi::platform {
namespace {

OutputHandle handle_for(int fd) noexcept {
    if (::fcntl(fd, F_GETFD) < 0) {
        return nullptr;
    }
    return reinterpret_cast<OutputHandle>(static_cast<std::uintptr_t>(fd) + 1);
}

int fd_of(OutputHandle handle) noexcept {
    return static_cast<int>(reinterpret_cast<std::uintptr_t>(handle) - 1);
}

// 터미널도 UTF-8 바이트를 그대로 받는다. 콘솔 쓰기 경로가 따로 없다. 한 번의 버퍼로 내 줄이 갈라지지 않게 한다.
bool write_bytes(int fd, std::string_view utf8, bool newline) {
    std::string line;
    line.reserve(utf8.size() + 1);
    line.append(utf8);
    if (newline) {
        line.push_back('\n');
    }

    std::size_t done = 0;
    while (done < line.size()) {
        const ssize_t written = ::write(fd, line.data() + done, line.size() - done);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            return false;
        }
        done += static_cast<std::size_t>(written);
    }
    return true;
}

bool write(OutputHandle handle, std::string_view utf8, bool newline) noexcept {
    if (handle == nullptr) {
        return false;
    }
    try {
        return write_bytes(fd_of(handle), utf8, newline);
    } catch (const std::bad_alloc&) {
        return false;
    }
}

}  // namespace

OutputHandle stdout_handle() noexcept {
    return handle_for(STDOUT_FILENO);
}

OutputHandle stderr_handle() noexcept {
    return handle_for(STDERR_FILENO);
}

bool is_console(OutputHandle handle) noexcept {
    if (handle == nullptr) {
        return false;
    }
    return ::isatty(fd_of(handle)) == 1;
}

bool write_line(OutputHandle handle, std::string_view utf8_line) noexcept {
    return write(handle, utf8_line, true);
}

bool write_text(OutputHandle handle, std::string_view utf8_text) noexcept {
    return write(handle, utf8_text, false);
}

bool write_stdout_line(std::string_view utf8_line) noexcept {
    return write_line(stdout_handle(), utf8_line);
}

}  // namespace sangtachi::platform
