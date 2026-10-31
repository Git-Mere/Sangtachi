#include "sangtachi/platform/session_end.hpp"

namespace sangtachi::platform {

// 창 메시지가 없다. GUI 는 Windows 만이다 (ADR 0035 결정 1).
bool is_session_ending(const void* /*message*/) noexcept {
    return false;
}

}  // namespace sangtachi::platform
