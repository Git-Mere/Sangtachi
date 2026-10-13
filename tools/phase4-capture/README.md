# phase4-capture

`roadmap.md` Phase 4 "검증" 가운데 **패킷 캡처로만 판정할 수 있는 항목**을 사람이 손으로 돌리는 절차다.
코드가 아니라 체크리스트다. 실행 파일도 스크립트도 없다.

- 같은 기기의 로그, 로컬 기록 파일, 하네스 로그로 판정하는 항목은 여기서 하지 않는다. 그것은 `scripts/session-check.ps1` 이 맡는다
- 단위 시험이 이미 보는 논리는 여기서 다시 보지 않는다. 여기서 보는 것은 **그 논리가 와이어에 실제로 낸 것**이다
- 값과 규칙의 출처는 [`roadmap.md`](../../docs/kor/roadmap.md) Phase 4 검증과 [`protocol.md`](../../docs/kor/protocol.md) 다. 이 문서는 값을 옮겨 적지 않고 절을 가리킨다. 이 문서가 스스로 정한 것은 아래 "이 문서가 정한 것" 표에만 있다

## 이 체크리스트가 덮는 roadmap 항목

`roadmap.md` Phase 4 검증의 항목을 위에서부터 차례로 적는다. 왼쪽 칸은 그 항목의 원문 첫 구절이다. 판정 자리는 넷 가운데 하나다.

- **C절, D절.** 이 문서의 절이다. C 는 한 기기로 된다. D 는 두 번째 기기가 필요하다
- **스크립트(session-check).** 같은 기기에서 로그, 기록 파일, 하네스 로그로 판정한다. 이 칸은 이 문서가 읽은 대로 적은 것이고 그 스크립트의 실제 범위는 그 스크립트가 정한다
- **단위 시험.** `tests/` 의 파일이다. 이름이 맞는 `TEST_CASE` 가 있는 파일만 적었다
- **보류.** 캡처가 필요한데 지금 개시 수단이 없다. 그 절에 무엇이 있어야 돌릴 수 있는지 적었다

