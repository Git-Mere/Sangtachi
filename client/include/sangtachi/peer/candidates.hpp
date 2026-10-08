#pragma once

// 로컬 후보 판정, 자기 서브넷 집합, 등록 목록 (protocol.md 10.1 후보 수집과 위생).
//
// **순수 코드다.** 인터페이스 열거와 경로 조회의 결과(platform/interfaces.hpp 의 값)를 받아 판정한다.
// OS 를 부르는 것은 CandidateSources 하나이고, 시험은 그 자리에 값과 실패를 주입한다.
// 결정의 근거와 버린 대안은 ADR 0017(로컬 후보 판정 기준)이 갖는다.
//
// ## 로컬 후보 판정 (10.1)
//
// IPv4 주소마다 두 단계를 차례로 본다.
//
// | 단계 | 규칙 | 걸리면 |
// |------|------|--------|
// | 1 활성 | 인터페이스 `OperStatus` 가 `Up` **이고** 주소 `DadState` 가 `Preferred` | 그 주소는 어디에도 쓰지 않는다 (후보도 자기 서브넷 집합도 아니다) |
// | 2 제외 | `IfType` 이 24, 53, 131, 23 가운데 하나 | 후보가 아니다 |
// | 2 제외 | 주소가 루프백 `127.0.0.0/8` 이나 APIPA `169.254.0.0/16` | 후보가 아니다 |
// | 2 제외 | 인터페이스 식별자가 기록해 둔 자기 Wintun 의 `InterfaceGuid` 와 같다 | 후보가 아니다 |
//
// 위 목록에 없는 `IfType` 은 포함한다(허용 목록이 아니다). 자기 Wintun 은 `IfType` 과 무관하게
// 식별자로 따로 본다. 판정이 어댑터의 `IfType` 값에 기대지 않게 하려는 것이다.
// prefix 길이는 후보 판정에 쓰지 않는다.
//
// ## 자기 서브넷 집합 (10.1)
//
// 7장의 자기 서브넷 directed broadcast 판정(loop.hpp 의 classify_datagram)과 받은 후보 위생의
// 자기 서브넷 행(control/ops.hpp 의 sanitize_received_candidates)이 쓰는 집합이다.
//
// - 원소는 **활성 단계를 통과한** 주소와 그 prefix 다. 제외 단계와 무관하다
// - 루프백 대역의 주소는 넣지 않는다 (7장이 루프백 출발지를 통과시킨다)
// - prefix 가 0 이상 30 이하인 주소만 넣는다. 31 과 32 에는 브로드캐스트가 없고 그 밖은 prefix 가 아니다
// - 원소 하나가 막는 주소는 호스트 비트를 모두 1 로 채운 값 하나다. 서브넷 전체를 막지 않는다
// - 첫 수집 전에는 비어 있다
//
// ## 등록 목록 (10.1)
//
// 1. 서버 반사 후보. 받은 순서대로
// 2. 기본 경로 인터페이스의 로컬 후보. 그 인터페이스 안에서는 열거 순서대로
// 3. 나머지 로컬 후보. 열거 순서대로
// 4. 중복(`ip:port`)을 지운다. 먼저 온 원소를 남긴다
// 5. 앞에서부터 MAX_CANDIDATES 개를 남긴다. **자르기는 정렬과 중복 제거 뒤다**
//
// 기본 경로 인터페이스는 0.0.0.0/0 경로 가운데 경로 메트릭 + 인터페이스 메트릭이 가장 작은 줄의
// 인터페이스다. 합이 같으면 열거 순서가 앞인 줄이다. 그런 줄이 없거나 조회가 실패하면 2번은 비고
// 로컬 후보는 열거 순서 그대로다. 로컬 후보의 포트는 6장이 getsockname 으로 읽은 값 하나다.
//
// ## 조회 실패 (10.1 수집 시점)
//
// | 무엇 | 로컬 후보 | 자기 서브넷 집합 | WARN |
// |------|-----------|------------------|------|
// | 열거 실패 | 없음. 반사 후보만 등록한다 | **이전 집합을 그대로 둔다** | `candidates.enumerate_failed` |
// | 열거 성공, 활성 주소 없음 | 없음 | 빈다 | 없음 |
// | 경로 조회 실패 | 열거 순서 | 영향 없음 | `candidates.route_failed` |
//
// 둘 다 다시 시도하지 않는다. 시도를 끝내지도 않는다. WARN 이벤트 이름은 architecture.md 9장의
// 고정 키가 아니다(그 장의 "여기 없는 이벤트는 자유롭게 늘려도 된다").

#include "sangtachi/control/ops.hpp"
#include "sangtachi/network/endpoint.hpp"
#include "sangtachi/platform/interfaces.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace sangtachi::peer {

// 10.1 제외 단계의 IfType 넷.
inline constexpr std::uint32_t kIfTypePpp = 23;
inline constexpr std::uint32_t kIfTypeSoftwareLoopback = 24;
inline constexpr std::uint32_t kIfTypePropVirtual = 53;
inline constexpr std::uint32_t kIfTypeTunnel = 131;

