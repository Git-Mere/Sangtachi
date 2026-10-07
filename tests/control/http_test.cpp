#include "sangtachi/control/http.hpp"

#include "sangtachi/control/constants.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// 시험 케이스 이름은 ASCII 로만 적는다. 이유는 network/wsa_test.cpp 머리에 있다.
//
// 응답 판정의 규칙은 client/include/sangtachi/control/http.hpp 머리의 표다. 아래 표의 행이
// 그 번호를 하나씩 건드린다.

using sangtachi::control::kMaxBodyBytes;
using sangtachi::control::kMaxHeaderBytes;
using sangtachi::control::http::build_request;
using sangtachi::control::http::frame_response;
using sangtachi::control::http::host_header;
using sangtachi::control::http::kMaxHostHeaderBytes;
using sangtachi::control::http::FrameState;
using sangtachi::control::http::Op;
using sangtachi::control::http::op_name;
using sangtachi::control::http::parse_response;
using sangtachi::control::http::ResponseError;
using sangtachi::control::http::to_token;

namespace {

constexpr std::string_view kBody = "{\"ok\":true}";

// 서버(control-server/controlplane/server.py 의 response)가 보내는 모양 그대로다.
std::string server_response(std::string_view body, std::string_view status = "200 OK") {
    return "HTTP/1.1 " + std::string(status) + "\r\nContent-Type: application/json\r\n" +
           "Content-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" +
           std::string(body);
}

// 머리 줄들 + 빈 줄 + 본문.
std::string with_head(std::string_view head_lines, std::string_view body) {
    return std::string(head_lines) + "\r\n\r\n" + std::string(body);
}

struct Row {
    std::string input;
    ResponseError error;
    std::string_view why;
};

}  // namespace

TEST_CASE("control_http: build_request produces the 3.3 bytes", "[control][http]") {
    const auto req = build_request("cp.example:8000", Op::kGetPeers, kBody);
    REQUIRE(req);
    REQUIRE(*req ==
            "POST /v1/get_peers HTTP/1.1\r\n"
            "Host: cp.example:8000\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: 11\r\n"
            "Connection: close\r\n"
            "\r\n"
            "{\"ok\":true}");
}

TEST_CASE("control_http: host_header joins name or ipv4 and port", "[control][http]") {
    // 3.3 Host 행: --server 의 이름 또는 IPv4 와 포트를 ':' 로 잇는다.
    CHECK(host_header("cp.example", 8000) == "cp.example:8000");
    CHECK(host_header("203.0.113.7", 8000) == "203.0.113.7:8000");
    CHECK(host_header("a", 1) == "a:1");
    const auto longest = host_header(std::string(253, 'a'), 65535);
    REQUIRE(longest);
    CHECK(longest->size() == kMaxHostHeaderBytes);
    // 그 값은 build_request 가 받는다.
    CHECK(build_request(*longest, Op::kGetPeers, "{}").has_value());

    struct Case {
        std::string host;
        std::uint16_t port;
        std::string_view why;
    };
    const Case rejected[] = {
        {"", 8000, "empty host"},
        {std::string(254, 'a'), 8000, "over the 253-byte name limit"},
        {"cp.example", 0, "port 0"},
        {"cp.example:8000", 8000, "already has a port"},
        {"::1", 8000, "ipv6 is out of scope"},
        {"a b", 8000, "space"},
        {"a\r\nX: 1", 8000, "CRLF injection"},
        {"\xC3\xA9", 8000, "non-ascii"},
    };
    for (const auto& c : rejected) {
        INFO("why=" << c.why);
        CHECK_FALSE(host_header(c.host, c.port).has_value());
    }
}

TEST_CASE("control_http: op names are the five of chapter 4", "[control][http]") {
    CHECK(op_name(Op::kCreateRoom) == "create_room");
    CHECK(op_name(Op::kJoinRoom) == "join_room");
    CHECK(op_name(Op::kRegisterCandidate) == "register_candidate");
    CHECK(op_name(Op::kGetPeers) == "get_peers");
    CHECK(op_name(Op::kHostReport) == "host_report");
}

