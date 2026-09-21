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
| `network/udp_socket` | Winsock2 초기화/해제, UDP 소켓 생성, 논블로킹 송수신, 이벤트 핸들 노출 (3.2) | Winsock2 |
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

클라이언트는 단일 프로세스다. 스레드 구성과 상태 소유는 3.2에 있다.

**모든 UDP 송수신은 하나의 로컬 소켓을 공유하고, 그 소켓은 하나의 수신 루프가 배타적으로 소유한다.** 서로 다른 소켓을 쓰면 NAT가 각각 다른 공인 포트를 매핑하므로 STUN으로 발견한 엔드포인트가 터널에 적용되지 않는다. 이 제약은 Phase 2 시점부터 지켜야 한다.

소켓을 공유한다는 것은 STUN 응답과 터널 패킷이 같은 큐로 들어온다는 뜻이다. STUN 클라이언트가 직접 `recvfrom`을 부르면 두 읽기가 서로의 패킷을 훔친다. STUN 클라이언트는 요청만 등록하고 응답은 수신 루프에서 전달받는다. 분류 규칙과 필수 소켓 옵션(`SO_EXCLUSIVEADDRUSE`, `SIO_UDP_CONNRESET` off, `connect()` 미호출)은 [`protocol.md`](protocol.md) 6~7장에 있다.

### 3.2 동시성 모델

**터널 상태를 건드리는 모든 코드는 한 스레드에서만 돈다.** 잠금을 걸어 공유하는 대신 공유하지 않는다.

초기 안은 `[main]` / `[net_rx]` / `[tun_rx]` / `[timer]` 4스레드였다. 두 가지 이유로 폐기했다. 첫째, 네 스레드가 세션 상태와 송신 `sequence`를 동기화 없이 변경한다. 둘째, 텔레메트리 업로드가 타이머 스레드를 공유해 제어 평면 장애가 keepalive를 멈춘다. 후자는 NFR-3을 설계 단계에서 위반한다.

#### 3.2.1 스레드

| 스레드 | 책임 | 공유 상태 |
|--------|------|-----------|
| `[loop]` | UDP 수신, Wintun 수신, 콘솔 명령, 타이머, 세션 상태, 라우팅, 모든 송신 | **터널 상태 없음.** 텔레메트리 큐와 종료/정리 이벤트만 공유한다 |
| `[telemetry]` | 지표 큐 소비, 제어 평면 업로드 | 큐 하나 |
| `[console]` | 표준 입력 읽기. Phase 1~5 스캐폴딩 | 명령 큐 하나 |

Phase 6 이후는 두 스레드, Phase 1~5는 세 스레드다. `[loop]`가 프로세스 주 스레드다. `[console]`은 Phase 1~5에만 있다. `[loop]`가 표준 입력에서 블록할 수 없으므로 별도 스레드가 읽어 큐에 넣는다. Phase 6에서 어댑터가 들어올 때는 Wintun 읽기 이벤트가 `[loop]`의 대기 집합에 직접 합류하므로 스레드가 늘지 않는다.

#### 3.2.2 대기

```text
WaitForMultipleObjects(n, handles, FALSE, timeout_ms)
```

| 순위 | 핸들 | 생성 | 유효 구간 |
|------|------|------|-----------|
| 0 | 종료 이벤트 | `CreateEvent` (수동 리셋) | 항상 |
| 1 | UDP 소켓 이벤트 | `WSACreateEvent` + `WSAEventSelect(sock, ev, FD_READ)` | 항상 |
| 2 | Wintun 읽기 이벤트 | `WintunGetReadWaitEvent(session)` | Phase 6 이후 |
| 3 | 콘솔 명령 이벤트 | `CreateEvent` (자동 리셋). `[console]`이 신호 | Phase 1~5 |