// 자기 서브넷 집합에 넣는 prefix 의 상한. 31 과 32 에는 브로드캐스트 주소가 없다 (10.1).
inline constexpr std::uint8_t kMaxBroadcastPrefix = 30;

struct OwnSubnet {
    std::uint32_t address = 0;  // 호스트 바이트 순서
    std::uint8_t prefix_length = 0;  // 0 이상 kMaxBroadcastPrefix 이하만 들어온다

    // 호스트 비트를 모두 1 로 채운 값.
    [[nodiscard]] std::uint32_t broadcast() const noexcept;

    friend bool operator==(const OwnSubnet&, const OwnSubnet&) = default;
};

// 자기 서브넷 집합. 상태 소유는 [loop] 다 (concurrency.md 5장).
class OwnSubnets {
public:
    OwnSubnets() = default;

    // 원소를 그대로 담는다. 0~30 밖의 prefix 와 루프백 주소는 버린다. 판정 함수 밖에서 만든
    // 집합(시험)도 같은 규칙을 지키게 하려는 것이다.
    explicit OwnSubnets(std::vector<OwnSubnet> entries);

    // 그 주소가 어느 원소의 브로드캐스트 주소와 같은가. 7장과 10.1 자기 서브넷 행의 판정이다.
    [[nodiscard]] bool blocks_broadcast(std::uint32_t address) const noexcept;

    [[nodiscard]] std::span<const OwnSubnet> entries() const noexcept { return entries_; }
    [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }

    friend bool operator==(const OwnSubnets&, const OwnSubnets&) = default;

private:
    std::vector<OwnSubnet> entries_;
};

// 로컬 후보 하나. 포트는 등록 목록을 만들 때 붙인다.
struct LocalAddress {
    std::uint32_t address = 0;
    std::uint32_t if_index = 0;

    friend bool operator==(const LocalAddress&, const LocalAddress&) = default;
};

struct InterfaceJudgement {
    std::vector<LocalAddress> candidates;  // 열거 순서
    OwnSubnets own_subnets;
};

// 위 "로컬 후보 판정" 과 "자기 서브넷 집합". own_wintun 은 기록해 둔 자기 Wintun 의 식별자이고,
// 아직 어댑터를 만들지 않았으면 비어 있다.
[[nodiscard]] InterfaceJudgement judge_interfaces(
    std::span<const platform::InterfaceInfo> interfaces,
    const std::optional<platform::InterfaceGuid>& own_wintun);

// 기본 경로 인터페이스의 if_index. 0.0.0.0/0 줄이 없으면 비어 있다.
[[nodiscard]] std::optional<std::uint32_t> default_route_interface(
    std::span<const platform::Ipv4Route> routes) noexcept;

// 위 "등록 목록". reflexive 는 STUN 이 관측한 순서, locals 는 열거 순서다.
[[nodiscard]] std::vector<control::Candidate> registration_list(
    std::span<const network::Endpoint> reflexive, std::span<const LocalAddress> locals,
    std::optional<std::uint32_t> default_if, std::uint16_t local_port);

// 수집 한 번의 결과 (10.1 수집 시점: register_candidate 를 보내기 직전에 한 번).
struct Collection {
    std::vector<control::Candidate> registration;  // register_candidate 에 그대로 싣는다
    OwnSubnets own_subnets;                        // 다음 수집까지 이 값을 쓴다
    bool enumerate_failed = false;
    std::uint32_t enumerate_error = 0;
    bool route_failed = false;
    std::uint32_t route_error = 0;
};

// 위 "조회 실패" 표까지 포함한 수집. previous 는 지금 쓰는 자기 서브넷 집합이다.
[[nodiscard]] Collection collect(std::span<const network::Endpoint> reflexive,
                                 std::uint16_t local_port,
                                 const platform::InterfaceQuery& interfaces,
                                 const platform::RouteQuery& routes,
                                 const std::optional<platform::InterfaceGuid>& own_wintun,
                                 const OwnSubnets& previous);

// 두 조회의 출처. 제품 경로는 platform_candidate_sources() 이고 시험은 값과 실패를 주입한다.
struct CandidateSources {
    std::function<platform::InterfaceQuery()> query_interfaces;
    std::function<platform::RouteQuery()> query_routes;
};

[[nodiscard]] CandidateSources platform_candidate_sources();

// 두 조회를 한 번씩 부르고 collect 한다. 둘 다 늘 부른다. 열거가 실패해도 경로 조회를 건너뛰지 않는다.
[[nodiscard]] Collection collect(const CandidateSources& sources,
                                 std::span<const network::Endpoint> reflexive,
                                 std::uint16_t local_port,
                                 const std::optional<platform::InterfaceGuid>& own_wintun,
                                 const OwnSubnets& previous);

// 위 "조회 실패" 표의 WARN 을 낸다. 실패가 없으면 아무것도 내지 않는다.
void emit_collection_warnings(const Collection& collection);

}  // namespace sangtachi::peer
