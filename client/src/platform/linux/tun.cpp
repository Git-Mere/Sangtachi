#include "sangtachi/platform/tun.hpp"

#include "../posix/fd_handle.hpp"
#include "netlink.hpp"
#include "subnet_check.hpp"

#include <fcntl.h>
#include <linux/if_tun.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sangtachi::platform {
namespace {

// 가상 LAN 의 접두사 길이 (architecture.md 7장 가상 네트워크 설계의 /24).
constexpr std::uint8_t kPrefixLength = 24;

// IP 패킷 하나의 상한. 읽기 버퍼가 이보다 작으면 커널이 남은 바이트를 버린다.
constexpr std::size_t kMaxPacket = 65535;

// /proc/self/status 의 CapEff 에서 본다. CAP_NET_ADMIN 의 비트 번호다 (capability.h).
constexpr unsigned kCapNetAdmin = 12;

OsFailure os_failure(std::string_view op, int code) {
    return OsFailure{op, static_cast<std::uint32_t>(code)};
}

OsFailure from_status(const netlink::Status& status) {
    return OsFailure{status.op, status.error};
}

struct FdCloser {
    int fd = -1;
    ~FdCloser() {
        if (fd >= 0) {
            ::close(fd);
        }
    }
};

void fill_name(ifreq& request) noexcept {
    std::memset(&request, 0, sizeof(request));
    std::memcpy(request.ifr_name, adapter::kName.data(), adapter::kName.size());
}

}  // namespace

struct TunDevice::Api {
    int fd = -1;
    std::array<std::byte, kMaxPacket> buffer{};
};

bool is_elevated() noexcept {
    std::FILE* file = std::fopen("/proc/self/status", "r");
    if (file == nullptr) {
        return false;
    }
    char line[256];
    bool elevated = false;
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        if (std::strncmp(line, "CapEff:", 7) == 0) {
            const unsigned long long effective = std::strtoull(line + 7, nullptr, 16);
            elevated = ((effective >> kCapNetAdmin) & 1u) != 0;
            break;
        }
    }
    std::fclose(file);
    return elevated;
}

// 실행 파일 옆에 둘 파일이 없다. TUN 은 커널이 주고 서브넷 검사는 이 원본이 한다.
std::span<const std::string_view> adapter_files() noexcept {
    return {};
}

std::vector<std::string_view> missing_adapter_files() {
    return {};
}

SubnetCheck run_subnet_check(std::string_view target) {
    OsFailure failure;
    std::vector<subnet::Prefix> routes;
    {
        std::vector<netlink::Route> rows;
        if (const auto status = netlink::dump_ipv4_routes(rows); !status.ok()) {
            failure = from_status(status);
        } else {
            for (const auto& row : rows) {
                if (row.table == RT_TABLE_MAIN) {
                    routes.push_back(subnet::Prefix{row.destination, row.prefix_length});
                }
            }
        }
    }
    std::vector<std::uint32_t> addresses;
    {
        std::vector<netlink::Address> rows;
        if (const auto status = netlink::dump_ipv4_addresses(rows); !status.ok()) {
            if (failure.op.empty()) {
                failure = from_status(status);
            }
        } else {
            for (const auto& row : rows) {
                addresses.push_back(row.address);
            }
        }
    }
    return subnet::judge(target, routes, addresses, failure);
}

// 배포판마다 방화벽 도구가 달라 클라이언트가 만지지 않는다 (linux-prereq.md 5절).
FirewallCheck run_firewall_check() {
    FirewallCheck result;
    result.verdict = FirewallVerdict::kSkipped;
    return result;
}

// ---------------------------------------------------------------- TunDevice

