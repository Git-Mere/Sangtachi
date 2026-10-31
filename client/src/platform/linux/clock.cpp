#include "sangtachi/platform/wait.hpp"

#include <time.h>

#include <cstdint>

namespace sangtachi::platform {
namespace {

std::uint64_t read_clock(clockid_t clock, std::uint64_t divisor) noexcept {
    timespec now{};
    ::clock_gettime(clock, &now);
    return static_cast<std::uint64_t>(now.tv_sec) * (1000000000u / divisor) +
           static_cast<std::uint64_t>(now.tv_nsec) / divisor;
}

}  // namespace

// 절전 시간을 포함한다. GetTickCount64 와 같은 성질이라 절전 뒤에도 유휴 타임아웃이 흐른다.
WaitMillis monotonic_ms() noexcept {
    return read_clock(CLOCK_BOOTTIME, 1000000u);
}

std::uint64_t monotonic_us() noexcept {
    return read_clock(CLOCK_MONOTONIC, 1000u);
}

}  // namespace sangtachi::platform
