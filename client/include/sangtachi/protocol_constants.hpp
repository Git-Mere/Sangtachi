#pragma once

// 터널 프로토콜 상수 (protocol.md 3장 상수).
//
// protocol.md 가 이 값들의 단일 출처다. 여기는 그 블록을 옮겨 적은 것이고, 그 문서가
// 바뀌면 여기를 따라 고친다. 코드에서 새 값을 만들지 않는다.

#include <cstddef>
#include <cstdint>

namespace sangtachi::protocol {

inline constexpr std::uint32_t kTunnelMagic = 0x53414E47;  // "SANG"
inline constexpr std::uint8_t kTunnelVersion = 0x01;
inline constexpr std::size_t kHeaderSize = 20;
inline constexpr std::size_t kMaxInner = 1452;
inline constexpr std::size_t kMaxDatagram = kHeaderSize + kMaxInner;  // 1472
inline constexpr std::uint32_t kStunCookie = 0x2112A442;  // RFC 5389
inline constexpr std::size_t kMaxCandidates = 8;
inline constexpr std::size_t kReplayWindow = 64;
inline constexpr std::size_t kMaxPendingPings = 16;
inline constexpr std::size_t kMaxProbePaths = 4;
inline constexpr std::size_t kMaxRetiredEpochs = 16;
inline constexpr std::size_t kMaxRenegotiations = 8;
inline constexpr std::uint32_t kMinRenegIntervalMs = 1000;
inline constexpr std::size_t kUnverifiedTxBucket = 10;
inline constexpr std::size_t kUnverifiedTxRefill = 5;

}  // namespace sangtachi::protocol
