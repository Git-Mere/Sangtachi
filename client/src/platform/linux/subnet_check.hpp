#pragma once

// 플랫폼 내부 헤더: 서브넷 충돌 판정 (linux-prereq.md 4절, 판정표는 windows-prereq.md 7절).
//
// OS 자료를 받기만 하는 순수 함수다. 조회는 tun.cpp 가 한다. 그래야 판정표를 시험이 행마다 돌린다.
// 순서와 경계는 tools/winprereq/Test-SubnetOverlap.ps1 의 Test-PrefixOverlap 과 Get-SubnetVerdict 를 따른다.

#include "sangtachi/network/endpoint.hpp"
#include "sangtachi/platform/tun.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sangtachi::platform::subnet {

struct Prefix {
    std::uint32_t network = 0;  // 호스트 바이트 순서
    std::uint8_t length = 0;
};

enum class Overlap {
    kIgnore,       // 겹치지 않는다
    kBlockRoute,   // 대상과 길이가 같거나 더 길다
    kCatchAll,     // 길이 0~1. 기본 경로를 가리는 것은 어떤 터널이든 하는 일이라 충돌이 아니다
    kBlockShadow,  // 길이 2 ~ 대상-1
};

[[nodiscard]] constexpr std::uint32_t prefix_mask(std::uint8_t length) noexcept {
    return length == 0 ? 0u : (0xFFFFFFFFu << (32 - length));
}

// 짧은 쪽 마스크로 비교한다. 자기 마스크로만 비교하면 대상 안의 호스트 경로를 놓친다.
// 같은 길이 이상인 겹침을 catch-all 보다 먼저 본다. 순서가 판정이다.
[[nodiscard]] constexpr Overlap classify(Prefix prefix, Prefix target) noexcept {
    const std::uint32_t mask = prefix_mask(std::min(prefix.length, target.length));
    if ((prefix.network & mask) != (target.network & mask)) {
        return Overlap::kIgnore;
    }
    if (prefix.length >= target.length) {
        return Overlap::kBlockRoute;
    }
    if (prefix.length <= 1) {
        return Overlap::kCatchAll;
    }
    return Overlap::kBlockShadow;
}

// "a.b.c.d/len". 길이는 2~32 이고 호스트 비트가 0 이어야 한다. 스크립트도 그 밖의 대상을 거부한다.
[[nodiscard]] inline std::optional<Prefix> parse_target(std::string_view text) {
    const auto slash = text.find('/');
    if (slash == std::string_view::npos) {
        return std::nullopt;
    }
    const auto address = network::parse_ipv4(text.substr(0, slash));
    const std::string_view digits = text.substr(slash + 1);
    if (!address || digits.empty() || digits.size() > 2 ||
        !std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        return std::nullopt;
    }
    const int length = digits.size() == 1 ? digits[0] - '0' : (digits[0] - '0') * 10 + (digits[1] - '0');
    if (length < 2 || length > 32) {
        return std::nullopt;
    }
    const Prefix target{*address, static_cast<std::uint8_t>(length)};
    if ((target.network & prefix_mask(target.length)) != target.network) {
        return std::nullopt;
    }
    return target;
}

// 기기 한 대의 판정. routes 는 main 표의 IPv4 경로 전부, addresses 는 모든 인터페이스의 IPv4 주소다.
// query_failure 의 op 가 비지 않으면 자료 일부를 읽지 못한 것이다.
//
// 차단이 하나라도 있으면 kBlock(1) 이다. 아니고 읽지 못한 자료가 있으면 kUnknown(2), 아니면 kProceed(0).
// blocks 는 BLOCK-route 와 BLOCK-shadow 의 접두사다. 주소 차단은 판정에만 들고 blocks 에 넣지 않는다
// (Windows 가 스크립트 출력에서 `.` 과 `/` 를 함께 가진 낱말만 뽑는 것과 같은 결과다).
[[nodiscard]] inline SubnetCheck judge(std::string_view target_text, const std::vector<Prefix>& routes,
                                       const std::vector<std::uint32_t>& addresses, OsFailure query_failure) {
    SubnetCheck result;
    const auto target = parse_target(target_text);
    if (!target) {
        result.exit_code = 2;
        return result;
    }
    bool blocked = false;
    bool unparsed = false;
    for (const Prefix& route : routes) {
        if (route.length > 32) {
            unparsed = true;
            continue;
        }
        const Overlap overlap = classify(route, *target);
        if (overlap == Overlap::kBlockRoute || overlap == Overlap::kBlockShadow) {
            blocked = true;
            result.blocks.push_back(network::format_ipv4(route.network) + "/" + std::to_string(route.length));
        }
    }
    for (const std::uint32_t address : addresses) {
        if (classify(Prefix{address, 32}, *target) != Overlap::kIgnore) {
            blocked = true;
        }
    }
    if (blocked) {
        result.verdict = SubnetVerdict::kBlock;
        result.exit_code = 1;
    } else if (!query_failure.op.empty() || unparsed) {
        result.verdict = SubnetVerdict::kUnknown;
        result.exit_code = 2;
    } else {
        result.verdict = SubnetVerdict::kProceed;
        result.exit_code = 0;
    }
    result.failure = query_failure;
    return result;
}

}  // namespace sangtachi::platform::subnet
