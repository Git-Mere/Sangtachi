# Phase 7 구현 (ADR 0029)

## 왜 했나

저장소 소유자가 Phase 7 을 할 수 있는 곳까지 하라고 했다. 검증은 소유자가 나중에 시점을 알려 준다. 착수 전 결정은 ADR 0029
이고 이 커밋은 그것을 구현한다. 두 기기와 관리자 셸이 있어야 하는 확인은 남았다(`plan.md`).

## 무엇을 바꿨나

| 무엇 | 자리 |
|------|------|
| 어댑터에서 읽은 패킷을 8.5 송신 측 검증과 `DATA` 로 잇는다. 시험 빌드는 라우팅 범위 밖의 ICMP 에코 요청에 먼저 답한다 | `main.cpp` |
| 8.4 를 통과한 `DATA` 를 어댑터가 있으면 두 빌드 모두 주입한다. 어댑터가 없으면 전과 같다 | `main.cpp` |
| 8.5 유니캐스트 검사와 `tx_drop_not_unicast` | `tunnel/router.hpp`, `router.cpp`, `tunnel.cpp`, `tunnel.hpp` 주석 |
| 카운터 다섯. 개수 57 -> 62 | `counters.hpp`, `counters.cpp` |
| `sendto` 성공의 바이트와 데이터그램. `recvfrom` 성공의 바이트와 데이터그램(1473바이트 과대분 포함, `WSAEMSGSIZE` 제외, 0바이트 포함) | `loop.cpp`, `loop.hpp`, `network/udp_socket.hpp`, `platform/win32/udp_socket.cpp` |
| 시험 빌드 ICMP 응답기의 목적지 범위 | `testbuild/instrument.cpp`, `instrument.hpp` |
| 방화벽 규칙의 검증과 만들기 스크립트. 케이스 46건 | `tools/winprereq/Initialize-FirewallRules.ps1` (새 파일), `tools/winprereq/README.md` |
| 스크립트를 실행 파일 옆에 두고 기동 단계에서 부른다. 서브넷 검사와 같은 실행 코드를 쓰도록 `run_script` 로 뺐다 | `client/CMakeLists.txt`, `platform/tun.hpp`, `platform/win32/tun.cpp`, `main.cpp` |
| 두 기기의 파일 전송 확인 도구 | `tools/transfer/Test-Transfer.ps1`, `tools/transfer/README.md` (새 파일) |
| 관리자 셸 확인에 방화벽 장면과 경로 없음 장면 | `scripts/adapter-check.ps1` |
| 시험. 8.5 케이스 표의 유니캐스트 행과 검사 순서, ICMP 응답기의 목적지 표, 바이트·데이터그램 카운터, 0바이트 수신, 링이 찬 동안의 주입과 타이머 간격, 카운터 목록 | `tunnel_test.cpp`, `instrument_test.cpp`, `loop_test.cpp`, `adapter_loop_test.cpp`, `counters_test.cpp` |
| 예정 목록에서 새 카운터 다섯을 뺐다 | `tools/docgate/docgate.py` |
| 규칙 이름, `ERROR` 줄, 판정 스크립트, 구현 상태 | `windows-prereq.md` 0·2·14절 |
| 모듈 표의 `platform/tun`, 9장 고정 키에 `adapter.firewall` 과 `adapter.error` | `architecture.md` |
| `route_and_send` 와 주입 표를 현재 상태로 | `concurrency.md` 3장 |
| 파일 전송 도구와 경로 없음 확인의 자리 | `roadmap.md` Phase 7 검증 |
| `AssignAddress` 가 터널의 자신의 가상 IP 도 둔다 | `control/runner.cpp`, `lobby.hpp`·`tunnel.hpp` 주석, `runner_test.cpp` |
| 시험 빌드가 실패하면 마지막 40줄을 보인다 | `scripts/harness-lib.ps1` |
| 남은 일 둘 | `plan.md` |

## 중요한 결정

결정과 버린 대안은 ADR 0029 다. 이 기록에는 구현하면서 정한 것만 적는다.

- **규칙 이름에서 공백을 뺐다.** `Sangtachi-TCP-25565-In`, `Sangtachi-ICMPv4-Echo-In` 이다. 승인받은 안은 `ERROR` 줄에 지우는
  명령을 싣는 것이었는데 `architecture.md` 9장이 로그 값에 공백을 싣지 않는다. 그래서 줄에는 규칙 이름과 검사를 싣고 지우는 명령은
  `windows-prereq.md` 2절에 둔다. 이름에 공백이 있으면 그 이름조차 밑줄로 바뀐다. 저장소 소유자에게 보고한다
