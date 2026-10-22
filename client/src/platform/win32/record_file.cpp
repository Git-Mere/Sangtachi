#include "sangtachi/platform/record_file.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <new>
#include <string>
#include <string_view>
#include <utility>

namespace sangtachi::platform {
namespace {

// 32767 자가 확장 경로의 상한이다. 끝의 널 한 자리를 더한다.
constexpr DWORD kPathBuffer = 32768;

// 1601-01-01 과 1970-01-01 사이의 100ns 단위 수.
constexpr std::uint64_t kUnixEpochIn100ns = 116444736000000000ull;

RecordFileOpen failure(std::string_view op, DWORD error) {
    RecordFileOpen result;
    result.failed_op = op;
    result.error = static_cast<std::uint32_t>(error);
    return result;
}

RecordFileOpen open_in_exe_dir(std::wstring_view folder, std::string_view file_name) {
    std::wstring path(kPathBuffer, L'\0');
    const DWORD length = ::GetModuleFileNameW(nullptr, path.data(), kPathBuffer);
    if (length == 0) {
        return failure("GetModuleFileNameW", ::GetLastError());
    }
    // 버퍼를 다 채웠으면 잘린 것이다 (9장: 경로를 끝까지 얻지 못하면 열기 실패).
    if (length >= kPathBuffer) {
        return failure("GetModuleFileNameW", ERROR_INSUFFICIENT_BUFFER);
    }
    path.resize(length);
    const auto slash = path.rfind(L'\\');
    if (slash == std::wstring::npos) {
        return failure("GetModuleFileNameW", 0);
    }
    path.resize(slash + 1);
    path += folder;

    if (::CreateDirectoryW(path.c_str(), nullptr) == 0) {
        const DWORD error = ::GetLastError();
        if (error != ERROR_ALREADY_EXISTS) {
            return failure("CreateDirectoryW", error);
        }
    }

    path += L'\\';
    for (const char c : file_name) {
        path += static_cast<wchar_t>(static_cast<unsigned char>(c));
    }
    const HANDLE handle = ::CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                                        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return failure("CreateFileW", ::GetLastError());
    }
    RecordFileOpen result;
    result.file = RecordFile(handle);
    return result;
}

}  // namespace

std::uint64_t wall_clock_unix_ms() noexcept {
    FILETIME ft{};
    ::GetSystemTimePreciseAsFileTime(&ft);
    const std::uint64_t ticks =
        (static_cast<std::uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    if (ticks < kUnixEpochIn100ns) {
        return 0;
    }
    return (ticks - kUnixEpochIn100ns) / 10000;
}

std::uint32_t current_process_id() noexcept {
    return static_cast<std::uint32_t>(::GetCurrentProcessId());
}

bool write_whole(FileHandle handle, std::string_view bytes) noexcept {
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE || bytes.size() > MAXDWORD) {
        return false;
    }
    DWORD written = 0;
    const DWORD size = static_cast<DWORD>(bytes.size());
    if (::WriteFile(static_cast<HANDLE>(handle), bytes.data(), size, &written, nullptr) == 0) {
        return false;
    }
    // 일부만 쓰인 것도 실패다. 남은 것을 이어 쓰지 않는다 (9장: 한 줄은 쓰기 호출 하나다).
    return written == size;
}

RecordFile::~RecordFile() {
    if (handle_ != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(handle_));
    }
}

RecordFile::RecordFile(RecordFile&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}

RecordFile& RecordFile::operator=(RecordFile&& other) noexcept {
    if (this != &other) {
        if (handle_ != nullptr) {
            ::CloseHandle(static_cast<HANDLE>(handle_));
        }
        handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
}

RecordFileOpen open_record_file(std::string_view file_name) noexcept {
    try {
        return open_in_exe_dir(L"records", file_name);
    } catch (const std::bad_alloc&) {
        return failure("open_record_file", ERROR_NOT_ENOUGH_MEMORY);
    }
}

RecordFileOpen open_log_file(std::string_view file_name) noexcept {
    try {
        return open_in_exe_dir(L"logs", file_name);
    } catch (const std::bad_alloc&) {
        return failure("open_log_file", ERROR_NOT_ENOUGH_MEMORY);
    }
}

}  // namespace sangtachi::platform
