<#
.SYNOPSIS
    방화벽 정책 판정. `docs/kor/windows-prereq.md` 2절의 판정식을 스크립트로 뺀 것이다.

.DESCRIPTION
    문서 본문에 산문으로 있던 판정식을 케이스 표를 가진 코드로 옮긴다
    (design-audit 6장 규칙 7: 판정을 내는 함수는 검증을 먼저 쓴다).

    판정하는 것은 둘이다.

    | 검사 | 통과 조건 |
    |------|-----------|
    | `policy` | `netsh advfirewall show allprofiles firewallpolicy` 의 정책 값 줄 3개가 전부 `BlockInbound,AllowOutbound` 이고 `BlockInboundAlways` 가 0 |
    | `profile` | 세 프로필(Domain/Private/Public)의 `Enabled` 가 전부 `True` |

    `-InterfaceAlias` 를 주면 우리 어댑터에 대한 검사를 더 한다. 어댑터 분류(참고),
    우리 규칙의 범위, 우리 규칙의 프로그램 경로다.

    마지막 줄은 항상 `VERDICT: pass`, `VERDICT: fail`, `VERDICT: unknown` 중 하나다.
    종료 코드는 각각 0, 1, 2 다. **판정할 수 없는 것을 통과로 적지 않는다.**
    확인된 결함이 하나라도 있으면 판정 불가 항목이 같이 있어도 `fail` 이다. 결함은
    이미 사실이고, 모르는 항목이 그것을 덮지 못한다.

    -SelfTest 는 케이스 표 전체를 돌리고 마지막 줄에 `SELFTEST: pass` 또는
    `SELFTEST: fail` 을 낸다. 실패한 케이스는 기대값과 실제값을 같이 찍는다.

    사용:
        powershell -ExecutionPolicy Bypass -File tools\winprereq\Test-FirewallPolicy.ps1 -SelfTest
        powershell -ExecutionPolicy Bypass -File tools\winprereq\Test-FirewallPolicy.ps1
        powershell -ExecutionPolicy Bypass -File tools\winprereq\Test-FirewallPolicy.ps1 -InterfaceAlias 'sangtachi0'

.NOTES
    케이스 표는 `docs/kor/commit_history/2026-09-21-windows-prereq.md` 의 라운드 표에서
    옮긴 반례가 뼈대다. 새로 상상한 것이 아니라 이미 확보된 반례다.

    | 라운드 | 반례 | 케이스 |
    |:--:|------|--------|
    | 8 | `netsh` 레이블이 표시 언어에 따라 번역된다 | `policy-label-korean`, `policy-label-mojibake`, `verdict-label-korean` |
    | 9 | `Select-String 'BlockInbound'` 가 `BlockInboundAlways` 도 통과시킨다 | `policy-always-value`, `always-detect`, `naive-substring-counterexample`, `verdict-always` |
    | 13 | 정책이 `BlockInbound` 라도 프로필 방화벽이 꺼져 있으면 전제가 무너진다 | `profiles-public-disabled`, `verdict-profile-disabled` |
    | 13 | `Set-NetConnectionProfile` 은 식별되지 않은 네트워크에서 실패할 수 있다 | `scope-profile-private-only` |
    | 14 | 정책 값 검사가 부분 문자열이다 | `policy-affix`, `policy-glued-prefix` |
    | 16 | `Where-Object Enabled -eq $false` 단축 구문은 깨지기 쉽다 | `profiles-enabled-notconfigured`, `naive-enabled-counterexample` |
    | 19 | `Test-Path $_.Program` 이 `%ProgramFiles%` 를 못 푼다 | `program-expanded-exists`, `naive-program-path-counterexample` |
    | 23 | 정책 값 정규식의 앞 `\s` 가 공백 형태가 바뀌면 너무 엄격하다 | `policy-tab-separated`, `policy-line-start` |
    | 24 | `-Profile Any` 만 권하면 물리 네트워크에도 열린다 | `scope-any-interface`, `scope-any-protocol`, `scope-any-port` |
    | 31 | `New-NetFirewallRule -InterfaceAlias` 가 없을 수 있다 | `param-present`, `param-absent`, `param-unknown` |

    2라운드 크로스 모델 리뷰(Codex)에서 규칙 범위 판정에 구멍 셋이 나왔다. 전부 반영했다.

    | 심각도 | 반례 | 케이스 |
    |--------|------|--------|
    | blocker | `Any` 가 아니기만 하면 통과해서 **엉뚱한 어댑터에 묶인 규칙도 통과**했다 | `scope-wrong-adapter`, `scope-alias-extra`, `scope-alias-case-insensitive`, `scope-alias-spacing` |
    | warn | 프로필을 `Domain,Private,Public` 전부 나열한 규칙은 사실상 `Any` 인데 분류 의존으로 읽어 **정상 기기를 떨어뜨렸다** | `scope-profile-full-set`, `scope-profile-full-set-unordered`, `scope-profile-two-of-three` |
    | warn | 포트가 문자 그대로 `Any` 일 때만 걸러 `1-65535` 와 콤마 목록이 "좁혀졌다" 로 통과했다 | `scope-port-all-range`, `scope-port-comma-superset`, `scope-port-equal-range-form`, `port-*` 9건 |

    3라운드에서 blocker 가 하나 더 나왔다. **ICMP 범위를 종류로 판정하지 않았다.**
    어댑터로 좁혀진 ICMPv4 규칙이 모든 종류를 허용해도 `scoped` 로 나왔다. 문서 2절이 쓰는
    규칙은 에코 요청(type 8) 전용이다. 규칙 추출, 기대 표기(`ICMPv4:8`), 판정에 `IcmpType` 을
    넣었다. 케이스는 `scope-icmp-*` 13건과 `icmp-set-*`/`icmp-covers-*` 12건이다.
    기본 기대값도 `TCP:25565`, `ICMPv4:8` 로 바꿨다.

    4라운드 blocker 는 **판정에 넣지 않은 값**이었다. `New-NetFirewallRule -InterfaceAlias`
    지원 여부를 주의 문구로만 찍어서, **어댑터로 좁힌 규칙을 만들 수 없는 기기가 그대로
    `VERDICT: pass` 로 끝났다.** `scope-param` 검사로 올려 `True` 가 아니면 판정을 바꾼다.
    `False` 는 fail, 그 밖은 unknown 이다. 케이스는 `verdict-scope-param-*` 9건이다.
    **헬퍼 반환값만 보는 케이스로는 부족했다.** 값이 최종 판정까지 가는지를 보는 케이스가
    없어서 리뷰어가 잡았다.

    5라운드 blocker 2건은 **조회 계약**이었다. 라이브 조회가 `-ErrorAction Stop` 없이
    `2>$null` 로 오류를 지워서, 권한·제공자 실패가 "어댑터 없음" 과 "규칙 0건" 으로 보고될 수
    있었다. 문서 0장 계약이 **"조회 실패와 없음을 구분한다"** 이다. 조회에 `-ErrorAction Stop`
    을 직접 붙이고 안전 조회 안에서 리디렉션하지 않는다. 검사 조립을 `Get-AdapterCheck` 와
    `Get-RuleChecks` 로 떼어 케이스로 시험한다 (`adapter-check-*`, `rule-checks-*` 14건).
    **없음과 조회 실패는 둘 다 unknown 이지만 이유 문구가 다르다.**

    6라운드 blocker 도 둘이다.

    **1. 조회 실패를 값으로 바꾸는 자리가 하나 남아 있었다.** 응용프로그램 필터 조회가
    실패하면 빈 프로그램 값이 되고 `Get-RuleProgramState ''` 가 `any` 를 내서 **경로를 못 읽은
    규칙이 통과했다.** 규칙별 필터 판정을 `Get-RuleFilterStates` 로 떼어 케이스로 고정했다.
    `Failed` 를 내는 조회 8곳을 전수로 훑어 전부 `Get-QueryItemsOrNull` 을 지나게 했다.
    **실패는 빈 목록이 아니라 `$null` 이고, `$null` 은 판정에서 unknown 이다.**

    **2. 기대 엔드포인트를 "하나만 맞으면" 으로 판정했다.** 문서 2절은 TCP 25565 와 ICMP
    에코를 둘 다 요구한다. `endpoint-coverage` 검사를 새로 만들어 **전부 요구**로 바꿨다.
    규칙이 0건인 Phase 6 이전은 `unknown` 이다. 결함이 아니라 아직 안 만든 것이다.
    `Get-EndpointCoverage` 는 기대 하나만 담은 집합으로 같은 판정 함수를 다시 돌린다.
    판정 규칙을 두 벌 쓰지 않는다.

    7라운드 blocker 는 **판정 대상을 고르는 자리**였다. 라이브 조회가 `Enabled` 를 보지 않아
    **꺼진 인바운드 허용 규칙이 덮은 것으로 세어졌다.** 이 기기에 꺼진 규칙이 240개 있다.
    `Get-RuleSelection` 으로 떼어내고 `Direction`, `Action` 도 같이 본다. 값의 형은 `[bool]` 이
    아니라 열거형이라 이름 문자열로 비교한다. **뺀 규칙은 버리지 않고 사유와 함께 남긴다.**
    버리면 "우리 규칙이 꺼져 있다" 와 "우리 규칙이 아직 없다" 가 같은 값이 된다. 앞은 fail,
    뒤는 unknown 이다. 케이스는 `selection-*` 12건과 `rule-checks-*` 7건이다.

    8라운드 warn 도 규칙 3 자리였다. `Get-ProfileStateVerdict` 가 프로필이
    `Domain,Private,Public` **순서로** 올 것을 요구해서, 조회 순서가 다른 정상 기기가
    `unknown` 이 됐다. 이름을 정렬해 집합으로 비교한다. 켜짐 검사는 그대로다.
    출력 문장도 이름순으로 고정해 조회 순서를 타지 않게 했다 (`profiles-reordered*` 등 7건).

    그래서 `Get-RuleScopeVerdict` 는 **기대 집합**(`-InterfaceAlias` 와 `-ExpectedEndpoint`)과
    대조한다. 무엇과 대조했는지는 판정 출력의 기대 열에 적힌다. 기대 집합이 없으면 판정하지
    않고 `unknown` 이다.

    `Get-NetFirewallProfile` 의 `NotConfigured` 실측도 케이스로 고정했다.
    `DefaultInboundAction` 이 `NotConfigured` 인 것은 "설정 안 됨" 이 아니라 "내장
    기본값(Block)" 이다. `-eq 'Block'` 으로 판정하면 **정상 기기가 떨어진다**
    (`inbound-notconfigured`, `verdict-ignores-inbound-action`). 그래서 정책 판정은
    `netsh` 로 하고 cmdlet 값은 참고로만 찍는다.

    라운드 표에 없는 케이스도 있다. 이 기기에서 돌리다 만난 것이다.
    `query-empty-result` 는 함수가 돌린 빈 배열이 풀려 `$null` 이 되면서 **결과 0건과
    조회 실패가 같은 값**이 되던 것을 잡았다. `program-system-token` 은 Windows 기본
    규칙의 `System` 토큰을 죽은 경로로 세던 것을 잡았다. 둘 다 케이스 표를 먼저 돌려서
    나왔다.

    `Enabled` 도 같은 함정이 있다. 이 기기에서 그 값의 형은 `[bool]` 이 아니라
    `GpoBoolean` 이다. `-is [bool]` 로 입력을 거르면 정상 기기가 떨어진다
    (`profiles-nonbool-enabled`). 그래서 이름 문자열로 비교한다.
#>

