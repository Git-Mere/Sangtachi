# Architecture

**프로젝트:** Direct-First P2P Virtual Network for Multiplayer Games
**참고 문서:** [`first_design.md`](first_design.md)
**대상 플랫폼:** Windows 10 / 11 x64 (클라이언트), Linux/AWS EC2 (제어 서버)

> English version: [`../eng/architecture.md`](../eng/architecture.md)

---

## 1. 설계 원칙

| # | 원칙 | 의미 |
|---|------|------|
| 1 | Direct-First | 게임 트래픽은 가능한 한 피어 간 직접 경로로 흐른다. AWS는 조율 전용이며 데이터 경로가 아니다. |
| 2 | 평면 분리 | 제어 평면(Python/AWS)과 데이터 평면(C++/클라이언트)은 서로 독립적으로 동작하고 테스트된다. |
| 3 | 게임 비종속 | 터널 계층에 Minecraft 전용 로직을 넣지 않는다. Minecraft는 검증 대상일 뿐이다. |
| 4 | 관측 가능 | 모든 연결 시도는 성공/실패와 **실패 단계**를 기록한다. |
| 5 | 의존성 최소화 | Wintun은 가상 인터페이스 접근만 담당한다. STUN, NAT 통과, 터널, 라우팅은 직접 구현한다. |
| 6 | 위험 우선 | 가장 불확실한 것(NAT 통과)을 가장 먼저 검증한다. 가상 어댑터는 그 뒤다. |

---

## 2. 시스템 구성

```text
                        AWS Control Plane (EC2)
                    +---------------------------+
                    | Python Coordination Server|
                    |  rooms / peers            |
                    |  candidate exchange       |
                    |  telemetry ingest         |
                    |  SQLite                   |
                    +-------------+-------------+
                                  |
                        조율 트래픽만 (TCP/JSON)
                                  |
              +-------------------+-------------------+
              |                                       |
      +-------v---------+                    +--------v--------+
      | Windows Client A|                    | Windows Client B|
      |  Wintun Adapter |                    |  Wintun Adapter |
      |  Router         |                    |  Router         |
      |  Tunnel         |<==================>|  Tunnel         |
      |  Hole Punch     |   Direct P2P UDP   |  Hole Punch     |
      |  STUN Client    |   (게임 트래픽)      |  STUN Client    |
      |  Telemetry      |                    |  Telemetry      |
      +-------+---------+                    +--------+--------+
              |                                       |
      Minecraft Server                         Minecraft Client
        10.100.0.1:25565                          10.100.0.2
```

제어 평면이 죽어도 **이미 수립된 P2P 터널은 계속 동작**해야 한다. 제어 평면은 세션 수립과 텔레메트리 수집에만 필요하다.

---

## 3. 컴포넌트

### 3.1 클라이언트 (C++20)

| 모듈 | 책임 | 외부 의존 |
|------|------|-----------|
| `network/udp_socket` | Winsock2 초기화/해제, UDP 소켓 생성, 논블로킹 송수신, `WSAPoll` 기반 대기 | Winsock2 |
| `network/endpoint` | `IP:Port` 값 타입, 파싱, 비교, 직렬화 | 없음 |
| `network/stun_client` | STUN Binding Request 생성, 트랜잭션 ID 관리, 응답 파싱, `XOR-MAPPED-ADDRESS` 디코드, 타임아웃/재시도 | 없음 (RFC 5389 직접 구현) |
| `peer/peer` | 피어 식별자, 가상 IP, 후보 엔드포인트 목록 | 없음 |
| `peer/hole_punch` | 양방향 동시 송신, 재시도, 성공 판정, 실패 사유 분류 | 없음 |
| `peer/session` | 핸드셰이크, keepalive, 연결 상태 머신, RTT 측정 | 없음 |
| `tunnel/packet` | 터널 헤더 직렬화/역직렬화, 손상 패킷 검증 | 없음 |
| `tunnel/tunnel` | 패킷 타입별 디스패치, 캡슐화/역캡슐화, 시퀀스 관리 | 없음 |
| `tunnel/router` | 가상 IP -> 피어 세션 매핑, 목적지 결정 | 없음 |
| `adapter/wintun_adapter` | 가상 어댑터 생성/개방과 패킷 read/inject (Wintun), 가상 IP 주소 및 라우트 설정 (IP Helper) | **Wintun (승인 완료)**, IP Helper |
| `telemetry/telemetry` | 지표 수집, 로컬 버퍼링, 제어 평면 전송 | 없음 |
| `control/control_client` | 제어 평면 REST/JSON 호출 | Winsock2 |

