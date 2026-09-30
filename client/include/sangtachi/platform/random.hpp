#pragma once

// CSPRNG 바이트 (protocol.md 13장 트랜잭션 ID, 4.3 epoch, 5.1 nonce).
// 구현은 client/src/platform/win32/ 에 있다 (ADR 0010).

#include <cstddef>
#include <span>

namespace sangtachi::platform {

// 암호학적 난수로 채운다. 실패하면 거짓이고 버퍼 내용은 뜻이 없다.
//
// 성공하면 out 전체를 채운다. 그 밖의 바이트는 건드리지 않는다.
//
// 빈 span 은 성공이다. 채울 것이 없는 요청과 실패를 같은 값으로 알리면 부르는 쪽이
// 둘을 가를 수 없다.
//
// Winsock 과 달리 앞선 초기화가 필요하지 않다. 호출마다 OS 에 묻는다.
[[nodiscard]] bool random_bytes(std::span<std::byte> out) noexcept;

}  // namespace sangtachi::platform
