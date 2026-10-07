#include "sangtachi/control/exchange.hpp"

#include "sangtachi/control/http.hpp"
#include "sangtachi/network/endpoint.hpp"
#include "sangtachi/network/wsa.hpp"
#include "sangtachi/platform/wait.hpp"

#include "loopback_tcp.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <thread>

// 시험 케이스 이름은 ASCII 로만 적는다. 이유는 network/wsa_test.cpp 머리에 있다.
//
// control_plane.md 8.2 시간 제한과 3.5 의 "Content-Length 만큼 읽으면 멈춘다" 를 루프백 서버로
// 본다. 시간 제한은 짧은 값을 넣어 돌리고, 문서의 기본값(3초)으로 도는 케이스는 하나만 둔다.

using sangtachi::control::exchange;
using sangtachi::control::ExchangeResult;
using sangtachi::control::ExchangeStatus;
using sangtachi::control::ExchangeTimeouts;
using sangtachi::network::Endpoint;
using sangtachi::network::WsaContext;
using sangtachi::platform::monotonic_ms;
using sangtachi_test::closed_port;
using sangtachi_test::http_reply;
using sangtachi_test::LoopbackServer;
using sangtachi_test::Script;

namespace {

constexpr std::uint32_t kLoopback = 0x7F000001u;

std::string sample_request() {
    auto bytes = sangtachi::control::http::build_request("127.0.0.1:8000",
                                                         sangtachi::control::http::Op::kGetPeers,
                                                         R"({"room_id":"ABCDEF"})");
    REQUIRE(bytes.has_value());
    return *bytes;
}

ExchangeTimeouts short_timeouts(std::uint32_t io_ms = 1000) {
    ExchangeTimeouts t;
    t.connect_ms = 1000;
    t.io_ms = io_ms;
    return t;
}

// 이 프로세스가 연 커널 핸들 수. 소켓도 여기 든다.
DWORD handle_count() {
    DWORD count = 0;
    ::GetProcessHandleCount(::GetCurrentProcess(), &count);
    return count;
}

}  // namespace

TEST_CASE("exchange: a normal response is returned whole and the request arrives intact",
          "[control][exchange]") {
    const WsaContext wsa;
    const std::string body = R"({"ok":true})";
    LoopbackServer server({Script{http_reply(body)}});

    const std::string request = sample_request();
    const ExchangeResult r =
        exchange(Endpoint(kLoopback, server.port()), request, nullptr, short_timeouts());

    REQUIRE(r.status == ExchangeStatus::kComplete);
    REQUIRE(r.complete());
    REQUIRE(r.response == http_reply(body));
    server.stop();
    const auto seen = server.requests();
    REQUIRE(seen.size() == 1);
    REQUIRE(seen[0] == request);
}

TEST_CASE("exchange: stops at Content-Length without waiting for EOF", "[control][exchange]") {
    // 서버가 응답 뒤 연결을 3초 붙든다. EOF 를 기다리는 구현은 수신 상한(1초)에 걸려
    // kRecvTimeout 이 된다 (control_plane.md 3.5).
    const WsaContext wsa;
    Script script{http_reply(R"({"ok":true})")};
    script.hold_ms = 3000;
    LoopbackServer server({script});

    const auto start = monotonic_ms();
    const ExchangeResult r =
        exchange(Endpoint(kLoopback, server.port()), sample_request(), nullptr, short_timeouts());
    const auto elapsed = monotonic_ms() - start;

    REQUIRE(r.status == ExchangeStatus::kComplete);
    REQUIRE(elapsed < 800);
}

TEST_CASE("exchange: bytes after the body are cut at the boundary", "[control][exchange]") {
    const WsaContext wsa;
    const std::string reply = http_reply(R"({"ok":true})");
    Script script{reply + std::string(3000, 'x')};
    script.hold_ms = 2000;
    LoopbackServer server({script});

    const ExchangeResult r =
        exchange(Endpoint(kLoopback, server.port()), sample_request(), nullptr, short_timeouts());
    REQUIRE(r.status == ExchangeStatus::kComplete);
    REQUIRE(r.response == reply);
}

TEST_CASE("exchange: an empty body completes at the head", "[control][exchange]") {
    // Content-Length 0 이면 머리 끝이 곧 경계다. 본문을 한 바이트라도 더 기다리면 시간 초과다.
    const WsaContext wsa;
    Script script{http_reply("")};
    script.hold_ms = 3000;
    LoopbackServer server({script});

    const ExchangeResult r =
        exchange(Endpoint(kLoopback, server.port()), sample_request(), nullptr, short_timeouts());
    REQUIRE(r.status == ExchangeStatus::kComplete);
    REQUIRE(r.response == http_reply(""));
}

