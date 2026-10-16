#include "sangtachi/platform/tun.hpp"

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
#include <wintun.h>

#include "sangtachi/hash.hpp"
#include "sangtachi/wintun_expected.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <span>
#include <type_traits>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace sangtachi::platform {
namespace {

// 32767 자가 확장 경로의 상한이다. 끝의 널 한 자리를 더한다 (record_file.cpp 와 같다).
constexpr DWORD kPathBuffer = 32768;

// 세션 링 용량. Wintun 의 예제와 같은 4 MiB 다. 2의 거듭제곱이고 WINTUN_MIN_RING_CAPACITY 와
// WINTUN_MAX_RING_CAPACITY 사이여야 한다.
constexpr DWORD kRingCapacity = 0x400000;
static_assert((kRingCapacity & (kRingCapacity - 1)) == 0);
static_assert(kRingCapacity >= WINTUN_MIN_RING_CAPACITY && kRingCapacity <= WINTUN_MAX_RING_CAPACITY);

// 주소가 Preferred 가 되기를 기다리는 상한과 간격 (windows-prereq.md 9절). DAD 를 꺼 두므로 보통은
// 첫 조회에서 끝난다.
constexpr DWORD kPreferredCapMs = 10000;
constexpr DWORD kPreferredPollMs = 50;

// 서브넷 충돌 스크립트의 상한. 이 기기에서 한 번에 1~4초가 걸렸다.
constexpr DWORD kSubnetCheckCapMs = 30000;

// 가상 LAN 의 접두사 길이 (architecture.md 7장 가상 네트워크 설계의 /24).
constexpr std::uint8_t kPrefixLength = 24;

OsFailure os_failure(std::string_view op, DWORD code) {
    return OsFailure{op, static_cast<std::uint32_t>(code)};
}

std::wstring widen_ascii(std::string_view text) {
    std::wstring out;
    out.reserve(text.size());
    for (const char c : text) {
        out += static_cast<wchar_t>(static_cast<unsigned char>(c));
    }
    return out;
}

// 실행 파일의 폴더. 끝에 역슬래시가 붙는다. 실패하면 빈 문자열이다.
std::wstring exe_dir() {
    std::wstring path(kPathBuffer, L'\0');
    const DWORD length = ::GetModuleFileNameW(nullptr, path.data(), kPathBuffer);
    if (length == 0 || length >= kPathBuffer) {
        return {};
    }
    path.resize(length);
    const auto slash = path.rfind(L'\\');
    if (slash == std::wstring::npos) {
        return {};
    }
    path.resize(slash + 1);
    return path;
}

std::uint32_t network_order(std::uint32_t host) noexcept {
    return ::htonl(host);
}

// ---------------------------------------------------------------- 서브넷 충돌 스크립트

// 핸들 하나를 스코프 끝에서 닫는다.
struct HandleCloser {
    HANDLE handle = nullptr;
    ~HandleCloser() {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
            ::CloseHandle(handle);
        }
    }
};

// 지금 환경에서 PSModulePath 만 뺀 유니코드 환경 블록.
//
// pwsh 에서 띄운 프로세스는 그 값이 pwsh 의 모듈 경로를 가리킨다. 그것을 물려받은 Windows
// PowerShell 은 자기 모듈을 불러오지 못한다. 빌드의 서명 판정에서 실측으로 겪었다 (cmake/Wintun.cmake).
std::vector<wchar_t> environment_without_psmodulepath() {
    std::vector<wchar_t> block;
    wchar_t* strings = ::GetEnvironmentStringsW();
    if (strings == nullptr) {
        block.push_back(L'\0');
        block.push_back(L'\0');
        return block;
    }
    constexpr std::wstring_view kSkip = L"psmodulepath=";
    for (const wchar_t* entry = strings; *entry != L'\0';) {
        const std::wstring_view line(entry);
        std::wstring lower(line.substr(0, kSkip.size()));
        for (auto& ch : lower) {
            ch = static_cast<wchar_t>(::towlower(ch));
        }
        if (lower != kSkip) {
            block.insert(block.end(), line.begin(), line.end());
            block.push_back(L'\0');
        }
        entry += line.size() + 1;
    }
    ::FreeEnvironmentStringsW(strings);
    block.push_back(L'\0');
    if (block.size() == 1) {
        block.push_back(L'\0');
    }
    return block;
}

