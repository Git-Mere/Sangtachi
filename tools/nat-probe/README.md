# nat-probe

blocker 20 실측 도구. 두 망의 NAT 매핑 거동과 실제 UDP 홀펀칭 성립 여부를 측정한다.

**근거 문서:** [`docs/kor/plan.md`](../../docs/kor/plan.md) 5장 "직접 연결 불가 대비책",
[`docs/kor/design-audit.md`](../../docs/kor/design-audit.md) blocker 20.
**기록 양식:** [`RECORD-TEMPLATE.md`](RECORD-TEMPLATE.md)

이 디렉터리는 `docs/` 트리 밖이므로 kor/eng 미러 규칙 대상이 아니다. 한국어로만 유지한다.

---

## 1. 무엇을 재고 무엇을 재지 않는가

| 측정 | 얻는 것 | 얻지 못하는 것 |
|------|---------|----------------|
| `probe` (STUN Binding) | **매핑 거동.** 목적지가 달라져도 공인 엔드포인트가 그대로인지 | 필터링 거동. 외부에서 들어오는 패킷을 NAT이 통과시키는지 |
| `punch` (실제 왕복) | 두 망 사이에서 홀펀칭이 실제로 성립하는지 | 원인 분해. 실패했을 때 NAT 때문인지 방화벽 때문인지는 따로 좁혀야 한다 |

**STUN Binding은 매핑 거동만 알려주고 필터링 거동은 알려주지 않는다.** 홀펀칭 성립은 두 가지
모두에 달려 있다. 그래서 `probe` 결과만 보고 "된다/안 된다"를 판정하면 안 된다. 매핑은
엔드포인트 독립인데 필터링 때문에 안 되는 경우를 그대로 놓친다. 이것이 `plan.md` 5장이
"첫 번째만으로는 부족하다"라고 못박은 이유다.

판정은 항상 `punch` 결과까지 본 뒤에 내린다.

## 2. 실행 환경

| 항목 | 값 |
|------|-----|
| 언어 | Python 3.8 이상 |
| 의존성 | 없다. 표준 라이브러리만 쓴다 |
| 대상 OS | Windows 10 / 11 x64 |
| 관리자 권한 | 필요 없다 |
| 네트워크 | UDP 아웃바운드. 임시 포트 하나 |

클라이언트 본체(C++20 / Winsock2)와 무관한 독립 도구다. 빌드가 필요 없다.

## 3. CLI

```
python natprobe.py probe [--label NAME] [--port N] [--stun HOST:PORT ...] [--out DIR]
python natprobe.py punch [--label NAME] [--port N] [--peer IP:PORT] [--duration SEC] [--out DIR]
```

| 하위 명령 | 하는 일 | 필요 인원 |
|-----------|---------|-----------|
| `probe` | STUN 매핑 거동만 측정한다 | 혼자 실행 가능 |
| `punch` | STUN 측정을 먼저 하고, 이어서 **같은 소켓으로** 상대와 홀펀칭을 시도한다 | 양쪽 망에서 동시에 |

| 옵션 | 기본값 | 뜻 |
|------|--------|-----|
| `--label NAME` | 호스트명 | 이 측정 지점의 이름. 결과 파일 이름과 기록에 들어간다. `home-A` 처럼 기록 양식의 망 식별자와 맞춘다 |
| `--port N` | `0` (임시 포트) | 바인드할 로컬 UDP 포트. 재현을 위해 고정하고 싶으면 지정한다 |
| `--stun HOST:PORT ...` | 도구 내장 목록 (`DEFAULT_STUN_SERVERS`) | 질의할 STUN 서버. 여러 개를 나열한다. 기본 목록은 `natprobe.py`의 상수에서 확인한다 |
| `--peer IP:PORT` | 없음 | 상대의 공인 엔드포인트. 주지 않으면 대화식으로 물어본다 |
| `--duration SEC` | `30` | 펀치 시도 시간(초) |
| `--unsolicited` | 꺼짐 | 펀치 성립 후, 요청하지 않은 인바운드가 통과하는지 시험한다 (4.3). **양쪽이 함께 줘야 한다** |
| `--unsolicited-duration SEC` | `10` | 위 시험 시간(초) |
| `--out DIR` | `tools/nat-probe/results/` | 결과 JSON을 쓸 디렉터리 |

