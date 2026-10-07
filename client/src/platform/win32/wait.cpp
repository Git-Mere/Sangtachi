#include "sangtachi/platform/wait.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <span>

namespace sangtachi::platform {

WaitMillis monotonic_ms() noexcept {
    // 밀리초 단위 64비트라 실질적으로 랩어라운드가 없다 (concurrency.md 4장 타이머).
    return static_cast<WaitMillis>(::GetTickCount64());
}

WaitHandle create_event(ResetMode mode) noexcept {
    const BOOL manual_reset = (mode == ResetMode::Manual) ? TRUE : FALSE;
    return ::CreateEventW(nullptr, manual_reset, FALSE, nullptr);
}

bool signal_event(WaitHandle handle) noexcept {
    if (handle == nullptr) {
        return false;
    }
    return ::SetEvent(static_cast<HANDLE>(handle)) != 0;
}

void close_event(WaitHandle handle) noexcept {
    if (handle == nullptr) {
        return;
    }
    ::CloseHandle(static_cast<HANDLE>(handle));
}

WaitHandle duplicate_event(WaitHandle handle) noexcept {
    if (handle == nullptr) {
        return nullptr;
    }
    HANDLE duplicated = nullptr;
    if (::DuplicateHandle(::GetCurrentProcess(), static_cast<HANDLE>(handle),
                          ::GetCurrentProcess(), &duplicated, 0, FALSE,
                          DUPLICATE_SAME_ACCESS) == 0) {
        return nullptr;
    }
    return duplicated;
}

WaitResult wait_any(std::span<const WaitHandle> handles, std::uint32_t timeout_ms) noexcept {
    // 먼저 검증한다. 빈 배열과 상한 초과는 OS 에 묻지 않고 여기서 판정한다.
    // 널이 섞인 배열은 OS 가 WAIT_FAILED 로 돌려주므로 아래 경로가 받는다
    // (concurrency.md 2장 대기).
    if (handles.empty() || handles.size() > MAXIMUM_WAIT_OBJECTS) {
        return WaitResult{WaitStatus::Failed, 0, static_cast<std::uint32_t>(ERROR_INVALID_PARAMETER)};
    }

    const DWORD count = static_cast<DWORD>(handles.size());
    const DWORD result = ::WaitForMultipleObjects(
        count, reinterpret_cast<const HANDLE*>(handles.data()), FALSE,
        static_cast<DWORD>(timeout_ms));

    if (result == WAIT_FAILED) {
        return WaitResult{WaitStatus::Failed, 0, static_cast<std::uint32_t>(::GetLastError())};
    }
    if (result == WAIT_TIMEOUT) {
        return WaitResult{WaitStatus::Timeout, 0, 0};
    }
    if (result >= WAIT_OBJECT_0 && result < WAIT_OBJECT_0 + count) {
        return WaitResult{WaitStatus::Signaled, static_cast<std::size_t>(result - WAIT_OBJECT_0), 0};
    }

    // 남는 것은 WAIT_ABANDONED 범위뿐이고 그것은 뮤텍스에만 난다. 이 대기 집합에는
    // 이벤트 핸들만 들어가므로 여기에 닿으면 대기 집합 구성이 깨진 것이다. 모르는 값을
    // "신호됨" 으로 읽지 않는다. OS 오류 코드가 아니므로 error 는 0 이다.
    //
    // **이 재배치가 도달 불가 경로 하나의 동작을 바꾼다.** 앞의 loop.cpp 는 WAIT_FAILED 도
    // 종료 인덱스도 아닌 반환값을 전부 흘려보내 그 바퀴의 비우기로 넘겼고, 지금은 Failed 가
    // 되어 루프가 기록하고 정리 경로로 간다. 바꾼 쪽을 고른 이유는 뜻을 모르는 반환값을
    // 정상으로 읽는 것이 더 나쁘기 때문이다. 크로스 모델 리뷰가 이 자리를 warn 으로 짚었고
    // 그대로 두기로 했다.
    return WaitResult{WaitStatus::Failed, 0, 0};
}

}  // namespace sangtachi::platform