**위 번호는 논리적 순위이지 배열 인덱스가 아니다.** `WaitForMultipleObjects`는 유효한 핸들이 빈틈없이 채워진 배열을 요구한다. 빈자리에 `NULL`을 넣으면 `WAIT_FAILED`가 난다. 구성이 바뀌는 시점마다 살아 있는 핸들만 모아 조밀한 배열을 만들고, 논리적 출처에서 실제 인덱스로 가는 대응표를 따로 둔다. Phase 1~5에는 Wintun 이벤트가 없어 배열 길이가 3이다.

`timeout_ms`는 아래처럼 계산한다. **뺄셈을 먼저 하면 안 된다.** `GetTickCount64()`는 부호 없는 값이라 마감이 이미 지났을 때 뺄셈이 언더플로해 거대한 값이 되고, `DWORD`로 좁히면 `0xFFFFFFFF`, 즉 `INFINITE`에 착지할 수 있다. 그러면 루프가 영원히 깨지 않는다.

```text
next_timeout:
    if 대기 중인 타이머 없음:  return INFINITE
    t = now()                              # 한 번만 읽는다
    d = next_deadline()
    if d <= t:                return 0
    return (DWORD) min(d - t, INFINITE - 1)
```

`now()`를 두 번 부르면 안 된다. 비교와 뺄셈 사이에 시계가 `d`를 지나가면 언더플로해서 타임아웃이 약 49.7일이 되고, 루프가 사실상 멈춘다.

`WSAPoll`을 쓰지 않는 이유는 Wintun 읽기 대기가 소켓이 아니라 Win32 이벤트 핸들이기 때문이다. 소켓과 이벤트 핸들을 한 번의 대기에 섞으려면 `WaitForMultipleObjects`여야 한다. `WSAEventSelect`는 소켓을 자동으로 논블로킹 모드로 바꾸므로 `ioctlsocket(FIONBIO)`를 따로 부르지 않는다.

#### 3.2.3 루프 한 바퀴

```text
busy = false
loop:
    r = WaitForMultipleObjects(n, handles, FALSE, busy ? 0 : next_timeout())

    if r == WAIT_FAILED:                      # 복구 불가
        로그 후 shutdown(); break             # 정상 경로와 같은 정리를 거친다
    if r == WAIT_OBJECT_0 + idx(SHUTDOWN):
        shutdown(); break

    WSAEnumNetworkEvents(sock, udp_ev, &ne)   # 소켓 이벤트를 리셋한다. 필수
    u = drain_udp(MAX_DRAIN)                  # r로 분기하지 않는다
    w = drain_wintun(MAX_DRAIN)
    if w == RESTART:
        어댑터 세션 재생성 -> 실패하면 shutdown(); break
        성공하면 살아 있는 핸들로 배열과 인덱스 대응표 재구성
    drain_console()
    run_expired_timers(now())
    busy = (u == BUDGET) or (w == BUDGET)
```

`WAIT_FAILED`도 `shutdown()`을 거친다. 여기서 바로 빠져나가면 `CLOSE` 송신과 어댑터 정리를 건너뛰어 상대는 50초 동안 죽은 세션을 붙들고 어댑터와 라우트가 남는다. **비정상 종료야말로 정리가 필요한 경우다.**

**`WSAEnumNetworkEvents`를 반드시 부른다.** `WSAEventSelect`가 만드는 이벤트는 수동 리셋이고, `recvfrom`은 `FD_READ` 통지를 재무장할 뿐 이미 신호된 이벤트 객체를 리셋하지 않는다. 이 호출을 빠뜨리면 첫 데이터그램 이후 이벤트가 계속 신호 상태로 남아 `WaitForMultipleObjects`가 매번 즉시 반환하고, 루프가 CPU를 태우며 스핀한다. 이 호출이 이벤트 리셋과 네트워크 이벤트 조회를 한 번에 처리한다.