`--stun`과 `--peer`는 `punch`에서 같은 소켓을 공유한다. 소켓을 따로 열면 출발지 포트가 달라져
매핑도 달라지고, 그러면 목적지 의존성과 구분할 수 없다 (`spec.md` NFR-9와 같은 이유).

## 4. 실행 순서

### 4.1 단독 측정

각자 자기 망에서 먼저 돌려 본다. 상대가 필요 없다.

```
python natprobe.py probe --label home-A
```

`stun.mapping`이 `unknown`으로 나오면 서버 수가 부족한 것이다. `--stun`으로 서로 **다른 IP**를
가진 서버를 더 넣어 다시 돌린다.

### 4.2 펀치 시험

두 망에서 거의 동시에 시작해야 한다. 순서는 이렇다.

1. 양쪽이 각자 `punch`를 시작한다.

   ```
   python natprobe.py punch --label home-A --duration 30
   ```

2. 각자 화면에 자기 공인 엔드포인트(`IP:Port`)가 출력된다. 이것을 상대에게 전달한다.
   전화, 메신저, 무엇이든 상관없다. 제어 평면은 필요 없다.
3. 도구가 상대 엔드포인트를 묻는다. 받은 값을 입력한다.
4. 도구가 Enter 대기 상태로 멈춘다. **여기서 시각을 맞춘다.** 양쪽이 "셋, 둘, 하나"를 세고
   함께 Enter를 누른다.
5. `--duration` 초 동안 패킷을 주고받은 뒤 결과 JSON을 쓴다.

시작 시각이 크게 어긋나면 먼저 시작한 쪽의 패킷이 상대 NAT에 구멍이 뚫리기 전에 도착해
버려지고, 그 사이 상대 매핑이 만료될 수 있다. 결과가 `failure`인데 원인이 NAT이 아니라
타이밍인 경우가 생긴다. 한쪽이 늦었다고 판단되면 결과를 버리고 다시 한다.

`--peer`를 인자로 주면 3번 질문과 4번 Enter 대기를 **둘 다 건너뛰고** 바로 시작한다. 시각을
맞추려면 `--peer`를 생략하고 대화식으로 입력한다. 인자로 줄 때는 다른 방법으로 시작 시각을
맞춰야 한다.

**공용 컴퓨터에서는 `--peer`를 쓰지 않는다.** 상대의 공인 엔드포인트가 셸 기록과 프로세스
목록에 남는다. 비밀은 아니지만 남길 이유도 없다. 대화식 입력을 쓴다.

### 4.3 요청하지 않은 인바운드 시험 (`--unsolicited`)

**무엇을 묻는가.** 내가 먼저 보낸 적 없는 출발지에서 오는 UDP를 이 PC가 받는가.

**왜 중요한가.** Windows 방화벽은 UDP를 상태 기반으로 처리한다. 내가 먼저 보내면 그 상대
주소와 포트에 대해서만 인바운드가 열린다. 인바운드 기본 동작은 `Block`이고 보통 허용 규칙이
없다. 그래서 **상대 NAT이 목적지 의존 매핑을 쓰면** 상대는 내가 보낸 적 없는 포트로 오게 되고,
NAT이 통과시켜도 내 방화벽이 막는다. 홀펀칭이 실패하는데 원인이 NAT이 아니다.

```
python natprobe.py punch --label home-A --unsolicited
```

**양쪽이 함께 줘야 한다.** 한쪽만 주면 판정이 `unknown`이 된다.

**동작.** 펀치가 `success`로 끝난 뒤에만 돈다. 새 소켓을 하나 더 열어 상대의 같은 엔드포인트로
0.5초 간격으로 쏜다. 상대가 그것을 본 소켓에서 받으면 확인응답을 돌려준다. 한 번에 두 방향을
잰다.

