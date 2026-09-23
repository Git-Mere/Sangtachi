# 클라이언트 (spec.md C-1, C-2: Windows 10/11 x64, C++20, MSVC)
#
# hamychi_core  main() 을 뺀 전부. 시험 실행 파일이 이것을 링크한다.
# hamychi_client  main() 만 얹은 실행 파일.
#
# main() 을 라이브러리에 넣지 않는 이유는 Catch2 가 자기 main() 을 제공하기 때문이다.
# 두 개가 같이 링크되면 시험 실행 파일이 만들어지지 않는다.

add_library(hamychi_core STATIC)

target_include_directories(hamychi_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(hamychi_core
    PRIVATE hamychi_warnings
    PUBLIC  ws2_32
)

add_subdirectory(src)

add_executable(hamychi_client
    src/main.cpp
)

target_link_libraries(hamychi_client
    PRIVATE hamychi_core
    PRIVATE hamychi_warnings
)
