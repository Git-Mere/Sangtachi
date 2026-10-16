#pragma once

// 가상 어댑터 (architecture.md 7장 어댑터 수명, concurrency.md 3장 루프 한 바퀴).
//
// 이 헤더는 OS 헤더를 들이지 않는다. 루프가 보는 것은 아래 Port 하나이고, Wintun 으로 그것을
// 채우는 구현은 platform/tun.hpp 와 client/src/platform/win32/ 에 있다 (ADR 0010 이음새 3).
// 시험은 같은 모양의 가짜를 루프에 붙인다.

#include "sangtachi/platform/interfaces.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace sangtachi::adapter {

// 어댑터의 이름과 Wintun 터널 형식 이름 (windows-prereq.md 3절).
//
// 터널 형식은 "Wintun" 이다. Wintun 은 이것에 " Tunnel" 을 붙여 어댑터 설명으로 쓰고, 서브넷 충돌
// 스크립트는 설명이 Wintun 인 어댑터만 자기 것으로 뺀다 (windows-prereq.md 7절). 다른 값을 쓰면 살아
// 있는 자기 어댑터가 빠지지 않아 자기 경로 때문에 차단이 나온다. 관리자 확인에서 실제로 그렇게 걸렸다.
inline constexpr std::string_view kName = "Sangtachi";
inline constexpr std::string_view kTunnelType = "Wintun";

// 가상 LAN 대역 (architecture.md 7장 가상 네트워크 설계). 서브넷 충돌 검사의 대상이다.
inline constexpr std::string_view kSubnetText = "10.100.0.0/24";

// 어댑터의 GUID. **자기 어댑터를 가리는 기록이 이 상수다** (windows-prereq.md 3절, 7절,
// protocol.md 10.1 제외 단계). Wintun 은 생성 때 이 값을 받는다. 바꾸면 이전 판이 남긴 주소를
// 다음 기동이 지우지 못한다.
//
// {E2085BB3-83CE-4597-8DD3-68795CCFD00A}. 아래 바이트는 Windows GUID 의 메모리 배치다
// (Data1, Data2, Data3 은 리틀 엔디언).
inline constexpr std::string_view kGuidText = "{E2085BB3-83CE-4597-8DD3-68795CCFD00A}";
inline constexpr platform::InterfaceGuid kGuid = {
    0xB3, 0x5B, 0x08, 0xE2, 0xCE, 0x83, 0x97, 0x45,
    0x8D, 0xD3, 0x68, 0x79, 0x5C, 0xCF, 0xD0, 0x0A,
};

// 수신 한 번의 결과 (concurrency.md 3장 drain_wintun 의 분기).
enum class ReceiveStatus {
    kPacket,   // packet 이 읽은 패킷이다. 다 쓴 뒤 release 로 돌려준다
    kEmpty,    // 링이 비었다. 정상
    kEof,      // 어댑터가 끝나는 중이다. 루프가 종료를 신호한다
    kCorrupt,  // 링이 손상됐다. 세션을 다시 만든다
    kError,    // 그 밖의 오류. error 가 OS 오류 코드다
};

struct Received {
    ReceiveStatus status = ReceiveStatus::kEmpty;
    std::span<const std::byte> packet;
    std::uint32_t error = 0;
};

// 주입 한 번의 결과 (concurrency.md 3장 inject).
enum class InjectStatus {
    kOk,
    kRingFull,  // drop_inject_ring_full
    kError,     // drop_inject_error. error 가 OS 오류 코드다
};

struct InjectResult {
    InjectStatus status = InjectStatus::kError;
    std::uint32_t error = 0;
};

// 루프가 쓰는 어댑터의 면. `[loop]` 하나가 부른다 (concurrency.md 5장 상태 소유).
class Port {
public:
    virtual ~Port() = default;

    // 대기 집합의 순위 2 (concurrency.md 2장 대기). 세션을 다시 만들면 값이 바뀐다.
    [[nodiscard]] virtual void* read_event() const noexcept = 0;

    // 패킷 하나를 읽는다. kPacket 이면 부르는 쪽이 release 를 꼭 부른다.
    [[nodiscard]] virtual Received receive() noexcept = 0;
    virtual void release(std::span<const std::byte> packet) noexcept = 0;

    // 패킷 하나를 넣는다. 실패해도 재시도하지 않는다 (concurrency.md 3장).
    [[nodiscard]] virtual InjectResult inject(std::span<const std::byte> packet) noexcept = 0;

    // 세션을 닫고 다시 연다 (concurrency.md 3장의 RESTART). 실패하면 거짓이다.
    [[nodiscard]] virtual bool restart_session() noexcept = 0;
};

}  // namespace sangtachi::adapter