- **방화벽 판정에 쓰는 두 이벤트를 9장 고정 키 표에 올렸다.** ADR 0027 은 어댑터 쪽 이벤트를 고정 키로 두지 않았다. 그때는 판정을
  OS 조회와 카운터로 했다. 방화벽 장면은 `adapter.firewall` 과 `adapter.error reason=firewall` 줄로 판정하므로 9장의 "검증이 찾는
  이벤트에는 고정 키를 준다" 에 든다. 리뷰 지적이다
- **스크립트 실행 코드를 서브넷 검사와 나눠 쓴다.** `run_subnet_check` 의 프로세스 실행 부분을 `run_script` 로 뺐다. 상속 핸들
  제한, `PSModulePath` 제거, 임시 파일 출력, 상한 뒤 강제 종료가 그대로다. 서브넷 쪽 판정 코드는 바뀌지 않았다
- **ICMP 규칙의 로컬 포트는 `Any` 와 `RPC` 를 둘 다 받는다.** 처음에는 `Any` 만 기대했다. 저장소 소유자가 관리자 셸에서
  `adapter-check.ps1` 을 돌리자 클라이언트가 `failed=Sangtachi-ICMPv4-Echo-In:port` 로 기동에 실패했다. 우리가 포트 없이 만든 ICMP
  규칙을 `Get-NetFirewallPortFilter` 가 `LocalPort=RPC`, `IcmpType=8` 로 돌려줬다. 내장 ICMP 규칙(`FPS-ICMP4-ERQ-In`)도 같다. ICMP 에는
  포트가 없으므로 이 칸으로 넓어질 것이 없다. `IcmpType` 은 `8` 과 레지스트리 원문의 `8:*` 를 둘 다 받는다
- **터널은 자신의 가상 IP 를 방에 든 때부터 안다.** 전에는 첫 세션을 만들 때 알았다. 관리자 셸의 `adapter-check.ps1` 에서
  호스트 혼자 `ping 10.100.0.3` 을 보내자 `tx_drop_no_route` 가 오르지 않았다. 로그를 보니 그때까지 어댑터에서 읽은 패킷이 모두
  `tx_drop_bad_src` 였다(93). 출발지를 대조할 자신의 가상 IP 가 없어서다. Phase 6 까지는 어댑터의 패킷이 송신 측 검증에 오지
  않아 드러나지 않았다. `AssignAddress` 가 터널에도 그 값을 넣는다. 실행기 시험을 더했고 그 줄을 빼면 떨어진다
- **주입 실패 시험의 타이머 허용 폭은 50ms 다.** 100ms 주기에 타이머 시계 해상도(약 15ms)를 넘는 여유를 둔다
- **파일 전송 도구는 받는 쪽이 해시를 돌려주고 보내는 쪽이 판정한다.** 사람이 두 해시를 눈으로 비교하지 않는다

## 검증

- `pwsh -NoProfile -File scripts/test.ps1` 통과. ctest 627건과 cli, e2e, lobby, session 확인
- `python tools/docgate/docgate.py`, `python tools/platformgate/platformgate.py` 통과
- `Initialize-FirewallRules.ps1 -SelfTest` 46건 통과. 관리자가 아닌 셸에서 판정 모드는 규칙을 만들지 못해 `FAIL <이름> create`
  두 줄과 `VERDICT: fail`, 종료 코드 1 이었다
- 변이 열둘이 모두 잡혔다
  - C++: 유니캐스트 검사 빼기, 응답기의 범위 검사 빼기, 과대분 수신 세기 빼기, 송신 바이트를 틀리게 세기, 링이 찼을 때 5ms 동안
    재시도하기
  - 방화벽 스크립트: 그룹 검사 빼기, 인터페이스를 포함으로 비교하기, 원격 포트 검사 빼기, 정책 값 줄 수 검사 빼기, 판정 불가
    빼기, 결함이 판정 불가를 이기는 규칙 빼기
  - 파일 전송 도구: 받는 쪽이 틀린 해시를 돌려주면 `VERDICT: fail`
