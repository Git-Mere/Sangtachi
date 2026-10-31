#include "netlink.hpp"

#include <arpa/inet.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <utility>
#include <vector>

namespace sangtachi::platform::netlink {
namespace {

// 덤프 응답 한 묶음의 상한보다 넉넉하다. 커널은 한 묶음을 페이지 크기 안팎으로 끊어 보낸다.
constexpr std::size_t kReceiveBuffer = 65536;

// 메시지와 속성은 4바이트 경계로 놓인다 (NLMSG_ALIGN, RTA_ALIGN). 매크로를 쓰지 않는 이유는 그 비교식이
// 부호 다른 정수를 섞어 -Werror 아래 경고가 되기 때문이다.
constexpr std::size_t align4(std::size_t size) noexcept {
    return (size + 3u) & ~std::size_t{3u};
}

constexpr std::size_t kHeaderSize = align4(sizeof(nlmsghdr));
constexpr std::size_t kAttributeHeader = align4(sizeof(rtattr));

struct SocketCloser {
    int fd = -1;
    ~SocketCloser() {
        if (fd >= 0) {
            ::close(fd);
        }
    }
};

// 요청 메시지 하나. 이 원본의 요청은 고정 구조체 하나와 속성 몇 개라 작은 버퍼로 충분하다.
class Request {
public:
    Request(std::uint16_t type, std::uint16_t flags) noexcept {
        nlmsghdr header{};
        header.nlmsg_type = type;
        header.nlmsg_flags = flags;
        std::memcpy(bytes_, &header, sizeof(header));
        length_ = kHeaderSize;
    }

    template <typename T>
    void body(const T& value) noexcept {
        std::memcpy(bytes_ + length_, &value, sizeof(T));
        length_ = align4(length_ + sizeof(T));
    }

    void attribute(std::uint16_t type, std::uint32_t value) noexcept {
        rtattr header{};
        header.rta_len = static_cast<unsigned short>(kAttributeHeader + sizeof(value));
        header.rta_type = type;
        std::memcpy(bytes_ + length_, &header, sizeof(header));
        std::memcpy(bytes_ + length_ + kAttributeHeader, &value, sizeof(value));
        length_ = align4(length_ + kAttributeHeader + sizeof(value));
    }

    // 길이와 순번을 채운 메시지.
    std::pair<const unsigned char*, std::size_t> finish(std::uint32_t sequence) noexcept {
        nlmsghdr header{};
        std::memcpy(&header, bytes_, sizeof(header));
        header.nlmsg_len = static_cast<std::uint32_t>(length_);
        header.nlmsg_seq = sequence;
        std::memcpy(bytes_, &header, sizeof(header));
        return {bytes_, length_};
    }

private:
    alignas(nlmsghdr) unsigned char bytes_[128]{};
    std::size_t length_ = 0;
};

// 요청을 보내고 응답이 끝날 때까지 읽는다. 덤프는 NLMSG_DONE, 확인 요청은 NLMSG_ERROR 가 끝이다.
// 그 사이의 메시지마다 on_message(type, 본문, 본문 길이) 를 부른다.
template <typename OnMessage>
Status exchange(Request& request, std::string_view op, OnMessage on_message) {
    SocketCloser socket{::socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE)};
    if (socket.fd < 0) {
        return Status{op, static_cast<std::uint32_t>(errno)};
    }
    // 요청마다 소켓이 따로라 순번이 겹칠 상대가 없다.
    constexpr std::uint32_t sequence = 1;
    const auto [message, size] = request.finish(sequence);
    sockaddr_nl kernel{};
    kernel.nl_family = AF_NETLINK;
    for (;;) {
        const ssize_t sent =
            ::sendto(socket.fd, message, size, 0, reinterpret_cast<const sockaddr*>(&kernel), sizeof(kernel));
        if (sent >= 0) {
            break;
        }
        if (errno != EINTR) {
            return Status{op, static_cast<std::uint32_t>(errno)};
        }
    }

