#include "sangtachi/peer/candidates.hpp"

#include "sangtachi/control/ops.hpp"
#include "sangtachi/network/endpoint.hpp"
#include "sangtachi/platform/interfaces.hpp"
#include "sangtachi/protocol_constants.hpp"

#include "../log_capture.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// 시험 케이스 이름은 ASCII 로만 적는다. 이유는 network/wsa_test.cpp 머리에 있다.
//
// protocol.md 10.1 후보 수집과 위생의 케이스 표 둘(로컬 후보 판정, 등록 목록)을 전 행 돌린다. 셋째 표
// (자기 서브넷 행)는 그 행을 판정하는 sanitize_received_candidates 의 시험(control/ops_test.cpp)이
// 갖는다. 행 번호와 "걸리는 구현" 은 그 표 그대로다.

using sangtachi::control::Candidate;
using sangtachi::control::CandidateKind;
using sangtachi::network::Endpoint;
using sangtachi::network::parse_ipv4;
using sangtachi::peer::CandidateSources;
using sangtachi::peer::collect;
using sangtachi::peer::Collection;
using sangtachi::peer::default_route_interface;
using sangtachi::peer::emit_collection_warnings;
using sangtachi::peer::judge_interfaces;
using sangtachi::peer::LocalAddress;
using sangtachi::peer::OwnSubnet;
using sangtachi::peer::OwnSubnets;
using sangtachi::peer::registration_list;
using sangtachi::platform::DadState;
using sangtachi::platform::InterfaceAddress;
using sangtachi::platform::InterfaceGuid;
using sangtachi::platform::InterfaceInfo;
using sangtachi::platform::InterfaceQuery;
using sangtachi::platform::Ipv4Route;
using sangtachi::platform::OperStatus;
using sangtachi::platform::RouteQuery;

namespace {

std::uint32_t ip(std::string_view text) {
    const auto v = parse_ipv4(text);
    REQUIRE(v);
    return *v;
}

// 자기 Wintun 의 식별자. 표의 "그 밖" 칸이 비면 남의 어댑터이므로 다른 값을 쓴다.
constexpr InterfaceGuid kOwnWintun = {0x5A, 0x4E, 0x47, 0x54, 0x01, 0x02, 0x03, 0x04,
                                      0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C};
constexpr InterfaceGuid kOtherGuid = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
                                      0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00};

struct Addr {
    std::string_view address;
    std::uint8_t prefix;
    DadState dad = DadState::kPreferred;
};

InterfaceInfo iface(std::uint32_t if_index, std::uint32_t if_type, std::vector<Addr> addrs,
                    OperStatus status = OperStatus::kUp, const InterfaceGuid& guid = kOtherGuid) {
    InterfaceInfo out;
    out.if_index = if_index;
    out.if_type = if_type;
    out.oper_status = status;
    out.guid = guid;
    for (const auto& a : addrs) {
        out.addresses.push_back(InterfaceAddress{ip(a.address), a.prefix, a.dad});
    }
    return out;
}

std::vector<std::uint32_t> broadcasts(const OwnSubnets& set) {
    std::vector<std::uint32_t> out;
    for (const auto& e : set.entries()) {
        out.push_back(e.broadcast());
    }
    return out;
}

constexpr std::uint16_t kPort = 40000;

}  // namespace

// ---------------------------------------------------------------- 로컬 후보 판정 케이스 표 (10.1)