클라이언트는 단일 프로세스다. 초기 버전은 다음 스레드 구성을 가정한다.

```text
[main]        초기화, 상태 머신, 종료 처리
[net_rx]      UDP 소켓 수신 루프 -> 터널 디스패치
[tun_rx]      Wintun 읽기 루프 -> 라우팅 -> UDP 송신
[timer]       keepalive, PING/PONG, 재시도, 텔레메트리 플러시
```

Phase 1~5에서는 `[tun_rx]`가 없고 `[main]`이 콘솔 입력을 대신 넣는다.

**모든 UDP 송수신은 하나의 로컬 소켓을 공유한다.** STUN Binding Request, `HELLO`, `HELLO_ACK`, `KEEPALIVE`, `PING`/`PONG`, `DATA`가 서로 다른 소켓을 쓰면 NAT가 각각 다른 공인 포트를 매핑하므로, STUN으로 발견한 엔드포인트가 터널에 적용되지 않는다. 이 제약은 Phase 2 시점부터 지켜야 한다.

### 3.2 제어 평면 (Python)

| 모듈 | 책임 |
|------|------|
| `server.py` | HTTP/JSON 엔드포인트, 요청 디스패치 |
| `room.py` | 방 생성/참가, 가상 IP 풀 관리, 방 상태 |
| `peer.py` | 피어 등록, 후보 엔드포인트 저장, 피어 목록 조회 |
| `telemetry.py` | 연결 결과와 성능 지표 수신, SQLite 저장 |

표준 라이브러리(`asyncio`, `json`, `sqlite3`)만 사용한다. 웹 프레임워크는 필요성이 입증되고 승인된 뒤에 도입한다.

---

## 4. 데이터 평면 경로

게임 패킷이 상대 머신에 도달하기까지의 전체 경로다.

```text
Minecraft (TCP, 10.100.0.2 -> 10.100.0.1:25565)
   |
   v  Windows 라우팅 테이블이 10.100.0.0/24 를 가상 어댑터로 보냄
Wintun Adapter (송신측)
   |
   v  wintun_adapter 가 원본 IP 패킷을 읽음
Router: 목적지 가상 IP -> 피어 세션 결정
   |
   v  tunnel 이 DATA 헤더를 붙임
[TunnelHeader][원본 IP 패킷]
   |
   v  udp_socket 이 직접 P2P 경로로 송신
Internet (UDP, 공인 엔드포인트 -> 공인 엔드포인트)
   |
   v  상대 udp_socket 수신
tunnel: 헤더 검증 및 제거
   |
   v  wintun_adapter 가 원본 IP 패킷을 주입
Wintun Adapter (수신측)
   |
   v
Minecraft Server
```

Minecraft Java Edition은 게임플레이에 TCP를 쓰지만, 터널 자체는 UDP를 쓴다. 원본 TCP 패킷이 페이로드로 실려 가기 때문에 문제되지 않는다. TCP 재전송과 혼잡 제어는 게임의 TCP 스택이 그대로 담당한다.

### MTU 처리

터널 헤더가 붙으므로 페이로드 가용 크기가 줄어든다.

```text
1500 (경로 MTU 가정)
 - 20 (외부 IPv4 헤더)
 -  8 (외부 UDP 헤더)
 - 16 (TunnelHeader)
= 1456 바이트 (내부 IP 패킷 최대치)
```

가상 어댑터의 MTU를 1400 정도로 낮춰 잡아 조각화를 피한다.

다만 위 계산은 **외부 경로 MTU가 1500이라는 가정**에 의존한다. PPPoE 회선이나 다른 터널을 경유하는 경로는 이보다 작고, 그 경우 외부 UDP 패킷이 조각화된다. 정확한 값은 Phase 7에서 실측으로 확정하며, 보수적 고정값을 쓴다면 그 한계를 [`experiments.md`](experiments.md)에 명시한다.

---

## 5. 터널 프로토콜

### 5.1 헤더

```cpp
struct TunnelHeader
{
    uint32_t magic;           // 프로토콜 식별자, 오배달 패킷 차단
    uint8_t  version;         // 프로토콜 버전
    uint8_t  type;            // PacketType
    uint32_t peer_id;         // 송신 피어 식별자
    uint32_t sequence;        // 손실/재정렬 측정용
    uint16_t payload_length;  // 뒤따르는 페이로드 바이트 수
};                            // 와이어 포맷 16 바이트, 네트워크 바이트 오더
```

