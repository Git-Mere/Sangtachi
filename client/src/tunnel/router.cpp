#include "sangtachi/tunnel/router.hpp"

namespace sangtachi::tunnel {
namespace {

constexpr std::uint32_t kVirtualNetwork = 0x0A640000u;  // 10.100.0.0/24 (control_plane.md 2장)
constexpr std::uint32_t kVirtualBroadcast = 0x0A6400FFu; // 10.100.0.255
constexpr std::uint32_t kMulticastNetwork = 0xE0000000u; // 224.0.0.0/4
constexpr std::uint32_t kMulticastMask = 0xF0000000u;
constexpr std::uint32_t kLimitedBroadcast = 0xFFFFFFFFu; // 255.255.255.255

}  // namespace

bool is_not_unicast(std::uint32_t destination) noexcept {
    return (destination & kMulticastMask) == kMulticastNetwork || destination == kLimitedBroadcast ||
           destination == kVirtualBroadcast;
}

std::optional<std::size_t> route_index(std::uint32_t virtual_ip) noexcept {
    if ((virtual_ip & 0xFFFFFF00u) != kVirtualNetwork) {
        return std::nullopt;
    }
    const std::size_t octet = virtual_ip & 0xFFu;
    if (octet < 1 || octet > protocol::kMaxPeers) {
        return std::nullopt;
    }
    return octet;
}

bool Router::assign(std::uint32_t virtual_ip, peer::Session* session) noexcept {
    const auto slot = route_index(virtual_ip);
    if (!slot || slots_[*slot] != nullptr) {
        return false;
    }
    slots_[*slot] = session;
    return true;
}

void Router::release(const peer::Session* session) noexcept {
    for (auto& slot : slots_) {
        if (slot == session) {
            slot = nullptr;
        }
    }
}

peer::Session* Router::lookup(std::uint32_t virtual_ip) const noexcept {
    const auto slot = route_index(virtual_ip);
    return slot ? slots_[*slot] : nullptr;
}

}  // namespace sangtachi::tunnel
