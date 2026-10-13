#include "sangtachi/testbuild/instrument.hpp"

#include "sangtachi/hash.hpp"
#include "sangtachi/log.hpp"
#include "sangtachi/network/endpoint.hpp"
#include "sangtachi/tunnel/data_sender.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>

namespace sangtachi::testbuild {
namespace {

std::string vip_at(std::span<const std::byte> inner, std::size_t offset) {
    if (inner.size() < offset + 4) {
        return "-";
    }
    std::uint32_t v = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        v = (v << 8) | std::to_integer<std::uint32_t>(inner[offset + i]);
    }
    return network::format_ipv4(v);
}

void emit_data(std::string_view event, std::string_view peer_key, std::size_t peer_offset,
               std::span<const std::byte> inner) {
    const std::string peer = vip_at(inner, peer_offset);
    const std::string hash = sha256_hex(inner).value_or("-");
    const LogField fields[] = {
        field(peer_key, peer),
        field("len", static_cast<std::uint64_t>(inner.size())),
        field("sha256", hash),
    };
    emit(LogLevel::Info, event, fields);
}

}  // namespace

void emit_data_tx(std::span<const std::byte> inner) {
    emit_data("data.tx", "to", 16, inner);
}

void emit_data_rx(std::span<const std::byte> inner) {
    emit_data("data.rx", "from", 12, inner);
}

std::optional<std::vector<std::byte>> make_echo_reply(std::span<const std::byte> inner) {
    // 합성 패킷의 IHL 5 레이아웃만 되돌린다. 옵션이 있으면 표시의 오프셋도 체크섬 범위도 다르다.
    if (inner.size() <= tunnel::kMarkerOffset || (std::to_integer<std::uint8_t>(inner[0]) & 0x0Fu) != 5 ||
        inner[tunnel::kMarkerOffset] != tunnel::kMarkerRequest) {
        return std::nullopt;
    }
    std::vector<std::byte> reply(inner.begin(), inner.end());
    std::swap_ranges(reply.begin() + 12, reply.begin() + 16, reply.begin() + 16);
    reply[tunnel::kMarkerOffset] = tunnel::kMarkerEcho;
    tunnel::fill_ipv4_checksum(reply);
    return reply;
}

}  // namespace sangtachi::testbuild
