#include "sangtachi/telemetry/telemetry.hpp"

#include "sangtachi/control/json.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

// 시험 케이스 이름은 ASCII 로만 적는다. 이유는 network/wsa_test.cpp 머리에 있다.
//
// 규칙의 출처는 architecture.md 6.3 의 필드 표와 9장 텔레메트리 업로드다.

using namespace sangtachi::telemetry;

namespace {

Stamp stamp() {
    return Stamp{"00112233445566778899aabbccddeeff", "windows", "console", "test"};
}

}  // namespace

TEST_CASE("telemetry: establish json carries the common and establish fields only", "[telemetry]") {
    Record r;
    r.kind = Kind::kEstablish;
    r.attempt = 3;
    r.role = "player";
    r.room_id = "ABCDEF";
    r.peer_id = 11;
    r.peer = 7;
    r.result = "OK";
    r.setup_ms = 2410;
    r.local_differs_public = true;
    r.stun_same_endpoint = false;
    r.same_lan = false;
    CHECK(to_json(r, stamp()) ==
          R"({"v":1,"kind":"establish","instance_id":"00112233445566778899aabbccddeeff","attempt":3,"seq":0,)"
          R"("role":"player","room_id":"ABCDEF","peer_id":11,"peer":7,"os":"windows","ui":"console",)"
          R"("build_kind":"test","result":"OK","setup_ms":2410,"local_differs_public":true,)"
          R"("stun_same_endpoint":false,"same_lan":false})");
}

TEST_CASE("telemetry: values not yet known are null, not zero", "[telemetry]") {
    // 6.3: 받기 전이면 null. 0 은 측정된 값이다 (9장).
    Record r;
    r.kind = Kind::kEstablish;
    r.attempt = 1;
    r.role = "host";
    r.result = "STUN_DISCOVERY_FAILED";
    CHECK(to_json(r, stamp()) ==
          R"({"v":1,"kind":"establish","instance_id":"00112233445566778899aabbccddeeff","attempt":1,"seq":0,)"
          R"("role":"host","room_id":null,"peer_id":null,"peer":null,"os":"windows","ui":"console",)"
          R"("build_kind":"test","result":"STUN_DISCOVERY_FAILED","setup_ms":null,"local_differs_public":null,)"
          R"("stun_same_endpoint":null,"same_lan":null})");
}

TEST_CASE("telemetry: interval and end json", "[telemetry]") {
    Record r;
    r.kind = Kind::kInterval;
    r.attempt = 2;
    r.seq = 1;
    r.role = "host";
    r.room_id = "ABCDEF";
    r.peer_id = 7;
    r.peer = 11;
    r.interval = IntervalValues{60000, {1200, 1300}, 0, 6, 40, 39, 5000, 4800};
    const std::string head =
        R"({"v":1,"kind":"interval","instance_id":"00112233445566778899aabbccddeeff","attempt":2,"seq":1,)"
        R"("role":"host","room_id":"ABCDEF","peer_id":7,"peer":11,"os":"windows","ui":"console","build_kind":"test",)";
    CHECK(to_json(r, stamp()) == head +
          R"("interval_ms":60000,"rtt_us":[1200,1300],"rtt_overflow":0,"jitter_us":6,"expected":40,)"
          R"("accepted":39,"bytes_out":5000,"bytes_in":4800})");

    r.kind = Kind::kEnd;
    r.seq = 2;
    r.result = "TUNNEL_DROPPED";
    r.interval = IntervalValues{1500, {}, 0, std::nullopt, 0, 0, 0, 0};
    r.totals = SessionTotals{61500, 40, 39, 5100, 4900, 2};
    std::string end_head = head;
    end_head.replace(end_head.find("interval"), 8, "end");
    end_head.replace(end_head.find(R"("seq":1)"), 7, R"("seq":2)");
    CHECK(to_json(r, stamp()) == end_head +
          R"("interval_ms":1500,"rtt_us":[],"rtt_overflow":0,"jitter_us":null,"expected":0,"accepted":0,)"
          R"("bytes_out":0,"bytes_in":0,"result":"TUNNEL_DROPPED","duration_ms":61500,"total_expected":40,)"
          R"("total_accepted":39,"total_bytes_out":5100,"total_bytes_in":4900,"total_tx_err_send":2})");
}

TEST_CASE("telemetry: every record is a json object the service can parse", "[telemetry]") {
    for (const Kind kind : {Kind::kEstablish, Kind::kInterval, Kind::kEnd}) {
        Record r;
        r.kind = kind;
        r.attempt = 1;
        r.role = "player";
        r.result = "CLOSED";
        const auto parsed = sangtachi::control::json::parse(to_json(r, stamp()));
        REQUIRE(parsed.value.has_value());
        REQUIRE(parsed.value->kind() == sangtachi::control::json::Kind::kObject);
    }
}

