# sangtachi_core 의 원본 목록.
#
# 모듈 묶음마다 한 줄씩 늘린다. 디렉터리 구성은 architecture.md 10 목표 구조가 갖는다.

# 중립 원본. OS 헤더를 포함하지 않는다.
target_sources(sangtachi_core PRIVATE
    args.cpp
    console.cpp
    counters.cpp
    log.cpp
    loop.cpp
    timer.cpp
    network/endpoint.cpp
)

# 플랫폼 원본. OS 헤더(windows.h, winsock2.h, bcrypt.h ...)를 포함하는 원본은 전부
# 이 아래에 있다. 근거는 decisions/0010 이다.
if(WIN32)
    target_sources(sangtachi_core PRIVATE
        platform/win32/hash.cpp
        platform/win32/udp_socket.cpp
        platform/win32/wait.cpp
        platform/win32/wsa.cpp
    )
endif()
