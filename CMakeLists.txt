cmake_minimum_required(VERSION 3.20)

project(Sangtachi LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(SangtachiWarnings)

option(SANGTACHI_BUILD_TESTS "Build the Catch2 test suite" ON)

# 시험 빌드. probe200 타이머 같은 검증 전용 장치를 켠다 (architecture.md 9장).
# 켜진 빌드는 기동 시 WARN 한 줄을 남긴다.
option(SANGTACHI_TEST_BUILD "Enable verification-only instrumentation" OFF)

# ASan 시험 빌드 (roadmap.md Phase 5). 퍼즈 시험이 이 빌드에서 메모리 오류를 판정한다.
# 이 줄 아래의 모든 대상(Catch2 포함)에 같이 건다. 한 실행 파일 안에서 계측한 목적 파일과
# 계측하지 않은 목적 파일을 섞지 않는다.
option(SANGTACHI_ASAN "Build every target with AddressSanitizer (MSVC /fsanitize=address)" OFF)
if(SANGTACHI_ASAN)
    if(NOT MSVC)
        message(FATAL_ERROR "SANGTACHI_ASAN is wired for MSVC only (spec.md C-1, C-2)")
    endif()
    add_compile_options(/fsanitize=address)
    # /fsanitize=address 는 /RTC 와 같이 쓸 수 없다. CMake 의 Debug 기본 플래그가 /RTC1 을 넣는다.
    string(REGEX REPLACE "/RTC[^ ]*" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
    # 증분 링크도 ASan 과 맞지 않는다. 링커가 경고하고 끈다. 경고 없이 처음부터 끈다.
    add_link_options(/INCREMENTAL:NO)
endif()

add_subdirectory(client)

if(SANGTACHI_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
