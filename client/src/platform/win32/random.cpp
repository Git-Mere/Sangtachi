#include "sangtachi/platform/random.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>

#include <cstddef>
#include <limits>
#include <span>

namespace sangtachi::platform {
namespace {

bool succeeded(NTSTATUS status) noexcept {
    return status >= 0;
}

}  // namespace

bool random_bytes(std::span<std::byte> out) noexcept {
    if (out.empty()) {
        // 채울 것이 없다. data() 가 널일 수 있으므로 호출하지 않는다.
        return true;
    }
    // BCryptGenRandom 의 길이 인자가 ULONG 이다. 좁히기 전에 거른다. 좁히면 크기가
    // 감싸 돌아 버퍼의 앞부분만 채우고 나머지는 이전 내용이 남은 채로 성공을 알린다
    // (network/udp_socket.cpp 의 sendto 길이 검사와 같은 이유다).
    if (out.size() > static_cast<std::size_t>((std::numeric_limits<ULONG>::max)())) {
        return false;
    }

    // BCRYPT_RNG_ALG_HANDLE 는 CNG 가 주는 의사 핸들이다. 알고리즘 공급자를 열고 닫지
    // 않는다. 같은 디렉터리의 hash.cpp 가 BCRYPT_SHA256_ALG_HANDLE 을 쓰는 것과 같다.
    const NTSTATUS status = ::BCryptGenRandom(
        BCRYPT_RNG_ALG_HANDLE,
        reinterpret_cast<PUCHAR>(out.data()),
        static_cast<ULONG>(out.size()),
        0);

    // 실패를 삼키지 않는다. 트랜잭션 ID 가 예측 가능해지면 응답을 가르는 근거가 사라진다
    // (protocol.md 13장 STUN 사용 범위의 응답 검증).
    return succeeded(status);
}

}  // namespace sangtachi::platform