TEST_CASE("candidates: local candidate judgement table", "[peer][candidates]") {
    struct Row {
        int no;
        InterfaceInfo in;
        bool candidate;                         // 표의 "로컬 후보" 칸. 21 은 두 주소 모두
        std::vector<std::string_view> blocked;  // 표의 "자기 서브넷 집합이 막는 브로드캐스트" 칸
        std::string_view why;
    };
    using O = OperStatus;
    using D = DadState;
    const Row rows[] = {
        {1, iface(1, 71, {{"10.0.0.49", 24}}), true, {"10.0.0.255"}, "baseline"},
        {2, iface(1, 6, {{"192.168.0.10", 24}}), true, {"192.168.0.255"}, "filters IfType 6"},
        {3, iface(1, 243, {{"10.20.0.5", 24}}), true, {"10.20.0.255"}, "allow-list implementation"},
        {4, iface(1, 6, {{"192.168.1.5", 24}}, O::kDown), false, {}, "looks only at the address"},
        {5, iface(1, 6, {{"192.168.1.5", 24, D::kTentative}}), false, {}, "looks only at OperStatus"},
        {6, iface(1, 6, {{"192.168.1.5", 24, D::kDuplicate}}), false, {}, "filters only Tentative"},
        {7, iface(1, 6, {{"192.168.1.5", 24, D::kDeprecated}}), false, {},
         "not-a-failure-state instead of Preferred"},
        {8, iface(1, 6, {{"169.254.199.113", 16, D::kTentative}}, O::kDown), false, {},
         "address left on a disconnected adapter"},
        {9, iface(1, 24, {{"127.0.0.1", 8}}), false, {}, "includes loopback"},
        {10, iface(1, 6, {{"127.0.0.5", 8}}), false, {}, "looks only at IfType, not the range"},
        {11, iface(1, 6, {{"169.254.10.1", 16}}), false, {"169.254.255.255"},
         "misses APIPA; builds the set from candidates"},
        {12, iface(1, 6, {{"169.253.255.254", 24}}), true, {"169.253.255.255"}, "APIPA lower edge"},
        {13, iface(1, 6, {{"169.255.0.1", 24}}), true, {"169.255.0.255"}, "APIPA upper edge"},
        {14, iface(1, 53, {{"100.102.127.59", 32}}), false, {}, "misses 53; computes /32"},
        {15, iface(1, 131, {{"10.30.0.2", 24}}), false, {"10.30.0.255"}, "misses 131"},
        {16, iface(1, 23, {{"10.40.0.2", 24}}), false, {"10.40.0.255"}, "misses 23"},
        {17, iface(1, 6, {{"10.100.0.1", 24}}, O::kUp, kOwnWintun), false, {"10.100.0.255"},
         "drops the identifier rule and leans on IfType 53; IfType is deliberately not 53"},
        {18, iface(1, 6, {{"172.17.0.1", 20}}), true, {"172.17.15.255"},
         "virtual adapter with IfType 6; accepted cost"},
        {19, iface(1, 6, {{"10.0.0.2", 31}}), true, {}, "computes /31"},
        {20, iface(1, 6, {{"10.0.0.4", 30}}), true, {"10.0.0.7"}, "treats /30 like /31"},
        {21, iface(1, 6, {{"192.168.0.10", 24}, {"192.168.5.10", 24}}), true,
         {"192.168.0.255", "192.168.5.255"}, "uses only the first address per interface"},
        {22, iface(1, 6, {{"192.168.0.20", 255}}), true, {},
         "computes without checking the prefix range; drops a candidate by prefix"},
    };
    for (const auto& row : rows) {
        INFO("row=" << row.no << " why=" << row.why);
        const InterfaceInfo one[] = {row.in};
        const auto got = judge_interfaces(one, kOwnWintun);

        std::vector<LocalAddress> want;
        if (row.candidate) {
            for (const auto& a : row.in.addresses) {
                want.push_back(LocalAddress{a.address, row.in.if_index});
            }
        }
        CHECK(got.candidates == want);

        std::vector<std::uint32_t> want_blocked;
        for (const auto b : row.blocked) {
            want_blocked.push_back(ip(b));
        }
        CHECK(broadcasts(got.own_subnets) == want_blocked);
        for (const auto b : want_blocked) {
            CHECK(got.own_subnets.blocks_broadcast(b));
        }
        // 집합은 브로드캐스트 주소 하나만 막는다. 자기 주소는 막지 않는다 (10.1 자기 서브넷 행 표).
        for (const auto& a : row.in.addresses) {
            if (std::find(want_blocked.begin(), want_blocked.end(), a.address) == want_blocked.end()) {
                CHECK_FALSE(got.own_subnets.blocks_broadcast(a.address));
            }
        }
    }
}

TEST_CASE("candidates: own wintun is matched by identifier only", "[peer][candidates]") {
    // 같은 인터페이스라도 기록한 식별자가 없으면(어댑터를 만들기 전) 남의 어댑터다.
    const InterfaceInfo one[] = {iface(1, 6, {{"10.100.0.1", 24}}, OperStatus::kUp, kOwnWintun)};
    CHECK(judge_interfaces(one, std::nullopt).candidates.size() == 1);
    CHECK(judge_interfaces(one, kOtherGuid).candidates.size() == 1);
    CHECK(judge_interfaces(one, kOwnWintun).candidates.empty());
}