| 필드 | 뜻 |
|------|-----|
| `unsolicited.recv` | 상대의 요청하지 않은 패킷이 **나에게** 도달한 수 |
| `unsolicited.ack_recv` | 내 요청하지 않은 패킷이 **상대에게** 도달했다는 확인 수 |
| `unsolicited.probe_local_port` | 시험에 쓴 새 소켓의 로컬 포트. 본 소켓과 달라야 의미가 있다 |
| `unsolicited.sync.peer_ready` | 상대의 READY 를 받았는지. **판정의 핵심 증거다.** 탐침이 전부 막혀도 이 경로는 열려 있다 |
| `unsolicited.peer_sent_reported` | 상대가 성공적으로 쏜 탐침 수. 상대가 READY 에 실어 보낸다 |
| `unsolicited.peer_ran_phase` | 상대가 이 단계를 실제로 돌았는지 |
| `unsolicited.inbound_unsolicited` | **내 쪽** 판정 |
| `unsolicited.peer_inbound_unsolicited` | **상대 쪽** 판정 |

판정은 셋 중 하나다.

| 값 | 뜻 | 함의 |
|----|-----|------|
| `allowed` | 통과한다 | 목적지 의존 매핑을 쓰는 상대와도 연결될 수 있다 |
| `blocked` | 막힌다 | 상대가 예상과 다른 포트로 응답하면 실패한다. **클라이언트가 방화벽 규칙을 등록해야 한다** |
| `unknown` | 상대가 이 단계를 돌지 않았거나, **쏜 쪽이 한 발도 못 보냈다** | 판정 불가. 양쪽이 같이 다시 돌린다 |

**`blocked` 는 두 조건이 다 있어야 쓴다.** 상대가 단계에 들어왔고(`sync.peer_ready`), 쏜 쪽이
한 발이라도 성공했어야 한다. 하나라도 없으면 `unknown` 이다. **증거 없는 `blocked` 는 없는
결함을 만들어 낸다.**

**`blocked`는 원인을 분해하지 못한다.** 집 안 NAT, 상위 ISP 필터링, 경로 중간 장비,
호스트 방화벽 중 무엇이든 될 수 있다.

한쪽이 NAT 뒤가 아니면(`socket.local_ip`가 공인 IP와 같으면) **집 안 NAT 변환 하나만
배제된다.** 그것으로 "방화벽 때문"이라고 단정할 수는 없다. ISP 쪽 필터링과 중간 장비가
남는다. 원인을 좁히려면 같은 호스트에서 방화벽을 잠시 끄고 다시 재는 등 조건을 하나씩
바꿔야 한다.

**상대가 안 돌았을 때 `blocked`라고 쓰지 않는다.** 증거 없이 차단으로 기록하면 없는 결함을
만들어 낸다. 그래서 `unknown`을 따로 둔다.

## 5. 사전 조건

**이 세 가지를 지키지 않으면 결과가 무효다.** 나쁜 결과가 나와도 NAT 때문인지 알 수 없다.

### 5.1 Windows 방화벽

첫 실행 때 Windows 방화벽이 허용 프롬프트를 띄운다. **허용해야 한다.** 거부하거나 창을 닫으면
인바운드가 전부 막혀서, 실제 NAT 거동과 무관하게 `punch.result`가 `failure`로 나온다.

- 이 도구는 관리자 권한을 요구하지 않는다. 프롬프트는 사용자 권한으로 처리된다
- 프롬프트가 뜨지 않았는데 결과가 `failure`면, 이전에 거부해 둔 규칙이 남아 있는지 확인한다
- 개인/공용 프로필 체크박스는 현재 쓰는 연결의 프로필에 맞춘다

### 5.2 서로 다른 가정망

`spec.md` 122행이 요구하는 것은 **"서로 다른 가정망 2곳"**이다. "서로 다른 ISP"가 아니다.
ISP가 같아도 별개의 가입 회선이면 필수 시험 토폴로지 (2)를 만족한다.

| 구성 | 유효한가 |
|------|----------|
| 집 A ↔ 집 B, 공인 IP가 다름 | **유효.** ISP가 같아도 된다. 필수 시험 토폴로지 (2)에 해당 |
| 같은 공유기 아래 PC 2대 | **무효.** NAT을 통과하지 않는다. blocker 20의 증거가 되지 않는다 |
| 같은 건물의 다른 세대 | 조건부. 공인 IP가 서로 다른지 확인한다. 같으면 같은 NAT 뒤다 |

**별개의 회선인지 확인하는 법.** 양쪽 `stun.servers[].mapped.ip`가 다르면 별개다. 같으면 같은
NAT 뒤이므로 무효다. RDAP(`https://rdap.arin.net/registry/ip/<IP>`)로 할당 블록까지 보면 더
확실하다.

