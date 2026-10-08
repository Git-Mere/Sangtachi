#include "sangtachi/platform/interfaces.hpp"

#include "sangtachi/peer/candidates.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

// 시험 케이스 이름은 ASCII 로만 적는다. 이유는 network/wsa_test.cpp 머리에 있다.
//
// 이 기기의 OS 를 실제로 부른다. 값은 기기마다 다르므로 판정은 어느 기기에서나 참인 것만 본다.
// 판정 규칙 자체의 시험은 peer/candidates_test.cpp 가 값을 주입해 돌린다.

using sangtachi::platform::DadState;
using sangtachi::platform::query_ipv4_default_routes;
using sangtachi::platform::query_ipv4_interfaces;

TEST_CASE("platform_interfaces: enumeration succeeds and holds only ipv4 unicast", "[platform][interfaces]") {
    const auto q = query_ipv4_interfaces();
    REQUIRE(q.ok);
    CHECK(q.error == 0);
    for (const auto& i : q.interfaces) {
        INFO("if_index=" << i.if_index << " if_type=" << i.if_type);
        CHECK_FALSE(i.addresses.empty());
        for (const auto& a : i.addresses) {
            // 멀티캐스트는 건너뛰었다. 유니캐스트 주소에 그 대역이 오지 않는다.
            CHECK((a.address >> 28) != 0xEu);
            CHECK(static_cast<int>(a.dad_state) <= static_cast<int>(DadState::kPreferred));
        }
    }
}

TEST_CASE("platform_interfaces: the loopback pseudo interface is never a candidate", "[platform][interfaces]") {
    // Windows 는 Loopback Pseudo-Interface 1(IfType 24, 127.0.0.1)을 늘 열거한다. 있든 없든
    // 판정 결과에 루프백 후보가 없어야 한다.
    const auto q = query_ipv4_interfaces();
    REQUIRE(q.ok);
    const auto judged = sangtachi::peer::judge_interfaces(q.interfaces, std::nullopt);
    for (const auto& c : judged.candidates) {
        CHECK((c.address >> 24) != 127u);
    }
    for (const auto& e : judged.own_subnets.entries()) {
        CHECK((e.address >> 24) != 127u);
        CHECK(e.prefix_length <= sangtachi::peer::kMaxBroadcastPrefix);
    }
}

TEST_CASE("platform_interfaces: default route lookup returns only 0.0.0.0/0", "[platform][interfaces]") {
    const auto q = query_ipv4_default_routes();
    REQUIRE(q.ok);
    for (const auto& r : q.routes) {
        CHECK(r.destination == 0u);
        CHECK(r.prefix_length == 0u);
        CHECK(r.if_index != 0u);
    }
}
