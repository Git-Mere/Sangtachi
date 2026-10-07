#pragma once

// 제어 평면 상수 가운데 클라이언트가 쓰는 것 (control_plane.md 2.6 상수).
//
// control_plane.md 가 이 값들의 단일 출처다. 여기는 그 블록을 옮겨 적은 것이고, 그 문서가
// 바뀌면 여기를 따라 고친다. 코드에서 새 값을 만들지 않는다. CONTROL_PORT 는 args.hpp 의
// kControlPort 가 갖는다.

#include <cstddef>
#include <cstdint>

namespace sangtachi::control {

inline constexpr std::size_t kMaxBodyBytes = 4096;          // MAX_BODY_BYTES (3.3)
inline constexpr std::size_t kMaxHeaderBytes = 2048;        // MAX_HEADER_BYTES (3.3). 응답 머리에도 쓴다 (3.5)
inline constexpr std::size_t kClientJsonMaxDepth = 32;      // CLIENT_JSON_MAX_DEPTH (3.5)
inline constexpr std::uint32_t kClientConnectTimeoutS = 3;  // CLIENT_CONNECT_TIMEOUT_S (8.2)
inline constexpr std::uint32_t kClientIoTimeoutS = 3;       // CLIENT_IO_TIMEOUT_S (8.2)
inline constexpr std::uint32_t kHostReportOpenS = 5;        // HOST_REPORT_OPEN_S (4.6). 빈 자리가 있을 때
inline constexpr std::uint32_t kHostReportFullS = 30;       // HOST_REPORT_FULL_S (4.6). 정원이 찼을 때

// 아래 넷은 2.6 상수 블록에 이름이 없다. 값의 출처를 각 줄에 적는다. 이름은 이 코드가 붙였다.
inline constexpr std::uint64_t kRetryIntervalMs = 1000;     // 8.3. 일시 오류 재시도 간격 1초
inline constexpr std::uint32_t kMaxTries = 3;               // 8.3. 첫 시도를 포함해 총 3회
inline constexpr std::uint64_t kGetPeersPollMs = 500;       // protocol.md 11장. 응답 수신 후
inline constexpr std::uint64_t kGetPeersDeadlineMs = 60000; // protocol.md 11장. register_candidate 성공 응답부터

}  // namespace sangtachi::control
