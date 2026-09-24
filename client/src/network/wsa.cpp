#include "sangtachi/network/wsa.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>

#include <string>

namespace sangtachi::network {

static_assert(sizeof(VersionWord) == sizeof(WORD),
              "VersionWord must match the Win32 WORD width");

WsaStartupError::WsaStartupError(int code)
    : std::runtime_error("WSAStartup failed with code " + std::to_string(code)),
      code_(code) {}

WsaContext::WsaContext() {
    WSADATA data{};
    // MAKEWORD(2, 2) = 버전 2.2. Windows 98 이후 모든 대상이 지원한다.
    const int result = WSAStartup(MAKEWORD(2, 2), &data);
    if (result != 0) {
        // 실패한 WSAStartup 은 WSACleanup 을 부르면 안 된다. 짝이 맞지 않는다.
        throw WsaStartupError(result);
    }
    // 이 대입 뒤로 생성자가 하는 일이 없다. 던질 수 있는 것을 여기에 넣지 않는다.
    version_ = data.wVersion;
}

std::string WsaContext::negotiated_version() const {
    // WSADATA::wVersion 은 하위 바이트가 major, 상위 바이트가 minor 다.
    const unsigned major = LOBYTE(version_);
    const unsigned minor = HIBYTE(version_);
    return std::to_string(major) + "." + std::to_string(minor);
}

WsaContext::~WsaContext() {
    WSACleanup();
}

}  // namespace sangtachi::network
