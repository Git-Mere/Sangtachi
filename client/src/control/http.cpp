#include "sangtachi/control/http.hpp"

#include "sangtachi/control/constants.hpp"
#include "sangtachi/control/json.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace sangtachi::control::http {

std::string_view op_name(Op op) noexcept {
    switch (op) {
        case Op::kCreateRoom:        return "create_room";
        case Op::kJoinRoom:          return "join_room";
        case Op::kRegisterCandidate: return "register_candidate";
        case Op::kGetPeers:          return "get_peers";
        case Op::kHostReport:        return "host_report";
    }
    return "unknown_op";
}

namespace {

constexpr std::size_t kMaxHostNameBytes = 253;  // args.cpp 의 이름 상한
constexpr std::string_view kCrlf = "\r\n";
constexpr std::string_view kHeadEnd = "\r\n\r\n";

bool is_visible_ascii(char c) noexcept {
    return c >= '!' && c <= '~';
}

char to_lower_ascii(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool equals_ignore_case(std::string_view a, std::string_view lower) noexcept {
    if (a.size() != lower.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (to_lower_ascii(a[i]) != lower[i]) {
            return false;
        }
    }
    return true;
}

bool is_digit(char c) noexcept {
    return c >= '0' && c <= '9';
}

// 3.5 의 1. `HTTP/1.1 ` + 숫자 셋, 그 뒤는 공백이거나 줄 끝.
bool status_line_ok(std::string_view line) noexcept {
    constexpr std::string_view kPrefix = "HTTP/1.1 ";
    if (line.substr(0, kPrefix.size()) != kPrefix) {
        return false;
    }
    const std::string_view rest = line.substr(kPrefix.size());
    if (rest.size() < 3 || !is_digit(rest[0]) || !is_digit(rest[1]) || !is_digit(rest[2])) {
        return false;
    }
    return rest.size() == 3 || rest[3] == ' ';
}

// 3.3 `Content-Length` 값. 콜론 뒤 공백 하나를 떼고 ^[0-9]+$. 상한을 넘으면 더 세지 않는다.
// 정수 변환 함수에 맡기지 않는다 (3.3 표).
struct LengthValue {
    bool syntax_ok = false;
    bool over_limit = false;
    std::size_t value = 0;
};

LengthValue parse_length(std::string_view raw) noexcept {
    std::string_view v = raw;
    if (!v.empty() && v.front() == ' ') {
        v.remove_prefix(1);
    }
    LengthValue out;
    if (v.empty()) {
        return out;
    }
    for (const char c : v) {
        if (!is_digit(c)) {
            return out;
        }
        if (!out.over_limit) {
            out.value = out.value * 10 + static_cast<std::size_t>(c - '0');
            if (out.value > kMaxBodyBytes) {
                out.over_limit = true;
            }
        }
    }
    out.syntax_ok = true;
    return out;
}

Frame invalid(ResponseError e) noexcept {
    Frame f;
    f.state = FrameState::kInvalid;
    f.error = e;
    return f;
}

}  // namespace

std::optional<std::string> host_header(std::string_view host, std::uint16_t port) {
    if (host.empty() || host.size() > kMaxHostNameBytes || port == 0) {
        return std::nullopt;
    }
    for (const char c : host) {
        if (!is_visible_ascii(c) || c == ':') {
            return std::nullopt;
        }
    }
    std::string out(host);
    out.push_back(':');
    out.append(std::to_string(port));
    return out;
}

std::optional<std::string> build_request(std::string_view host, Op op, std::string_view body) {
    if (host.empty() || host.size() > kMaxHostHeaderBytes) {
        return std::nullopt;
    }
    for (const char c : host) {
        if (!is_visible_ascii(c)) {
            return std::nullopt;
        }
    }
    if (body.size() > kMaxBodyBytes) {
        return std::nullopt;
    }
    std::string out;
    out.reserve(160 + host.size() + body.size());
    out.append("POST /v1/");
    out.append(op_name(op));
    out.append(" HTTP/1.1\r\n");
    out.append("Host: ");
    out.append(host);
    out.append(kCrlf);
    out.append("Content-Type: application/json\r\n");
    out.append("Content-Length: ");
    out.append(std::to_string(body.size()));
    out.append(kCrlf);
    out.append("Connection: close\r\n");
    out.append(kCrlf);
    out.append(body);
    return out;
}

std::string_view to_token(ResponseError error) noexcept {
    switch (error) {
        case ResponseError::kNone:                   return "none";
        case ResponseError::kHeaderIncomplete:       return "header_incomplete";
        case ResponseError::kHeaderTooLarge:         return "header_too_large";
        case ResponseError::kBadStatusLine:          return "bad_status_line";
        case ResponseError::kBadHeaderLine:          return "bad_header_line";
        case ResponseError::kTransferEncoding:       return "transfer_encoding";
        case ResponseError::kMissingContentLength:   return "missing_content_length";
        case ResponseError::kDuplicateContentLength: return "duplicate_content_length";
        case ResponseError::kBadContentLength:       return "bad_content_length";
        case ResponseError::kBodyTooLarge:           return "body_too_large";
        case ResponseError::kBodyIncomplete:         return "body_incomplete";
        case ResponseError::kBodyNotJson:            return "body_not_json";
        case ResponseError::kBodyNotObject:          return "body_not_object";
    }
    return "bad_status_line";
}

Frame frame_response(std::string_view received) {
    // 1. 빈 줄을 상한 안에서 찾는다. 찾은 자리 + 4 가 상한 이하여야 한다.
    const std::string_view window = received.substr(0, kMaxHeaderBytes);
    const std::size_t end = window.find(kHeadEnd);
    if (end == std::string_view::npos) {
        if (received.size() >= kMaxHeaderBytes) {
            return invalid(ResponseError::kHeaderTooLarge);
        }
        return Frame{};  // kNeedMore, needed 0
    }
    const std::size_t head_size = end + kHeadEnd.size();
    // 줄들. 마지막 CRLF CRLF 의 앞 CRLF 까지가 줄이다.
    const std::string_view lines = received.substr(0, end);

    // 2. 머리 안의 CR 과 LF 는 전부 CRLF 의 일부여야 한다.
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (lines[i] == '\r') {
            if (i + 1 >= lines.size() || lines[i + 1] != '\n') {
                return invalid(ResponseError::kBadHeaderLine);
            }
            ++i;
        } else if (lines[i] == '\n') {
            return invalid(ResponseError::kBadHeaderLine);
        }
    }

