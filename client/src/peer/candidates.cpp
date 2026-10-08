#include "sangtachi/peer/candidates.hpp"

#include "sangtachi/control/ops.hpp"
#include "sangtachi/log.hpp"
#include "sangtachi/network/endpoint.hpp"
#include "sangtachi/platform/interfaces.hpp"
#include "sangtachi/protocol_constants.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace sangtachi::peer {
namespace {

constexpr std::uint32_t kLoopbackNetwork = 0x7F000000u;  // 127.0.0.0
constexpr std::uint32_t kLoopbackMask = 0xFF000000u;     // /8
constexpr std::uint32_t kApipaNetwork = 0xA9FE0000u;     // 169.254.0.0
constexpr std::uint32_t kApipaMask = 0xFFFF0000u;        // /16

constexpr bool is_loopback(std::uint32_t address) noexcept {
    return (address & kLoopbackMask) == kLoopbackNetwork;
}

constexpr bool is_apipa(std::uint32_t address) noexcept {
    return (address & kApipaMask) == kApipaNetwork;
}

// 10.1 활성 단계. **두 조건이 다 참이어야 한다.** 하나만 보면 DAD 가 끝나지 않은 주소나 끊긴
// 어댑터에 남은 주소가 들어온다 (ADR 0017 대안 7).
constexpr bool is_active(platform::OperStatus status, platform::DadState dad) noexcept {
    return status == platform::OperStatus::kUp && dad == platform::DadState::kPreferred;
}

// 10.1 제외 단계의 IfType 넷. 제외 목록이다. 여기 없는 IfType 은 포함한다.
constexpr bool is_excluded_if_type(std::uint32_t if_type) noexcept {
    return if_type == kIfTypeSoftwareLoopback || if_type == kIfTypePropVirtual ||
           if_type == kIfTypeTunnel || if_type == kIfTypePpp;
}

// 자기 서브넷 집합의 원소가 될 수 있는가 (10.1 자기 서브넷 집합의 둘째, 셋째 줄).
constexpr bool fits_own_subnet(std::uint32_t address, std::uint8_t prefix_length) noexcept {
    return !is_loopback(address) && prefix_length <= kMaxBroadcastPrefix;
}

void push_unique(std::vector<control::Candidate>& out, const control::Candidate& c) {
    for (const auto& kept : out) {
        if (kept.endpoint == c.endpoint) {
            return;  // 먼저 온 원소를 남긴다. kind 도 그 원소의 것이다
        }
    }
    out.push_back(c);
}

}  // namespace

std::uint32_t OwnSubnet::broadcast() const noexcept {
    // prefix 0 이면 마스크가 0 이다. 32 비트를 통째로 미는 시프트는 정의되지 않으므로 따로 둔다.
    const std::uint32_t mask = prefix_length == 0 ? 0u : (0xFFFFFFFFu << (32u - prefix_length));
    return address | ~mask;
}

OwnSubnets::OwnSubnets(std::vector<OwnSubnet> entries) {
    for (const auto& e : entries) {
        if (fits_own_subnet(e.address, e.prefix_length)) {
            entries_.push_back(e);
        }
    }
}

bool OwnSubnets::blocks_broadcast(std::uint32_t address) const noexcept {
    for (const auto& e : entries_) {
        if (e.broadcast() == address) {
            return true;
        }
    }
    return false;
}

InterfaceJudgement judge_interfaces(std::span<const platform::InterfaceInfo> interfaces,
                                    const std::optional<platform::InterfaceGuid>& own_wintun) {
    InterfaceJudgement out;
    std::vector<OwnSubnet> subnets;
    for (const auto& iface : interfaces) {
        // 자기 Wintun 은 IfType 과 무관하게 식별자로만 본다 (10.1). IfType 53 에 기대면 우리 어댑터가
        // 53 이 아닐 때 놓치고, 같은 53 인 남의 Wintun(예: Tailscale)과 가르지 못한다 (ADR 0017).
        const bool own = own_wintun.has_value() && iface.guid == *own_wintun;
        const bool excluded_interface = own || is_excluded_if_type(iface.if_type);
        for (const auto& addr : iface.addresses) {
            if (!is_active(iface.oper_status, addr.dad_state)) {
                continue;  // 어디에도 쓰지 않는다
            }
            // 자기 서브넷 집합은 활성 단계만 본다. 제외 단계와 무관하다 (10.1, ADR 0017 대안 8).
            if (fits_own_subnet(addr.address, addr.prefix_length)) {
                subnets.push_back(OwnSubnet{addr.address, addr.prefix_length});
            }
            if (excluded_interface || is_loopback(addr.address) || is_apipa(addr.address)) {
                continue;
            }
            // prefix 는 후보 판정에 쓰지 않는다 (10.1 케이스 22).
            out.candidates.push_back(LocalAddress{addr.address, iface.if_index});
        }
    }
    out.own_subnets = OwnSubnets(std::move(subnets));
    return out;
}