와이어 포맷은 16바이트지만 `sizeof(TunnelHeader)`는 16이 아니다. MSVC 기본 정렬에서 `type` 뒤에 2바이트 패딩이 들어가 20바이트가 된다. 구조체를 그대로 `memcpy` 하거나 `sizeof`를 헤더 길이로 쓰면 안 된다. **필드 단위로 직접 직렬화**하고 헤더 길이는 상수로 고정한다.

`magic`과 `version`의 실제 값, `HELLO` / `HELLO_ACK` 페이로드의 필드 폭과 배치는 Phase 4 착수 시점에 확정하고 [`protocol.md`](protocol.md)에 기록한다. 따라서 `protocol.md`는 Phase 4에서 만들고 Phase 5에서 확장한다. 이 값들이 없으면 Phase 4의 패킷 캡처 검증 기준이 성립하지 않는다.

`payload_length`는 수신 측 검증에 쓴다. 실제 수신 바이트 수와 다르면 패킷을 버리고 카운터를 올린다.

### 5.2 패킷 타입

| 타입 | 방향 | 페이로드 | 용도 |
|------|------|----------|------|
| `HELLO` | 양방향 | 피어 ID, 가상 IP, 세션 nonce | 홀펀칭 시도 및 핸드셰이크 개시 |
| `HELLO_ACK` | 양방향 | 상대 `HELLO`의 nonce 에코 | 상대가 내 패킷을 실제로 받았음을 증명 |
| `KEEPALIVE` | 양방향 | 없음 | NAT 매핑 유지 (주기: 15~25초) |
| `DATA` | 양방향 | 원본 IP 패킷 | 게임 트래픽 전달 |
| `PING` | 양방향 | 송신 타임스탬프 | RTT 측정 |
| `PONG` | 양방향 | 원본 타임스탬프 에코 | RTT 측정 응답 |

### 5.3 세션 상태 머신

```text
IDLE
  | 제어 평면에서 피어 후보 수신
  v
PUNCHING  --- 타임아웃 --->  FAILED(HOLE_PUNCH_TIMEOUT)
  | 상대 HELLO 수신 (자신의 HELLO 는 계속 재송신 중)
  v
HANDSHAKING --- 타임아웃 --->  FAILED(PEER_HANDSHAKE_FAILED)
  | 자신이 보낸 nonce 가 담긴 HELLO_ACK 수신
  v
CONNECTED
  | 유휴 타임아웃 (상대 패킷 무수신)
  v
FAILED(TUNNEL_DROPPED)
```

UDP `sendto`의 성공은 전달을 보장하지 않는다. 따라서 두 전이 모두 **수신한 패킷**으로만 판정한다.

- `HANDSHAKING -> CONNECTED`: 자신이 보낸 `HELLO`의 nonce가 담긴 `HELLO_ACK`을 받아야 한다. 내가 `HELLO`를 보냈고 상대 `HELLO`를 받았다는 사실만으로는 상대가 내 패킷을 받았는지 알 수 없다. 홀펀칭은 한쪽 방향만 뚫릴 수 있다.
- `CONNECTED -> FAILED`: keepalive 송신 실패가 아니라, 일정 시간(keepalive 주기의 3배 정도) 동안 상대로부터 유효 패킷을 하나도 받지 못한 것으로 판정한다.

---

## 6. 제어 평면 인터페이스

### 6.1 연산

| 연산 | 입력 | 출력 |
|------|------|------|
| `create_room` | 호스트 식별자 | room_id, 할당된 가상 IP |
| `join_room` | room_id, 피어 식별자 | 할당된 가상 IP, 방 정보 |
| `register_peer` | room_id, peer_id | 등록 확인. `create_room` / `join_room` 이 내부적으로 수행하므로 클라이언트가 따로 호출하지 않는다 (재연결 시에만 사용) |
| `register_candidate` | room_id, peer_id, 로컬/공인 엔드포인트 | 등록 확인 |
| `get_peers` | room_id, peer_id | 다른 피어들의 가상 IP와 후보 엔드포인트 |
| `report_connection` | room_id, peer_id, 결과, 실패 단계, 수립 시간 | 확인 |
| `report_telemetry` | room_id, peer_id, RTT/손실/지터/전송량 | 확인 |

### 6.2 연결 수립 시퀀스