| roadmap 항목 (원문 첫 구절) | 판정 자리 |
|------|------|
| "포트 포워딩 규칙이 없는 상태에서 서로 다른 가정망의 두 PC가 양방향 통신 성공" | D1 (두 번째 기기) |
| "**같은 LAN 의 두 PC 에서도 `CONNECTED` 에 도달한다.**" | 같은 기기의 두 프로세스는 스크립트(session-check 장면 `connect`). 학습한 주소는 로그 `session.endpoint` 로 본다. 실제 두 PC 는 D3 (두 번째 기기) |
| "패킷 캡처로 트래픽이 AWS를 경유하지 않음을 확인" | D1 (두 번째 기기) |
| "기동 절차 5번(" … "같은 공인 IP:Port 가 관측됨" | 로그(`stun.result`). C2 의 공개 STUN 실행(R4)에서 같이 읽는다. 배포 서버로는 D2 |
| "같은 캡처에서 **STUN 질의의 출발지 포트와 터널 송신의 출발지 포트가 같음**을 대조 (NFR-9)" | C2. 배포 서버로 다시 D2 |
| "헤더 직렬화 후 역직렬화 결과가 원본과 동일 (라운드트립 테스트)" | 단위 시험(`tests/tunnel/packet_test.cpp`) |
| "캡처한 패킷의 바이트 배열이 `protocol.md` 4.1(와이어 레이아웃)의 오프셋과 바이트 오더에" | C1 |
| "`PUNCHING` 상태에서 `HELLO_ACK`이 먼저 도착해도 정상 수용됨" | 단위 시험(`tests/peer/session_test.cpp`) |
| "`CONNECTED` 상태에서 상대가 `HELLO`를 재전송하면 `HELLO_ACK`을 다시 보냄" | 단위 시험(`tests/peer/session_test.cpp`) |
| "재협상 뒤 지연 패킷 처리." | 단위 시험(`tests/tunnel/tunnel_test.cpp`, `tests/peer/session_test.cpp`) |
| "응답 없는 후보로 계속 송신해도 `WSAECONNRESET`로 수신 루프가 죽지 않음" | 단위 시험(`tests/network/udp_socket_test.cpp`). C3 에서도 응답 없는 후보로 10초 쏜다 |
| "로컬 후보 판정, 등록 목록, 수신 후보 위생의 자기 서브넷 행" | 단위 시험(`tests/peer/candidates_test.cpp`, `tests/control/ops_test.cpp`) |
| "출발지 위생(`protocol.md` 7장)을 두 층에서 본다" | 단위 시험(`tests/loop_test.cpp` 가 (a), `tests/tunnel/tunnel_test.cpp` 가 (b)) |
| "종료 상태 수신(9.4.1)." | 카운터는 단위 시험(`tests/tunnel/tunnel_test.cpp`). "캡처로 무응답을 확인한다" 는 C6, **보류** |
| "종료 상태 세션의 보존(`protocol.md` 5.6 `CLOSE`)." | 단위 시험(`tests/tunnel/tunnel_test.cpp`) |
| "`IDLE` 에는 세션이 없다(`protocol.md` 9.1 상태)." | 단위 시험(`tests/tunnel/tunnel_test.cpp`) |
| "폐기 목록 상한(`protocol.md` 4.3 `session_epoch`)." | 단위 시험(`tests/peer/epoch_test.cpp`, `tests/tunnel/tunnel_test.cpp`) |
| "`session_epoch` 난수원(4.3)." | 단위 시험(`tests/peer/epoch_test.cpp`)과 코드 확인(`client/src/platform/win32/random.cpp`) |
| "재협상 뒤 타이머(`protocol.md` 9.5 재협상 10번)." | 셋째(유휴 정지)와 둘째는 단위 시험(`tests/peer/session_test.cpp`). 첫째 "(캡처)" 는 C7, **보류** |
| "재협상 상한(`protocol.md` 9.5)" | 단위 시험(`tests/peer/session_test.cpp`) |
| "잠정 경로 상한(`protocol.md` 10.4)" | 단위 시험(`tests/peer/session_test.cpp`). "(캡처로 확인)" 은 C8, **보류** |
| "무검증 목적지 공통 예산(`protocol.md` 9.4 전이표)." | 단위 시험(`tests/peer/session_test.cpp`). 나가는 송신 수를 와이어에서 세는 것은 C9, **보류** |
| "`CLOSE` 송신 조건(`protocol.md` 5.6 `CLOSE`, 9.6 실패 전이)" | C4 |
| "한쪽만 `HELLO`를 받은 상태에서는 `CONNECTED`로 전이하지 않음" | 단위 시험(`tests/peer/session_test.cpp`), 스크립트(session-check) |
| "호스트는 마지막 플레이어의 세션이 끝나도 방에 남는다" | 스크립트(session-check) |
| "방이 서버에서 끝나도 연결된 세션은 유지된다" | 스크립트(session-check 장면 `room-ended`. `KEEPALIVE` 는 `timer.tick`, `DATA` 왕복은 시험 빌드의 에코로 본다) |
| "세션이 끝나면 호스트의 `host_report` 가 다음 주기를 기다리지 않고 나간다" | 스크립트(session-check) |
| "호스트가 보고하지 못한 `departed` 는 다음 호출에 다시 담긴다" | 스크립트(session-check 장면 `departed-retry`), 단위 시험(`tests/control/lobby_test.cpp`). 스크립트는 첫 호출을 서버가 처리하기 전에 실패시킨다. 처리 뒤 응답만 잃는 경우는 회수가 멱등이라 다음 호출의 `departed` 가 서버 로그에 드러나지 않아 단위 시험만 본다 |
| "keepalive 주기: 캡처에서 `CONNECTED` 진입 즉시 1회, 그 뒤 15초 간격으로 `KEEPALIVE`가 나간다." | C5 |
| "합성한 내부 IPv4 패킷을 `DATA`로 양방향 왕복시켜 바이트가 일치 (M-5)." | 스크립트(session-check). 시험 빌드의 `data.tx`, `data.rx` 다 |
| "내부 출발지/목적지 가상 IP가 틀린 `DATA`가 `drop_inner_src` / `drop_inner_dst`로 폐기됨" | 단위 시험(`tests/peer/session_test.cpp`, `tests/tunnel/tunnel_test.cpp`) |
| "이름 형식 검사" | 단위 시험(`control-server/tests/test_name.py`, `tests/tunnel/packet_test.cpp`, `tests/control/lobby_test.cpp`) |
| "방 안 유일성" | 단위 시험(`control-server/tests/test_ops.py`, `control-server/tests/test_store.py`) |
| "이름은 자리와 함께 풀린다" | 단위 시험(`control-server/tests/test_ops.py`, `control-server/tests/test_host_report.py`) |
| "`peers` 원소에 `name` 이 있고" | 단위 시험(`control-server/tests/test_get_peers.py`, `tests/control/ops_test.cpp`) |
| "`FAIL` 줄의 `<상대>` 칸" | 스크립트(session-check), 단위 시험(`tests/control/lobby_test.cpp`) |
| "`ROSTER` 의 이름" | 단위 시험(`tests/tunnel/tunnel_test.cpp`, `tests/tunnel/packet_test.cpp`) |
| "로비 명령 `name` 으로 이름을 바꾸면 다음 `join` 요청이 새 이름을 싣는다." | 스크립트(`scripts/lobby-check.ps1` 의 장면 `name`) |
| "로컬 기록 파일의 줄 수가 도달 여부와 맞는다" | 스크립트(session-check), 단위 시험(`tests/record_test.cpp`, `tests/platform/record_file_test.cpp`) |
| "홀펀칭 실패 시 `HOLE_PUNCH_TIMEOUT`이 기록되고 `FAIL` 줄이 나온다." | 기록과 `FAIL` 줄은 스크립트(session-check). "`HELLO` 재전송이 멈추는 것도 캡처로 함께 본다" 는 C3 |
| "RTT 기준선 비교는 ICMP가 아니라" | 스크립트(session-check), 단위 시험(`tests/testbuild/instrument_test.cpp`) |
| "실패 코드 강제 발생 시험:" | 스크립트(session-check) |

## 이 문서가 정한 것

문서에 값이 없어 이 체크리스트가 정한 것이다. 바꾸려면 이 표를 고친다. roadmap 으로 옮길 만하면 그쪽을 먼저 고치고 여기서 지운다.

| 무엇 | 정한 값 | 근거 |
|------|---------|------|
| 주기 판정의 허용 폭 | 간격 `T` 인 송신의 연속 간격이 `T - 50ms` 이상 `T + 200ms` 이하. 200ms 재전송은 150~400ms, 15초 keepalive 는 14950~15200ms 다 | 상한은 roadmap Phase 1 과 Phase 3 이 200ms 타이머에 쓴 값(`elapsed_ms` 400 이하)과 같은 폭이다. 마감은 지나기 전에 돌지 않으므로(`protocol.md` 11장) 송신 간격은 원래 `T` 이상이다. 하한의 50ms 는 캡처 시각과 `sendto` 시각의 차이를 위한 여유다. 그 차이를 잰 적은 없다. 이 폭으로도 100ms 로 쏘는 구현과 10초나 30초로 쏘는 구현은 걸린다 |
| "진입 즉시" 의 판정 | 첫 `KEEPALIVE` 가 같은 쪽의 마지막 `HELLO` 뒤 400ms 이내 | `CONNECTED` 진입이 `HELLO` 재전송을 멈추고 같은 자리에서 `KEEPALIVE` 를 보낸다(`protocol.md` 11장). 진입 직전 `HELLO` 는 많아야 한 주기 전이다. 그래서 200ms 에 위 상한 여유를 더했다. 진입 때 보내지 않는 구현은 첫 `KEEPALIVE` 가 15초 뒤라 걸린다 |
| "나가지 않는다" 를 보는 창 | 멈춰야 하는 시점부터 10초 | 멈추지 않는 구현은 200ms 마다 쏘므로 10초면 약 50개가 보인다 |
| 캡처 도구 | Wireshark 와 Npcap. Npcap 은 루프백 캡처를 켜고 설치한다. 두 기기 실행(D절)은 pktmon 도 된다 | 같은 기기의 두 프로세스가 LAN 주소로 주고받는 패킷은 실제 NIC 를 지나지 않는다. 이 문서를 쓴 기기에서는 루프백에서만 보였다(C0). 기기마다 C0 에서 먼저 확인한다 |
| 캡처 범위 | 루프백 어댑터와 실제 NIC 를 **동시에** 잡는다 | 같은 기기 구성에서 상대 후보가 둘이다. LAN 주소 후보는 루프백 쪽에, 하네스 STUN 이 적는 `192.0.2.1` 후보는 기본 경로를 따라 NIC 쪽에 보인다(C0). 한쪽만 잡으면 "후보 전체로 뿌리는 구현" 을 놓친다 |
| 캡처 파일의 자리 | 레포 밖 | D절 캡처에는 두 가정의 공인 IP 가 들어 있다. `tools/nat-probe` 의 원본과 같은 이유다 |

