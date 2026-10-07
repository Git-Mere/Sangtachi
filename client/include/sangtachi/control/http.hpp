#pragma once

// 제어 평면의 HTTP 부분집합 (control_plane.md 3.3 HTTP 부분집합, 3.5 클라이언트가 하는 검사).
//
// 요청 바이트를 만들고 응답 바이트를 판정한다. **순수 코드다.** 소켓, 시간 제한, DNS 는
// [control] 스레드의 일이다 (control_plane.md 8.1, 8.2).
//
// ## 응답 판정 (3.5)
//
// 위에서부터 보고 처음 걸린 자리에서 멈춘다. 어느 것이든 3.5 의 "전송 오류" 이고 8.3 오류
// 분류와 재시도에서 일시 오류다. 오른쪽 칸은 control_plane.md 3.5 클라이언트가 하는 검사의 번호다.
// 3.5 의 6~8(`ok`, 연산별 형, 오류 코드)은 ops.hpp 가 본다.
//
// | # | 검사 | 어긋나면 | 3.5 |
// |---|------|----------|-----|
// | 1 | 빈 줄(CRLF CRLF)이 `kMaxHeaderBytes` 안에 있다 | `kHeaderTooLarge`, 덜 왔으면 `kHeaderIncomplete` | 2 |
// | 2 | 머리 안에 CRLF 가 아닌 CR 이나 LF 가 없다 | `kBadHeaderLine` | 3 "줄 끝은 CRLF 만" |
// | 3 | 상태 줄이 `HTTP/1.1 ` + 숫자 3자리 + (공백 또는 줄 끝) | `kBadStatusLine` | 1 |
// | 4 | 헤더 줄이 `이름:값` 이고 접기(공백·탭으로 시작)가 아니며 이름 끝에 공백·탭이 없다 | `kBadHeaderLine` | 3 |
// | 5 | `Transfer-Encoding` 이 없다 | `kTransferEncoding` | 3 |
// | 6 | `Content-Length` 가 있다 | `kMissingContentLength` | 4 |
// | 7 | `Content-Length` 가 하나다 | `kDuplicateContentLength` | 4 |
// | 8 | 값이 (콜론 뒤 공백 하나를 뗀 뒤) `^[0-9]+$` | `kBadContentLength` | 4. 공백 처리는 3.3 헤더 줄 문법 |
// | 9 | 값이 `kMaxBodyBytes` 이하 | `kBodyTooLarge` | 4 |
// | 10 | 본문이 그 길이만큼 왔다 | `kBodyIncomplete` | 5 |
// | 11 | 본문이 JSON 값 하나다 (`CLIENT_JSON_MAX_DEPTH`, 중복 키 거부) | `kBodyNotJson` | 5 |
// | 12 | 그 값이 객체다 | `kBodyNotObject` | 5 |
//
// 헤더 이름은 대소문자를 무시한다(3.3). 그 밖의 헤더는 값을 보지 않는다. `Content-Length` 만큼
// 읽으면 멈추고 그 뒤에 더 온 바이트는 보지 않는다(3.5 "서버가 연결을 닫기를 기다리지 않는다").
//
// **상태 코드는 돌려주지 않는다.** 성공과 오류는 본문의 `ok` 가 가른다(3.5 의 6). 숫자를
// 노출하면 부르는 쪽이 그것으로 가르고 싶어진다. 형식만 보고 버린다.
//
// 표에 없는 순서는 이 코드가 정했다. 1 과 2 가 3 보다 앞인 것은 줄을 나누기 전에 경계를 정해야
// 하기 때문이다. 한 응답이 두 검사에 걸리면 앞의 것이 이유가 된다. 어느 쪽이든 전송 오류다.

#include "sangtachi/control/json.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace sangtachi::control::http {

// 연산 다섯 (control_plane.md 4장).
enum class Op : std::uint8_t {
    kCreateRoom,
    kJoinRoom,
    kRegisterCandidate,
    kGetPeers,
    kHostReport,
};

// 경로와 로그(architecture.md 9장 control.result 의 op)에 쓰는 이름. 4장의 표기 그대로다.
[[nodiscard]] std::string_view op_name(Op op) noexcept;

