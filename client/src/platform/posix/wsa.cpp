#include "sangtachi/network/wsa.hpp"

#include <string>

// POSIX 소켓은 프로세스 초기화가 없다. 객체는 남겨 부르는 쪽(client_process.cpp)을 OS 와 무관하게 둔다.

namespace sangtachi::network {

WsaStartupError::WsaStartupError(int code)
    : std::runtime_error("WSAStartup failed with code " + std::to_string(code)),
      code_(code) {}

WsaContext::WsaContext() : version_(0) {}

std::string WsaContext::negotiated_version() const {
    // 협상한 버전이 없다. 숫자를 흉내 내면 `wsa.init version=` 줄이 거짓을 말한다.
    return "-";
}

WsaContext::~WsaContext() = default;

}  // namespace sangtachi::network
