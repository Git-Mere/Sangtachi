#include "sangtachi/platform/session_end.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace sangtachi::platform {

bool is_session_ending(const void* message) noexcept {
    if (message == nullptr) {
        return false;
    }
    const auto* msg = static_cast<const MSG*>(message);
    // wParam 이 거짓이면 다른 프로그램이 종료를 취소했다. 세션은 계속된다.
    return msg->message == WM_ENDSESSION && msg->wParam != FALSE;
}

}  // namespace sangtachi::platform
