#include "sangtachi/platform/resolve.hpp"

#include "sangtachi/network/udp_socket.hpp"
#include "sangtachi/network/wsa.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <string_view>

// 시험 케이스 이름은 ASCII 로만 적는다. 이유는 network/wsa_test.cpp 머리에 있다.

// **이름 해석을 네트워크에 의존시키지 않는다.** 공개 DNS 이름을 넣으면 오프라인에서
// 실패하고, NXDOMAIN 을 가로채는 망에서는 없는 이름이 주소를 받아 온다. 결정적인 입력은
// 셋뿐이다.
//   (1) IPv4 리터럴. 이름 해석을 거치지 않는다
//   (2) localhost. RFC 6761 이 예약했고 Windows 가 자체 해석한다
//   (3) 이 함수가 해석에 들어가기 전에 거르는 입력(빈 문자열, 널, 길이 초과)
// 그래서 "없는 이름은 값이 없다" 를 실제 DNS 이름으로 판정하지 않는다. (3) 이 그 자리다.

using sangtachi::network::Endpoint;
using sangtachi::network::open_udp_socket;
using sangtachi::network::WsaContext;
using sangtachi::platform::looks_like_numeric_host;
using sangtachi::platform::resolve_ipv4;

namespace {

constexpr std::uint32_t kLoopback = 0x7F000001u;

}  // namespace

TEST_CASE("resolve: an IPv4 literal keeps its address and port", "[platform][resolve]") {
    // 리터럴 경로는 Winsock 초기화 없이도 돈다. 여기서 WsaContext 를 만들지 않는 것이
    // 그 판정이다.
    const auto endpoint = resolve_ipv4("127.0.0.1", 3478);
    REQUIRE(endpoint.has_value());
    REQUIRE(endpoint->address() == kLoopback);
    REQUIRE(endpoint->port() == 3478);
    REQUIRE(endpoint->to_string() == "127.0.0.1:3478");
}

TEST_CASE("resolve: literal edge addresses survive the byte order", "[platform][resolve]") {
    // 네트워크 순서로 두면 여기서 뒤집혀 나온다.
    const auto low = resolve_ipv4("1.2.3.4", 19302);
    REQUIRE(low.has_value());
    REQUIRE(low->address() == 0x01020304u);
    REQUIRE(low->to_string() == "1.2.3.4:19302");

    const auto high = resolve_ipv4("255.255.255.254", 1);
    REQUIRE(high.has_value());
    REQUIRE(high->address() == 0xFFFFFFFEu);

    // 포트 0 도 담는다. 1~65535 규칙은 기동 인자 쪽이 판정한다 (endpoint.hpp).
    const auto zero = resolve_ipv4("0.0.0.0", 0);
    REQUIRE(zero.has_value());
    REQUIRE(zero->address() == 0u);
    REQUIRE(zero->port() == 0);
}

TEST_CASE("resolve: an empty host has no value", "[platform][resolve]") {
    // 이름 경로가 살아 있는 상태에서 본다. 초기화가 없으면 어떤 구현이든 값 없음을
    // 돌려주므로 아무것도 판정하지 못한다.
    const WsaContext wsa;
    REQUIRE_FALSE(resolve_ipv4("", 3478).has_value());
    REQUIRE_FALSE(resolve_ipv4(std::string_view{}, 3478).has_value());
}

TEST_CASE("resolve: a host with an embedded NUL has no value", "[platform][resolve]") {
    // 그대로 복사하면 널 앞까지만 이름으로 읽혀, 사용자가 친 것과 우리가 묻는 이름이
    // 달라진다. "localhost" 로 잘려 127.0.0.1 이 나오면 그것이 이 케이스의 실패다.
    //
    // WsaContext 가 있어야 이 케이스가 뜻을 갖는다. 없으면 이름 경로가 초기화 부족으로
    // 실패해서, 검사를 지운 구현도 값 없음을 돌려주고 통과한다. 변이 시험으로 확인했다.
    const WsaContext wsa;
    const std::string_view hidden("localhost\0.example", 18);
    REQUIRE(hidden.size() == 18);
    REQUIRE_FALSE(resolve_ipv4(hidden, 3478).has_value());
}

TEST_CASE("resolve: an over-long host has no value", "[platform][resolve]") {
    // DNS 이름의 상한은 253자다. 버퍼에 담기지 않는 입력은 잘라 묻지 않고 거른다.
    const std::string too_long(300, 'a');
    REQUIRE_FALSE(resolve_ipv4(too_long, 3478).has_value());

    // 경계 바로 아래(255자)는 거르지 않는다. 이름으로 넘어가 해석에 실패할 뿐이다.
    // 그 결과는 망에 달렸으므로 값을 단정하지 않고, 이 호출이 돌아온다는 것만 본다.
    const WsaContext wsa;
    const std::string at_limit(255, 'a');
    (void)resolve_ipv4(at_limit, 3478);
    SUCCEED();
}

