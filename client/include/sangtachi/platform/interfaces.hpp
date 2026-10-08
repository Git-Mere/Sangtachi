#pragma once

// 인터페이스 열거와 기본 경로 조회 (protocol.md 10.1 후보 수집과 위생).
// 구현은 client/src/platform/win32/ 에 있다 (ADR 0010).
//
// **OS 가 준 값을 옮겨 담기만 한다.** 판정(활성, 제외, 자기 서브넷 집합, 기본 경로, 등록 목록)은
// 하나도 하지 않는다. 판정은 peer/candidates.hpp 의 순수 함수가 하고, 시험은 이 함수 대신 같은 모양의
// 값을 주입한다 (roadmap.md Phase 4 "판정 함수가 인터페이스 열거와 경로 조회를 OS 대신 주입받을 수
// 있어야 한다").
//
// 아래 열거형과 필드 이름은 Windows 구현의 이름이다(`OperStatus`, `DadState`, `IfType`,
// `InterfaceGuid`). 10.1 이 "지켜야 하는 것은 이름이 아니라 각 단계의 의도다" 로 적었다.
//
// **[loop] 가 부른다** (concurrency.md 5장 상태 소유 "로컬 후보와 자기 서브넷 집합"). 둘 다 동기 OS
// 호출이고 시도마다 한 번이다.

#include <array>
#include <cstdint>
#include <vector>

namespace sangtachi::platform {

// 인터페이스 식별자 16바이트. Windows 의 GUID 를 메모리 배치 그대로 담는다. 비교에만 쓴다.
using InterfaceGuid = std::array<std::uint8_t, 16>;

// 인터페이스의 운영 상태. 값은 IF_OPER_STATUS 의 순서와 같다(1부터). 10.1 이 보는 것은 kUp 하나다.
enum class OperStatus : std::uint8_t {
    kUp = 1,
    kDown = 2,
    kTesting = 3,
    kUnknown = 4,
    kDormant = 5,
    kNotPresent = 6,
    kLowerLayerDown = 7,
};

// 주소의 중복 주소 검출 상태. 값은 NL_DAD_STATE 와 같다. 10.1 이 보는 것은 kPreferred 하나다.
enum class DadState : std::uint8_t {
    kInvalid = 0,
    kTentative = 1,
    kDuplicate = 2,
    kDeprecated = 3,
    kPreferred = 4,
};

struct InterfaceAddress {
    std::uint32_t address = 0;        // 호스트 바이트 순서
    std::uint8_t prefix_length = 0;   // OS 가 준 값 그대로. 0~32 밖일 수 있다 (10.1 케이스 22)
    DadState dad_state = DadState::kInvalid;
};

struct InterfaceInfo {
    std::uint32_t if_index = 0;  // 경로 조회의 if_index 와 같은 번호 체계
    std::uint32_t if_type = 0;   // IANA ifType
    OperStatus oper_status = OperStatus::kDown;
    InterfaceGuid guid{};
    std::vector<InterfaceAddress> addresses;  // IPv4 유니캐스트만. OS 가 열거한 순서
};

// IPv4 경로 한 줄. 목적지가 0.0.0.0/0 이 아닌 줄은 기본 경로 판정에 쓰이지 않는다.
struct Ipv4Route {
    std::uint32_t destination = 0;  // 호스트 바이트 순서
    std::uint8_t prefix_length = 0;
    std::uint32_t if_index = 0;
    std::uint32_t route_metric = 0;
    std::uint32_t interface_metric = 0;
};

// 조회 하나의 결과. ok 가 거짓이면 목록은 비어 있고 error 는 OS 오류 코드다 (로그에만 싣는다).
struct InterfaceQuery {
    bool ok = false;
    std::uint32_t error = 0;
    std::vector<InterfaceInfo> interfaces;  // OS 가 열거한 순서
};

struct RouteQuery {
    bool ok = false;
    std::uint32_t error = 0;
    std::vector<Ipv4Route> routes;  // OS 가 열거한 순서
};

// IPv4 주소를 가진 인터페이스를 모두 읽는다. 상태와 무관하게 다 담는다. 거르는 것은 판정이 한다.
//
// 주소가 하나도 없는 것은 성공이다(빈 목록). 10.1 "열거가 성공했는데 활성 주소가 없으면" 과
// "열거가 실패하면" 을 가르는 자리가 여기다.
[[nodiscard]] InterfaceQuery query_ipv4_interfaces();

// 목적지가 0.0.0.0/0 인 IPv4 경로를 읽는다. 각 줄에 경로 메트릭과 그 인터페이스의 인터페이스
// 메트릭을 담는다. 10.1 의 기본 경로는 그 합이 가장 작은 줄이다. 판정은 peer/candidates.hpp 가 한다.
//
// 0.0.0.0/0 이 아닌 줄은 담지 않는다. 그 줄마다 인터페이스 메트릭을 묻는 비용을 치를 이유가 없다.
[[nodiscard]] RouteQuery query_ipv4_default_routes();

}  // namespace sangtachi::platform
