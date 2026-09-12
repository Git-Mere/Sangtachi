cmake_minimum_required(VERSION 3.20)

project(Hamychi LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(Hamychi
    src/main.cpp
)

target_include_directories(Hamychi PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)