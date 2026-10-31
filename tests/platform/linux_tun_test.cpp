#include "sangtachi/platform/tun.hpp"

#include "sangtachi/adapter.hpp"
#include "sangtachi/peer/candidates.hpp"
#include "sangtachi/platform/interfaces.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <string>

// 시험 케이스 이름은 ASCII 로만 적는다. 이유는 network/wsa_test.cpp 머리에 있다.
//
// 이 기기의 커널을 실제로 부른다. 어댑터를 여는 케이스는 CAP_NET_ADMIN 이 없으면 건너뛴다.

using namespace sangtachi::platform;

namespace {

bool has_ipv4(const InterfaceQuery& q, std::uint32_t if_index, std::uint32_t address, std::uint8_t prefix) {
    for (const auto& i : q.interfaces) {
        if (i.if_index != if_index) {
            continue;
        }
        for (const auto& a : i.addresses) {
            if (a.address == address && a.prefix_length == prefix) {
                return true;
            }
        }
    }
    return false;
}

bool has_interface(const InterfaceQuery& q, const std::string& name) {
    for (const auto& i : q.interfaces) {
        if (interface_id_text(i.guid) == name) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("linux_tun: no files sit next to the executable", "[platform][linux]") {
    CHECK(adapter_files().empty());
    CHECK(missing_adapter_files().empty());
}

TEST_CASE("linux_tun: firewall check is skipped", "[platform][linux]") {
    const auto r = run_firewall_check();
    CHECK(r.verdict == FirewallVerdict::kSkipped);
    CHECK(r.failure.op.empty());
}

TEST_CASE("linux_tun: subnet check reads the kernel tables", "[platform][linux]") {
    // 판정은 기기마다 다르다. 조회가 성공했으면 unknown 이 아니다.
    const auto r = run_subnet_check(sangtachi::adapter::kSubnetText);
    INFO("op=" << r.failure.op << " code=" << r.failure.code);
    CHECK(r.verdict != SubnetVerdict::kUnknown);
    CHECK(r.failure.op.empty());
}

TEST_CASE("linux_interfaces: loopback is enumerated by name with type 24", "[platform][linux]") {
    const auto q = query_ipv4_interfaces();
    REQUIRE(q.ok);
    bool found = false;
    for (const auto& i : q.interfaces) {
        if (interface_id_text(i.guid) != "lo") {
            continue;
        }
        found = true;
        CHECK(i.if_type == 24u);
        CHECK(i.oper_status == OperStatus::kUp);
        CHECK(has_ipv4(q, i.if_index, 0x7F000001u, 8));
    }
    CHECK(found);
}

TEST_CASE("linux_interfaces: id text is the name up to the first zero", "[platform][linux]") {
    InterfaceGuid id{};
    CHECK(interface_id_text(id).empty());
    const std::string name = "Sangtachi";
    std::copy(name.begin(), name.end(), id.begin());
    CHECK(interface_id_text(id) == name);
    InterfaceGuid full{};
    full.fill('a');
    CHECK(interface_id_text(full) == std::string(16, 'a'));
}

TEST_CASE("linux_tun: adapter lifecycle with address and on-link route", "[platform][linux][adapter]") {
    if (!is_elevated()) {
        SKIP("needs CAP_NET_ADMIN");
    }
    auto opened = TunDevice::open(1400);
    INFO("op=" << opened.failure.op << " code=" << opened.failure.code);
    REQUIRE(opened.device);
    auto& tun = *opened.device;
    CHECK(interface_id_text(tun.guid()) == "Sangtachi");
    CHECK(tun.read_event() != nullptr);
    // 장치가 올라가면 커널이 IPv6 링크 로컬 패킷(라우터 요청 등)을 내보낼 수 있다. 그것을 비운 뒤 빈 상태를 본다.
    auto status = tun.receive().status;
    for (int i = 0; i < 64 && status == sangtachi::adapter::ReceiveStatus::kPacket; ++i) {
        status = tun.receive().status;
    }
    CHECK(status == sangtachi::adapter::ReceiveStatus::kEmpty);

    const std::uint32_t address = 0x0A640001u;  // 10.100.0.1
    const auto assigned = tun.assign_address(address);
    INFO("reason=" << assigned.reason << " op=" << assigned.failure.op << " code=" << assigned.failure.code);
    REQUIRE(assigned.ok);
    {
        const auto q = query_ipv4_interfaces();
        REQUIRE(q.ok);
        CHECK(has_ipv4(q, static_cast<std::uint32_t>(tun.luid()), address, 24));
        // 자기 어댑터는 후보와 자기 서브넷 집합에서 빠진다 (protocol.md 10.1).
        const auto judged = sangtachi::peer::judge_interfaces(q.interfaces, tun.guid());
        for (const auto& c : judged.candidates) {
            CHECK(c.address != address);
        }
    }
    // 주소가 붙었으니 같은 기기의 서브넷 검사는 그 대역을 막는다.
    CHECK(run_subnet_check(sangtachi::adapter::kSubnetText).verdict == SubnetVerdict::kBlock);

    // 다시 붙이면 앞 주소를 지운다.
    const std::uint32_t second = 0x0A640002u;
    REQUIRE(tun.assign_address(second).ok);
    {
        const auto q = query_ipv4_interfaces();
        REQUIRE(q.ok);
        CHECK_FALSE(has_ipv4(q, static_cast<std::uint32_t>(tun.luid()), address, 24));
        CHECK(has_ipv4(q, static_cast<std::uint32_t>(tun.luid()), second, 24));
    }

    CHECK(tun.close().op.empty());
    CHECK(tun.close().op.empty());  // 두 번 불러도 된다
    CHECK(tun.read_event() == nullptr);
    const auto after = query_ipv4_interfaces();
    REQUIRE(after.ok);
    CHECK_FALSE(has_interface(after, "Sangtachi"));
}