    std::vector<unsigned char> buffer(kReceiveBuffer);
    for (;;) {
        const ssize_t got = ::recv(socket.fd, buffer.data(), buffer.size(), 0);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            return Status{op, static_cast<std::uint32_t>(errno)};
        }
        const auto total = static_cast<std::size_t>(got);
        std::size_t offset = 0;
        while (offset + sizeof(nlmsghdr) <= total) {
            nlmsghdr header{};
            std::memcpy(&header, buffer.data() + offset, sizeof(header));
            if (header.nlmsg_len < sizeof(header) || offset + header.nlmsg_len > total) {
                return Status{op, EPROTO};
            }
            const unsigned char* payload = buffer.data() + offset + kHeaderSize;
            const std::size_t payload_size = header.nlmsg_len > kHeaderSize ? header.nlmsg_len - kHeaderSize : 0;
            offset += align4(header.nlmsg_len);
            if (header.nlmsg_seq != sequence) {
                continue;
            }
            // 덤프 도중 표가 바뀌면 커널이 이 표시를 단다. 빠진 줄이 있을 수 있으므로 판정에 쓰지 않는다.
            if ((header.nlmsg_flags & NLM_F_DUMP_INTR) != 0) {
                return Status{op, EINTR};
            }
            if (header.nlmsg_type == NLMSG_DONE) {
                // 덤프가 중간에 실패하면 커널이 DONE 본문에 음의 오류 값을 싣는다.
                int done_error = 0;
                if (payload_size >= sizeof(done_error)) {
                    std::memcpy(&done_error, payload, sizeof(done_error));
                }
                if (done_error < 0) {
                    return Status{op, static_cast<std::uint32_t>(-done_error)};
                }
                return {};
            }
            if (header.nlmsg_type == NLMSG_ERROR) {
                nlmsgerr error{};
                if (payload_size < sizeof(error)) {
                    return Status{op, EPROTO};
                }
                std::memcpy(&error, payload, sizeof(error));
                if (error.error == 0) {
                    return {};
                }
                return Status{op, static_cast<std::uint32_t>(-error.error)};
            }
            on_message(header.nlmsg_type, payload, payload_size);
        }
    }
}

// 고정 구조체 뒤의 속성마다 on_attribute(type, 값, 값 길이) 를 부른다.
template <typename OnAttribute>
void for_each_attribute(const unsigned char* data, std::size_t size, std::size_t fixed, OnAttribute on_attribute) {
    std::size_t offset = align4(fixed);
    while (offset + sizeof(rtattr) <= size) {
        rtattr header{};
        std::memcpy(&header, data + offset, sizeof(header));
        if (header.rta_len < sizeof(header) || offset + header.rta_len > size) {
            return;
        }
        on_attribute(header.rta_type, data + offset + kAttributeHeader, header.rta_len - kAttributeHeader);
        offset += align4(header.rta_len);
    }
}

std::uint32_t read_u32(const unsigned char* data, std::size_t size) noexcept {
    std::uint32_t value = 0;
    if (size >= sizeof(value)) {
        std::memcpy(&value, data, sizeof(value));
    }
    return value;
}

// 주소 속성은 네트워크 바이트 순서다.
std::uint32_t read_ipv4(const unsigned char* data, std::size_t size) noexcept {
    return ntohl(read_u32(data, size));
}

Status change_address(std::uint16_t type, std::uint16_t flags, std::string_view op, std::uint32_t if_index,
                      std::uint32_t address, std::uint8_t prefix_length) {
    Request request(type, static_cast<std::uint16_t>(NLM_F_REQUEST | NLM_F_ACK | flags));
    ifaddrmsg body{};
    body.ifa_family = AF_INET;
    body.ifa_prefixlen = prefix_length;
    body.ifa_scope = RT_SCOPE_UNIVERSE;
    body.ifa_index = if_index;
    request.body(body);
    request.attribute(IFA_LOCAL, htonl(address));
    request.attribute(IFA_ADDRESS, htonl(address));
    return exchange(request, op, [](std::uint16_t, const unsigned char*, std::size_t) {});
}

// 만들 때와 지울 때 같은 값을 준다. 지울 때 커널은 0 이 아닌 칸만 맞춰 보므로 protocol 이 RTPROT_BOOT 라
// 커널이 만든 같은 접두사의 경로(RTPROT_KERNEL)는 지우지 않는다.
Status change_route(std::uint16_t type, std::uint16_t flags, std::string_view op, std::uint32_t if_index,
                    std::uint32_t destination, std::uint8_t prefix_length) {
    Request request(type, static_cast<std::uint16_t>(NLM_F_REQUEST | NLM_F_ACK | flags));
    rtmsg body{};
    body.rtm_family = AF_INET;
    body.rtm_dst_len = prefix_length;
    body.rtm_table = RT_TABLE_MAIN;
    body.rtm_protocol = RTPROT_BOOT;
    body.rtm_scope = RT_SCOPE_LINK;
    body.rtm_type = RTN_UNICAST;
    request.body(body);
    request.attribute(RTA_DST, htonl(destination));
    request.attribute(RTA_OIF, if_index);
    return exchange(request, op, [](std::uint16_t, const unsigned char*, std::size_t) {});
}

}  // namespace