TEST_CASE("telemetry: jitter follows the smoothed formula of architecture 9", "[telemetry]") {
    // J = J + (|R_i - R_(i-1)| - J) / 16, 처음 값 0, 두 번째 표본부터. 표본이 둘보다 적으면 없다.
    struct Row {
        std::vector<std::uint64_t> samples;
        std::optional<std::uint64_t> jitter;
        const char* why;
    };
    const Row rows[] = {
        {{}, std::nullopt, "no sample"},
        {{1000}, std::nullopt, "one sample"},
        {{1000, 1000}, 0, "two equal samples are a measured zero"},
        {{1000, 1160}, 10, "160 / 16"},
        {{1160, 1000}, 10, "the difference is absolute"},
        {{1000, 1160, 1000}, 19, "10 + (160 - 10) / 16 = 19.375"},
        {{1000, 1016, 1032, 1048}, 3, "small steps are not lost to integer division (2.82)"},
    };
    for (const auto& row : rows) {
        INFO("why=" << row.why);
        SessionMeter m;
        m.start(0, 0, 0);
        for (const auto s : row.samples) {
            m.add_rtt(s);
        }
        CHECK(m.jitter_us() == row.jitter);
    }
}

TEST_CASE("telemetry: interval values are differences since the previous record", "[telemetry]") {
    SessionMeter m;
    // 처음 CONNECTED 전의 바이트는 구간에 들지 않고 세션 전체에는 든다.
    m.add_out(300);
    m.add_in(200);
    m.start(1000, 0, 0);
    m.start(5000, 99, 99);  // 재협상 뒤의 진입은 기점을 옮기지 않는다
    m.add_out(1000);
    m.add_in(900);
    m.add_rtt(500);
    m.add_rtt(700);

    const IntervalValues first = m.take_interval(61000, 12, 11);
    CHECK(first.interval_ms == 60000);
    CHECK(first.rtt_us == std::vector<std::uint64_t>{500, 700});
    CHECK(first.rtt_overflow == 0);
    CHECK(first.jitter_us == std::optional<std::uint64_t>(13));  // 200 / 16 = 12.5
    CHECK(first.expected == 12);
    CHECK(first.accepted == 11);
    CHECK(first.bytes_out == 1000);
    CHECK(first.bytes_in == 900);

    m.add_out(10);
    const IntervalValues second = m.take_interval(70000, 20, 18);
    CHECK(second.interval_ms == 9000);
    CHECK(second.rtt_us.empty());
    CHECK(second.expected == 8);
    CHECK(second.accepted == 7);
    CHECK(second.bytes_out == 10);
    CHECK(second.bytes_in == 0);
    // 지터는 구간이 아니라 세션의 값이다.
    CHECK(second.jitter_us == first.jitter_us);

    CHECK(m.bytes_out() == 1310);
    CHECK(m.bytes_in() == 1100);
    CHECK(m.started_at() == std::optional<sangtachi::Millis>(1000));
}

TEST_CASE("telemetry: rtt samples beyond sixteen per interval are counted, not kept", "[telemetry]") {
    SessionMeter m;
    m.start(0, 0, 0);
    for (std::uint64_t i = 0; i < kMaxRttSamples + 3; ++i) {
        m.add_rtt(1000 + i);
    }
    const IntervalValues v = m.take_interval(60000, 0, 0);
    REQUIRE(v.rtt_us.size() == kMaxRttSamples);
    CHECK(v.rtt_us.front() == 1000);
    CHECK(v.rtt_us.back() == 1000 + kMaxRttSamples - 1);  // 앞의 16개를 싣고 뒤를 버린다
    CHECK(v.rtt_overflow == 3);

    m.add_rtt(5);
    const IntervalValues next = m.take_interval(120000, 0, 0);
    CHECK(next.rtt_us == std::vector<std::uint64_t>{5});
    CHECK(next.rtt_overflow == 0);
}

TEST_CASE("telemetry: instance id is 32 lowercase hex digits from the random source", "[telemetry]") {
    const auto id = make_instance_id([](std::span<std::byte> out) {
        for (std::size_t i = 0; i < out.size(); ++i) {
            out[i] = static_cast<std::byte>(i * 17 + 0xA0);
        }
        return true;
    });
    REQUIRE(id == std::optional<std::string>("a0b1c2d3e4f5061728394a5b6c7d8e9f"));
    // 난수원이 실패하면 값이 없다. 고정값으로 대신하지 않는다.
    CHECK_FALSE(make_instance_id([](std::span<std::byte>) { return false; }).has_value());
    CHECK_FALSE(make_instance_id({}).has_value());
}