TEST_CASE("control_http: build_request rejects header injection and oversize", "[control][http]") {
    struct Case {
        std::string host;
        std::string body;
        bool accepted;
        std::string_view why;
    };
    const Case cases[] = {
        {"10.0.0.1", "{}", true, "ipv4 literal"},
        {std::string(253, 'a') + ":65535", "{}", true, "longest host_header value"},
        {std::string(254, 'a') + ":65535", "{}", false, "one byte over kMaxHostHeaderBytes"},
        {"", "{}", false, "empty host"},
        {"a\r\nX-Evil: 1", "{}", false, "CRLF injection"},
        {"a\nb", "{}", false, "bare LF"},
        {"a b", "{}", false, "space"},
        {"a\tb", "{}", false, "tab"},
        {"a\x7F", "{}", false, "DEL"},
        {"\xC3\xA9", "{}", false, "non-ascii"},
        {"a", std::string(kMaxBodyBytes, 'x'), true, "body at MAX_BODY_BYTES"},
        {"a", std::string(kMaxBodyBytes + 1, 'x'), false, "body over MAX_BODY_BYTES"},
        {"a", "", true, "empty body is the caller's business"},
    };
    for (const auto& c : cases) {
        INFO("why=" << c.why);
        CHECK(build_request(c.host, Op::kCreateRoom, c.body).has_value() == c.accepted);
    }
}

