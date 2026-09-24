# hamychi_core 의 원본 목록.
#
# 모듈 묶음마다 한 줄씩 늘린다. 디렉터리 구성은 architecture.md 10 목표 구조가 갖는다.

target_sources(hamychi_core PRIVATE
    args.cpp
    console.cpp
    counters.cpp
    hash.cpp
    log.cpp
    loop.cpp
    timer.cpp
    network/endpoint.cpp
    network/udp_socket.cpp
    network/wsa.cpp
)
