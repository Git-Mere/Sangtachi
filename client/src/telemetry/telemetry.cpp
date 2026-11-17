#include "sangtachi/telemetry/telemetry.hpp"

#include "sangtachi/control/json.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace sangtachi::telemetry {
namespace {

using control::json::Value;

Value integer(std::uint64_t v) {
    return Value::integer(static_cast<std::int64_t>(v));
}

template <class T>
Value integer_or_null(const std::optional<T>& v) {
    return v ? integer(static_cast<std::uint64_t>(*v)) : Value::null();
}

Value bool_or_null(const std::optional<bool>& v) {
    return v ? Value::boolean(*v) : Value::null();
}

Value text(std::string_view s) {
    return Value::string(std::string(s));
}

void add(Value& object, std::string_view key, Value value) {
    (void)object.insert(std::string(key), std::move(value));
}

void add_interval(Value& o, const IntervalValues& v) {
    add(o, "interval_ms", integer(v.interval_ms));
    Value rtt = Value::array();
    for (const std::uint64_t sample : v.rtt_us) {
        (void)rtt.push_back(integer(sample));
    }
    add(o, "rtt_us", std::move(rtt));
    add(o, "rtt_overflow", integer(v.rtt_overflow));
    add(o, "jitter_us", integer_or_null(v.jitter_us));
    add(o, "expected", integer(v.expected));
    add(o, "accepted", integer(v.accepted));
    add(o, "bytes_out", integer(v.bytes_out));
    add(o, "bytes_in", integer(v.bytes_in));
}

}  // namespace

std::string_view to_token(Kind kind) noexcept {
    switch (kind) {
        case Kind::kEstablish: return "establish";
        case Kind::kInterval:  return "interval";
        case Kind::kEnd:       return "end";
    }
    return "unknown";
}

std::string to_json(const Record& r, const Stamp& stamp) {
    Value o = Value::object();
    add(o, "v", integer(1));
    add(o, "kind", text(to_token(r.kind)));
    add(o, "instance_id", text(stamp.instance_id));
    add(o, "attempt", integer(r.attempt));
    add(o, "seq", integer(r.seq));
    add(o, "role", text(r.role));
    add(o, "room_id", r.room_id.empty() ? Value::null() : text(r.room_id));
    add(o, "peer_id", integer_or_null(r.peer_id));
    add(o, "peer", integer_or_null(r.peer));
    add(o, "os", text(stamp.os));
    add(o, "ui", text(stamp.ui));
    add(o, "build_kind", text(stamp.build_kind));
    switch (r.kind) {
        case Kind::kEstablish:
            add(o, "result", text(r.result));
            add(o, "setup_ms", integer_or_null(r.setup_ms));
            add(o, "local_differs_public", bool_or_null(r.local_differs_public));
            add(o, "stun_same_endpoint", bool_or_null(r.stun_same_endpoint));
            add(o, "same_lan", bool_or_null(r.same_lan));
            break;
        case Kind::kInterval:
            add_interval(o, r.interval);
            break;
        case Kind::kEnd:
            add_interval(o, r.interval);
            add(o, "result", text(r.result));
            add(o, "duration_ms", integer(r.totals.duration_ms));
            add(o, "total_expected", integer(r.totals.total_expected));
            add(o, "total_accepted", integer(r.totals.total_accepted));
            add(o, "total_bytes_out", integer(r.totals.total_bytes_out));
            add(o, "total_bytes_in", integer(r.totals.total_bytes_in));
            add(o, "total_tx_err_send", integer(r.totals.total_tx_err_send));
            break;
    }
    // 모든 값이 ASCII 이고 깊이가 1 이라 serialize 가 거부할 자리가 없다.
    return control::json::serialize(o).value_or(std::string());
}

std::optional<std::string> make_instance_id(const RandomFn& random) {
    std::array<std::byte, 16> bytes{};
    if (!random || !random(bytes)) {
        return std::nullopt;
    }
    constexpr std::string_view kHex = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const std::byte b : bytes) {
        const auto v = std::to_integer<unsigned>(b);
        out.push_back(kHex[v >> 4]);
        out.push_back(kHex[v & 0x0F]);
    }
    return out;
}

void SessionMeter::add_rtt(std::uint64_t rtt_us) {
    // 9장 지터. 두 번째 표본부터 J = J + (|R_i - R_(i-1)| - J) / 16. 처음 값은 0 이다.
    if (samples_ > 0) {
        const std::uint64_t diff = rtt_us > last_rtt_us_ ? rtt_us - last_rtt_us_ : last_rtt_us_ - rtt_us;
        jitter_ += (static_cast<double>(diff) - jitter_) / 16.0;
    }
    ++samples_;
    last_rtt_us_ = rtt_us;
    if (rtt_.size() < kMaxRttSamples) {
        rtt_.push_back(rtt_us);
    } else {
        ++rtt_overflow_;
    }
}

void SessionMeter::start(Millis now, std::uint64_t expected, std::uint64_t accepted) {
    if (started_at_) {
        return;
    }
    started_at_ = now;
    mark_ms_ = now;
    mark_expected_ = expected;
    mark_accepted_ = accepted;
    mark_bytes_out_ = bytes_out_;
    mark_bytes_in_ = bytes_in_;
    rtt_.clear();
    rtt_overflow_ = 0;
}

IntervalValues SessionMeter::take_interval(Millis now, std::uint64_t expected, std::uint64_t accepted) {
    if (!started_at_) {
        return IntervalValues{};
    }
    IntervalValues v;
    v.interval_ms = now >= mark_ms_ ? now - mark_ms_ : 0;
    v.rtt_us = std::move(rtt_);
    v.rtt_overflow = rtt_overflow_;
    v.jitter_us = jitter_us();
    // 4.5 세션 값은 줄지 않는다 (6.3 "둘 다 줄지 않으므로 0 이상이다").
    v.expected = expected - mark_expected_;
    v.accepted = accepted - mark_accepted_;
    v.bytes_out = bytes_out_ - mark_bytes_out_;
    v.bytes_in = bytes_in_ - mark_bytes_in_;

    mark_ms_ = now;
    mark_expected_ = expected;
    mark_accepted_ = accepted;
    mark_bytes_out_ = bytes_out_;
    mark_bytes_in_ = bytes_in_;
    rtt_.clear();
    rtt_overflow_ = 0;
    return v;
}

std::optional<std::uint64_t> SessionMeter::jitter_us() const noexcept {
    if (samples_ < 2) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(std::llround(jitter_));
}

}  // namespace sangtachi::telemetry
