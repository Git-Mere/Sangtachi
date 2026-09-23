#include "hamychi/network/endpoint.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace hamychi::network {
namespace {

constexpr bool is_ascii_digit(char c) noexcept {
    return c >= '0' && c <= '9';
}

// 옥텟 하나. 숫자 1~3자, 값 0~255, 0 으로 시작하는 여러 자리를 거부한다.
std::optional<std::uint32_t> parse_octet(std::string_view text) {
    if (text.empty() || text.size() > 3) {
        return std::nullopt;
    }
    if (text.size() > 1 && text[0] == '0') {
        return std::nullopt;  // "010" 을 8진수로 읽는 구현이 있다
    }
    std::uint32_t value = 0;
    for (const char c : text) {
        if (!is_ascii_digit(c)) {
            return std::nullopt;
        }
        value = value * 10 + static_cast<std::uint32_t>(c - '0');
    }
    if (value > 255) {
        return std::nullopt;
    }
    return value;
}

}  // namespace

std::optional<std::uint32_t> parse_ipv4(std::string_view text) {
    std::uint32_t address = 0;
    std::size_t start = 0;

    for (int octet = 0; octet < 4; ++octet) {
        const bool last = (octet == 3);
        const std::size_t dot = text.find('.', start);

        // 마지막 옥텟 뒤에는 점이 없어야 하고, 그 앞 셋은 점으로 끝나야 한다.
        if (last == (dot != std::string_view::npos)) {
            return std::nullopt;
        }

        const std::size_t end = last ? text.size() : dot;
        const auto value = parse_octet(text.substr(start, end - start));
        if (!value) {
            return std::nullopt;
        }
        address = (address << 8) | *value;
        start = end + 1;
    }
    return address;
}

std::string format_ipv4(std::uint32_t address) {
    return std::to_string((address >> 24) & 0xFFu) + "." +
           std::to_string((address >> 16) & 0xFFu) + "." +
           std::to_string((address >> 8) & 0xFFu) + "." +
           std::to_string(address & 0xFFu);
}

std::optional<std::uint16_t> parse_port(std::string_view text) {
    if (text.empty() || text.size() > 5) {
        return std::nullopt;
    }
    if (text.size() > 1 && text[0] == '0') {
        return std::nullopt;  // 옥텟과 같은 규칙. 여러 자리는 0 으로 시작하지 않는다
    }
    std::uint32_t value = 0;
    for (const char c : text) {
        if (!is_ascii_digit(c)) {
            return std::nullopt;
        }
        value = value * 10 + static_cast<std::uint32_t>(c - '0');
    }
    if (value > 65535) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(value);
}

std::optional<Endpoint> Endpoint::parse(std::string_view text) {
    const std::size_t colon = text.find(':');
    if (colon == std::string_view::npos) {
        // 이 검사를 지워도 지금의 케이스는 하나도 떨어지지 않는다. npos + 1 이 0 으로
        // 감싸 돌아 포트 쪽이 문자열 전체를 받고, 점이 섞여 있어 parse_port 가 거부하기
        // 때문이다. 그래도 남긴다. 그 동작은 우연이고, parse_port 가 조금이라도 너그러워
        // 지면 주소 문자열이 포트로 읽힌다.
        return std::nullopt;
    }

    // 콜론 개수를 따로 세지 않는다. 첫 콜론 뒤에 콜론이 또 있으면 그것은 포트 부분에
    // 들어가고 parse_port 가 숫자가 아니라서 거부한다. 첫 콜론 앞에는 콜론이 있을 수
    // 없다. 그래서 개수 검사는 어떤 입력에서도 결과를 바꾸지 못한다. 변이 시험에서
    // 그 검사를 지워도 케이스가 하나도 떨어지지 않아 확인했다.
    const auto address = parse_ipv4(text.substr(0, colon));
    if (!address) {
        return std::nullopt;
    }
    const auto port = parse_port(text.substr(colon + 1));
    if (!port) {
        return std::nullopt;
    }
    return Endpoint(*address, *port);
}

std::string Endpoint::to_string() const {
    return format_ipv4(address_) + ":" + std::to_string(port_);
}

}  // namespace hamychi::network
