#include "sangtachi/record.hpp"

#include "log_capture.hpp"
#include "sangtachi/counters.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// 시험 케이스 이름은 ASCII 로만 적는다. 이유는 network/wsa_test.cpp 머리에 있다.
//
// architecture.md 9장 로컬 기록 파일의 줄 형식, 시도 번호, 쓰기와 실패.

using sangtachi::Counter;
using sangtachi::Counters;
using sangtachi::record::file_name;
using sangtachi::record::format_line;
using sangtachi::record::format_timestamp;
using sangtachi::record::LineKind;
using sangtachi::record::RecordLine;
using sangtachi::record::RecordWriter;

namespace {

// 9장 줄 형식 예의 세 시각.
constexpr std::uint64_t kT1 = 1791376496789;  // 2026-10-07T12:34:56.789Z
constexpr std::uint64_t kT2 = 1791378130004;  // 2026-10-07T13:02:10.004Z
constexpr std::uint64_t kT3 = 1791378301120;  // 2026-10-07T13:05:01.120Z

// 쓰기 호출을 모은다. fail_from 번째 호출(0부터)부터 거짓을 돌려준다.
struct FakeFile {
    std::vector<std::string> writes;
    std::size_t fail_from = SIZE_MAX;

    RecordWriter::WriteFn fn() {
        return [this](std::string_view bytes) {
            const bool ok = writes.size() < fail_from;
            writes.emplace_back(bytes);
            return ok;
        };
    }
};

}  // namespace

TEST_CASE("record: the three example lines of chapter 9 come out byte for byte", "[record]") {
    REQUIRE(format_line(RecordLine{LineKind::kEstablish, 1, "player", 3021551877u, "OK", std::nullopt, 2410}, kT1) ==
            "v=1 kind=establish ts=2026-10-07T12:34:56.789Z attempt=1 role=player peer=3021551877 "
            "result=OK rtt_us=- setup_ms=2410\n");
    REQUIRE(format_line(RecordLine{LineKind::kEnd, 1, "player", 3021551877u, "CLOSED", 23410, std::nullopt}, kT2) ==
            "v=1 kind=end ts=2026-10-07T13:02:10.004Z attempt=1 role=player peer=3021551877 "
            "result=CLOSED rtt_us=23410 setup_ms=-\n");
    REQUIRE(format_line(RecordLine{LineKind::kEstablish, 2, "player", std::nullopt, "STUN_DISCOVERY_FAILED",
                                   std::nullopt, std::nullopt},
                        kT3) ==
            "v=1 kind=establish ts=2026-10-07T13:05:01.120Z attempt=2 role=player peer=- "
            "result=STUN_DISCOVERY_FAILED rtt_us=- setup_ms=-\n");
}

TEST_CASE("record: units are not converted", "[record]") {
    // roadmap.md Phase 4: 밀리초를 마이크로초 자리에 적는 구현이 여기서 걸린다.
    const auto line =
        format_line(RecordLine{LineKind::kEstablish, 7, "host", 5u, "OK", 23410, 2410}, kT1);
    REQUIRE(line.find(" rtt_us=23410 ") != std::string::npos);
    REQUIRE(line.find(" setup_ms=2410\n") != std::string::npos);
    REQUIRE(line.find(" role=host ") != std::string::npos);
    REQUIRE(line.find(" attempt=7 ") != std::string::npos);
}

TEST_CASE("record: zero is a value and only a missing value is a dash", "[record]") {
    // 9장: 값이 없으면 `-` 다. 0 은 측정된 값이다.
    const auto zeros = format_line(RecordLine{LineKind::kEstablish, 1, "host", 0u, "OK", 0, 0}, kT1);
    REQUIRE(zeros.find(" peer=0 ") != std::string::npos);
    REQUIRE(zeros.find(" rtt_us=0 ") != std::string::npos);
    REQUIRE(zeros.find(" setup_ms=0\n") != std::string::npos);
    REQUIRE(zeros.find('-', zeros.find("Z ")) == std::string::npos);

    const auto none = format_line(
        RecordLine{LineKind::kEstablish, 1, "host", std::nullopt, "ABORTED", std::nullopt, std::nullopt}, kT1);
    REQUIRE(none.find(" peer=- ") != std::string::npos);
    REQUIRE(none.find(" rtt_us=- ") != std::string::npos);
    REQUIRE(none.find(" setup_ms=-\n") != std::string::npos);
}