```text
Host                      AWS                       Player
 |                         |                          |
 |--- create_room -------->|                          |
 |<-- room_id, 10.100.0.1--|                          |
 |                         |<------- join_room -------|
 |                         |-- room_id, 10.100.0.2 -->|
 |                         |                          |
 |-- STUN Binding Req ->(공개 STUN 서버)<- STUN Binding Req --|
 |<- XOR-MAPPED-ADDRESS -                - XOR-MAPPED-ADDRESS ->|
 |                         |                          |
 |-- register_candidate -->|<-- register_candidate ---|
 |                         |                          |
 |--- get_peers ---------->|<-------- get_peers ------|
 |<-- 상대 엔드포인트 -------|--- 상대 엔드포인트 ------->|
 |                         |                          |
 |=========== UDP HELLO 동시 송신 (홀펀칭) =============|
 |=========== Direct P2P Tunnel 수립 =================|
 |                         |                          |
 |--- report_connection -->|<--- report_connection ---|
```

---

## 7. 가상 네트워크 설계

- 대역: `10.100.0.0/24`
- `10.100.0.1`: 방 생성자(호스트). 게임 서버가 여기서 돈다.
- `10.100.0.2` 이상: 참가자. 제어 평면이 순차 할당한다.
- 가상 IP는 방 단위로 유효하며, 방이 사라지면 회수한다.

플레이어는 Minecraft 서버 주소란에 `10.100.0.1:25565`를 입력한다. 공인 IP나 포트를 알 필요가 없다.

Windows 라우팅은 가상 어댑터에 `10.100.0.0/24` 경로를 붙여 처리한다. 클라이언트가 어댑터 생성 시 Windows IP Helper API로 이 경로를 직접 설정하고, 종료 시 제거한다. Wintun은 주소나 라우트를 설정해 주지 않는다.

---

## 8. 실패 진단

연결 실패는 반드시 단계로 분류해 기록한다. "연결 안 됨"은 기록으로 받아들이지 않는다.

| 코드 | 단계 | 판정 근거 |
|------|------|-----------|
| `STUN_DISCOVERY_FAILED` | 공인 엔드포인트 발견 | STUN 응답 없음 또는 파싱 실패 |
| `CONTROL_PLANE_EXCHANGE_FAILED` | 후보 교환 | 제어 평면 요청 실패 또는 상대 후보 없음 |
| `HOLE_PUNCH_TIMEOUT` | 홀펀칭 | 제한 시간 내 상대 `HELLO` 미수신 |
| `PEER_HANDSHAKE_FAILED` | 핸드셰이크 | 상대 `HELLO`는 왔으나 제한 시간 내 `HELLO_ACK` 미수신 |
| `TUNNEL_DROPPED` | 유지 | 수립 후 유휴 타임아웃 동안 상대 패킷 무수신 |

각 실패에는 확보 가능한 NAT 환경 정보(로컬 대역, 공인 엔드포인트, STUN 서버별 매핑 차이)를 함께 남긴다. `STUN_DISCOVERY_FAILED`는 발견 자체가 실패한 경우라 공인 엔드포인트가 없다. 모든 필드를 요구하지 말고 존재하는 것만 기록한다.

**NAT 유형을 단정하지 않는다.** RFC 5389 Binding 결과와 포트 변화만으로는 NAT 유형이나 필터링 거동을 판별할 수 없다. Phase 9의 분석은 "NAT 유형"이 아니라 **관측된 매핑 거동**(목적지에 따라 공인 포트가 바뀌는가)과 **연결 성공 여부**로 분류한다. 유형 판별이 필요해지면 RFC 5780 지원 서버나 통제된 다중 목적지 시험이 추가로 필요하며, 이는 초기 범위 밖이다.

---

## 9. 텔레메트리

| 지표 | 수집 지점 | 용도 |
|------|-----------|------|
| 연결 성공/실패 + 실패 단계 | 세션 수립 종료 시 | 신뢰성 분석 |
| 연결 수립 시간 | STUN 시작 ~ `CONNECTED` | 사용자 체감 분석 |
| RTT | `PING`/`PONG` 주기 측정 | 지연 분석 |
| 패킷 손실률 | `sequence` 간격 누락 집계 | 품질 분석 |
| 지터 | 연속 RTT 편차 | 품질 분석 |
| keepalive 로컬 송신 오류 | 타이머 스레드 | 안정성 분석. 로컬 `sendto` 오류만 세며 전달 실패의 근거가 아니다. 그래서 단절 판정은 유휴 타임아웃으로 한다 |
| 유휴 타임아웃 발생 횟수 | 수신 경로 | 안정성 분석 |
| 직접 연결 유지 시간 | `CONNECTED` 지속 구간 | 안정성 분석 |
| 터널 처리량 | 송수신 바이트 카운터 | 성능 분석 |
| 세션 지속 시간 | 게임 세션 전체 | Minecraft 안정성 |