TEST_CASE("exchange: the server closing before the boundary is closed_early",
          "[control][exchange]") {
    const WsaContext wsa;
    // Content-Length 는 20 인데 본문 2 바이트만 보내고 닫는다.
    LoopbackServer server({Script{"HTTP/1.1 200 OK\r\nContent-Length: 20\r\n\r\n{}"}});

    const ExchangeResult r =
        exchange(Endpoint(kLoopback, server.port()), sample_request(), nullptr, short_timeouts());
    REQUIRE(r.status == ExchangeStatus::kClosedEarly);
    REQUIRE(r.response.empty());
}

TEST_CASE("exchange: an oversized head is a bad response", "[control][exchange]") {
    // 빈 줄이 MAX_HEADER_BYTES(2048) 안에 없다 (control_plane.md 3.5 의 2).
    const WsaContext wsa;
    Script script{"HTTP/1.1 200 OK\r\nX-Pad: " + std::string(4000, 'a')};
    script.hold_ms = 2000;
    LoopbackServer server({script});

    const ExchangeResult r =
        exchange(Endpoint(kLoopback, server.port()), sample_request(), nullptr, short_timeouts());
    REQUIRE(r.status == ExchangeStatus::kBadResponse);
    REQUIRE(r.frame_error == sangtachi::control::http::ResponseError::kHeaderTooLarge);
}

TEST_CASE("exchange: a body over MAX_BODY_BYTES is a bad response", "[control][exchange]") {
    const WsaContext wsa;
    Script script{"HTTP/1.1 200 OK\r\nContent-Length: 4097\r\n\r\n" + std::string(4097, '1')};
    script.hold_ms = 2000;
    LoopbackServer server({script});

    const ExchangeResult r =
        exchange(Endpoint(kLoopback, server.port()), sample_request(), nullptr, short_timeouts());
    REQUIRE(r.status == ExchangeStatus::kBadResponse);
    REQUIRE(r.frame_error == sangtachi::control::http::ResponseError::kBodyTooLarge);
}

TEST_CASE("exchange: a silent server times out at io_ms", "[control][exchange]") {
    const WsaContext wsa;
    Script script;
    script.silent = true;
    LoopbackServer server({script});

    const auto start = monotonic_ms();
    const ExchangeResult r = exchange(Endpoint(kLoopback, server.port()), sample_request(),
                                      nullptr, short_timeouts(300));
    const auto elapsed = monotonic_ms() - start;

    REQUIRE(r.status == ExchangeStatus::kRecvTimeout);
    // GetTickCount64 의 눈금(약 15.6ms)만큼 짧게 보일 수 있다.
    REQUIRE(elapsed >= 250);
    REQUIRE(elapsed < 1500);
}

TEST_CASE("exchange: a dripping server cannot stretch the receive phase", "[control][exchange]") {
    // 바이트를 20ms 마다 하나씩 흘린다. recv 한 번마다 걸리는 SO_RCVTIMEO 만으로는 끝나지
    // 않는다. 수신 단계 전체를 io_ms 로 묶어야 끝난다 (control_plane.md 8.2 의 "최대 9초").
    const WsaContext wsa;
    Script script{"HTTP/1.1 200 OK\r\nX-Pad: " + std::string(300, 'a')};
    script.drip_ms = 20;
    LoopbackServer server({script});

    const auto start = monotonic_ms();
    const ExchangeResult r = exchange(Endpoint(kLoopback, server.port()), sample_request(),
                                      nullptr, short_timeouts(500));
    const auto elapsed = monotonic_ms() - start;

    REQUIRE(r.status == ExchangeStatus::kRecvTimeout);
    REQUIRE(elapsed < 2000);
}

TEST_CASE("exchange: the documented 3 second receive limit holds against a silent server",
          "[control][exchange][slow]") {
    // 문서의 기본값으로 도는 유일한 케이스다 (control_plane.md 2.6 의 CLIENT_IO_TIMEOUT_S).
    const WsaContext wsa;
    Script script;
    script.silent = true;
    LoopbackServer server({script});

    const auto start = monotonic_ms();
    const ExchangeResult r =
        exchange(Endpoint(kLoopback, server.port()), sample_request(), nullptr);
    const auto elapsed = monotonic_ms() - start;

    REQUIRE(r.status == ExchangeStatus::kRecvTimeout);
    REQUIRE(elapsed >= 2900);
    REQUIRE(elapsed < 5000);
}

