#pragma once

// 시험용 루프백 TCP 서버. control/exchange.hpp 와 control/channel.hpp 의 시험이 같이 쓴다.
//
// 연결 하나에 대본 하나를 순서대로 쓴다. 대본은 받은 요청 뒤에 무엇을 보내고 언제 닫는지를
// 정한다. 시험 전용이라 OS 헤더를 직접 들인다 (platformgate 는 client/ 만 본다).

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace sangtachi_test {

struct Script {
    std::string reply;           // 요청을 다 받은 뒤 보낼 바이트
    bool silent = false;         // 참이면 아무것도 보내지 않고 서버가 멈출 때까지 붙든다
    std::uint32_t hold_ms = 0;   // 보낸 뒤 닫기 전에 붙드는 시간. EOF 를 늦춘다
    std::uint32_t drip_ms = 0;   // 0 이 아니면 reply 를 한 바이트씩 이 간격으로 보낸다
};

// 응답 하나. 상태 줄과 Content-Length 를 맞춰 만든다.
inline std::string http_reply(std::string_view body) {
    std::string out = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: ";
    out += std::to_string(body.size());
    out += "\r\nConnection: close\r\n\r\n";
    out += body;
    return out;
}

class LoopbackServer {
public:
    explicit LoopbackServer(std::vector<Script> scripts) : scripts_(std::move(scripts)) {
        stop_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        accepted_event_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
        listen_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        ::bind(listen_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
        ::listen(listen_, 16);
        int len = sizeof(addr);
        ::getsockname(listen_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ::ntohs(addr.sin_port);
        thread_ = std::thread([this] { serve(); });
    }

    ~LoopbackServer() { stop(); }

    LoopbackServer(const LoopbackServer&) = delete;
    LoopbackServer& operator=(const LoopbackServer&) = delete;

    void stop() {
        if (thread_.joinable()) {
            ::SetEvent(stop_);
            thread_.join();
        }
        if (listen_ != INVALID_SOCKET) {
            ::closesocket(listen_);
            listen_ = INVALID_SOCKET;
        }
        if (stop_ != nullptr) {
            ::CloseHandle(stop_);
            stop_ = nullptr;
        }
        if (accepted_event_ != nullptr) {
            ::CloseHandle(accepted_event_);
            accepted_event_ = nullptr;
        }
    }

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

    // 다음 연결이 요청을 다 받을 때까지 기다린다 (자동 리셋). 받았으면 참이다.
    bool wait_request(DWORD timeout_ms) { return ::WaitForSingleObject(accepted_event_, timeout_ms) == WAIT_OBJECT_0; }

    // 받은 요청이 n 개가 될 때까지 기다린다. 자동 리셋 이벤트는 깨우기로만 쓴다. 신호가 겹치면
    // 하나로 합쳐지므로 그 횟수로 요청 수를 세지 않는다. 판정은 누적 수다.
    bool wait_requests(std::size_t n, DWORD timeout_ms) {
        const ULONGLONG deadline = ::GetTickCount64() + timeout_ms;
        while (true) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (requests_.size() >= n) {
                    return true;
                }
            }
            const ULONGLONG now = ::GetTickCount64();
            if (now >= deadline) {
                return false;
            }
            const ULONGLONG left = deadline - now;
            ::WaitForSingleObject(accepted_event_, static_cast<DWORD>(left < 50 ? left : 50));
        }
    }

    [[nodiscard]] std::size_t served() const noexcept { return served_.load(); }

    [[nodiscard]] std::vector<std::string> requests() {
        std::lock_guard<std::mutex> lock(mutex_);
        return requests_;
    }

private:
    bool stopped(DWORD wait_ms = 0) { return ::WaitForSingleObject(stop_, wait_ms) == WAIT_OBJECT_0; }

    // 요청 하나를 머리와 Content-Length 본문까지 읽는다. 상한 2초.
    std::string read_request(SOCKET s) {
        std::string got;
        const ULONGLONG deadline = ::GetTickCount64() + 2000;
        while (::GetTickCount64() < deadline && !stopped()) {
            const std::size_t end = got.find("\r\n\r\n");
            if (end != std::string::npos) {
                std::size_t length = 0;
                const std::size_t at = got.find("Content-Length: ");
                if (at != std::string::npos && at < end) {
                    length = std::stoul(got.substr(at + 16));
                }
                if (got.size() >= end + 4 + length) {
                    return got;
                }
            }
            fd_set r;
            FD_ZERO(&r);
            FD_SET(s, &r);
            timeval tv{0, 20000};
            if (::select(0, &r, nullptr, nullptr, &tv) <= 0) {
                continue;
            }
            char buf[1024];
            const int n = ::recv(s, buf, sizeof(buf), 0);
            if (n <= 0) {
                return got;
            }
            got.append(buf, static_cast<std::size_t>(n));
        }
        return got;
    }

    void serve() {
        for (const Script& script : scripts_) {
            SOCKET s = INVALID_SOCKET;
            while (s == INVALID_SOCKET) {
                if (stopped()) {
                    return;
                }
                fd_set r;
                FD_ZERO(&r);
                FD_SET(listen_, &r);
                timeval tv{0, 20000};
                if (::select(0, &r, nullptr, nullptr, &tv) > 0) {
                    s = ::accept(listen_, nullptr, nullptr);
                }
            }
            const std::string request = read_request(s);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                requests_.push_back(request);
            }
            ::SetEvent(accepted_event_);

            if (script.silent) {
                stopped(INFINITE);
            } else if (script.drip_ms != 0) {
                for (const char c : script.reply) {
                    if (::send(s, &c, 1, 0) != 1 || stopped(script.drip_ms)) {
                        break;
                    }
                }
                stopped(script.hold_ms);
            } else {
                std::size_t sent = 0;
                while (sent < script.reply.size()) {
                    const int n = ::send(s, script.reply.data() + sent,
                                         static_cast<int>(script.reply.size() - sent), 0);
                    if (n <= 0) {
                        break;
                    }
                    sent += static_cast<std::size_t>(n);
                }
                stopped(script.hold_ms);
            }
            ++served_;
            // 상대가 보낸 것을 다 읽은 뒤 닫는다. 남긴 채 닫으면 RST 가 응답을 앞지를 수 있다.
            ::shutdown(s, SD_SEND);
            ::closesocket(s);
        }
        stopped(INFINITE);
    }

    std::vector<Script> scripts_;
    SOCKET listen_ = INVALID_SOCKET;
    HANDLE stop_ = nullptr;
    HANDLE accepted_event_ = nullptr;
    std::uint16_t port_ = 0;
    std::atomic<std::size_t> served_{0};
    std::mutex mutex_;
    std::vector<std::string> requests_;
    std::thread thread_;
};

// 지금 아무도 듣지 않는 루프백 포트. bind 해 보고 번호를 읽은 뒤 닫는다.
inline std::uint16_t closed_port() {
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    ::bind(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    int len = sizeof(addr);
    ::getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len);
    ::closesocket(s);
    return ::ntohs(addr.sin_port);
}

}  // namespace sangtachi_test
