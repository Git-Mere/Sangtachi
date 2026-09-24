cmake_minimum_required(VERSION 3.20)

project(Hamychi LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(HamychiWarnings)

option(HAMYCHI_BUILD_TESTS "Build the Catch2 test suite" ON)

# 시험 빌드. probe200 타이머 같은 검증 전용 장치를 켠다 (architecture.md 9장).
# 켜진 빌드는 기동 시 WARN 한 줄을 남긴다.
option(HAMYCHI_TEST_BUILD "Enable verification-only instrumentation" OFF)

add_subdirectory(client)

if(HAMYCHI_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
