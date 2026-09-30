#include "sangtachi/platform/resolve.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <ws2tcpip.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>

namespace sangtachi::platform {
namespace {

// DNS 이름의 상한은 253자다. 종료 널까지 담을 자리를 둔다.
//
// 고정 버퍼를 쓰는 이유는 이 함수가 noexcept 이기 때문이다. std::string 을 만들면
// 할당 실패가 예외로 나오고 noexcept 함수에서 그것은 곧 종료다.
constexpr std::size_t kHostBufferBytes = 256;

// sockaddr_in 은 네트워크 바이트 순서다. Endpoint 는 호스트 순서로 들고 있다
// (network/udp_socket.cpp 의 from_sockaddr 과 같은 규칙이다).
std::uint32_t host_order_address(const sockaddr_in& addr) noexcept {
    return ::ntohl(addr.sin_addr.s_addr);
}

[[nodiscard]] constexpr bool is_decimal_digit(char c) noexcept {
    return c >= '0' && c <= '9';
}

[[nodiscard]] constexpr bool is_hex_digit(char c) noexcept {
    return is_decimal_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// 라벨 하나가 숫자 표기인가. 규칙과 근거는 resolve.hpp 가 갖는다.
[[nodiscard]] bool is_numeric_label(std::string_view label) noexcept {
    if (label.empty()) {
        return false;
    }
    if (label.size() >= 2 && label[0] == '0' && (label[1] == 'x' || label[1] == 'X')) {
        for (const char c : label.substr(2)) {
            if (!is_hex_digit(c)) {
                return false;
            }
        }
        return true;
    }
    for (const char c : label) {
        if (!is_decimal_digit(c)) {
            return false;
        }
    }
    return true;
}

}  // namespace

bool looks_like_numeric_host(std::string_view host) noexcept {
    if (host.empty()) {
        return false;
    }
    std::size_t start = 0;
    while (true) {
        const std::size_t dot = host.find('.', start);
        const std::string_view label =
            (dot == std::string_view::npos) ? host.substr(start) : host.substr(start, dot - start);
        if (!is_numeric_label(label)) {
            return false;
        }
        if (dot == std::string_view::npos) {
            return true;
        }
        start = dot + 1;
    }
}

std::optional<network::Endpoint> resolve_ipv4(std::string_view host, std::uint16_t port) noexcept {
    if (host.empty()) {
        return std::nullopt;
    }

    // IPv4 리터럴이면 이름 해석을 거치지 않는다. 판정은 network::parse_ipv4 하나가 한다.
    if (const auto literal = network::parse_ipv4(host)) {
        return network::Endpoint(*literal, port);
    }

    // 리터럴로 읽히지 않았는데 모양이 숫자다. 이름으로 묻지 않는다. 그대로 넘기면
    // 사용자가 친 것과 다른 주소를 쓰게 될 수 있다. 두 경로와 그중 무엇을 실측했는지는
    // resolve.hpp 가 갖는다.
    if (looks_like_numeric_host(host)) {
        return std::nullopt;
    }

    // 널이 박힌 입력은 거른다. 그대로 복사하면 널 앞까지만 이름으로 읽혀, 사용자가 친
    // 것과 우리가 묻는 이름이 달라진다.
    if (host.find('\0') != std::string_view::npos) {
        return std::nullopt;
    }
    if (host.size() >= kHostBufferBytes) {
        return std::nullopt;
    }

    std::array<char, kHostBufferBytes> name{};
    std::memcpy(name.data(), host.data(), host.size());
    name[host.size()] = '\0';

    addrinfo hints{};
    hints.ai_family = AF_INET;  // AAAA 는 묻지 않는다
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    addrinfo* results = nullptr;
    // 서비스는 넘기지 않는다. 포트는 인자로 받은 것을 그대로 싣는다.
    if (::getaddrinfo(name.data(), nullptr, &hints, &results) != 0) {
        return std::nullopt;
    }

    std::optional<network::Endpoint> endpoint;
    for (const addrinfo* it = results; it != nullptr; it = it->ai_next) {
        // 첫 IPv4 하나만 쓴다 (architecture.md 3.5 기동 입력).
        if (it->ai_family != AF_INET || it->ai_addr == nullptr) {
            continue;
        }
        if (it->ai_addrlen < sizeof(sockaddr_in)) {
            continue;
        }
        sockaddr_in addr{};
        std::memcpy(&addr, it->ai_addr, sizeof(addr));
        if (addr.sin_family != AF_INET) {
            continue;
        }
        endpoint = network::Endpoint(host_order_address(addr), port);
        break;
    }

    ::freeaddrinfo(results);
    return endpoint;
}

}  // namespace sangtachi::platform