Status dump_links(std::vector<Link>& out) {
    Request request(RTM_GETLINK, NLM_F_REQUEST | NLM_F_DUMP);
    request.body(ifinfomsg{});
    return exchange(request, "RTM_GETLINK", [&out](std::uint16_t type, const unsigned char* data, std::size_t size) {
        ifinfomsg info{};
        if (type != RTM_NEWLINK || size < sizeof(info)) {
            return;
        }
        std::memcpy(&info, data, sizeof(info));
        Link link;
        link.index = static_cast<std::uint32_t>(info.ifi_index);
        link.type = info.ifi_type;
        link.flags = info.ifi_flags;
        for_each_attribute(data, size, sizeof(info),
                           [&link](std::uint16_t attribute, const unsigned char* value, std::size_t length) {
                               if (attribute == IFLA_IFNAME) {
                                   const auto* text = reinterpret_cast<const char*>(value);
                                   link.name.assign(text, ::strnlen(text, length));
                               }
                           });
        out.push_back(std::move(link));
    });
}

Status dump_ipv4_addresses(std::vector<Address>& out) {
    Request request(RTM_GETADDR, NLM_F_REQUEST | NLM_F_DUMP);
    ifaddrmsg filter{};
    filter.ifa_family = AF_INET;
    request.body(filter);
    return exchange(request, "RTM_GETADDR", [&out](std::uint16_t type, const unsigned char* data, std::size_t size) {
        ifaddrmsg info{};
        if (type != RTM_NEWADDR || size < sizeof(info)) {
            return;
        }
        std::memcpy(&info, data, sizeof(info));
        if (info.ifa_family != AF_INET) {
            return;
        }
        Address address;
        address.index = info.ifa_index;
        address.prefix_length = info.ifa_prefixlen;
        address.flags = info.ifa_flags;
        bool have_local = false;
        for_each_attribute(data, size, sizeof(info),
                           [&](std::uint16_t attribute, const unsigned char* value, std::size_t length) {
                               // 점대점 링크에서 IFA_ADDRESS 는 상대 주소다. 자기 주소는 IFA_LOCAL 이다.
                               if (attribute == IFA_LOCAL) {
                                   address.address = read_ipv4(value, length);
                                   have_local = true;
                               } else if (attribute == IFA_ADDRESS && !have_local) {
                                   address.address = read_ipv4(value, length);
                               } else if (attribute == IFA_FLAGS) {
                                   address.flags = read_u32(value, length);
                               }
                           });
        out.push_back(address);
    });
}

Status dump_ipv4_routes(std::vector<Route>& out) {
    Request request(RTM_GETROUTE, NLM_F_REQUEST | NLM_F_DUMP);
    rtmsg filter{};
    filter.rtm_family = AF_INET;
    request.body(filter);
    return exchange(request, "RTM_GETROUTE", [&out](std::uint16_t type, const unsigned char* data, std::size_t size) {
        rtmsg info{};
        if (type != RTM_NEWROUTE || size < sizeof(info)) {
            return;
        }
        std::memcpy(&info, data, sizeof(info));
        if (info.rtm_family != AF_INET) {
            return;
        }
        Route route;
        route.table = info.rtm_table;
        route.type = info.rtm_type;
        route.prefix_length = info.rtm_dst_len;
        for_each_attribute(data, size, sizeof(info),
                           [&route](std::uint16_t attribute, const unsigned char* value, std::size_t length) {
                               switch (attribute) {
                                   case RTA_TABLE:    route.table = read_u32(value, length); break;
                                   case RTA_DST:      route.destination = read_ipv4(value, length); break;
                                   case RTA_OIF:      route.if_index = read_u32(value, length); break;
                                   case RTA_PRIORITY: route.priority = read_u32(value, length); break;
                                   default:           break;
                               }
                           });
        out.push_back(route);
    });
}

Status add_ipv4_address(std::uint32_t if_index, std::uint32_t address, std::uint8_t prefix_length) {
    return change_address(RTM_NEWADDR, NLM_F_CREATE | NLM_F_EXCL, "RTM_NEWADDR", if_index, address, prefix_length);
}

Status delete_ipv4_address(std::uint32_t if_index, std::uint32_t address, std::uint8_t prefix_length) {
    return change_address(RTM_DELADDR, 0, "RTM_DELADDR", if_index, address, prefix_length);
}

Status add_link_route(std::uint32_t if_index, std::uint32_t destination, std::uint8_t prefix_length) {
    return change_route(RTM_NEWROUTE, NLM_F_CREATE | NLM_F_EXCL, "RTM_NEWROUTE", if_index, destination,
                        prefix_length);
}

Status delete_link_route(std::uint32_t if_index, std::uint32_t destination, std::uint8_t prefix_length) {
    return change_route(RTM_DELROUTE, 0, "RTM_DELROUTE", if_index, destination, prefix_length);
}

}  // namespace sangtachi::platform::netlink