TunDevice::Opened TunDevice::open(std::uint32_t mtu) {
    Opened opened;
    auto device = std::unique_ptr<TunDevice>(new TunDevice());
    device->api_ = std::make_unique<Api>();
    Api& api = *device->api_;

    api.fd = ::open("/dev/net/tun", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (api.fd < 0) {
        opened.failure = os_failure("open./dev/net/tun", errno);
        return opened;
    }
    // 비영구 장치다. fd 를 닫으면 커널이 장치와 그 주소, 경로를 지운다 (linux-prereq.md 3절).
    // IFF_TUN_EXCL 이 없으면 같은 이름의 영구 장치에 붙어 그 주소를 지우고, 닫은 뒤에도 장치가 남는다.
    ifreq request{};
    fill_name(request);
    // IFF_TUN_EXCL 은 0x8000 이라 short 칸에 담으면 부호 비트다. 커널은 같은 비트로 읽는다.
    request.ifr_flags = static_cast<short>(IFF_TUN | IFF_NO_PI | IFF_TUN_EXCL);
    if (::ioctl(api.fd, TUNSETIFF, &request) < 0) {
        opened.failure = os_failure("TUNSETIFF", errno);
        return opened;
    }

    // MTU, UP, 번호는 장치 fd 가 아니라 아무 소켓에 이름으로 묻는다.
    FdCloser control{::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0)};
    if (control.fd < 0) {
        opened.failure = os_failure("socket", errno);
        return opened;
    }
    fill_name(request);
    request.ifr_mtu = static_cast<int>(mtu);
    if (::ioctl(control.fd, SIOCSIFMTU, &request) < 0) {
        opened.failure = os_failure("SIOCSIFMTU", errno);
        return opened;
    }
    fill_name(request);
    if (::ioctl(control.fd, SIOCGIFFLAGS, &request) < 0) {
        opened.failure = os_failure("SIOCGIFFLAGS", errno);
        return opened;
    }
    // 링크가 내려가 있으면 주소를 붙여도 커널이 on-link 경로를 만들지 않는다.
    request.ifr_flags = static_cast<short>(request.ifr_flags | IFF_UP);
    if (::ioctl(control.fd, SIOCSIFFLAGS, &request) < 0) {
        opened.failure = os_failure("SIOCSIFFLAGS", errno);
        return opened;
    }
    fill_name(request);
    if (::ioctl(control.fd, SIOCGIFINDEX, &request) < 0) {
        opened.failure = os_failure("SIOCGIFINDEX", errno);
        return opened;
    }
    const auto if_index = static_cast<std::uint32_t>(request.ifr_ifindex);

    // 새로 만든 장치라 보통 없다. 남이 같은 이름으로 미리 둔 장치를 받았을 때를 위해 지운다.
    {
        std::vector<netlink::Address> addresses;
        if (const auto status = netlink::dump_ipv4_addresses(addresses); !status.ok()) {
            opened.failure = from_status(status);
            return opened;
        }
        for (const auto& a : addresses) {
            if (a.index != if_index) {
                continue;
            }
            const auto status = netlink::delete_ipv4_address(if_index, a.address, a.prefix_length);
            if (!status.ok() && status.error != EADDRNOTAVAIL) {
                opened.failure = from_status(status);
                return opened;
            }
        }
    }

    device->luid_ = if_index;
    std::memcpy(device->guid_.data(), adapter::kName.data(), adapter::kName.size());
    opened.device = std::move(device);
    return opened;
}

TunDevice::~TunDevice() {
    (void)close();
}

void* TunDevice::read_event() const noexcept {
    if (!api_ || api_->fd < 0) {
        return nullptr;
    }
    return posix::fd_read_handle(api_->fd);
}

adapter::Received TunDevice::receive() noexcept {
    if (!api_ || api_->fd < 0) {
        return adapter::Received{adapter::ReceiveStatus::kEof, {}, EBADF};
    }
    const ssize_t got = ::read(api_->fd, api_->buffer.data(), api_->buffer.size());
    if (got >= 0) {
        return adapter::Received{adapter::ReceiveStatus::kPacket,
                                 std::span<const std::byte>(api_->buffer.data(), static_cast<std::size_t>(got)), 0};
    }
    const int error = errno;
    switch (error) {
        case EAGAIN:
            return adapter::Received{adapter::ReceiveStatus::kEmpty, {}, 0};
        case EIO:
        case EBADFD:
        case ENODEV:
            return adapter::Received{adapter::ReceiveStatus::kEof, {}, static_cast<std::uint32_t>(error)};
        default:
            return adapter::Received{adapter::ReceiveStatus::kError, {}, static_cast<std::uint32_t>(error)};
    }
}

// 읽기 버퍼는 다음 receive 가 다시 쓴다. 돌려줄 것이 없다.
void TunDevice::release(std::span<const std::byte> /*packet*/) noexcept {}

