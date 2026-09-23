cmake_minimum_required(VERSION 3.20)

project(Hamychi LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(HamychiWarnings)

option(HAMYCHI_BUILD_TESTS "Build the Catch2 test suite" ON)

add_subdirectory(client)

if(HAMYCHI_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
