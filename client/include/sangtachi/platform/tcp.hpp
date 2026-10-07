#pragma once

// 제어 평면용 TCP 연결 하나 (control_plane.md 8.2 시간 제한, concurrency.md 8장 `[control]`
// 스레드).
//
// **이 헤더는 OS 헤더를 들이지 않는다.** 구현만 `client/src/platform/win32/` 아래에 있다
// (decisions/0010). 요청 한 건을 주고받는 순서와 응답 경계 판정은 OS 와 무관하므로
// control/exchange.hpp 가 갖는다. 여기는 소켓 호출만 감싼다.
//
// ## 8.2 시간 제한이 여기서 어떻게 걸리나
//
// | 단계 | 8.2 의 값 | 이 이음새 |
// |------|-----------|-----------|
// | `connect` | 논블로킹 `connect` + `select`, 3초 | connect_tcp 가 논블로킹으로 connect 를 걸고 select 로 기다린다 |
// | 송신, 수신 각각 | `SO_SNDTIMEO` / `SO_RCVTIMEO`, 3초 | 연결이 서면 소켓을 블로킹으로 되돌리고 두 옵션을 건다 |
//
// **select 는 kAbortPollMs 조각으로 나눠 부르고 조각마다 중단 이벤트를 본다.** 8장 `[control]`
// 스레드의 종료 행이 "종료 이벤트를 보면 진행 중인 요청의 응답을 기다리지 않고 소켓을 닫고
// 반환한다" 이기 때문이다. select 는 이벤트 핸들을 기다리지 못하므로 조각 사이에서 본다. 조각을
// 나눠도 기다리는 총량은 부르는 쪽이 준 값 그대로다.
//
// 수신도 같은 방법으로 기다린다. wait_readable 이 읽을 것이 생길 때까지 조각으로 기다리고,
// 그 뒤의 recv_some 은 이미 도착한 바이트를 가져온다. `SO_RCVTIMEO` 는 그 recv 가 그래도
// 막힐 때의 뒷받침이다. **송신은 조각으로 나누지 않는다.** 요청은 머리와 본문 상한을 합쳐
// 6KB 남짓이라(control_plane.md 2.6 상수) 송신 버퍼에 한 번에 들어가고, 막히면 `SO_SNDTIMEO`
// 가 끝낸다. 그 동안은 중단 이벤트를 보지 못한다. 상한은 send 호출마다 io_timeout_ms 이고,
// 송신이 여러 호출로 나뉘면 호출 수만큼 쌓일 수 있다. 그 끝을 가두는 것은 concurrency.md 7장
// 종료의 join 상한과 `_exit` 다 (concurrency.md 8장 `[control]` 스레드의 종료 행).
// 송신이 호출별이고 수신이 단계 전체라는 것은 문서가 정했다 (control_plane.md 8.2 시간 제한).

#include "sangtachi/network/endpoint.hpp"
#include "sangtachi/platform/wait.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace sangtachi::platform {

// select 한 조각의 상한. 중단 이벤트를 보는 간격이다.
//
// 50ms 는 문서가 정했다 (concurrency.md 8장 `[control]` 스레드: "연결과 수신을 기다리는 동안 종료
// 이벤트를 50ms 마다 본다").
inline constexpr std::uint32_t kAbortPollMs = 50;

enum class TcpStatus : std::uint8_t {
    kOk,
    kTimedOut,  // 준 시간 안에 끝나지 않았다
    kAborted,   // 중단 이벤트가 신호됐다
    kClosed,    // 상대가 닫았다 (recv 가 0 을 돌려줬다)
    kRefused,   // connect 가 실패로 끝났다 (거부, 도달 불가). error 가 OS 코드다
    kError,     // 그 밖의 실패. error 가 OS 코드, failed_op 가 호출 이름이다
};

struct TcpResult {
    TcpStatus status = TcpStatus::kError;
    std::uint32_t error = 0;     // kRefused, kError 일 때 OS 오류 코드
    std::size_t bytes = 0;       // recv_some 이 kOk 일 때 받은 바이트 수
    std::string_view failed_op;  // kError 일 때 실패한 호출 이름
};

// 연결된 TCP 소켓 하나. 블로킹 모드이고 `SO_SNDTIMEO` / `SO_RCVTIMEO` 가 걸려 있다.
//
// **소멸자가 소켓을 닫는다.** 연결 하나에 요청 하나이고 재사용하지 않는다
// (control_plane.md 3.3, 8.2). 이동만 된다.
class TcpStream {
public:
    TcpStream() = delete;
    ~TcpStream();

    TcpStream(const TcpStream&) = delete;
    TcpStream& operator=(const TcpStream&) = delete;
    TcpStream(TcpStream&& other) noexcept;
    TcpStream& operator=(TcpStream&&) = delete;

    // 전부 보낸다. 한 번의 send 가 `SO_SNDTIMEO` 를 넘기면 kTimedOut 이다.
    [[nodiscard]] TcpResult send_all(std::span<const std::byte> data) noexcept;

    // 읽을 것이 생기거나 상대가 닫을 때까지 기다린다. 그러면 kOk 다.
    // timeout_ms 를 넘기면 kTimedOut, abort 가 신호되면 kAborted 다. abort 는 널이어도 된다.
    [[nodiscard]] TcpResult wait_readable(std::uint32_t timeout_ms, WaitHandle abort) noexcept;

    // 받는다. 받은 바이트 수가 bytes 다. 상대가 닫았으면 kClosed 다.
    // 빈 버퍼는 받지 않고 kError 다. 0 바이트 recv 는 닫힘과 구별되지 않는다.
    [[nodiscard]] TcpResult recv_some(std::span<std::byte> buffer) noexcept;

    // 원시 소켓 핸들. 소유권을 넘기지 않는다.
    //
    // 시험이 소켓 옵션을 다시 읽으려고 쓴다 (network/udp_socket.hpp 의 같은 자리와 같은 이유).
    // `SO_RCVTIMEO` 와 `SO_SNDTIMEO` 는 동작으로 드러나지 않는다. 수신은 wait_readable 이 먼저
    // 막고, 송신은 요청이 작아 막히지 않는다. 지운 변이가 동작 시험을 그대로 통과했다.
    [[nodiscard]] std::uintptr_t native_handle() const noexcept { return handle_; }

private:
    friend struct TcpStreamOpener;
    explicit TcpStream(std::uintptr_t handle) noexcept : handle_(handle) {}

    std::uintptr_t handle_;
};

struct TcpConnect {
    std::optional<TcpStream> stream;  // result.status 가 kOk 일 때만 있다
    TcpResult result;
};

// 새 TCP 소켓을 열어 to 에 연결한다.
//
// connect_timeout_ms 안에 연결이 서지 않으면 kTimedOut, 상대가 거부하면 kRefused, abort 가
// 신호되면 kAborted 다. 연결이 서면 소켓을 블로킹으로 되돌리고 `SO_SNDTIMEO` 와
// `SO_RCVTIMEO` 를 io_timeout_ms 로 건다. **실패한 경로는 전부 소켓을 닫고 돌아온다.**
//
// Winsock 이 초기화된 뒤에 부른다 (network::WsaContext).
[[nodiscard]] TcpConnect connect_tcp(const network::Endpoint& to, std::uint32_t connect_timeout_ms,
                                     std::uint32_t io_timeout_ms, WaitHandle abort) noexcept;

}  // namespace sangtachi::platform
