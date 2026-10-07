#include "sangtachi/platform/tcp.hpp"

#include "sangtachi/network/endpoint.hpp"
#include "sangtachi/network/wsa.hpp"

#include "../control/loopback_tcp.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

// 시험 케이스 이름은 ASCII 로만 적는다. 이유는 network/wsa_test.cpp 머리에 있다.

using sangtachi::network::Endpoint;
using sangtachi::network::WsaContext;
using sangtachi::platform::connect_tcp;
using sangtachi::platform::TcpStatus;

TEST_CASE("tcp: a connected stream carries both io timeouts", "[platform][tcp]") {
    // control_plane.md 8.2 시간 제한: 송신과 수신 각각 SO_SNDTIMEO / SO_RCVTIMEO.
    const WsaContext wsa;
    sangtachi_test::Script script;
    script.silent = true;
    sangtachi_test::LoopbackServer server({script});

    auto connected = connect_tcp(Endpoint(0x7F000001u, server.port()), 1000, 1234, nullptr);
    REQUIRE(connected.result.status == TcpStatus::kOk);
    REQUIRE(connected.stream.has_value());

    const auto sock = static_cast<SOCKET>(connected.stream->native_handle());
    for (const int option : {SO_RCVTIMEO, SO_SNDTIMEO}) {
        DWORD value = 0;
        int length = sizeof(value);
        REQUIRE(::getsockopt(sock, SOL_SOCKET, option, reinterpret_cast<char*>(&value), &length) == 0);
        INFO("option=" << option);
        REQUIRE(value == 1234);
    }
}