TEST_CASE("control_http: parse_response case table", "[control][http]") {
    const std::string over_limit_cl = std::to_string(kMaxBodyBytes + 1);
    const Row rows[] = {
        // 받는다
        {server_response(kBody), ResponseError::kNone, "what the server sends"},
        {server_response("{\"ok\":false,\"error\":\"internal\",\"message\":\"x\"}", "500 Internal Server Error"),
         ResponseError::kNone, "status code does not decide; the body does"},
        {with_head("HTTP/1.1 200\r\nContent-Length: 11", kBody), ResponseError::kNone, "no reason phrase"},
        {with_head("HTTP/1.1 200 OK\r\ncontent-length: 11", kBody), ResponseError::kNone, "header name case"},
        {with_head("HTTP/1.1 200 OK\r\nCONTENT-LENGTH:11", kBody), ResponseError::kNone, "no space after colon"},
        {with_head("HTTP/1.1 200 OK\r\nContent-Length: 011", kBody), ResponseError::kNone, "leading zeros are digits"},
        {with_head("HTTP/1.1 200 OK\r\nX-A: 1\r\nX-A: 2\r\nContent-Length: 11", kBody), ResponseError::kNone,
         "other headers twice are ignored"},
        {with_head("HTTP/1.1 999 Whatever it says\r\nContent-Length: 11", kBody), ResponseError::kNone,
         "any three digits and any reason"},
        {server_response(kBody) + "trailing bytes", ResponseError::kNone, "bytes after Content-Length are not read"},

        // 1. 머리
        {"HTTP/1.1 200 OK\r\nContent-Length: 11\r\n", ResponseError::kHeaderIncomplete, "no blank line yet"},
        {"", ResponseError::kHeaderIncomplete, "nothing received"},
        {"HTTP/1.1 200 OK\r\nX: " + std::string(kMaxHeaderBytes, 'a') + "\r\n\r\n{}",
         ResponseError::kHeaderTooLarge, "head over MAX_HEADER_BYTES"},

        // 2. 줄 끝
        {with_head("HTTP/1.1 200 OK\nContent-Length: 11", kBody), ResponseError::kBadHeaderLine, "bare LF"},
        {with_head("HTTP/1.1 200 OK\r\nX: a\rb\r\nContent-Length: 11", kBody), ResponseError::kBadHeaderLine,
         "bare CR"},

        // 3. 상태 줄 (3.5 의 1)
        {with_head("HTTP/1.0 200 OK\r\nContent-Length: 11", kBody), ResponseError::kBadStatusLine, "HTTP/1.0"},
        {with_head("http/1.1 200 OK\r\nContent-Length: 11", kBody), ResponseError::kBadStatusLine, "lower case"},
        {with_head("HTTP/1.1  200 OK\r\nContent-Length: 11", kBody), ResponseError::kBadStatusLine, "two spaces"},
        {with_head("HTTP/1.1 20 OK\r\nContent-Length: 11", kBody), ResponseError::kBadStatusLine, "two digits"},
        {with_head("HTTP/1.1 2000 OK\r\nContent-Length: 11", kBody), ResponseError::kBadStatusLine, "four digits"},
        {with_head("HTTP/1.1 2x0 OK\r\nContent-Length: 11", kBody), ResponseError::kBadStatusLine, "non-digit"},
        {with_head("HTTP/1.1 200\tOK\r\nContent-Length: 11", kBody), ResponseError::kBadStatusLine, "tab after code"},
        {with_head(" HTTP/1.1 200 OK\r\nContent-Length: 11", kBody), ResponseError::kBadStatusLine, "leading space"},
        {with_head("Content-Length: 11", kBody), ResponseError::kBadStatusLine, "no status line"},

        // 4. 헤더 줄 문법
        {with_head("HTTP/1.1 200 OK\r\nX: 1\r\n Content-Length: 11", kBody), ResponseError::kBadHeaderLine, "obs-fold space"},
        {with_head("HTTP/1.1 200 OK\r\n\tContent-Length: 11", kBody), ResponseError::kBadHeaderLine, "obs-fold tab"},
        {with_head("HTTP/1.1 200 OK\r\nContent-Length : 11", kBody), ResponseError::kBadHeaderLine, "space before colon"},
        {with_head("HTTP/1.1 200 OK\r\nContent-Length\t: 11", kBody), ResponseError::kBadHeaderLine, "tab before colon"},
        {with_head("HTTP/1.1 200 OK\r\nno colon here\r\nContent-Length: 11", kBody), ResponseError::kBadHeaderLine,
         "line without colon"},
        {with_head("HTTP/1.1 200 OK\r\n: 11\r\nContent-Length: 11", kBody), ResponseError::kBadHeaderLine, "empty name"},

        // 5. Transfer-Encoding
        {with_head("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Length: 11", kBody),
         ResponseError::kTransferEncoding, "TE with CL"},
        {with_head("HTTP/1.1 200 OK\r\ntransfer-encoding: identity\r\nContent-Length: 11", kBody),
         ResponseError::kTransferEncoding, "any TE value"},

        // 6~7. Content-Length 하나 (3.5 의 2)
        {with_head("HTTP/1.1 200 OK\r\nContent-Type: application/json", kBody),
         ResponseError::kMissingContentLength, "no Content-Length"},
        {with_head("HTTP/1.1 200 OK\r\nContent-Length: 11\r\nContent-Length: 11", kBody),
         ResponseError::kDuplicateContentLength, "same value twice"},
        {with_head("HTTP/1.1 200 OK\r\nContent-Length: 11\r\ncontent-length: 5", kBody),
         ResponseError::kDuplicateContentLength, "different values, different case"},

        // 8. 값 형식 (3.3)
        {with_head("HTTP/1.1 200 OK\r\nContent-Length: +11", kBody), ResponseError::kBadContentLength, "plus sign"},
        {with_head("HTTP/1.1 200 OK\r\nContent-Length: -1", kBody), ResponseError::kBadContentLength, "negative"},
        {with_head("HTTP/1.1 200 OK\r\nContent-Length: 1_1", kBody), ResponseError::kBadContentLength, "underscore"},
        {with_head("HTTP/1.1 200 OK\r\nContent-Length:  11", kBody), ResponseError::kBadContentLength,
         "second space is part of the value"},
        {with_head("HTTP/1.1 200 OK\r\nContent-Length: 11 ", kBody), ResponseError::kBadContentLength, "trailing space"},
        {with_head("HTTP/1.1 200 OK\r\nContent-Length: ", kBody), ResponseError::kBadContentLength, "empty"},
        {with_head("HTTP/1.1 200 OK\r\nContent-Length: 0x0B", kBody), ResponseError::kBadContentLength, "hex"},
        {with_head("HTTP/1.1 200 OK\r\nContent-Length: \xEF\xBC\x91\xEF\xBC\x91", kBody),
         ResponseError::kBadContentLength, "full-width digits"},
        {with_head("HTTP/1.1 200 OK\r\nContent-Length: 11, 11", kBody), ResponseError::kBadContentLength, "list form"},

        // 9. 상한 (3.5 의 2)
        {with_head("HTTP/1.1 200 OK\r\nContent-Length: " + over_limit_cl, kBody), ResponseError::kBodyTooLarge,
         "MAX_BODY_BYTES + 1"},
        {with_head("HTTP/1.1 200 OK\r\nContent-Length: 99999999999999999999999999", kBody),
         ResponseError::kBodyTooLarge, "would overflow if counted"},

        // 10~12. 본문 (3.5 의 3)
        {with_head("HTTP/1.1 200 OK\r\nContent-Length: 12", kBody), ResponseError::kBodyIncomplete,
         "one byte short; peer closed"},
        {with_head("HTTP/1.1 200 OK\r\nContent-Length: 0", ""), ResponseError::kBodyNotJson, "empty body"},
        {server_response("{\"ok\":tru}"), ResponseError::kBodyNotJson, "broken json"},
        {server_response("{\"ok\":true,\"ok\":false}"), ResponseError::kBodyNotJson, "duplicate key"},
        {server_response("[{\"ok\":true}]"), ResponseError::kBodyNotObject, "array"},
        {server_response("\"ok\""), ResponseError::kBodyNotObject, "string"},
        {with_head("HTTP/1.1 200 OK\r\nContent-Length: 10", kBody), ResponseError::kBodyNotJson,
         "Content-Length cuts the body short"},
    };
    for (const auto& row : rows) {
        INFO("why=" << row.why);
        const auto got = parse_response(row.input);
        CHECK(got.error == row.error);
        CHECK(got.body.has_value() == (row.error == ResponseError::kNone));
    }
}