**ISP가 같으면 증거의 범위가 좁아진다.** 공유기 제조사와 기본 설정, NAT 구현이 같을 가능성이
높다. 결과를 다른 ISP로 일반화하지 말고, 그 한계를 기록에 적는다. 무효는 아니다.

같은 LAN 시험은 `spec.md`의 필수 시험 토폴로지 (1)에 해당하지만, blocker 20이 묻는 질문에는
답하지 못한다. 이 도구의 목적은 (2)다.

### 5.3 Windows에서 막힐 때

| 증상 | 원인 | 대처 |
|------|------|------|
| `ValueError: invalid ioctl command 2550136844` | 2026-09-20 이전 판의 결함. `socket.ioctl()`은 `SIO_UDP_CONNRESET`을 지원하지 않는다 | 최신 판을 받는다. 지금은 `ws2_32.dll`의 `WSAIoctl`을 직접 부른다 |
| `UnicodeEncodeError: 'charmap' codec can't encode` | 콘솔 코드페이지가 cp1252(영문 Windows 기본). 이 도구의 출력은 한국어다 | 최신 판을 받는다. 시작할 때 UTF-8로 맞춘다. 글자가 깨져도 JSON은 항상 정확하다 |
| `python`을 치면 Microsoft Store가 열린다 | Windows의 앱 실행 별칭 | `py natprobe.py ...` 를 쓴다 |
| `SIO_UDP_CONNRESET off: 실패` 가 뜬다 | `WSAIoctl` 호출이 실패했다 | 측정은 계속된다. 이 값은 JSON의 `socket.udp_connreset_disabled`에 남으므로 기록에 적는다. 펀치 결과가 이상하면 이것부터 의심한다 |

`SIO_UDP_CONNRESET`을 꺼야 하는 이유는 `protocol.md` 15장에 있다. 끄지 않으면 ICMP port
unreachable을 받은 뒤 `recvfrom`이 `WSAECONNRESET`로 깨진다. **펀치 초반에는 상대가 아직
포트를 열기 전이라 이 ICMP가 정상적으로 발생한다.** 즉 이 설정이 빠지면 가장 중요한 구간에서
수신이 흔들린다.

### 5.4 경로 오염 제거

VPN, 회사망, 기숙사망, 캠퍼스망이 켜져 있으면 결과가 오염된다. 측정 전에 끈다. 끌 수 없으면
그 사실을 기록에 남기고, 그 결과를 가정망 측정값으로 쓰지 않는다.

- VPN 클라이언트는 종료한다. 가상 어댑터가 남아 있는지도 확인한다
- 모바일 핫스팟은 `spec.md`상 best-effort 토폴로지다. 필수 토폴로지 판정에 쓰지 않는다
- 측정 중 다른 대용량 트래픽(업데이트, 스트리밍)을 돌리지 않는다. RTT와 손실률이 왜곡된다

## 6. 결과 파일

```
<out>/<YYYYmmdd-HHMMSS>-<label>-<probe|punch>.json
```

예: `tools/nat-probe/results/20260916-210433-home-A-punch.json`

시각은 파일명 기준이고, 내용의 `started_utc`가 정본이다. 파일을 지우거나 덮어쓰지 않는다.
실패한 측정도 남긴다. 실패 기록은 `spec.md` NFR-7이 요구하는 증거다.

## 7. 결과 읽는 법

### 7.1 매핑 판정 `stun.mapping`

| 값 | 뜻 | 함의 |
|----|-----|------|
| `endpoint-independent` | 응답한 모든 서버가 같은 공인 엔드포인트를 봤다 | 홀펀칭 가능성이 높다. `spec.md`의 "지원 조건"에 해당 |
| `destination-dependent` | 서버마다 다른 엔드포인트를 봤다. 대칭형 거동 | **blocker 20 위험.** `spec.md`의 "미지원 조건"에 해당 |
| `unknown` | 응답한 서버가 2곳 미만이거나, 서로 다른 IP의 서버가 2곳 미만 | 판정 불가. 측정 실패이지 NAT 거동이 아니다 |