    // 3. 상태 줄.
    std::size_t line_end = lines.find(kCrlf);
    const std::string_view status = lines.substr(0, line_end);
    if (!status_line_ok(status)) {
        return invalid(ResponseError::kBadStatusLine);
    }

    // 4~7. 헤더 줄.
    std::size_t content_length_count = 0;
    std::string_view content_length_raw;
    bool transfer_encoding = false;
    std::size_t pos = (line_end == std::string_view::npos) ? lines.size() : line_end + 2;
    while (pos < lines.size()) {
        line_end = lines.find(kCrlf, pos);
        const std::size_t stop = (line_end == std::string_view::npos) ? lines.size() : line_end;
        const std::string_view line = lines.substr(pos, stop - pos);
        pos = stop + 2;

        if (line.empty() || line.front() == ' ' || line.front() == '\t') {
            return invalid(ResponseError::kBadHeaderLine);  // 접기 (3.3 obs-fold)
        }
        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0) {
            return invalid(ResponseError::kBadHeaderLine);
        }
        const std::string_view name = line.substr(0, colon);
        if (name.back() == ' ' || name.back() == '\t') {
            return invalid(ResponseError::kBadHeaderLine);  // RFC 9112 5.1
        }
        if (equals_ignore_case(name, "content-length")) {
            ++content_length_count;
            content_length_raw = line.substr(colon + 1);
        } else if (equals_ignore_case(name, "transfer-encoding")) {
            transfer_encoding = true;
        }
    }
    if (transfer_encoding) {
        return invalid(ResponseError::kTransferEncoding);
    }
    if (content_length_count == 0) {
        return invalid(ResponseError::kMissingContentLength);
    }
    if (content_length_count > 1) {
        return invalid(ResponseError::kDuplicateContentLength);
    }

    // 8~9. 값.
    const LengthValue length = parse_length(content_length_raw);
    if (!length.syntax_ok) {
        return invalid(ResponseError::kBadContentLength);
    }
    if (length.over_limit) {
        return invalid(ResponseError::kBodyTooLarge);
    }

    Frame f;
    f.head_size = head_size;
    f.body_size = length.value;
    const std::size_t total = head_size + length.value;
    if (received.size() >= total) {
        f.state = FrameState::kComplete;
    } else {
        f.state = FrameState::kNeedMore;
        f.needed = total - received.size();
    }
    return f;
}

Response parse_response(std::string_view received) {
    Response out;
    const Frame f = frame_response(received);
    if (f.state == FrameState::kInvalid) {
        out.error = f.error;
        return out;
    }
    if (f.state == FrameState::kNeedMore) {
        out.error = (f.head_size == 0) ? ResponseError::kHeaderIncomplete
                                       : ResponseError::kBodyIncomplete;
        return out;
    }
    // Content-Length 뒤에 더 온 바이트는 보지 않는다.
    auto parsed = json::parse(received.substr(f.head_size, f.body_size));
    if (!parsed.value) {
        out.error = ResponseError::kBodyNotJson;
        return out;
    }
    if (parsed.value->kind() != json::Kind::kObject) {
        out.error = ResponseError::kBodyNotObject;
        return out;
    }
    out.body = std::move(parsed.value);
    return out;
}

}  // namespace sangtachi::control::http