std::optional<std::uint32_t> default_route_interface(
    std::span<const platform::Ipv4Route> routes) noexcept {
    std::optional<std::uint32_t> best_if;
    std::uint64_t best_sum = 0;
    for (const auto& r : routes) {
        if (r.destination != 0 || r.prefix_length != 0) {
            continue;  // 0.0.0.0/0 만
        }
        // 두 메트릭은 각각 32비트다. 합이 넘치지 않게 64비트로 더한다.
        const std::uint64_t sum = std::uint64_t{r.route_metric} + std::uint64_t{r.interface_metric};
        // 엄격히 작을 때만 바꾼다. 같으면 열거 순서가 앞인 것이 남는다.
        if (!best_if || sum < best_sum) {
            best_if = r.if_index;
            best_sum = sum;
        }
    }
    return best_if;
}

std::vector<control::Candidate> registration_list(std::span<const network::Endpoint> reflexive,
                                                  std::span<const LocalAddress> locals,
                                                  std::optional<std::uint32_t> default_if,
                                                  std::uint16_t local_port) {
    // 1~4 를 한 번에 한다. 정렬된 순서로 넣으면서 중복을 지운다. 5(자르기)는 맨 끝이다.
    // 자르기를 앞으로 옮기면 걸러질 중복이나 뒤로 갈 후보가 자리를 차지한다 (10.1 O4, O6).
    std::vector<control::Candidate> out;
    for (const auto& r : reflexive) {
        push_unique(out, control::Candidate{r, control::CandidateKind::kReflexive});
    }
    if (default_if) {
        for (const auto& l : locals) {
            if (l.if_index == *default_if) {
                push_unique(out, control::Candidate{network::Endpoint(l.address, local_port),
                                                    control::CandidateKind::kLocal});
            }
        }
    }
    for (const auto& l : locals) {
        if (default_if && l.if_index == *default_if) {
            continue;  // 2번에서 넣었다
        }
        push_unique(out, control::Candidate{network::Endpoint(l.address, local_port),
                                            control::CandidateKind::kLocal});
    }
    if (out.size() > protocol::kMaxCandidates) {
        out.resize(protocol::kMaxCandidates);
    }
    return out;
}

Collection collect(std::span<const network::Endpoint> reflexive, std::uint16_t local_port,
                   const platform::InterfaceQuery& interfaces, const platform::RouteQuery& routes,
                   const std::optional<platform::InterfaceGuid>& own_wintun,
                   const OwnSubnets& previous) {
    Collection out;
    std::vector<LocalAddress> locals;
    if (interfaces.ok) {
        auto judged = judge_interfaces(interfaces.interfaces, own_wintun);
        locals = std::move(judged.candidates);
        // 성공이면 새 집합으로 바꾼다. 활성 주소가 없으면 빈 집합이다 (10.1 O10).
        out.own_subnets = std::move(judged.own_subnets);
    } else {
        // 실패를 빈 열거로 다루지 않는다. 이전 집합을 지우면 막던 브로드캐스트 출발지가 열린다 (O9).
        out.enumerate_failed = true;
        out.enumerate_error = interfaces.error;
        out.own_subnets = previous;
    }

    std::optional<std::uint32_t> default_if;
    if (routes.ok) {
        default_if = default_route_interface(routes.routes);
    } else {
        // 기본 경로가 없는 것과 같다 (10.1, O11). 등록은 멈추지 않는다.
        out.route_failed = true;
        out.route_error = routes.error;
    }

    out.registration = registration_list(reflexive, locals, default_if, local_port);
    return out;
}

CandidateSources platform_candidate_sources() {
    CandidateSources s;
    s.query_interfaces = [] { return platform::query_ipv4_interfaces(); };
    s.query_routes = [] { return platform::query_ipv4_default_routes(); };
    return s;
}

Collection collect(const CandidateSources& sources, std::span<const network::Endpoint> reflexive,
                   std::uint16_t local_port,
                   const std::optional<platform::InterfaceGuid>& own_wintun,
                   const OwnSubnets& previous) {
    // 빈 출처는 실패로 본다. 조회를 하지 못했다는 점에서 OS 실패와 같다.
    const platform::InterfaceQuery interfaces =
        sources.query_interfaces ? sources.query_interfaces() : platform::InterfaceQuery{};
    const platform::RouteQuery routes =
        sources.query_routes ? sources.query_routes() : platform::RouteQuery{};
    return collect(reflexive, local_port, interfaces, routes, own_wintun, previous);
}

void emit_collection_warnings(const Collection& collection) {
    if (collection.enumerate_failed) {
        const LogField fields[] = {field("code", std::uint64_t{collection.enumerate_error})};
        emit(LogLevel::Warn, "candidates.enumerate_failed", fields);
    }
    if (collection.route_failed) {
        const LogField fields[] = {field("code", std::uint64_t{collection.route_error})};
        emit(LogLevel::Warn, "candidates.route_failed", fields);
    }
}

}  // namespace sangtachi::peer