**`r`로 분기하지 않는다.** `bWaitAll = FALSE`인 `WaitForMultipleObjects`는 신호된 핸들 중 **가장 낮은 인덱스 하나만** 반환한다. 반환값으로 분기하면 UDP 이벤트가 계속 신호되는 동안 그보다 뒤에 놓인 Wintun 이벤트가 굶는다. 게임 트래픽이 많을 때가 정확히 그 상황이다. 매 바퀴 양쪽을 모두 비우면 인덱스 순서와 무관해지고, 비어 있을 때의 비용은 `WSAEWOULDBLOCK` 하나와 `ERROR_NO_MORE_ITEMS` 하나뿐이다.

**두 drain은 예산을 가진다.** 예산 없이 비어질 때까지 돌면, 도착 속도가 처리 속도보다 빠를 때 `drain_udp`가 영원히 반환하지 않고 타이머가 한 번도 돌지 않는다. keepalive와 `HELLO` 재전송이 멈추고, 바로 위에서 세운 "수신 먼저, 타이머 나중" 순서가 "타이머는 절대 안 돈다"로 변한다. `MAX_DRAIN`은 한 바퀴당 소스별 64개다. 1472바이트 기준 94KB이고 파싱과 송신 비용이 1ms 미만이라, 가장 짧은 타이머(200ms `HELLO` 재전송) 대비 무시할 수 있다. 예산에 걸려 멈추면 `busy`가 참이 되어 다음 대기가 타임아웃 0으로 즉시 반환한다. 남은 데이터는 다음 바퀴에 처리되고 **그 사이에 타이머가 한 번 돈다.**

```text
drain_udp(budget) -> {EMPTY, BUDGET}:
    budget회 반복:
        n = recvfrom(sock, buf, MAX_DATAGRAM, &src)
        if n < 0:
            e = WSAGetLastError()
            if e == WSAEWOULDBLOCK:  return EMPTY      # 소스가 비었다
            카운터 증가 후 return EMPTY                 # 소켓은 계속 사용한다
        classify(buf, n, src)                          # protocol.md 7장
    return BUDGET                                      # 예산 소진. 아직 남았다
```

`FD_READ`는 레벨 성격이다. `recvfrom` 후에도 데이터가 남아 있으면 이벤트가 다시 신호되므로, 한 바퀴에 한 개만 읽어도 데이터그램을 잃지는 않는다. **비우는 이유는 정확성이 아니라 비용이다.** 데이터그램마다 대기와 `WSAEnumNetworkEvents`를 왕복하면 깨어남 횟수가 수신 패킷 수만큼 늘고 타이머 처리도 그만큼 밀린다. 정확성 요건은 위의 `WSAEnumNetworkEvents` 호출이고, 비우기는 그 위에 얹는 배칭 정책이다.

```text
drain_wintun(budget) -> {EMPTY, BUDGET, RESTART}:
    if session == NULL:  return EMPTY                  # Phase 1~5. 어댑터가 없다
    budget회 반복:
        p = WintunReceivePacket(session, &size)
        if p == NULL:
            e = GetLastError()
            ERROR_NO_MORE_ITEMS -> return EMPTY         # 링이 비었다. 정상
            ERROR_HANDLE_EOF    -> 종료 이벤트 신호 후 return EMPTY
            ERROR_INVALID_DATA  -> return RESTART       # 링 손상. 아래 참조
            그 외                -> 카운터 증가 후 return EMPTY
        guard = 스코프 이탈 시 WintunReleaseReceivePacket(session, p)
        route_and_send(p, size)
    return BUDGET
```

`WintunReleaseReceivePacket`은 **스코프 가드로 건다.** 호출 순서상 `route_and_send` 뒤에 두면 그 안에서 조기 반환하거나 예외가 나갈 때 링 패킷이 샌다. 한 번 빠뜨릴 때마다 링 버퍼가 영구히 그만큼 줄고 결국 수신이 멈춘다.

