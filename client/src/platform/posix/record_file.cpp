#include "sangtachi/platform/record_file.hpp"

#include "exe_dir.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

// 파일 핸들은 fd + 1 이다 (ADR 0035 결정 3).

namespace sangtachi::platform {
namespace {

RecordFileOpen failure(std::string_view op, int error) {
    RecordFileOpen result;
    result.failed_op = op;
    result.error = static_cast<std::uint32_t>(error);
    return result;
}

int fd_of(FileHandle handle) noexcept {
    return static_cast<int>(reinterpret_cast<std::uintptr_t>(handle) - 1);
}

RecordFileOpen open_in_exe_dir(std::string_view folder, std::string_view file_name) {
    std::uint32_t dir_error = 0;
    std::string_view dir_op;
    const std::optional<std::string> dir = executable_dir(dir_error, dir_op);
    if (!dir) {
        return failure(dir_op, static_cast<int>(dir_error));
    }
    std::string path = *dir;
    path += '/';
    path += folder;

    if (::mkdir(path.c_str(), 0755) != 0 && errno != EEXIST) {
        return failure("mkdir", errno);
    }

    path += '/';
    path += file_name;
    // O_EXCL: 같은 이름이 있으면 실패다 (9장). O_APPEND: 추가 전용.
    const int fd = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0) {
        return failure("open", errno);
    }
    RecordFileOpen result;
    result.file = RecordFile(reinterpret_cast<FileHandle>(static_cast<std::uintptr_t>(fd) + 1));
    return result;
}

}  // namespace

std::uint64_t wall_clock_unix_ms() noexcept {
    timespec ts{};
    if (::clock_gettime(CLOCK_REALTIME, &ts) != 0 || ts.tv_sec < 0) {
        return 0;
    }
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000u +
           static_cast<std::uint64_t>(ts.tv_nsec) / 1000000u;
}

std::uint32_t current_process_id() noexcept {
    return static_cast<std::uint32_t>(::getpid());
}

bool write_whole(FileHandle handle, std::string_view bytes) noexcept {
    if (handle == nullptr) {
        return false;
    }
    // 일부만 쓰인 것도 실패다. 남은 것을 이어 쓰지 않는다 (9장: 한 줄은 쓰기 호출 하나다).
    const ssize_t written = ::write(fd_of(handle), bytes.data(), bytes.size());
    return written >= 0 && static_cast<std::size_t>(written) == bytes.size();
}

RecordFile::~RecordFile() {
    if (handle_ != nullptr) {
        ::close(fd_of(handle_));
    }
}

RecordFile::RecordFile(RecordFile&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}

RecordFile& RecordFile::operator=(RecordFile&& other) noexcept {
    if (this != &other) {
        if (handle_ != nullptr) {
            ::close(fd_of(handle_));
        }
        handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
}

RecordFileOpen open_record_file(std::string_view file_name) noexcept {
    try {
        return open_in_exe_dir("records", file_name);
    } catch (const std::bad_alloc&) {
        return failure("open_record_file", ENOMEM);
    }
}

RecordFileOpen open_log_file(std::string_view file_name) noexcept {
    try {
        return open_in_exe_dir("logs", file_name);
    } catch (const std::bad_alloc&) {
        return failure("open_log_file", ENOMEM);
    }
}

}  // namespace sangtachi::platform
