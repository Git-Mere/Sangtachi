# Phase 6 가상 어댑터 (ADR 0027)

## 왜 했나

`plan.md` 다음에 할 일이 Phase 6 이었다. 저장소 소유자가 Phase 6 을 끝까지 하라고 했다. 착수 전 셋은 직전 커밋
(ADR 0026)이 닫았고, 구현하면서 문서가 정하지 않은 자리를 ADR 0027 로 정했다.

## 무엇을 바꿨나

| 무엇 | 자리 |
|------|------|
| 빌드가 Wintun zip 을 받아 zip 해시, DLL 해시, 서명 `Status` 와 `Subject` 를 판정한다. 실행 시 확인의 기대값 헤더를 만들고, `wintun.dll` 과 서브넷 충돌 스크립트를 실행 파일 옆에 둔다 | `cmake/Wintun.cmake`, `client/CMakeLists.txt` |
| 어댑터 상수(이름, 터널 형식, GUID, 대역)와 루프가 보는 면 `adapter::Port` | `client/include/sangtachi/adapter.hpp` |
| Wintun 과 IP Helper. 관리자 확인, 서브넷 충돌 스크립트 실행, DLL 해시 확인과 적재, 어댑터 생성, MTU 와 DAD, 남은 주소 정리, 세션, 읽기와 주입, 주소와 경로 | `client/include/sangtachi/platform/tun.hpp`, `client/src/platform/win32/tun.cpp` |
| 대기 집합의 순위 2, `drain_wintun`, `inject_packet`, 예산과 `busy` | `loop.hpp`, `loop.cpp` |
| 카운터 `rx_err_adapter`. 개수 56 -> 57 | `counters.hpp`, `counters.cpp`, `counters_test.cpp` |
| 행동 `AssignAddress`. `LeaveRoom` 이 주소를 뗀다. 후보 수집에 자기 Wintun GUID 를 넘긴다 | `lobby.hpp`, `lobby.cpp`, `runner.hpp`, `runner.cpp` |
| 기동 인자 `--no-adapter` | `args.hpp`, `args.cpp`, `args_test.cpp` |
| 시험 빌드의 ICMP 에코 응답기 | `testbuild/instrument.hpp`, `instrument.cpp`, `instrument_test.cpp` |
| 기동 단계, 어댑터 수명, 종료 순서의 (5)(6) | `main.cpp` |
| 어댑터 MTU 상수 1400 (`protocol.md` 12장) | `protocol_constants.hpp` |
| 가짜 어댑터로 대기, 비우기, 예산, EOF, 링 손상, 그 밖의 오류, 주입 카운터 | `tests/adapter_loop_test.cpp` (새 파일) |
| 같은 기기 시험이 `--no-adapter` 를 붙인다. cli-check 에 관리자가 아닐 때의 기동 실패 확인 | `cli-check.ps1`, `e2e-check.ps1`, `harness-lib.ps1` |
| 관리자 셸에서 도는 Phase 6 검증 | `scripts/adapter-check.ps1` (새 파일) |
| `--no-adapter` 입력, 모듈 표, 7장 어댑터 수명, 10장 목표 구조 | `architecture.md` |
| 순위 2 의 유효 구간, `rx_err_adapter`, Phase 7 로 미룬 읽기 경로와 주입, 상태 소유 | `concurrency.md` |
| 1·2·3·4·7절의 검증 상태와 기록 상수, 0절의 수 | `windows-prereq.md` |
| Phase 6 검증 스크립트, Phase 7 방화벽 작업 | `roadmap.md` |
| 결정과 버린 대안 | ADR 0027 |
| 다음 할 일을 Phase 7 로. `--peer`/`raw` 결정 대기. 자기 Wintun 식별자 자리를 지움 | `plan.md` |

## 중요한 결정

결정과 버린 대안은 ADR 0027 이다. 이 기록에는 그 밖의 판단만 적는다.

