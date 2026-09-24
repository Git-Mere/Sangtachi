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
    network/udp_socket_test.cpp
    network/wsa_test.cpp
)

target_link_libraries(hamychi_tests
    PRIVATE hamychi_core
    PRIVATE hamychi_warnings
    PRIVATE Catch2::Catch2WithMain
)

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)
# 케이스마다 시간 상한을 둔다.
#
# 상한이 없으면 매달리는 케이스 하나가 하네스를 영원히 붙잡는다. 실측으로 겪었다.
# WSAEventSelect 를 빠뜨린 변이가 소켓을 블로킹으로 남겨 recvfrom 이 돌아오지 않았고,
# 그 변이는 `FAIL` 이 아니라 무한 대기로 나타났다.
#
# 30초는 가장 느린 케이스(루프백 왕복과 100ms 잠)의 백 배가 넘는다.
catch_discover_tests(hamychi_tests PROPERTIES TIMEOUT 30)
