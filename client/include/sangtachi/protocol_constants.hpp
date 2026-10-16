#pragma once

// 터널 프로토콜 상수 (protocol.md 3장 상수, 11장 타이머).
//
// protocol.md 가 이 값들의 단일 출처다. 여기는 그 블록을 옮겨 적은 것이고, 그 문서가
// 바뀌면 여기를 따라 고친다. 코드에서 새 값을 만들지 않는다.

#include <cstddef>
#include <cstdint>

namespace sangtachi::protocol {

// --- 3장 상수 블록. 줄 순서와 이름이 그 블록과 같다 ---

inline constexpr std::uint32_t kTunnelMagic = 0x53414E47;  // "SANG"
inline constexpr std::uint8_t kTunnelVersion = 0x01;
inline constexpr std::size_t kHeaderSize = 20;
inline constexpr std::size_t kMaxInner = 1452;
inline constexpr std::size_t kMaxDatagram = kHeaderSize + kMaxInner;  // 1472
inline constexpr std::uint32_t kAdapterMtu = 1400;  // 가상 어댑터 MTU 기본값 (12장). 실측으로 확정한다
inline constexpr std::uint32_t kStunCookie = 0x2112A442;  // RFC 5389
inline constexpr std::size_t kMaxCandidates = 8;
inline constexpr std::size_t kMaxPeers = 5;  // 한 방의 인원. 5.7 ROSTER 의 상한이기도 하다
inline constexpr std::size_t kMaxNameLen = 16;  // 표시 이름 바이트 상한. 형식은 control_plane.md 2.7
inline constexpr std::size_t kReplayWindow = 64;
inline constexpr std::size_t kMaxPendingPings = 16;
inline constexpr std::size_t kMaxProbePaths = 4;
inline constexpr std::size_t kMaxRetiredEpochs = 16;
inline constexpr std::size_t kMaxRenegotiations = 8;
inline constexpr std::uint32_t kMinRenegIntervalMs = 1000;
inline constexpr std::size_t kUnverifiedTxBucket = 10;
inline constexpr std::size_t kUnverifiedTxRefill = 5;

static_assert(kMaxDatagram == 1472, "protocol.md 3장 MAX_DATAGRAM");

// --- 11장 타이머 ---
//
// 11장은 표로 적고 이름을 붙이지 않았다. 이름은 이 코드가 붙였고 값과 출처 행을 줄마다
// 적는다. 단위는 밀리초이고 형은 timer.hpp 의 Millis 와 같은 64비트다.
//
// 이 블록에 없는 11장 행은 이미 다른 자리가 갖는다. 같은 값을 두 곳에 두지 않는다.
//   STUN 재시도·마감        network/stun_client.hpp 의 kStunRetryIntervalsMs, kStunDeadlineMs
//   get_peers 폴링·마감     control/constants.hpp 의 kGetPeersPollMs, kGetPeersDeadlineMs
//   host_report 주기        control/constants.hpp 의 kHostReportOpenS, kHostReportFullS
//   펀치 지연               10.2 랑데부의 계산값이라 상수가 아니다
//   재협상 최소 간격        위 3장 블록의 kMinRenegIntervalMs

inline constexpr std::uint64_t kHelloRetransmitMs = 200;     // `HELLO` 재전송 간격
inline constexpr std::uint64_t kPunchDeadlineMs = 10000;     // 펀치 마감
inline constexpr std::uint64_t kKeepaliveIntervalMs = 15000; // `KEEPALIVE` 간격
inline constexpr std::uint64_t kRosterIntervalMs = 15000;    // `ROSTER` 주기. 호스트만
inline constexpr std::uint64_t kRosterResendIntervalMs = 5000;  // `ROSTER` 변경 재송 간격
inline constexpr std::size_t kRosterResendCount = 2;            // 그 재송 횟수
inline constexpr std::uint64_t kIdleTimeoutMs = 50000;       // 유휴 타임아웃
inline constexpr std::uint64_t kPingIntervalMs = 5000;       // `PING` 간격. 첫 송신도 진입 5s 뒤다

// 11장 "아래 만료 값" 표. 그 표는 색인이고 값의 출처는 오른쪽 주석의 절이다.
inline constexpr std::uint64_t kRetiredEpochTtlMs = 120000;    // 폐기 목록 항목 (4.3). best-effort
inline constexpr std::uint64_t kRetiredNonceTtlMs = 120000;    // 시도 nonce 폐기 목록 항목 (5.1)
inline constexpr std::uint64_t kProbeTtlMs = 5000;             // probe nonce 와 잠정 경로 자리 (5.1, 10.4 (c))
inline constexpr std::uint64_t kPendingPingTtlMs = 5000;       // `pending_pings` 항목 (5.5)
inline constexpr std::uint64_t kTerminalSessionTtlMs = 120000; // 종료 상태 세션 (5.6)

}  // namespace sangtachi::protocol