// 3.3 요청 바이트.
//
//   POST /v1/<op> HTTP/1.1\r\n
//   Host: <host>\r\n
//   Content-Type: application/json\r\n
//   Content-Length: <본문 바이트 수>\r\n
//   Connection: close\r\n
//   \r\n
//   <본문>
//
// 거부하면 nullopt 다. host 가 비었거나 kMaxHostHeaderBytes 를 넘거나 [!-~] 밖의 바이트(공백,
// CR, LF, 제어 바이트, 0x80 이상)를 담으면 헤더 주입이 되므로 거부한다. 본문이 kMaxBodyBytes 를
// 넘으면 서버가 413 으로 답할 것을 보내지 않는다. 본문의 내용은 보지 않는다. ops 의 본문 생성기가
// 만든다. host 에는 host_header 가 만든 값을 넣는다 (3.3 `Host`, `Content-Type` 행).
// Host 헤더 값의 상한. args.cpp 가 받는 이름 253 자 + `:` + 포트 5자리.
inline constexpr std::size_t kMaxHostHeaderBytes = 253 + 1 + 5;

// 3.3 `Host`, `Content-Type` 행의 `<서버 주소>`. `--server` 의 이름 또는 IPv4 와 포트를 `:` 로
// 잇는다 (architecture.md 3.5 기동 입력).
//
// 거부하면 nullopt 다. host 가 비었거나 253 바이트를 넘거나 [!-~] 밖의 바이트나 `:` 를 담으면,
// 또는 port 가 0 이면 거부한다. 이름과 IPv4 의 문법은 args.cpp 가 기동 시 이미 봤다. 여기서는
// 헤더 한 줄을 깨뜨리지 않는 것만 본다.
[[nodiscard]] std::optional<std::string> host_header(std::string_view host, std::uint16_t port);

[[nodiscard]] std::optional<std::string> build_request(std::string_view host, Op op,
                                                       std::string_view body);

enum class ResponseError : std::uint8_t {
    kNone = 0,
    kHeaderIncomplete,        // 빈 줄이 아직 오지 않았다
    kHeaderTooLarge,          // kMaxHeaderBytes 안에 빈 줄이 없다
    kBadStatusLine,
    kBadHeaderLine,
    kTransferEncoding,
    kMissingContentLength,
    kDuplicateContentLength,
    kBadContentLength,
    kBodyTooLarge,
    kBodyIncomplete,
    kBodyNotJson,
    kBodyNotObject,
};

// 로그와 시험이 쓰는 고정 토큰. 입력 내용을 싣지 않는다.
[[nodiscard]] std::string_view to_token(ResponseError error) noexcept;

// 읽기를 어디서 멈출지 (control_plane.md 3.3 연결 행). 서버는 응답 뒤에 닫지만, 클라이언트는
// EOF 를 기다리지 않고 `Content-Length` 까지 읽으면 멈춘다.
enum class FrameState : std::uint8_t {
    kNeedMore,  // 더 읽는다. needed 가 0 이 아니면 그만큼 더 있으면 끝난다
    kComplete,  // 다 왔다. parse_response 를 부른다
    kInvalid,   // 더 읽어도 소용없다. error 를 본다
};

struct Frame {
    FrameState state = FrameState::kNeedMore;
    ResponseError error = ResponseError::kNone;  // kInvalid 일 때만 뜻이 있다
    std::size_t head_size = 0;  // 머리가 다 왔을 때 빈 줄까지의 바이트 수
    std::size_t body_size = 0;  // 머리가 다 왔을 때 Content-Length
    std::size_t needed = 0;     // kNeedMore 이고 머리가 다 왔을 때 더 받아야 하는 바이트 수
};

// 지금까지 받은 바이트로 응답의 경계를 판정한다. 위 표의 1~9 를 본다.
//
// 머리가 다 오기 전에는 kNeedMore 이고 needed 는 0 이다. 그때 부르는 쪽은 한 번에 최대
// kMaxHeaderBytes - received.size() 바이트까지 더 읽어도 된다. 머리가 다 오면 needed 가
// 정해지고, 부르는 쪽은 그 이상을 읽지 않는다.
[[nodiscard]] Frame frame_response(std::string_view received);

struct Response {
    ResponseError error = ResponseError::kNone;
    std::optional<json::Value> body;  // 성공일 때만 있고 늘 객체다

    [[nodiscard]] bool ok() const noexcept { return error == ResponseError::kNone; }
};

// 받은 바이트 전체를 판정한다. 위 표의 1~12 를 본다.
//
// 상대가 닫아 더 받을 수 없을 때도 이것을 부른다. 그때 덜 온 머리는 kHeaderIncomplete,
// 덜 온 본문은 kBodyIncomplete 다.
[[nodiscard]] Response parse_response(std::string_view received);

}  // namespace sangtachi::control::http