## 준비

### 빌드

**제품 빌드로 돌린다.** 이 문서의 어느 절도 시험 빌드 장치(`data.tx`, 에코 응답기, `probe200`)를 쓰지 않는다.
시연에 나가는 것이 제품 빌드이므로 와이어 확인도 그것으로 한다.

```powershell
pwsh -NoProfile -File scripts/build.ps1
```

실행 파일은 `<빌드 디렉터리>\client\sangtachi_client.exe` 다. 빌드 디렉터리는 `scripts/buildpath.ps1` 이 정한다.

### 같은 기기 구성 (C절)

DynamoDB local 이 떠 있어야 한다. 띄우는 명령은 `control-server/README.md` 에 있다.

1. 제어 서버 하네스를 띄운다. 표준 오류의 `HARNESS ready` 줄에서 `cp_port` 와 `stun_ports` 를 읽는다

   ```powershell
   cd control-server
   .venv\Scripts\python tests\harness\cp_harness.py --stun 2
   # HARNESS ready cp_port=<CP> admin_port=<ADMIN> stun_ports=<S1>,<S2> table=... mapped_ip=192.0.2.1
   ```

   하네스는 제어 서버와 STUN 응답기 둘을 `127.0.0.1` 에 연다. STUN 응답기는 매핑 주소를 `192.0.2.1`
   (문서용 대역)로 적는다. 이유는 `cp_harness.py` 의 `DEFAULT_MAPPED_IP` 위에 있다

2. 클라이언트마다 창 하나를 연다. 표준 오류는 파일로 돌리고 표준 출력(`ROOM`, `FAIL`)과 콘솔 입력은 창에 둔다

   ```powershell
   $exe = '<빌드 디렉터리>\client\sangtachi_client.exe'
   & $exe --name Alice --server 127.0.0.1:<CP> --stun 127.0.0.1:<S1> --stun 127.0.0.1:<S2> 2> A.err
   ```

   다른 창에서 `Get-Content A.err -Wait` 로 로그를 본다

3. 로그 첫머리의 `socket.bind local=0.0.0.0:<P>` 에서 그 클라이언트의 UDP 포트 `P` 를 적는다. 아래 표시 필터는
   모두 이 포트로 거른다. 클라이언트는 `host` 나 `join` 전에는 UDP 를 보내지 않는다. 그 사이에 캡처를 시작하면 된다

4. 방을 만들고 참가하는 명령은 로비 명령 `host` 와 `join <방 코드>` 다(`architecture.md` 3.5). 방 코드는 호스트 창의
   `ROOM <코드>` 줄이다

**가짜 피어.** C3 과 C4 의 실패 장면은 하네스의 가짜 피어 `fake_host`, `fake_player` 를 쓴다. 모드는 `silent`,
`ack_only`, `hello_only`, `close01`, `close03` 이다. 거는 방법은 `cp_harness.py` 머리가 갖는다. **이 문서는 모드
이름에서 아래 거동을 기대한다.** 판정 전에 캡처로 그렇게 하는지 먼저 본다. 다르면 이 문서를 고친다.

| 모드 | 이 문서가 기대하는 것 | 캡처에서 먼저 볼 것 |
|------|----------------------|--------------------|
| `silent` | 후보는 등록하고 UDP 는 하나도 보내지 않는다 | 가짜 피어 쪽에서 `P` 로 오는 터널 패킷이 0개 |
| `hello_only` | 자기 후보 주소에서 `HELLO` 만 보내고 `HELLO_ACK` 은 보내지 않는다 | 가짜 피어 → `P` 의 타입이 `0x01` 뿐이고, 우리 로그에 `session.state ... to=HANDSHAKING` 이 있다 |

### 캡처 도구

Wireshark 와 Npcap 을 설치한다. Npcap 설치 화면에서 루프백 캡처 지원을 켠다. 설치에는 관리자 권한이 필요하다. 이 문서를 쓴 기기에서는 설치 뒤 캡처(`dumpcap`, `tshark`)가 일반 사용자로 됐다.

**C0. 구성 확인.** 처음 한 번, 그리고 기기나 망이 바뀔 때마다 한다.

1. 루프백 어댑터와 실제 NIC(이 기기에서는 `Wi-Fi`)를 함께 골라 캡처를 시작한다
2. 아래 R1 을 `CONNECTED` 까지 돌린다
3. 두 클라이언트의 `P` 로 거른다. 아래 표를 채운다

| 패킷 | 어느 인터페이스에서 보였나 |
|------|---------------------------|
| 클라이언트 → 하네스 STUN (`127.0.0.1`) | |
| A ↔ B, 목적지가 LAN 주소 | |
| A, B → `192.0.2.1` | |

**이 문서를 쓴 기기에서 한 번 채웠다.** Windows 11, Wireshark 4.6.9, Npcap 1.89(루프백 캡처 켬). 캡처는 `dumpcap` 으로
루프백 어댑터와 `Wi-Fi` 를 함께 잡았다. Npcap 설치에는 관리자 권한이 들었고 캡처 자체는 일반 사용자로 됐다.