TEST_CASE("candidates: every non-up oper status is inactive", "[peer][candidates]") {
    // 10.1 활성: OperStatus 가 Up 인 것만. 표의 Down 말고 나머지도 같다.
    const OperStatus inactive[] = {OperStatus::kDown, OperStatus::kTesting, OperStatus::kUnknown,
                                   OperStatus::kDormant, OperStatus::kNotPresent,
                                   OperStatus::kLowerLayerDown};
    for (const auto status : inactive) {
        INFO("status=" << static_cast<int>(status));
        const InterfaceInfo one[] = {iface(1, 6, {{"192.168.1.5", 24}}, status)};
        const auto got = judge_interfaces(one, kOwnWintun);
        CHECK(got.candidates.empty());
        CHECK(got.own_subnets.empty());
    }
    const InterfaceInfo invalid[] = {iface(1, 6, {{"192.168.1.5", 24, DadState::kInvalid}})};
    CHECK(judge_interfaces(invalid, kOwnWintun).candidates.empty());
}

TEST_CASE("candidates: prefix 0 and 30 are the edges of the own subnet set", "[peer][candidates]") {
    const InterfaceInfo zero[] = {iface(1, 6, {{"10.1.2.3", 0}})};
    CHECK(broadcasts(judge_interfaces(zero, kOwnWintun).own_subnets) ==
          std::vector<std::uint32_t>{0xFFFFFFFFu});
    const InterfaceInfo thirty_three[] = {iface(1, 6, {{"10.1.2.3", 33}})};
    const auto got = judge_interfaces(thirty_three, kOwnWintun);
    CHECK(got.own_subnets.empty());
    CHECK(got.candidates.size() == 1);
}

TEST_CASE("candidates: OwnSubnets drops what the rules do not allow", "[peer][candidates]") {
    // 손으로 만든 집합도 10.1 규칙을 지킨다. 루프백, 31, 32, 그 밖의 prefix 는 들어가지 않는다.
    const OwnSubnets set({{ip("10.0.0.49"), 24}, {ip("127.0.0.1"), 8}, {ip("10.0.0.2"), 31},
                          {ip("10.0.0.9"), 32}, {ip("10.0.0.9"), 200}});
    CHECK(broadcasts(set) == std::vector<std::uint32_t>{ip("10.0.0.255")});
    CHECK(set.blocks_broadcast(ip("10.0.0.255")));
    CHECK_FALSE(set.blocks_broadcast(ip("127.255.255.255")));
    CHECK_FALSE(set.blocks_broadcast(ip("10.0.0.3")));
    CHECK_FALSE(OwnSubnets().blocks_broadcast(ip("10.0.0.255")));
}

// ---------------------------------------------------------------- 기본 경로 (10.1)

TEST_CASE("candidates: default route is the smallest metric sum", "[peer][candidates]") {
    struct Row {
        std::vector<Ipv4Route> routes;
        std::optional<std::uint32_t> want;
        std::string_view why;
    };
    const Row rows[] = {
        {{}, std::nullopt, "no route"},
        {{{0, 0, 7, 10, 25}}, 7u, "one default route"},
        {{{0, 0, 7, 10, 25}, {0, 0, 8, 30, 0}}, 8u, "route metric alone does not decide"},
        {{{0, 0, 7, 0, 35}, {0, 0, 8, 30, 0}}, 8u, "interface metric alone does not decide"},
        {{{0, 0, 7, 10, 20}, {0, 0, 8, 20, 10}}, 7u, "tie goes to the first enumerated"},
        {{{0, 0, 8, 20, 10}, {0, 0, 7, 10, 20}}, 8u, "tie goes to the first enumerated, swapped"},
        {{{0, 1, 9, 0, 0}, {0, 0, 7, 50, 50}}, 7u, "0.0.0.0/1 is not the default route"},
        {{{0x80000000u, 1, 9, 0, 0}}, std::nullopt, "128.0.0.0/1 alone is not a default route"},
        {{{0x0A000000u, 0, 9, 0, 0}, {0, 0, 7, 50, 50}}, 7u, "destination must be 0.0.0.0"},
        {{{0, 0, 9, 0xFFFFFFFFu, 1}, {0, 0, 7, 0xFFFFFFFFu, 0}}, 7u, "the sum does not wrap"},
    };
    for (const auto& row : rows) {
        INFO("why=" << row.why);
        CHECK(default_route_interface(row.routes) == row.want);
    }
}

// ---------------------------------------------------------------- 등록 목록 케이스 표 (10.1)

