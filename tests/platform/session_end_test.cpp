#include "sangtachi/platform/session_end.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <catch2/catch_test_macros.hpp>

// 시험 케이스 이름은 ASCII 로만 적는다. 이유는 network/wsa_test.cpp 머리에 있다.

TEST_CASE("session end: only WM_ENDSESSION with wParam TRUE ends the session", "[session_end]") {
    // concurrency.md 7장 GUI 빌드의 종료: 로그오프와 시스템 종료는 WM_ENDSESSION 이다. wParam 이 거짓이면 종료가
    // 취소된 것이다.
    struct Case {
        const char* what;
        UINT message;
        WPARAM wparam;
        bool ending;
    };
    const Case cases[] = {
        {"end session", WM_ENDSESSION, TRUE, true},
        {"end session, cancelled", WM_ENDSESSION, FALSE, false},
        {"query end session", WM_QUERYENDSESSION, TRUE, false},
        {"close", WM_CLOSE, TRUE, false},
    };
    for (const auto& c : cases) {
        INFO(c.what);
        MSG msg{};
        msg.message = c.message;
        msg.wParam = c.wparam;
        CHECK(sangtachi::platform::is_session_ending(&msg) == c.ending);
    }
    CHECK_FALSE(sangtachi::platform::is_session_ending(nullptr));
}