adapter::InjectResult TunDevice::inject(std::span<const std::byte> packet) noexcept {
    if (!api_ || api_->fd < 0) {
        return adapter::InjectResult{adapter::InjectStatus::kError, EBADF};
    }
    if (packet.empty() || packet.size() > kMaxPacket) {
        return adapter::InjectResult{adapter::InjectStatus::kError, EINVAL};
    }
    if (::write(api_->fd, packet.data(), packet.size()) >= 0) {
        return adapter::InjectResult{adapter::InjectStatus::kOk, 0};
    }
    const int error = errno;
    return adapter::InjectResult{error == EAGAIN || error == ENOBUFS ? adapter::InjectStatus::kRingFull
                                                                     : adapter::InjectStatus::kError,
                                 static_cast<std::uint32_t>(error)};
}

// 다시 만들 세션이 없다. kCorrupt 를 내지 않으므로 루프가 부를 일도 없다.
bool TunDevice::restart_session() noexcept {
    return true;
}

AddressResult TunDevice::assign_address(std::uint32_t ip) {
    AddressResult result;
    if (const OsFailure cleared = clear_address(); !cleared.op.empty()) {
        result.failure = cleared;
        result.reason = "clear";
        return result;
    }
    const auto if_index = static_cast<std::uint32_t>(luid_);
    if (const auto status = netlink::add_ipv4_address(if_index, ip, kPrefixLength); !status.ok()) {
        result.failure = from_status(status);
        result.reason = "create";
        return result;
    }
    address_ = ip;

    // IPv4 에는 DAD 가 없어 기다릴 상태가 없다. 붙었는지 한 번 확인만 한다 (windows-prereq.md 9절의 조회 단계).
    {
        std::vector<netlink::Address> addresses;
        const auto status = netlink::dump_ipv4_addresses(addresses);
        bool present = false;
        for (const auto& a : addresses) {
            present = present || (a.index == if_index && a.address == ip);
        }
        if (!status.ok() || !present) {
            result.failure = status.ok() ? OsFailure{"RTM_GETADDR", ENOENT} : from_status(status);
            result.reason = "query";
            (void)clear_address();
            return result;
        }
    }

    // on-link 경로는 UP 인 링크에 /24 주소를 붙이면 커널이 만든다. 없을 때만 만든다 (windows-prereq.md 8절).
    const std::uint32_t destination = ip & subnet::prefix_mask(kPrefixLength);
    std::vector<netlink::Route> routes;
    if (const auto status = netlink::dump_ipv4_routes(routes); !status.ok()) {
        result.failure = from_status(status);
        result.reason = "route_query";
        (void)clear_address();
        return result;
    }
    bool present = false;
    for (const auto& r : routes) {
        present = present || (r.table == RT_TABLE_MAIN && r.if_index == if_index && r.destination == destination &&
                              r.prefix_length == kPrefixLength);
    }
    if (!present) {
        if (const auto status = netlink::add_link_route(if_index, destination, kPrefixLength); !status.ok()) {
            result.failure = from_status(status);
            result.reason = "route_create";
            (void)clear_address();
            return result;
        }
        route_created_ = true;
    }
    result.ok = true;
    result.route_created = route_created_;
    return result;
}

OsFailure TunDevice::clear_address() {
    if (address_ == 0) {
        return {};
    }
    const auto if_index = static_cast<std::uint32_t>(luid_);
    // 지우지 못한 것은 기억해 둔다. 다음 clear_address 나 close 가 다시 지운다.
    OsFailure failure;
    if (route_created_) {
        const auto status =
            netlink::delete_link_route(if_index, address_ & subnet::prefix_mask(kPrefixLength), kPrefixLength);
        // 없는 경로를 지우면 커널은 ESRCH 를 낸다.
        if (status.ok() || status.error == ESRCH || status.error == ENOENT) {
            route_created_ = false;
        } else {
            failure = from_status(status);
        }
    }
    const auto status = netlink::delete_ipv4_address(if_index, address_, kPrefixLength);
    if (status.ok() || status.error == EADDRNOTAVAIL || status.error == ENOENT) {
        if (!route_created_) {
            address_ = 0;
        }
    } else if (failure.op.empty()) {
        failure = from_status(status);
    }
    return failure;
}

OsFailure TunDevice::close() noexcept {
    OsFailure failure;
    if (!api_ || api_->fd < 0) {
        return failure;
    }
    // concurrency.md 7장 종료의 (6) 주소와 경로, 그다음 장치 순서다. fd 를 닫으면 장치가 사라진다.
    try {
        failure = clear_address();
    } catch (...) {
        failure = OsFailure{"clear_address", ENOMEM};
    }
    ::close(api_->fd);
    api_->fd = -1;
    return failure;
}

}  // namespace sangtachi::platform