`mapping_confident`는 **서로 다른 IP를 가진 서버 2곳 이상**에서 응답을 받았을 때만 `true`다.
같은 호스트명이 같은 IP로 해석되면 목적지가 하나뿐이므로 목적지 의존성을 볼 수 없다.
`distinct_server_ips`가 그 개수이고, `distinct_mapped_endpoints`가 관측된 서로 다른 공인
엔드포인트 개수다.

`mapping_confident`가 `false`인 결과는 판정에 쓰지 않는다. 서버를 늘려 다시 측정한다.

`port_preserving`은 공인 포트가 로컬 포트와 같은지를 나타낸다. 참고 정보이며 판정 기준은
아니다. 포트를 보존하지 않아도 엔드포인트 독립이면 홀펀칭은 된다.

### 7.2 펀치 판정 `punch.result`

| 값 | 뜻 | 함의 |
|----|-----|------|
| `success` | 상대 패킷을 받았고, 내 패킷에 대한 응답(PONG)도 받았다 | 양방향 성립 |
| `one-way` | 받기만 했거나 보내기만 성립 | 한쪽 필터링 의심. 방화벽도 의심 대상 |
| `failure` | 아무것도 못 받았다 | 원인 미분해. 방화벽, 타이밍, 엔드포인트 오기부터 배제한다 |

`source_matches_expected`가 `false`면 받은 패킷의 출발지가 기대한 상대 엔드포인트와 다르다.
상대 NAT이 다른 포트로 매핑했거나, 측정 중 매핑이 바뀌었을 수 있다. `inbound_sources`에 실제
관측된 출발지가 들어 있으니 그 값을 기록에 남긴다.

### 7.3 조합 판정표

두 망의 `mapping`과 한 번의 `punch.result`를 함께 본다.

| 망 A 매핑 | 망 B 매핑 | punch | 판정 | 다음에 할 일 |
|-----------|-----------|-------|------|--------------|
| EI | EI | `success` | blocker 20 해소 가능 | 3회 반복해 재현을 확인한다. 성공하면 M-1/M-4를 그대로 둘 수 있다 |
| EI | EI | `one-way` | 매핑은 문제없음. 필터링 또는 방화벽 의심 | 방화벽 허용 상태를 먼저 확인하고 재시험. 그래도 같으면 위험으로 분류 |
| EI | EI | `failure` | 원인 미상 | 방화벽, 시작 시각, 엔드포인트 오기를 순서대로 배제한 뒤 재시험 |
| EI | DD | 무관 | **위험.** 한쪽이 대칭형 | 선택지 A/B/C 검토 대상 |
| DD | DD | 무관 | **blocker 20 현실화** | 선택지 A/B/C 중 선택이 필수 |
| 어느 쪽이든 `unknown` | | 무관 | 판정 불가 | 서로 다른 IP의 STUN 서버를 늘려 재측정 |

EI = `endpoint-independent`, DD = `destination-dependent`.

매핑이 `destination-dependent`인데 `punch`가 `success`로 나오는 경우가 있을 수 있다. 실제 왕복이
성립했다는 것은 매핑 추정보다 강한 증거다. 다만 그런 성공은 재현성이 낮을 수 있으므로,
3회 반복 결과가 모두 성공일 때만 성공으로 취급한다. 이유는 모르는 채로 기록에 남긴다.

`plan.md` 5장은 선택한 안마다 "3회 연속 성공"을 재현 조건으로 요구한다. 한 번의 성공은 판정
근거가 아니다.

## 8. 나쁜 결과도 실측 성공이다

`destination-dependent`가 나오거나 `punch`가 `failure`로 끝나도, 그것은 **도구가 제대로 일한
것이다.** 측정 실패가 아니다.

blocker 20이 위험한 이유는 대칭형 NAT 자체가 아니라, 그 사실을 **늦게 아는 것**이다.
`plan.md`는 이렇게 적는다. "학기 중반에 드러나면 되돌릴 시간이 없다."

나쁜 결과는 다음을 뜻한다.

1. `plan.md` 5장의 선택지 A(NAT 에뮬레이션), B(최소 릴레이 P1 승격), C(제3의 경로 추가) 중
   하나를 지금 고르라는 신호다