- 파일 전송 도구를 루프백(127.0.0.1:25565)에서 8 MiB 로 돌려 `VERDICT: pass` 였다
- 저장소 소유자가 관리자 셸에서 `scripts/adapter-check.ps1` 을 네 번 돌렸다
  - 첫째는 시험 빌드의 링크 단계에서 멈췄다. 빌드 출력을 버리고 있어 원인이 남지 않았다. 바로 다시 지으니 지나갔다. 실패하면
    마지막 40줄을 보이게 고쳤다
  - 둘째는 네 장면이 모두 `failed=Sangtachi-ICMPv4-Echo-In:port` 로 기동에 실패했다. ICMP 규칙의 `RPC` 표기 때문이다(위 결정)
  - 셋째는 59개 가운데 58개가 통과했다. 방화벽 규칙의 만들기, 다시 쓰기, 끈 규칙의 기동 실패와 되돌리기, `Test-FirewallPolicy`
    판정이 통과했다. 떨어진 하나는 route 장면의 `tx_drop_no_route` 로, 자기 가상 IP 결함이다(위 결정)
  - 넷째는 자기 가상 IP 를 고친 빌드로 돌렸고 전부 통과했다(`0 checks failed`). route 장면의 `tx_drop_no_route` 가 0 에서 2 로
    올랐고, 그때까지 `tx_drop_not_unicast` 가 63 이었다. 운영체제가 어댑터에 보낸 멀티캐스트와 브로드캐스트다
- **아직 보지 않은 것.** 두 기기 검증. 방화벽 규칙이 실제로 상대의 트래픽을 통과시키는지도 그때 본다

## 크로스 모델 리뷰

Codex 로 staged diff 를 두 번 보였다. 한 번은 C++ 과 PowerShell 의 정확성과 실패할 수 없는 시험, 한 번은 코드와 문서의 계약
일치를 보게 했다. 지적은 일곱이고 모두 `warn` 이다. 두 리뷰가 같이 낸 하나를 합쳐 여섯이다. 모두 반영했다.

| # | 지적 | 처리 |
|---|------|------|
| 1 | 0바이트 데이터그램이 `rx_datagrams` 에 들지 않는다 (두 리뷰 모두) | 반영. 성공한 수신은 길이와 무관하게 센다. `WSAEMSGSIZE` 만 뺀다. 시험을 더했다 |
| 2 | `tunnel.hpp` 와 `loop.hpp` 주석이 아직 제품 빌드는 훅을 비운다, Phase 7 작업이다로 적는다 | 반영 |
| 3 | `plan.md` 가 이 커밋이 한 구현을 아직 할 일로 적는다 | 반영. 남은 일을 두 기기가 있어야 하는 확인 둘로 바꿨다 |
| 4 | `adapter-check.ps1` 이 읽는 `adapter.firewall` 과 `adapter.error` 필드가 9장 고정 키 표에 없다 (미러 포함) | 반영. 두 행을 더했다 |
| 5 | 스크립트의 `UNKNOWN` 줄이 클라이언트 로그에 남지 않아 조회 실패로 기동이 막히면 무엇이 실패했는지 모른다 | 반영. `ERROR` 줄에 `unknown` 필드를 더했다 |
| 6 | `adapter-check.ps1` 의 `created=-` 정규식이 로그 전체의 끝에 닻을 내려 정상 실행도 떨어진다. 규칙을 다시 켜는 실패를 숨긴다 | 반영. 여러 줄 모드로 고치고, 다시 켜기를 `Stop` 으로 부른 뒤 켜졌는지 확인한다 |

리뷰 반영분만 Codex 로 한 번 더 보였다. 지적 둘, 모두 `warn` 이다. 둘 다 반영했다.

| # | 지적 | 처리 |
|---|------|------|
| 7 | `plan.md` 가 ICMP 로컬 포트 표기를 아직 모른다고 적는다 | 반영. 넷째 실행이 통과해 그 행을 지웠다 |
| 8 | 기록이 `adapter-check.ps1` 을 돌리지 않았다고 적는데 소유자의 실행이 두 결함을 드러냈다고도 적는다 (미러 포함) | 반영. 실행마다 결과를 적었다 |

전체 staged diff 의 마지막 확인에서 지적 둘이 더 나왔다. 둘 다 반영했다.

| # | 지적 | 처리 |
|---|------|------|
| 9 | `warn`. `plan.md` 의 축 표가 아직 Phase 6 까지 끝났다고 적는다 | 반영. Phase 7 구현과 한 기기 확인이 끝났고 두 기기 검증이 남았다로 고쳤다 |
| 10 | `nit`. 기록이 남은 일을 셋으로 적는데 `plan.md` 는 둘이다 (미러 포함) | 반영 |