| 패킷 | 어느 인터페이스에서 보였나 |
|------|---------------------------|
| 클라이언트 → 하네스 STUN (`127.0.0.1`) | 루프백 |
| A ↔ B, 목적지가 LAN 주소 | 루프백. 같은 기기의 LAN 주소로 가는 패킷은 `Wi-Fi` 에 보이지 않았다 |
| A, B → `192.0.2.1` | `Wi-Fi` |

LAN 주소 행이 어느 인터페이스에도 보이지 않으면 그 캡처로는 C1, C3, C4, C5 를 판정할 수 없다. 그 실행은 판정에
쓰지 않는다.

## 표시 필터

`P` 는 그 클라이언트의 포트다. Wireshark 의 `udp[i:n]` 은 UDP 헤더 첫 바이트부터 센다. 그래서 UDP 페이로드는
`udp[8]` 부터이고 터널 헤더(`protocol.md` 4.1)는 아래 자리에 온다.

| 필드 | 4.1 오프셋 | 필터 자리 |
|------|:---:|-----------|
| `magic` | 0 | `udp[8:4]` |
| `version` | 4 | `udp[12:1]` |
| `type` | 5 | `udp[13:1]` |
| `payload_length` | 6 | `udp[14:2]` |
| `peer_id` | 8 | `udp[16:4]` |
| `session_epoch` | 12 | `udp[20:4]` |
| `sequence` | 16 | `udp[24:4]` |
| 페이로드 첫 바이트 (`CLOSE` 의 `reason`) | 20 | `udp[28:1]` |
| `HELLO` 의 `virtual_ip` (5.1 오프셋 16) | 36 | `udp[44:4]` |

| 무엇 | 필터 |
|------|------|
| 그 클라이언트의 UDP 전부 | `udp.port == P` |
| 그 클라이언트가 보낸 터널 패킷 | `udp.srcport == P && udp[8:4] == 53:41:4e:47` |
| 타입 하나 | 위에 `&& udp[13:1] == 01` 을 더한다. 값은 `protocol.md` 5장 표다. `01` `HELLO`, `03` `KEEPALIVE`, `07` `CLOSE` |
| `CLOSE` 의 `reason` | 위에 `&& udp[13:1] == 07 && udp[28:1] == 00` |
| 그 클라이언트의 STUN Binding Request | `udp.srcport == P && udp[8:2] == 00:01 && udp[12:4] == 21:12:a4:42` |

셀 수 있는 것은 눈으로 세지 않는다. 캡처를 `pcapng` 로 저장하고 `tshark` 로 줄을 뽑아 센다.

```powershell
tshark -r run.pcapng -Y "udp.srcport == P && udp[8:4] == 53:41:4e:47 && udp[13:1] == 01" `
       -T fields -e frame.time_relative -e ip.dst -e udp.dstport