namespace {

Endpoint ep(std::string_view address, std::uint16_t port = kPort) {
    return Endpoint(ip(address), port);
}

Candidate refl(std::string_view address, std::uint16_t port = 50000) {
    return Candidate{ep(address, port), CandidateKind::kReflexive};
}

Candidate local(std::string_view address) {
    return Candidate{ep(address), CandidateKind::kLocal};
}

constexpr std::uint32_t kEthernet = 1;
constexpr std::uint32_t kWifi = 2;
constexpr std::uint32_t kVpn = 3;

Ipv4Route default_via(std::uint32_t if_index) {
    return Ipv4Route{0, 0, if_index, 0, 25};
}

InterfaceQuery enumerated(std::vector<InterfaceInfo> items) {
    InterfaceQuery q;
    q.ok = true;
    q.interfaces = std::move(items);
    return q;
}

RouteQuery routed(std::vector<Ipv4Route> items) {
    RouteQuery q;
    q.ok = true;
    q.routes = std::move(items);
    return q;
}

// 반사 후보 R1~R8. 서로 다르다.
std::vector<Endpoint> reflexive(std::size_t n) {
    std::vector<Endpoint> out;
    for (std::size_t i = 1; i <= n; ++i) {
        out.push_back(Endpoint(ip("203.0.113.0") + static_cast<std::uint32_t>(i), 50000));
    }
    return out;
}

std::string_view lx(int i) {
    static const std::string_view names[] = {"",          "192.168.0.1", "192.168.0.2",
                                             "192.168.0.3", "192.168.0.4", "192.168.0.5",
                                             "192.168.0.6", "192.168.0.7"};
    return names[i];
}

}  // namespace

TEST_CASE("candidates: registration list table", "[peer][candidates]") {
    struct Row {
        std::string_view no;
        std::vector<Endpoint> reflexive;
        InterfaceQuery interfaces;
        RouteQuery routes;
        std::vector<Candidate> want;
        std::string_view why;
    };
    const InterfaceInfo eth_l1 = iface(kEthernet, 6, {{"192.168.0.1", 24}});
    const InterfaceInfo wifi_l2 = iface(kWifi, 71, {{"192.168.1.2", 24}});
    const InterfaceInfo vpn = iface(kVpn, 53, {{"100.64.0.1", 32}});

    // O4: L1~L6 은 각자의 인터페이스, L7 이 기본 경로 인터페이스다.
    std::vector<InterfaceInfo> seven;
    for (int i = 1; i <= 7; ++i) {
        seven.push_back(iface(static_cast<std::uint32_t>(10 + i), 6, {{lx(i), 24}}));
    }
    // O6: L1~L7 이 한 인터페이스.
    const InterfaceInfo one_with_seven =
        iface(kEthernet, 6,
              {{lx(1), 24}, {lx(2), 24}, {lx(3), 24}, {lx(4), 24}, {lx(5), 24}, {lx(6), 24}, {lx(7), 24}});

    InterfaceQuery failed_enum;
    failed_enum.ok = false;
    failed_enum.error = 8;  // ERROR_NOT_ENOUGH_MEMORY. 값은 무엇이든 실패다
    RouteQuery failed_route;
    failed_route.ok = false;
    failed_route.error = 1168;

    const auto r = reflexive(8);
    const Row rows[] = {
        {"O1", {r[0]}, enumerated({eth_l1, wifi_l2}), routed({default_via(kWifi)}),
         {refl("203.0.113.1"), local("192.168.1.2"), local("192.168.0.1")}, "does not sort"},
        {"O2", {r[0]}, enumerated({eth_l1, wifi_l2, vpn}), routed({default_via(kVpn)}),
         {refl("203.0.113.1"), local("192.168.0.1"), local("192.168.1.2")},
         "reorders when the default route has no candidate"},
        {"O3", {r[0]}, enumerated({eth_l1, wifi_l2}), routed({}),
         {refl("203.0.113.1"), local("192.168.0.1"), local("192.168.1.2")}, "same, no default route"},
        {"O4", {r[0], r[1]}, enumerated(seven), routed({default_via(17)}),
         {refl("203.0.113.1"), refl("203.0.113.2"), local(lx(7)), local(lx(1)), local(lx(2)),
          local(lx(3)), local(lx(4)), local(lx(5))},
         "cuts reflexive candidates; sorts after cutting"},
        {"O5", {Endpoint(ip("203.0.113.7"), kPort)}, enumerated({iface(kEthernet, 6, {{"203.0.113.7", 24}})}),
         routed({default_via(kEthernet)}),
         {Candidate{Endpoint(ip("203.0.113.7"), kPort), CandidateKind::kReflexive}},
         "does not dedupe, or keeps the local one"},
        {"O6", {r[0], r[0]}, enumerated({one_with_seven}), routed({}),
         {refl("203.0.113.1"), local(lx(1)), local(lx(2)), local(lx(3)), local(lx(4)), local(lx(5)),
          local(lx(6)), local(lx(7))},
         "cuts before dedupe"},
        {"O7", {r[0]},
         enumerated({eth_l1, iface(kWifi, 71, {{"192.168.1.2", 24}, {"192.168.1.3", 24}})}),
         routed({default_via(kWifi)}),
         {refl("203.0.113.1"), local("192.168.1.2"), local("192.168.1.3"), local("192.168.0.1")},
         "raises only one address of the default interface"},
        {"O8", r, enumerated({wifi_l2}), routed({default_via(kWifi)}),
         {refl("203.0.113.1"), refl("203.0.113.2"), refl("203.0.113.3"), refl("203.0.113.4"),
          refl("203.0.113.5"), refl("203.0.113.6"), refl("203.0.113.7"), refl("203.0.113.8")},
         "pushes out reflexive candidates for a local one"},
        {"O9", {r[0]}, failed_enum, routed({default_via(kWifi)}), {refl("203.0.113.1")},
         "treats failure as an empty enumeration; ends the attempt"},
        {"O10", {r[0]}, enumerated({iface(kEthernet, 6, {{"10.0.0.49", 24}}, OperStatus::kDown)}), routed({}),
         {refl("203.0.113.1")}, "treats an empty success as failure"},
        {"O11", {r[0]}, enumerated({eth_l1, wifi_l2}), failed_route,
         {refl("203.0.113.1"), local("192.168.0.1"), local("192.168.1.2")},
         "stops registering when the route lookup fails"},
    };
    const OwnSubnets previous({{ip("10.0.0.49"), 24}});
    for (const auto& row : rows) {
        INFO("row=" << row.no << " why=" << row.why);
        const Collection got = collect(row.reflexive, kPort, row.interfaces, row.routes, kOwnWintun, previous);
        CHECK(got.registration == row.want);
        CHECK(got.registration.size() <= sangtachi::protocol::kMaxCandidates);
        CHECK(got.enumerate_failed == !row.interfaces.ok);
        CHECK(got.route_failed == !row.routes.ok);
    }
}