TEST_CASE("record: nine fields in the fixed order and one LF", "[record]") {
    const auto line =
        format_line(RecordLine{LineKind::kEnd, 3, "host", 9u, "PEER_FAILED", 120, std::nullopt}, kT1);
    REQUIRE(line.back() == '\n');
    REQUIRE(line.find('\n') == line.size() - 1);
    REQUIRE(line.find('\r') == std::string::npos);

    std::vector<std::string> names;
    std::size_t start = 0;
    const std::string body = line.substr(0, line.size() - 1);
    while (start <= body.size()) {
        const auto space = body.find(' ', start);
        const auto pair = body.substr(start, space == std::string::npos ? std::string::npos : space - start);
        const auto eq = pair.find('=');
        REQUIRE(eq != std::string::npos);
        REQUIRE(eq + 1 < pair.size());  // 빈 값이 없다
        names.push_back(pair.substr(0, eq));
        if (space == std::string::npos) {
            break;
        }
        start = space + 1;
    }
    REQUIRE(names == std::vector<std::string>{"v", "kind", "ts", "attempt", "role", "peer", "result",
                                              "rtt_us", "setup_ms"});
}

TEST_CASE("record: timestamp pads every part and turns over at midnight", "[record]") {
    REQUIRE(format_timestamp(0) == "1970-01-01T00:00:00.000Z");
    REQUIRE(format_timestamp(kT1) == "2026-10-07T12:34:56.789Z");
    REQUIRE(format_timestamp(kT2) == "2026-10-07T13:02:10.004Z");  // 밀리초 앞 두 자리 0
    REQUIRE(format_timestamp(1791376496000) == "2026-10-07T12:34:56.000Z");
    REQUIRE(format_timestamp(1791376496050) == "2026-10-07T12:34:56.050Z");
    REQUIRE(format_timestamp(1767323045999) == "2026-01-02T03:04:05.999Z");
    // 자정의 앞뒤.
    REQUIRE(format_timestamp(1791417599999) == "2026-10-07T23:59:59.999Z");
    REQUIRE(format_timestamp(1791417600000) == "2026-10-08T00:00:00.000Z");
    // 윤일.
    REQUIRE(format_timestamp(1709164800050) == "2024-02-29T00:00:00.050Z");
}

TEST_CASE("record: file name is UTC start second and decimal PID", "[record]") {
    REQUIRE(file_name(kT1, 4812) == "20261007T123456Z-4812.log");
    // 초 아래는 버린다. 올리지 않는다.
    REQUIRE(file_name(1767323045999, 7) == "20260102T030405Z-7.log");
    REQUIRE(file_name(1791417600000, 4294967295u) == "20261008T000000Z-4294967295.log");
}

TEST_CASE("record: attempts start at 1 and the end line reuses the number", "[record]") {
    FakeFile file;
    std::uint64_t now = kT1;
    Counters counters;
    RecordWriter writer(file.fn(), [&now] { return now; }, counters);

    const auto first = writer.establish("player", 3021551877u, "OK", std::nullopt, 2410);
    now = kT2;
    const auto second = writer.establish("player", std::nullopt, "HOLE_PUNCH_TIMEOUT", std::nullopt, std::nullopt);
    now = kT3;
    writer.end(first, "player", 3021551877u, "CLOSED", 23410);

    REQUIRE(first == 1);
    REQUIRE(second == 2);
    REQUIRE(file.writes.size() == 3);  // 줄마다 쓰기 호출 하나
    REQUIRE(file.writes[0] ==
            "v=1 kind=establish ts=2026-10-07T12:34:56.789Z attempt=1 role=player peer=3021551877 "
            "result=OK rtt_us=- setup_ms=2410\n");
    REQUIRE(file.writes[1].find(" ts=2026-10-07T13:02:10.004Z attempt=2 ") != std::string::npos);
    // 종료 줄은 그 세션의 수립 줄 번호를 쓰고 setup_ms 는 늘 `-` 다.
    REQUIRE(file.writes[2] ==
            "v=1 kind=end ts=2026-10-07T13:05:01.120Z attempt=1 role=player peer=3021551877 "
            "result=CLOSED rtt_us=23410 setup_ms=-\n");
    REQUIRE_FALSE(writer.failed());
    REQUIRE(counters.value(Counter::RecordWriteFailed) == 0);
}

