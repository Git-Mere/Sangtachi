#pragma once

// Wintun 어댑터와 그 주변의 OS 호출 (architecture.md 7장 어댑터 수명).
// 구현은 client/src/platform/win32/tun.cpp 다 (ADR 0010 이음새 3).
//
// **[loop] 가 부른다** (concurrency.md 5장 상태 소유). 기동 단계는 [loop] 를 시작하기 전에 main 이
// 부른다. 판정을 하나도 하지 않는다. 서브넷 충돌과 방화벽 규칙의 판정은 스크립트가 하고 여기서는 그
// 종료 코드를 옮긴다 (windows-prereq.md 7절, 2절).

#include "sangtachi/adapter.hpp"
#include "sangtachi/platform/interfaces.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sangtachi::platform {

// 실패한 OS 호출 하나. op 는 문자열 상수다.
struct OsFailure {
    std::string_view op;
    std::uint32_t code = 0;
};

// 관리자 권한으로 돌고 있는가 (windows-prereq.md 1절). 조회가 실패하면 거짓이다.
[[nodiscard]] bool is_elevated() noexcept;

// 서브넷 충돌 검사의 결과 (windows-prereq.md 7절). 스크립트의 종료 코드 0, 1, 2 와 같다.
enum class SubnetVerdict {
    kProceed,  // 0
    kBlock,    // 1. pick another range
    kUnknown,  // 2, 그 밖의 코드, 또는 스크립트를 돌리지 못했다
};

struct SubnetCheck {
    SubnetVerdict verdict = SubnetVerdict::kUnknown;
    std::uint32_t exit_code = 0;
    std::vector<std::string> blocks;  // 스크립트 출력의 BLOCK 줄에서 뽑은 접두사 (예: 10.100.0.0/24)
    OsFailure failure;                // 스크립트를 돌리지 못했으면 op 가 비지 않는다
};

// 실행 파일 옆의 Test-SubnetOverlap.ps1 을 Windows PowerShell 로 돌린다. 30초 안에 끝나지 않으면
// 프로세스를 끝내고 kUnknown 이다.
[[nodiscard]] SubnetCheck run_subnet_check(std::string_view target);

// 방화벽 규칙의 검증과 만들기 결과 (windows-prereq.md 2절 클라이언트가 만드는 규칙). 스크립트의 종료 코드
// 0, 1, 2 와 같다.
enum class FirewallVerdict {
    kPass,     // 0
    kFail,     // 1. 검사가 어긋났거나 만들지 못했다
    kUnknown,  // 2, 그 밖의 코드, 또는 스크립트를 돌리지 못했다
};

struct FirewallCheck {
    FirewallVerdict verdict = FirewallVerdict::kUnknown;
    std::uint32_t exit_code = 0;
    std::vector<std::string> failed;   // 스크립트 출력의 FAIL 줄. `<규칙 이름 또는 policy>:<검사>`
    std::vector<std::string> unknown;  // 스크립트 출력의 UNKNOWN 줄. `<규칙 이름 또는 policy>:<무엇>`
    std::vector<std::string> created;  // 스크립트 출력의 CREATED 줄. 이번에 만든 규칙 이름
    OsFailure failure;                // 스크립트를 돌리지 못했으면 op 가 비지 않는다
};

// 실행 파일 옆의 Initialize-FirewallRules.ps1 을 Windows PowerShell 로 돌린다. 어댑터가 선 뒤에 부른다.
// 60초 안에 끝나지 않으면 프로세스를 끝내고 kUnknown 이다.
[[nodiscard]] FirewallCheck run_firewall_check();

// 주소 하나를 붙인 결과.
struct AddressResult {
    bool ok = false;
    bool route_created = false;  // on-link 경로가 없어 만들었다 (windows-prereq.md 8절)
    OsFailure failure;
    std::string_view reason;     // ok 가 거짓일 때. duplicate, timeout, ...
};

// Wintun 어댑터 하나. 만들 때 열고 소멸할 때 닫는다. 닫으면 Wintun 이 어댑터를 지운다.
class TunDevice final : public adapter::Port {
public:
    struct Opened {
        std::unique_ptr<TunDevice> device;
        OsFailure failure;  // device 가 비면 이것이 원인이다
    };

    // wintun.dll 을 실행 파일 폴더에서 해시를 맞춘 뒤 불러오고, 어댑터를 만들고, MTU 와 DAD 를
    // 설정하고, 남은 IPv4 주소를 지우고, 세션을 연다 (architecture.md 7장 어댑터 수명의 기동 단계).
    [[nodiscard]] static Opened open(std::uint32_t mtu);

    ~TunDevice() override;
    TunDevice(const TunDevice&) = delete;
    TunDevice& operator=(const TunDevice&) = delete;

    [[nodiscard]] void* read_event() const noexcept override;
    [[nodiscard]] adapter::Received receive() noexcept override;
    void release(std::span<const std::byte> packet) noexcept override;
    [[nodiscard]] adapter::InjectResult inject(std::span<const std::byte> packet) noexcept override;
    [[nodiscard]] bool restart_session() noexcept override;

    // 가상 IP 를 /24 로 붙이고 Preferred 를 기다린다 (windows-prereq.md 8절, 9절). 앞서 붙인 주소가
    // 있으면 먼저 지운다. ip 는 호스트 바이트 순서다.
    [[nodiscard]] AddressResult assign_address(std::uint32_t ip);

    // 붙인 주소와 만든 경로를 지운다. 붙인 것이 없으면 아무것도 하지 않고 성공이다. 지우지 못한 것은
    // 기억해 두고 다음 호출이 다시 지운다.
    [[nodiscard]] OsFailure clear_address();

    // 세션을 닫고, 주소를 지우고, 어댑터를 닫는다 (concurrency.md 7장 종료의 5번과 6번). 소멸자도
    // 부른다. 두 번 불러도 된다. 주소나 경로를 지우지 못했으면 그 실패를 돌려준다. 어댑터는 그래도 닫는다.
    [[nodiscard]] OsFailure close() noexcept;

    // 이 어댑터의 실제 GUID 와 LUID. GUID 는 adapter::kGuid 를 요청해서 받은 값이다.
    [[nodiscard]] const InterfaceGuid& guid() const noexcept { return guid_; }
    [[nodiscard]] std::uint64_t luid() const noexcept { return luid_; }

private:
    struct Api;
    TunDevice() = default;

    std::unique_ptr<Api> api_;
    void* adapter_ = nullptr;
    void* session_ = nullptr;
    std::uint64_t luid_ = 0;
    InterfaceGuid guid_{};
    std::uint32_t address_ = 0;  // 붙인 주소. 0 이면 없다
    bool route_created_ = false;
};

}  // namespace sangtachi::platform