// 스크립트 출력에서 BLOCK 줄의 접두사를 뽑는다. 줄은 `  BLOCK-route   /24  10.100.0.0/24  ...` 모양이다.
// 출력은 콘솔 코드 페이지라 이름 칸이 깨질 수 있다. 점과 빗금이 든 첫 낱말만 쓴다.
std::vector<std::string> block_prefixes(const std::string& output) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start < output.size()) {
        std::size_t end = output.find('\n', start);
        if (end == std::string::npos) {
            end = output.size();
        }
        std::string_view line(output.data() + start, end - start);
        start = end + 1;
        const auto first = line.find_first_not_of(" \t");
        if (first == std::string_view::npos || line.substr(first, 6) != "BLOCK-") {
            continue;
        }
        std::size_t pos = first;
        while (pos < line.size()) {
            const auto word_start = line.find_first_not_of(" \t\r", pos);
            if (word_start == std::string_view::npos) {
                break;
            }
            auto word_end = line.find_first_of(" \t\r", word_start);
            if (word_end == std::string_view::npos) {
                word_end = line.size();
            }
            const auto word = line.substr(word_start, word_end - word_start);
            if (word.find('.') != std::string_view::npos && word.find('/') != std::string_view::npos) {
                out.emplace_back(word);
                break;
            }
            pos = word_end;
        }
    }
    return out;
}

// ---------------------------------------------------------------- 주소

std::uint32_t prefix_mask(std::uint8_t length) noexcept {
    return length == 0 ? 0u : (0xFFFFFFFFu << (32 - length));
}

void fill_ipv4(SOCKADDR_INET& out, std::uint32_t host) noexcept {
    std::memset(&out, 0, sizeof(out));
    out.Ipv4.sin_family = AF_INET;
    out.Ipv4.sin_addr.s_addr = network_order(host);
}

// 그 어댑터에 붙은 IPv4 유니캐스트 주소를 모두 지운다. 남은 주소의 정리다 (windows-prereq.md 3절).
OsFailure delete_all_ipv4_addresses(const NET_LUID& luid) {
    PMIB_UNICASTIPADDRESS_TABLE table = nullptr;
    const DWORD got = ::GetUnicastIpAddressTable(AF_INET, &table);
    if (got == ERROR_NOT_FOUND) {
        return {};
    }
    if (got != NO_ERROR) {
        return os_failure("GetUnicastIpAddressTable", got);
    }
    OsFailure failure;
    for (ULONG i = 0; i < table->NumEntries; ++i) {
        MIB_UNICASTIPADDRESS_ROW& row = table->Table[i];
        if (row.InterfaceLuid.Value != luid.Value) {
            continue;
        }
        const DWORD deleted = ::DeleteUnicastIpAddressEntry(&row);
        if (deleted != NO_ERROR && deleted != ERROR_NOT_FOUND && failure.op.empty()) {
            failure = os_failure("DeleteUnicastIpAddressEntry", deleted);
        }
    }
    ::FreeMibTable(table);
    return failure;
}

// 그 어댑터에 그 목적지 접두사의 IPv4 경로가 있는가 (windows-prereq.md 8절의 개수 판정).
// 조회가 실패하면 failure 가 채워진다. "없음" 과 "못 봤음" 을 섞지 않는다.
bool has_route(const NET_LUID& luid, std::uint32_t destination, std::uint8_t length, OsFailure& failure) {
    PMIB_IPFORWARD_TABLE2 table = nullptr;
    const DWORD got = ::GetIpForwardTable2(AF_INET, &table);
    if (got == ERROR_NOT_FOUND) {
        return false;
    }
    if (got != NO_ERROR) {
        failure = os_failure("GetIpForwardTable2", got);
        return false;
    }
    bool found = false;
    for (ULONG i = 0; i < table->NumEntries && !found; ++i) {
        const MIB_IPFORWARD_ROW2& row = table->Table[i];
        found = row.InterfaceLuid.Value == luid.Value &&
                row.DestinationPrefix.PrefixLength == length &&
                ::ntohl(row.DestinationPrefix.Prefix.Ipv4.sin_addr.s_addr) == destination;
    }
    ::FreeMibTable(table);
    return found;
}