`RESTART`는 `[loop]`가 처리한다. 링이 손상되면 세션을 새로 만들어야 하는데, **새 세션은 새 읽기 이벤트 핸들을 준다.** 따라서 세션 재생성과 대기 배열 재구성이 한 묶음이다. `drain_wintun` 안에서 재시작하면 자기가 방금 무효화한 핸들을 호출자가 계속 기다리게 된다. 재생성이 실패하면 어댑터 없이 계속 도는 대신 `shutdown()`으로 간다. 배열은 조밀하게 다시 만들어지므로 Wintun 핸들이 그냥 빠질 뿐 대기 자체는 정상 동작한다. 그래서 더 위험하다. **터널이 죽은 채 살아 있는 척한다.** 세션은 `CONNECTED`로 남고 keepalive도 계속 나가지만 게임 패킷은 한 개도 흐르지 않는다.

`drain_console`은 `[console]`이 큐에 넣은 명령 줄을 비운다. 이벤트가 자동 리셋이라 별도 리셋 호출이 필요 없다. Phase 6 이후에는 이 핸들도 스레드도 사라진다.

Wintun 문서는 부하가 높을 때 `ERROR_NO_MORE_ITEMS`에서 잠시 스핀한 뒤 이벤트를 기다리라고 권한다. **스핀하지 않는다.** 이 루프는 타이머도 책임지므로 스핀이 keepalive와 `HELLO` 재전송을 그만큼 지연시킨다. 스핀은 처리량 최적화이고 우리 목표는 지연(NFR-1)이다. Phase 7 실측에서 수신이 병목으로 나오면 그때 재검토한다.

#### 3.2.4 타이머

- 단조 시계는 `GetTickCount64()`를 쓴다. 밀리초 단위 64비트라 실질적으로 랩어라운드가 없다(약 5.8억 년). RTT 측정만 `QueryPerformanceCounter`를 쓰고, **두 시계의 값을 서로 비교하지 않는다.**
- 타이머 집합이 작다. 세션당 6개 내외이고 최소 범위에서 세션은 하나다. 매 바퀴 선형 주사로 가장 이른 마감을 구한다. 타이머 휠은 필요 없다.
- [`protocol.md`](protocol.md) 11장은 마감 비교에 엄격 부등호를 쓰고, 우선순위를 **꺼낸 시각** 기준으로 정의한다. 같은 바퀴에 꺼낸 수신 이벤트가 그 바퀴의 타이머보다 먼저 처리된다. **수신을 먼저 비우고 타이머를 나중에 도는 이 루프 순서가 그 규칙의 구현이다.** 도착 시각 기준이 아닌 이유는 drain 예산 때문이고, 그 근거는 protocol.md 11장에 있다.

#### 3.2.5 상태 소유

다음은 전부 `[loop]`가 단독 소유한다. 뮤텍스도 원자 변수도 쓰지 않는다. 필요가 없어서다.

세션 상태 머신, `session_epoch`, 송신 `sequence`, 재생 방지 비트맵, `peer_endpoint`, 마지막 수신 시각, 후보 목록, `pending_pings`, 모든 폐기 카운터.

**송신 `sequence` 증가는 반드시 `[loop]`에서만 일어난다.** `DATA`와 `KEEPALIVE`가 같은 카운터를 쓰므로 두 스레드에서 증가시키면 같은 번호를 단 패킷 두 개가 나간다. 받는 쪽 중복 억제([`protocol.md`](protocol.md) 4.5)는 뒤엣것을 정확히 버린다. 자기가 만든 위조 중복이다.

어떤 구조체에도 "스레드 안전"이라는 주석을 붙이지 않는다. 단일 소유가 유일한 규칙이고, 예외를 하나 허용하는 순간 규칙이 사라진다.

#### 3.2.6 텔레메트리 격리

`[telemetry]`가 별도 스레드인 이유는 병렬성이 아니라 **격리**다.