- **터널 형식을 `Wintun` 으로 바꿨다.** 첫 관리자 확인 뒤 서브넷 충돌 스크립트의 자기 어댑터 제외를 확인에 넣었더니 실패했다.
  어댑터 설명이 `Sangtachi Tunnel` 이라 스크립트가 `not-wintun` 으로 아무것도 빼지 않았다. 스크립트를 넓히지 않고 터널 형식을
  바꿨다. 지금 클라이언트는 어댑터를 만들기 전에 검사하므로 이 결함은 기동에서 드러나지 않는다. 같은 기기에서 두 번째 기동이
  어댑터를 든 프로세스와 겹칠 때만 차이가 난다
- **`--peer` 와 `raw` 를 지우지 않았다.** `architecture.md` 3.5 와 `concurrency.md` 1장은 Phase 6 에서 사라진다고 적었지만
  `roadmap.md` 에 작업이 없고 Phase 1 검증 스크립트가 그 둘로 돈다. `plan.md` 에 소유자 결정 대기로 걸었다
- 어댑터 쪽 이벤트(`adapter.open`, `adapter.address`, `adapter.error`, `adapter.close`)는 9장의 고정 키가 아니다. 검증
  스크립트가 읽지만 고정 키 표를 늘리지 않았다. 판정은 OS 조회와 카운터가 한다
- 종료의 주소 정리 실패가 `WARN` 한 줄을 내도록 `TunDevice::close` 가 그 실패를 돌려주고 `main.cpp` 가 줄을 낸다.
  `architecture.md` 7장 표의 그 행을 지키려는 것이다

## 확인한 사실

- Wintun 0.14.1 은 요청한 GUID 를 그대로 인터페이스 GUID 로 쓴다. `adapter.open` 의 GUID 와 `Get-NetAdapter` 의
  `InterfaceGuid` 가 상수와 같았다
- 같은 GUID 로 두 번째 프로세스가 만들면 `WintunCreateAdapter` 가 실패한다
- 프로세스를 강제로 끝내면 그 GUID 의 어댑터가 남지 않는다. 다시 띄우면 어댑터와 주소가 다시 선다
- 새 어댑터의 네트워크 범주는 `Public` 이다
- `/24` 주소를 붙이면 on-link 경로가 생겨 따로 만들지 않았다(`adapter.address route=on_link`)
- Wintun 은 어댑터 설명을 `<터널 형식> Tunnel` 로 쓴다
- 어댑터가 지워지는 동안 `Get-NetAdapter` 가 "삭제 표시된 레지스트리 키" 오류를 낸 적이 있다. 검증 스크립트는 그 바퀴를
  다시 묻는다
- 어댑터가 서자마자 운영체제가 보내는 패킷이 있어 `adapter_rx` 는 ping 전에 이미 0 이 아니었다. 검증은 증가량으로 본다
- Windows PowerShell 5.1 은 pwsh 의 `PSModulePath` 를 물려받으면 모듈을 불러오지 못한다. 빌드, 클라이언트, 검증 스크립트가
  모두 그 값을 지우고 부른다

## 검증

- `pwsh -NoProfile -File scripts/test.ps1` 통과. ctest 627건과 cli, e2e, lobby, session 확인. cli-check 에 관리자가 아닌
  기동 실패 확인이 늘었다
- `scripts/adapter-check.ps1` 를 관리자 셸에서 네 번 돌렸다. 첫 번째는 전부 통과했다. 서브넷 제외 확인을 더한 두 번째는
  그 확인 하나가 실패했고(위 터널 형식), 고친 세 번째는 전부 통과했다. 리뷰를 반영한 뒤의 네 번째도 43개 확인이 전부 통과했다
- 변이 여덟 개가 모두 잡혔다. 대기 집합에서 순위 2 빼기, `busy` 에서 어댑터 빼기, 링 자리 돌려주기 빼기, 링 가득 참을
  `drop_inject_error` 로 세기, EOF 에서 종료 빼기, ICMP 체크섬 다시 채우기 빼기, 조각 검사 좁히기, ICMP 체크섬 검사 빼기
- `cmake/Wintun.cmake` 의 zip 해시, DLL 해시, `Subject` 를 하나씩 바꾸면 설정이 멈췄다. 되돌리면 빌드가 지나갔다
- `python tools/platformgate/platformgate.py` 와 `python tools/docgate/docgate.py` 통과

## 크로스 모델 리뷰