MIB_IPFORWARD_ROW2 route_row(const NET_LUID& luid, std::uint32_t destination, std::uint8_t length) {
    MIB_IPFORWARD_ROW2 row;
    ::InitializeIpForwardEntry(&row);
    row.InterfaceLuid = luid;
    fill_ipv4(row.DestinationPrefix.Prefix, destination);
    row.DestinationPrefix.PrefixLength = length;
    fill_ipv4(row.NextHop, 0);
    row.Protocol = MIB_IPPROTO_NETMGMT;
    row.Metric = 0;
    return row;
}

}  // namespace

// ---------------------------------------------------------------- Wintun 함수표

struct TunDevice::Api {
    HMODULE module = nullptr;
    WINTUN_CREATE_ADAPTER_FUNC* create_adapter = nullptr;
    WINTUN_CLOSE_ADAPTER_FUNC* close_adapter = nullptr;
    WINTUN_GET_ADAPTER_LUID_FUNC* get_luid = nullptr;
    WINTUN_START_SESSION_FUNC* start_session = nullptr;
    WINTUN_END_SESSION_FUNC* end_session = nullptr;
    WINTUN_GET_READ_WAIT_EVENT_FUNC* read_wait_event = nullptr;
    WINTUN_RECEIVE_PACKET_FUNC* receive_packet = nullptr;
    WINTUN_RELEASE_RECEIVE_PACKET_FUNC* release_receive_packet = nullptr;
    WINTUN_ALLOCATE_SEND_PACKET_FUNC* allocate_send_packet = nullptr;
    WINTUN_SEND_PACKET_FUNC* send_packet = nullptr;

    ~Api() {
        if (module != nullptr) {
            ::FreeLibrary(module);
        }
    }
};

bool is_elevated() noexcept {
    HANDLE token = nullptr;
    if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token) == 0) {
        return false;
    }
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const BOOL ok = ::GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
    ::CloseHandle(token);
    return ok != 0 && elevation.TokenIsElevated != 0;
}

