#include "sangtachi/hash.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>

namespace sangtachi {
namespace {

constexpr std::size_t kDigestBytes = 32;

bool succeeded(NTSTATUS status) noexcept {
    return status >= 0;
}

}  // namespace

std::optional<std::string> sha256_hex(std::span<const std::byte> data) {
    std::array<unsigned char, kDigestBytes> digest{};

    // BCRYPT_HASH_REUSABLE_FLAG 를 쓰지 않는다. 한 번 쓰고 버리는 호출이다.
    const NTSTATUS status = ::BCryptHash(
        BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
        // 입력이 비어 있으면 data() 가 널일 수 있다. 길이가 0 이면 포인터를 보지 않지만
        // 널을 넘기지 않도록 자리 하나를 가리키게 둔다.
        const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(
            data.empty() ? static_cast<const void*>(digest.data()) : data.data())),
        static_cast<ULONG>(data.size()),
        digest.data(), static_cast<ULONG>(digest.size()));

    if (!succeeded(status)) {
        return std::nullopt;
    }

    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(kDigestBytes * 2);
    for (const unsigned char byte : digest) {
        out.push_back(kHex[byte >> 4]);
        out.push_back(kHex[byte & 0x0F]);
    }
    return out;
}

std::optional<std::string> sha256_short(std::span<const std::byte> data) {
    auto full = sha256_hex(data);
    if (!full) {
        return std::nullopt;
    }
    full->resize(kShortHashChars);
    return full;
}

}  // namespace sangtachi