TEST_CASE("candidates: O9 keeps the previous set, O10 empties it", "[peer][candidates]") {
    const OwnSubnets previous({{ip("10.0.0.49"), 24}});
    const auto r = reflexive(1);

    InterfaceQuery failed;
    failed.ok = false;
    failed.error = 8;
    const Collection o9 = collect(r, kPort, failed, routed({}), kOwnWintun, previous);
    CHECK(o9.own_subnets == previous);
    CHECK(o9.own_subnets.blocks_broadcast(ip("10.0.0.255")));
    CHECK(o9.enumerate_error == 8);

    const Collection o10 = collect(
        r, kPort, enumerated({iface(kEthernet, 6, {{"10.0.0.49", 24}}, OperStatus::kDown)}), routed({}),
        kOwnWintun, previous);
    CHECK(o10.own_subnets.empty());
    CHECK_FALSE(o10.own_subnets.blocks_broadcast(ip("10.0.0.255")));
    CHECK_FALSE(o10.enumerate_failed);

    // 빈 열거(인터페이스 0개)도 성공이다.
    const Collection none = collect(r, kPort, enumerated({}), routed({}), kOwnWintun, previous);
    CHECK(none.own_subnets.empty());

    // 성공한 수집은 새 집합으로 바꾼다. 이전 것을 더하지 않는다.
    const Collection fresh = collect(
        r, kPort, enumerated({iface(kEthernet, 6, {{"192.168.0.10", 24}})}), routed({}), kOwnWintun, previous);
    CHECK(broadcasts(fresh.own_subnets) == std::vector<std::uint32_t>{ip("192.168.0.255")});
}

