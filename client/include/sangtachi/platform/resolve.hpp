#pragma once

// 이름 해석 (architecture.md 3.5 기동 입력).
// 구현은 client/src/platform/win32/ 에 있다 (ADR 0010).

#include "sangtachi/network/endpoint.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

namespace sangtachi::platform {

// 이름 또는 IPv4 리터럴을 엔드포인트로 바꾼다. 첫 IPv4 하나만 쓴다.
//
// **[loop] 안에서 부르지 않는다.** 이름 해석(getaddrinfo)은 동기 호출이라 응답이 올
// 때까지 부른 스레드를 붙잡는다. 기동 시 [loop] 를 시작하기 전에 목록 전체를 한 번
// 해석한다 (architecture.md 3.5 기동 입력).
//
// 결과가 여럿이면 첫 IPv4 주소 하나를 쓰고 나머지는 쓰지 않는다. AAAA 는 묻지도 쓰지도
// 않는다. 터널이 IPv4 전용인 것과 같은 범위다 (protocol.md 1장 범위와 전제).
//
// 해석 실패는 값 없음이다. 그 자리의 처리(경고 한 줄과 목록에서 제외)는 부르는 쪽이
// 한다 (architecture.md 3.5 기동 입력).
//
// IPv4 리터럴은 이름 해석을 거치지 않는다. network::parse_ipv4 가 판정하므로 그 함수가
// 받는 것만 리터럴이다 (network/endpoint.hpp 의 허용 목록).
//
// **주소로 읽히는 모양은 이름으로 묻지 않는다.** "1.2.3" 이나 "0x7f.1" 은 우리
// parse_ipv4 가 거부한다. 그것을 그대로 `getaddrinfo` 에 넘기면 사용자가 친 것과 다른
// 주소를 쓰게 될 수 있고, 그 어긋남은 크래시 없이 조용히 틀린다. 그래서 리터럴로 읽히지
// 않은 입력이 아래 looks_like_numeric_host 에 걸리면 값 없음으로 끝낸다.
//
// **경로는 둘이고 하나만 실측했다.** (1) `getaddrinfo` 가 옛 `inet_addr` 표기를 스스로
// 해석하는 경우. 이 레포를 개발한 Windows 기계에서는 위 표기들이 전부 해석에 실패했다.
// 그래서 이 경로는 **이 기계에서 확인되지 않았고** OS 와 버전에 달린 것으로 둔다.
// (2) 그 입력이 이름으로 DNS 에 나가는 경우. NXDOMAIN 을 가로채는 망이나 검색 접미사를
// 붙이는 설정에서는 주소 모양의 입력이 엉뚱한 주소를 받아 온다. 이쪽은 우리가 확인할 수
// 없는 남의 설정이고, 그래서 막는 쪽을 고른다.
//
// **그 결과 이 판정은 위 (1) 이 없는 기계에서 눈에 보이지 않는다.** 막았을 때와 막지
// 않았을 때의 반환값이 같기 때문이다. 그래서 규칙을 지키는 것은 이 함수의 케이스 표이고
// (tests/platform/resolve_test.cpp), resolve_ipv4 쪽 단정은 거동을 못박을 뿐 기전을
// 가르지 못한다.
//
// 이름 경로는 Winsock 초기화를 요구한다. network::WsaContext 가 살아 있어야 한다.
// 없으면 실패로 보고한다. 리터럴 경로는 그것 없이도 된다.
//
// port 는 그대로 결과에 싣는다. 0 도 받는다. 1~65535 규칙은 기동 인자 쪽이 판정한다
// (network/endpoint.hpp 와 같은 경계다).
[[nodiscard]] std::optional<network::Endpoint> resolve_ipv4(std::string_view host,
                                                           std::uint16_t port) noexcept;

// 이 입력이 **숫자 표기**인가. 참이면 이름으로 묻지 않는다.
//
// 판정하는 것은 "주소인가" 가 아니라 "주소로 읽힐 수 있는 모양인가" 다. 점으로 나눈 라벨이
// 하나라도 숫자가 아니면 거짓이다. 라벨이 숫자인 것은 셋뿐이다.
//   - 10진: ASCII 숫자만 (`1`, `010`, `256`). 8진으로 읽는 구현이 있어 앞의 0 도 여기 든다
//   - 16진: `0x` 또는 `0X` 로 시작하고 나머지가 16진 숫자 (`0x7f`, `0X1A`, `0x`)
//   - 빈 라벨은 숫자가 아니다
//
// **정상 이름을 막지 않는 것이 이 규칙의 목표다.** 케이스 표는 이 함수의 시험이 갖는다
// (tests/platform/resolve_test.cpp). 요점 둘만 여기 적는다.
//   - `1drv.ms` 는 이름이다. `1drv` 가 10진수도 `0x` 표기도 아니다
//   - `cafe.example` 도 이름이다. 16진 **글자**지만 `0x` 가 없으므로 숫자가 아니다
//
// **한계.** `inet_addr` 의 문법을 그대로 흉내 내지 않는다. 끝에 점이 붙은 `1.2.3.4.` 같은
// 입력은 빈 라벨 때문에 이름으로 넘어가고, 그것을 `getaddrinfo` 가 숫자로 받아 줄 수
// 있다. 그 자리는 막지 못하는 것으로 두고, 지어낸 규칙으로 정상 이름을 막지 않는다.
[[nodiscard]] bool looks_like_numeric_host(std::string_view host) noexcept;

}  // namespace sangtachi::platform
