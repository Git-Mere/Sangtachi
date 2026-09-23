#pragma once

// IPv4 엔드포인트 표현 (roadmap.md Phase 1: 파싱, 비교, 출력).
//
// 이 타입은 표현이고 정책이 아니다. 포트 1~65535 같은 인자 규칙은 기동 인자 쪽이
// 판정한다 (architecture.md 3.5 기동 입력). 여기서는 0 도 담는다. 소켓을 포트 0 으로
// bind 하기 때문이다 (protocol.md 6장 소켓 소유권).

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace hamychi::network {

// 점 십진 IPv4 리터럴을 호스트 바이트 순서 32비트로 바꾼다.
//
// 표준 라이브러리의 변환 함수를 쓰지 않는다. inet_addr 은 "1.2.3" 같은 3옥텟 표기와
// "0x7f.1" 같은 16진수, "010" 같은 8진수를 받아들인다. 그것들이 통과하면 사용자가 친
// 것과 우리가 쓰는 주소가 달라진다. 받는 것을 목록으로 정한다.
//
// 받는 것: 점 4개로 나뉜 옥텟 넷. 옥텟은 ASCII 숫자 1~3자이고 값이 0~255 다.
// "0" 이 아닌데 0 으로 시작하는 옥텟은 거부한다. 8진수로 읽는 구현이 있어서다.
[[nodiscard]] std::optional<std::uint32_t> parse_ipv4(std::string_view text);

// 호스트 바이트 순서 32비트를 점 십진으로 적는다.
[[nodiscard]] std::string format_ipv4(std::uint32_t address);

// ASCII 숫자만으로 된 10진 포트. 0~65535 를 받는다.
//
// 옥텟과 같은 규칙으로 여러 자리가 0 으로 시작하면 거부한다. 인자 규칙인 1~65535 는
// 여기가 아니라 기동 인자 쪽이 판정한다.
[[nodiscard]] std::optional<std::uint16_t> parse_port(std::string_view text);

class Endpoint {
public:
    constexpr Endpoint() noexcept = default;
    constexpr Endpoint(std::uint32_t address, std::uint16_t port) noexcept
        : address_(address), port_(port) {}

    // "<점 십진 IPv4>:<10진 포트>" 하나를 읽는다. 앞뒤 공백을 지워 주지 않는다.
    //
    // IPv6 표기는 받지 않는다. 이 프로젝트의 범위 밖이다 (spec.md 초기 범위 밖).
    // 거르는 자리는 콜론 개수가 아니라 주소와 포트 각각의 형식 검사다.
    [[nodiscard]] static std::optional<Endpoint> parse(std::string_view text);

    [[nodiscard]] constexpr std::uint32_t address() const noexcept { return address_; }
    [[nodiscard]] constexpr std::uint16_t port() const noexcept { return port_; }

    // architecture.md 9장이 정한 `IPv4:port` 10진 표기.
    [[nodiscard]] std::string to_string() const;

    // 비교는 멤버 순서를 그대로 쓴다. 정렬 순서에 의미를 두지 않고, 연관 컨테이너에
    // 넣을 수 있게 하려는 것이다.
    friend constexpr bool operator==(const Endpoint&, const Endpoint&) noexcept = default;
    friend constexpr std::strong_ordering operator<=>(const Endpoint&,
                                                      const Endpoint&) noexcept = default;

private:
    std::uint32_t address_ = 0;
    std::uint16_t port_ = 0;
};

}  // namespace hamychi::network
