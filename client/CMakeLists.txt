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
        PUBLIC iphlpapi # IP Helper 인터페이스 열거와 경로 조회 (src/platform/win32/interfaces.cpp)
    )
endif()

# Wintun 배포물 (windows-prereq.md 4절). 설정할 때 받아서 판정한다. 판정을 지나지 못하면 설정이 멈춘다.
include(Wintun)
sangtachi_fetch_wintun()

# 실행 시 확인의 기대값. 값의 출처는 cmake/Wintun.cmake 하나이고 여기서 헤더로 옮긴다.
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/sangtachi/wintun_expected.hpp"
     CONTENT "#pragma once\n\n// cmake/Wintun.cmake 에서 만든다. 고치지 않는다.\n\n#include <string_view>\n\nnamespace sangtachi::platform {\ninline constexpr std::string_view kWintunDllSha256 = \"@SANGTACHI_WINTUN_DLL_SHA256@\";\n}  // namespace sangtachi::platform\n"
     @ONLY)
target_include_directories(sangtachi_core PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
    "${SANGTACHI_WINTUN_INCLUDE}"
)

add_subdirectory(src)

add_executable(sangtachi_client
    src/main.cpp
)

# 시험 빌드 장치는 실행 파일에만 컴파일한다. core 에 넣지 않는다 (testbuild/instrument.hpp, ADR 0023).
if(SANGTACHI_TEST_BUILD)
    target_compile_definitions(sangtachi_client PRIVATE SANGTACHI_TEST_BUILD)
    target_sources(sangtachi_client PRIVATE src/testbuild/instrument.cpp)
endif()

target_link_libraries(sangtachi_client
    PRIVATE sangtachi_core
    PRIVATE sangtachi_warnings
)

# 실행 파일 옆에 두는 것 셋 (architecture.md 7장 어댑터 수명). 클라이언트는 wintun.dll 을 실행 파일의
# 폴더에서만 불러오고, 서브넷 충돌 검사와 방화벽 규칙은 같은 폴더의 스크립트를 부른다.
#
# configure_file 로 둔다. 원본이 바뀌면 CMake 가 다시 설정하면서 다시 복사한다. 링크 뒤에 복사하면
# 스크립트만 고친 빌드가 낡은 판정을 실행 파일 옆에 남긴다. 실행 파일은 Ninja 에서 이 폴더에 생긴다.
configure_file("${SANGTACHI_WINTUN_DLL}" "${CMAKE_CURRENT_BINARY_DIR}/wintun.dll" COPYONLY)
configure_file("${PROJECT_SOURCE_DIR}/tools/winprereq/Test-SubnetOverlap.ps1"
               "${CMAKE_CURRENT_BINARY_DIR}/Test-SubnetOverlap.ps1" COPYONLY)
configure_file("${PROJECT_SOURCE_DIR}/tools/winprereq/Initialize-FirewallRules.ps1"
               "${CMAKE_CURRENT_BINARY_DIR}/Initialize-FirewallRules.ps1" COPYONLY)