2. 어느 안을 고르든 **M-1과 M-4를 함께 개정해야 한다.** 현재 최소 성공은 "두 가정망 사이의
   직접 UDP"인데 A는 그 토폴로지가 아니고, B는 직접이 아니며, C는 요구된 두 경로를 통과시키지
   않는다. 안만 고르고 기준을 두면 blocker 20은 그대로 남는다
3. 결정과 근거는 `docs/kor/decisions/`에 ADR로 남긴다

판정 불가(`unknown`, 방화벽 미허용, 같은 공유기 시험)만이 진짜 측정 실패다. 이것은 다시 재야
한다.

## 9. JSON 스키마

`schema` 값은 `"natprobe/1"`이다. 이 값이 다르면 아래 표는 적용되지 않는다.

### 9.1 최상위

| 필드 | 형 | 뜻 |
|------|-----|-----|
| `schema` | string | 스키마 판. `"natprobe/1"` |
| `label` | string | `--label` 값 |
| `mode` | string | `"probe"` 또는 `"punch"` |
| `started_utc` | string | 측정 시작 시각. UTC, ISO 8601 |
| `platform` | object | 실행 환경 |
| `socket` | object | 사용한 로컬 소켓 |
| `stun` | object | STUN 측정 결과 |
| `punch` | object / null | 펀치 시험 결과. `probe` 실행 결과에서는 `null`이다 |

### 9.2 `platform` / `socket`

| 필드 | 형 | 뜻 |
|------|-----|-----|
| `platform.system` | string | 예: `"Windows"` |
| `platform.release` | string | 예: `"11"` |
| `platform.python` | string | 예: `"3.12.1"` |
| `socket.local_ip` | string | 바인드된 로컬 IP |
| `socket.local_port` | int | 바인드된 로컬 포트. `--port 0`이면 임시 포트 |
| `socket.reused_for_punch` | bool | STUN과 펀치가 같은 소켓을 썼는지. `punch`에서 `true`여야 결과가 유효하다 |
| `socket.udp_connreset_disabled` | bool / null | Windows에서 `SIO_UDP_CONNRESET`을 껐는지. Windows가 아니면 `null` |
| `socket.udp_connreset_detail` | string / null | 위가 `false`일 때의 실패 사유. 그 외에는 항상 `null` |

### 9.3 `stun`

| 필드 | 형 | 뜻 |
|------|-----|-----|
| `stun.servers[]` | array | 서버별 결과 |
| `stun.servers[].host` / `.port` | string / int | 질의한 서버 |
| `stun.servers[].resolved_ip` | string | 해석된 IP. 목적지가 실제로 달랐는지 판단하는 근거다 |
| `stun.servers[].ok` | bool | 응답을 받았는지 |
| `stun.servers[].attempts` | int | 시도 횟수 |
| `stun.servers[].rtt_ms` | number | 왕복 시간(ms) |
| `stun.servers[].mapped` | object | 그 서버가 본 공인 엔드포인트 `{ip, port}` |
| `stun.servers[].error` | string / null | 실패 사유. 성공이면 `null` |
| `stun.distinct_server_ips` | int | 서로 다른 서버 IP 개수 |
| `stun.distinct_mapped_endpoints` | int | 관측된 서로 다른 공인 엔드포인트 개수 |
| `stun.mapping` | string | `endpoint-independent` / `destination-dependent` / `unknown` |
| `stun.mapping_confident` | bool | 서로 다른 IP의 서버 2곳 이상에서 응답을 받았을 때만 `true` |
| `stun.port_preserving` | bool / null | 공인 포트가 로컬 포트와 같은지. 응답한 서버가 없으면 `null` |
| `stun.stray_datagrams[]` | array | STUN 단계에서 버린 데이터그램. 다른 출발지, 검증 실패, 수신 오류. 보통 비어 있다 |

### 9.4 `punch`

