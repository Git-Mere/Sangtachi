#pragma once

// 시험 빌드 장치 (roadmap.md Phase 4 의 M-5 비교와 RTT 기준선 에코, ADR 0023).
//
// **이 원본은 제품 바이너리에 들어가지 않는다.** sangtachi_client 는 SANGTACHI_TEST_BUILD 일 때만
// testbuild/instrument.cpp 를 컴파일하고, sangtachi_core 에는 없다. 시험 실행 파일은 단위 시험을
// 위해 늘 컴파일한다. 붙는 자리는 터널의 페이로드 훅 칸과 터널의 송신 관찰 칸이다
// (concurrency.md 3장).

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace sangtachi::testbuild {

// architecture.md 9장의 `data.tx`. 8.5 를 통과해 캡슐화되는 내부 패킷. `to` 는 오프셋 16 의 목적지다.
void emit_data_tx(std::span<const std::byte> inner);

// architecture.md 9장의 `data.rx`. 8.4 를 통과해 훅이 받은 내부 패킷. `from` 은 오프셋 12 의 출발지다.
void emit_data_rx(std::span<const std::byte> inner);

// architecture.md 9장의 `data.echo`. 방향 표시가 에코(`0x5A`)인 패킷, 즉 이쪽이 보낸 요청이 되돌아온 것이면
// `rtt_ms` = now_ms - (오프셋 32..40 의 송신 시각) 한 줄을 낸다. 송신 시각은 이쪽 `send` 가 같은 단조 시계로
// 채운 값이다 (roadmap.md Phase 4 시험 장치의 레이아웃). 에코가 아니거나 IHL 이 5 가 아니면 내지 않는다.
// 시각이 now_ms 보다 뒤면(다른 프로세스의 시계) 내지 않는다.
void emit_echo_rtt(std::span<const std::byte> inner, std::uint64_t now_ms);

// 에코 응답기. 방향 표시가 요청(`0xA5`)인 패킷이면 출발지와 목적지를 뒤집고 표시를 `0x5A` 로 바꾸고
// IPv4 체크섬을 다시 채운 사본이다. 41바이트 미만(표시가 없다)이거나, IHL 이 5 가 아니거나(합성 레이아웃이
// 아니다), 표시가 요청이 아니면 비어 있다. `0x5A` 에 응답하지 않으므로 양쪽이 켜져 있어도 끝없이 오가지 않는다.
[[nodiscard]] std::optional<std::vector<std::byte>> make_echo_reply(std::span<const std::byte> inner);

// roadmap.md Phase 6 검증의 ICMP 응답기. 어댑터에서 읽은 패킷이 라우팅 범위 밖으로 가는 ICMP 에코 요청이면
// 그 응답을 만든다. 응답이 없는 패킷은 부르는 쪽이 라우팅으로 보낸다 (concurrency.md 3장).
// 출발지와 목적지를 뒤집고, TTL 을 64 로, ICMP 형식을 0(에코 응답)으로 바꾸고, ICMP 체크섬과 IPv4 헤더
// 체크섬을 다시 채운다. 식별자, 순번, 데이터는 그대로다. 받은 것을 되돌리는 장치라 위 에코 응답기와 같은
// 시험 빌드에만 있다.
//
// | 받은 패킷 | 결과 |
// |-----------|------|
// | IPv4, IHL 5, 전체 길이 = 받은 길이, 조각 아님, 프로토콜 1, ICMP 형식 8 코드 0, ICMP 체크섬 맞음 | 응답 |
// | 버전이 4 가 아님, IHL 이 5 가 아님(옵션이 있음) | 없음 |
// | 전체 길이가 받은 길이와 다름, ICMP 머리 8바이트가 없음 | 없음 |
// | 조각(MF 가 섰거나 조각 오프셋이 0 이 아님) | 없음. 조각 하나로는 ICMP 체크섬을 볼 수 없다 |
// | 프로토콜이 1 이 아님, 형식이 8 이 아니거나 코드가 0 이 아님 | 없음. 응답(형식 0)에 다시 응답하지 않는다 |
// | ICMP 체크섬이 틀림 | 없음 |
// | 목적지가 `10.100.0.0/24` 밖이거나, 마지막 옥텟이 `MAX_PEERS` 이하이거나 255 | 없음. 상대의 가상 IP 와 브로드캐스트에 답하지 않는다 |
[[nodiscard]] std::optional<std::vector<std::byte>> make_icmp_echo_reply(std::span<const std::byte> packet);

}  // namespace sangtachi::testbuild
