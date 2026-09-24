#include "sangtachi/network/wsa.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>

#include <catch2/catch_test_macros.hpp>

#include <string>

// 시험 케이스 이름은 ASCII 로만 적는다.
//
// catch_discover_tests 는 케이스 이름을 CTest 에 등록하고, 실행할 때 그 이름을 명령
// 인자로 실행 파일에 도로 넘긴다. 이름에 ASCII 밖 문자가 있으면 콘솔 코드 페이지를
// 거치며 깨져서 어느 케이스와도 일치하지 않고, "No tests ran" 인 채로 CTest 가
// 실패를 보고한다. 실측으로 확인했다. 설명은 아래 주석과 태그에 적는다.

using sangtachi::network::WsaContext;

// WsaContext 가 협상된 버전을 보고한다.
TEST_CASE("wsa: context reports negotiated version", "[wsa]") {
    const WsaContext wsa;
    const std::string version = wsa.negotiated_version();

    // 요청은 2.2 다. Winsock 은 요청값 이하의 가장 높은 버전을 줄 수 있으므로
    // 정확히 "2.2" 를 요구하지 않는다. major 가 2 인 것까지만 판정한다.
    REQUIRE_FALSE(version.empty());
    REQUIRE(version.substr(0, 2) == "2.");
}

// 소멸 뒤에 다시 만들 수 있다. WSAStartup 과 WSACleanup 의 짝을 본다.
TEST_CASE("wsa: context can be recreated after destruction", "[wsa]") {
    // 호출 수가 어긋나면 두 번째 socket() 이 WSANOTINITIALISED 로 실패한다.
    {
        const WsaContext first;
        REQUIRE_FALSE(first.negotiated_version().empty());
    }

    const WsaContext second;
    const SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    REQUIRE(s != INVALID_SOCKET);
    ::closesocket(s);
}

// 대조군. 초기화가 없으면 socket() 이 실패한다.
TEST_CASE("wsa: socket fails without initialization", "[wsa]") {
    // 이것이 없으면 위 케이스의 socket() 성공이 WsaContext 덕분인지 다른 무엇
    // 덕분인지 구분할 수 없다.
    //
    // Winsock 은 프로세스 단위로 참조를 센다. 이 케이스가 도는 시점에 살아 있는
    // WsaContext 가 없어야 성립하므로 여기서는 아무것도 만들지 않는다.
    const SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s != INVALID_SOCKET) {
        ::closesocket(s);
        FAIL("Winsock is already initialized; run this case in its own process");
    }
    REQUIRE(::WSAGetLastError() == WSANOTINITIALISED);
}

// 마지막 context 가 사라지면 다시 초기화 안 된 상태로 돌아간다.
TEST_CASE("wsa: socket fails after the last context is destroyed", "[wsa]") {
    // 이 케이스가 소멸자의 WSACleanup 을 덮는다. 위의 두 케이스는 소멸자가 아무것도
    // 하지 않아도 전부 통과한다. Winsock 참조 수가 줄지 않을 뿐 socket() 은 계속
    // 성공하기 때문이다. 변이 시험으로 확인했다.
    {
        const WsaContext wsa;
        const SOCKET inside = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        REQUIRE(inside != INVALID_SOCKET);
        ::closesocket(inside);
    }

    const SOCKET after = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (after != INVALID_SOCKET) {
        ::closesocket(after);
        FAIL("WSACleanup was not called; Winsock is still initialized");
    }
    REQUIRE(::WSAGetLastError() == WSANOTINITIALISED);
}