TEST_CASE("candidates: the set comes from the active stage, not the candidates", "[peer][candidates]") {
    // 후보에서 뺀 인터페이스(53, 자기 Wintun, APIPA)의 서브넷도 집합에 든다 (10.1, ADR 0017 대안 8).
    const InterfaceInfo items[] = {
        iface(1, 131, {{"10.30.0.2", 24}}),
        iface(2, 6, {{"10.100.0.1", 24}}, OperStatus::kUp, kOwnWintun),
        iface(3, 71, {{"10.0.0.49", 24}}),
    };
    const auto got = judge_interfaces(items, kOwnWintun);
    CHECK(got.candidates == std::vector<LocalAddress>{{ip("10.0.0.49"), 3}});
    CHECK(broadcasts(got.own_subnets) ==
          std::vector<std::uint32_t>{ip("10.30.0.255"), ip("10.100.0.255"), ip("10.0.0.255")});
}

TEST_CASE("candidates: sources are each called once even when enumeration fails", "[peer][candidates]") {
    int interface_calls = 0;
    int route_calls = 0;
    CandidateSources sources;
    sources.query_interfaces = [&] {
        ++interface_calls;
        InterfaceQuery q;
        q.ok = false;
        q.error = 5;
        return q;
    };
    sources.query_routes = [&] {
        ++route_calls;
        RouteQuery q;
        q.ok = false;
        q.error = 6;
        return q;
    };
    const auto r = reflexive(1);
    const Collection got = collect(sources, r, kPort, kOwnWintun, OwnSubnets());
    CHECK(interface_calls == 1);
    CHECK(route_calls == 1);
    CHECK(got.registration == std::vector<Candidate>{refl("203.0.113.1")});
    CHECK(got.enumerate_failed);
    CHECK(got.enumerate_error == 5);
    CHECK(got.route_failed);
    CHECK(got.route_error == 6);
}

TEST_CASE("candidates: each failure leaves one WARN line", "[peer][candidates]") {
    Collection c;
    {
        sangtachi::testing::LogCapture log;
        emit_collection_warnings(c);
        CHECK(log.lines.empty());
    }
    c.enumerate_failed = true;
    c.enumerate_error = 8;
    {
        sangtachi::testing::LogCapture log;
        emit_collection_warnings(c);
        REQUIRE(log.with_event("candidates.enumerate_failed").size() == 1);
        CHECK(log.with_event("candidates.enumerate_failed")[0] == "WARN candidates.enumerate_failed code=8");
        CHECK(log.with_event("candidates.route_failed").empty());
    }
    c.route_failed = true;
    c.route_error = 1168;
    {
        sangtachi::testing::LogCapture log;
        emit_collection_warnings(c);
        CHECK(log.lines.size() == 2);
        CHECK(log.with_event("candidates.route_failed") ==
              std::vector<std::string>{"WARN candidates.route_failed code=1168"});
    }
}

TEST_CASE("candidates: IfType 24 is excluded on its own", "[peer][candidates]") {
    // 표 9행은 루프백 대역 규칙에도 걸려 IfType 24 규칙을 따로 보지 못한다. 대역 밖 주소로 IfType 만 남긴다.
    const InterfaceInfo one[] = {iface(1, 24, {{"10.50.0.2", 24}})};
    const auto got = judge_interfaces(one, kOwnWintun);
    CHECK(got.candidates.empty());
    CHECK(broadcasts(got.own_subnets) == std::vector<std::uint32_t>{ip("10.50.0.255")});
}

TEST_CASE("candidates: registration dedupe compares ip and port", "[peer][candidates]") {
    // 10.1 등록 목록 4번의 중복은 같은 엔드포인트다. 주소만 비교하는 구현은 여기서 걸린다.
    const Endpoint refl_list[] = {Endpoint(ip("203.0.113.7"), 50000), Endpoint(ip("203.0.113.7"), 50001)};
    const LocalAddress locals[] = {{ip("203.0.113.7"), 1}};
    const auto got = registration_list(refl_list, locals, std::nullopt, kPort);
    CHECK(got == std::vector<Candidate>{refl("203.0.113.7", 50000), refl("203.0.113.7", 50001),
                                        local("203.0.113.7")});
}

TEST_CASE("candidates: registration list uses the bound port for every local", "[peer][candidates]") {
    const LocalAddress locals[] = {{ip("192.168.0.1"), 1}, {ip("10.0.0.1"), 2}};
    const auto got = registration_list({}, locals, std::nullopt, 12345);
    REQUIRE(got.size() == 2);
    for (const auto& c : got) {
        CHECK(c.endpoint.port() == 12345);
        CHECK(c.kind == CandidateKind::kLocal);
    }
}
