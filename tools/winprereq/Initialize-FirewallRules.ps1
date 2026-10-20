<#
.SYNOPSIS
  우리 어댑터의 인바운드 방화벽 규칙을 검증하고, 없으면 만든다. `windows-prereq.md` 2절 "클라이언트가
  만드는 규칙" 의 판정을 스크립트로 뺀 것이다.

.DESCRIPTION
  클라이언트가 어댑터를 만든 직후 `System32` 의 Windows PowerShell 로 부른다. 관리자 권한이 있어야 한다.

  규칙은 둘이다. 그룹은 `Sangtachi` 다.

  | 이름                       | 여는 것                              |
  |----------------------------|--------------------------------------|
  | `Sangtachi-TCP-25565-In`   | 인바운드 허용, TCP, 로컬 포트 25565  |
  | `Sangtachi-ICMPv4-Echo-In` | 인바운드 허용, ICMPv4, 에코 요청(8)  |

  **이름에 공백을 두지 않는다.** 클라이언트가 어긋난 규칙의 이름을 `ERROR` 줄에 싣는데 로그 값에는 공백을
  싣지 않는다 (architecture.md 9장).

  그 이름의 규칙이 없으면 만들고, 있던 것이든 만든 것이든 같은 검사를 지나야 한다. 이미 있는 규칙을 고치거나
  지우지 않는다. 검사는 문서 2절의 표 순서다.

  | 토큰        | 검사 |
  |-------------|------|
  | `count`     | 그 이름의 규칙이 정확히 하나다 |
  | `group`     | 그룹이 `Sangtachi` 다 |
  | `enabled`   | 켜져 있다 |
  | `direction` | 방향이 인바운드이고 동작이 허용이다 |
  | `interface` | 인터페이스가 우리 어댑터 하나다 |
  | `port`      | 프로토콜, 로컬 포트, 원격 포트, ICMP 종류가 위 표와 같다 |
  | `scope`     | 프로필, 주소, 프로그램, 서비스가 `Any` 다 |

  방화벽이 `BlockInboundAlways` 인지도 본다(문서 2절 기본 정책 확인). 같은 포트를 막는 다른 차단 규칙은
  보지 않는다.

  **출력.** 줄마다 하나다. 클라이언트는 `FAIL` 과 `CREATED` 줄만 읽고 판정은 종료 코드로 한다.

  | 줄 | 뜻 |
  |----|----|
  | `CREATED <이름>` | 이번에 만들었다 |
  | `FAIL <이름 또는 policy> <토큰>` | 검사가 어긋났다. 만들지 못했으면 토큰이 `create` 다 |
  | `UNKNOWN <이름 또는 policy> <무엇>` | 조회가 실패해 판정할 수 없다 |
  | `VERDICT: pass` / `fail` / `unknown` | 마지막 줄. 종료 코드 0 / 1 / 2 |

  **판정할 수 없으면 통과로 적지 않는다.** 조회 실패는 `unknown` 이다. 확인된 결함이 하나라도 있으면 판정
  불가가 같이 있어도 `fail` 이다.

  `-SelfTest` 는 케이스 표를 돌린다. 방화벽을 조회하지도 바꾸지도 않는다. 마지막 줄은 `SELFTEST: pass` 또는
  `SELFTEST: fail` 이고 종료 코드는 0 또는 1 이다.

  이 스크립트가 출력하는 문자열은 ASCII 로만 적는다. 클라이언트가 바이트로 읽는다.