TEST_CASE("resolve: localhost resolves to an IPv4 loopback address", "[platform][resolve]") {
    // RFC 6761 이 예약한 이름이고 Windows 가 DNS 로 나가지 않고 해석한다.
    const WsaContext wsa;
    const auto endpoint = resolve_ipv4("localhost", 19302);
    REQUIRE(endpoint.has_value());
    // AAAA 를 쓰지 않으므로 ::1 이 아니라 127/8 이 나와야 한다.
    REQUIRE((endpoint->address() >> 24) == 127u);
    REQUIRE(endpoint->port() == 19302);
}

TEST_CASE("resolve: a name needs Winsock but a literal does not", "[platform][resolve]") {
    // 초기화가 없으면 getaddrinfo 는 WSANOTINITIALISED 로 실패한다. 그때도 터지지 않고
    // 값 없음으로 돌아온다.
    //
    // **전제를 가정하지 않고 검사한다.** 앞의 판이 이 자리에서 아무것도 단정하지 못하고
    // SUCCEED 로 끝났다. 그러면 이름 경로가 초기화를 요구하지 않게 바뀌어도 통과한다.
    // 여기서는 소켓 하나를 열어 보고 그것이 실패하는 것으로 "지금 이 프로세스에 Winsock
    // 이 없다" 를 세운 다음 단정한다. ctest 는 케이스마다 프로세스를 따로 띄우므로
    // (catch_discover_tests) 그 전제가 이 파일 안의 다른 케이스에 흔들리지 않는다.
    REQUIRE_FALSE(open_udp_socket().ok());  // 전제: 초기화가 없다

    const auto literal = resolve_ipv4("127.0.0.1", 3478);
    REQUIRE(literal.has_value());
    REQUIRE(literal->address() == kLoopback);

    REQUIRE_FALSE(resolve_ipv4("localhost", 3478).has_value());
}

TEST_CASE("resolve: numeric-looking hosts never reach the name path", "[platform][resolve]") {
    // 우리 parse_ipv4 가 거부한 옛 IPv4 표기가 getaddrinfo 로 새 나가면 사용자가 친 것과
    // 다른 주소를 쓰게 된다 (resolve.hpp). 판정 함수의 케이스 표다.
    struct Row {
        std::string_view host;
        bool numeric;
        const char* why;
    };
    const Row rows[] = {
        {"1.2.3.4", true, "dotted quad"},
        {"1.2.3", true, "too few parts"},
        {"1.2.3.4.5", true, "too many parts"},
        {"010.1.1.1", true, "leading zero reads as octal"},
        {"0x7f.1", true, "hex form"},
        {"0X7F.0x1", true, "upper case hex form"},
        {"256.1.1.1", true, "out of range but still numeric"},
        {"1", true, "a single decimal number"},
        {"0x", true, "the hex prefix with no digits"},
        // 여기부터 이름이다. **정상 이름을 막지 않는 것이 이 규칙의 목표다.**
        {"1drv.ms", false, "a real name that starts with a digit"},
        {"cafe.example", false, "hex letters without the 0x prefix"},
        // **모든 라벨이 16진 글자인 이름.** 위 두 행은 16진이 아닌 글자를 하나씩 갖고
        // 있어서, 규칙이 `0x` 없는 16진까지 숫자로 보게 바뀌어도 통과한다. 변이 시험으로
        // 확인했다. 이 행들이 그 자리를 지킨다.
        {"cafe.beef", false, "every label is made of hex letters"},
        {"abc", false, "a single hex-letter label"},
        {"ff.ff", false, "two hex-letter labels"},
        {"deadbeef.example", false, "a long hex-letter label"},
        {"stun.l.google.com", false, "the default STUN list"},
        {"127.0.0.1.example", false, "a name that starts like an address"},
        {"1.2.3.4.", false, "a trailing dot leaves an empty label"},
        {"localhost", false, "a plain name"},
        {"", false, "the empty host"},
    };
    for (const Row& row : rows) {
        INFO(row.why);
        REQUIRE(looks_like_numeric_host(row.host) == row.numeric);
    }

    // 아래는 거동을 못박을 뿐 기전을 가르지 못한다. 이 기계의 getaddrinfo 는 위 표기들을
    // 스스로 거부하므로, 판정을 빼도 같은 값이 나온다. **변이 시험으로 확인했다.** 규칙을
    // 지키는 것은 위의 케이스 표다. 그래도 남겨 두는 이유는 판정이 정상 입력을 먹지
    // 않는다는 것과, 어느 기계에서도 이 입력이 주소가 되지 않는다는 것을 고정하기
    // 위해서다. 기전과 실측 범위는 resolve.hpp 가 갖는다.
    const WsaContext wsa;
    REQUIRE_FALSE(resolve_ipv4("1.2.3", 3478).has_value());
    REQUIRE_FALSE(resolve_ipv4("0x7f.1", 3478).has_value());
    REQUIRE_FALSE(resolve_ipv4("010.1.1.1", 3478).has_value());
    REQUIRE_FALSE(resolve_ipv4("1.2.3.4.5", 3478).has_value());
    REQUIRE_FALSE(resolve_ipv4("256.1.1.1", 3478).has_value());

    // 점 십진 리터럴은 그대로 통과한다. 위 규칙이 정상 입력을 먹지 않는다.
    const auto ok = resolve_ipv4("1.2.3.4", 3478);
    REQUIRE(ok.has_value());
    REQUIRE(ok->address() == 0x01020304u);
}
