#pragma once

// 기동 인자 파싱 (architecture.md 3.5 기동 입력).
//
// 이 파일은 형식만 판정한다. 값이 그 구간에서 필수인지는 보지 않는다. 필수 판정은
// architecture.md 3.5 표의 "필수" 열이 정하고, 그 검사는 제어 평면이 들어오는 Phase 3
// 에서 붙는다 (roadmap.md). Phase 1~2 는 역할이 없어도 기동한다.
//
// 형식 검사는 그 구간에서 값을 쓰지 않아도 한다. 미루면 Phase 1 에서 통과한 입력이
// Phase 3 에서야 거부되고, 그 사이 시험이 어느 형식으로 돌았는지 기록에서 읽을 수 없다.
//
// 옵션은 `--이름 값` 꼴만 받는다. `--이름=값` 은 알 수 없는 인자다. 한 가지 꼴만 받으면
// 시험이 볼 경우의 수가 반으로 준다.

#include "sangtachi/network/endpoint.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sangtachi {

// 제어 평면 기본 포트. control_plane.md 2.6 상수의 CONTROL_PORT 다.
inline constexpr std::uint16_t kControlPort = 8000;

enum class Role {
    None,    // 주지 않았다. Phase 1~2 는 이것으로 기동한다
    Host,
    Player,
};

// 이름 또는 IPv4 리터럴과 포트. 해석하지 않은 값이다.
//
// 이름 해석은 [control] 스레드와 기동 시 STUN 해석이 맡는다 (architecture.md 3.2.8,
// 3.5). 여기서는 적은 그대로 들고 있는다.
struct HostPort {
    std::string host;
    std::uint16_t port = 0;
};

// --rejoin 이 나르는 재참가 증명.
struct Rejoin {
    std::uint32_t peer_id = 0;
    std::string peer_token;
};

struct Args {
    Role role = Role::None;
    std::optional<HostPort> server;
    std::optional<std::string> room;   // control_plane.md 2.1 대로 대문자로 정규화한 값
    std::optional<Rejoin> rejoin;
    std::vector<HostPort> stun;        // 비어 있으면 architecture.md 3.5 의 기본 목록
    std::optional<network::Endpoint> peer;
};

// 실패 이유. 로그에 싣는 값이라 토큰이 바뀌면 시험도 같이 낡는다.
//
// 포트 관련 둘은 뜻이 다르다. MissingPort 는 포트 자리가 아예 없는 것이고
// (`--stun a.example`), BadPort 는 자리는 있는데 값이 틀린 것이다 (`--stun a.example:`,
// `a.example:0`, `a.example:65536`). 한 토큰에 둘을 담으면 로그만 보고 어느 쪽인지
// 알 수 없다.
enum class ArgError {
    None,
    UnknownOption,
    MissingValue,
    BadRole,
    ExtraPositional,
    BadRoom,
    BadRejoin,
    BadHost,
    BadPort,
    MissingPort,
};

// 로그와 시험이 쓰는 고정 토큰.
[[nodiscard]] std::string_view to_token(ArgError error) noexcept;

struct ParseResult {
    std::optional<Args> args;          // 성공일 때만 값이 있다
    ArgError error = ArgError::None;
    std::string offending;             // 문제가 된 인자 원문. 성공이면 비어 있다

    [[nodiscard]] bool ok() const noexcept { return args.has_value(); }
};

// 프로그램 이름을 뺀 인자열을 읽는다.
//
// 같은 인자를 두 번 주면 --stun 은 목록에 쌓이고 나머지는 마지막 값이 이긴다.
[[nodiscard]] ParseResult parse_args(std::span<const std::string_view> argv);

}  // namespace sangtachi