SubnetCheck run_subnet_check(std::string_view target) {
    SubnetCheck result;
    const std::wstring dir = exe_dir();
    if (dir.empty()) {
        result.failure = os_failure("GetModuleFileNameW", ::GetLastError());
        return result;
    }
    wchar_t system_dir[MAX_PATH] = {};
    const UINT system_length = ::GetSystemDirectoryW(system_dir, MAX_PATH);
    if (system_length == 0 || system_length >= MAX_PATH) {
        result.failure = os_failure("GetSystemDirectoryW", ::GetLastError());
        return result;
    }
    // 경로를 다 적는다. PATH 에서 찾으면 남이 둔 powershell.exe 가 판정을 낼 수 있다.
    const std::wstring powershell = std::wstring(system_dir) + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
    std::wstring command = L"\"" + powershell +
                           L"\" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"" + dir +
                           L"Test-SubnetOverlap.ps1\" -Target " + widen_ascii(target) +
                           L" -OwnInterfaceGuid " + widen_ascii(adapter::kGuidText) + L" -OwnAlias " +
                           widen_ascii(adapter::kName);

    // 출력은 지울 임시 파일로 받는다. 파이프는 버퍼가 차면 자식이 멈추고 그러면 상한까지 기다린다.
    wchar_t temp_dir[MAX_PATH + 1] = {};
    wchar_t temp_file[MAX_PATH + 1] = {};
    if (::GetTempPathW(MAX_PATH + 1, temp_dir) == 0 ||
        ::GetTempFileNameW(temp_dir, L"stc", 0, temp_file) == 0) {
        result.failure = os_failure("GetTempFileNameW", ::GetLastError());
        return result;
    }
    SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HandleCloser output{::CreateFileW(temp_file, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                      &inherit, CREATE_ALWAYS,
                                      FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr)};
    if (output.handle == INVALID_HANDLE_VALUE) {
        result.failure = os_failure("CreateFileW", ::GetLastError());
        return result;
    }
    HandleCloser input{::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
                                     OPEN_EXISTING, 0, nullptr)};
    if (input.handle == INVALID_HANDLE_VALUE) {
        result.failure = os_failure("CreateFileW", ::GetLastError());
        return result;
    }

    // 물려줄 핸들을 이 둘로 좁힌다. 그러지 않으면 상속 가능한 소켓까지 자식에게 간다.
    SIZE_T list_size = 0;
    ::InitializeProcThreadAttributeList(nullptr, 1, 0, &list_size);
    std::vector<std::byte> list_storage(list_size);
    auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(list_storage.data());
    if (::InitializeProcThreadAttributeList(list, 1, 0, &list_size) == 0) {
        result.failure = os_failure("InitializeProcThreadAttributeList", ::GetLastError());
        return result;
    }
    HANDLE inherited[] = {input.handle, output.handle};
    if (::UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited),
                                    nullptr, nullptr) == 0) {
        result.failure = os_failure("UpdateProcThreadAttribute", ::GetLastError());
        ::DeleteProcThreadAttributeList(list);
        return result;
    }

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input.handle;
    startup.StartupInfo.hStdOutput = output.handle;
    startup.StartupInfo.hStdError = output.handle;
    startup.lpAttributeList = list;
    std::vector<wchar_t> environment = environment_without_psmodulepath();
    PROCESS_INFORMATION process{};
    const BOOL created = ::CreateProcessW(powershell.c_str(), command.data(), nullptr, nullptr, TRUE,
                                          CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT |
                                              EXTENDED_STARTUPINFO_PRESENT,
                                          environment.data(), nullptr, &startup.StartupInfo, &process);
    const DWORD create_error = ::GetLastError();
    ::DeleteProcThreadAttributeList(list);
    if (created == 0) {
        result.failure = os_failure("CreateProcessW", create_error);
        return result;
    }
    HandleCloser process_handle{process.hProcess};
    HandleCloser thread_handle{process.hThread};

    if (::WaitForSingleObject(process.hProcess, kSubnetCheckCapMs) != WAIT_OBJECT_0) {
        ::TerminateProcess(process.hProcess, 1);
        ::WaitForSingleObject(process.hProcess, 5000);
        result.failure = os_failure("WaitForSingleObject", WAIT_TIMEOUT);
        return result;
    }
    DWORD code = 0;
    if (::GetExitCodeProcess(process.hProcess, &code) == 0) {
        result.failure = os_failure("GetExitCodeProcess", ::GetLastError());
        return result;
    }
    result.exit_code = static_cast<std::uint32_t>(code);
    result.verdict = code == 0 ? SubnetVerdict::kProceed
                     : code == 1 ? SubnetVerdict::kBlock
                                 : SubnetVerdict::kUnknown;

    // 출력은 진단에만 쓴다. 판정은 위의 종료 코드다.
    std::string text;
    ::SetFilePointer(output.handle, 0, nullptr, FILE_BEGIN);
    char buffer[4096];
    DWORD read = 0;
    while (::ReadFile(output.handle, buffer, sizeof(buffer), &read, nullptr) != 0 && read > 0) {
        text.append(buffer, read);
    }
    result.blocks = block_prefixes(text);
    return result;
}

// ---------------------------------------------------------------- TunDevice