| 필드 | 형 | 뜻 |
|------|-----|-----|
| `punch.peer_endpoint` | string | 입력한 상대 엔드포인트 `IP:Port` |
| `punch.duration_s` | int | 시도 시간(초) |
| `punch.sent` | int | 보낸 패킷 수. 약 200ms 간격으로 보낸다 |
| `punch.recv_ping` | int | 받은 PING 수. 상대가 보낸 것 |
| `punch.session` | int | 이 실행의 32비트 난수 세션 값. 이전 회차의 지연 패킷을 이번 회차로 오인하지 않기 위한 것 |
| `punch.recv_pong` | int | 받은 PONG 수. **내가 실제로 보낸 PING과 세션·시퀀스·타임스탬프가 모두 맞는 것만** 센다. 같은 시퀀스의 중복 PONG은 한 번만 센다 |
| `punch.rejected_pong` | int | 위 검증에 걸려 버린 PONG 수. 0이 아니면 망 중복이나 위조를 의심한다 |
| `punch.first_inbound_ms` | number / null | 시작부터 첫 인바운드까지 걸린 시간(ms). 아무것도 못 받았으면 `null` |
| `punch.rtt_ms` | object / null | `{min, median, max, samples}`. 표본이 없으면 `null` |
| `punch.loss_pct` | number / null | 손실률(%). **첫 인바운드 이후에 보낸 패킷만** 분모로 쓴다. 상대가 늦게 시작한 구간을 손실로 세지 않기 위해서다. 인바운드가 없었으면 `null` |
| `punch.loss_basis` | object | 손실률 계산 근거. `{sent_after_first_inbound, pong_after_first_inbound}` |
| `punch.inbound_sources` | array | 실제로 관측된 인바운드 출발지 목록 |
| `punch.source_matches_expected` | bool | 출발지가 기대한 상대 엔드포인트와 일치하는지. 인바운드가 없으면 `false` |
| `punch.non_punch_datagrams` | int | 이 도구의 형식이 아닌 수신 데이터그램 수. 0이 아니면 다른 트래픽이 섞인 것이다 |
| `punch.next_phase_datagrams` | int | 펀치 중에 도착한 다음 단계(4.3) 패킷 수. 상대가 나보다 먼저 펀치를 끝냈다는 뜻이며 **정상이다**. 펀치 지표에는 넣지 않는다 |
| `punch.result` | string | `success` / `one-way` / `failure` |

### 9.5 `unsolicited`

`--unsolicited`를 주지 않으면 `null`이다. 펀치가 성립하지 않으면
`{"enabled": true, "skipped": "..."}` 만 들어간다. 필드와 판정값은 4.3에 있다.

## 10. 기록에 반영하는 법

1. 양쪽 결과 JSON을 `tools/nat-probe/results/`에 모은다. 두 망의 파일을 한곳에 모아야 비교가
   된다
2. [`RECORD-TEMPLATE.md`](RECORD-TEMPLATE.md)를 복사해 채운다. 파일명은
   `tools/nat-probe/records/YYYY-MM-DD-nat-실측.md`를 권한다
3. 기록에는 JSON 파일명을 그대로 적는다. 수치를 옮겨 적되 원본을 지우지 않는다
4. 판정이 나오면 `plan.md` 5장의 후속 결정 칸을 채운다. 선택지와 M-1/M-4 개정안은 한 묶음이다
5. 결정은 `docs/kor/decisions/`에 ADR로 옮긴다. 기록 양식의 ADR 메모 칸이 그 초안이다

## 11. 한계

- 이 도구는 NAT 종류를 RFC 3489식 이름으로 분류하지 않는다. 관측된 거동만 보고한다.
  `spec.md`도 "분류 기준은 통신사 구성이 아니라 관측된 거동"이라고 적는다
- `punch` 실패의 원인을 분해하지 못한다. NAT 필터링, Windows 방화벽, 공유기 설정, 시각
  어긋남을 구분하려면 사람이 조건을 하나씩 바꿔가며 다시 재야 한다
- 측정 시점의 결과다. 공유기 재부팅, ISP 구성 변경, 요금제 변경으로 거동이 바뀔 수 있다.
  기록에 측정 일시와 회선 정보를 남기는 이유다
- IPv4 UDP만 다룬다. `protocol.md` 1장의 v1 범위와 같다
- **펀치 패킷 형식에 판이 있다.** 현재는 `NATPRB2`(28바이트)다. 이전 판 `NATPRB1`(24바이트)과
  호환되지 않으며 서로 무시한다. **양쪽이 같은 판의 `natprobe.py`를 써야 한다.** 판이 다르면
  결과가 `failure`로 나오는데 그 원인은 NAT이 아니다. 2026-09-17과 2026-09-18 측정은
  `NATPRB1`로 이루어졌다