- `[loop]`는 고정 크기 SPSC 링 버퍼에 레코드를 넣는다. 생산자 하나, 소비자 하나뿐이라 원자 head/tail만으로 **락 없이** 구현된다. 가득 차면 **새 레코드를 버리고** 유실 카운터를 올린다. 대기 없음, 실패 보고 없음.
- 가장 오래된 것을 버리는 쪽이 지표 품질로는 낫지만, 그러려면 생산자가 소비자의 tail을 움직여야 해서 락이 필요하다. **지표 하나 더 살리자고 `[loop]`에 락을 들이지 않는다.** 뮤텍스로도 임계 구역이 짧아 실제로는 거의 안 막히겠지만, "거의"는 보장이 아니다. 소비자가 락을 쥔 채 스케줄에서 밀리면 `[loop]`가 그만큼 멈춘다.
- **데이터 평면에 락이 하나도 없다.**
- `[telemetry]`가 큐를 비워 제어 평면에 올린다. 업로드가 블록되든 실패하든 `[loop]`와 무관하다.
- 텔레메트리는 제어 평면 TCP 소켓을 쓴다. [`protocol.md`](protocol.md) 6장의 단일 소켓 제약은 **UDP 데이터 평면에만** 적용된다.

Phase 5의 "제어 서버 중단 10분" 시험(NFR-3)은 이 격리 없이는 통과할 수 없다. 4스레드 안에서는 응답하지 않는 제어 평면으로의 TCP 업로드가 타이머 스레드를 수십 초 막고, 그 사이 keepalive가 나가지 않아 NAT 매핑이 만료된다. 제어 평면 장애가 터널을 끊는다.

#### 3.2.7 종료

- `[telemetry]`는 `WaitForSingleObject(shutdown_ev, 1000)`으로 1초마다 깨어 큐를 비운다. push마다 이벤트를 신호하지 않는다. 종료 이벤트는 수동 리셋이라 신호되는 즉시 이 대기도 함께 깨운다.
- `[telemetry]`는 업로드 소켓에 `SO_SNDTIMEO` / `SO_RCVTIMEO` 1.5초를 건다. 업로드와 업로드 사이마다 종료 이벤트를 확인한다. 3초로 잡으면 진행 중인 업로드 하나가 아래 2초 join 상한을 넘어설 수 있다.
- `SetConsoleCtrlHandler`가 종료 이벤트를 신호한다. **핸들러는 `[loop]` 소유 상태를 절대 건드리지 않는다.** 다른 스레드 컨텍스트에서 실행되기 때문이다.
- **제어 신호 종류에 따라 반환 시점이 다르다.** `CTRL_C_EVENT`와 `CTRL_BREAK_EVENT`는 이벤트를 신호하고 즉시 `TRUE`를 반환하면 된다. 그러나 `CTRL_CLOSE_EVENT`, `CTRL_LOGOFF_EVENT`, `CTRL_SHUTDOWN_EVENT`는 **핸들러가 반환하는 순간 Windows가 프로세스를 종료한다.** 신호만 하고 돌아오면 `CLOSE` 송신도 어댑터 정리도 실행되지 않는다. 이 세 경우에는 종료 이벤트를 신호한 뒤 정리 완료 이벤트를 OS 유예 시간 안쪽(3초)까지 기다렸다가 반환한다. 콘솔 창을 닫는 것은 이 프로그램의 정상적인 종료 방법이므로 예외 처리가 아니라 주 경로다.
- `[loop]`의 `shutdown()`은 순서대로 (1) 종료 이벤트 신호(아직 안 됐으면), (2) 세션마다 `CLOSE` 1회 송신([`protocol.md`](protocol.md) 5.6), (3) 어댑터 세션 종료, (4) 어댑터/주소/라우트 정리, (5) **정리 완료 이벤트 신호**, (6) `[telemetry]` join을 수행한다.
- **큐에 종료 표식을 넣지 않는다.** 큐는 가득 차면 새 항목을 버리므로 하필 그때 표식이 버려지면 `[telemetry]`가 종료를 영영 못 본다. 종료는 큐 밖의 이벤트로만 전달한다.
- **정리가 join보다 먼저다.** 이 순서 덕분에 콘솔 핸들러는 텔레메트리를 기다리지 않고 반환할 수 있고, OS가 그 시점에 프로세스를 죽여도 잃는 것은 지표 몇 건뿐이다.
- join 상한은 2초다. 넘기면 남은 레코드를 포기하고 join을 건너뛴 채 `_exit`로 즉시 끝낸다. 정리가 (5)에서 이미 끝났으므로 안전하다. **상한을 실제로 강제하는 것은 소켓 타임아웃이 아니라 이 `_exit`다.** 소켓 타임아웃은 흔한 경우를 깔끔하게 끝내줄 뿐이고, 어떤 경우에도 종료가 제어 평면 가용성에 인질 잡히지 않게 하는 것은 `_exit`다.
- 어댑터, 가상 IP 주소, 라우트의 정리와 비정상 종료 후 잔존물 처리는 후속 3번에서 다룬다.