```

간격은 같은 목적지(`ip.dst`, `udp.dstport`)끼리 이웃한 `frame.time_relative` 의 차다.

## 실행

절마다 따로 돌리지 않고 실행 넷을 돌려 절들이 나눠 쓴다. 실행마다 캡처 파일 하나다.

| 실행 | 구성 | 콘솔 순서 | 쓰는 절 |
|------|------|-----------|---------|
| R1 | 실제 클라이언트 둘. A `--name Alice`, B `--name Bob` | A `host` → B `join <코드>` → 둘 다 `session.state ... to=CONNECTED` → 70초 기다린다 → B `send 1` → A `quit` → 5초 뒤 B `quit` | C0, C1, C2, C4-2, C5 |
| R2 | A 하나와 `fake_player` `silent` | A `host` → 가짜 피어 참가 → A 의 `session.state ... to=PUNCHING` → `FAIL HOLE_PUNCH_TIMEOUT` 을 기다린다 → 그 뒤 15초 → 같은 방에 가짜 피어를 다시 붙이고 `to=PUNCHING` 이 나오면 10초가 지나기 전에 A `quit` | C3, C4-1, C4-5 |
| R3 | A 하나와 `fake_player` `hello_only` | A `host` → 가짜 피어 참가 → A 의 `to=HANDSHAKING` → `FAIL PEER_HANDSHAKE_FAILED` 를 기다린다 → 그 뒤 15초 → 가짜 피어를 다시 붙이고 `to=HANDSHAKING` 이 나오면 10초가 지나기 전에 A `quit` | C4-3, C4-4 |
| R4 | R1 과 같되 두 클라이언트에 `--stun` 을 주지 않는다. 기본 목록의 공개 STUN 서버를 쓴다. 제어 서버는 하네스 그대로다 | R1 과 같다. `CONNECTED` 뒤 바로 A `quit` 해도 된다 | C2 |

R2, R3 에서 A 는 호스트이므로 쌍이 실패해도 방에 남는다(`concurrency.md` 7장 로비). 그래서 같은 방에 가짜 피어를
다시 붙일 수 있다. 역할을 바꿔 A 를 플레이어로, 가짜 피어를 `fake_host` 로 해도 판정은 같다. 그때 A 는 실패 뒤
로비로 가므로 두 번째 장면은 `join` 부터 다시 한다.

## C1. 와이어 레이아웃

**roadmap 원문.** "캡처한 패킷의 바이트 배열이 `protocol.md` 4.1(와이어 레이아웃)의 오프셋과 바이트 오더에
정확히 일치. 첫 4바이트가 `53 41 4E 47`"

- **구성.** 한 기기, 제품 빌드, R1
- **캡처.** C0 이 LAN 주소 행을 찾은 인터페이스
- **필터.** `udp.port == P_A || udp.port == P_B` 로 보고 타입마다 한 프레임씩 고른다

**기대 관측과 판정.** 아래가 모두 맞으면 통과다. 하나라도 어긋나면 실패다.

| 확인 | 통과 조건 |
|------|-----------|
| 매직과 버전 | `udp.srcport == P && !(udp[8:4] == 53:41:4e:47) && !(udp[12:4] == 21:12:a4:42)` 가 0개다. 터널도 STUN 도 아닌 것을 보내지 않았다는 뜻이다. 터널 프레임은 모두 `udp[12:1] == 01` 이다 |
| 타입마다 하나씩 | `HELLO`(01), `HELLO_ACK`(02), `KEEPALIVE`(03), `DATA`(04), `PING`(05), `PONG`(06), `CLOSE`(07), `ROSTER`(08, 호스트 A 만 보낸다) 가 다 보인다 |
| `payload_length` | 고른 프레임마다 `udp[14:2]` 를 big-endian 으로 읽은 값이 `UDP 길이 - 8 - 20` 과 같다. 그 값이 `protocol.md` 5장 표의 길이와 맞는다. `HELLO`, `HELLO_ACK` 20, `KEEPALIVE` 0, `PING`, `PONG` 8, `CLOSE` 1, `send 1` 의 `DATA` 100, `ROSTER` 16~135 |
| `peer_id` 의 바이트 순서 | B 가 보낸 프레임의 `udp[16:4]` 를 big-endian 으로 읽은 10진 값이 A 로그의 `session.state peer_id=` 값과 같다. 거꾸로도 본다. 예를 들어 `2424547312` 는 `90 83 a7 f0` 이어야 하고 `f0 a7 83 90` 이면 little-endian 이라 실패다 |
| `HELLO` 의 가상 IP | B 의 `HELLO` 의 `udp[44:4]` 가 A 로그 `control.peers virtual_ip=` 의 네 옥텟과 같은 순서다. `10.100.0.2` 면 `0a 64 00 02` |
| `session_epoch` | 한 쪽이 보낸 프레임의 `udp[20:4]` 가 그 실행 내내 같고 0 이 아니다 |
| `sequence` | `CONNECTED` 뒤 한 쪽이 보낸 프레임(목적지가 하나다)을 시간 순으로 놓으면 `udp[24:4]` 를 big-endian 으로 읽은 값이 늘기만 한다. little-endian 으로 쓴 구현은 256 개마다 BE 값이 줄어들어 여기서 걸린다. 짧은 실행에서도 첫 바이트(`udp[24:1]`)가 바뀌고 마지막 바이트가 그대로인 이웃 쌍이 보이면 실패다. 빈 번호는 판정하지 않는다. 실패한 송신도 번호를 쓰기 때문이다(`tests/peer/session_test.cpp` "sequence keeps counting across renegotiation and failed sends") |

**걸리는 구현.** 필드를 little-endian 으로 쓰는 것, 구조체를 `memcpy` 해 패딩이나 순서가 어긋난 것, 매직을 다른
값으로 쓰는 것, `payload_length` 에 헤더를 더해 적는 것.

## C2. STUN 질의와 터널 송신의 출발지 포트

**roadmap 원문.** "같은 캡처에서 **STUN 질의의 출발지 포트와 터널 송신의 출발지 포트가 같음**을 대조 (NFR-9)."

- **구성.** 한 기기, 제품 빌드. R1(하네스 STUN)과 R4(공개 STUN) 둘 다 본다
- **캡처.** R1 은 C0 의 STUN 행과 LAN 행 인터페이스. R4 의 STUN 질의는 실제 NIC 로 나간다
- **필터.** 이 절은 포트로 먼저 거르지 않는다. 포트가 판정 대상이기 때문이다. 같은 기기의 두 클라이언트는 출발지
  주소가 같으므로 포트로만 갈린다. 그래서 아래 순서대로 본다

1. 캡처 전체에서 STUN Binding Request(`udp[8:2] == 00:01 && udp[12:4] == 21:12:a4:42`)의 출발지 포트 집합을 뽑는다.
   R1 이면 목적지가 하네스 STUN 포트(`S1`, `S2`)인 것만 센다. R4 는 같은 기기의 다른 프로그램도 공개 STUN 서버에 묻는다(이
   문서를 쓴 기기에서는 Tailscale 의 `tailscaled` 가 출발지 41641 에서 보냈다). 그래서 실행 중에 두 클라이언트 프로세스가 가진
   UDP 포트를 모두 적고(`Get-NetUDPEndpoint -OwningProcess <PID>`, 관리자 권한이 필요 없다) 그 포트에서 나간 것만 센다. 남은
   출발지 포트는 그 포트를 가진 프로세스를 적어 우리 것이 아님을 보인다. 로그의 `stun.result server=` 는 해석 전의 이름이라
   목적지로 거르는 데 쓸 수 없다
2. 캡처 전체에서 터널 패킷(`udp[8:4] == 53:41:4e:47`)의 출발지 포트 집합을 뽑는다
3. 두 클라이언트 로그의 `socket.bind local=0.0.0.0:<P>` 를 적는다

**판정.** 통과는 셋이 다 맞는 것이다.

- STUN 출발지 포트 집합이 `{P_A, P_B}` 와 같다. R4 는 두 클라이언트 프로세스가 가진 UDP 포트가 각각 그 `P` 하나뿐이어야 한다
- 터널 출발지 포트 집합이 `{P_A, P_B}` 와 같다
- 각 클라이언트의 `stun.result` 응답이 돌아온 포트(STUN 응답의 목적지 포트)가 그 클라이언트의 `P` 다

포트가 셋 이상이면 실패다. 어느 클라이언트가 STUN 이나 터널에 소켓을 따로 열었다는 뜻이다. 둘이어도 STUN 쪽 집합과
터널 쪽 집합이 다르면 실패다.

**같이 적는 것.** R4 에서 각 클라이언트의 `stun.result mapped=` 두 줄이 같은 `IP:port` 인지 적는다. 그것이 roadmap 의
"기동 절차 5번 … 같은 공인 IP:Port 가 관측됨" 이다. 갈리면 실패로 적지 않고 미지원으로 적는다(roadmap 원문).
`mapped` 의 포트가 `P` 와 다르면 NAT 이 포트를 바꾼 것이다. 이 절의 판정과 무관하다. 판정은 출발지 포트이고 매핑
포트가 아니다.

**걸리는 구현.** STUN 에 소켓을 따로 여는 것, 시도마다 다시 bind 하는 것. 로그(`socket.bind`)는 자기 보고라 이것을
못 잡는다.

## C3. 펀치의 `HELLO` 재전송과 `HOLE_PUNCH_TIMEOUT` 뒤 정지

**roadmap 원문.** "그 전이에서 `HELLO` 재전송이 멈추는 것도 캡처로 함께 본다. 멈추지 않는 구현은 끝난 세션이
후보 전체로 계속 쏜다"

- **구성.** 한 기기, 제품 빌드, R2 의 첫 장면
- **캡처.** C0 의 두 인터페이스를 함께
- **필터.** `udp.srcport == P_A && udp[8:4] == 53:41:4e:47 && udp[13:1] == 01`

`t0` 은 이 장면의 첫 `HELLO` 의 시각이다. 펀치 시작에 모든 후보로 `HELLO` 를 보내고(`protocol.md` 10.3) 펀치 마감은
그 시작에서 10초다.

**판정.**

| 확인 | 통과 조건 |
|------|-----------|
| 모든 후보로 | A 로그의 `control.peers candidates=` 에 있는 가짜 피어 후보마다 `HELLO` 가 있다 |
| 200ms 간격 | 후보마다 이웃한 `HELLO` 의 간격이 150~400ms 다 |
| 마감에서 멈춘다 | 마지막 `HELLO` 가 `t0 + 10.2초` 보다 앞이다 |
| 끝난 뒤 나가지 않는다 | `t0 + 10.2초` 부터 10초 동안 `P_A` 에서 나간 `HELLO` 가 0개다 |
| 실패 줄 | A 창에 `FAIL HOLE_PUNCH_TIMEOUT` 줄이 하나 나왔다. 줄 자체의 판정은 session-check 가 한다. 여기서는 이 장면이 그 실패였는지만 확인한다 |

**걸리는 구현.** 실패 전이에서 재전송 타이머를 멈추지 않는 것(`protocol.md` 9.6 1번), 첫 후보에만 쏘는 것.

## C4. `CLOSE` 송신 조건과 `reason`

**roadmap 원문.** "`peer_endpoint`를 학습한 세션에서 종료하면 `CLOSE`가 그 주소로 1회 나가고, 학습 전에
종료하면 **한 개도 나가지 않는다**(캡처로 확인)." 그리고 "reason 값도 캡처에서 본다. `CONNECTED` 세션을 `quit` 으로
닫으면 `0x00`, 수립 중에 `quit` 하면 `0x01`, 펀치 마감으로 실패하면 `0x03` 이다."

- **구성.** 한 기기, 제품 빌드. 장면마다 실행이 다르다(아래 표)
- **캡처.** C0 의 두 인터페이스를 **함께.** 상대 후보 가운데 `192.0.2.1` 이 NIC 쪽에 있으므로 한쪽만 잡으면 후보 전체로
  뿌리는 구현을 놓친다
- **필터.** `udp.srcport == P_A && udp[8:4] == 53:41:4e:47 && udp[13:1] == 07`

**학습한 주소는 로그 `session.endpoint` 가 싣는다**(`architecture.md` 9장). 값이 바뀔 때마다 한 줄이라 `CLOSE` 직전의
마지막 줄이 그 순간의 주소다. 줄이 없으면 학습 전이다. 캡처로도 대조한다. `protocol.md` 10.4 에 따라 `KEEPALIVE` 와
`PING` 은 학습한 `peer_endpoint` 로 가고, `HELLO_ACK` 같은 응답은 받은 데이터그램의 출발지로 간다.

- `CONNECTED` 세션: 그 쪽이 `CLOSE` 직전에 보낸 마지막 `KEEPALIVE` 나 `PING` 의 목적지
- 수립 중 세션: 상대가 보낸 마지막 터널 패킷의 출발지. 학습은 후보 집합 안의 출발지에서 온, 검증을 통과하고 `sequence`
  를 전진시킨 패킷으로 한다(`protocol.md` 10.4)

| 장면 | 실행 | 그 순간의 세션 | 기대 | 통과 조건 |
|------|------|----------------|------|-----------|
| C4-1 | R2 의 둘째 장면 (`silent`, 10초 전 `quit`) | 학습 전 `PUNCHING` | 나가지 않는다 | `quit` 앞뒤로 `P_A` 에서 나간 `CLOSE` 가 0개 |
| C4-2 | R1 의 A `quit` | `CONNECTED`, 학습 완료 | `0x00` 1회 | `CLOSE` 가 정확히 1개, `udp[28:1] == 00`, 목적지가 학습한 주소 |
| C4-3 | R3 의 둘째 장면 (`hello_only`, 10초 전 `quit`) | 학습 후 `HANDSHAKING` | `0x01` 1회 | `CLOSE` 가 정확히 1개, `udp[28:1] == 01`. 목적지는 관측값으로만 적는다(아래 전제) |
| C4-4 | R3 의 첫 장면 (`hello_only`, 마감) | 학습 후 `HANDSHAKING` 에서 `PEER_HANDSHAKE_FAILED` | `0x03` 1회 | `CLOSE` 가 정확히 1개, `udp[28:1] == 03`. 그 뒤 C3 과 같은 창에서 `HELLO` 가 0개. 목적지는 관측값으로만 적는다(아래 전제) |
| C4-5 | R2 의 첫 장면 (`silent`, 마감) | 학습 전 `PUNCHING` 에서 `HOLE_PUNCH_TIMEOUT` | 나가지 않는다 | `P_A` 에서 나간 `CLOSE` 가 0개 |

**판정의 전제.** "그 주소로" 와 "후보 전체로" 는 상대 후보가 둘 이상이어야 가를 수 있다. C4-2 는 진짜 클라이언트끼리라 상대
후보가 둘(하네스가 적는 반사 주소와 로컬 주소)이다. A 로그의 `control.peers candidates=` 에 둘 있는지 먼저 본다. **C4-3 과
C4-4 의 가짜 피어는 후보를 하나만 등록한다.** 그래서 그 두 장면은 개수와 `reason` 만 판정하고 목적지 칸은 관측값으로만
적는다. 수립 중 세션에서 "그 주소로" 를 가르는 것은 후보 둘을 등록하는 가짜 피어가 생길 때까지 보류다.

**걸리는 구현.** 후보 전체로 `CLOSE` 를 뿌리는 것, 학습 전에도 보내는 것, `CONNECTED` 일 때만 보내는 것(C4-3 에서
0개가 된다), 세 `reason` 을 한 값으로 보내는 것.

## C5. keepalive 주기

**roadmap 원문.** "keepalive 주기: 캡처에서 `CONNECTED` 진입 즉시 1회, 그 뒤 15초 간격으로 `KEEPALIVE`가 나간다."

- **구성.** 한 기기, 제품 빌드, R1. `CONNECTED` 뒤 적어도 60초를 잡는다
- **캡처.** C0 의 두 인터페이스를 함께. 진입 직전의 마지막 `HELLO` 는 `192.0.2.1` 로 간 것일 수 있다
- **필터.** 쪽마다 `udp.srcport == P && udp[8:4] == 53:41:4e:47 && (udp[13:1] == 01 || udp[13:1] == 03)`

**판정.** A 와 B 를 따로 본다. 둘 다 통과해야 통과다.

| 확인 | 통과 조건 |
|------|-----------|
| 진입 즉시 | 첫 `KEEPALIVE` 가 같은 쪽의 마지막 `HELLO` 뒤 400ms 이내 |
| 그 뒤 15초 | 이웃한 `KEEPALIVE` 의 간격이 모두 14950~15200ms. 60초 창에 `KEEPALIVE` 가 4개 이상 |
| 진입 뒤 재전송 없음 | 첫 `KEEPALIVE` 뒤로 그 쪽이 보낸 `HELLO` 가 0개 |
| 한 주소로 | 모든 `KEEPALIVE` 의 목적지가 같다. 그 주소가 C4-2 의 "학습한 주소" 다 |

roadmap 원문은 "**여기서 재는 것은 우리 송신 주기이고 NAT 매핑 수명이 아니다.**" 다. 매핑 수명은 `tools/nat-lifetime` 이 잰다.

**걸리는 구현.** 진입 때 보내지 않는 것, 간격을 다른 송신으로 초기화하는 것, 학습 뒤에도 후보 전체로 보내는 것.

## 보류된 절

아래 넷은 캡처로 볼 것이 roadmap 에 적혀 있지만 지금 한 기기에서 그 상황을 만들 수단이 없다. 클라이언트에는 수신 경로
주입 콘솔 명령이 없다. 위 가짜 피어 모드 다섯도 이 상황을 만든다고 이 문서는 가정하지 않는다. **논리는 단위 시험이
보고 있다.** 남은 것은 그 논리가 와이어에 낸 것이다.

| 절 | roadmap 원문 | 있어야 하는 것 | 생기면 볼 것 | 지금 논리를 보는 시험 |
|----|--------------|----------------|--------------|----------------------|
| C6 | "`CLOSE`를 주고받아 `CLOSED`에 도달한 뒤 같은 상대가 보낸 `KEEPALIVE`가 `drop_terminal_state`로만 오르고 어떤 응답도 나가지 않는다. 캡처로 무응답을 확인한다" | `CONNECTED` 뒤 `CLOSE` 를 보내고도 같은 세션 값으로 `KEEPALIVE` 를 계속 보내는 가짜 플레이어. A 는 호스트여야 한다. 플레이어는 세션이 `CLOSED` 로 끝나면 로비로 가며 보존 세션을 지우므로(`concurrency.md` 7장 로비, `protocol.md` 5.6) 그 뒤 수신은 `drop_unknown_peer` 가 된다 | 그 피어의 `CLOSE` 뒤 10초 동안 `P_A` 에서 그 피어로 가는 터널 패킷이 0개 | `tests/tunnel/tunnel_test.cpp` "after CLOSE a KEEPALIVE only raises drop_terminal_state and gets no reply" |
| C7 | "`HELLO` 재전송이 200ms 간격으로 다시 나간다 (캡처)" | `CONNECTED` 세션에 다른 epoch 의 `HELLO` 를 같은 주소에서 보내는 주입기 | 주입 뒤 `P_A` 의 `HELLO` 가 상대 후보마다 150~400ms 간격으로 다시 나가고, 같은 구간에 `KEEPALIVE`(03)와 `PING`(05)이 0개 | `tests/peer/session_test.cpp` "renegotiation from CONNECTED follows the ten steps" |
| C8 | "검증 `HELLO`는 경로마다 1회만 나간다(캡처로 확인)." | 후보 집합 밖의 서로 다른 출발지 포트 다섯 곳에서 검증을 통과하고 `sequence` 를 전진시키는 패킷을 보내는 주입기 | 앞의 넷 각각으로 `HELLO` 가 정확히 1개, 다섯째로 0개 | `tests/peer/session_test.cpp` "four probe paths at most, the fifth packet is still processed" |
| C9 | "나가는 송신 수가 초기 10개 + 초당 5개 보충을 넘지 않고" | 후보 집합 밖 출발지에서 `HELLO` 를 초당 20회 보내는 주입기 | 그 출발지로 나간 `HELLO_ACK`, `PONG`, 검증 `HELLO` 의 합을 초 단위로 센다. roadmap 의 (a)~(e) 다 | `tests/peer/session_test.cpp` "the unverified budget counts tokens as the roadmap does" |

## 두 번째 기기 (D절)

`docs/kor/plan.md` "대기 중인 것" 의 두 번째 기기 묶음과 같이 돈다. 배포한 제어 서버와 공개 STUN 서버를 쓴다.
Elastic IP 와 `CONTROL_PORT` 는 `deploy/aws.local.md` 에 있다. **그 값을 이 문서나 기록에 옮겨 적지 않는다.**

캡처는 두 PC 각자의 실제 NIC 에서 잡는다. 기기 사이 트래픽은 NIC 를 지나므로 Wireshark 대신 Windows 에 들어 있는
pktmon 으로도 된다. 아래 명령은 이 기기의 `pktmon help` 출력에 맞췄다. 관리자 권한이 없어 실제로 돌려 보지는 못했다.

```powershell
pktmon filter add -t UDP -p <P>
pktmon start --capture --comp nics --pkt-size 0 -f run.etl
# ... 실행 ...
pktmon stop
pktmon etl2pcap run.etl -o run.pcapng
pktmon filter remove
```

`--comp nics` 를 준다. 기본값 `all` 은 한 패킷을 여러 구성 요소에서 한 번씩 적어 같은 패킷이 여러 번 보인다. 그러면 개수
판정이 틀린다. `P` 는 클라이언트를 띄운 뒤 `socket.bind` 에서 읽고, `host` 나 `join` 을 치기 전에 필터를 건다.

### D1. AWS 를 경유하지 않는 양방향 통신

**roadmap 원문.** "포트 포워딩 규칙이 없는 상태에서 서로 다른 가정망의 두 PC가 양방향 통신 성공", "패킷 캡처로
트래픽이 AWS를 경유하지 않음을 확인"

- **구성.** 서로 다른 가정망의 PC 둘. 공유기에 포트 포워딩 규칙이 없다. 망의 조건은 `tools/nat-probe` README 의 "측정 전에
  확인할 것" 과 같다
- **순서.** A `host` → B `join <코드>` → 둘 다 `CONNECTED` → 양쪽에서 `send 5` → 20초 기다린다 → A `quit`

**판정.**

| 확인 | 통과 조건 |
|------|-----------|
| AWS 미경유 | 각 PC 의 캡처에서 `udp && ip.addr == <Elastic IP>` 가 0개. 제어 서버와는 TCP 만 오간다 |
| 상대와 직접 | `udp.port == P && udp[8:4] == 53:41:4e:47` 의 상대 주소가 모두 상대의 후보 가운데 하나다. 후보는 자기 로그의 `control.peers candidates=` 다 |
| 양방향 | 두 PC 모두 상대에게서 온 `KEEPALIVE`, `DATA`, `PONG` 이 있다 |

### D2. 배포 환경의 출발지 포트

C2 를 두 PC 에서 다시 본다. STUN 질의는 공개 서버로, 터널은 상대 공인 주소로 나간다. 판정은 C2 와 같다. 각 PC 의
`stun.result mapped=` 두 줄이 같은지도 적는다.

### D3. 같은 LAN 의 두 PC

**roadmap 원문.** "학습된 `peer_endpoint` 가 상대의 로컬 후보인 것을 로그로 확인한다", "같은 NAT 뒤에서 공인 후보로
수렴하면 헤어핀에 기대는 것이므로 그 경우도 관측값으로 적는다"

학습한 주소는 로그 `session.endpoint peer_id=<상대> endpoint=<IPv4:port>` 가 싣는다(`architecture.md` 9장). 그 주소가
상대의 로컬 후보이면 기대대로이고, 상대의 공인 후보이면 헤어핀으로 적는다. 도달 판정은 로그의 `session.state ... to=CONNECTED`
다. 캡처에서는 `CONNECTED` 뒤 각 PC 가 보낸 `KEEPALIVE` 의 목적지가 그 주소와 같은지 대조한다.

## 기록 양식

실행마다 아래를 채운다. 캡처 파일 이름은 실행과 맞춘다. 판정 칸은 `통과`, `실패`, `판정 불가`, `보류` 가운데 하나다.
`판정 불가` 는 전제가 서지 않은 것이다(C0 에서 LAN 행이 안 보임, C4-2 의 상대 후보가 하나뿐, 가짜 피어가 기대와 다르게 움직임). C4-3 과 C4-4 는 가짜 피어의 후보가 원래 하나라 판정 불가가 아니다. 목적지는 관측값 칸에만 적고 판정에 넣지 않는다.
실패로 적지 않고 이유를 적는다.

```text
날짜:
커밋:                      (git rev-parse --short HEAD)
빌드:                      제품 / Debug|Release
기기와 망:
캡처 도구와 인터페이스:
C0 표:                     STUN 행 / LAN 행 / 192.0.2.1 행
P_A, P_B:
```

| 절 | 실행 | 캡처 파일 | 관측값 | 판정 | 비고 |
|----|------|-----------|--------|------|------|
| C1 | R1 | | 매직 아닌 프레임 수, 타입별 길이, peer_id 바이트, seq 증가 | | |
| C2 | R1, R4 | | STUN 출발지 포트 집합, 터널 출발지 포트 집합, R4 의 `mapped` 두 값 | | |
| C3 | R2 | | 후보별 `HELLO` 수, 간격 최소/최대, 마지막 `HELLO` − `t0`, 정지 뒤 10초의 `HELLO` 수 | | |
| C4-1 | R2 | | `CLOSE` 수 | | |
| C4-2 | R1 | | `CLOSE` 수, `reason`, 목적지, 학습한 주소 | | |
| C4-3 | R3 | | `CLOSE` 수, `reason`, 목적지 | | 목적지는 관측값. 수립 중 "그 주소로" 판정은 보류 |
| C4-4 | R3 | | `CLOSE` 수, `reason`, 목적지, 정지 뒤 10초의 `HELLO` 수 | | 목적지는 관측값. 수립 중 "그 주소로" 판정은 보류 |
| C4-5 | R2 | | `CLOSE` 수 | | |
| C5 | R1 | | A/B 각각 첫 `KEEPALIVE` − 마지막 `HELLO`, 간격 최소/최대, 개수, 목적지 | | |
| C6~C9 | - | - | - | 보류 | |
| D1 | 두 기기 | | Elastic IP 행 UDP 수, 상대 주소 목록, 방향별 타입 | | |
| D2 | 두 기기 | | C2 와 같다 | | |
| D3 | 두 기기 | | 각 PC 의 `KEEPALIVE` 목적지, 로컬/공인 | | |

**공인 IP 가 들어간 캡처와 기록은 커밋하지 않는다.** D절 원본은 레포 밖에 두고 보고서에는 가린 사본만 붙인다. C절
캡처에는 이 기기의 사설 주소만 있지만 같은 규칙으로 레포 밖에 둔다.
