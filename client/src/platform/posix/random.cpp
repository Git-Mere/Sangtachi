#include "sangtachi/platform/random.hpp"

#include <unistd.h>

#include <algorithm>
#include <cstddef>
#include <span>

namespace sangtachi::platform {
namespace {

// getentropy 가 한 번에 받는 상한이다.
constexpr std::size_t kEntropyChunk = 256;

}  // namespace

bool random_bytes(std::span<std::byte> out) noexcept {
    // getentropy 는 Linux 와 macOS 에 다 있고, 상한 안쪽이면 다 채우거나 실패한다. 부분 반환이 없다.
    for (std::size_t done = 0; done < out.size();) {
        const std::size_t chunk = std::min(out.size() - done, kEntropyChunk);
        // 실패를 삼키지 않는다 (win32 판과 같은 이유).
        if (::getentropy(out.data() + done, chunk) != 0) {
            return false;
        }
        done += chunk;
    }
    return true;
}

}  // namespace sangtachi::platform
