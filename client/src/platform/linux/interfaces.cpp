#include "sangtachi/platform/interfaces.hpp"

#include "netlink.hpp"

#include <linux/if_addr.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <net/if_arp.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sangtachi::platform {
namespace {

// IANA ifType 로 옮긴다. 10.1 의 제외 형식(24, 53, 131, 23)에 드는 것과 그 밖을 가르는 것이 목적이다.
std::uint32_t to_if_type(std::uint16_t arphrd) noexcept {
    switch (arphrd) {
        case ARPHRD_LOOPBACK: return 24;
        case ARPHRD_ETHER:    return 6;
        case ARPHRD_PPP:      return 23;
        case ARPHRD_NONE:     return 53;
        case ARPHRD_TUNNEL:
        case ARPHRD_SIT:
        case ARPHRD_IPGRE:    return 131;
        default:              return 1;
    }
}

// IFLA_OPERSTATE 를 쓰지 않는다. lo 와 tun 은 늘 UNKNOWN 을 내서 쓰는 인터페이스가 Up 이 아니게 된다.
OperStatus to_oper_status(std::uint32_t flags) noexcept {
    return (flags & IFF_UP) != 0 && (flags & IFF_RUNNING) != 0 ? OperStatus::kUp : OperStatus::kDown;
}

// IPv4 에는 DAD 가 없어 보통 Preferred 다. 커널이 표시를 달았을 때만 옮긴다.
DadState to_dad_state(std::uint32_t flags) noexcept {
    if ((flags & IFA_F_DADFAILED) != 0) {
        return DadState::kDuplicate;
    }
    if ((flags & IFA_F_TENTATIVE) != 0) {
        return DadState::kTentative;
    }
    return DadState::kPreferred;
}

// 인터페이스 이름이 정체성이다. IFNAMSIZ 가 16 이라 0 으로 채우면 정확히 맞는다.
InterfaceGuid name_bytes(const std::string& name) noexcept {
    InterfaceGuid out{};
    std::copy_n(name.begin(), std::min(name.size(), out.size()), out.begin());
    return out;
}

}  // namespace

InterfaceQuery query_ipv4_interfaces() {
    InterfaceQuery out;
    std::vector<netlink::Link> links;
    if (const auto status = netlink::dump_links(links); !status.ok()) {
        out.error = status.error;
        return out;
    }
    std::vector<netlink::Address> addresses;
    if (const auto status = netlink::dump_ipv4_addresses(addresses); !status.ok()) {
        out.error = status.error;
        return out;
    }
    out.ok = true;
    for (const auto& link : links) {
        InterfaceInfo info;
        info.if_index = link.index;
        info.if_type = to_if_type(link.type);
        info.oper_status = to_oper_status(link.flags);
        info.guid = name_bytes(link.name);
        for (const auto& a : addresses) {
            if (a.index == link.index) {
                info.addresses.push_back(InterfaceAddress{a.address, a.prefix_length, to_dad_state(a.flags)});
            }
        }
        if (!info.addresses.empty()) {
            out.interfaces.push_back(std::move(info));
        }
    }
    return out;
}

RouteQuery query_ipv4_default_routes() {
    RouteQuery out;
    std::vector<netlink::Route> routes;
    if (const auto status = netlink::dump_ipv4_routes(routes); !status.ok()) {
        out.error = status.error;
        return out;
    }
    out.ok = true;
    for (const auto& r : routes) {
        // 정책 라우팅의 다른 표와 blackhole 같은 비유니캐스트 경로는 나가는 길이 아니다.
        if (r.table != RT_TABLE_MAIN || r.type != RTN_UNICAST || r.prefix_length != 0) {
            continue;
        }
        // Linux 에는 인터페이스 메트릭이 없다. 경로 메트릭만으로 비교된다.
        out.routes.push_back(Ipv4Route{0, 0, r.if_index, r.priority, 0});
    }
    return out;
}

std::string interface_id_text(const InterfaceGuid& id) {
    const auto end = std::find(id.begin(), id.end(), std::uint8_t{0});
    return std::string(id.begin(), end);
}

}  // namespace sangtachi::platform
