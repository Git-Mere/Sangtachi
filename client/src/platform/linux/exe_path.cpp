#include "../posix/exe_dir.hpp"

#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace sangtachi::platform {
namespace {

// PATH_MAX 와 같은 크기다. 커널이 /proc/self/exe 로 주는 경로는 이보다 길 수 없다.
constexpr std::size_t kPathBuffer = 4096;

}  // namespace

std::optional<std::string> executable_dir(std::uint32_t& error, std::string_view& failed_op) {
    failed_op = "readlink";
    std::string path(kPathBuffer, '\0');
    const ssize_t length = ::readlink("/proc/self/exe", path.data(), path.size());
    if (length < 0) {
        error = static_cast<std::uint32_t>(errno);
        return std::nullopt;
    }
    // readlink 는 잘라도 알리지 않는다. 버퍼를 다 채웠으면 잘린 것으로 본다.
    if (static_cast<std::size_t>(length) >= path.size()) {
        error = ENAMETOOLONG;
        return std::nullopt;
    }
    path.resize(static_cast<std::size_t>(length));
    const auto slash = path.rfind('/');
    if (slash == std::string::npos) {
        error = 0;
        return std::nullopt;
    }
    path.resize(slash);
    return path;
}

}  // namespace sangtachi::platform
