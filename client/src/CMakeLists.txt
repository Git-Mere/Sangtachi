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
    record.cpp
    timer.cpp
    control/channel.cpp
    control/exchange.cpp
    control/http.cpp
    control/json.cpp
    control/lobby.cpp
    control/ops.cpp
    control/runner.cpp
    network/endpoint.cpp
    network/stun.cpp
    network/stun_client.cpp
    peer/candidates.cpp
    peer/epoch.cpp
    peer/session.cpp
    telemetry/telemetry.cpp
    telemetry/uploader.cpp
    tunnel/data_sender.cpp
    tunnel/packet.cpp
    tunnel/replay.cpp
    tunnel/roster_publisher.cpp
    tunnel/router.cpp
    tunnel/tunnel.cpp
    ui/gui_ui.cpp
    ui/view.cpp
)

# 플랫폼 원본. OS 헤더(windows.h, winsock2.h, bcrypt.h, unistd.h ...)를 포함하는 원본은 전부
# 이 아래에 있다. 근거는 decisions/0010 과 decisions/0035 다.
if(WIN32)
    target_sources(sangtachi_core PRIVATE
        platform/win32/console_ctrl.cpp
        platform/win32/hash.cpp
        platform/win32/interfaces.cpp
        platform/win32/random.cpp
        platform/win32/record_file.cpp
        platform/win32/resolve.cpp
        platform/win32/session_end.cpp
        platform/win32/stdout.cpp
        platform/win32/tcp.cpp
        platform/win32/tun.cpp
        platform/win32/udp_socket.cpp
        platform/win32/wait.cpp
        platform/win32/wsa.cpp
    )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # POSIX 공통과 Linux 전용으로 나눈다. macOS 를 넣을 때 다시 쓸 자리를 줄인다 (ADR 0035 결정 2).
    target_sources(sangtachi_core PRIVATE
        platform/posix/console_ctrl.cpp
        platform/posix/hash.cpp
        platform/posix/random.cpp
        platform/posix/record_file.cpp
        platform/posix/resolve.cpp
        platform/posix/session_end.cpp
        platform/posix/stdout.cpp
        platform/posix/tcp.cpp
        platform/posix/udp_socket.cpp
        platform/posix/wait.cpp
        platform/posix/wsa.cpp
        platform/linux/clock.cpp
        platform/linux/exe_path.cpp
        platform/linux/interfaces.cpp
        platform/linux/netlink.cpp
        platform/linux/tun.cpp
    )
endif()