클라이언트는 로컬에 먼저 쌓고 주기적으로 제어 평면에 보낸다. 제어 평면 장애가 데이터 평면에 영향을 주지 않도록 전송 실패는 무시하고 다음 주기에 재시도한다.

---

## 10. 레포 구조

### 목표 구조

```text
Sangtachi/
+-- client/
|   +-- include/
|   +-- src/
|   |   +-- main.cpp
|   |   +-- network/     udp_socket, endpoint, stun_client
|   |   +-- peer/        peer, hole_punch, session
|   |   +-- tunnel/      packet, tunnel, router
|   |   +-- adapter/     wintun_adapter
|   |   +-- telemetry/   telemetry
|   |   +-- control/     control_client
|   +-- CMakeLists.txt
+-- control-server/
|   +-- server.py
|   +-- room.py
|   +-- peer.py
|   +-- telemetry.py
+-- tests/
+-- scripts/
+-- docs/
|   +-- kor/                 한국어 문서 (이 트리)
|   |   +-- architecture.md      이 문서
|   |   +-- spec.md              요구사항과 성공 기준
|   |   +-- roadmap.md           단계별 개발 계획
|   |   +-- plan.md              현재 작업 단위 체크리스트
|   |   +-- first_design.md      초기 기획서 (참고용)
|   |   +-- protocol.md          터널 프로토콜 상세 (Phase 4에서 착수, Phase 5에서 완성)
|   |   +-- experiments.md       실험 설계와 측정 결과 (Phase 9에서 작성)
|   |   +-- decisions/           설계 결정 기록
|   |   +-- commit_history/      작업 단위 변경 기록
|   +-- eng/                 영어 문서, 동일 구조
+-- README.md
```

두 언어 트리는 같은 파일을 갖는다. 문서를 고칠 때는 같은 커밋에서 양쪽을 함께 수정한다.

### 현재 상태와의 차이

현재 소스 트리는 루트의 `src/main.cpp`와 `CMakeLists.txt`뿐이다 (`docs/`는 이미 채워져 있다). Phase 1 시작 시 `client/` 하위로 옮기고 `control-server/`를 만든다. 이 재배치는 Phase 1의 첫 작업으로 처리한다.

---

## 11. 외부 의존성

| 의존성 | 제공 범위 | 승인 상태 |
|--------|-----------|-----------|
| Wintun | Windows 가상 네트워크 인터페이스 접근만. 어댑터 생성, 패킷 read/inject | **승인 완료 (2026-09-14)** |
| Windows IP Helper / NetIO API | 가상 어댑터의 IP 주소와 라우트 설정. **Wintun은 이 기능을 제공하지 않는다** | OS 기본 제공 |
| Winsock2 | Windows 기본 소켓 API | OS 기본 제공 |
| 공개 STUN 서버 | Binding Response 응답. 서버는 구현하지 않고 이용만 한다 | 외부 공개 서비스 |
| Python 표준 라이브러리 | `asyncio`, `json`, `sqlite3` | 표준 라이브러리 |

Wintun이 제공하지 **않는** 것을 명확히 한다. 피어 발견, STUN, NAT 통과, 홀펀칭, 터널 프로토콜, 라우팅 결정, 세션 관리, 모니터링, 진단은 모두 이 프로젝트가 직접 구현한다. Wintun은 커널 모드 가상 NIC 드라이버에 대한 접근 수단일 뿐이다.

---

## 12. 초기 범위 밖

다음은 의도적으로 초기 아키텍처에 넣지 않는다. 필요해지면 [`decisions/`](decisions/)에 결정 기록을 남기고 도입한다.

- **암호화 및 피어 인증**: 터널 페이로드는 평문이다. 스트레치 목표.
- **릴레이 폴백 (TURN 유사)**: 직접 연결 실패 시 대안 경로 없음. 스트레치 목표.
- **완전한 ICE**: 초기에는 최소 연결 수립 절차만. Phase 9에서 ICE 개념과 비교 분석만 수행.
- **3자 이상 메시**: 초기는 2 피어. 라우팅 테이블 구조는 확장 가능하게 두되 구현하지 않는다.
- **GUI**: 콘솔 전용.
- **macOS / 모바일**: 지원하지 않는다. Linux는 시간이 남으면 상호 운용성만 검토.