Codex 로 staged diff 를 두 번 보였다. 한 번은 Win32 와 Wintun API 사용, 루프의 비우기와 예산, 종료 순서, 문서와 코드의
불일치를 보게 했다. 한 번은 결함이 있다고 가정하고 낡은 문장, 미러 불일치, PowerShell 과 CMake 결함, 실패할 수 없는 시험을
보게 했다. 지적은 열셋이고 모두 `warn` 이었다. 두 리뷰가 같이 낸 셋을 합쳐 열이다. 모두 반영했다.

| # | 지적 | 처리 |
|---|------|------|
| 1 | `clear_address` 가 지우지 못해도 주소와 경로를 잊는다. 다음 호출이 다시 지울 수 없다 (두 리뷰 모두) | 반영. 지웠거나 `ERROR_NOT_FOUND` 일 때만 잊는다 |
| 2 | 종료가 주소를 세션보다 먼저 지워 `concurrency.md` 7장의 (5)(6) 순서와 반대다 | 반영. `close` 가 세션, 주소와 경로, 어댑터 순서로 돌고 실패를 돌려준다. `main` 은 그것으로 `WARN` 을 낸다 |
| 3 | CMake 가 DLL 경로를 PowerShell 작은따옴표 문자열에 그대로 넣는다 (두 리뷰 모두) | 반영. 작은따옴표를 둘로 적는다 |
| 4 | 문서의 기동 순서(해시 다음 서브넷)와 코드(서브넷 다음 해시)가 다르다. "3절 정리를 먼저" 와 어댑터 생성 뒤 정리도 어긋나 보인다 (두 리뷰 모두) | 반영. 문서를 코드 순서로 고치고, 서브넷 검사 때 우리 어댑터가 없다는 근거를 적었다 |
| 5 | 강제 종료 뒤 어댑터가 남지 않는다는 문장이 보장처럼 읽히는데 검증은 `Not Present` 를 받아 준다 | 반영. 관측으로 고쳐 적고 검증을 "그 GUID 의 어댑터가 없다" 로 좁혔다 |
| 6 | 서브넷 스크립트를 링크 뒤에 복사해 스크립트만 고친 빌드가 낡은 판정을 남긴다 | 반영. `configure_file` 로 바꿨다 |
| 7 | `concurrency.md` 3장이 콘솔 핸들과 스레드가 Phase 6 에서 사라진다고 적었다 | 반영. 콘솔 빌드에는 남는다로 고쳤다. 이 커밋 전부터 있던 문장이다 |
| 8 | `--peer`, `raw` 가 Phase 6 에서 사라진다는 문장이 `plan.md` 의 결정 대기와 맞지 않는다 | 반영. `architecture.md` 3.5, `concurrency.md` 1장, `loop.cpp` 주석을 "정하지 않았다" 로 고쳤다 |
| 9 | `windows-prereq.md` 머리가 아직 어댑터가 없고 Phase 1 구현 전이라고 적는다 | 반영. 0절의 두 단락과 14절 끝 문장을 고쳤다 |
| 10 | `roadmap.md` 가 `adapter-check.ps1` 이 위 항목 전부를 돈다고 적지만 설정 거부 항목은 돌지 않는다 | 반영. 첫 항목은 설정을 다시 돌려 보는 것으로 나눠 적었다 |

리뷰 반영분만 Codex 로 한 번 더 보였다. 지적 둘, 모두 `warn` 이다.

| # | 지적 | 처리 |
|---|------|------|
| 11 | `configure_file` 은 `client/` 에 복사하는데 Visual Studio 와 Ninja Multi-Config 는 실행 파일을 `client/<config>/` 에 둔다 | 기각. 이 저장소의 빌드는 `scripts/build.ps1` 의 Ninja 단일 구성뿐이고 `CMakeLists.txt` 주석에 그 전제를 적었다. 다른 생성기를 위한 장치를 더하지 않았다 |
| 12 | `windows-prereq.md` 14절의 "2번만 아직 구현되지 않았다" 가 4번의 Qt 동봉을 빠뜨렸다 (미러 포함) | 반영. 14절 끝과 0절 단락에 Qt 동봉이 아직이라고 적었다 |