TEST_CASE("record: after a failed write nothing more is written", "[record]") {
    // 9장 쓰기와 실패. 일부만 쓰인 것도 쓰기 함수가 거짓을 돌려주는 실패다 (platform/record_file).
    sangtachi::testing::LogCapture log;
    FakeFile file;
    file.fail_from = 1;  // 두 번째 쓰기가 실패한다
    Counters counters;
    std::size_t clock_reads = 0;
    RecordWriter writer(file.fn(), [&clock_reads] { ++clock_reads; return kT1; }, counters);

    // 쓴 줄마다 벽시계를 한 번 읽는다. 성공한 쓰기와 실패한 쓰기 모두.
    const auto first = writer.establish("host", 5u, "OK", std::nullopt, 900);
    REQUIRE_FALSE(writer.failed());
    REQUIRE(clock_reads == 1);
    const auto second = writer.establish("host", 6u, "OK", std::nullopt, 950);
    REQUIRE(writer.failed());
    REQUIRE(clock_reads == 2);
    REQUIRE(file.writes.size() == 2);
    REQUIRE(counters.value(Counter::RecordWriteFailed) == 1);
    const auto warns = log.with_event("record.write_failed");
    REQUIRE(warns.size() == 1);
    REQUIRE(warns[0] == "WARN record.write_failed kind=establish attempt=2");

    // 그 뒤의 줄은 쓰기 호출에 닿지 않고 벽시계도 읽지 않는다. 카운터와 WARN 은 실패한 그 한 번뿐이다.
    const auto third = writer.establish("host", 7u, "ABORTED", std::nullopt, std::nullopt);
    REQUIRE(clock_reads == 2);
    writer.end(first, "host", 5u, "CLOSED", 410);
    REQUIRE(clock_reads == 2);
    writer.end(second, "host", 6u, "CLOSED", std::nullopt);
    REQUIRE(clock_reads == 2);
    REQUIRE(file.writes.size() == 2);
    REQUIRE(counters.value(Counter::RecordWriteFailed) == 1);
    REQUIRE(log.with_event("record.write_failed").size() == 1);
    REQUIRE(writer.failed());

    // 시도 번호는 줄이 남지 않아도 오른다.
    REQUIRE(first == 1);
    REQUIRE(second == 2);
    REQUIRE(third == 3);
}

TEST_CASE("record: an end line reads the clock once when it is written", "[record]") {
    FakeFile file;
    Counters counters;
    std::size_t clock_reads = 0;
    RecordWriter writer(file.fn(), [&clock_reads] { ++clock_reads; return kT2; }, counters);
    writer.end(1, "player", 2u, "CLOSED", 10);
    REQUIRE(clock_reads == 1);
    REQUIRE(file.writes.size() == 1);
}

TEST_CASE("record: a failed first write stops the file at once", "[record]") {
    sangtachi::testing::LogCapture log;
    FakeFile file;
    file.fail_from = 0;
    Counters counters;
    std::size_t clock_reads = 0;
    RecordWriter writer(file.fn(), [&clock_reads] { ++clock_reads; return kT1; }, counters);

    writer.end(4, "player", 1u, "TUNNEL_DROPPED", std::nullopt);
    REQUIRE(writer.failed());
    REQUIRE(clock_reads == 1);
    REQUIRE(log.with_event("record.write_failed") ==
            std::vector<std::string>{"WARN record.write_failed kind=end attempt=4"});
    REQUIRE(writer.establish("player", std::nullopt, "OK", std::nullopt, 1) == 1);
    REQUIRE(clock_reads == 1);
    REQUIRE(file.writes.size() == 1);
    REQUIRE(counters.value(Counter::RecordWriteFailed) == 1);
}
