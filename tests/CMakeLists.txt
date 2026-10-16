# 시험 실행 파일.
#
# Catch2 v3 는 시험 전용 서드파티 의존성이다. 승인 범위는 decisions/0005 가 갖는다.
# 제품 코드(sangtachi_core, sangtachi_client)는 이것을 링크하지 않는다.

include(FetchContent)

FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.16.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(Catch2)

add_executable(sangtachi_tests
    adapter_loop_test.cpp
    args_test.cpp
    crt_report_guard.cpp
    console_test.cpp
    counters_test.cpp
    hash_test.cpp
    log_test.cpp
    loop_test.cpp
    record_test.cpp
    timer_test.cpp
    spsc_ring_test.cpp
    control/channel_test.cpp
    control/exchange_test.cpp
    control/http_test.cpp
    control/json_test.cpp
    control/lobby_test.cpp
    control/ops_test.cpp
    control/runner_test.cpp
    network/endpoint_test.cpp
    network/stun_test.cpp
    network/stun_client_test.cpp
    network/udp_socket_test.cpp
    network/wsa_test.cpp
    peer/candidates_test.cpp
    peer/epoch_test.cpp
    peer/session_test.cpp
    platform/console_ctrl_test.cpp
    platform/interfaces_test.cpp
    platform/random_test.cpp
    platform/record_file_test.cpp
    platform/resolve_test.cpp
    platform/stdout_test.cpp
    platform/tcp_test.cpp
    platform/wait_test.cpp
    tunnel/packet_test.cpp
    tunnel/replay_test.cpp
    tunnel/data_sender_test.cpp
    tunnel/tunnel_test.cpp
    testbuild/instrument_test.cpp
    # 시험 빌드 장치는 core 에 없다. 단위 시험을 위해 여기서 컴파일한다 (ADR 0023)
    ${PROJECT_SOURCE_DIR}/client/src/testbuild/instrument.cpp
)

target_link_libraries(sangtachi_tests
    PRIVATE sangtachi_core
    PRIVATE sangtachi_warnings
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
# `[slow]` 는 시간을 재는 시험이다(벽시계로 간격이나 상한을 본다). 다른 시험 프로세스와 CPU 를 다투면 재는 값이 그 다툼을 담는다.
# 실측으로 겪었다. 시험이 늘자 -j 16 에서 부하 아래 타이머 시험의 최악 간격이 750ms 가 됐고
# 혼자 돌리면 219ms 였다. 그래서 혼자 돈다.
catch_discover_tests(sangtachi_tests TEST_SPEC "~[slow]" PROPERTIES TIMEOUT 30)
catch_discover_tests(sangtachi_tests TEST_SPEC "[slow]" PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)
