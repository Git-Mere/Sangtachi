#include "sangtachi/telemetry/uploader.hpp"

#include "sangtachi/control/exchange.hpp"
#include "sangtachi/counters.hpp"
#include "sangtachi/network/wsa.hpp"
#include "sangtachi/platform/wait.hpp"

#include "../control/loopback_tcp.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// 시험 케이스 이름은 ASCII 로만 적는다. 이유는 network/wsa_test.cpp 머리에 있다.
//
// `[telemetry]` 를 루프백의 가짜 HTTP 수신기에 붙여 본다. 규칙의 출처는 concurrency.md 6장 텔레메트리 격리, 7장 종료,
// architecture.md 6.3 의 응답 검사와 실패 셈이다.

using sangtachi::Counter;
using sangtachi::Counters;
using sangtachi::control::ExchangeTimeouts;
using sangtachi::network::WsaContext;
using sangtachi::telemetry::Kind;
using sangtachi::telemetry::kQueueCapacity;
using sangtachi::telemetry::QueuedRecord;
using sangtachi::telemetry::Uploader;
using sangtachi_test::closed_port;
using sangtachi_test::http_reply;
using sangtachi_test::LoopbackServer;
using sangtachi_test::Script;

namespace {

class ShutdownEvent {
public:
    ShutdownEvent() : handle_(sangtachi::platform::create_event(sangtachi::platform::ResetMode::Manual)) {}
    ~ShutdownEvent() { sangtachi::platform::close_event(handle_); }
    ShutdownEvent(const ShutdownEvent&) = delete;
    ShutdownEvent& operator=(const ShutdownEvent&) = delete;
    [[nodiscard]] sangtachi::platform::WaitHandle get() const noexcept { return handle_; }
    void signal() const { REQUIRE(sangtachi::platform::signal_event(handle_)); }

private:
    sangtachi::platform::WaitHandle handle_;
};

ExchangeTimeouts short_timeouts() {
    ExchangeTimeouts t;
    t.connect_ms = 1000;
    t.io_ms = 500;
    return t;
}

std::unique_ptr<Uploader> start(std::uint16_t port, const ShutdownEvent& shutdown, Counters& counters) {
    auto uploader = Uploader::start("127.0.0.1", port, shutdown.get(), counters, short_timeouts());
    REQUIRE(uploader != nullptr);
    return uploader;
}

// concurrency.md 7장 종료의 순서대로 끝낸다. 종료 이벤트, 정리 완료, join.
void finish(Uploader& uploader, const ShutdownEvent& shutdown) {
    shutdown.signal();
    uploader.cleanup_done();
    REQUIRE(uploader.join_for(25'000));
}

}  // namespace

TEST_CASE("uploader: one connection per record carrying POST /v1/report", "[telemetry][uploader]") {
    const WsaContext wsa;
    LoopbackServer server({Script{http_reply(R"({"ok":true})")}, Script{http_reply(R"({"ok":true})")}});
    const ShutdownEvent shutdown;
    Counters counters;
    auto uploader = start(server.port(), shutdown, counters);

    REQUIRE(uploader->push(QueuedRecord{Kind::kEstablish, R"({"n":1})"}));
    REQUIRE(uploader->push(QueuedRecord{Kind::kEnd, R"({"n":2})"}));
    finish(*uploader, shutdown);

    server.stop();
    const auto seen = server.requests();
    REQUIRE(seen.size() == 2);
    const std::string head = "POST /v1/report HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(server.port()) + "\r\n";
    for (std::size_t i = 0; i < seen.size(); ++i) {
        CHECK(seen[i].rfind(head, 0) == 0);
        CHECK(seen[i].ends_with("\r\n\r\n{\"n\":" + std::to_string(i + 1) + "}"));
    }
    CHECK(uploader->upload_failed() == 0);
    CHECK(counters.value(Counter::TelemetryQueueDropped) == 0);
}

TEST_CASE("uploader: response table decides success by ok, not by status", "[telemetry][uploader]") {
    // control_plane.md 3.5 의 1~6. ok 가 true 가 아니면 실패다 (architecture.md 6.3).
    struct Row {
        std::string reply;
        std::uint64_t failed;
        const char* why;
    };
    const Row rows[] = {
        {http_reply(R"({"ok":true})"), 0, "success"},
        {"HTTP/1.1 500 Internal\r\nContent-Length: 11\r\n\r\n{\"ok\":true}", 0, "status code is not looked at"},
        {http_reply(R"({"ok":false,"error":"rate_limited"})"), 1, "ok false is a failure"},
        {http_reply(R"({"error":"internal"})"), 1, "missing ok"},
        {http_reply(R"({"ok":"true"})"), 1, "ok must be a bool"},
        {http_reply(R"([true])"), 1, "body must be an object"},
        {http_reply(R"({"ok":true,"ok":true})"), 1, "duplicate key"},
        {"HTTP/1.0 200 OK\r\nContent-Length: 11\r\n\r\n{\"ok\":true}", 1, "status line"},
        {"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Length: 11\r\n\r\n{\"ok\":true}", 1,
         "transfer encoding"},
        {"HTTP/1.1 200 OK\r\n\r\n{\"ok\":true}", 1, "missing content length"},
    };
    for (const auto& row : rows) {
        INFO("why=" << row.why);
        const WsaContext wsa;
        LoopbackServer server({Script{row.reply}});
        const ShutdownEvent shutdown;
        Counters counters;
        auto uploader = start(server.port(), shutdown, counters);
        REQUIRE(uploader->push(QueuedRecord{Kind::kInterval, R"({"n":1})"}));
        finish(*uploader, shutdown);
        CHECK(server.requests().size() == 1);
        CHECK(uploader->upload_failed() == row.failed);
    }
}

TEST_CASE("uploader: transport failures are counted and not retried", "[telemetry][uploader]") {
    const WsaContext wsa;
    {
        // 아무도 듣지 않는 포트. 재시도하지 않으므로 레코드 둘이 실패 둘이다.
        const ShutdownEvent shutdown;
        Counters counters;
        auto uploader = start(closed_port(), shutdown, counters);
        REQUIRE(uploader->push(QueuedRecord{Kind::kEstablish, "{}"}));
        REQUIRE(uploader->push(QueuedRecord{Kind::kEstablish, "{}"}));
        finish(*uploader, shutdown);
        CHECK(uploader->upload_failed() == 2);
    }
    {
        // 답하지 않는 수신기. 수신 시간 제한에 걸리고 연결은 하나뿐이다.
        Script silent;
        silent.silent = true;
        LoopbackServer server({silent, Script{http_reply(R"({"ok":true})")}});
        const ShutdownEvent shutdown;
        Counters counters;
        auto uploader = start(server.port(), shutdown, counters);
        REQUIRE(uploader->push(QueuedRecord{Kind::kEstablish, "{}"}));
        finish(*uploader, shutdown);
        CHECK(uploader->upload_failed() == 1);
        CHECK(server.requests().size() == 1);
    }
}

TEST_CASE("uploader: a full queue drops the new record and counts it", "[telemetry][uploader]") {
    // concurrency.md 6장: 256 항목, 가득 차면 새 것을 버린다. 가장 오래된 것을 버리는 구현은 여기서 걸린다.
    // `[telemetry]` 는 처음 1초 동안 깨지 않으므로 그 사이에 채운다.
    const WsaContext wsa;
    std::vector<Script> scripts(kQueueCapacity, Script{http_reply(R"({"ok":true})")});
    LoopbackServer server(scripts);
    const ShutdownEvent shutdown;
    Counters counters;
    auto uploader = start(server.port(), shutdown, counters);

    for (std::size_t i = 0; i < kQueueCapacity; ++i) {
        REQUIRE(uploader->push(QueuedRecord{Kind::kInterval, "{\"n\":" + std::to_string(i) + "}"}));
    }
    CHECK_FALSE(uploader->push(QueuedRecord{Kind::kInterval, R"({"n":"newest"})"}));
    CHECK(counters.value(Counter::TelemetryQueueDropped) == 1);

    finish(*uploader, shutdown);
    server.stop();
    const auto seen = server.requests();
    REQUIRE(seen.size() == kQueueCapacity);
    CHECK(seen.front().ends_with(R"({"n":0})"));
    CHECK(seen.back().ends_with("{\"n\":" + std::to_string(kQueueCapacity - 1) + "}"));
    for (const auto& request : seen) {
        CHECK(request.find("newest") == std::string::npos);
    }
    CHECK(uploader->upload_failed() == 0);
}

TEST_CASE("uploader: after shutdown it waits for cleanup done, then drains", "[telemetry][uploader]") {
    // concurrency.md 7장: 종료를 보면 바로 끝나지 않고 정리 완료를 기다린다. 종료 절차 (3)의 마지막 레코드가 그 사이에 든다.
    const WsaContext wsa;
    LoopbackServer server({Script{http_reply(R"({"ok":true})")}});
    const ShutdownEvent shutdown;
    Counters counters;
    auto uploader = start(server.port(), shutdown, counters);

    shutdown.signal();
    REQUIRE(uploader->push(QueuedRecord{Kind::kEnd, R"({"n":"last"})"}));
    CHECK_FALSE(server.wait_request(300));  // 정리 완료 전에는 보내지 않는다
    CHECK_FALSE(uploader->join_for(0));

    uploader->cleanup_done();
    REQUIRE(uploader->join_for(5000));
    server.stop();
    const auto seen = server.requests();
    REQUIRE(seen.size() == 1);
    CHECK(seen[0].ends_with(R"({"n":"last"})"));
}

TEST_CASE("uploader: join_for gives up while an upload is stuck", "[telemetry][uploader]") {
    // join 상한을 넘기면 부르는 쪽이 _exit 로 끝낸다 (concurrency.md 7장). 여기서는 거짓을 돌려주는지만 본다.
    const WsaContext wsa;
    Script silent;
    silent.silent = true;
    LoopbackServer server({silent});
    const ShutdownEvent shutdown;
    Counters counters;
    auto uploader = start(server.port(), shutdown, counters);
    REQUIRE(uploader->push(QueuedRecord{Kind::kEnd, "{}"}));
    shutdown.signal();
    uploader->cleanup_done();
    REQUIRE(server.wait_request(3000));
    CHECK_FALSE(uploader->join_for(100));
    REQUIRE(uploader->join_for(5000));  // 수신 시간 제한(500ms)이 끝내 준다
    CHECK(uploader->upload_failed() == 1);
}
