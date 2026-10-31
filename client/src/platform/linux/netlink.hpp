#pragma once

// 플랫폼 내부 헤더: rtnetlink 요청 (ADR 0035 결정 2의 인터페이스, 경로, TUN 주소).
//
// interfaces.cpp 의 조회와 tun.cpp 의 주소, 경로, 서브넷 검사가 같은 덤프를 쓴다. 값은 커널이 준 그대로
// 옮긴다. 주소는 호스트 바이트 순서다.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sangtachi::platform::netlink {

struct Link {
    std::uint32_t index = 0;
    std::uint16_t type = 0;   // ARPHRD_*
    std::uint32_t flags = 0;  // IFF_*
    std::string name;
};

struct Address {
    std::uint32_t index = 0;
    std::uint32_t address = 0;
    std::uint8_t prefix_length = 0;
    std::uint32_t flags = 0;  // IFA_F_*. IFA_FLAGS 가 있으면 그 값이다
};

struct Route {
    std::uint32_t table = 0;
    std::uint8_t type = 0;  // RTN_*
    std::uint32_t destination = 0;
    std::uint8_t prefix_length = 0;
    std::uint32_t if_index = 0;  // RTA_OIF. 없으면 0
    std::uint32_t priority = 0;  // RTA_PRIORITY. 없으면 0
};

// 실패하면 op 가 비지 않는다. op 는 실패한 호출 이름(정적 문자열)이고 error 는 errno 다.
struct Status {
    std::string_view op;
    std::uint32_t error = 0;
    [[nodiscard]] bool ok() const noexcept { return op.empty(); }
};

// 덤프 셋. 커널이 준 순서 그대로 담는다. 실패하면 목록은 부분일 수 있다. 쓰지 않는다.
[[nodiscard]] Status dump_links(std::vector<Link>& out);
[[nodiscard]] Status dump_ipv4_addresses(std::vector<Address>& out);
[[nodiscard]] Status dump_ipv4_routes(std::vector<Route>& out);  // 모든 경로 표

// 주소 하나를 붙이거나 지운다. 지울 때 없으면 EADDRNOTAVAIL 이다.
[[nodiscard]] Status add_ipv4_address(std::uint32_t if_index, std::uint32_t address, std::uint8_t prefix_length);
[[nodiscard]] Status delete_ipv4_address(std::uint32_t if_index, std::uint32_t address, std::uint8_t prefix_length);

// main 표의 scope link 경로 하나를 만들거나 지운다. 지울 때 없으면 ESRCH 다.
[[nodiscard]] Status add_link_route(std::uint32_t if_index, std::uint32_t destination, std::uint8_t prefix_length);
[[nodiscard]] Status delete_link_route(std::uint32_t if_index, std::uint32_t destination,
                                       std::uint8_t prefix_length);

}  // namespace sangtachi::platform::netlink
