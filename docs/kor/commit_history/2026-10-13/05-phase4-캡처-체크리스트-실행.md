# Phase 4 캡처 체크리스트 C0~C5 실행

## 왜 했나

4차 K 가 만든 [`tools/phase4-capture`](../../../../tools/phase4-capture/README.md) 의 같은 기기 절(C0~C5)을 처음으로 돌렸다.
저장소 소유자가 Wireshark 와 Npcap 설치를 허락했다. 두 번째 기기가 필요한 D1~D3 와 보류인 C6~C9 는 돌리지 않았다.

## 무엇을 바꿨나

| 무엇 | 자리 |
|------|------|
| C0 표를 이 기기에서 채웠다. "확인하지 않았다" 문단을 그 결과로 바꿨다. 설치에만 관리자 권한이 들고 캡처는 일반 사용자로 된다고 적었다 | `tools/phase4-capture/README.md` |
| C2 에서 같은 기기의 다른 프로그램의 STUN 을 빼는 기준을 더했다. 실행 중 클라이언트 프로세스가 가진 UDP 포트를 적고 그 포트에서 나간 것만 센다 | 같은 파일 |
| 다음 할 일에서 캡처 행을 지우고, 보류 C6~C9 를 "대기 중인 것" 에 옮겼다 | `plan.md` |

## 실행

| 항목 | 값 |
|------|-----|
| 날짜 | 2026-10-13 |
| 커밋 | `82cebaa` |
| 빌드 | 제품, Debug |
| 기기와 망 | 이 기기(Windows 11), 집 Wi-Fi. 같은 기기에 Tailscale 이 돈다 |
| 캡처 도구 | Wireshark 4.6.9 의 `dumpcap`, Npcap 1.89. 루프백 어댑터와 `Wi-Fi` 를 같이, 캡처 필터 `udp`. 분석은 `tshark` 로 줄을 뽑아 스크립트로 셌다 |
| 구성 | 제어 서버 하네스(DynamoDB local), 실행마다 캡처 파일 하나(R1~R4). 캡처 파일은 레포 밖에 두었다 |

## 결과

| 절 | 실행 | 관측값 | 판정 |
|----|------|--------|------|
| C0 | R1 | 하네스 STUN 은 루프백, A ↔ B 의 LAN 주소는 루프백, `192.0.2.1` 은 `Wi-Fi` | 통과(구성 확인) |
| C1 | R1 | 터널도 STUN 도 아닌 송신 0. 버전 모두 1. 여덟 타입이 다 보였다(`DATA` 는 B 의 `send 1`, `ROSTER` 는 호스트 A). `payload_length` 가 모두 UDP 길이 - 28 이고 타입별 길이와 맞았다. 헤더 `peer_id` 를 big-endian 으로 읽은 값이 상대 로그의 값과 같았다. `HELLO` 의 가상 IP 가 `control.peers` 와 같은 순서였다. 쪽마다 epoch 가 하나이고 0 이 아니었다. `sequence` 가 1 부터 늘기만 했다. 첫 4바이트 `53 41 4e 47` | 통과 |
| C2 | R1, R4 | R1: STUN 출발지 포트 집합과 터널 출발지 포트 집합이 둘 다 `{P_A, P_B}`. R4(포트 소유를 적으려고 한 번 더 돌린 것): 실행 중 두 클라이언트 프로세스가 가진 UDP 포트는 각각 자기 `P` 하나였다(`Get-NetUDPEndpoint`). 그 포트들에서 나간 STUN 과 터널의 출발지가 둘 다 `{P_A, P_B}`, 응답의 목적지 포트도 각자의 `P`. 남은 STUN 출발지는 41641 하나였고 그 포트는 `tailscaled` 가 가졌다. `stun.result` 두 줄은 클라이언트마다 같은 `<공인 IP>:P` 였다(공인 IP 는 적지 않는다). 집 NAT 가 포트를 보존했다 | 통과. 같은 공인 IP:Port 관측도 통과 |
| C3 | R2 첫 장면 | 가짜 피어의 후보 하나로 `HELLO` 49개, 간격 190~215ms, 첫 `HELLO`(`t0`)에서 마지막까지 9.918s(`t0 + 10.2s` 앞). `t0 + 10.2s` 부터 `t0 + 20.2s` 까지 `P_A` 에서 나간 `HELLO` 는 어느 목적지로도 0개. A 에 `FAIL HOLE_PUNCH_TIMEOUT Fake1` | 통과 |
| C4-1 | R2 둘째 장면 | 학습 전 `PUNCHING` 에서 `quit`. A 가 보낸 `CLOSE` 0개 | 통과 |
| C4-2 | R1 | A 의 `CLOSE` 1개, `reason` 0x00, 목적지가 `session.endpoint` 의 학습 주소(상대의 LAN 주소). 후보였던 `192.0.2.1` 로는 가지 않았다. B 는 A 의 `CLOSE` 로 이미 `CLOSED` 라 보내지 않았다 | 통과 |
| C4-3 | R3 둘째 장면 | 학습 뒤 `HANDSHAKING` 에서 `quit`. `CLOSE` 1개, `reason` 0x01. 목적지(관측값)는 가짜 피어 주소 | 통과(개수와 `reason`). 목적지 판정은 보류 |
| C4-4 | R3 첫 장면 | `PEER_HANDSHAKE_FAILED` 에서 `CLOSE` 1개, `reason` 0x03, 그 뒤 `HELLO` 0개. 목적지(관측값)는 가짜 피어 주소 | 통과(개수, `reason`, 정지). 목적지 판정은 보류 |
| C4-5 | R2 첫 장면 | 학습 전 `HOLE_PUNCH_TIMEOUT` 에서 `CLOSE` 0개 | 통과 |
| C5 | R1 | 쪽마다 첫 `KEEPALIVE` 가 마지막 `HELLO` 뒤 1ms, 간격 15.001~15.011s, 첫 것부터 60초 창에 4개, 첫 `KEEPALIVE` 뒤 `HELLO` 0개, 모든 `KEEPALIVE` 의 목적지가 학습한 주소 하나 | 통과 |
| C6~C9 | - | - | 보류 |
| D1~D3 | - | - | 두 번째 기기 |

가짜 피어의 거동도 README 의 기대와 같았다. `silent` 는 A 로 아무것도 보내지 않았고, `hello_only` 는 `HELLO` 만 보냈으며 A 는
`HANDSHAKING` 에 들어갔다.

## 검증

| 무엇 | 결과 |
|------|------|
| `docgate.py` | `VERDICT: pass` |

## 크로스 모델 리뷰

Codex 에 기록의 근거와 README 일관성으로 돌렸다.

| 지적 | 처리 |
|------|------|
| warn. C3 의 "멈춘 뒤 나가지 않는다" 창을 마지막 `HELLO` 에서 10초로 적었다. README 는 `t0 + 10.2s` 부터 10초다 | 반영. 캡처에서 그 창을 다시 세어 어느 목적지로도 0개임을 적었다 |
| warn. C2 의 R4 거르기가 로그의 `stun.result server=` 에 해석한 주소가 있다고 가정했다. 그 값은 해석 전의 이름이다 | 반영. 실행 중 클라이언트 프로세스의 UDP 포트 소유를 적어 거르는 것으로 README 를 고쳤고, R4 를 그 방법으로 다시 돌렸다 |