TunDevice::Opened TunDevice::open(std::uint32_t mtu) {
    Opened opened;
    auto device = std::unique_ptr<TunDevice>(new TunDevice());
    device->api_ = std::make_unique<Api>();
    Api& api = *device->api_;

    const std::wstring dir = exe_dir();
    if (dir.empty()) {
        opened.failure = os_failure("GetModuleFileNameW", ::GetLastError());
        return opened;
    }
    const std::wstring dll_path = dir + L"wintun.dll";

    // 실행 시 확인 (windows-prereq.md 4절). 부르기 전에 빌드가 판정한 그 파일인지 해시로 본다.
    {
        std::ifstream file(dll_path, std::ios::binary);
        if (!file) {
            opened.failure = os_failure("wintun.dll.open", ERROR_FILE_NOT_FOUND);
            return opened;
        }
        const std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        const auto hash = sha256_hex(std::as_bytes(std::span<const char>(bytes)));
        if (!hash || *hash != kWintunDllSha256) {
            opened.failure = os_failure("wintun.dll.sha256", ERROR_INVALID_IMAGE_HASH);
            return opened;
        }
    }

    // 실행 파일의 폴더에서만 찾는다. 경로를 다 주고, 그 DLL 이 부르는 것은 System32 에서만 찾는다.
    api.module = ::LoadLibraryExW(dll_path.c_str(), nullptr,
                                  LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (api.module == nullptr) {
        opened.failure = os_failure("LoadLibraryExW", ::GetLastError());
        return opened;
    }
    const auto load = [&api](auto*& slot, const char* name) {
        slot = reinterpret_cast<std::remove_reference_t<decltype(slot)>>(::GetProcAddress(api.module, name));
        return slot != nullptr;
    };
    if (!load(api.create_adapter, "WintunCreateAdapter") || !load(api.close_adapter, "WintunCloseAdapter") ||
        !load(api.get_luid, "WintunGetAdapterLUID") || !load(api.start_session, "WintunStartSession") ||
        !load(api.end_session, "WintunEndSession") || !load(api.read_wait_event, "WintunGetReadWaitEvent") ||
        !load(api.receive_packet, "WintunReceivePacket") ||
        !load(api.release_receive_packet, "WintunReleaseReceivePacket") ||
        !load(api.allocate_send_packet, "WintunAllocateSendPacket") || !load(api.send_packet, "WintunSendPacket")) {
        opened.failure = os_failure("GetProcAddress", ::GetLastError());
        return opened;
    }

    GUID requested{};
    static_assert(sizeof(GUID) == sizeof(adapter::kGuid));
    std::memcpy(&requested, adapter::kGuid.data(), sizeof(GUID));
    const std::wstring name = widen_ascii(adapter::kName);
    const std::wstring tunnel_type = widen_ascii(adapter::kTunnelType);
    device->adapter_ = api.create_adapter(name.c_str(), tunnel_type.c_str(), &requested);
    if (device->adapter_ == nullptr) {
        opened.failure = os_failure("WintunCreateAdapter", ::GetLastError());
        return opened;
    }

    NET_LUID luid{};
    api.get_luid(static_cast<WINTUN_ADAPTER_HANDLE>(device->adapter_), &luid);
    device->luid_ = luid.Value;
    GUID actual{};
    if (const DWORD got = ::ConvertInterfaceLuidToGuid(&luid, &actual); got != NO_ERROR) {
        opened.failure = os_failure("ConvertInterfaceLuidToGuid", got);
        return opened;
    }
    std::memcpy(device->guid_.data(), &actual, sizeof(GUID));

    // MTU 는 protocol.md 12장 기본값이다. DAD 는 끈다 (windows-prereq.md 9절). 점대점 터널 어댑터라
    // 중복 주소를 물어볼 상대가 없고, 켜 두면 주소마다 Tentative 를 기다린다. 이 인터페이스에만 걸린다.
    {
        MIB_IPINTERFACE_ROW row;
        ::InitializeIpInterfaceEntry(&row);
        row.Family = AF_INET;
        row.InterfaceLuid = luid;
        if (const DWORD got = ::GetIpInterfaceEntry(&row); got != NO_ERROR) {
            opened.failure = os_failure("GetIpInterfaceEntry", got);
            return opened;
        }
        row.NlMtu = mtu;
        row.DadTransmits = 0;
        // IPv4 에서는 이 값이 0 이어야 SetIpInterfaceEntry 가 받는다.
        row.SitePrefixLength = 0;
        if (const DWORD set = ::SetIpInterfaceEntry(&row); set != NO_ERROR) {
            opened.failure = os_failure("SetIpInterfaceEntry", set);
            return opened;
        }
    }

    // 같은 GUID 에 묶여 남아 있던 주소를 지운다 (windows-prereq.md 3절).
    if (const OsFailure cleaned = delete_all_ipv4_addresses(luid); !cleaned.op.empty()) {
        opened.failure = cleaned;
        return opened;
    }

    device->session_ = api.start_session(static_cast<WINTUN_ADAPTER_HANDLE>(device->adapter_), kRingCapacity);
    if (device->session_ == nullptr) {
        opened.failure = os_failure("WintunStartSession", ::GetLastError());
        return opened;
    }
    opened.device = std::move(device);
    return opened;
}

TunDevice::~TunDevice() {
    (void)close();
}

void* TunDevice::read_event() const noexcept {
    if (session_ == nullptr) {
        return nullptr;
    }
    return api_->read_wait_event(static_cast<WINTUN_SESSION_HANDLE>(session_));
}

adapter::Received TunDevice::receive() noexcept {
    if (session_ == nullptr) {
        return adapter::Received{adapter::ReceiveStatus::kEof, {}, ERROR_HANDLE_EOF};
    }
    DWORD size = 0;
    BYTE* packet = api_->receive_packet(static_cast<WINTUN_SESSION_HANDLE>(session_), &size);
    if (packet != nullptr) {
        return adapter::Received{adapter::ReceiveStatus::kPacket,
                                 std::span<const std::byte>(reinterpret_cast<const std::byte*>(packet), size), 0};
    }
    const DWORD error = ::GetLastError();
    switch (error) {
        case ERROR_NO_MORE_ITEMS:
            return adapter::Received{adapter::ReceiveStatus::kEmpty, {}, 0};
        case ERROR_HANDLE_EOF:
            return adapter::Received{adapter::ReceiveStatus::kEof, {}, error};
        case ERROR_INVALID_DATA:
            return adapter::Received{adapter::ReceiveStatus::kCorrupt, {}, error};
        default:
            return adapter::Received{adapter::ReceiveStatus::kError, {}, error};
    }
}

void TunDevice::release(std::span<const std::byte> packet) noexcept {
    if (session_ == nullptr || packet.empty()) {
        return;
    }
    api_->release_receive_packet(static_cast<WINTUN_SESSION_HANDLE>(session_),
                                 reinterpret_cast<const BYTE*>(packet.data()));
}

adapter::InjectResult TunDevice::inject(std::span<const std::byte> packet) noexcept {
    if (session_ == nullptr) {
        return adapter::InjectResult{adapter::InjectStatus::kError, ERROR_HANDLE_EOF};
    }
    // Wintun 은 이 상한을 검사하지 않는다. 부르는 쪽의 의무다 (concurrency.md 3장).
    if (packet.empty() || packet.size() > WINTUN_MAX_IP_PACKET_SIZE) {
        return adapter::InjectResult{adapter::InjectStatus::kError, ERROR_INVALID_PARAMETER};
    }
    BYTE* slot = api_->allocate_send_packet(static_cast<WINTUN_SESSION_HANDLE>(session_),
                                            static_cast<DWORD>(packet.size()));
    if (slot == nullptr) {
        const DWORD error = ::GetLastError();
        return adapter::InjectResult{
            error == ERROR_BUFFER_OVERFLOW ? adapter::InjectStatus::kRingFull : adapter::InjectStatus::kError,
            error};
    }
    std::memcpy(slot, packet.data(), packet.size());
    api_->send_packet(static_cast<WINTUN_SESSION_HANDLE>(session_), slot);
    return adapter::InjectResult{adapter::InjectStatus::kOk, 0};
}

bool TunDevice::restart_session() noexcept {
    if (adapter_ == nullptr) {
        return false;
    }
    if (session_ != nullptr) {
        api_->end_session(static_cast<WINTUN_SESSION_HANDLE>(session_));
        session_ = nullptr;
    }
    session_ = api_->start_session(static_cast<WINTUN_ADAPTER_HANDLE>(adapter_), kRingCapacity);
    return session_ != nullptr;
}

AddressResult TunDevice::assign_address(std::uint32_t ip) {
    AddressResult result;
    if (const OsFailure cleared = clear_address(); !cleared.op.empty()) {
        result.failure = cleared;
        result.reason = "clear";
        return result;
    }
    NET_LUID luid{};
    luid.Value = luid_;

    MIB_UNICASTIPADDRESS_ROW row;
    ::InitializeUnicastIpAddressEntry(&row);
    fill_ipv4(row.Address, ip);
    row.InterfaceLuid = luid;
    row.OnLinkPrefixLength = kPrefixLength;
    row.DadState = IpDadStatePreferred;
    if (const DWORD created = ::CreateUnicastIpAddressEntry(&row); created != NO_ERROR) {
        result.failure = os_failure("CreateUnicastIpAddressEntry", created);
        result.reason = "create";
        return result;
    }
    address_ = ip;

    // Preferred 를 기다린다. 끝나는 조건이 셋이다 (windows-prereq.md 9절).
    const ULONGLONG deadline = ::GetTickCount64() + kPreferredCapMs;
    for (;;) {
        MIB_UNICASTIPADDRESS_ROW probe;
        ::InitializeUnicastIpAddressEntry(&probe);
        probe.Address = row.Address;
        probe.InterfaceLuid = luid;
        const DWORD got = ::GetUnicastIpAddressEntry(&probe);
        if (got != NO_ERROR) {
            result.failure = os_failure("GetUnicastIpAddressEntry", got);
            result.reason = "query";
            break;
        }
        if (probe.DadState == IpDadStatePreferred) {
            break;
        }
        if (probe.DadState == IpDadStateDuplicate || probe.DadState == IpDadStateInvalid) {
            result.reason = probe.DadState == IpDadStateDuplicate ? "duplicate" : "invalid";
            break;
        }
        if (::GetTickCount64() >= deadline) {
            result.reason = "timeout";
            break;
        }
        ::Sleep(kPreferredPollMs);
    }
    if (!result.reason.empty()) {
        (void)clear_address();
        return result;
    }

    // on-link 경로는 /24 주소를 붙이면 Windows 가 만든다. 없을 때만 만든다 (windows-prereq.md 8절).
    const std::uint32_t destination = ip & prefix_mask(kPrefixLength);
    OsFailure query;
    const bool present = has_route(luid, destination, kPrefixLength, query);
    if (!query.op.empty()) {
        result.failure = query;
        result.reason = "route_query";
        (void)clear_address();
        return result;
    }
    if (!present) {
        MIB_IPFORWARD_ROW2 route = route_row(luid, destination, kPrefixLength);
        if (const DWORD created = ::CreateIpForwardEntry2(&route); created != NO_ERROR) {
            result.failure = os_failure("CreateIpForwardEntry2", created);
            result.reason = "route_create";
            (void)clear_address();
            return result;
        }
        route_created_ = true;
    }
    result.ok = true;
    result.route_created = route_created_;
    return result;
}

OsFailure TunDevice::clear_address() {
    if (address_ == 0) {
        return {};
    }
    NET_LUID luid{};
    luid.Value = luid_;
    // 지우지 못한 것은 기억해 둔다. 다음 clear_address 나 close 가 다시 지운다.
    OsFailure failure;
    if (route_created_) {
        MIB_IPFORWARD_ROW2 route = route_row(luid, address_ & prefix_mask(kPrefixLength), kPrefixLength);
        const DWORD deleted = ::DeleteIpForwardEntry2(&route);
        if (deleted == NO_ERROR || deleted == ERROR_NOT_FOUND) {
            route_created_ = false;
        } else {
            failure = os_failure("DeleteIpForwardEntry2", deleted);
        }
    }
    MIB_UNICASTIPADDRESS_ROW row;
    ::InitializeUnicastIpAddressEntry(&row);
    fill_ipv4(row.Address, address_);
    row.InterfaceLuid = luid;
    const DWORD deleted = ::DeleteUnicastIpAddressEntry(&row);
    if (deleted == NO_ERROR || deleted == ERROR_NOT_FOUND) {
        if (!route_created_) {
            address_ = 0;
        }
    } else if (failure.op.empty()) {
        failure = os_failure("DeleteUnicastIpAddressEntry", deleted);
    }
    return failure;
}

OsFailure TunDevice::close() noexcept {
    OsFailure failure;
    if (!api_) {
        return failure;
    }
    // concurrency.md 7장 종료의 (5) 세션, (6) 주소와 경로, 그다음 어댑터 순서다.
    if (session_ != nullptr) {
        api_->end_session(static_cast<WINTUN_SESSION_HANDLE>(session_));
        session_ = nullptr;
    }
    if (adapter_ != nullptr) {
        failure = clear_address();
        api_->close_adapter(static_cast<WINTUN_ADAPTER_HANDLE>(adapter_));
        adapter_ = nullptr;
    }
    return failure;
}

}  // namespace sangtachi::platform
