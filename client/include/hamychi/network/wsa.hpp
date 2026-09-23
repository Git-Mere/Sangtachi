#pragma once

// Winsock2 초기화와 해제 (roadmap.md Phase 1).
//
// Winsock 은 프로세스마다 WSAStartup 과 WSACleanup 의 호출 수가 맞아야 한다. 짝을
// 손으로 맞추면 조기 반환 하나에 WSACleanup 이 빠진다. 그래서 수명을 객체에 묶는다.

#include <stdexcept>
#include <string>

namespace hamychi::network {

// Win32 WORD 와 같은 폭. <winsock2.h> 를 헤더에 들이지 않으려고 따로 둔다.
// 원본에서 static_assert 로 폭을 대조한다.
using VersionWord = unsigned short;

// WSAStartup 이 실패했을 때 던진다. code 는 WSAStartup 의 반환값이다.
//
// 이 오류만 예외로 다룬다. Winsock 이 없으면 이 프로그램이 할 수 있는 일이 없고,
// 그것은 기동 실패이지 실행 중 처리할 상황이 아니다. 소켓 연산의 실패는 예외가 아니라
// 반환값으로 다룬다 (protocol.md 6장: sendto 실패는 카운터를 올리고 계속한다).
class WsaStartupError : public std::runtime_error {
public:
    explicit WsaStartupError(int code);

    // WSAStartup 의 반환값.
    [[nodiscard]] int code() const noexcept { return code_; }

private:
    int code_;
};

// 생성자가 WSAStartup, 소멸자가 WSACleanup 을 부른다.
//
// 복사도 이동도 하지 않는다. 이 객체는 프로세스 수명과 같고, 옮길 수 있으면 짝이
// 맞는지를 다시 추적해야 한다.
class WsaContext {
public:
    // 요청 버전 2.2 로 WSAStartup 을 부른다. 실패하면 WsaStartupError 를 던진다.
    WsaContext();
    ~WsaContext();

    WsaContext(const WsaContext&) = delete;
    WsaContext& operator=(const WsaContext&) = delete;
    WsaContext(WsaContext&&) = delete;
    WsaContext& operator=(WsaContext&&) = delete;

    // WSADATA 가 보고한 실제 버전. "major.minor" 10진 표기.
    //
    // 요청 버전과 다를 수 있다. Winsock 은 요청값 이하의 가장 높은 버전을 줄 수 있다.
    //
    // 문자열을 멤버로 들고 있지 않고 부를 때 만든다. 생성자가 WSAStartup 성공 뒤에
    // 무엇이든 던지면 그 객체는 완성되지 않아 소멸자가 돌지 않고, 그러면 Winsock
    // 참조 수가 샌다. 만들 것이 없으면 그 창이 닫힌다.
    [[nodiscard]] std::string negotiated_version() const;

private:
    VersionWord version_;
};

}  // namespace hamychi::network