TEST_CASE("exchange: a refused connection is connect_failed, not a timeout",
          "[control][exchange]") {
    // Windows 는 RST 를 받고도 SYN 을 몇 번 다시 보내므로 거부가 즉시 오지 않는다(실측 약
    // 2초). 그래서 연결 상한을 문서 기본값(3초)으로 두고, 상한에 닿기 전에 거부로 끝나는지 본다.
    const WsaContext wsa;
    ExchangeTimeouts t;
    t.io_ms = 1000;
    const auto start = monotonic_ms();
    const ExchangeResult r = exchange(Endpoint(kLoopback, closed_port()), sample_request(), nullptr, t);
    const auto elapsed = monotonic_ms() - start;

    REQUIRE(r.status == ExchangeStatus::kConnectFailed);
    REQUIRE(r.os_error != 0);
    REQUIRE(elapsed < 2900);
}

TEST_CASE("exchange: the connect limit is honored", "[control][exchange]") {
    // 연결 상한 0 이면 select 를 한 번도 기다리지 않고 시간 초과다. 상한을 무시하는 구현은
    // 루프백에서 그대로 연결해 응답까지 받는다.
    const WsaContext wsa;
    LoopbackServer server({Script{http_reply("{}")}});
    ExchangeTimeouts t;
    t.connect_ms = 0;
    t.io_ms = 1000;
    const ExchangeResult r = exchange(Endpoint(kLoopback, server.port()), sample_request(), nullptr, t);
    REQUIRE(r.status == ExchangeStatus::kConnectTimeout);
}

TEST_CASE("exchange: a signaled abort event stops the connect", "[control][exchange]") {
    const WsaContext wsa;
    LoopbackServer server({Script{http_reply("{}")}});
    auto* abort = sangtachi::platform::create_event(sangtachi::platform::ResetMode::Manual);
    REQUIRE(abort != nullptr);
    REQUIRE(sangtachi::platform::signal_event(abort));

    const ExchangeResult r =
        exchange(Endpoint(kLoopback, server.port()), sample_request(), abort, short_timeouts());
    sangtachi::platform::close_event(abort);
    REQUIRE(r.status == ExchangeStatus::kAborted);
}

TEST_CASE("exchange: an abort while waiting for the response returns promptly",
          "[control][exchange]") {
    // concurrency.md 8장 `[control]` 스레드의 종료 행. 응답을 기다리지 않고 돌아온다.
    const WsaContext wsa;
    Script script;
    script.silent = true;
    LoopbackServer server({script});
    auto* abort = sangtachi::platform::create_event(sangtachi::platform::ResetMode::Manual);
    REQUIRE(abort != nullptr);

    std::thread signaler([&server, abort] {
        server.wait_request(5000);
        sangtachi::platform::signal_event(abort);
    });

    const auto start = monotonic_ms();
    const ExchangeResult r = exchange(Endpoint(kLoopback, server.port()), sample_request(), abort,
                                      short_timeouts(5000));
    const auto elapsed = monotonic_ms() - start;
    signaler.join();
    sangtachi::platform::close_event(abort);

    REQUIRE(r.status == ExchangeStatus::kAborted);
    REQUIRE(elapsed < 1500);
}

TEST_CASE("exchange: no socket leaks across success and failure paths", "[control][exchange]") {
    // 요청마다 새 소켓이다 (control_plane.md 8.2). 닫기를 하나 흘리면 요청마다 쌓인다.
    const WsaContext wsa;
    const std::string request = sample_request();

    auto round = [&] {
        {
            LoopbackServer ok({Script{http_reply("{}")}});
            REQUIRE(exchange(Endpoint(kLoopback, ok.port()), request, nullptr, short_timeouts()).complete());
        }
        {
            LoopbackServer early({Script{"HTTP/1.1 200 OK\r\nContent-Length: 20\r\n\r\n{}"}});
            REQUIRE(exchange(Endpoint(kLoopback, early.port()), request, nullptr, short_timeouts())
                        .status == ExchangeStatus::kClosedEarly);
        }
        {
            LoopbackServer bad({Script{"HTTP/1.0 200 OK\r\nContent-Length: 2\r\n\r\n{}"}});
            REQUIRE(exchange(Endpoint(kLoopback, bad.port()), request, nullptr, short_timeouts())
                        .status == ExchangeStatus::kBadResponse);
        }
        {
            LoopbackServer slow({Script{http_reply("{}")}});
            ExchangeTimeouts t;
            t.connect_ms = 0;
            REQUIRE(exchange(Endpoint(kLoopback, slow.port()), request, nullptr, t).status ==
                    ExchangeStatus::kConnectTimeout);
        }
    };

    round();  // Winsock 이 처음 쓸 때 잡는 핸들을 먼저 치운다
    const DWORD before = handle_count();
    for (int i = 0; i < 10; ++i) {
        round();
    }
    const DWORD after = handle_count();
    INFO("before=" << before << " after=" << after);
    // 한 바퀴에 소켓 넷이다. 열 바퀴에 하나라도 흘리면 열 개 이상 는다.
    REQUIRE(after < before + 8);
}
