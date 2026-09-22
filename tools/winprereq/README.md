# winprereq

`docs/kor/windows-prereq.md` 의 판정식을 뺀 스크립트다. 문서에 산문으로 두면 아무도 돌려
보지 않고, 반례가 나올 때마다 문서를 고쳐야 한다.

Windows PowerShell 에서 돈다. 관리자 권한은 필요 없다.

## Test-FirewallPolicy.ps1

`windows-prereq.md` 2절. 방화벽 정책과 우리 규칙의 범위를 본다.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\winprereq\Test-FirewallPolicy.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File tools\winprereq\Test-FirewallPolicy.ps1 -InterfaceAlias '<어댑터 이름>'
powershell -NoProfile -ExecutionPolicy Bypass -File tools\winprereq\Test-FirewallPolicy.ps1 -SelfTest
```

보는 것은 셋이다. 세 프로필의 기본 인바운드 정책이 차단인가, `BlockInboundAlways` 가
아닌가, 프로필 방화벽이 켜져 있는가. `-InterfaceAlias` 를 주면 우리 규칙이 그 어댑터와
기대 엔드포인트로 좁혀졌는지까지 본다.

## Test-SubnetOverlap.ps1

`windows-prereq.md` 7절. 우리가 쓸 대역이 기존 라우트·주소와 겹치는지 본다.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\winprereq\Test-SubnetOverlap.ps1 -Target 10.100.0.0/24
powershell -NoProfile -ExecutionPolicy Bypass -File tools\winprereq\Test-SubnetOverlap.ps1 -SelfTest
```

**3절의 잔존물 정리를 먼저 돌린다.** 우리 어댑터가 이전 실행의 주소를 달고 살아 있으면
자기 자신 때문에 차단이 나온다. 우리 어댑터는 `-OwnInterfaceGuid` 로만 뺀다. 이름은 소유의
증거가 아니다.

## 판정 읽는 법

| 스크립트 | 마지막 줄 | 종료 코드 |
|----------|-----------|-----------|
| `Test-FirewallPolicy` | `VERDICT: pass` / `fail` / `unknown` | 0 / 1 / 2 |
| `Test-SubnetOverlap` | `VERDICT: proceed` / `pick another range` / `unknown` | 0 / 1 / 2 |

**`unknown` 은 통과가 아니라 판정 불가다.** 확인된 결함이 하나라도 있으면 판정 불가가 같이
있어도 실패로 낸다. 결함은 이미 사실이고 모르는 항목이 그것을 덮지 못한다.

`-SelfTest` 는 케이스 표 전체를 돌리고 `SELFTEST: pass` 또는 `fail` 을 낸다. 각각 225건과
85건이다. 케이스 표는 스크립트 안에 있고 구현보다 먼저 쓴다.
