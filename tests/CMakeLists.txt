# 시험 실행 파일.
#
# Catch2 v3 는 시험 전용 서드파티 의존성이다. 승인 범위는 decisions/0005 가 갖는다.
# 제품 코드(hamychi_core, hamychi_client)는 이것을 링크하지 않는다.

include(FetchContent)

FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.16.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(Catch2)

add_executable(hamychi_tests
    args_test.cpp
    counters_test.cpp
    log_test.cpp
    network/endpoint_test.cpp
    network/wsa_test.cpp
)

target_link_libraries(hamychi_tests
    PRIVATE hamychi_core
    PRIVATE hamychi_warnings
    PRIVATE Catch2::Catch2WithMain
)

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(hamychi_tests)
