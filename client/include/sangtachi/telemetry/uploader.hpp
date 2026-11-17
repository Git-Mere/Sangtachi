#pragma once

// `[telemetry]` 스레드와 텔레메트리 큐 (concurrency.md 6장 텔레메트리 격리, 7장 종료, architecture.md 6.3).
//
// | 항목 | 이 파일 |
// |------|---------|
// | 큐 | SpscRing 256 항목. 생산자 `[loop]`, 소비자 `[telemetry]` |
// | 가득 찼을 때 | push 가 새 레코드를 버리고 telemetry_queue_dropped 를 올린다. 기다리지 않는다 |
// | 깨우기 | 종료 이벤트를 1초 상한으로 기다리고 깨면 큐를 비운다. push 마다 신호하지 않는다 |
// | 업로드 | 레코드마다 서비스 주소를 이 스레드에서 해석하고 새 TCP 연결로 `POST /v1/report` 하나 (control/exchange.hpp) |
// | 시간 제한 | connect, 송신, 수신 모두 1.5초 |
// | 응답 | control_plane.md 3.5 의 1~6. `ok` 가 true 가 아니면 실패다 |
// | 실패 | 재시도하지 않는다. telemetry_upload_failed 를 올리고 WARN `telemetry.upload_failed` 한 줄 |
// | 종료 | 업로드 사이마다 종료 이벤트를 본다. 보면 정리 완료(cleanup_done)를 기다린 뒤 남은 것을 비우고 끝난다 |
//
// **telemetry_upload_failed 는 이 스레드가 올리는 원자 변수다.** `[loop]` 는 읽어 카운터 표에 set 한다.
// console_queue_dropped 와 같은 예외다 (concurrency.md 6장).
//
// ## 스레드 경계
//
// | 함수 | 부르는 스레드 |
// |------|---------------|
// | start | 기동 중의 주 스레드 |
// | push | `[loop]` 만 |
// | upload_failed | 아무 스레드. `[loop]` 가 카운터를 낼 때 읽는다 |
// | cleanup_done, stop, join_for, 소멸자 | 종료 중의 주 스레드 |
//
// Winsock 참조와 종료 이벤트 복제본을 따로 드는 이유는 control/channel.hpp 와 같다.

#include "sangtachi/control/exchange.hpp"
#include "sangtachi/counters.hpp"
#include "sangtachi/network/wsa.hpp"
#include "sangtachi/platform/wait.hpp"
#include "sangtachi/spsc_ring.hpp"
#include "sangtachi/telemetry/telemetry.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

namespace sangtachi::telemetry {

// concurrency.md 6장 "링은 256 항목이다".
inline constexpr std::size_t kQueueCapacity = 256;
// concurrency.md 7장. 종료 이벤트를 기다리는 상한.
inline constexpr std::uint32_t kWakeMs = 1000;
// concurrency.md 7장 `SO_SNDTIMEO` / `SO_RCVTIMEO` 1.5초. connect 도 같은 값이다.
inline constexpr std::uint32_t kSocketTimeoutMs = 1500;

// 큐의 항목. kind 는 실패 로그가 싣는다.
struct QueuedRecord {
    Kind kind = Kind::kEstablish;
    std::string body;  // to_json 이 만든 본문
};

class Uploader {
public:
    // `[telemetry]` 를 띄운다. shutdown_event 는 복제해서 든다. host 와 port 는 기동 인자 검사를 지난 값이다.
    // Host 헤더를 만들 수 없거나 이벤트나 스레드를 만들지 못하면 널이다.
    // Winsock 초기화가 실패하면 network::WsaStartupError 를 던진다.
    [[nodiscard]] static std::unique_ptr<Uploader> start(
        std::string host, std::uint16_t port, platform::WaitHandle shutdown_event, Counters& counters,
        const control::ExchangeTimeouts& timeouts = {kSocketTimeoutMs, kSocketTimeoutMs});

    ~Uploader();

    Uploader(const Uploader&) = delete;
    Uploader& operator=(const Uploader&) = delete;
    Uploader(Uploader&&) = delete;
    Uploader& operator=(Uploader&&) = delete;

    // 레코드를 넣는다. 가득 찼으면 버리고 telemetry_queue_dropped 를 올린 뒤 거짓이다.
    bool push(QueuedRecord record);

    [[nodiscard]] std::uint64_t upload_failed() const noexcept {
        return upload_failed_.load(std::memory_order_relaxed);
    }

    // concurrency.md 7장 종료의 (7). 이 뒤에 큐에 남은 것을 비운다.
    void cleanup_done();

    // 종료 절차를 거치지 않고 끝낼 때(기동 실패 경로). 종료와 정리 완료를 함께 신호한다. 이 뒤에 join_for 로 기다린다.
    void stop();

    // `[telemetry]` 가 끝나기를 timeout_ms 까지 기다린다. 끝났으면 join 하고 참이다.
    [[nodiscard]] bool join_for(std::uint32_t timeout_ms);

private:
    Uploader(std::string host, std::uint16_t port, std::string host_header, Counters& counters,
             const control::ExchangeTimeouts& timeouts);

    void run();
    // 업로드 하나. 실패면 세고 로그 한 줄을 낸다.
    void upload(const QueuedRecord& record);
    [[nodiscard]] bool shutdown_signaled() const noexcept;

    network::WsaContext wsa_;
    std::string host_;
    std::uint16_t port_;
    std::string host_header_;
    Counters& counters_;  // `[loop]` 소유. push 에서만 만진다
    control::ExchangeTimeouts timeouts_;

    platform::WaitHandle shutdown_event_ = nullptr;  // 복제본
    platform::WaitHandle cleanup_event_ = nullptr;   // 수동 리셋. 주 스레드가 신호한다
    platform::WaitHandle done_event_ = nullptr;      // 수동 리셋. `[telemetry]` 가 끝날 때 신호한다

    SpscRing<QueuedRecord, kQueueCapacity> queue_;
    std::atomic<std::uint64_t> upload_failed_{0};

    std::thread thread_;
};

}  // namespace sangtachi::telemetry
