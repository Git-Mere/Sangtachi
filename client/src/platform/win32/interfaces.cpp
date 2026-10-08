#include "sangtachi/platform/interfaces.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netioapi.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <vector>

namespace sangtachi::platform {
namespace {

// GetAdaptersAddresses 는 버퍼가 작으면 필요한 크기를 알려 준다. 그 사이 인터페이스가 늘면 다시
// 모자랄 수 있어 몇 번 더 묻는다. 15KB 로 시작하라는 것이 그 API 문서의 권고다.
constexpr ULONG kInitialAdapterBuffer = 15 * 1024;
constexpr int kAdapterAttempts = 3;

// sockaddr_in 은 네트워크 바이트 순서다. 이 레포의 주소는 호스트 순서로 든다
// (platform/win32/resolve.cpp 와 같은 규칙).
std::uint32_t host_order(const sockaddr_in& addr) noexcept {
    return ::ntohl(addr.sin_addr.s_addr);
}

InterfaceGuid to_bytes(const GUID& guid) noexcept {
    InterfaceGuid out{};
    static_assert(sizeof(GUID) == sizeof(InterfaceGuid));
    std::memcpy(out.data(), &guid, sizeof(GUID));
    return out;
}

// IF_OPER_STATUS 의 값을 그대로 옮긴다. 목록 밖의 값은 Up 이 아니므로 kUnknown 이다.
OperStatus to_oper_status(IF_OPER_STATUS status) noexcept {
    switch (status) {
        case IfOperStatusUp:             return OperStatus::kUp;
        case IfOperStatusDown:           return OperStatus::kDown;
        case IfOperStatusTesting:        return OperStatus::kTesting;
        case IfOperStatusDormant:        return OperStatus::kDormant;
        case IfOperStatusNotPresent:     return OperStatus::kNotPresent;
        case IfOperStatusLowerLayerDown: return OperStatus::kLowerLayerDown;
        case IfOperStatusUnknown:        return OperStatus::kUnknown;
    }
    return OperStatus::kUnknown;
}

// NL_DAD_STATE 의 값을 그대로 옮긴다. 목록 밖의 값은 Preferred 가 아니므로 kInvalid 다.
DadState to_dad_state(NL_DAD_STATE state) noexcept {
    switch (state) {
        case IpDadStatePreferred:  return DadState::kPreferred;
        case IpDadStateTentative:  return DadState::kTentative;
        case IpDadStateDuplicate:  return DadState::kDuplicate;
        case IpDadStateDeprecated: return DadState::kDeprecated;
        case IpDadStateInvalid:    return DadState::kInvalid;
    }
    return DadState::kInvalid;
}

InterfaceQuery failed_interfaces(std::uint32_t error) {
    InterfaceQuery q;
    q.ok = false;
    q.error = error;
    return q;
}

RouteQuery failed_routes(std::uint32_t error) {
    RouteQuery q;
    q.ok = false;
    q.error = error;
    return q;
}

}  // namespace

InterfaceQuery query_ipv4_interfaces() {
    // 애니캐스트, 멀티캐스트, DNS 서버 목록은 판정에 쓰지 않는다. 유니캐스트 주소만 읽는다.
    constexpr ULONG kFlags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                             GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_FRIENDLY_NAME;

    // IP_ADAPTER_ADDRESSES 는 8바이트 정렬을 요구한다. unsigned long long 배열로 잡는다.
    std::vector<unsigned long long> buffer;
    ULONG size = kInitialAdapterBuffer;
    ULONG result = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0; attempt < kAdapterAttempts && result == ERROR_BUFFER_OVERFLOW; ++attempt) {
        buffer.assign((size + sizeof(unsigned long long) - 1) / sizeof(unsigned long long), 0);
        size = static_cast<ULONG>(buffer.size() * sizeof(unsigned long long));
        result = ::GetAdaptersAddresses(AF_INET, kFlags, nullptr,
                                        reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    }
    if (result == ERROR_NO_DATA) {
        // IPv4 주소를 가진 인터페이스가 없다. 열거는 성공했다 (10.1 "활성 주소가 없으면").
        InterfaceQuery q;
        q.ok = true;
        return q;
    }
    if (result != NO_ERROR) {
        return failed_interfaces(result);
    }

    InterfaceQuery out;
    out.ok = true;
    for (const IP_ADAPTER_ADDRESSES* a = reinterpret_cast<const IP_ADAPTER_ADDRESSES*>(buffer.data());
         a != nullptr; a = a->Next) {
        InterfaceInfo info;
        info.if_index = a->IfIndex;
        info.if_type = a->IfType;
        info.oper_status = to_oper_status(a->OperStatus);

        // 자기 Wintun 판정의 식별자 (10.1, windows-prereq.md 3절). Wintun 이 생성 때 받는 GUID 가
        // 이 값이다. 읽지 못하면 그 어댑터가 우리 것인지 알 수 없으므로 열거 전체를 실패로 둔다.
        // 남의 것으로 짐작해 넣으면 자기 Wintun 주소가 후보로 나갈 수 있다.
        GUID guid{};
        const NETIO_STATUS converted = ::ConvertInterfaceLuidToGuid(&a->Luid, &guid);
        if (converted != NO_ERROR) {
            return failed_interfaces(static_cast<std::uint32_t>(converted));
        }
        info.guid = to_bytes(guid);

        for (const IP_ADAPTER_UNICAST_ADDRESS* u = a->FirstUnicastAddress; u != nullptr; u = u->Next) {
            const SOCKET_ADDRESS& sa = u->Address;
            if (sa.lpSockaddr == nullptr || sa.lpSockaddr->sa_family != AF_INET ||
                sa.iSockaddrLength < static_cast<INT>(sizeof(sockaddr_in))) {
                continue;
            }
            sockaddr_in addr{};
            std::memcpy(&addr, sa.lpSockaddr, sizeof(addr));
            info.addresses.push_back(InterfaceAddress{host_order(addr), u->OnLinkPrefixLength,
                                                      to_dad_state(u->DadState)});
        }
        if (!info.addresses.empty()) {
            out.interfaces.push_back(std::move(info));
        }
    }
    return out;
}

RouteQuery query_ipv4_default_routes() {
    MIB_IPFORWARD_TABLE2* table = nullptr;
    const NETIO_STATUS got = ::GetIpForwardTable2(AF_INET, &table);
    if (got == ERROR_NOT_FOUND) {
        // 경로가 하나도 없다. 기본 경로가 없는 것이고 조회는 성공이다.
        RouteQuery q;
        q.ok = true;
        return q;
    }
    if (got != NO_ERROR || table == nullptr) {
        return failed_routes(static_cast<std::uint32_t>(got));
    }
    // 표는 FreeMibTable 로 돌려준다. 아래의 어느 길로 나가도 돌려주도록 가드에 맡긴다.
    const std::unique_ptr<MIB_IPFORWARD_TABLE2, decltype(&::FreeMibTable)> owned(table, &::FreeMibTable);

    RouteQuery out;
    out.ok = true;
    for (ULONG i = 0; i < table->NumEntries; ++i) {
        const MIB_IPFORWARD_ROW2& row = table->Table[i];
        const IP_ADDRESS_PREFIX& prefix = row.DestinationPrefix;
        if (prefix.Prefix.si_family != AF_INET || prefix.PrefixLength != 0 ||
            prefix.Prefix.Ipv4.sin_addr.s_addr != 0) {
            continue;  // 0.0.0.0/0 만 (헤더의 계약)
        }
        // 인터페이스 메트릭. 경로 메트릭과 더한 값이 10.1 의 비교 기준이다.
        MIB_IPINTERFACE_ROW iface{};
        ::InitializeIpInterfaceEntry(&iface);
        iface.Family = AF_INET;
        iface.InterfaceLuid = row.InterfaceLuid;
        const NETIO_STATUS entry = ::GetIpInterfaceEntry(&iface);
        if (entry != NO_ERROR) {
            // 합을 모르면 어느 줄이 기본 경로인지 판정할 수 없다. 조회 실패로 둔다.
            // 10.1 이 그것을 "기본 경로가 없는 것과 같이" 다룬다.
            return failed_routes(static_cast<std::uint32_t>(entry));
        }
        out.routes.push_back(Ipv4Route{0, 0, row.InterfaceIndex, row.Metric, iface.Metric});
    }
    return out;
}

}  // namespace sangtachi::platform
