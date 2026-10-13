#pragma once

// 8.5 라우팅 표 (protocol.md 8.5 송신 측 검증).
//
// 6칸 고정 배열이다. 인덱스는 상대 가상 IP 의 마지막 옥텟이고 값은 세션이다. 조회는 두 단계다.
//
// | 단계 | 하는 일 |
// |:----:|---------|
// | 1 | 목적지가 `10.100.0.0/24` 안이고 마지막 옥텟이 1 이상 MAX_PEERS 이하인가 (`route_index`) |
// | 2 | 그 칸을 읽는다. 비어 있으면 경로 없음이다 |
//
// 인덱스 0 칸과 자신의 칸은 늘 비어 있다. 채우는 쪽(터널)이 자신의 가상 IP 로는 채우지 않는다.
// 소유는 `[loop]` 다 (concurrency.md 5장).

#include "sangtachi/protocol_constants.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace sangtachi::peer {
class Session;
}

namespace sangtachi::tunnel {

// 1단계. 칸 인덱스. 범위 밖이면 비어 있다.
[[nodiscard]] std::optional<std::size_t> route_index(std::uint32_t virtual_ip) noexcept;

class Router {
public:
    // 칸을 채운다. 1단계 밖이거나 칸이 차 있으면 아무것도 하지 않고 거짓이다(8.5 "비어 있지 않으면
    // 구현 결함이다").
    bool assign(std::uint32_t virtual_ip, peer::Session* session) noexcept;
    // 그 세션이 든 칸을 비운다.
    void release(const peer::Session* session) noexcept;
    // 두 단계 조회. 경로가 없으면 널.
    [[nodiscard]] peer::Session* lookup(std::uint32_t virtual_ip) const noexcept;
    // 비어 있지 않은 칸들. 칸 순서다.
    [[nodiscard]] const std::array<peer::Session*, protocol::kMaxPeers + 1>& slots() const noexcept {
        return slots_;
    }

private:
    std::array<peer::Session*, protocol::kMaxPeers + 1> slots_{};
};

}  // namespace sangtachi::tunnel