[CmdletBinding()]
param(
    # 케이스 표 전체를 돌린다. 판정 실행은 하지 않는다.
    [switch]$SelfTest,

    # 우리 어댑터 이름. 주면 어댑터 분류와 우리 규칙까지 본다. 없으면 그 검사는 건너뛴다.
    [string]$InterfaceAlias,

    # 우리 규칙을 고르는 이름 패턴. `-InterfaceAlias` 와 함께 쓴다.
    [string]$RuleNamePattern = 'sangtachi*',

    # 우리가 열려는 엔드포인트. `프로토콜:포트` 또는 ICMP 면 `프로토콜:종류` 다.
    # 기본값은 문서 2절이 쓰는 `-Protocol ICMPv4 -IcmpType 8`(에코 요청)에 맞춘다.
    # 규칙 범위 판정은 이 집합과 대조한다. 넓으면 wide, 다르면 other-* 다.
    [string[]]$ExpectedEndpoint = @('TCP:25565', 'ICMPv4:8')
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

# 판정 값. 세 개뿐이다. 모르면 pass 로 적지 않는다.
$script:PASS = 'pass'
$script:FAIL = 'fail'
$script:UNKNOWN = 'unknown'


# ---------------------------------------------------------------------------
# 케이스 표. 구현보다 먼저 쓴다 (design-audit 6장 규칙 7).
#
# 케이스마다 정상 입력이 통과하는지(규칙 3)와 결함을 잡는지를 쌍으로 넣는다.
# `Run` 은 문자열을 내고 `Expect` 와 문자열로 비교한다. 비교를 한 가지로 두면
# 실패 출력에 기대값과 실제값을 그대로 찍을 수 있다.
# `Why` 는 그 케이스가 어디서 왔는지다. `R<번호>` 는 리뷰 라운드 번호다.
# ---------------------------------------------------------------------------

$script:Cases = @(

    # --- 정책 값 줄 매칭 -----------------------------------------------------
    @{ Name = 'policy-normal'; Why = '규칙 3. 이 기기의 실제 netsh 줄'; Expect = 'True'
       Run = { [string](Test-PolicyValueLine 'Firewall Policy                       BlockInbound,AllowOutbound') } }

    @{ Name = 'policy-label-korean'; Why = 'R8. 레이블이 번역돼도 값은 같다'; Expect = 'True'
       Run = { [string](Test-PolicyValueLine '방화벽 정책                           BlockInbound,AllowOutbound') } }

    @{ Name = 'policy-label-mojibake'; Why = 'R8. cp949 출력을 잘못 디코딩해도 값은 ASCII 다'; Expect = 'True'
       Run = { [string](Test-PolicyValueLine 'ë°©í™”ë²½ ì •ì±…   BlockInbound,AllowOutbound') } }

    @{ Name = 'policy-always-value'; Why = 'R9. 더 엄격한 모드를 정상으로 읽으면 안 된다'; Expect = 'False'
       Run = { [string](Test-PolicyValueLine 'Firewall Policy   BlockInboundAlways,AllowOutbound') } }

    @{ Name = 'policy-affix'; Why = 'R14. 값 앞뒤에 글자가 붙은 줄'; Expect = 'False'
       Run = { [string](Test-PolicyValueLine 'Firewall Policy   XBlockInbound,AllowOutboundY') } }

    @{ Name = 'policy-glued-prefix'; Why = 'R14. 앞에만 글자가 붙은 줄'; Expect = 'False'
       Run = { [string](Test-PolicyValueLine 'Firewall Policy   ABlockInbound,AllowOutbound') } }

    @{ Name = 'policy-allow-inbound'; Why = '인바운드가 열린 기기를 잡는다'; Expect = 'False'
       Run = { [string](Test-PolicyValueLine 'Firewall Policy   AllowInbound,AllowOutbound') } }

    @{ Name = 'policy-block-outbound'; Why = '아웃바운드가 막힌 기기도 우리 전제가 아니다'; Expect = 'False'
       Run = { [string](Test-PolicyValueLine 'Firewall Policy   BlockInbound,BlockOutbound') } }

    @{ Name = 'policy-trailing-space'; Why = 'R23. 줄 끝 공백은 정상 출력에도 있다'; Expect = 'True'
       Run = { [string](Test-PolicyValueLine 'Firewall Policy   BlockInbound,AllowOutbound   ') } }

    @{ Name = 'policy-tab-separated'; Why = 'R23. 공백 형태가 바뀌어도 통과해야 한다'; Expect = 'True'
       Run = { [string](Test-PolicyValueLine "Firewall Policy`tBlockInbound,AllowOutbound") } }

    @{ Name = 'policy-line-start'; Why = 'R23. 레이블 없이 값만 있는 줄'; Expect = 'True'
       Run = { [string](Test-PolicyValueLine 'BlockInbound,AllowOutbound') } }

    @{ Name = 'policy-empty-line'; Why = '빈 줄은 값이 아니다'; Expect = 'False'
       Run = { [string](Test-PolicyValueLine '') } }

    @{ Name = 'policy-null-line'; Why = '입력 가드. 예외 대신 False'; Expect = 'False'
       Run = { [string](Test-PolicyValueLine $null) } }

    # --- BlockInboundAlways 검출 --------------------------------------------
    @{ Name = 'always-detect'; Why = 'R9. 이 모드는 인바운드 허용 규칙을 전부 무시한다'; Expect = 'True'
       Run = { [string](Test-AlwaysModeLine 'Firewall Policy   BlockInboundAlways,AllowOutbound') } }

    @{ Name = 'always-block-outbound'; Why = 'R9. 뒤 값이 달라도 Always 다'; Expect = 'True'
       Run = { [string](Test-AlwaysModeLine 'Firewall Policy   BlockInboundAlways,BlockOutbound') } }

    @{ Name = 'always-absent-in-normal'; Why = '규칙 3. 정상 줄을 Always 로 읽으면 안 된다'; Expect = 'False'
       Run = { [string](Test-AlwaysModeLine 'Firewall Policy   BlockInbound,AllowOutbound') } }

    @{ Name = 'naive-substring-counterexample'; Why = 'R9 반례 고정. 부분 문자열 검사는 Always 를 통과시킨다'; Expect = 'True'
       Run = { [string]('Firewall Policy   BlockInboundAlways,AllowOutbound' -match 'BlockInbound') } }

    # --- 프로필 켜짐 상태 ----------------------------------------------------
    @{ Name = 'profiles-all-enabled'; Why = '규칙 3. 이 기기의 실제 상태'; Expect = 'ok'
       Run = { Get-ProfileStateVerdict (New-ProfileSet 'True' 'True' 'True') } }

    # 이 기기의 실제 형은 GpoBoolean 이다. 여기서는 bool 이 아니면서 이름이 'True' 인
    # 형의 대역으로 SwitchParameter 를 쓴다. `-is [bool]` 로 거르면 정상 기기가 떨어진다.
    @{ Name = 'profiles-nonbool-enabled'; Why = '실측. 값의 형은 bool 이 아니라 GpoBoolean 이다'; Expect = 'ok'
       Run = { Get-ProfileStateVerdict @(
                   (New-ProfileStub 'Domain'  ([System.Management.Automation.SwitchParameter]$true)),
                   (New-ProfileStub 'Private' ([System.Management.Automation.SwitchParameter]$true)),
                   (New-ProfileStub 'Public'  ([System.Management.Automation.SwitchParameter]$true))) } }

    @{ Name = 'profiles-public-disabled'; Why = 'R13. 꺼진 프로필에서는 정책 값이 의미 없다'; Expect = 'fail:Public'
       Run = { Get-ProfileStateVerdict (New-ProfileSet 'True' 'True' 'False') } }

    @{ Name = 'profiles-two-disabled'; Why = 'R13. 꺼진 프로필을 전부 적는다'; Expect = 'fail:Domain,Public'
       Run = { Get-ProfileStateVerdict (New-ProfileSet 'False' 'True' 'False') } }

    @{ Name = 'profiles-enabled-notconfigured'; Why = 'R16. -eq $false 는 NotConfigured 를 놓친다. 모르면 unknown'; Expect = 'unknown:Public=NotConfigured'
       Run = { Get-ProfileStateVerdict (New-ProfileSet 'True' 'True' 'NotConfigured') } }

    @{ Name = 'naive-enabled-counterexample'; Why = 'R16 반례 고정. 단축 구문은 NotConfigured 를 0건으로 센다'; Expect = '0'
       Run = { [string](@((New-ProfileSet 'True' 'True' 'NotConfigured') | Where-Object { $_.Enabled -eq $false }).Count) } }

    @{ Name = 'profiles-disabled-and-unknown'; Why = '확인된 결함이 모르는 항목에 가려지지 않는다'; Expect = 'fail:Domain'
       Run = { Get-ProfileStateVerdict (New-ProfileSet 'False' 'True' 'NotConfigured') } }

    @{ Name = 'profiles-reordered'; Why = '8라운드 warn. 순서가 달라도 같은 세 프로필이다 (규칙 3)'; Expect = 'ok'
       Run = { Get-ProfileStateVerdict @((New-ProfileStub 'Public' 'True'), (New-ProfileStub 'Domain' 'True'), (New-ProfileStub 'Private' 'True')) } }

    @{ Name = 'profiles-reordered-disabled'; Why = '순서가 달라도 꺼진 프로필은 같은 문장으로 걸린다'; Expect = 'fail:Public'
       Run = { Get-ProfileStateVerdict @((New-ProfileStub 'Public' 'False'), (New-ProfileStub 'Domain' 'True'), (New-ProfileStub 'Private' 'True')) } }

    @{ Name = 'profiles-disabled-order-stable'; Why = '출력이 조회 순서를 타지 않는다'; Expect = 'fail:Domain,Public'
       Run = { Get-ProfileStateVerdict @((New-ProfileStub 'Public' 'False'), (New-ProfileStub 'Private' 'True'), (New-ProfileStub 'Domain' 'False')) } }

    @{ Name = 'profiles-reordered-unknown-value'; Why = '순서가 달라도 모르는 값은 모른다'; Expect = 'unknown:Public=NotConfigured'
       Run = { Get-ProfileStateVerdict @((New-ProfileStub 'Public' 'NotConfigured'), (New-ProfileStub 'Domain' 'True'), (New-ProfileStub 'Private' 'True')) } }

    @{ Name = 'profiles-duplicate-name'; Why = '중복 이름은 세 프로필이 아니다'; Expect = 'unknown:프로필 이름 Domain,Domain,Public'
       Run = { Get-ProfileStateVerdict @((New-ProfileStub 'Domain' 'True'), (New-ProfileStub 'Domain' 'True'), (New-ProfileStub 'Public' 'True')) } }

    @{ Name = 'profiles-four'; Why = '넷이면 판정하지 않는다'; Expect = 'unknown:프로필 4개'
       Run = { Get-ProfileStateVerdict @((New-ProfileStub 'Domain' 'True'), (New-ProfileStub 'Private' 'True'), (New-ProfileStub 'Public' 'True'), (New-ProfileStub 'Guest' 'True')) } }

    # Windows 이름 비교는 대소문자를 가리지 않는다. 어댑터 별칭(2라운드)과 같은 규칙을 쓴다.
    # 여기서 대소문자로 떨어뜨리면 그것이 또 규칙 3 위반이 된다.
    @{ Name = 'profiles-case-insensitive'; Why = '규칙 3. 대소문자로 정상 기기를 떨어뜨리지 않는다'; Expect = 'ok'
       Run = { Get-ProfileStateVerdict @((New-ProfileStub 'domain' 'True'), (New-ProfileStub 'Private' 'True'), (New-ProfileStub 'Public' 'True')) } }

    @{ Name = 'profiles-missing-one'; Why = '세 프로필이 아니면 판정하지 않는다'; Expect = 'unknown:프로필 2개'
       Run = { Get-ProfileStateVerdict @((New-ProfileStub 'Domain' 'True'), (New-ProfileStub 'Private' 'True')) } }

    @{ Name = 'profiles-unexpected-name'; Why = '이름이 다르면 무엇을 본 것인지 모른다'; Expect = 'unknown:프로필 이름 Domain,Private,Guest'
       Run = { Get-ProfileStateVerdict @((New-ProfileStub 'Domain' 'True'), (New-ProfileStub 'Private' 'True'), (New-ProfileStub 'Guest' 'True')) } }

    @{ Name = 'profiles-empty'; Why = '조회 실패를 통과로 읽지 않는다'; Expect = 'unknown:프로필 0개'
       Run = { Get-ProfileStateVerdict @() } }

    @{ Name = 'profiles-null'; Why = '조회 자체를 못 했다'; Expect = 'unknown:조회 없음'
       Run = { Get-ProfileStateVerdict $null } }

    @{ Name = 'profiles-null-enabled'; Why = '값이 비면 모른다'; Expect = 'unknown:Public='
       Run = { Get-ProfileStateVerdict (New-ProfileSet 'True' 'True' '') } }

    # --- DefaultInboundAction 의 뜻 (참고용. 게이트가 아니다) ----------------
    @{ Name = 'inbound-notconfigured'; Why = '실측. NotConfigured 는 내장 기본값 Block 이다'; Expect = 'block'
       Run = { Get-InboundActionMeaning 'NotConfigured' } }

    @{ Name = 'inbound-block'; Why = '명시된 Block'; Expect = 'block'
       Run = { Get-InboundActionMeaning 'Block' } }

    @{ Name = 'inbound-allow'; Why = '인바운드가 열린 설정'; Expect = 'allow'
       Run = { Get-InboundActionMeaning 'Allow' } }

    @{ Name = 'inbound-garbage'; Why = '모르는 값은 모른다고 한다'; Expect = 'unknown'
       Run = { Get-InboundActionMeaning 'Something' } }

    @{ Name = 'naive-inbound-counterexample'; Why = '반례 고정. -eq Block 판정은 정상 기기를 떨어뜨린다'; Expect = 'False'
       Run = { [string]('NotConfigured' -eq 'Block') } }

    # --- 규칙 프로그램 경로 (R19) -------------------------------------------
    @{ Name = 'program-expanded-exists'; Why = 'R19. 환경 변수를 먼저 푼다'; Expect = 'exists'
       Run = { Get-RuleProgramState '%SystemRoot%\explorer.exe' } }

    @{ Name = 'naive-program-path-counterexample'; Why = 'R19 반례 고정. 풀지 않으면 멀쩡한 규칙이 죽은 것으로 보인다'; Expect = 'False'
       Run = { [string](Test-Path -LiteralPath '%SystemRoot%\explorer.exe') } }

    @{ Name = 'program-missing'; Why = '진짜 죽은 경로는 잡는다'; Expect = 'missing'
       Run = { Get-RuleProgramState 'C:\sangtachi-없는경로\없다.exe' } }

    @{ Name = 'program-any'; Why = '프로그램을 안 건 규칙이다. 죽은 것이 아니다'; Expect = 'any'
       Run = { Get-RuleProgramState 'Any' } }

    @{ Name = 'program-empty'; Why = '빈 값도 프로그램 미지정이다'; Expect = 'any'
       Run = { Get-RuleProgramState '' } }

    @{ Name = 'program-null'; Why = '입력 가드'; Expect = 'any'
       Run = { Get-RuleProgramState $null } }

    @{ Name = 'program-system-token'; Why = '실측. Windows 기본 규칙의 `System` 은 경로가 아니다'; Expect = 'unknown'
       Run = { Get-RuleProgramState 'System' } }

    @{ Name = 'program-relative-path'; Why = '상대 경로는 현재 디렉터리에 따라 답이 바뀐다'; Expect = 'unknown'
       Run = { Get-RuleProgramState 'notepad.exe' } }

    # --- 프로토콜 이름 맞추기 -----------------------------------------------
    @{ Name = 'protocol-name-tcp'; Why = '규칙 3. 이름 그대로'; Expect = 'TCP'
       Run = { ConvertTo-ProtocolName 'TCP' } }

    @{ Name = 'protocol-number-tcp'; Why = '규칙 3. 번호로 오는 판이 있다. 6 은 TCP 다'; Expect = 'TCP'
       Run = { ConvertTo-ProtocolName '6' } }

    @{ Name = 'protocol-number-icmpv6'; Why = '58 은 ICMPv6 다'; Expect = 'ICMPv6'
       Run = { ConvertTo-ProtocolName '58' } }

    @{ Name = 'protocol-lowercase'; Why = '규칙 3. 대소문자로 떨어뜨리지 않는다'; Expect = 'ICMPv4'
       Run = { ConvertTo-ProtocolName 'icmpv4' } }

    @{ Name = 'protocol-unknown-number'; Why = '모르는 번호는 지어내지 않고 그대로 둔다'; Expect = '253'
       Run = { ConvertTo-ProtocolName '253' } }

    # --- 포트 폭 계산 (2라운드 warn) ----------------------------------------
    @{ Name = 'port-single'; Why = '규칙 3. 한 포트'; Expect = '25565-25565'
       Run = { ConvertTo-PortRangeSet '25565' } }

    @{ Name = 'port-adjacent-merge'; Why = '규칙 3. `25565,25566` 과 `25565-25566` 은 같은 폭이다'; Expect = '25565-25566'
       Run = { ConvertTo-PortRangeSet '25565,25566' } }

    @{ Name = 'port-full-range'; Why = '2라운드 warn. 전 포트를 여는 표기'; Expect = '1-65535'
       Run = { ConvertTo-PortRangeSet '1-65535' } }

    @{ Name = 'port-any'; Why = '문자 그대로의 Any'; Expect = 'any'
       Run = { ConvertTo-PortRangeSet 'Any' } }

    @{ Name = 'port-empty'; Why = '빈 값과 Any 를 구분한다'; Expect = 'empty'
       Run = { ConvertTo-PortRangeSet '' } }

    @{ Name = 'port-keyword'; Why = '`RPC` 는 동적 범위다. 폭을 모른다'; Expect = 'unknown'
       Run = { ConvertTo-PortRangeSet 'RPC' } }

    @{ Name = 'port-zero'; Why = '범위 밖 숫자를 받지 않는다'; Expect = 'unknown'
       Run = { ConvertTo-PortRangeSet '0' } }

    @{ Name = 'port-too-big'; Why = '65536 은 포트가 아니다'; Expect = 'unknown'
       Run = { ConvertTo-PortRangeSet '65536' } }

    @{ Name = 'port-reversed-range'; Why = '뒤집힌 구간을 조용히 받지 않는다'; Expect = 'unknown'
       Run = { ConvertTo-PortRangeSet '200-100' } }

    @{ Name = 'port-covers-self'; Why = '규칙 3. 같은 폭은 담는다'; Expect = 'True'
       Run = { [string](Test-PortRangeCovers '25565-25565' '25565-25565') } }

    @{ Name = 'port-covers-wider'; Why = '전 범위는 한 포트를 담는다'; Expect = 'True'
       Run = { [string](Test-PortRangeCovers '1-65535' '25565-25565') } }

    @{ Name = 'port-covers-disjoint'; Why = '엉뚱한 포트는 담지 않는다'; Expect = 'False'
       Run = { [string](Test-PortRangeCovers '3389-3389' '25565-25565') } }

    # --- ICMP 종류 집합 계산 -------------------------------------------------
    @{ Name = 'icmp-set-type-only'; Why = '실측. `8` 은 종류 8 의 모든 코드다'; Expect = '8:*'
       Run = { ConvertTo-IcmpTypeSet '8' } }

    @{ Name = 'icmp-set-type-code'; Why = '실측. 이 기기의 `3:4`'; Expect = '3:4'
       Run = { ConvertTo-IcmpTypeSet '3:4' } }

    @{ Name = 'icmp-set-numeric-sort'; Why = '문자열로 정렬하면 `10` 이 `8` 앞에 온다'; Expect = '8:*,10:*'
       Run = { ConvertTo-IcmpTypeSet '10,8' } }

    @{ Name = 'icmp-set-dedupe'; Why = '같은 종류를 두 번 적어도 한 번이다'; Expect = '8:*'
       Run = { ConvertTo-IcmpTypeSet '8,8' } }

    @{ Name = 'icmp-set-any'; Why = '실측. 모든 종류를 여는 규칙이 있다'; Expect = 'any'
       Run = { ConvertTo-IcmpTypeSet 'Any' } }

    @{ Name = 'icmp-set-empty'; Why = '빈 값과 Any 를 구분한다'; Expect = 'empty'
       Run = { ConvertTo-IcmpTypeSet '' } }

    @{ Name = 'icmp-set-out-of-range'; Why = '256 은 ICMP 종류가 아니다'; Expect = 'unknown'
       Run = { ConvertTo-IcmpTypeSet '256' } }

    @{ Name = 'icmp-set-bad-token'; Why = '이름 표기는 폭을 모른다'; Expect = 'unknown'
       Run = { ConvertTo-IcmpTypeSet 'echo' } }

    @{ Name = 'icmp-covers-star'; Why = '규칙 3. `8:*` 는 `8:0` 을 담는다'; Expect = 'True'
       Run = { [string](Test-IcmpTypeCovers '8:*' '8:0') } }

    @{ Name = 'icmp-covers-exact'; Why = '규칙 3. 같은 값은 담는다'; Expect = 'True'
       Run = { [string](Test-IcmpTypeCovers '3:4' '3:4') } }

    @{ Name = 'icmp-covers-narrow'; Why = '`8:0` 은 `8:*` 를 담지 못한다'; Expect = 'False'
       Run = { [string](Test-IcmpTypeCovers '8:0' '8:*') } }

    @{ Name = 'icmp-covers-other-type'; Why = '다른 종류는 담지 않는다'; Expect = 'False'
       Run = { [string](Test-IcmpTypeCovers '0:*' '8:*') } }

    # --- 규칙 범위 (R24, R13, 2라운드 blocker/warn) --------------------------
    # 기대 집합을 인자로 준다. 무엇과 대조하는지 검사 표에 적힌다.
    @{ Name = 'scope-good'; Why = '규칙 3. 문서가 권하는 형태가 통과해야 한다'; Expect = 'scoped'
       Run = { Get-RuleScopeVerdict (New-RuleStub 'Any' 'sangtachi0' 'TCP' '25565') 'sangtachi0' @((New-RuleExpectation 'TCP' '25565')) } }

    @{ Name = 'scope-alias-case-insensitive'; Why = '규칙 3. 어댑터 이름은 대소문자를 가리지 않는다'; Expect = 'scoped'
       Run = { Get-RuleScopeVerdict (New-RuleStub 'Any' 'SANGTACHI0' 'TCP' '25565') 'sangtachi0' @((New-RuleExpectation 'TCP' '25565')) } }

    @{ Name = 'scope-alias-spacing'; Why = '규칙 3. 앞뒤 공백으로 떨어뜨리지 않는다'; Expect = 'scoped'
       Run = { Get-RuleScopeVerdict (New-RuleStub 'Any' ' sangtachi0 ' 'TCP' '25565') 'sangtachi0' @((New-RuleExpectation 'TCP' '25565')) } }

    @{ Name = 'scope-wrong-adapter'; Why = '2라운드 blocker. Any 가 아니어도 엉뚱한 어댑터면 우리 규칙이 아니다'; Expect = 'other-adapter'
       Run = { Get-RuleScopeVerdict (New-RuleStub 'Any' 'Wi-Fi' 'TCP' '25565') 'sangtachi0' @((New-RuleExpectation 'TCP' '25565')) } }

    @{ Name = 'scope-alias-extra'; Why = '2라운드 blocker. 우리 어댑터를 포함해도 물리 어댑터가 같이 있으면 넓다'; Expect = 'wide'
       Run = { Get-RuleScopeVerdict (New-RuleStub 'Any' 'sangtachi0,Wi-Fi' 'TCP' '25565') 'sangtachi0' @((New-RuleExpectation 'TCP' '25565')) } }

    @{ Name = 'scope-any-interface'; Why = 'R24. 물리 네트워크에도 열린다'; Expect = 'wide'
       Run = { Get-RuleScopeVerdict (New-RuleStub 'Any' 'Any' 'TCP' '25565') 'sangtachi0' @((New-RuleExpectation 'TCP' '25565')) } }

    @{ Name = 'scope-any-protocol'; Why = 'R24. 프로토콜을 안 좁히면 범위가 넓다'; Expect = 'wide'
       Run = { Get-RuleScopeVerdict (New-RuleStub 'Any' 'sangtachi0' 'Any' '25565') 'sangtachi0' @((New-RuleExpectation 'TCP' '25565')) } }

    @{ Name = 'scope-other-protocol'; Why = '기대하지 않은 프로토콜을 연 규칙'; Expect = 'other-protocol'
       Run = { Get-RuleScopeVerdict (New-RuleStub 'Any' 'sangtachi0' 'UDP' '25565') 'sangtachi0' @((New-RuleExpectation 'TCP' '25565')) } }

    @{ Name = 'scope-any-port'; Why = 'R24. 포트를 안 좁히면 범위가 넓다'; Expect = 'wide'
       Run = { Get-RuleScopeVerdict (New-RuleStub 'Any' 'sangtachi0' 'TCP' 'Any') 'sangtachi0' @((New-RuleExpectation 'TCP' '25565')) } }

    @{ Name = 'scope-port-all-range'; Why = '2라운드 warn. `1-65535` 는 Any 와 폭이 같다'; Expect = 'wide'
       Run = { Get-RuleScopeVerdict (New-RuleStub 'Any' 'sangtachi0' 'TCP' '1-65535') 'sangtachi0' @((New-RuleExpectation 'TCP' '25565')) } }

    @{ Name = 'scope-port-comma-superset'; Why = '2라운드 warn. 콤마 목록으로 더 여는 규칙'; Expect = 'wide'
       Run = { Get-RuleScopeVerdict (New-RuleStub 'Any' 'sangtachi0' 'TCP' '25565,25566') 'sangtachi0' @((New-RuleExpectation 'TCP' '25565')) } }

    @{ Name = 'scope-port-equal-range-form'; Why = '규칙 3. `25565-25565` 는 `25565` 와 같다'; Expect = 'scoped'
       Run = { Get-RuleScopeVerdict (New-RuleStub 'Any' 'sangtachi0' 'TCP' '25565-25565') 'sangtachi0' @((New-RuleExpectation 'TCP' '25565')) } }

    @{ Name = 'scope-port-other'; Why = '다른 포트를 연 규칙은 우리 규칙이 아니다'; Expect = 'other-port'
       Run = { Get-RuleScopeVerdict (New-RuleStub 'Any' 'sangtachi0' 'TCP' '3389') 'sangtachi0' @((New-RuleExpectation 'TCP' '25565')) } }

    @{ Name = 'scope-port-keyword'; Why = '폭을 모르는 표기는 통과시키지 않는다'; Expect = 'unknown'
       Run = { Get-RuleScopeVerdict (New-RuleStub 'Any' 'sangtachi0' 'TCP' 'RPC') 'sangtachi0' @((New-RuleExpectation 'TCP' '25565')) } }

    @{ Name = 'scope-port-missing'; Why = 'TCP 규칙에 포트가 없으면 모른다'; Expect = 'unknown'
       Run = { Get-RuleScopeVerdict (New-RuleStub 'Any' 'sangtachi0' 'TCP' $null) 'sangtachi0' @((New-RuleExpectation 'TCP' '25565')) } }

    @{ Name = 'scope-profile-full-set'; Why = '2라운드 warn. 세 프로필 전부는 Any 와 폭이 같다 (규칙 3)'; Expect = 'scoped'
       Run = { Get-RuleScopeVerdict (New-RuleStub 'Domain, Private, Public' 'sangtachi0' 'TCP' '25565') 'sangtachi0' @((New-RuleExpectation 'TCP' '25565')) } }

    @{ Name = 'scope-profile-full-set-unordered'; Why = '2라운드 warn. 순서가 달라도 같은 집합이다'; Expect = 'scoped'
       Run = { Get-RuleScopeVerdict (New-RuleStub 'Public,Domain,Private' 'sangtachi0' 'TCP' '25565') 'sangtachi0' @((New-RuleExpectation 'TCP' '25565')) } }

    @{ Name = 'scope-profile-two-of-three'; Why = 'R13. 둘만 있으면 나머지 분류에서 안 걸린다'; Expect = 'profile-dependent'
       Run = { Get-RuleScopeVerdict (New-RuleStub 'Domain,Private' 'sangtachi0' 'TCP' '25565') 'sangtachi0' @((New-RuleExpectation 'TCP' '25565')) } }

    @{ Name = 'scope-profile-private-only'; Why = 'R13. 분류가 Public 이면 안 걸린다. 분류 변경은 실패할 수 있다'; Expect = 'profile-dependent'
       Run = { Get-RuleScopeVerdict (New-RuleStub 'Private' 'sangtachi0' 'TCP' '25565') 'sangtachi0' @((New-RuleExpectation 'TCP' '25565')) } }

    # --- ICMP 종류 (3라운드 blocker) ----------------------------------------
    # 기대는 에코 요청(type 8) 전용이다. 문서 2절의 `-Protocol ICMPv4 -IcmpType 8` 이다.
    @{ Name = 'scope-icmp-echo-only'; Why = '규칙 3. 에코만 여는 정상 규칙. ICMP 에는 포트가 없다'; Expect = 'scoped'
       Run = { Get-RuleScopeVerdict (New-RuleStub 'Any' 'sangtachi0' 'ICMPv4' $null '8') 'sangtachi0' @((ConvertTo-RuleExpectation 'ICMPv4:8')) } }

    @{ Name = 'scope-icmp-any-type'; Why = '3라운드 blocker. 어댑터로 좁혀도 모든 ICMP 종류를 연다'; Expect = 'wide'
       Run = { Get-RuleScopeVerdict (New-RuleStub 'Any' 'sangtachi0' 'ICMPv4' $null 'Any') 'sangtachi0' @((ConvertTo-RuleExpectation 'ICMPv4:8')) } }

    @{ Name = 'scope-icmp-extra-types'; Why = '3라운드 blocker. 에코 말고 다른 종류까지 열었다'; Expect = 'wide'
       Run = { Get-RuleScopeVerdict (New-RuleStub 'Any' 'sangtachi0' 'ICMPv4' $null '8,0') 'sangtachi0' @((ConvertTo-RuleExpectation 'ICMPv4:8')) } }

    @{ Name = 'scope-icmp-other-type'; Why = '에코가 아닌 종류만 연 규칙은 우리 규칙이 아니다'; Expect = 'other-icmp-type'
       Run = { Get-RuleScopeVerdict (New-RuleStub 'Any' 'sangtachi0' 'ICMPv4' $null '0') 'sangtachi0' @((ConvertTo-RuleExpectation 'ICMPv4:8')) } }

    @{ Name = 'scope-icmp-type-missing'; Why = '종류를 알 수 없으면 통과로 적지 않는다'; Expect = 'unknown'
       Run = { Get-RuleScopeVerdict (New-RuleStub 'Any' 'sangtachi0' 'ICMPv4' $null '') 'sangtachi0' @((ConvertTo-RuleExpectation 'ICMPv4:8')) } }

    @{ Name = 'scope-icmp-bad-type'; Why = '모르는 표기를 조용히 받지 않는다'; Expect = 'unknown'
       Run = { Get-RuleScopeVerdict (New-RuleStub 'Any' 'sangtachi0' 'ICMPv4' $null 'echo') 'sangtachi0' @((ConvertTo-RuleExpectation 'ICMPv4:8')) } }

    @{ Name = 'scope-icmp-code-exact'; Why = '규칙 3. 이 기기의 실제 값 `3:4` 형태'; Expect = 'scoped'
       Run = { Get-RuleScopeVerdict (New-RuleStub 'Any' 'sangtachi0' 'ICMPv4' $null '3:4') 'sangtachi0' @((ConvertTo-RuleExpectation 'ICMPv4:3:4')) } }

    @{ Name = 'scope-icmp-all-codes-is-wider'; Why = '`8` 은 종류 8 의 모든 코드다. `8:0` 보다 넓다'; Expect = 'wide'
       Run = { Get-RuleScopeVerdict (New-RuleStub 'Any' 'sangtachi0' 'ICMPv4' $null '8') 'sangtachi0' @((ConvertTo-RuleExpectation 'ICMPv4:8:0')) } }

    @{ Name = 'scope-icmp-narrower-code'; Why = '기대보다 좁아도 기대와 다르면 우리 규칙이 아니다'; Expect = 'other-icmp-type'
       Run = { Get-RuleScopeVerdict (New-RuleStub 'Any' 'sangtachi0' 'ICMPv4' $null '8:0') 'sangtachi0' @((ConvertTo-RuleExpectation 'ICMPv4:8')) } }

    @{ Name = 'scope-icmp-expectation-without-type'; Why = '기대가 종류를 안 적었으면 대조할 것이 없다'; Expect = 'unknown'
       Run = { Get-RuleScopeVerdict (New-RuleStub 'Any' 'sangtachi0' 'ICMPv4' $null '8') 'sangtachi0' @((ConvertTo-RuleExpectation 'ICMPv4')) } }

    @{ Name = 'scope-icmp-v6-echo'; Why = '규칙 3. ICMPv6 에코는 128 이다'; Expect = 'scoped'
       Run = { Get-RuleScopeVerdict (New-RuleStub 'Any' 'sangtachi0' 'ICMPv6' $null '128') 'sangtachi0' @((ConvertTo-RuleExpectation 'ICMPv6:128')) } }

    @{ Name = 'scope-icmp-not-expected'; Why = 'ICMP 를 기대하지 않았는데 열려 있다'; Expect = 'other-protocol'
       Run = { Get-RuleScopeVerdict (New-RuleStub 'Any' 'sangtachi0' 'ICMPv6' $null '128') 'sangtachi0' @((New-RuleExpectation 'TCP' '25565')) } }

    @{ Name = 'scope-multi-expectation'; Why = '규칙 3. 기대 집합이 여럿이면 하나만 맞아도 된다'; Expect = 'scoped'
       Run = { Get-RuleScopeVerdict (New-RuleStub 'Any' 'sangtachi0' 'ICMPv4' $null '8') 'sangtachi0' @((ConvertTo-RuleExpectation 'TCP:25565'), (ConvertTo-RuleExpectation 'ICMPv4:8')) } }

    @{ Name = 'scope-icmp-profile-dependent'; Why = 'ICMP 규칙도 프로필 검사를 지난다'; Expect = 'profile-dependent'
       Run = { Get-RuleScopeVerdict (New-RuleStub 'Private' 'sangtachi0' 'ICMPv4' $null '8') 'sangtachi0' @((ConvertTo-RuleExpectation 'ICMPv4:8')) } }

    @{ Name = 'scope-no-expectation'; Why = '기대 집합이 없으면 판정하지 않는다'; Expect = 'unknown'
       Run = { Get-RuleScopeVerdict (New-RuleStub 'Any' 'sangtachi0' 'TCP' '25565') 'sangtachi0' @() } }

    @{ Name = 'scope-no-expected-alias'; Why = '무엇과 대조할지 모르면 통과로 적지 않는다'; Expect = 'unknown'
       Run = { Get-RuleScopeVerdict (New-RuleStub 'Any' 'sangtachi0' 'TCP' '25565') '' @((New-RuleExpectation 'TCP' '25565')) } }

    @{ Name = 'scope-missing-interface'; Why = '값이 없으면 판정하지 않는다'; Expect = 'unknown'
       Run = { Get-RuleScopeVerdict (New-RuleStub 'Any' $null 'TCP' '25565') 'sangtachi0' @((New-RuleExpectation 'TCP' '25565')) } }

    @{ Name = 'scope-null-rule'; Why = '입력 가드'; Expect = 'unknown'
       Run = { Get-RuleScopeVerdict $null 'sangtachi0' @((New-RuleExpectation 'TCP' '25565')) } }

    # --- 기대 집합 문자열 파싱 ----------------------------------------------
    @{ Name = 'expectation-parse-port'; Why = '규칙 3. `TCP:25565` 형태'; Expect = 'TCP/25565/'
       Run = { $e = ConvertTo-RuleExpectation 'TCP:25565'; '{0}/{1}/{2}' -f $e.Protocol, $e.Port, $e.IcmpType } }

    @{ Name = 'expectation-parse-icmp'; Why = '규칙 3. 종류를 안 적은 형태'; Expect = 'ICMPv4//'
       Run = { $e = ConvertTo-RuleExpectation 'ICMPv4'; '{0}/{1}/{2}' -f $e.Protocol, $e.Port, $e.IcmpType } }

    @{ Name = 'expectation-parse-icmp-type'; Why = '3라운드. ICMP 뒤쪽은 포트가 아니라 종류다'; Expect = 'ICMPv4//8'
       Run = { $e = ConvertTo-RuleExpectation 'ICMPv4:8'; '{0}/{1}/{2}' -f $e.Protocol, $e.Port, $e.IcmpType } }

    @{ Name = 'expectation-parse-icmp-code'; Why = '`ICMPv4:3:4` 는 종류 3 코드 4 다'; Expect = 'ICMPv4//3:4'
       Run = { $e = ConvertTo-RuleExpectation 'ICMPv4:3:4'; '{0}/{1}/{2}' -f $e.Protocol, $e.Port, $e.IcmpType } }

    @{ Name = 'expectation-parse-empty'; Why = '빈 문자열은 기대 집합이 아니다'; Expect = 'null'
       Run = { $e = ConvertTo-RuleExpectation '  '; if ($null -eq $e) { 'null' } else { 'object' } } }

    # --- 범위 파라미터 지원 (R31) -------------------------------------------
    @{ Name = 'param-present'; Why = 'R31. 이 기기에는 있다. 실측으로 기각한 지적이다'; Expect = 'True'
       Run = { Get-ScopeParameterSupport @('DisplayName', 'InterfaceAlias', 'Profile') } }

    @{ Name = 'param-absent'; Why = 'R31. 없는 기기라면 범위를 좁히라는 안내를 낼 수 없다'; Expect = 'False'
       Run = { Get-ScopeParameterSupport @('DisplayName', 'Profile') } }

    @{ Name = 'param-unknown'; Why = 'cmdlet 조회 자체를 못 했다'; Expect = 'unknown'
       Run = { Get-ScopeParameterSupport $null } }

    # --- 종합 판정 -----------------------------------------------------------
    @{ Name = 'verdict-normal'; Why = '규칙 3. 이 기기의 실제 출력이 통과해야 한다'; Expect = 'pass'
       Run = { (Get-FirewallPolicyVerdict -PolicyText (New-NetshSample 'BlockInbound,AllowOutbound' 'BlockInbound,AllowOutbound' 'BlockInbound,AllowOutbound') -Profiles (New-ProfileSet 'True' 'True' 'True') -ScopeParameterSupport 'True').Verdict } }

    @{ Name = 'verdict-label-korean'; Why = 'R8. 한국어 표시 Windows 도 통과해야 한다'; Expect = 'pass'
       Run = { (Get-FirewallPolicyVerdict -PolicyText (New-NetshSample 'BlockInbound,AllowOutbound' 'BlockInbound,AllowOutbound' 'BlockInbound,AllowOutbound' -Label '방화벽 정책') -Profiles (New-ProfileSet 'True' 'True' 'True') -ScopeParameterSupport 'True').Verdict } }

    @{ Name = 'verdict-always'; Why = 'R9. 허용 규칙이 무시되는 모드'; Expect = 'fail'
       Run = { (Get-FirewallPolicyVerdict -PolicyText (New-NetshSample 'BlockInbound,AllowOutbound' 'BlockInbound,AllowOutbound' 'BlockInboundAlways,AllowOutbound') -Profiles (New-ProfileSet 'True' 'True' 'True') -ScopeParameterSupport 'True').Verdict } }

    @{ Name = 'verdict-allow-inbound'; Why = '인바운드가 이미 열린 기기'; Expect = 'fail'
       Run = { (Get-FirewallPolicyVerdict -PolicyText (New-NetshSample 'BlockInbound,AllowOutbound' 'AllowInbound,AllowOutbound' 'BlockInbound,AllowOutbound') -Profiles (New-ProfileSet 'True' 'True' 'True') -ScopeParameterSupport 'True').Verdict } }

    @{ Name = 'verdict-profile-disabled'; Why = 'R13. 정책 값만 보면 통과였을 입력'; Expect = 'fail'
       Run = { (Get-FirewallPolicyVerdict -PolicyText (New-NetshSample 'BlockInbound,AllowOutbound' 'BlockInbound,AllowOutbound' 'BlockInbound,AllowOutbound') -Profiles (New-ProfileSet 'True' 'True' 'False') -ScopeParameterSupport 'True').Verdict } }

    @{ Name = 'verdict-ignores-inbound-action'; Why = '실측. NotConfigured 가 판정을 흔들면 안 된다'; Expect = 'pass'
       Run = { (Get-FirewallPolicyVerdict -PolicyText (New-NetshSample 'BlockInbound,AllowOutbound' 'BlockInbound,AllowOutbound' 'BlockInbound,AllowOutbound') -Profiles (New-ProfileSet 'True' 'True' 'True' -InboundAction 'NotConfigured') -ScopeParameterSupport 'True').Verdict } }

    @{ Name = 'verdict-empty-netsh'; Why = '출력이 없으면 모른다. 통과가 아니다'; Expect = 'unknown'
       Run = { (Get-FirewallPolicyVerdict -PolicyText @() -Profiles (New-ProfileSet 'True' 'True' 'True') -ScopeParameterSupport 'True').Verdict } }

    @{ Name = 'verdict-null-netsh'; Why = 'netsh 자체를 못 돌렸다'; Expect = 'unknown'
       Run = { (Get-FirewallPolicyVerdict -PolicyText $null -Profiles (New-ProfileSet 'True' 'True' 'True') -ScopeParameterSupport 'True').Verdict } }

    @{ Name = 'verdict-two-policy-lines'; Why = '값 줄이 3개가 아니면 무엇을 본 것인지 모른다'; Expect = 'unknown'
       Run = { (Get-FirewallPolicyVerdict -PolicyText @('Firewall Policy   BlockInbound,AllowOutbound', 'Firewall Policy   BlockInbound,AllowOutbound') -Profiles (New-ProfileSet 'True' 'True' 'True') -ScopeParameterSupport 'True').Verdict } }

    @{ Name = 'verdict-profiles-unknown'; Why = '한쪽만 알면 전체는 모른다'; Expect = 'unknown'
       Run = { (Get-FirewallPolicyVerdict -PolicyText (New-NetshSample 'BlockInbound,AllowOutbound' 'BlockInbound,AllowOutbound' 'BlockInbound,AllowOutbound') -Profiles $null -ScopeParameterSupport 'True').Verdict } }

    @{ Name = 'verdict-fail-beats-unknown'; Why = '확인된 결함은 모르는 항목에 가려지지 않는다'; Expect = 'fail'
       Run = { (Get-FirewallPolicyVerdict -PolicyText $null -Profiles (New-ProfileSet 'True' 'True' 'False') -ScopeParameterSupport 'True').Verdict } }

    @{ Name = 'verdict-extra-check-fail'; Why = '추가 검사도 판정에 들어간다'; Expect = 'fail'
       Run = { (Get-FirewallPolicyVerdict -PolicyText (New-NetshSample 'BlockInbound,AllowOutbound' 'BlockInbound,AllowOutbound' 'BlockInbound,AllowOutbound') -Profiles (New-ProfileSet 'True' 'True' 'True') -ExtraChecks @((New-Check 'rule-scope' 'fail' '범위 좁힘' '어댑터 지정 없음')) -ScopeParameterSupport 'True').Verdict } }

    @{ Name = 'verdict-check-count'; Why = '검사 항목을 빠뜨리지 않는다'; Expect = '3'
       Run = { [string]((Get-FirewallPolicyVerdict -PolicyText (New-NetshSample 'BlockInbound,AllowOutbound' 'BlockInbound,AllowOutbound' 'BlockInbound,AllowOutbound') -Profiles (New-ProfileSet 'True' 'True' 'True') -ScopeParameterSupport 'True').Checks.Count) } }

    @{ Name = 'verdict-policy-detail'; Why = '실제값을 사람이 다시 세지 않게 한다'; Expect = '값 줄 3, 기대값 2, Always 1'
       Run = { ((Get-FirewallPolicyVerdict -PolicyText (New-NetshSample 'BlockInbound,AllowOutbound' 'BlockInbound,AllowOutbound' 'BlockInboundAlways,AllowOutbound') -Profiles (New-ProfileSet 'True' 'True' 'True') -ScopeParameterSupport 'True').Checks | Where-Object { $_.Name -eq 'policy' }).Actual } }

    # --- 범위 파라미터가 판정까지 전파되는가 (4라운드 blocker) ---------------
    # 헬퍼 반환값만 보면 부족하다. 값이 최종 판정을 바꾸는지를 본다.
    @{ Name = 'verdict-scope-param-present'; Why = '규칙 3. 지원되는 정상 기기는 pass 로 남는다'; Expect = 'pass'
       Run = { (Get-FirewallPolicyVerdict -PolicyText (New-NetshSample 'BlockInbound,AllowOutbound' 'BlockInbound,AllowOutbound' 'BlockInbound,AllowOutbound') -Profiles (New-ProfileSet 'True' 'True' 'True') -ScopeParameterSupport 'True').Verdict } }

    @{ Name = 'verdict-scope-param-absent'; Why = '4라운드 blocker. 범위 규칙을 못 만드는 기기가 pass 로 끝나면 안 된다'; Expect = 'fail'
       Run = { (Get-FirewallPolicyVerdict -PolicyText (New-NetshSample 'BlockInbound,AllowOutbound' 'BlockInbound,AllowOutbound' 'BlockInbound,AllowOutbound') -Profiles (New-ProfileSet 'True' 'True' 'True') -ScopeParameterSupport 'False').Verdict } }

    @{ Name = 'verdict-scope-param-unknown'; Why = '조회를 못 했으면 통과가 아니다'; Expect = 'unknown'
       Run = { (Get-FirewallPolicyVerdict -PolicyText (New-NetshSample 'BlockInbound,AllowOutbound' 'BlockInbound,AllowOutbound' 'BlockInbound,AllowOutbound') -Profiles (New-ProfileSet 'True' 'True' 'True') -ScopeParameterSupport 'unknown').Verdict } }

    @{ Name = 'verdict-scope-param-missing'; Why = '인자를 안 주면 모른다. 통과로 적지 않는다'; Expect = 'unknown'
       Run = { (Get-FirewallPolicyVerdict -PolicyText (New-NetshSample 'BlockInbound,AllowOutbound' 'BlockInbound,AllowOutbound' 'BlockInbound,AllowOutbound') -Profiles (New-ProfileSet 'True' 'True' 'True')).Verdict } }

    @{ Name = 'verdict-scope-param-garbage'; Why = '모르는 값을 True 로 읽지 않는다'; Expect = 'unknown'
       Run = { (Get-FirewallPolicyVerdict -PolicyText (New-NetshSample 'BlockInbound,AllowOutbound' 'BlockInbound,AllowOutbound' 'BlockInbound,AllowOutbound') -Profiles (New-ProfileSet 'True' 'True' 'True') -ScopeParameterSupport 'yes').Verdict } }

    @{ Name = 'verdict-scope-param-check-result'; Why = '검사 줄에 결과가 남는다'; Expect = 'fail/없음. 어댑터로 좁힌 규칙을 만들 수 없다'
       Run = { $c = (Get-FirewallPolicyVerdict -PolicyText (New-NetshSample 'BlockInbound,AllowOutbound' 'BlockInbound,AllowOutbound' 'BlockInbound,AllowOutbound') -Profiles (New-ProfileSet 'True' 'True' 'True') -ScopeParameterSupport 'False').Checks | Where-Object { $_.Name -eq 'scope-param' }
               '{0}/{1}' -f $c.Result, $c.Actual } }

    @{ Name = 'verdict-scope-param-helper-to-verdict'; Why = '헬퍼 값이 그대로 판정에 들어간다 (전파 확인)'; Expect = 'fail'
       Run = { (Get-FirewallPolicyVerdict -PolicyText (New-NetshSample 'BlockInbound,AllowOutbound' 'BlockInbound,AllowOutbound' 'BlockInbound,AllowOutbound') -Profiles (New-ProfileSet 'True' 'True' 'True') -ScopeParameterSupport (Get-ScopeParameterSupport @('DisplayName', 'Profile'))).Verdict } }

    @{ Name = 'verdict-scope-param-helper-present'; Why = '규칙 3. 파라미터가 있는 목록이면 pass 로 남는다'; Expect = 'pass'
       Run = { (Get-FirewallPolicyVerdict -PolicyText (New-NetshSample 'BlockInbound,AllowOutbound' 'BlockInbound,AllowOutbound' 'BlockInbound,AllowOutbound') -Profiles (New-ProfileSet 'True' 'True' 'True') -ScopeParameterSupport (Get-ScopeParameterSupport @('DisplayName', 'InterfaceAlias', 'Profile'))).Verdict } }

    @{ Name = 'verdict-scope-param-fail-with-policy-fail'; Why = '다른 결함과 같이 있어도 fail 이다'; Expect = 'fail'
       Run = { (Get-FirewallPolicyVerdict -PolicyText $null -Profiles (New-ProfileSet 'True' 'True' 'True') -ScopeParameterSupport 'False').Verdict } }

    # --- 종료 코드 -----------------------------------------------------------
    @{ Name = 'exit-code-pass'; Why = '판정과 종료 코드가 같이 간다'; Expect = '0'
       Run = { [string](Get-VerdictExitCode $script:PASS) } }

    @{ Name = 'exit-code-fail'; Why = '결함은 1'; Expect = '1'
       Run = { [string](Get-VerdictExitCode $script:FAIL) } }

    @{ Name = 'exit-code-unknown'; Why = '판정 불가는 0 이 아니다'; Expect = '2'
       Run = { [string](Get-VerdictExitCode $script:UNKNOWN) } }

    @{ Name = 'exit-code-garbage'; Why = '모르는 판정을 0 으로 돌리지 않는다'; Expect = '2'
       Run = { [string](Get-VerdictExitCode 'whatever') } }

    # --- 항목별 판정 합치기 --------------------------------------------------
    @{ Name = 'rollup-all-pass'; Why = '규칙 3. 전부 통과면 pass'; Expect = 'pass'
       Run = { Get-StateRollup @((New-StateStub 'a' 'scoped'), (New-StateStub 'b' 'scoped')) @('scoped') @('unknown') } }

    @{ Name = 'rollup-unknown'; Why = '모르는 항목이 있으면 pass 가 아니다'; Expect = 'unknown'
       Run = { Get-StateRollup @((New-StateStub 'a' 'scoped'), (New-StateStub 'b' 'unknown')) @('scoped') @('unknown') } }

    @{ Name = 'rollup-fail'; Why = '넓은 규칙 하나면 fail'; Expect = 'fail'
       Run = { Get-StateRollup @((New-StateStub 'a' 'scoped'), (New-StateStub 'b' 'wide')) @('scoped') @('unknown') } }

    @{ Name = 'rollup-fail-beats-unknown'; Why = '확인된 결함이 모르는 항목에 가려지지 않는다'; Expect = 'fail'
       Run = { Get-StateRollup @((New-StateStub 'a' 'unknown'), (New-StateStub 'b' 'other-adapter')) @('scoped') @('unknown') } }

    @{ Name = 'rollup-empty'; Why = '볼 것이 없으면 통과가 아니다'; Expect = 'unknown'
       Run = { Get-StateRollup @() @('scoped') @('unknown') } }

    @{ Name = 'rollup-name-with-equals'; Why = '이름에 `=` 가 있어도 흔들리지 않는다 (문자열 매칭 회귀)'; Expect = 'pass'
       Run = { Get-StateRollup @((New-StateStub 'rule=wide (In)' 'scoped')) @('scoped') @('unknown') } }

    @{ Name = 'rollup-program-any-passes'; Why = '규칙 3. 프로그램 미지정은 결함이 아니다'; Expect = 'pass'
       Run = { Get-StateRollup @((New-StateStub 'a' 'exists'), (New-StateStub 'b' 'any')) @('exists', 'any') @('unknown') } }

    # --- 기대 엔드포인트 덮음 (6라운드 blocker) -----------------------------
    # 결정: **전부 요구**다. 문서 2절이 TCP 25565 와 ICMP 에코를 둘 다 요구한다.
    # 하나만 있으면 다른 쪽이 안 열린다. 규칙이 아예 0건인 Phase 6 이전은 `Get-RuleChecks`
    # 가 `unknown` 으로 처리한다. 결함이 아니라 아직 안 만든 것이다.
    @{ Name = 'coverage-all-covered'; Why = '규칙 3. 둘 다 있으면 전부 덮인다'; Expect = 'TCP:25565=covered ICMPv4:8=covered'
       Run = { Format-StateList (Get-EndpointCoverage @(
                   (New-RuleStub 'Any' 'sangtachi0' 'TCP' '25565'),
                   (New-RuleStub 'Any' 'sangtachi0' 'ICMPv4' $null '8')) 'sangtachi0' @((ConvertTo-RuleExpectation 'TCP:25565'), (ConvertTo-RuleExpectation 'ICMPv4:8'))) } }

    @{ Name = 'coverage-missing-icmp'; Why = '6라운드 blocker. 하나만 있어도 통과하던 자리'; Expect = 'TCP:25565=covered ICMPv4:8=missing'
       Run = { Format-StateList (Get-EndpointCoverage @(
                   (New-RuleStub 'Any' 'sangtachi0' 'TCP' '25565')) 'sangtachi0' @((ConvertTo-RuleExpectation 'TCP:25565'), (ConvertTo-RuleExpectation 'ICMPv4:8'))) } }

    @{ Name = 'coverage-missing-tcp'; Why = '반대쪽도 같다'; Expect = 'TCP:25565=missing ICMPv4:8=covered'
       Run = { Format-StateList (Get-EndpointCoverage @(
                   (New-RuleStub 'Any' 'sangtachi0' 'ICMPv4' $null '8')) 'sangtachi0' @((ConvertTo-RuleExpectation 'TCP:25565'), (ConvertTo-RuleExpectation 'ICMPv4:8'))) } }

    @{ Name = 'coverage-wide-rule-does-not-cover'; Why = '넓은 규칙은 덮은 것으로 치지 않는다'; Expect = 'TCP:25565=missing'
       Run = { Format-StateList (Get-EndpointCoverage @(
                   (New-RuleStub 'Any' 'Any' 'TCP' '25565')) 'sangtachi0' @((ConvertTo-RuleExpectation 'TCP:25565'))) } }

    @{ Name = 'coverage-other-adapter-does-not-cover'; Why = '엉뚱한 어댑터 규칙도 덮은 것이 아니다'; Expect = 'TCP:25565=missing'
       Run = { Format-StateList (Get-EndpointCoverage @(
                   (New-RuleStub 'Any' 'Wi-Fi' 'TCP' '25565')) 'sangtachi0' @((ConvertTo-RuleExpectation 'TCP:25565'))) } }

    @{ Name = 'coverage-unknown-rule'; Why = '판정 못 한 규칙이 있으면 "없다" 로 단정하지 않는다'; Expect = 'TCP:25565=unknown'
       Run = { Format-StateList (Get-EndpointCoverage @(
                   (New-RuleStub '' '' '' '')) 'sangtachi0' @((ConvertTo-RuleExpectation 'TCP:25565'))) } }

    @{ Name = 'coverage-unknown-plus-covered'; Why = '덮은 규칙이 따로 있으면 covered 다'; Expect = 'TCP:25565=covered'
       Run = { Format-StateList (Get-EndpointCoverage @(
                   (New-RuleStub '' '' '' ''),
                   (New-RuleStub 'Any' 'sangtachi0' 'TCP' '25565')) 'sangtachi0' @((ConvertTo-RuleExpectation 'TCP:25565'))) } }

    @{ Name = 'coverage-no-rules'; Why = '규칙이 없으면 전부 missing 이다. 0건 처리는 Get-RuleChecks 가 한다'; Expect = 'TCP:25565=missing ICMPv4:8=missing'
       Run = { Format-StateList (Get-EndpointCoverage @() 'sangtachi0' @((ConvertTo-RuleExpectation 'TCP:25565'), (ConvertTo-RuleExpectation 'ICMPv4:8'))) } }

    @{ Name = 'coverage-profile-dependent-not-covered'; Why = '분류 의존 규칙도 덮음으로 치지 않는다'; Expect = 'TCP:25565=missing'
       Run = { Format-StateList (Get-EndpointCoverage @(
                   (New-RuleStub 'Private' 'sangtachi0' 'TCP' '25565')) 'sangtachi0' @((ConvertTo-RuleExpectation 'TCP:25565'))) } }

    @{ Name = 'format-expectation-port'; Why = '표기'; Expect = 'TCP:25565'
       Run = { Format-Expectation (ConvertTo-RuleExpectation 'TCP:25565') } }

    @{ Name = 'format-expectation-icmp'; Why = '표기'; Expect = 'ICMPv4:8'
       Run = { Format-Expectation (ConvertTo-RuleExpectation 'ICMPv4:8') } }

    @{ Name = 'format-expectation-bare'; Why = '표기'; Expect = 'TCP'
       Run = { Format-Expectation (New-RuleExpectation 'TCP') } }

    @{ Name = 'format-state-list-reason'; Why = '조회 실패 사유가 출력에 남는다'; Expect = 'r=unknown(포트 필터 조회 실패)'
       Run = { Format-StateList @((New-StateStub 'r' 'unknown' '포트 필터 조회 실패')) } }

    # --- 판정 대상 고르기 (7라운드 blocker) ---------------------------------
    # 켜진 인바운드 허용 규칙만 판정한다. 뺀 규칙은 사유와 함께 남긴다.
    @{ Name = 'selection-normal'; Why = '규칙 3. 켜진 인바운드 허용 규칙은 대상이다'; Expect = '1/0'
       Run = { $s = Get-RuleSelection @((New-FirewallRuleStub 'r' 'Inbound' 'Allow' 'True'))
               '{0}/{1}' -f @($s.Candidates).Count, @($s.Excluded).Count } }

    @{ Name = 'selection-enum-enabled'; Why = '규칙 3. 값의 형은 bool 이 아니라 Enabled 열거형이다'; Expect = '1/0'
       Run = { $s = Get-RuleSelection @((New-FirewallRuleStub 'r' 'Inbound' 'Allow' ([System.Management.Automation.SwitchParameter]$true)))
               '{0}/{1}' -f @($s.Candidates).Count, @($s.Excluded).Count } }

    @{ Name = 'selection-disabled'; Why = '7라운드 blocker. 꺼진 규칙은 아무것도 열지 않는다'; Expect = '0/r=disabled'
       Run = { $s = Get-RuleSelection @((New-FirewallRuleStub 'r' 'Inbound' 'Allow' 'False'))
               '{0}/{1}' -f @($s.Candidates).Count, (Format-StateList $s.Excluded) } }

    @{ Name = 'selection-outbound'; Why = '같은 계열. 아웃바운드 규칙은 인바운드를 열지 않는다'; Expect = '0/r=outbound'
       Run = { $s = Get-RuleSelection @((New-FirewallRuleStub 'r' 'Outbound' 'Allow' 'True'))
               '{0}/{1}' -f @($s.Candidates).Count, (Format-StateList $s.Excluded) } }

    @{ Name = 'selection-block'; Why = '같은 계열. Block 규칙은 여는 규칙이 아니다'; Expect = '0/r=block'
       Run = { $s = Get-RuleSelection @((New-FirewallRuleStub 'r' 'Inbound' 'Block' 'True'))
               '{0}/{1}' -f @($s.Candidates).Count, (Format-StateList $s.Excluded) } }

    @{ Name = 'selection-enabled-missing'; Why = '켜졌다고 넘겨짚지 않는다'; Expect = '0/r=unreadable(Enabled=)'
       Run = { $s = Get-RuleSelection @((New-FirewallRuleStub 'r' 'Inbound' 'Allow' ''))
               '{0}/{1}' -f @($s.Candidates).Count, (Format-StateList $s.Excluded) } }

    @{ Name = 'selection-enabled-notconfigured'; Why = '프로필 때와 같은 함정. 모르면 모른다고 한다'; Expect = '0/r=unreadable(Enabled=NotConfigured)'
       Run = { $s = Get-RuleSelection @((New-FirewallRuleStub 'r' 'Inbound' 'Allow' 'NotConfigured'))
               '{0}/{1}' -f @($s.Candidates).Count, (Format-StateList $s.Excluded) } }

    @{ Name = 'selection-direction-missing'; Why = '방향을 못 읽으면 모른다'; Expect = '0/r=unreadable(Direction=)'
       Run = { $s = Get-RuleSelection @((New-FirewallRuleStub 'r' '' 'Allow' 'True'))
               '{0}/{1}' -f @($s.Candidates).Count, (Format-StateList $s.Excluded) } }

    @{ Name = 'selection-action-missing'; Why = '동작을 못 읽으면 모른다'; Expect = '0/r=unreadable(Action=)'
       Run = { $s = Get-RuleSelection @((New-FirewallRuleStub 'r' 'Inbound' '' 'True'))
               '{0}/{1}' -f @($s.Candidates).Count, (Format-StateList $s.Excluded) } }

    @{ Name = 'selection-mixed'; Why = '규칙 3. 켜진 것만 고르고 꺼진 것은 사유로 남긴다'; Expect = '1/off=disabled'
       Run = { $s = Get-RuleSelection @((New-FirewallRuleStub 'on' 'Inbound' 'Allow' 'True'), (New-FirewallRuleStub 'off' 'Inbound' 'Allow' 'False'))
               '{0}/{1}' -f @($s.Candidates).Count, (Format-StateList $s.Excluded) } }

    @{ Name = 'selection-empty'; Why = '입력이 없으면 둘 다 0'; Expect = '0/0'
       Run = { $s = Get-RuleSelection @()
               '{0}/{1}' -f @($s.Candidates).Count, @($s.Excluded).Count } }

    @{ Name = 'selection-null'; Why = '입력 가드'; Expect = '0/0'
       Run = { $s = Get-RuleSelection $null
               '{0}/{1}' -f @($s.Candidates).Count, @($s.Excluded).Count } }

    @{ Name = 'rule-checks-all-disabled'; Why = '7라운드 blocker. 우리 규칙이 전부 꺼진 기기가 pass 로 끝나면 안 된다'; Expect = 'fail/유효한 규칙 0건. 제외: r=disabled'
       Run = { $c = @(Get-RuleChecks (New-QueryResult -Items @(1)) 'sangtachi*' '기대' @() @() @() @((New-StateStub 'r' 'disabled')))[2]
               '{0}/{1}' -f $c.Result, $c.Actual } }

    @{ Name = 'rule-checks-all-outbound'; Why = '아웃바운드만 있어도 인바운드는 안 열린다'; Expect = 'fail'
       Run = { @(Get-RuleChecks (New-QueryResult -Items @(1)) 'sangtachi*' '기대' @() @() @() @((New-StateStub 'r' 'outbound')))[2].Result } }

    @{ Name = 'rule-checks-all-unreadable'; Why = '읽지 못한 규칙이 있으면 단정하지 않는다'; Expect = 'unknown'
       Run = { @(Get-RuleChecks (New-QueryResult -Items @(1)) 'sangtachi*' '기대' @() @() @() @((New-StateStub 'r' 'unreadable' 'Enabled=')))[2].Result } }

    @{ Name = 'rule-checks-disabled-reason-differs'; Why = '"0건" 과 "전부 꺼짐" 의 이유가 달라야 한다'; Expect = 'True'
       Run = { $a = @(Get-RuleChecks (New-QueryResult) 'sangtachi*' '기대' @() @() @() @())[2].Actual
               $b = @(Get-RuleChecks (New-QueryResult -Items @(1)) 'sangtachi*' '기대' @() @() @() @((New-StateStub 'r' 'disabled')))[2].Actual
               [string]($a -ne $b) } }

    @{ Name = 'rule-checks-excluded-shown'; Why = '대상이 있어도 뺀 규칙을 출력에 남긴다'; Expect = 'TCP:25565=covered | 제외: off=disabled'
       Run = { @(Get-RuleChecks (New-QueryResult -Items @(1, 2)) 'sangtachi*' '기대' @((New-StateStub 'r' 'scoped')) @((New-StateStub 'r' 'exists')) @((New-StateStub 'TCP:25565' 'covered')) @((New-StateStub 'off' 'disabled')))[2].Actual } }

    @{ Name = 'rule-checks-unreadable-softens-missing'; Why = '읽지 못한 규칙이 남아 있으면 missing 을 fail 로 단정하지 않는다'; Expect = 'unknown'
       Run = { @(Get-RuleChecks (New-QueryResult -Items @(1, 2)) 'sangtachi*' '기대' @((New-StateStub 'r' 'scoped')) @((New-StateStub 'r' 'exists')) @((New-StateStub 'ICMPv4:8' 'missing')) @((New-StateStub 'x' 'unreadable' 'Enabled=')))[2].Result } }

    @{ Name = 'rule-checks-missing-stays-fail'; Why = '규칙 3. 읽지 못한 규칙이 없으면 missing 은 fail 이다'; Expect = 'fail'
       Run = { @(Get-RuleChecks (New-QueryResult -Items @(1)) 'sangtachi*' '기대' @((New-StateStub 'r' 'scoped')) @((New-StateStub 'r' 'exists')) @((New-StateStub 'ICMPv4:8' 'missing')) @())[2].Result } }

    # --- 조회 결과 -> 검사 줄 (5라운드 blocker) -----------------------------
    # "없음" 과 "못 물어봤음" 이 같은 값이 되면 안 된다. 판정은 둘 다 unknown 이지만
    # 이유가 달라야 다음 행동이 갈린다.
    @{ Name = 'adapter-check-normal'; Why = '규칙 3. 어댑터가 있으면 pass'; Expect = 'pass/NetworkCategory=Public'
       Run = { $c = Get-AdapterCheck (New-QueryResult -Items @([pscustomobject]@{ NetworkCategory = 'Public' })) 'sangtachi0'
               '{0}/{1}' -f $c.Result, $c.Actual } }

    @{ Name = 'adapter-check-not-found'; Why = '규칙 3. Phase 6 전에는 어댑터가 없는 것이 정상이다'; Expect = 'unknown/어댑터 없음: sangtachi0'
       Run = { $c = Get-AdapterCheck (New-QueryResult) 'sangtachi0'
               '{0}/{1}' -f $c.Result, $c.Actual } }

    @{ Name = 'adapter-check-query-failed'; Why = '5라운드 blocker. 조회 실패를 "없음" 으로 적지 않는다'; Expect = 'unknown/조회 실패: 액세스가 거부되었습니다'
       Run = { $c = Get-AdapterCheck (New-QueryResult -Failed $true -Message '액세스가 거부되었습니다') 'sangtachi0'
               '{0}/{1}' -f $c.Result, $c.Actual } }

    @{ Name = 'adapter-check-reasons-differ'; Why = '5라운드 blocker. 두 경우의 이유가 서로 달라야 한다'; Expect = 'True'
       Run = { $a = (Get-AdapterCheck (New-QueryResult) 'sangtachi0').Actual
               $b = (Get-AdapterCheck (New-QueryResult -Failed $true -Message '액세스가 거부되었습니다') 'sangtachi0').Actual
               [string]($a -ne $b) } }

    @{ Name = 'adapter-check-null-query'; Why = '조회 자체를 안 했으면 모른다'; Expect = 'unknown/조회 없음'
       Run = { $c = Get-AdapterCheck $null 'sangtachi0'
               '{0}/{1}' -f $c.Result, $c.Actual } }

    @{ Name = 'rule-checks-none'; Why = '규칙 3. 우리 규칙이 아직 없는 것은 정상이다'; Expect = 'unknown/sangtachi* 인바운드 허용 규칙 0건'
       Run = { $c = @(Get-RuleChecks (New-QueryResult) 'sangtachi*' '기대' @() @() @() @())[0]
               '{0}/{1}' -f $c.Result, $c.Actual } }

    @{ Name = 'rule-checks-query-failed'; Why = '5라운드 blocker. 방화벽 조회 실패가 "0건" 이 되면 안 된다'; Expect = 'unknown/규칙 조회 실패: 액세스가 거부되었습니다'
       Run = { $c = @(Get-RuleChecks (New-QueryResult -Failed $true -Message '액세스가 거부되었습니다') 'sangtachi*' '기대' @() @() @() @())[0]
               '{0}/{1}' -f $c.Result, $c.Actual } }

    @{ Name = 'rule-checks-reasons-differ'; Why = '5라운드 blocker. 0건과 조회 실패의 이유가 달라야 한다'; Expect = 'True'
       Run = { $a = @(Get-RuleChecks (New-QueryResult) 'sangtachi*' '기대' @() @() @() @())[0].Actual
               $b = @(Get-RuleChecks (New-QueryResult -Failed $true -Message 'x') 'sangtachi*' '기대' @() @() @() @())[0].Actual
               [string]($a -ne $b) } }

    @{ Name = 'rule-checks-failure-hits-all'; Why = '조회 실패면 세 검사 다 unknown 이다'; Expect = 'unknown,unknown,unknown'
       Run = { (@(Get-RuleChecks (New-QueryResult -Failed $true -Message 'x') 'sangtachi*' '기대' @() @() @() @()) | ForEach-Object { $_.Result }) -join ',' } }

    @{ Name = 'rule-checks-null-query'; Why = '조회 자체를 안 했으면 모른다'; Expect = 'unknown/조회 없음'
       Run = { $c = @(Get-RuleChecks $null 'sangtachi*' '기대' @() @() @() @())[0]
               '{0}/{1}' -f $c.Result, $c.Actual } }

    @{ Name = 'rule-checks-scoped'; Why = '규칙 3. 좁혀진 규칙, 살아 있는 경로, 전부 덮으면 pass'; Expect = 'pass,pass,pass'
       Run = { (@(Get-RuleChecks (New-QueryResult -Items @(1)) 'sangtachi*' '기대' @((New-StateStub 'r' 'scoped')) @((New-StateStub 'r' 'exists')) @((New-StateStub 'TCP:25565' 'covered')) @()) | ForEach-Object { $_.Result }) -join ',' } }

    @{ Name = 'rule-checks-wide'; Why = '넓은 규칙은 fail 로 올라간다'; Expect = 'fail,pass,pass'
       Run = { (@(Get-RuleChecks (New-QueryResult -Items @(1)) 'sangtachi*' '기대' @((New-StateStub 'r' 'wide')) @((New-StateStub 'r' 'exists')) @((New-StateStub 'TCP:25565' 'covered')) @()) | ForEach-Object { $_.Result }) -join ',' } }

    @{ Name = 'rule-checks-program-missing'; Why = '죽은 경로는 fail 로 올라간다'; Expect = 'pass,fail,pass'
       Run = { (@(Get-RuleChecks (New-QueryResult -Items @(1)) 'sangtachi*' '기대' @((New-StateStub 'r' 'scoped')) @((New-StateStub 'r' 'missing')) @((New-StateStub 'TCP:25565' 'covered')) @()) | ForEach-Object { $_.Result }) -join ',' } }

    @{ Name = 'rule-checks-program-query-failed'; Why = '6라운드 blocker. 프로그램 필터 조회 실패는 통과가 아니다'; Expect = 'unknown/r=unknown(응용프로그램 필터 조회 실패)'
       Run = { $c = @(Get-RuleChecks (New-QueryResult -Items @(1)) 'sangtachi*' '기대' @((New-StateStub 'r' 'scoped')) @((New-StateStub 'r' 'unknown' '응용프로그램 필터 조회 실패')) @((New-StateStub 'TCP:25565' 'covered')) @())[1]
               '{0}/{1}' -f $c.Result, $c.Actual } }

    @{ Name = 'rule-checks-coverage-missing'; Why = '6라운드 blocker. 기대 하나가 안 열렸으면 fail'; Expect = 'fail/TCP:25565=covered ICMPv4:8=missing'
       Run = { $c = @(Get-RuleChecks (New-QueryResult -Items @(1)) 'sangtachi*' '기대' @((New-StateStub 'r' 'scoped')) @((New-StateStub 'r' 'exists')) @((New-StateStub 'TCP:25565' 'covered'), (New-StateStub 'ICMPv4:8' 'missing')) @())[2]
               '{0}/{1}' -f $c.Result, $c.Actual } }

    @{ Name = 'rule-checks-coverage-unknown'; Why = '판정 못 한 규칙이 있으면 덮음도 모른다'; Expect = 'unknown'
       Run = { @(Get-RuleChecks (New-QueryResult -Items @(1)) 'sangtachi*' '기대' @((New-StateStub 'r' 'unknown')) @((New-StateStub 'r' 'exists')) @((New-StateStub 'TCP:25565' 'unknown')) @())[2].Result } }

    @{ Name = 'rule-checks-count'; Why = '검사 세 줄을 늘 낸다'; Expect = '3'
       Run = { [string]@(Get-RuleChecks (New-QueryResult) 'sangtachi*' '기대' @() @() @() @()).Count } }

    # --- 규칙별 필터 조회 실패 (6라운드 blocker) ----------------------------
    # 조회 실패를 빈 값으로 바꾸면 그 값이 판정을 통과시킨다.
    @{ Name = 'filter-states-normal'; Why = '규칙 3. 셋 다 읽히면 정상 판정'; Expect = 'scoped/exists'
       Run = { $s = Get-RuleFilterStates (New-RuleStub 'Any' '' '' '') `
                   (New-QueryResult -Items @([pscustomobject]@{ InterfaceAlias = 'sangtachi0' })) `
                   (New-QueryResult -Items @([pscustomobject]@{ Protocol = 'TCP'; LocalPort = '25565'; IcmpType = '' })) `
                   (New-QueryResult -Items @([pscustomobject]@{ Program = '%SystemRoot%\explorer.exe' })) `
                   'sangtachi0' @((ConvertTo-RuleExpectation 'TCP:25565'))
               '{0}/{1}' -f $s.Scope, $s.Program } }

    @{ Name = 'filter-states-app-failed'; Why = '6라운드 blocker. 빈 값으로 바꾸면 any 가 되어 통과한다'; Expect = 'scoped/unknown/응용프로그램 필터 조회 실패'
       Run = { $s = Get-RuleFilterStates (New-RuleStub 'Any' '' '' '') `
                   (New-QueryResult -Items @([pscustomobject]@{ InterfaceAlias = 'sangtachi0' })) `
                   (New-QueryResult -Items @([pscustomobject]@{ Protocol = 'TCP'; LocalPort = '25565'; IcmpType = '' })) `
                   (New-QueryResult -Failed $true -Message 'x') `
                   'sangtachi0' @((ConvertTo-RuleExpectation 'TCP:25565'))
               '{0}/{1}/{2}' -f $s.Scope, $s.Program, $s.ProgramReason } }

    @{ Name = 'naive-app-failure-counterexample'; Why = '6라운드 blocker 반례 고정. 빈 문자열은 any 를 낸다'; Expect = 'any'
       Run = { Get-RuleProgramState '' } }

    @{ Name = 'filter-states-alias-failed'; Why = '인터페이스 필터 실패는 범위를 모른다는 뜻이다'; Expect = 'unknown/인터페이스 필터 조회 실패'
       Run = { $s = Get-RuleFilterStates (New-RuleStub 'Any' '' '' '') `
                   (New-QueryResult -Failed $true -Message 'x') `
                   (New-QueryResult -Items @([pscustomobject]@{ Protocol = 'TCP'; LocalPort = '25565'; IcmpType = '' })) `
                   (New-QueryResult -Items @([pscustomobject]@{ Program = 'Any' })) `
                   'sangtachi0' @((ConvertTo-RuleExpectation 'TCP:25565'))
               '{0}/{1}' -f $s.Scope, $s.ScopeReason } }

    @{ Name = 'filter-states-port-failed'; Why = '포트 필터 실패도 범위를 모른다'; Expect = 'unknown/포트 필터 조회 실패'
       Run = { $s = Get-RuleFilterStates (New-RuleStub 'Any' '' '' '') `
                   (New-QueryResult -Items @([pscustomobject]@{ InterfaceAlias = 'sangtachi0' })) `
                   (New-QueryResult -Failed $true -Message 'x') `
                   (New-QueryResult -Items @([pscustomobject]@{ Program = 'Any' })) `
                   'sangtachi0' @((ConvertTo-RuleExpectation 'TCP:25565'))
               '{0}/{1}' -f $s.Scope, $s.ScopeReason } }

    @{ Name = 'filter-states-all-failed'; Why = '셋 다 실패하면 둘 다 unknown'; Expect = 'unknown/unknown'
       Run = { $s = Get-RuleFilterStates (New-RuleStub 'Any' '' '' '') `
                   (New-QueryResult -Failed $true -Message 'x') (New-QueryResult -Failed $true -Message 'x') (New-QueryResult -Failed $true -Message 'x') `
                   'sangtachi0' @((ConvertTo-RuleExpectation 'TCP:25565'))
               '{0}/{1}' -f $s.Scope, $s.Program } }

    @{ Name = 'filter-states-app-null'; Why = '조회를 아예 안 했어도 통과가 아니다'; Expect = 'unknown'
       Run = { (Get-RuleFilterStates (New-RuleStub 'Any' '' '' '') `
                   (New-QueryResult -Items @([pscustomobject]@{ InterfaceAlias = 'sangtachi0' })) `
                   (New-QueryResult -Items @([pscustomobject]@{ Protocol = 'TCP'; LocalPort = '25565'; IcmpType = '' })) `
                   $null 'sangtachi0' @((ConvertTo-RuleExpectation 'TCP:25565'))).Program } }

    # --- 조회 목록 꺼내기 ----------------------------------------------------
    @{ Name = 'query-items-ok'; Why = '규칙 3. 정상 결과는 목록으로'; Expect = '2'
       Run = { $r = Get-QueryItemsOrNull (New-QueryResult -Items @(1, 2)); [string]@($r).Count } }

    @{ Name = 'query-items-single'; Why = '1건도 목록이다'; Expect = '1'
       Run = { $r = Get-QueryItemsOrNull (New-QueryResult -Items @('x')); [string]@($r).Count } }

    @{ Name = 'query-items-empty'; Why = '빈 결과는 빈 목록이다. $null 이 아니다'; Expect = '0'
       Run = { $r = Get-QueryItemsOrNull (New-QueryResult); if ($null -eq $r) { 'null' } else { [string]@($r).Count } } }

    @{ Name = 'query-items-failed'; Why = '실패는 $null 이다. 빈 목록으로 바꾸지 않는다'; Expect = 'null'
       Run = { $r = Get-QueryItemsOrNull (New-QueryResult -Failed $true -Message 'x'); if ($null -eq $r) { 'null' } else { [string]@($r).Count } } }

    @{ Name = 'query-items-null'; Why = '조회 자체가 없으면 $null'; Expect = 'null'
       Run = { $r = Get-QueryItemsOrNull $null; if ($null -eq $r) { 'null' } else { [string]@($r).Count } } }

    @{ Name = 'verdict-scope-param-from-failed-query'; Why = '조회 실패가 판정까지 올라간다'; Expect = 'unknown'
       Run = { (Get-FirewallPolicyVerdict -PolicyText (New-NetshSample 'BlockInbound,AllowOutbound' 'BlockInbound,AllowOutbound' 'BlockInbound,AllowOutbound') -Profiles (New-ProfileSet 'True' 'True' 'True') -ScopeParameterSupport (Get-ScopeParameterSupport (Get-QueryItemsOrNull (New-QueryResult -Failed $true -Message 'x')))).Verdict } }

    @{ Name = 'verdict-profiles-from-failed-query'; Why = '프로필 조회 실패도 판정까지 올라간다'; Expect = 'unknown'
       Run = { (Get-FirewallPolicyVerdict -PolicyText (New-NetshSample 'BlockInbound,AllowOutbound' 'BlockInbound,AllowOutbound' 'BlockInbound,AllowOutbound') -Profiles (Get-QueryItemsOrNull (New-QueryResult -Failed $true -Message 'x')) -ScopeParameterSupport 'True').Verdict } }

    # --- 조회 실패와 "없음" 구분 --------------------------------------------
    # 값은 'Failed/건수' 로 찍는다. 둘을 한 줄에 두면 실패 출력만 보고 구분된다.
    @{ Name = 'query-ok'; Why = '규칙 3. 정상 조회는 그대로 돌아온다'; Expect = 'False/2'
       Run = { $q = Invoke-QuerySafe { 1, 2 }; '{0}/{1}' -f $q.Failed, $q.Items.Count } }

    @{ Name = 'query-single-item'; Why = '규칙 3. 1건도 목록으로 받는다'; Expect = 'False/1'
       Run = { $q = Invoke-QuerySafe { 'x' }; '{0}/{1}' -f $q.Failed, $q.Items.Count } }

    @{ Name = 'query-empty-result'; Why = '빈 결과를 조회 실패로 읽지 않는다 (빈 배열 풀림 함정)'; Expect = 'False/0'
       Run = { $q = Invoke-QuerySafe { @() }; '{0}/{1}' -f $q.Failed, $q.Items.Count } }

    @{ Name = 'query-notfound-is-empty'; Why = '대상이 없는 것은 실패가 아니다'; Expect = 'False/0'
       Run = { $q = Invoke-QuerySafe { Write-Error -Message '없다' -ErrorId 'CmdletizationQuery_NotFound_InterfaceAlias' -ErrorAction Stop }
               '{0}/{1}' -f $q.Failed, $q.Items.Count } }

    @{ Name = 'query-error-is-failure'; Why = '권한 부족 같은 실패를 "없음" 으로 읽지 않는다'; Expect = 'True/0'
       Run = { $q = Invoke-QuerySafe { throw '접근 거부' }; '{0}/{1}' -f $q.Failed, $q.Items.Count } }
)

# ---------------------------------------------------------------------------
# 시험용 입력 만들기. 케이스 표만 쓴다.
# ---------------------------------------------------------------------------

function New-ProfileStub {
    param([string]$Name, $Enabled, [string]$InboundAction = 'NotConfigured')
    [pscustomobject]@{ Name = $Name; Enabled = $Enabled; DefaultInboundAction = $InboundAction }
}

function New-ProfileSet {
    param([string]$Domain, [string]$Private, [string]$Public, [string]$InboundAction = 'NotConfigured')
    @(
        (New-ProfileStub 'Domain'  $Domain  $InboundAction),
        (New-ProfileStub 'Private' $Private $InboundAction),
        (New-ProfileStub 'Public'  $Public  $InboundAction)
    )
}

function New-RuleStub {
    param([string]$Profile, $InterfaceAlias, $Protocol, $LocalPort, $IcmpType = $null)
    [pscustomobject]@{
        DisplayName    = 'stub'
        Profile        = $Profile
        InterfaceAlias = $InterfaceAlias
        Protocol       = $Protocol
        LocalPort      = $LocalPort
        IcmpType       = $IcmpType
    }
}

function New-StateStub {
    param([string]$Name, [string]$State, [string]$Reason = '')
    [pscustomobject]@{ Name = $Name; State = $State; Reason = $Reason }
}

function New-NetshSample {
    # `netsh advfirewall show allprofiles firewallpolicy` 출력 모양. 레이블은 번역될 수
    # 있으므로 인자로 받는다.
    param([string]$Domain, [string]$Private, [string]$Public, [string]$Label = 'Firewall Policy')
    $line = { param($value) ('{0}                       {1}' -f $Label, $value) }
    @(
        '',
        'Domain Profile Settings: ',
        '----------------------------------------------------------------------',
        (& $line $Domain),
        '',
        'Private Profile Settings: ',
        '----------------------------------------------------------------------',
        (& $line $Private),
        '',
        'Public Profile Settings: ',
        '----------------------------------------------------------------------',
        (& $line $Public),
        'Ok.',
        ''
    )
}

# ---------------------------------------------------------------------------
# 판정 함수. 하나씩 케이스 표에 걸려 있다.
# ---------------------------------------------------------------------------

# 값 전체를 줄 끝에 붙여 맞춘다. 부분 문자열로 보면 `BlockInboundAlways` 가 통과한다.
# 앞은 `(^|\s)` 다. 줄 머리에 값만 있는 줄과 탭 구분 줄을 둘 다 받는다.
$script:RE_POLICY_OK     = '(^|\s)BlockInbound,AllowOutbound\s*$'
$script:RE_POLICY_ALWAYS = '(^|\s)BlockInboundAlways,\S+\s*$'
# 정책 값 줄이 몇 개인지 세는 데만 쓴다. 값의 옳고 그름은 위 둘이 판정한다.
$script:RE_POLICY_ANY    = '(^|\s)(Block|Allow)Inbound(Always)?,(Allow|Block)Outbound\s*$'

function Test-PolicyValueLine {
    # 이 줄이 기대하는 정책 값(`BlockInbound,AllowOutbound`)을 말하는가.
    # 레이블은 보지 않는다. 표시 언어에 따라 번역되기 때문이다 (R8).
    param([string]$Line)
    if ([string]::IsNullOrEmpty($Line)) { return $false }
    return [bool]($Line -match $script:RE_POLICY_OK)
}

function Test-AlwaysModeLine {
    # `BlockInboundAlways` 는 인바운드 허용 규칙을 전부 무시하는 모드다 (R9).
    param([string]$Line)
    if ([string]::IsNullOrEmpty($Line)) { return $false }
    return [bool]($Line -match $script:RE_POLICY_ALWAYS)
}

function Get-PolicyLineCounts {
    param([string[]]$Lines)
    $value = 0; $ok = 0; $always = 0
    foreach ($line in @($Lines)) {
        if ($null -eq $line) { continue }
        if ($line -match $script:RE_POLICY_ANY) { $value++ }
        if (Test-PolicyValueLine $line) { $ok++ }
        if (Test-AlwaysModeLine $line) { $always++ }
    }
    [pscustomobject]@{ Value = $value; Expected = $ok; Always = $always }
}

function Get-ProfileStateVerdict {
    # 프로필 방화벽이 꺼져 있으면 정책 값이 의미 없다 (R13).
    # `Enabled` 는 이 기기에서 bool 이 아니라 GpoBoolean 이므로 이름 문자열로 본다.
    # `-eq $false` 단축 구문은 `NotConfigured` 를 0건으로 세어 조용히 통과시킨다 (R16).
    param($Profiles)
    if ($null -eq $Profiles) { return 'unknown:조회 없음' }
    $list = @($Profiles)

    $disabled = @()
    $unknown = @()
    foreach ($item in $list) {
        $name = [string]$item.Name
        $state = [string]$item.Enabled
        if ($state -eq 'False') { $disabled += $name }
        elseif ($state -ne 'True') { $unknown += ('{0}={1}' -f $name, $state) }
    }
    # 확인된 결함을 먼저 낸다. 모르는 항목이 그것을 덮지 않는다.
    # 출력은 이름순으로 고정한다. 조회 순서가 바뀌어도 같은 문장이 나와야 한다.
    if ($disabled.Count -gt 0) { return 'fail:' + (@($disabled | Sort-Object) -join ',') }
    if ($list.Count -ne 3) { return 'unknown:프로필 {0}개' -f $list.Count }

    # **순서가 아니라 집합으로 본다.** 조회가 Public 을 먼저 주는 기기도 정상이다
    # (8라운드 warn, 재발 방지 규칙 3). 중복 이름은 정렬해도 안 맞으므로 그대로 걸린다.
    $names = @($list | ForEach-Object { [string]$_.Name })
    if ((@($names | Sort-Object) -join ',') -ne 'Domain,Private,Public') {
        return 'unknown:프로필 이름 ' + ($names -join ',')
    }
    if ($unknown.Count -gt 0) { return 'unknown:' + (@($unknown | Sort-Object) -join ',') }
    return 'ok'
}

function Get-InboundActionMeaning {
    # 참고용이다. 판정에 쓰지 않는다.
    # `NotConfigured` 는 "설정 안 됨" 이 아니라 "내장 기본값을 쓴다" 이고 그 기본값이
    # Block 이다. `-eq 'Block'` 으로 판정하면 정상 기기가 떨어진다 (재발 방지 규칙 3).
    param([string]$Action)
    switch ($Action) {
        'Block'         { return 'block' }
        'NotConfigured' { return 'block' }
        'Allow'         { return 'allow' }
        default         { return 'unknown' }
    }
}

function Get-RuleProgramState {
    # `%ProgramFiles%` 형태 경로를 풀지 않고 `Test-Path` 하면 멀쩡한 규칙이 죽은 것으로
    # 보인다 (R19).
    #
    # 푼 뒤에도 절대 경로가 아니면 판정하지 않는다. Windows 기본 규칙에는 `System` 같은
    # 특수 토큰이 들어 있고, 상대 경로를 그대로 `Test-Path` 하면 현재 디렉터리에 따라
    # 답이 바뀐다. 이 기기의 `Core Networking` 규칙을 돌려서 실제로 만난 자리다.
    param([string]$Program)
    if ([string]::IsNullOrWhiteSpace($Program)) { return 'any' }
    if ($Program -eq 'Any') { return 'any' }
    $expanded = [Environment]::ExpandEnvironmentVariables($Program)
    if (-not [System.IO.Path]::IsPathRooted($expanded)) { return 'unknown' }
    if (Test-Path -LiteralPath $expanded) { return 'exists' }
    return 'missing'
}

function ConvertTo-ProtocolName {
    # 프로토콜은 이름으로도 번호로도 온다. 번호를 이름으로 맞춰 두지 않으면 정상 규칙이
    # "다른 프로토콜" 로 떨어진다 (규칙 3). 모르는 번호는 그대로 돌린다.
    param([string]$Protocol)
    if ([string]::IsNullOrWhiteSpace($Protocol)) { return '' }
    $value = $Protocol.Trim()
    switch ($value) {
        '1'  { return 'ICMPv4' }
        '6'  { return 'TCP' }
        '17' { return 'UDP' }
        '58' { return 'ICMPv6' }
    }
    switch ($value.ToUpperInvariant()) {
        'TCP'    { return 'TCP' }
        'UDP'    { return 'UDP' }
        'ICMPV4' { return 'ICMPv4' }
        'ICMPV6' { return 'ICMPv6' }
        'ANY'    { return 'Any' }
    }
    return $value
}

function ConvertTo-PortRangeSet {
    # 포트 표기를 정규형 구간 목록으로 바꾼다.
    #
    # 문자 그대로 `Any` 만 거르면 `1-65535` 나 콤마 목록처럼 **사실상 전 포트를 여는
    # 규칙이 "좁혀졌다" 로 통과한다** (2라운드 warn). 폭을 숫자로 계산한다.
    #
    # 값은 'any', 'empty', 'unknown', 또는 'lo-hi[,lo-hi...]' 다.
    # 겹치거나 맞닿은 구간은 합친다. `25565,25566` 과 `25565-25566` 은 같은 폭이다.
    param([string]$Port)
    if ([string]::IsNullOrWhiteSpace($Port)) { return 'empty' }
    $tokens = @($Port -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne '' })
    if ($tokens.Count -eq 0) { return 'empty' }
    if (@($tokens | Where-Object { $_ -eq 'Any' }).Count -gt 0) { return 'any' }

    $ranges = @()
    foreach ($token in $tokens) {
        $low = 0
        $high = 0
        if ($token -match '^(\d{1,5})$') {
            $low = [int]$matches[1]
            $high = $low
        }
        elseif ($token -match '^(\d{1,5})-(\d{1,5})$') {
            $low = [int]$matches[1]
            $high = [int]$matches[2]
        }
        else {
            # `RPC`, `IPHTTPS` 같은 동적 포트 키워드다. 폭을 모른다.
            return 'unknown'
        }
        if ($low -lt 1 -or $high -gt 65535 -or $low -gt $high) { return 'unknown' }
        $ranges += , @($low, $high)
    }

    $sorted = @($ranges | Sort-Object -Property @{ Expression = { $_[0] } }, @{ Expression = { $_[1] } })
    $merged = @()
    foreach ($range in $sorted) {
        if ($merged.Count -eq 0) { $merged += , @($range[0], $range[1]); continue }
        $last = $merged[$merged.Count - 1]
        if ($range[0] -le $last[1] + 1) {
            if ($range[1] -gt $last[1]) { $last[1] = $range[1] }
        }
        else { $merged += , @($range[0], $range[1]) }
    }
    return (@($merged | ForEach-Object { '{0}-{1}' -f $_[0], $_[1] }) -join ',')
}

function Test-PortRangeCovers {
    # 정규형 구간 문자열 $Outer 가 $Inner 를 전부 담는가.
    param([string]$Outer, [string]$Inner)
    $parse = {
        param($text)
        @($text -split ',' | ForEach-Object {
            $part = $_.Split('-')
            , @([int]$part[0], [int]$part[1])
        })
    }
    $outerRanges = @(& $parse $Outer)
    foreach ($range in @(& $parse $Inner)) {
        $covered = $false
        foreach ($candidate in $outerRanges) {
            if ($candidate[0] -le $range[0] -and $candidate[1] -ge $range[1]) { $covered = $true; break }
        }
        if (-not $covered) { return $false }
    }
    return $true
}

function ConvertTo-IcmpTypeSet {
    # ICMP 종류 표기를 정규형 집합으로 바꾼다.
    #
    # 프로토콜만 맞으면 통과시키면 안 된다. **어댑터로 좁혀진 ICMPv4 규칙이 모든 종류를
    # 허용해도 scoped 로 보고됐다** (3라운드 blocker). 문서 2절이 쓰는 규칙은 에코 요청
    # (type 8) 전용이다.
    #
    # 이 기기의 실제 값은 `8`, `3:4`, `Any` 다. `8` 은 종류 8 의 **모든 코드**이므로
    # `8:*` 로 적는다. 값은 'any', 'empty', 'unknown', 또는 'T:C[,T:C...]' 다.
    param([string]$IcmpType)
    if ([string]::IsNullOrWhiteSpace($IcmpType)) { return 'empty' }
    $tokens = @($IcmpType -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne '' })
    if ($tokens.Count -eq 0) { return 'empty' }
    if (@($tokens | Where-Object { $_ -eq 'Any' }).Count -gt 0) { return 'any' }

    $pairs = @()
    foreach ($token in $tokens) {
        $type = -1
        $code = -1
        if ($token -match '^(\d{1,3})$') {
            $type = [int]$matches[1]
        }
        elseif ($token -match '^(\d{1,3}):(\d{1,3})$') {
            $type = [int]$matches[1]
            $code = [int]$matches[2]
        }
        else { return 'unknown' }
        if ($type -lt 0 -or $type -gt 255 -or $code -gt 255) { return 'unknown' }
        $pairs += , @($type, $code)
    }
    # 숫자로 정렬한다. 문자열로 정렬하면 `10` 이 `8` 앞에 온다.
    $sorted = @($pairs | Sort-Object -Property @{ Expression = { $_[0] } }, @{ Expression = { $_[1] } })
    $seen = @{}
    $out = @()
    foreach ($pair in $sorted) {
        $text = '{0}:{1}' -f $pair[0], $(if ($pair[1] -lt 0) { '*' } else { $pair[1] })
        if ($seen.ContainsKey($text)) { continue }
        $seen[$text] = $true
        $out += $text
    }
    return ($out -join ',')
}

function Test-IcmpTypeCovers {
    # 정규형 집합 $Outer 가 $Inner 를 전부 담는가. `8:*` 는 `8:0` 을 담는다.
    param([string]$Outer, [string]$Inner)
    $outerTokens = @($Outer -split ',')
    foreach ($token in @($Inner -split ',')) {
        $want = $token.Split(':')
        $covered = $false
        foreach ($candidate in $outerTokens) {
            $have = $candidate.Split(':')
            if ($have[0] -ne $want[0]) { continue }
            if ($have[1] -eq '*' -or $have[1] -eq $want[1]) { $covered = $true; break }
        }
        if (-not $covered) { return $false }
    }
    return $true
}

function New-RuleExpectation {
    # 우리가 열려고 하는 엔드포인트. 판정은 이 집합과 대조한다.
    # ICMP 에는 포트가 없고 대신 종류가 있다. 그래서 칸을 따로 둔다.
    param([string]$Protocol, [string]$Port = '', [string]$IcmpType = '')
    [pscustomobject]@{ Protocol = (ConvertTo-ProtocolName $Protocol); Port = $Port; IcmpType = $IcmpType }
}

function ConvertTo-RuleExpectation {
    # `TCP:25565`, `ICMPv4:8`, `ICMPv4:3:4` 같은 문자열을 기대 집합 하나로 바꾼다.
    # ICMP 면 뒤쪽을 포트가 아니라 **종류**로 읽는다.
    param([string]$Text)
    if ([string]::IsNullOrWhiteSpace($Text)) { return $null }
    $parts = $Text.Split(':')
    $protocol = ConvertTo-ProtocolName $parts[0]
    $rest = ''
    if ($parts.Count -gt 1) { $rest = ($parts[1..($parts.Count - 1)] -join ':') }
    if ($protocol -eq 'ICMPv4' -or $protocol -eq 'ICMPv6') {
        return New-RuleExpectation -Protocol $protocol -IcmpType $rest
    }
    return New-RuleExpectation -Protocol $protocol -Port $rest
}

function Get-RuleScopeVerdict {
    # 규칙이 우리가 의도한 만큼만 열려 있는가.
    #
    # `Any` 가 아니기만 하면 통과시키면 안 된다. **엉뚱한 어댑터에 묶인 규칙도 통과한다**
    # (2라운드 blocker). 별칭을 기대값과 대조한다.
    #
    # `-Profile Any` 만으로는 부족하다. 그것은 물리 네트워크에서도 열린다는 뜻이다 (R24).
    # 반대로 프로필을 좁히면 어댑터 분류에 의존하게 되고, 분류 변경은 식별되지 않은
    # 네트워크에서 실패할 수 있다 (R13). 그래서 둘을 다른 값으로 낸다.
    # **세 프로필을 전부 나열한 규칙은 `Any` 와 폭이 같다.** 그것을 분류 의존으로 읽으면
    # 정상 기기가 떨어진다 (2라운드 warn, 규칙 3).
    #
    # 값은 여덟이다.
    # scoped / wide / other-adapter / other-protocol / other-port / other-icmp-type /
    # profile-dependent / unknown
    param($Rule, [string]$ExpectedAlias, $Expectations)
    if ($null -eq $Rule) { return 'unknown' }

    $profileScope = [string]$Rule.Profile
    $alias = [string]$Rule.InterfaceAlias
    $protocol = ConvertTo-ProtocolName ([string]$Rule.Protocol)
    $port = [string]$Rule.LocalPort

    if ([string]::IsNullOrWhiteSpace($profileScope)) { return 'unknown' }
    if ([string]::IsNullOrWhiteSpace($alias)) { return 'unknown' }
    if ([string]::IsNullOrWhiteSpace($protocol)) { return 'unknown' }
    # 기대 집합이 없으면 판정하지 않는다. 무엇과 대조할지 모른다.
    if ([string]::IsNullOrWhiteSpace($ExpectedAlias)) { return 'unknown' }
    $wanted = @($Expectations | Where-Object { $null -ne $_ })
    if ($wanted.Count -eq 0) { return 'unknown' }

    # --- 어댑터 ---
    $aliases = @($alias -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne '' })
    if (@($aliases | Where-Object { $_ -eq 'Any' }).Count -gt 0) { return 'wide' }
    # 별칭 비교는 대소문자를 가리지 않는다. Windows 어댑터 이름이 그렇다.
    if (@($aliases | Where-Object { $_ -eq $ExpectedAlias.Trim() }).Count -eq 0) { return 'other-adapter' }
    if ($aliases.Count -gt 1) { return 'wide' }

    # --- 프로토콜 ---
    if ($protocol -eq 'Any') { return 'wide' }
    $matched = @($wanted | Where-Object { $_.Protocol -eq $protocol })
    if ($matched.Count -eq 0) { return 'other-protocol' }

    # --- ICMP 종류 ---
    # ICMP 에는 포트가 없다. 대신 **종류**가 범위를 정한다. 종류를 안 보면 모든 ICMP 를
    # 허용하는 규칙이 scoped 로 나온다 (3라운드 blocker).
    if ($protocol -eq 'ICMPv4' -or $protocol -eq 'ICMPv6') {
        $ruleIcmp = ConvertTo-IcmpTypeSet ([string]$Rule.IcmpType)
        if ($ruleIcmp -eq 'any') { return 'wide' }
        if ($ruleIcmp -eq 'empty' -or $ruleIcmp -eq 'unknown') { return 'unknown' }

        $bestIcmp = 'other-icmp-type'
        foreach ($expectation in $matched) {
            $wantIcmp = ConvertTo-IcmpTypeSet ([string]$expectation.IcmpType)
            # 기대 집합이 종류를 안 적었으면 무엇과 대조할지 모른다.
            if ($wantIcmp -eq 'any' -or $wantIcmp -eq 'empty' -or $wantIcmp -eq 'unknown') { return 'unknown' }
            if ($ruleIcmp -eq $wantIcmp) { $bestIcmp = 'ok'; break }
            if (Test-IcmpTypeCovers $ruleIcmp $wantIcmp) { $bestIcmp = 'wide' }
        }
        if ($bestIcmp -ne 'ok') { return $bestIcmp }
    }

    # --- 포트 ---
    else {
        $ruleSet = ConvertTo-PortRangeSet $port
        if ($ruleSet -eq 'any') { return 'wide' }
        if ($ruleSet -eq 'empty' -or $ruleSet -eq 'unknown') { return 'unknown' }

        $best = 'other-port'
        foreach ($expectation in $matched) {
            $wantSet = ConvertTo-PortRangeSet ([string]$expectation.Port)
            if ($wantSet -eq 'any' -or $wantSet -eq 'empty' -or $wantSet -eq 'unknown') { return 'unknown' }
            if ($ruleSet -eq $wantSet) { $best = 'ok'; break }
            if (Test-PortRangeCovers $ruleSet $wantSet) { $best = 'wide' }
        }
        if ($best -ne 'ok') { return $best }
    }

    # --- 프로필 ---
    $profiles = @($profileScope -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne '' })
    if (@($profiles | Where-Object { $_ -eq 'Any' }).Count -gt 0) { return 'scoped' }
    $sortedProfiles = (@($profiles | Sort-Object -Unique) -join ',')
    if ($sortedProfiles -eq 'Domain,Private,Public') { return 'scoped' }
    return 'profile-dependent'
}


function Get-ScopeParameterSupport {
    # `New-NetFirewallRule -InterfaceAlias` 가 없는 판이면 범위를 좁히라는 안내를 낼 수
    # 없다 (R31). 이 기기에는 있다. 없을 때를 통과로 적지 않으려고 값을 셋으로 둔다.
    param($ParameterNames)
    if ($null -eq $ParameterNames) { return 'unknown' }
    if (@($ParameterNames) -contains 'InterfaceAlias') { return 'True' }
    return 'False'
}

function Invoke-QuerySafe {
    # 게이트 조회. "없음" 과 "조회 실패" 를 구분한다 (windows-prereq.md 0장).
    # 권한 부족을 "없음" 으로 읽으면 판정이 조용히 통과한다. 실패는 unknown 으로 올린다.
    #
    # 목록을 그대로 돌리지 않고 감싼다. PowerShell 은 함수가 돌린 빈 배열을 풀어서
    # $null 로 만든다. 그러면 **결과 0건과 조회 실패가 같은 값이 된다.**
    # 케이스 `query-empty-result` 가 이 함정을 고정한다.
    #
    # **부르는 쪽 계약이 둘이다** (5라운드 blocker).
    # 1. 조회에 `-ErrorAction Stop` 을 직접 붙인다. 스크립트 범위 기본값에 기대지 않는다.
    # 2. 이 안에서 오류를 리디렉션하지 않는다. `2>$null` 은 오류 기록을 지워 실패를
    #    "없음" 으로 바꿀 수 있고, 진단에 쓸 메시지도 같이 사라진다.
    param([scriptblock]$Query)
    try {
        $items = @(& $Query)
        return [pscustomobject]@{ Failed = $false; Items = $items; Message = '' }
    }
    catch {
        if ([string]$_.FullyQualifiedErrorId -match 'NotFound') {
            return [pscustomobject]@{ Failed = $false; Items = @(); Message = 'not found' }
        }
        return [pscustomobject]@{ Failed = $true; Items = @(); Message = [string]$_.Exception.Message }
    }
}

function New-FirewallRuleStub {
    # 케이스용 방화벽 규칙 스텁. 라이브의 `Get-NetFirewallRule` 항목 모양이다.
    param([string]$DisplayName, $Direction, $Action, $Enabled, [string]$Profile = 'Any')
    [pscustomobject]@{
        DisplayName = $DisplayName
        Direction   = $Direction
        Action      = $Action
        Enabled     = $Enabled
        Profile     = $Profile
    }
}

function New-QueryResult {
    # 케이스용 조회 결과 스텁. `Invoke-QuerySafe` 와 같은 모양이다.
    param([bool]$Failed = $false, $Items = @(), [string]$Message = '')
    [pscustomobject]@{ Failed = $Failed; Items = @($Items); Message = $Message }
}

function New-Check {
    param([string]$Name, [string]$Result, [string]$Expected, [string]$Actual)
    [pscustomobject]@{ Name = $Name; Result = $Result; Expected = $Expected; Actual = $Actual }
}

function Get-VerdictExitCode {
    param([string]$Verdict)
    switch ($Verdict) {
        $script:PASS    { return 0 }
        $script:FAIL    { return 1 }
        $script:UNKNOWN { return 2 }
        default         { return 2 }
    }
}

function Get-FirewallPolicyVerdict {
    # 검사 하나하나를 pass/fail/unknown 으로 내고 하나로 합친다.
    # 합치는 규칙: 결함이 하나라도 있으면 fail, 없고 모르는 것이 있으면 unknown.
    # **모르는 것을 통과로 적지 않는다.**
    param(
        [string[]]$PolicyText,
        $Profiles,
        $ExtraChecks,
        [string]$ScopeParameterSupport
    )
    $checks = @()

    $expectPolicy = '값 줄 3, 기대값 3, Always 0'
    if ($null -eq $PolicyText) {
        $checks += New-Check 'policy' $script:UNKNOWN $expectPolicy 'netsh 출력 없음'
    }
    else {
        $counts = Get-PolicyLineCounts $PolicyText
        $actual = '값 줄 {0}, 기대값 {1}, Always {2}' -f $counts.Value, $counts.Expected, $counts.Always
        if ($counts.Value -ne 3) {
            # 프로필 셋의 값 줄을 못 찾았다. 무엇을 본 것인지 모른다.
            $checks += New-Check 'policy' $script:UNKNOWN $expectPolicy $actual
        }
        elseif ($counts.Always -gt 0 -or $counts.Expected -ne 3) {
            $checks += New-Check 'policy' $script:FAIL $expectPolicy $actual
        }
        else {
            $checks += New-Check 'policy' $script:PASS $expectPolicy $actual
        }
    }

    $state = Get-ProfileStateVerdict $Profiles
    $detail = $state
    if ($null -ne $Profiles) {
        $shown = @(@($Profiles) | ForEach-Object { '{0}={1}' -f $_.Name, $_.Enabled })
        if ($shown.Count -gt 0) { $detail = '{0} ({1})' -f $state, ($shown -join ' ') }
    }
    $result = $script:UNKNOWN
    if ($state -eq 'ok') { $result = $script:PASS }
    elseif ($state.StartsWith('fail:')) { $result = $script:FAIL }
    $checks += New-Check 'profile' $result '꺼진 프로필 0, 세 프로필 전부 True' $detail

    # 어댑터 범위 규칙을 만들 수 없는 기기는 통과가 아니다. 주의 문구로만 두면 그런 기기가
    # 그대로 pass 로 끝난다 (4라운드 blocker). 문서 2절의 조치가 그 파라미터에 기대고 있다.
    $capabilityExpect = 'New-NetFirewallRule -InterfaceAlias 지원'
    if ($ScopeParameterSupport -eq 'True') {
        $checks += New-Check 'scope-param' $script:PASS $capabilityExpect '지원'
    }
    elseif ($ScopeParameterSupport -eq 'False') {
        $checks += New-Check 'scope-param' $script:FAIL $capabilityExpect '없음. 어댑터로 좁힌 규칙을 만들 수 없다'
    }
    else {
        $seen = '값 없음'
        if (-not [string]::IsNullOrWhiteSpace($ScopeParameterSupport)) { $seen = $ScopeParameterSupport }
        $checks += New-Check 'scope-param' $script:UNKNOWN $capabilityExpect ('확인 못 함: {0}' -f $seen)
    }

    foreach ($extra in @($ExtraChecks)) {
        if ($null -ne $extra) { $checks += $extra }
    }

    $verdict = $script:PASS
    if (@($checks | Where-Object { $_.Result -eq $script:UNKNOWN }).Count -gt 0) { $verdict = $script:UNKNOWN }
    if (@($checks | Where-Object { $_.Result -eq $script:FAIL }).Count -gt 0) { $verdict = $script:FAIL }

    [pscustomobject]@{ Verdict = $verdict; Checks = $checks }
}

function Get-AdapterCheck {
    # 조회 결과에서 어댑터 검사 한 줄을 만든다. 조회를 직접 하지 않으므로 케이스로 시험된다.
    #
    # **"없음" 과 "못 물어봤음" 은 다른 값이다** (문서 0장 계약, 5라운드 blocker).
    # 둘 다 판정은 unknown 이지만 **이유가 달라야** 다음에 무엇을 할지 갈린다.
    # 없음이면 기다리거나 만들면 되고, 실패면 권한이나 서비스를 봐야 한다.
    param($Query, [string]$Alias)
    $expect = '어댑터가 있고 분류를 읽는다'
    if ($null -eq $Query) { return New-Check 'adapter' $script:UNKNOWN $expect '조회 없음' }
    if ($Query.Failed) { return New-Check 'adapter' $script:UNKNOWN $expect ('조회 실패: {0}' -f $Query.Message) }
    if (@($Query.Items).Count -eq 0) {
        # 우리 어댑터는 Phase 6 전에는 없다. 이것은 정상 기기의 정상 상태다.
        return New-Check 'adapter' $script:UNKNOWN $expect ('어댑터 없음: {0}' -f $Alias)
    }
    return New-Check 'adapter' $script:PASS $expect ('NetworkCategory={0}' -f [string]@($Query.Items)[0].NetworkCategory)
}

function Get-RuleChecks {
    # 규칙 조회 결과와 규칙별 판정에서 검사 세 줄을 만든다.
    # "규칙 0건", "조회 실패", "규칙은 있는데 하나도 유효하지 않음" 을 서로 다른 이유로 적는다.
    param($Query, [string]$RulePattern, [string]$ScopeExpect, $Scopes, $Programs, $Coverage,
          $Excluded, [string]$CoverageExpect = '기대 엔드포인트 전부에 좁혀진 규칙이 있다')
    $programExpect = '우리 규칙의 프로그램 경로가 살아 있다'

    $threeUnknown = {
        param($reason)
        @(
            (New-Check 'rule-scope' $script:UNKNOWN $ScopeExpect $reason),
            (New-Check 'rule-program' $script:UNKNOWN $programExpect $reason),
            (New-Check 'endpoint-coverage' $script:UNKNOWN $CoverageExpect $reason)
        )
    }

    if ($null -eq $Query) { return (& $threeUnknown '조회 없음') }
    if ($Query.Failed) { return (& $threeUnknown ('규칙 조회 실패: {0}' -f $Query.Message)) }

    $dropped = @($Excluded)
    if (@($Scopes).Count -eq 0) {
        if (@($dropped | Where-Object { $_.State -eq 'unreadable' }).Count -gt 0) {
            # 읽지 못한 규칙이 있다. 그것이 우리 엔드포인트를 열고 있을 수도 있다.
            return (& $threeUnknown ('판정 대상 0건. 읽지 못한 규칙: {0}' -f (Format-StateList $dropped)))
        }
        if ($dropped.Count -gt 0) {
            # 우리 이름의 규칙이 있는데 하나도 유효하지 않다. 꺼졌거나 방향·동작이 다르다.
            # **이것은 아직 안 만든 것이 아니라 만들어 놓고 동작하지 않는 상태다.**
            $reason = '유효한 규칙 0건. 제외: {0}' -f (Format-StateList $dropped)
            return @(
                (New-Check 'rule-scope' $script:UNKNOWN $ScopeExpect $reason),
                (New-Check 'rule-program' $script:UNKNOWN $programExpect $reason),
                (New-Check 'endpoint-coverage' $script:FAIL $CoverageExpect $reason)
            )
        }
        # Phase 6 전에는 우리 규칙이 없다. 없는 것은 결함이 아니라 판정 불가다.
        return (& $threeUnknown ('{0} 인바운드 허용 규칙 0건' -f $RulePattern))
    }

    $note = ''
    if ($dropped.Count -gt 0) { $note = ' | 제외: {0}' -f (Format-StateList $dropped) }
    $coverageResult = Get-StateRollup $Coverage @('covered') @('unknown')
    if (@($dropped | Where-Object { $_.State -eq 'unreadable' }).Count -gt 0 -and $coverageResult -eq $script:FAIL) {
        # 읽지 못한 규칙이 남아 있으면 "없다" 로 단정하지 않는다.
        $coverageResult = $script:UNKNOWN
    }

    return @(
        (New-Check 'rule-scope' (Get-StateRollup $Scopes @('scoped') @('unknown')) $ScopeExpect ((Format-StateList $Scopes) + $note)),
        (New-Check 'rule-program' (Get-StateRollup $Programs @('exists', 'any') @('unknown')) $programExpect (Format-StateList $Programs)),
        (New-Check 'endpoint-coverage' $coverageResult $CoverageExpect ((Format-StateList $Coverage) + $note))
    )
}

function Get-StateRollup {
    # 항목별 판정을 하나로 합친다. 통과 값이 아닌 것이 있으면 fail, 모르는 것만 있으면
    # unknown 이다. 이름에 `=` 가 들어가도 흔들리지 않게 문자열이 아니라 값으로 센다.
    param($Results, [string[]]$PassStates, [string[]]$UnknownStates)
    if (@($Results).Count -eq 0) { return $script:UNKNOWN }
    $rollup = $script:PASS
    foreach ($item in @($Results)) {
        $state = [string]$item.State
        if ($PassStates -contains $state) { continue }
        if ($UnknownStates -contains $state) {
            if ($rollup -eq $script:PASS) { $rollup = $script:UNKNOWN }
            continue
        }
        return $script:FAIL
    }
    return $rollup
}

function Format-Expectation {
    # 기대 엔드포인트 하나를 사람이 읽는 표기로 만든다. 판정 이름으로도 쓴다.
    param($Expectation)
    if ($null -eq $Expectation) { return '' }
    if (-not [string]::IsNullOrEmpty($Expectation.IcmpType)) { return '{0}:{1}' -f $Expectation.Protocol, $Expectation.IcmpType }
    if (-not [string]::IsNullOrEmpty($Expectation.Port)) { return '{0}:{1}' -f $Expectation.Protocol, $Expectation.Port }
    return [string]$Expectation.Protocol
}

function Get-QueryItemsOrNull {
    # 조회 결과에서 목록만 꺼낸다. **실패는 빈 목록이 아니라 $null 이다.**
    # 실패를 값으로 바꾸면 그 값이 판정을 통과시킨다 (5·6라운드 blocker 계열).
    param($Query)
    if ($null -eq $Query) { return $null }
    if ($Query.Failed) { return $null }
    # 쉼표로 감싼다. 빈 배열을 그냥 돌리면 풀려서 $null 이 되고, **결과 0건이 조회 실패와
    # 같은 값이 된다.** 케이스 `query-items-empty` 가 이 함정을 고정한다.
    return , @($Query.Items)
}

function Get-RuleFilterStates {
    # 규칙 하나에 대한 범위·프로그램 상태를 낸다. 필터 조회 결과를 인자로 받으므로
    # 케이스로 시험된다.
    #
    # **조회 실패를 빈 값으로 바꾸지 않는다.** 프로그램 필터가 실패했는데 빈 문자열을
    # 넘기면 `Get-RuleProgramState` 가 `any` 를 내고 **경로를 못 읽은 규칙이 통과한다**
    # (6라운드 blocker).
    param($Rule, $AliasQuery, $PortQuery, $AppQuery, [string]$ExpectedAlias, $Expectations)

    $scope = 'unknown'
    $scopeReason = ''
    $program = 'unknown'
    $programReason = ''

    $aliasItems = Get-QueryItemsOrNull $AliasQuery
    $portItems = Get-QueryItemsOrNull $PortQuery
    $appItems = Get-QueryItemsOrNull $AppQuery

    if ($null -eq $aliasItems) { $scopeReason = '인터페이스 필터 조회 실패' }
    elseif ($null -eq $portItems) { $scopeReason = '포트 필터 조회 실패' }
    else {
        $stub = New-RuleStub ([string]$Rule.Profile) `
            (@($aliasItems | ForEach-Object { $_.InterfaceAlias }) -join ',') `
            (@($portItems | ForEach-Object { $_.Protocol }) -join ',') `
            (@($portItems | ForEach-Object { $_.LocalPort }) -join ',') `
            (@($portItems | ForEach-Object { $_.IcmpType }) -join ',')
        $scope = Get-RuleScopeVerdict $stub $ExpectedAlias $Expectations
    }

    if ($null -eq $appItems) { $programReason = '응용프로그램 필터 조회 실패' }
    else { $program = Get-RuleProgramState (@($appItems | ForEach-Object { $_.Program }) -join ',') }

    [pscustomobject]@{
        Scope         = $scope
        ScopeReason   = $scopeReason
        Program       = $program
        ProgramReason = $programReason
    }
}

function Get-EndpointCoverage {
    # 기대 엔드포인트 **전부**에 좁혀진 규칙이 있는가.
    #
    # 하나만 맞으면 통과하던 것이 6라운드 blocker 였다. 문서 2절은 TCP 25565 와 ICMP 에코를
    # 둘 다 요구한다. 하나가 없으면 그쪽이 안 열린다.
    #
    # 기대 하나만 담은 집합으로 같은 판정 함수를 다시 돌린다. 판정 규칙을 두 벌 쓰지 않는다.
    # 값은 covered / missing / unknown 이다. **넓은 규칙은 덮은 것으로 치지 않는다.**
    param($Rules, [string]$ExpectedAlias, $Expectations)
    $out = @()
    foreach ($expectation in @($Expectations)) {
        $name = Format-Expectation $expectation
        $state = 'missing'
        $sawUnknown = $false
        foreach ($rule in @($Rules)) {
            $verdict = Get-RuleScopeVerdict $rule $ExpectedAlias @($expectation)
            if ($verdict -eq 'scoped') { $state = 'covered'; break }
            if ($verdict -eq 'unknown') { $sawUnknown = $true }
        }
        # 판정 못 한 규칙이 있으면 그것이 이 기대를 덮었을 수도 있다. 없다고 단정하지 않는다.
        if ($state -ne 'covered' -and $sawUnknown) { $state = 'unknown' }
        $out += [pscustomobject]@{ Name = $name; State = $state; Reason = '' }
    }
    return , $out
}

function Format-StateList {
    param($Items)
    (@($Items | ForEach-Object {
        if ([string]::IsNullOrEmpty($_.Reason)) { '{0}={1}' -f $_.Name, $_.State }
        else { '{0}={1}({2})' -f $_.Name, $_.State, $_.Reason }
    }) -join ' ')
}

function Get-RuleSelection {
    # 판정 대상 규칙을 고른다. **켜진 인바운드 허용 규칙만** 대상이다.
    #
    # `Enabled` 를 안 보면 **꺼진 규칙이 덮은 것으로 세어진다** (7라운드 blocker).
    # 이 기기에는 꺼진 규칙이 240개 있다. `Direction` 과 `Action` 도 같은 종류라 함께 본다.
    # 아웃바운드 규칙이나 Block 규칙은 인바운드를 열지 않는다.
    #
    # 값의 형은 `[bool]` 이 아니라 `Enabled`/`Direction`/`Action` 열거형이다. 프로필
    # `Enabled` 때와 같은 함정이라 **이름 문자열로 비교한다.**
    #
    # 뺀 규칙은 버리지 않고 사유와 함께 남긴다. 버리면 "우리 규칙이 꺼져 있다" 와
    # "우리 규칙이 아직 없다" 가 같은 값이 된다.
    param($Rules)
    $candidates = @()
    $excluded = @()
    foreach ($rule in @($Rules)) {
        if ($null -eq $rule) { continue }
        $name = [string]$rule.DisplayName
        $direction = [string]$rule.Direction
        $action = [string]$rule.Action
        $enabled = [string]$rule.Enabled

        if ($direction -ne 'Inbound' -and $direction -ne 'Outbound') {
            $excluded += [pscustomobject]@{ Name = $name; State = 'unreadable'; Reason = ('Direction={0}' -f $direction) }
            continue
        }
        if ($action -ne 'Allow' -and $action -ne 'Block') {
            $excluded += [pscustomobject]@{ Name = $name; State = 'unreadable'; Reason = ('Action={0}' -f $action) }
            continue
        }
        if ($enabled -ne 'True' -and $enabled -ne 'False') {
            # `NotConfigured` 나 빈 값이다. 켜졌다고 넘겨짚지 않는다.
            $excluded += [pscustomobject]@{ Name = $name; State = 'unreadable'; Reason = ('Enabled={0}' -f $enabled) }
            continue
        }
        if ($direction -ne 'Inbound') {
            $excluded += [pscustomobject]@{ Name = $name; State = 'outbound'; Reason = '' }
            continue
        }
        if ($action -ne 'Allow') {
            $excluded += [pscustomobject]@{ Name = $name; State = 'block'; Reason = '' }
            continue
        }
        if ($enabled -ne 'True') {
            $excluded += [pscustomobject]@{ Name = $name; State = 'disabled'; Reason = '' }
            continue
        }
        $candidates += $rule
    }
    [pscustomobject]@{ Candidates = $candidates; Excluded = $excluded }
}

# ---------------------------------------------------------------------------
# 판정 실행 모드
# ---------------------------------------------------------------------------

function Get-NetshPolicyText {
    # netsh 를 못 돌렸으면 $null 이다. 빈 목록으로 돌리면 "값 줄 0" 과 구분되지 않는다.
    # 레이블은 번역되고 콘솔 코드 페이지에 따라 깨질 수도 있다. 값만 보므로 상관없다.
    try {
        $raw = & netsh advfirewall show allprofiles firewallpolicy 2>&1
        if ($LASTEXITCODE -ne 0) { return $null }
        return @($raw | ForEach-Object { [string]$_ })
    }
    catch { return $null }
}

function Get-FirewallProfileState {
    # 조회 실패를 "없음" 으로 읽지 않는다. 실패하면 $null 이고 판정은 unknown 이 된다.
    # 조회에 `-ErrorAction Stop` 을 직접 붙인다. 스크립트 범위 기본값에 기대지 않는다.
    # 목록을 그대로 `return` 하면 또 풀린다. 쉼표로 감싸되 $null 은 그대로 돌린다.
    $items = Get-QueryItemsOrNull (Invoke-QuerySafe { Get-NetFirewallProfile -ErrorAction Stop })
    if ($null -eq $items) { return $null }
    return , $items
}

function Get-AdapterChecks {
    # `-InterfaceAlias` 를 줬을 때만 돈다.
    #
    # 조회에는 `-ErrorAction Stop` 을 직접 붙이고 **리디렉션하지 않는다** (5라운드 blocker).
    # `2>$null` 을 쓰면 권한·제공자 실패가 오류 없이 빈 결과가 되어 "없음" 으로 보고된다.
    # 판정을 조립하는 일은 `Get-AdapterCheck` 와 `Get-RuleChecks` 가 한다. 그 둘은 조회를
    # 하지 않으므로 케이스 표로 시험된다.
    param([string]$Alias, [string]$RulePattern, $Expectations)

    $wantText = (@($Expectations | ForEach-Object {
        if (-not [string]::IsNullOrEmpty($_.IcmpType)) { '{0}:{1}' -f $_.Protocol, $_.IcmpType }
        elseif (-not [string]::IsNullOrEmpty($_.Port)) { '{0}:{1}' -f $_.Protocol, $_.Port }
        else { [string]$_.Protocol }
    }) -join ' ')
    $scopeExpect = '어댑터 {0} 에만, 기대 엔드포인트 {1}' -f $Alias, $wantText

    $checks = @()
    $connection = Invoke-QuerySafe { Get-NetConnectionProfile -InterfaceAlias $Alias -ErrorAction Stop }
    $checks += Get-AdapterCheck $connection $Alias

    $found = Invoke-QuerySafe { Get-NetFirewallRule -DisplayName $RulePattern -ErrorAction Stop }
    $scopes = @()
    $programs = @()
    $stubs = @()
    $excluded = @()
    if ($null -ne $found -and -not $found.Failed) {
        # 판정 대상 고르기는 순수 함수가 한다. 라이브 코드에 두면 케이스가 닿지 않는다.
        $selection = Get-RuleSelection $found.Items
        $excluded = @($selection.Excluded)
        foreach ($rule in @($selection.Candidates)) {
            # 필터 조회 셋도 같은 계약을 따른다. 실패는 값이 아니라 실패로 남는다.
            $aliasQuery = Invoke-QuerySafe { Get-NetFirewallInterfaceFilter -AssociatedNetFirewallRule $rule -ErrorAction Stop }
            $portQuery = Invoke-QuerySafe { Get-NetFirewallPortFilter -AssociatedNetFirewallRule $rule -ErrorAction Stop }
            $appQuery = Invoke-QuerySafe { Get-NetFirewallApplicationFilter -AssociatedNetFirewallRule $rule -ErrorAction Stop }

            $state = Get-RuleFilterStates $rule $aliasQuery $portQuery $appQuery $Alias $Expectations
            $name = [string]$rule.DisplayName
            $scopes += [pscustomobject]@{ Name = $name; State = $state.Scope; Reason = $state.ScopeReason }
            $programs += [pscustomobject]@{ Name = $name; State = $state.Program; Reason = $state.ProgramReason }

            $aliasItems = Get-QueryItemsOrNull $aliasQuery
            $portItems = Get-QueryItemsOrNull $portQuery
            if ($null -eq $aliasItems -or $null -eq $portItems) {
                # 필터를 못 읽은 규칙도 목록에 남긴다. 그래야 덮음 판정이 "없다" 로
                # 단정하지 않고 unknown 이 된다.
                $stubs += New-RuleStub '' '' '' '' ''
            }
            else {
                $stubs += New-RuleStub ([string]$rule.Profile) `
                    (@($aliasItems | ForEach-Object { $_.InterfaceAlias }) -join ',') `
                    (@($portItems | ForEach-Object { $_.Protocol }) -join ',') `
                    (@($portItems | ForEach-Object { $_.LocalPort }) -join ',') `
                    (@($portItems | ForEach-Object { $_.IcmpType }) -join ',')
            }
        }
    }
    $coverage = Get-EndpointCoverage $stubs $Alias $Expectations
    $checks += Get-RuleChecks $found $RulePattern $scopeExpect $scopes $programs $coverage $excluded

    return $checks
}


function Get-ReferenceNotes {
    # 판정이 아니다. 사람이 원인을 찾을 때 쓰는 값이다.
    param($Profiles)
    $notes = @()

    if ($null -eq $Profiles) {
        $notes += 'DefaultInboundAction: 조회 실패'
    }
    else {
        foreach ($item in @($Profiles)) {
            $action = [string]$item.DefaultInboundAction
            $notes += ('DefaultInboundAction {0}={1} ({2}). 판정에 쓰지 않는다' -f $item.Name, $action, (Get-InboundActionMeaning $action))
        }
        $first = @($Profiles)[0]
        if ($null -ne $first) {
            $notes += ('Enabled 값의 형: {0}. bool 이 아니므로 이름 문자열로 비교한다' -f $first.Enabled.GetType().FullName)
        }
    }

    # `New-NetFirewallRule -InterfaceAlias` 지원 여부는 참고가 아니라 판정이다.
    # `scope-param` 검사로 올라갔다. 여기에 또 적지 않는다 (규칙 5).
    return $notes
}

function Invoke-LiveCheck {
    param([string]$Alias, [string]$RulePattern, [string[]]$Endpoint)

    $policyText = Get-NetshPolicyText
    $profiles = Get-FirewallProfileState

    $expectations = @($Endpoint | ForEach-Object { ConvertTo-RuleExpectation $_ } | Where-Object { $null -ne $_ })

    $extra = @()
    if (-not [string]::IsNullOrWhiteSpace($Alias)) {
        $extra = Get-AdapterChecks -Alias $Alias -RulePattern $RulePattern -Expectations $expectations
    }

    # 조회 실패는 $null 로 남아 `unknown` 이 된다. 빈 목록(= 파라미터 없음)과 구분된다.
    $parameters = Get-QueryItemsOrNull (Invoke-QuerySafe { @((Get-Command New-NetFirewallRule -ErrorAction Stop).Parameters.Keys) })
    $support = Get-ScopeParameterSupport $parameters

    $report = Get-FirewallPolicyVerdict -PolicyText $policyText -Profiles $profiles -ExtraChecks $extra -ScopeParameterSupport $support

    Write-Output '방화벽 정책 판정 (windows-prereq.md 2절)'
    Write-Output ''
    Write-Output ('{0,-18} {1,-8} {2}' -f '검사', '결과', '기대 -> 실제')
    foreach ($check in $report.Checks) {
        Write-Output ('{0,-18} {1,-8} {2} -> {3}' -f $check.Name, $check.Result, $check.Expected, $check.Actual)
    }
    Write-Output ''
    foreach ($note in (Get-ReferenceNotes $profiles)) { Write-Output ('참고: {0}' -f $note) }
    if ([string]::IsNullOrWhiteSpace($Alias)) {
        Write-Output '참고: -InterfaceAlias 를 주지 않아 어댑터와 규칙 검사는 돌리지 않았다'
    }
    Write-Output ''
    Write-Output ('VERDICT: {0}' -f $report.Verdict)
    # 반환값 대신 스크립트 변수로 넘긴다. 반환값은 출력 스트림에 섞인다.
    $script:ExitCode = Get-VerdictExitCode $report.Verdict
}

# ---------------------------------------------------------------------------
# 케이스 표 실행
# ---------------------------------------------------------------------------

function Invoke-SelfTest {
    $failed = 0
    foreach ($case in $script:Cases) {
        $actual = $null
        try { $actual = [string](& $case.Run) }
        catch { $actual = 'EXCEPTION: ' + $_.Exception.Message }

        if ($actual -eq $case.Expect) {
            Write-Output ('PASS  {0,-34} {1}' -f $case.Name, $actual)
        }
        else {
            $failed++
            Write-Output ('FAIL  {0,-34} expected={1} actual={2}' -f $case.Name, $case.Expect, $actual)
        }
    }
    Write-Output ''
    Write-Output ('케이스 {0}건, 실패 {1}건' -f $script:Cases.Count, $failed)
    if ($failed -eq 0) { Write-Output 'SELFTEST: pass'; $script:ExitCode = 0; return }
    Write-Output 'SELFTEST: fail'
    $script:ExitCode = 1
}

# ---------------------------------------------------------------------------
# 진입점. 마지막 줄은 SELFTEST: 또는 VERDICT: 한 줄이고 종료 코드가 같이 간다.
# ---------------------------------------------------------------------------

$script:ExitCode = 2

if ($SelfTest) {
    Invoke-SelfTest
    exit $script:ExitCode
}

Invoke-LiveCheck -Alias $InterfaceAlias -RulePattern $RuleNamePattern -Endpoint $ExpectedEndpoint
exit $script:ExitCode