---

### 3.3 제어 평면 (Python)

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
 - 20 (TunnelHeader)
= 1452 바이트 (내부 IP 패킷 최대치)
```

가상 어댑터의 MTU를 1400 정도로 낮춰 잡아 조각화를 피한다.

다만 위 계산은 **외부 경로 MTU가 1500이라는 가정**에 의존한다. PPPoE 회선이나 다른 터널을 경유하는 경로는 이보다 작고, 그 경우 외부 UDP 패킷이 조각화된다. 정확한 값은 Phase 7에서 실측으로 확정하며, 보수적 고정값을 쓴다면 그 한계를 [`experiments.md`](experiments.md)에 명시한다.

---

## 5. 터널 프로토콜

### 5.1 헤더

```text
 오프셋  크기  필드              설명
   0      4   magic             0x53414E47 ("SANG")
   4      1   version           0x01
   5      1   type              PacketType
   6      2   payload_length    헤더 뒤 바이트 수
   8      4   peer_id           송신 피어 식별자
  12      4   session_epoch     송신 피어의 이번 실행 인스턴스
  16      4   sequence          방향별 단조 증가 카운터
  = 20 바이트, 네트워크 바이트 오더
```

`session_epoch`가 없으면 재시작 후 이전 인스턴스의 지연 패킷이 새 세션에 수용된다. 수신 측은 핸드셰이크에서 상대 epoch를 고정하고 이후 다른 epoch의 패킷을 폐기한다.

4바이트 필드가 모두 4의 배수 오프셋에 오도록 배치해 MSVC 기본 정렬에서도 패딩이 생기지 않는다. 그래도 구조체를 그대로 `memcpy` 하지 않는다. 바이트 오더 변환이 필요하고, 정렬이 우연히 맞는 것에 의존하면 조용히 깨진다. **필드 단위로 직렬화**하고 길이는 상수로 고정한다.

> **상세는 [`protocol.md`](protocol.md)에 있다.** 상수 값, 페이로드 레이아웃, 수신 검증 파이프라인, 후보 지명, 전이표 전체, 확정 타이머 값은 그 문서가 유일한 출처다. 이 절은 요약이며 충돌하면 `protocol.md`가 우선한다.

### 5.2 패킷 타입

| 타입 | 방향 | 페이로드 | 용도 |
|------|------|----------|------|
| `HELLO` `0x01` | 양방향 | nonce(16) + 가상 IP(4) | 홀펀칭 시도 및 핸드셰이크 개시 |
| `HELLO_ACK` `0x02` | 양방향 | echo nonce(16) + 가상 IP(4) | 상대가 내 패킷을 실제로 받았음을 증명 |
| `KEEPALIVE` `0x03` | 양방향 | 없음 | NAT 매핑 유지 (주기 15초) |
| `DATA` `0x04` | 양방향 | 내부 IPv4 패킷 | 게임 트래픽 전달 |
| `PING` `0x05` | 양방향 | ping_id(8) | RTT 측정. 타임스탬프는 와이어에 싣지 않는다 |
| `PONG` `0x06` | 양방향 | ping_id 에코(8) | RTT 측정 응답 |
| `CLOSE` `0x07` | 양방향 | reason(1) | 정상 종료 통지. 없으면 상대가 50초 유휴 타임아웃까지 죽은 세션을 붙든다 |

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

- `HANDSHAKING -> CONNECTED`: **두 개의 독립적인 플래그가 모두 서야 한다.** `got_ack`(우리가 보낸 nonce를 echo한 `HELLO_ACK` 수신, 우리 -> 상대 경로 확인)와 `sent_ack`(상대의 유효한 `HELLO`에 `HELLO_ACK` 송신, 상대 -> 우리 경로 확인)이다. 어느 쪽이 먼저 서는지는 정해져 있지 않다. 순서를 가정하면 한쪽만 뚫린 경로를 연결로 오판하거나 정상 순서에서 양쪽이 타임아웃한다.
- `CONNECTED -> FAILED`: keepalive 송신 실패가 아니라, 50초 동안 상대로부터 유효 패킷을 하나도 받지 못한 것으로 판정한다. keepalive는 15초 간격이고 `CONNECTED` 진입 즉시 1회 보내므로 3회 손실을 견딘다.

모든 상태 x 모든 수신 패킷의 전이는 [`protocol.md`](protocol.md) 9.4절 전이표에 있다. 특히 `PUNCHING`에서 `HELLO_ACK`이 먼저 도착하는 것과 `CONNECTED`에서 `HELLO`에 다시 응답하는 것은 정상 동작이며, 이를 빠뜨리면 양쪽이 타임아웃한다.

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
| keepalive 로컬 송신 오류 | `[loop]` 송신 경로 | 안정성 분석. 로컬 `sendto` 오류만 세며 전달 실패의 근거가 아니다. 그래서 단절 판정은 유휴 타임아웃으로 한다 |
| 유휴 타임아웃 발생 횟수 | 수신 경로 | 안정성 분석 |
| 직접 연결 유지 시간 | `CONNECTED` 지속 구간 | 안정성 분석 |
| 터널 처리량 | 송수신 바이트 카운터 | 성능 분석 |
| 세션 지속 시간 | 게임 세션 전체 | Minecraft 안정성 |

클라이언트는 로컬 링 버퍼에 먼저 쌓고 전용 스레드가 주기적으로 제어 평면에 보낸다. 업로드는 데이터 평면과 완전히 분리되어 있어 제어 평면 장애가 터널에 영향을 주지 않는다. 격리 구조는 3.2.6에 있다.

**업로드와 별개로 로컬 기록 파일이 있다.** [`spec.md`](spec.md) M-6 이 요구하는 것은 이것이고, 제어 평면 저장이 아니다. 계약은 넷이다.

| 항목 | 계약 |
|------|------|
| 내용 | 연결 시도 한 건마다 결과(성공 또는 [`spec.md`](spec.md) FR-13 의 실패 코드)와 RTT |
| 시점 | 세션 수립이 끝나는 시점. 성공이든 실패든 쓴다 |
| 방식 | 추가 전용(append-only). 이전 줄을 고치지 않는다 |
| 독립성 | 텔레메트리 큐와 업로드 경로에 의존하지 않는다. 제어 평면이 없어도 기록이 남는다 |

**경로와 줄 형식은 아직 정하지 않았다.** Phase 4 착수 전에 이 문서에서 먼저 정한다. 코드에서 임의로 정하지 않는다.

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
|   |   +-- protocol.md          터널 프로토콜 확정본 (구현 전 작성 완료)
|   |   +-- experiments.md       실험 설계와 측정 결과 (Phase 9에서 작성)
|   |   +-- audit-history/       전면 설계 점검 기록 보관소
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
