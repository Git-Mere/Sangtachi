#include "sangtachi/record.hpp"

#include "sangtachi/log.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace sangtachi::record {
namespace {

// value 를 width 자리 10진수로. 모자란 자리는 0 으로 채운다.
void append_padded(std::string& out, std::uint64_t value, std::size_t width) {
    const std::string digits = std::to_string(value);
    if (digits.size() < width) {
        out.append(width - digits.size(), '0');
    }
    out += digits;
}

void append_field(std::string& out, std::string_view name, std::string_view value) {
    if (!out.empty()) {
        out += ' ';
    }
    out += name;
    out += '=';
    out += value;
}

// 값이 없으면 `-` 다 (9장 줄 형식). 0 은 측정된 값이다.
template <typename T>
std::string or_dash(const std::optional<T>& value) {
    return value ? std::to_string(*value) : std::string("-");
}

std::string_view to_token(LineKind kind) noexcept {
    return kind == LineKind::kEstablish ? "establish" : "end";
}

struct UtcParts {
    std::uint64_t year, month, day, hour, minute, second, millis;
};

UtcParts split_utc(std::uint64_t unix_ms) {
    using namespace std::chrono;
    const sys_time<milliseconds> tp{milliseconds{static_cast<std::int64_t>(unix_ms)}};
    const auto day = floor<days>(tp);
    const year_month_day ymd{day};
    const hh_mm_ss<milliseconds> hms{tp - day};
    return UtcParts{
        static_cast<std::uint64_t>(static_cast<int>(ymd.year())),
        static_cast<unsigned>(ymd.month()),
        static_cast<unsigned>(ymd.day()),
        static_cast<std::uint64_t>(hms.hours().count()),
        static_cast<std::uint64_t>(hms.minutes().count()),
        static_cast<std::uint64_t>(hms.seconds().count()),
        static_cast<std::uint64_t>(hms.subseconds().count()),
    };
}

}  // namespace

std::string format_timestamp(std::uint64_t unix_ms) {
    const UtcParts t = split_utc(unix_ms);
    std::string out;
    out.reserve(24);
    append_padded(out, t.year, 4);
    out += '-';
    append_padded(out, t.month, 2);
    out += '-';
    append_padded(out, t.day, 2);
    out += 'T';
    append_padded(out, t.hour, 2);
    out += ':';
    append_padded(out, t.minute, 2);
    out += ':';
    append_padded(out, t.second, 2);
    out += '.';
    append_padded(out, t.millis, 3);
    out += 'Z';
    return out;
}

std::string format_line(const RecordLine& line, std::uint64_t unix_ms) {
    std::string out;
    out.reserve(160);
    append_field(out, "v", "1");
    append_field(out, "kind", to_token(line.kind));
    append_field(out, "ts", format_timestamp(unix_ms));
    append_field(out, "attempt", std::to_string(line.attempt));
    append_field(out, "role", line.role);
    append_field(out, "peer", or_dash(line.peer));
    append_field(out, "result", line.result);
    append_field(out, "rtt_us", or_dash(line.rtt_us));
    append_field(out, "setup_ms", or_dash(line.setup_ms));
    out += '\n';
    return out;
}

std::string file_name(std::uint64_t start_unix_ms, std::uint32_t pid) {
    const UtcParts t = split_utc(start_unix_ms);
    std::string out;
    out.reserve(32);
    append_padded(out, t.year, 4);
    append_padded(out, t.month, 2);
    append_padded(out, t.day, 2);
    out += 'T';
    append_padded(out, t.hour, 2);
    append_padded(out, t.minute, 2);
    append_padded(out, t.second, 2);
    out += "Z-";
    out += std::to_string(pid);
    out += ".log";
    return out;
}

RecordWriter::RecordWriter(WriteFn write, WallClockFn wall_clock, Counters& counters)
    : write_(std::move(write)), wall_clock_(std::move(wall_clock)), counters_(counters) {}

std::uint64_t RecordWriter::establish(std::string_view role, std::optional<std::uint32_t> peer,
                                      std::string_view result,
                                      std::optional<std::uint64_t> rtt_us,
                                      std::optional<std::uint64_t> setup_ms) {
    const std::uint64_t attempt = ++last_attempt_;
    write_line(RecordLine{LineKind::kEstablish, attempt, role, peer, result, rtt_us, setup_ms});
    return attempt;
}

void RecordWriter::end(std::uint64_t attempt, std::string_view role,
                       std::optional<std::uint32_t> peer, std::string_view result,
                       std::optional<std::uint64_t> rtt_us) {
    write_line(RecordLine{LineKind::kEnd, attempt, role, peer, result, rtt_us, std::nullopt});
}

void RecordWriter::write_line(const RecordLine& line) {
    // 줄이 반만 남은 자리에 다음 줄을 이으면 두 줄이 한 줄로 붙는다 (9장 쓰기와 실패).
    if (failed_) {
        return;
    }
    if (write_(format_line(line, wall_clock_()))) {
        return;
    }
    failed_ = true;
    counters_.increment(Counter::RecordWriteFailed);
    const LogField fields[] = {
        field("kind", to_token(line.kind)),
        field("attempt", line.attempt),
    };
    emit(LogLevel::Warn, "record.write_failed", fields);
}

}  // namespace sangtachi::record