.PARAMETER InterfaceAlias
  우리 어댑터 이름. 규칙을 이 어댑터에 묶고, 묶였는지 본다.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File .\Initialize-FirewallRules.ps1 -SelfTest

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File .\Initialize-FirewallRules.ps1 -InterfaceAlias Sangtachi
#>
[CmdletBinding()]
param(
    [string]$InterfaceAlias,
    [switch]$SelfTest
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:Group = 'Sangtachi'

# 우리가 만드는 규칙. LocalPort 와 IcmpType 은 받아들이는 표기들이다. 첫 값이 만들 때 쓰는 값이다.
#
# ICMP 에는 포트가 없다. 그런데 Get-NetFirewallPortFilter 는 우리가 포트 없이 만든 ICMP 규칙의 LocalPort 를
# `RPC` 로 돌려줬다(관리자 셸 실측. 레지스트리 원문에는 LPort 가 없다). 내장 ICMP 규칙도 같다. 그래서 `Any` 와
# `RPC` 를 둘 다 받는다. IcmpType 은 조회가 `8` 로 돌려줬다. 레지스트리 원문의 `8:*`(종류 8, 모든 코드)도 받는다.
$script:Rules = @(
    @{ Name = 'Sangtachi-TCP-25565-In'; Protocol = 'TCP'; LocalPort = @('25565'); IcmpType = @('Any') }
    @{ Name = 'Sangtachi-ICMPv4-Echo-In'; Protocol = 'ICMPv4'; LocalPort = @('Any', 'RPC'); IcmpType = @('8', '8:*') }
)

# ---------------------------------------------------------------------------
# 판정 함수. 조회 결과를 인자로 받으므로 케이스로 시험된다.
# ---------------------------------------------------------------------------

function Get-RuleFailures {
    # 규칙 하나의 모습($Snap)을 기대($Expected)와 대조한다. 어긋난 검사의 토큰 목록을 낸다. 읽지 못한 값이
    # 있으면 `unreadable` 을 더한다. 아무것도 어긋나지 않았으면 빈 목록이다.
    #
    # $Snap 은 해시다. RuleCount 는 그 이름의 규칙 수이고, 나머지는 문자열이다. $null 이면 그 조회가 실패했다.
    # RuleCount 가 1 이 아니면 다른 값을 보지 않는다. 어느 규칙의 값인지 정해지지 않기 때문이다.
    param([hashtable]$Expected, [hashtable]$Snap, [string]$Alias)

    if ($Snap['RuleCount'] -ne 1) { return , @('count') }

    $keys = @('Group', 'Enabled', 'Direction', 'Action', 'Interface', 'Protocol', 'LocalPort', 'RemotePort',
              'IcmpType', 'Profile', 'LocalAddress', 'RemoteAddress', 'Program', 'Service')
    $missing = @($keys | Where-Object { $null -eq $Snap[$_] })
    function Test-Read([string[]]$Need) {
        foreach ($k in $Need) { if ($missing -contains $k) { return $false } }
        return $true
    }

    $out = @()
    if ((Test-Read 'Group') -and ($Snap['Group'] -ne $script:Group)) { $out += 'group' }
    if ((Test-Read 'Enabled') -and ($Snap['Enabled'] -ne 'True')) { $out += 'enabled' }
    if ((Test-Read 'Direction', 'Action') -and
        (($Snap['Direction'] -ne 'Inbound') -or ($Snap['Action'] -ne 'Allow'))) { $out += 'direction' }
    # 어댑터 이름은 대소문자를 가리지 않는다. 목록이면 쉼표로 이어져 와서 이름 하나와 같지 않다.
    if ((Test-Read 'Interface') -and ($Snap['Interface'] -ne $Alias)) { $out += 'interface' }
    if ((Test-Read 'Protocol', 'LocalPort', 'RemotePort', 'IcmpType') -and
        (($Snap['Protocol'] -ne $Expected.Protocol) -or ($Expected.LocalPort -notcontains $Snap['LocalPort']) -or
         ($Snap['RemotePort'] -ne 'Any') -or ($Expected.IcmpType -notcontains $Snap['IcmpType']))) { $out += 'port' }
    $scopeKeys = @('Profile', 'LocalAddress', 'RemoteAddress', 'Program', 'Service')
    if ((Test-Read $scopeKeys) -and @($scopeKeys | Where-Object { $Snap[$_] -ne 'Any' }).Count -gt 0) { $out += 'scope' }
    if ($missing.Count -gt 0) { $out += 'unreadable' }
    return , $out
}

function Get-PolicyVerdict {
    # `netsh advfirewall show allprofiles firewallpolicy` 의 줄들. 레이블은 번역되고 깨질 수 있으므로 값만 본다.
    # 값 줄은 세 프로필에 하나씩 셋이어야 한다. 아니면 읽지 못한 것이다. `BlockInboundAlways` 가 하나라도
    # 있으면 fail 이다. 그 모드는 허용 규칙을 전부 무시한다.
    param($Lines)
    if ($null -eq $Lines) { return 'unknown' }
    $values = @()
    foreach ($line in @($Lines)) {
        $m = [regex]::Match([string]$line,
            '(?<![A-Za-z])(BlockInboundAlways|BlockInbound|AllowInbound),(AllowOutbound|BlockOutbound)(?![A-Za-z])')
        if ($m.Success) { $values += $m.Groups[1].Value }
    }
    if ($values.Count -ne 3) { return 'unknown' }
    if ($values -contains 'BlockInboundAlways') { return 'fail' }
    return 'pass'
}

function Get-OverallVerdict {
    # 결함이 하나라도 있으면 fail. 없고 판정 불가가 있으면 unknown. 둘 다 없으면 pass.
    param([int]$Fails, [int]$Unknowns)
    if ($Fails -gt 0) { return 'fail' }
    if ($Unknowns -gt 0) { return 'unknown' }
    return 'pass'
}

# ---------------------------------------------------------------------------
# 케이스 표
# ---------------------------------------------------------------------------

function New-Snap {
    # 정상 TCP 규칙의 모습. 케이스가 한 칸씩 바꾼다.
    param([hashtable]$Override = @{})
    $s = @{
        RuleCount = 1; Group = 'Sangtachi'; Enabled = 'True'; Direction = 'Inbound'; Action = 'Allow'
        Interface = 'Sangtachi'; Protocol = 'TCP'; LocalPort = '25565'; RemotePort = 'Any'; IcmpType = 'Any'
        Profile = 'Any'; LocalAddress = 'Any'; RemoteAddress = 'Any'; Program = 'Any'; Service = 'Any'
    }
    foreach ($k in $Override.Keys) { $s[$k] = $Override[$k] }
    return $s
}

function Join-Tokens([hashtable]$Snap, [int]$Index = 0) {
    return ((Get-RuleFailures $script:Rules[$Index] $Snap 'Sangtachi') -join ',')
}

$script:IcmpBase = @{ Protocol = 'ICMPv4'; LocalPort = 'Any'; IcmpType = '8' }

$script:Cases = @(
    # --- 규칙 하나 ----------------------------------------------------------
    @{ Name = 'tcp-exact-passes'; Expect = ''; Run = { Join-Tokens (New-Snap) } }
    @{ Name = 'icmp-exact-passes'; Expect = ''; Run = { Join-Tokens (New-Snap $script:IcmpBase) 1 } }
    @{ Name = 'icmp-rpc-local-port-passes'; Expect = ''
       Run = { Join-Tokens (New-Snap @{ Protocol = 'ICMPv4'; LocalPort = 'RPC'; IcmpType = '8' }) 1 } }
    @{ Name = 'icmp-real-port-is-not-ours'; Expect = 'port'
       Run = { Join-Tokens (New-Snap @{ Protocol = 'ICMPv4'; LocalPort = '25565'; IcmpType = '8' }) 1 } }
    @{ Name = 'tcp-rpc-port-is-not-ours'; Expect = 'port'; Run = { Join-Tokens (New-Snap @{ LocalPort = 'RPC' }) } }
    @{ Name = 'icmp-registry-form-passes'; Expect = ''
       Run = { Join-Tokens (New-Snap @{ Protocol = 'ICMPv4'; LocalPort = 'Any'; IcmpType = '8:*' }) 1 } }
    @{ Name = 'alias-case-insensitive-passes'; Expect = ''; Run = { Join-Tokens (New-Snap @{ Interface = 'sangtachi' }) } }
    @{ Name = 'missing-is-count'; Expect = 'count'; Run = { Join-Tokens (New-Snap @{ RuleCount = 0 }) } }
    @{ Name = 'duplicate-is-count'; Expect = 'count'; Run = { Join-Tokens (New-Snap @{ RuleCount = 2 }) } }
    @{ Name = 'other-group'; Expect = 'group'; Run = { Join-Tokens (New-Snap @{ Group = 'Other' }) } }
    @{ Name = 'no-group'; Expect = 'group'; Run = { Join-Tokens (New-Snap @{ Group = '' }) } }
    @{ Name = 'disabled'; Expect = 'enabled'; Run = { Join-Tokens (New-Snap @{ Enabled = 'False' }) } }
    @{ Name = 'block-action'; Expect = 'direction'; Run = { Join-Tokens (New-Snap @{ Action = 'Block' }) } }
    @{ Name = 'outbound'; Expect = 'direction'; Run = { Join-Tokens (New-Snap @{ Direction = 'Outbound' }) } }
    @{ Name = 'any-interface'; Expect = 'interface'; Run = { Join-Tokens (New-Snap @{ Interface = 'Any' }) } }
    @{ Name = 'other-interface'; Expect = 'interface'; Run = { Join-Tokens (New-Snap @{ Interface = 'Wi-Fi' }) } }
    @{ Name = 'extra-interface'; Expect = 'interface'; Run = { Join-Tokens (New-Snap @{ Interface = 'Sangtachi,Wi-Fi' }) } }
    @{ Name = 'any-port'; Expect = 'port'; Run = { Join-Tokens (New-Snap @{ LocalPort = 'Any' }) } }
    @{ Name = 'port-range'; Expect = 'port'; Run = { Join-Tokens (New-Snap @{ LocalPort = '25565-25566' }) } }
    @{ Name = 'other-port'; Expect = 'port'; Run = { Join-Tokens (New-Snap @{ LocalPort = '25566' }) } }
    @{ Name = 'remote-port-set'; Expect = 'port'; Run = { Join-Tokens (New-Snap @{ RemotePort = '1234' }) } }
    @{ Name = 'udp-protocol'; Expect = 'port'; Run = { Join-Tokens (New-Snap @{ Protocol = 'UDP' }) } }
    @{ Name = 'icmp-all-types'; Expect = 'port'
       Run = { Join-Tokens (New-Snap @{ Protocol = 'ICMPv4'; LocalPort = 'Any'; IcmpType = 'Any' }) 1 } }
    @{ Name = 'icmp-other-type'; Expect = 'port'
       Run = { Join-Tokens (New-Snap @{ Protocol = 'ICMPv4'; LocalPort = 'Any'; IcmpType = '0' }) 1 } }
    @{ Name = 'icmp-two-types'; Expect = 'port'
       Run = { Join-Tokens (New-Snap @{ Protocol = 'ICMPv4'; LocalPort = 'Any'; IcmpType = '8,0' }) 1 } }
    @{ Name = 'tcp-rule-in-icmp-slot'; Expect = 'port'; Run = { Join-Tokens (New-Snap) 1 } }
    @{ Name = 'profile-public-only'; Expect = 'scope'; Run = { Join-Tokens (New-Snap @{ Profile = 'Public' }) } }
    @{ Name = 'profile-full-set'; Expect = 'scope'; Run = { Join-Tokens (New-Snap @{ Profile = 'Domain, Private, Public' }) } }
    @{ Name = 'remote-address-set'; Expect = 'scope'; Run = { Join-Tokens (New-Snap @{ RemoteAddress = '10.100.0.0/255.255.255.0' }) } }
    @{ Name = 'local-address-set'; Expect = 'scope'; Run = { Join-Tokens (New-Snap @{ LocalAddress = '10.100.0.1' }) } }
    @{ Name = 'program-set'; Expect = 'scope'; Run = { Join-Tokens (New-Snap @{ Program = 'C:\x.exe' }) } }
    @{ Name = 'service-set'; Expect = 'scope'; Run = { Join-Tokens (New-Snap @{ Service = 'svc' }) } }
    @{ Name = 'two-defects-both-listed'; Expect = 'enabled,interface'
       Run = { Join-Tokens (New-Snap @{ Enabled = 'False'; Interface = 'Any' }) } }
    @{ Name = 'unreadable-interface'; Expect = 'unreadable'; Run = { Join-Tokens (New-Snap @{ Interface = $null }) } }
    @{ Name = 'unreadable-port'; Expect = 'unreadable'; Run = { Join-Tokens (New-Snap @{ IcmpType = $null }) } }
    @{ Name = 'unreadable-does-not-hide-defect'; Expect = 'enabled,unreadable'
       Run = { Join-Tokens (New-Snap @{ Enabled = 'False'; Service = $null }) } }

    # --- 정책 ---------------------------------------------------------------
    @{ Name = 'policy-normal'; Expect = 'pass'
       Run = { Get-PolicyVerdict @('Firewall Policy   BlockInbound,AllowOutbound', 'x',
                                   'Firewall Policy   BlockInbound,AllowOutbound', 'Firewall Policy   BlockInbound,AllowOutbound', 'Ok.') } }
    @{ Name = 'policy-korean-label'; Expect = 'pass'
       Run = { Get-PolicyVerdict @('방화벽 정책 BlockInbound,AllowOutbound', '방화벽 정책 BlockInbound,AllowOutbound',
                                   '방화벽 정책 BlockInbound,AllowOutbound') } }
    @{ Name = 'policy-allow-inbound-is-not-ours-to-fail'; Expect = 'pass'
       Run = { Get-PolicyVerdict @('P AllowInbound,AllowOutbound', 'P BlockInbound,AllowOutbound', 'P BlockInbound,AllowOutbound') } }
    @{ Name = 'policy-always-one-profile'; Expect = 'fail'
       Run = { Get-PolicyVerdict @('P BlockInbound,AllowOutbound', 'P BlockInboundAlways,AllowOutbound', 'P BlockInbound,AllowOutbound') } }
    @{ Name = 'policy-two-lines'; Expect = 'unknown'
       Run = { Get-PolicyVerdict @('P BlockInbound,AllowOutbound', 'P BlockInbound,AllowOutbound') } }
    @{ Name = 'policy-affix-not-a-value'; Expect = 'unknown'
       Run = { Get-PolicyVerdict @('P XBlockInbound,AllowOutbound', 'P BlockInbound,AllowOutbound', 'P BlockInbound,AllowOutbound') } }
    @{ Name = 'policy-query-failed'; Expect = 'unknown'; Run = { Get-PolicyVerdict $null } }

    # --- 합치기 -------------------------------------------------------------
    @{ Name = 'overall-pass'; Expect = 'pass'; Run = { Get-OverallVerdict 0 0 } }
    @{ Name = 'overall-unknown'; Expect = 'unknown'; Run = { Get-OverallVerdict 0 1 } }
    @{ Name = 'overall-fail-beats-unknown'; Expect = 'fail'; Run = { Get-OverallVerdict 1 1 } }
)

function Invoke-SelfTest {
    # 케이스마다 한 줄을 내고 실패한 수를 $script:SelfTestFailed 에 둔다.
    $script:SelfTestFailed = 0
    foreach ($case in $script:Cases) {
        $actual = [string](& $case.Run)
        if ($actual -ceq $case.Expect) {
            Write-Output ('ok    {0}' -f $case.Name)
        } else {
            Write-Output ('FAIL  {0} expect=[{1}] actual=[{2}]' -f $case.Name, $case.Expect, $actual)
            $script:SelfTestFailed++
        }
    }
    Write-Output ('{0} cases, {1} failed' -f $script:Cases.Count, $script:SelfTestFailed)
}

# ---------------------------------------------------------------------------
# 조회. 실패와 "없음" 을 구분한다 (windows-prereq.md 0절).
# ---------------------------------------------------------------------------

function Invoke-Query([scriptblock]$Query) {
    # 실패면 Failed 가 참이다. 찾는 것이 없을 때만 빈 목록으로 돌린다.
    try {
        return @{ Failed = $false; Items = @(& $Query) }
    } catch {
        if ($_.CategoryInfo.Category -eq 'ObjectNotFound') { return @{ Failed = $false; Items = @() } }
        return @{ Failed = $true; Items = @() }
    }
}

function Join-Values($Items, [string]$Property) {
    # 필터 조회 결과의 한 속성. 값이 목록이면 쉼표로 잇는다. 조회가 실패했으면 $null 이다.
    if ($Items.Failed -or $Items.Items.Count -eq 0) { return $null }
    $values = @()
    foreach ($item in $Items.Items) { foreach ($v in @($item.$Property)) { $values += [string]$v } }
    return ($values -join ',')
}

function Get-RuleSnap([string]$Name) {
    $rules = Invoke-Query { Get-NetFirewallRule -DisplayName $Name -ErrorAction Stop }
    if ($rules.Failed) { return $null }
    $snap = @{ RuleCount = $rules.Items.Count }
    if ($snap['RuleCount'] -ne 1) { return $snap }
    $rule = $rules.Items[0]
    $snap.Group = [string]$rule.Group
    $snap.Enabled = [string]$rule.Enabled
    $snap.Direction = [string]$rule.Direction
    $snap.Action = [string]$rule.Action
    $snap.Profile = [string]$rule.Profile
    $iface = Invoke-Query { $rule | Get-NetFirewallInterfaceFilter -ErrorAction Stop }
    $snap.Interface = Join-Values $iface 'InterfaceAlias'
    $port = Invoke-Query { $rule | Get-NetFirewallPortFilter -ErrorAction Stop }
    foreach ($k in @('Protocol', 'LocalPort', 'RemotePort', 'IcmpType')) { $snap[$k] = Join-Values $port $k }
    $addr = Invoke-Query { $rule | Get-NetFirewallAddressFilter -ErrorAction Stop }
    foreach ($k in @('LocalAddress', 'RemoteAddress')) { $snap[$k] = Join-Values $addr $k }
    $app = Invoke-Query { $rule | Get-NetFirewallApplicationFilter -ErrorAction Stop }
    $snap.Program = Join-Values $app 'Program'
    $svc = Invoke-Query { $rule | Get-NetFirewallServiceFilter -ErrorAction Stop }
    $snap.Service = Join-Values $svc 'Service'
    return $snap
}

function New-Rule([hashtable]$Expected, [string]$Alias) {
    $common = @{
        DisplayName = $Expected.Name; Group = $script:Group; Direction = 'Inbound'; Action = 'Allow'
        Profile = 'Any'; InterfaceAlias = $Alias; Protocol = $Expected.Protocol; ErrorAction = 'Stop'
    }
    if ($Expected.Protocol -eq 'TCP') { $common.LocalPort = $Expected.LocalPort[0] }
    else { $common.IcmpType = $Expected.IcmpType[0] }
    New-NetFirewallRule @common | Out-Null
}

function Get-NetshLines {
    # netsh 를 못 돌렸으면 $null 이다. 빈 목록으로 돌리면 "값 줄 0" 과 구분되지 않는다.
    try {
        $raw = & netsh advfirewall show allprofiles firewallpolicy 2>&1
        if ($LASTEXITCODE -ne 0) { return $null }
        return , @($raw | ForEach-Object { [string]$_ })
    } catch { return $null }
}

function Invoke-Live([string]$Alias) {
    # 줄마다 하나를 내고 판정을 $script:Verdict 에 둔다.
    $fails = 0
    $unknowns = 0

    switch (Get-PolicyVerdict (Get-NetshLines)) {
        'fail' { Write-Output 'FAIL policy block_inbound_always'; $fails++ }
        'unknown' { Write-Output 'UNKNOWN policy netsh'; $unknowns++ }
    }

    foreach ($expected in $script:Rules) {
        $snap = Get-RuleSnap $expected.Name
        if ($null -ne $snap -and $snap['RuleCount'] -eq 0) {
            try {
                New-Rule $expected $Alias
                Write-Output ('CREATED {0}' -f $expected.Name)
            } catch {
                Write-Output ('FAIL {0} create' -f $expected.Name)
                $fails++
                continue
            }
            $snap = Get-RuleSnap $expected.Name
        }
        if ($null -eq $snap) {
            Write-Output ('UNKNOWN {0} query' -f $expected.Name)
            $unknowns++
            continue
        }
        foreach ($token in (Get-RuleFailures $expected $snap $Alias)) {
            if ($token -eq 'unreadable') {
                Write-Output ('UNKNOWN {0} filter' -f $expected.Name)
                $unknowns++
            } else {
                Write-Output ('FAIL {0} {1}' -f $expected.Name, $token)
                $fails++
            }
        }
    }
    $script:Verdict = Get-OverallVerdict $fails $unknowns
}

if ($SelfTest) {
    Invoke-SelfTest
    if ($script:SelfTestFailed -eq 0) { Write-Output 'SELFTEST: pass'; exit 0 }
    Write-Output 'SELFTEST: fail'
    exit 1
}

if (-not $InterfaceAlias) {
    Write-Output 'UNKNOWN args interface_alias'
    Write-Output 'VERDICT: unknown'
    exit 2
}

$script:Verdict = 'unknown'
try {
    Invoke-Live $InterfaceAlias
} catch {
    # 예상하지 못한 오류는 판정 불가다. 통과로 적지 않는다.
    Write-Output 'UNKNOWN script error'
    $script:Verdict = 'unknown'
}
Write-Output ('VERDICT: {0}' -f $script:Verdict)
switch ($script:Verdict) {
    'pass' { exit 0 }
    'fail' { exit 1 }
    default { exit 2 }
}
