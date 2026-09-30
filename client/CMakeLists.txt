# 클라이언트 (spec.md C-1, C-2: Windows 10/11 x64, C++20, MSVC)
#
# sangtachi_core  main() 을 뺀 전부. 시험 실행 파일이 이것을 링크한다.
# sangtachi_client  main() 만 얹은 실행 파일.
#
# main() 을 라이브러리에 넣지 않는 이유는 Catch2 가 자기 main() 을 제공하기 때문이다.
# 두 개가 같이 링크되면 시험 실행 파일이 만들어지지 않는다.

# 대상은 Windows 하나다 (spec.md C-1, C-2). 다른 곳에서는 설정 단계에서 멈춘다.
#
# OS 헤더를 포함하는 원본은 전부 src/platform/win32/ 아래에 있고 다른 플랫폼의 짝은
# 없다. 그대로 설정하면 원본 목록과 링크 목록이 없는 API 를 가리킨 채 빌드가 진행된다.
if(NOT WIN32)
    message(FATAL_ERROR
        "Sangtachi client targets Windows 10/11 x64 only (spec.md C-1, C-2). "
        "Platform sources live under client/src/platform/win32/ and no other port exists. "
        "See docs/kor/decisions/0010-플랫폼-이식-이음새.md")
endif()

add_library(sangtachi_core STATIC)

target_include_directories(sangtachi_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(sangtachi_core
    PRIVATE sangtachi_warnings
)

# OS 라이브러리는 플랫폼 조건 아래 둔다. 위 FATAL_ERROR 가 이미 막지만, 링크 목록이
# 어느 플랫폼의 것인지를 파일에 드러내는 것이 이 조건의 일이다.
if(WIN32)
    target_link_libraries(sangtachi_core
        PUBLIC ws2_32   # Winsock2 (src/platform/win32/wsa.cpp, udp_socket.cpp)
        PUBLIC bcrypt   # CNG SHA-256 (src/platform/win32/hash.cpp)
    )
endif()

add_subdirectory(src)

add_executable(sangtachi_client
    src/main.cpp
)

if(SANGTACHI_TEST_BUILD)
    target_compile_definitions(sangtachi_client PRIVATE SANGTACHI_TEST_BUILD)
endif()

target_link_libraries(sangtachi_client
    PRIVATE sangtachi_core
    PRIVATE sangtachi_warnings
)