TEST_CASE("control_http: the body at MAX_BODY_BYTES is accepted", "[control][http]") {
    std::string body = "{\"ok\":true,\"pad\":\"";
    body += std::string(kMaxBodyBytes - body.size() - 2, 'a');
    body += "\"}";
    REQUIRE(body.size() == kMaxBodyBytes);
    const auto got = parse_response(server_response(body));
    REQUIRE(got.ok());
    REQUIRE(got.body->find("ok")->as_bool() == true);
}

TEST_CASE("control_http: the head at MAX_HEADER_BYTES is accepted", "[control][http]") {
    // 상태 줄 첫 바이트부터 빈 줄의 CRLF 까지가 정확히 kMaxHeaderBytes.
    const std::string prefix = "HTTP/1.1 200 OK\r\nContent-Length: 11\r\nX: ";
    const std::string head = prefix + std::string(kMaxHeaderBytes - prefix.size() - 4, 'a') + "\r\n\r\n";
    REQUIRE(head.size() == kMaxHeaderBytes);
    CHECK(parse_response(head + std::string(kBody)).error == ResponseError::kNone);

    const std::string longer = prefix + std::string(kMaxHeaderBytes - prefix.size() - 3, 'a') + "\r\n\r\n";
    CHECK(parse_response(longer + std::string(kBody)).error == ResponseError::kHeaderTooLarge);
}

TEST_CASE("control_http: frame tells the reader where to stop", "[control][http]") {
    const std::string full = server_response(kBody);
    const std::size_t head = full.size() - kBody.size();

    // 머리가 다 오기 전에는 needed 를 모른다.
    for (std::size_t n = 0; n < head; ++n) {
        INFO("received=" << n);
        const auto f = frame_response(std::string_view(full).substr(0, n));
        CHECK(f.state == FrameState::kNeedMore);
        CHECK(f.needed == 0);
    }
    // 머리가 오면 남은 본문 바이트 수를 안다. 한 바이트씩 나눠 와도 같다.
    for (std::size_t n = head; n < full.size(); ++n) {
        INFO("received=" << n);
        const auto f = frame_response(std::string_view(full).substr(0, n));
        CHECK(f.state == FrameState::kNeedMore);
        CHECK(f.head_size == head);
        CHECK(f.body_size == kBody.size());
        CHECK(f.needed == full.size() - n);
    }
    const auto done = frame_response(full);
    CHECK(done.state == FrameState::kComplete);
    CHECK(done.needed == 0);

    // EOF 를 기다리지 않는다. 더 온 바이트가 있어도 끝난 것이다.
    CHECK(frame_response(full + "xyz").state == FrameState::kComplete);
}

TEST_CASE("control_http: frame gives up early on a broken or oversized head", "[control][http]") {
    const auto big = frame_response(std::string(kMaxHeaderBytes, 'a'));
    CHECK(big.state == FrameState::kInvalid);
    CHECK(big.error == ResponseError::kHeaderTooLarge);

    const auto dup = frame_response(with_head("HTTP/1.1 200 OK\r\nContent-Length: 1\r\nContent-Length: 1", ""));
    CHECK(dup.state == FrameState::kInvalid);
    CHECK(dup.error == ResponseError::kDuplicateContentLength);

    // 상한을 넘는 Content-Length 는 본문을 기다리지 않는다.
    const auto huge = frame_response(with_head("HTTP/1.1 200 OK\r\nContent-Length: 5000", ""));
    CHECK(huge.state == FrameState::kInvalid);
    CHECK(huge.error == ResponseError::kBodyTooLarge);
}

TEST_CASE("control_http: response error tokens are fixed", "[control][http]") {
    CHECK(to_token(ResponseError::kNone) == "none");
    CHECK(to_token(ResponseError::kDuplicateContentLength) == "duplicate_content_length");
    CHECK(to_token(ResponseError::kBodyNotObject) == "body_not_object");
}
