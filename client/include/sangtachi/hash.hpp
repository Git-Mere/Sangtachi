#pragma once

// SHA-256 (architecture.md 3.5 기동 입력의 `rx.raw`).
//
// Windows CNG(bcrypt)를 쓴다. OS 기본 제공이라 spec.md NFR-5 의 서드파티 의존성이 아니고,
// C-2 가 요구하는 네이티브 Windows API 다. 직접 구현하지 않는 이유는 그 구현이 시험
// 대상으로 하나 더 늘 뿐 얻는 것이 없기 때문이다.

#include <cstddef>
#include <optional>
#include <span>
#include <string>

namespace sangtachi {

// architecture.md 3.5 가 정한 `rx.raw` 의 `sha256`. 16진 해시의 앞 16자다.
inline constexpr std::size_t kShortHashChars = 16;

// 소문자 16진수 64자. 실패하면 값이 없다.
[[nodiscard]] std::optional<std::string> sha256_hex(std::span<const std::byte> data);

// 위의 앞 kShortHashChars 자. 실패하면 값이 없다.
[[nodiscard]] std::optional<std::string> sha256_short(std::span<const std::byte> data);

}  // namespace sangtachi
