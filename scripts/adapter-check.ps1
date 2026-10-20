# Phase 6 가상 어댑터의 같은 기기 확인 (roadmap.md Phase 6 검증). 관리자 권한이 있어야 한다.
#
# 진짜 Wintun 어댑터를 만들고 지우므로 scripts/test.ps1 이 돌리지 않는다. 관리자 PowerShell 에서 손으로 돌린다.
# 관리자가 아니면 판정하지 않고 종료 코드 2 로 끝난다. DynamoDB local 이 떠 있어야 한다 (control-server/README.md).
# 시험 빌드 클라이언트를 따로 짓는다. ICMP 응답기가 그 빌드에만 있다.
#
# 이 스크립트가 출력하는 문자열은 ASCII 로만 적는다. 이유는 build.ps1 머리에 있다.
#
#   pwsh -NoProfile -File scripts/adapter-check.ps1
#
# 장면
#   up         호스트로 방을 세우면 어댑터, 방화벽 규칙, 주소, 경로, MTU 가 서고, 미할당 가상 IP 로 ping 이 응답을
#              받고, 라우팅 범위 안의 빈 가상 IP 로 보낸 ping 은 tx_drop_no_route 다
#   leave      leave 하면 주소가 떨어지고 어댑터는 남는다
#   quit       quit 하면 어댑터와 경로가 남지 않는다
#   firewall   있던 방화벽 규칙은 검증하고 쓰고, 끈 규칙은 기동 실패다 (windows-prereq.md 2절)
#   second     어댑터를 든 프로세스가 있으면 두 번째 프로세스는 기동 실패다
#   crash      강제로 끝낸 뒤 다시 띄우면 어댑터가 다시 생기고 ping 이 다시 된다

[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')]
    [string]$Config = 'Debug',
    [string]$BuildDir = '',
    [string]$TestBuildDir = '',
    [string]$Ddb = 'http://127.0.0.1:8001'
)

$ErrorActionPreference = 'Stop'

$elevated = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $elevated) {
    Write-Host 'NOT RUN: this check creates a Wintun adapter and needs an elevated shell.'
    exit 2
}

. (Join-Path $PSScriptRoot 'buildpath.ps1')

$repoRoot = Split-Path -Parent $PSScriptRoot
$BuildDir = Resolve-BuildDir -Config $Config -Explicit $BuildDir
$py = Join-Path $repoRoot 'control-server\.venv\Scripts\python.exe'
if (-not (Test-Path -LiteralPath $py -PathType Leaf)) {
    throw "control-server venv not found: $py. See control-server/README.md (Tests) to create it."
}

$work = Join-Path ([System.IO.Path]::GetTempPath()) ("sangtachi_adapter_" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $work | Out-Null

$failed = 0
$notrun = 0
$clients = @()
$harness = $null

. (Join-Path $PSScriptRoot 'harness-lib.ps1')

# 어댑터의 이름과 GUID. 출처는 client/include/sangtachi/adapter.hpp 다 (windows-prereq.md 3절).
$adapterName = 'Sangtachi'
$adapterGuid = '{E2085BB3-83CE-4597-8DD3-68795CCFD00A}'

# ---------------------------------------------------------------- 조회

# "없음" 만 빈 목록으로 돌린다. 다른 오류는 올린다 (windows-prereq.md 3절 Invoke-NetQuerySafe).
function Invoke-NetQuerySafe([scriptblock]$Query) {
    try { return @(& $Query) }
    catch {
        $isNoMatch = ($_.CategoryInfo.Category -eq 'ObjectNotFound') -and
                     ($_.FullyQualifiedErrorId -like 'CmdletizationQuery_NotFound*')
        if ($isNoMatch) { return @() }
        throw
    }
}

# 클라이언트가 만드는 방화벽 규칙의 이름. 출처는 tools/winprereq/Initialize-FirewallRules.ps1 이다 (windows-prereq.md 2절).
$firewallRules = @('Sangtachi-TCP-25565-In', 'Sangtachi-ICMPv4-Echo-In')

# 우리 GUID 의 어댑터. 이름으로 찾지 않는다.
function Get-OurAdapter {
    return @(Get-NetAdapter -IncludeHidden -ErrorAction Stop | Where-Object { $_.InterfaceGuid -eq $adapterGuid })
}

function Get-OurAddresses([uint32]$ifIndex) {
    return Invoke-NetQuerySafe { Get-NetIPAddress -InterfaceIndex $ifIndex -AddressFamily IPv4 -ErrorAction Stop }
}

function Get-RouteSafe([string]$prefix) {
    return Invoke-NetQuerySafe { Get-NetRoute -DestinationPrefix $prefix -ErrorAction Stop }
}

function Wait-Until([scriptblock]$cond, [int]$timeoutMs = 10000) {
    $deadline = [Environment]::TickCount64 + $timeoutMs
    while ($true) {
        # 어댑터가 지워지는 동안 Get-NetAdapter 가 "삭제 표시된 레지스트리 키" 오류를 낸 적이 있다. 지나가는 상태라
        # 그 바퀴를 거짓으로 보고 다시 묻는다. 상한까지 참이 아니면 실패다.
        $ok = $false
        try { $ok = [bool](& $cond) } catch { $ok = $false }
        if ($ok) { return $true }
        if ([Environment]::TickCount64 -ge $deadline) { return $false }
        Start-Sleep -Milliseconds 200
    }
}

# .NET Ping 으로 묻는다. ping.exe 의 출력은 표시 언어를 따라 번역된다.
function Test-Echo([string]$ip, [int]$count = 4) {
    $ok = 0
    $pinger = New-Object System.Net.NetworkInformation.Ping
    try {
        for ($i = 0; $i -lt $count; $i++) {
            $reply = $pinger.Send($ip, 2000)
            if ($reply.Status -eq 'Success' -and $reply.Address.ToString() -eq $ip) { $ok++ }
        }
    } finally { $pinger.Dispose() }
    return $ok
}

# counters 를 쳐서 프로세스 표의 값 하나를 읽는다 (architecture.md 9장).
function Get-Counter($c, [string]$name) {
    $m = Get-Mark $c.err
    Send-Line $c 'counters'
    $x = Wait-Match $c.err $m "^INFO counter name=$name value=(\d+)\r?$" 5000
    if ($null -eq $x) { return $null }
    return [uint64]$x.Groups[1].Value
}

function Quit-Client($c) {
    Send-Line $c 'quit'
    if (-not $c.proc.WaitForExit(15000)) { throw "$($c.name): did not exit after quit" }
}

# ---------------------------------------------------------------- 장면

function Scenario-UpLeaveQuit {
    $h = Start-Client 'ad-host' @() $script:testExe -Adapter
    Check 'up: the client logs adapter.open with our GUID' ((Read-Log $h.err) -match "INFO adapter\.open name=$adapterName guid=$([regex]::Escape($adapterGuid)) ")
    # windows-prereq.md 2절 클라이언트가 만드는 규칙. 처음이면 만들고, 있으면 검증하고 쓴다.
    Check 'up: the client logs adapter.firewall verdict=pass' ((Read-Log $h.err) -match 'INFO adapter\.firewall verdict=pass ')
    foreach ($rule in $firewallRules) {
        Check "up: exactly one rule named $rule" ((@(Invoke-NetQuerySafe { Get-NetFirewallRule -DisplayName $rule -ErrorAction Stop })).Count -eq 1)
    }
    $ours = Get-OurAdapter
    Check 'up: exactly one adapter has our GUID' ($ours.Count -eq 1)
    if ($ours.Count -ne 1) { return }
    $ifIndex = [uint32]$ours[0].ifIndex
    Check 'up: the adapter has our name' ($ours[0].Name -eq $adapterName)
    # windows-prereq.md 7절의 자기 어댑터 제외는 설명이 Wintun 인 어댑터만 뺀다.
    Check "up: the adapter description is Wintun ($($ours[0].InterfaceDescription))" ($ours[0].InterfaceDescription -like '*Wintun*')

    $ip = Get-NetIPInterface -InterfaceIndex $ifIndex -AddressFamily IPv4
    Check 'up: the interface MTU is 1400 (protocol.md 12)' ($ip.NlMtu -eq 1400)
    Check 'up: DAD transmits is 0 (windows-prereq.md 9)' ($ip.DadTransmits -eq 0)
    Check 'up: no IPv4 address before a room' ((@(Get-OurAddresses $ifIndex | Where-Object { $_.PrefixOrigin -ne 'WellKnown' -and $_.IPAddress -notlike '169.254.*' })).Count -eq 0)

    $room = Start-Room $h
    Check 'up: the client logs adapter.address for 10.100.0.1' ((Read-Log $h.err) -match 'INFO adapter\.address virtual_ip=10\.100\.0\.1 ')
    $addr = @(Get-OurAddresses $ifIndex | Where-Object { $_.IPAddress -eq '10.100.0.1' })
    Check 'up: 10.100.0.1/24 is on our adapter' ($addr.Count -eq 1 -and $addr[0].PrefixLength -eq 24)
    Check 'up: the address is Preferred' ($addr.Count -eq 1 -and "$($addr[0].AddressState)" -eq 'Preferred')

    # roadmap.md Phase 6 검증의 두 명령. 판정은 위의 조회이고 이 둘은 사람이 읽는 증거로 남긴다.
    $ipconfig = (ipconfig) -join "`n"
    Set-Content -LiteralPath (Join-Path $work 'ipconfig.txt') -Value $ipconfig -Encoding UTF8
    Check 'up: ipconfig shows 10.100.0.1' ($ipconfig -match '10\.100\.0\.1(\D|$)')
    $routes = @(Get-RouteSafe '10.100.0.0/24')
    Check 'up: 10.100.0.0/24 routes to our adapter' (($routes | Where-Object { $_.InterfaceIndex -eq $ifIndex }).Count -ge 1)
    Check 'up: no other interface has 10.100.0.0/24' (($routes | Where-Object { $_.InterfaceIndex -ne $ifIndex }).Count -eq 0)
    $routePrint = (route print -4) -join "`n"
    Set-Content -LiteralPath (Join-Path $work 'route-print.txt') -Value $routePrint -Encoding UTF8
    Check 'up: route print lists 10.100.0.0 255.255.255.0' ($routePrint -match '10\.100\.0\.0\s+255\.255\.255\.0')

    # windows-prereq.md 7절의 자기 어댑터 제외. 주소와 경로가 선 지금, GUID 를 주면 우리 것을 빼서 proceed 이고
    # 주지 않으면 우리 /24 경로 때문에 pick another range 다.
    # Windows PowerShell 은 pwsh 의 PSModulePath 를 물려받으면 자기 모듈을 불러오지 못한다 (cmake/Wintun.cmake).
    $script = Join-Path $repoRoot 'tools\winprereq\Test-SubnetOverlap.ps1'
    $savedModulePath = $env:PSModulePath
    Remove-Item Env:PSModulePath
    try {
        $withGuid = & powershell -NoProfile -ExecutionPolicy Bypass -File $script -Target 10.100.0.0/24 -OwnInterfaceGuid $adapterGuid -OwnAlias $adapterName
        $withGuidCode = $LASTEXITCODE
        $withoutGuid = & powershell -NoProfile -ExecutionPolicy Bypass -File $script -Target 10.100.0.0/24
        $withoutGuidCode = $LASTEXITCODE
    } finally {
        $env:PSModulePath = $savedModulePath
    }
    Set-Content -LiteralPath (Join-Path $work 'subnet-with-guid.txt') -Value $withGuid -Encoding UTF8
    Set-Content -LiteralPath (Join-Path $work 'subnet-without-guid.txt') -Value $withoutGuid -Encoding UTF8
    Check "subnet: with our GUID the live adapter is excluded and the verdict is proceed (exit $withGuidCode)" ($withGuidCode -eq 0)
    Check "subnet: without our GUID the same machine blocks (exit $withoutGuidCode)" ($withoutGuidCode -eq 1)

    # windows-prereq.md 2절. 새 어댑터의 네트워크 프로필은 관측으로만 남긴다. 규칙이 프로필과 무관하게 걸린다.
    $profile = @(Invoke-NetQuerySafe { Get-NetConnectionProfile -InterfaceIndex $ifIndex -ErrorAction Stop })
    Write-Host ("note  up: network category of our adapter: {0}" -f (($profile | ForEach-Object { "$($_.NetworkCategory)" }) -join ','))

    # 같은 절의 판정 스크립트가 클라이언트가 만든 규칙을 따로 판정한다. 범위가 우리 어댑터와 기대 엔드포인트로
    # 좁혀졌는지 본다.
    $policyScript = Join-Path $repoRoot 'tools\winprereq\Test-FirewallPolicy.ps1'
    $savedModulePath = $env:PSModulePath
    Remove-Item Env:PSModulePath
    try {
        $policy = & powershell -NoProfile -ExecutionPolicy Bypass -File $policyScript -InterfaceAlias $adapterName
        $policyCode = $LASTEXITCODE
    } finally {
        $env:PSModulePath = $savedModulePath
    }
    Set-Content -LiteralPath (Join-Path $work 'firewall-policy.txt') -Value $policy -Encoding UTF8
    Check "up: Test-FirewallPolicy passes for our adapter (exit $policyCode)" ($policyCode -eq 0)

    # roadmap.md Phase 7: 라우팅 대상이 없는 가상 IP 로 보낸 패킷은 폐기되고 카운터가 오른다. 10.100.0.3 은
    # 라우팅 범위 안이라 시험 빌드의 ICMP 응답기가 답하지 않는다 (concurrency.md 3장).
    $before = Get-Counter $h 'tx_drop_no_route'
    $replies = Test-Echo '10.100.0.3' 2
    $after = Get-Counter $h 'tx_drop_no_route'
    Check "route: ping 10.100.0.3 gets no reply (got $replies)" ($replies -eq 0)
    Check "route: tx_drop_no_route grew by at least 2 ($before -> $after)" ($null -ne $before -and $null -ne $after -and $after - $before -ge 2)
    Write-Host ("note  route: tx_drop_not_unicast so far: {0}" -f (Get-Counter $h 'tx_drop_not_unicast'))

    # 미할당 가상 IP 로 ping. 어댑터가 읽고(adapter_rx) 시험 빌드의 ICMP 응답기가 응답을 주입한다.
    $before = Get-Counter $h 'adapter_rx'
    $replies = Test-Echo '10.100.0.9' 4
    $after = Get-Counter $h 'adapter_rx'
    Check "up: ping 10.100.0.9 got 4 replies (got $replies)" ($replies -eq 4)
    Check "up: adapter_rx grew by at least 4 ($before -> $after)" ($null -ne $before -and $null -ne $after -and $after - $before -ge 4)
    Check 'up: drop_inject_ring_full is 0' ((Get-Counter $h 'drop_inject_ring_full') -eq 0)
    Check 'up: drop_inject_error is 0' ((Get-Counter $h 'drop_inject_error') -eq 0)
    Check 'up: rx_err_adapter is 0' ((Get-Counter $h 'rx_err_adapter') -eq 0)

    # leave: 주소가 떨어지고 어댑터는 남는다 (concurrency.md 7장 로비).
    Send-Line $h 'leave'
    Check 'leave: the address is gone' (Wait-Until { (@(Get-OurAddresses $ifIndex | Where-Object { $_.IPAddress -eq '10.100.0.1' })).Count -eq 0 })
    Check 'leave: the route is gone' (Wait-Until { (@(Get-RouteSafe '10.100.0.0/24')).Count -eq 0 })
    Check 'leave: the adapter is still there' ((Get-OurAdapter).Count -eq 1)

    # 다시 방을 세우면 같은 어댑터에 주소가 다시 선다.
    $null = Start-Room $h
    Check 'rejoin: 10.100.0.1 is back on the same adapter' ((@(Get-OurAddresses $ifIndex | Where-Object { $_.IPAddress -eq '10.100.0.1' })).Count -eq 1)

    Quit-Client $h
    Check 'quit: exit code 0' ($h.proc.ExitCode -eq 0)
    Check 'quit: the client logs adapter.close' ((Read-Log $h.err) -match 'INFO adapter\.close ')
    Check 'quit: no adapter has our GUID' (Wait-Until { (Get-OurAdapter).Count -eq 0 })
    Check 'quit: no route for 10.100.0.0/24' (Wait-Until { (@(Get-RouteSafe '10.100.0.0/24')).Count -eq 0 })
}

function Scenario-Second {
    $a = Start-Client 'ad-first' @() $script:testExe -Adapter
    $b = Start-Logged 'ad-second' $script:testExe ("--server 127.0.0.1:$script:cpPort --name second --stun 127.0.0.1:$($script:stunPorts[0]) --stun 127.0.0.1:$($script:stunPorts[1])") $script:repoRoot
    $script:clients += $b
    $exited = $b.proc.WaitForExit(60000)
    Check 'second: the second process exits' $exited
    if ($exited) {
        Check 'second: with the startup failure code 2' ($b.proc.ExitCode -eq 2)
        Check 'second: it logs adapter.error reason=open' ((Read-Log $b.err) -match 'ERROR adapter\.error reason=open op=WintunCreateAdapter ')
    }
    Check 'second: the first adapter is untouched' ((Get-OurAdapter).Count -eq 1)
    Quit-Client $a
    Check 'second: the adapter is gone after the first quits' (Wait-Until { (Get-OurAdapter).Count -eq 0 })
}

function Scenario-Firewall {
    # windows-prereq.md 2절. 앞 장면이 규칙을 만들었다. 이번 실행은 있던 규칙을 검증하고 그대로 쓴다.
    $a = Start-Client 'fw-existing' @() $script:testExe -Adapter
    Check 'firewall: an existing rule set is verified and used (created=-)' ((Read-Log $a.err) -match '(?m)^INFO adapter\.firewall verdict=pass created=-\r?$')
    Quit-Client $a
    Check 'firewall: the adapter is gone after quit' (Wait-Until { (Get-OurAdapter).Count -eq 0 })

    # 규칙 하나를 끄면 검증이 어긋나 기동 실패다. 클라이언트는 그 규칙을 고치지 않는다. 끝에 다시 켠다.
    $rule = $firewallRules[0]
    Disable-NetFirewallRule -DisplayName $rule -ErrorAction Stop
    try {
        $b = Start-Logged 'fw-disabled' $script:testExe ("--server 127.0.0.1:$script:cpPort --name fwoff --stun 127.0.0.1:$($script:stunPorts[0]) --stun 127.0.0.1:$($script:stunPorts[1])") $script:repoRoot
        $script:clients += $b
        $exited = $b.proc.WaitForExit(90000)
        Check 'firewall: a disabled rule makes startup fail' $exited
        if ($exited) {
            Check 'firewall: with the startup failure code 2' ($b.proc.ExitCode -eq 2)
            Check "firewall: it logs adapter.error reason=firewall with $($rule):enabled" ((Read-Log $b.err) -match "ERROR adapter\.error reason=firewall verdict=fail exit_code=1 failed=$([regex]::Escape($rule)):enabled ")
        }
        Check 'firewall: the client left the rule disabled' ("$((Get-NetFirewallRule -DisplayName $rule -ErrorAction Stop).Enabled)" -eq 'False')
        Check 'firewall: no adapter after the failed start' (Wait-Until { (Get-OurAdapter).Count -eq 0 } 15000)
    } finally {
        # 지속되는 규칙이다. 되돌리지 못하면 숨기지 않고 실패로 적는다.
        try {
            Enable-NetFirewallRule -DisplayName $rule -ErrorAction Stop
            Check 'firewall: the rule is enabled again after the scenario' ("$((Get-NetFirewallRule -DisplayName $rule -ErrorAction Stop).Enabled)" -eq 'True')
        } catch {
            Check "firewall: re-enabling $rule failed: $_" $false
        }
    }
}

function Scenario-Crash {
    $a = Start-Client 'ad-crash' @() $script:testExe -Adapter
    $null = Start-Room $a
    Check 'crash: the adapter is up before the kill' ((Get-OurAdapter).Count -eq 1)
    $a.proc.Kill()
    [void]$a.proc.WaitForExit(5000)
    # Wintun 은 만든 프로세스가 끝나면 어댑터를 지운다 (windows-prereq.md 3절). 관측으로 남긴다.
    $gone = Wait-Until { (Get-OurAdapter).Count -eq 0 } 15000
    Check 'crash: no adapter with our GUID after the kill' $gone
    Write-Host ("note  crash: adapters with our GUID after the kill: {0}" -f ((Get-OurAdapter | ForEach-Object { "$($_.Name)/$($_.Status)" }) -join ', '))

    $b = Start-Client 'ad-again' @() $script:testExe -Adapter
    Check 'crash: the next start opens the adapter again' ((Read-Log $b.err) -match 'INFO adapter\.open ')
    $null = Start-Room $b
    Check 'crash: 10.100.0.1 is assigned again' ((Read-Log $b.err) -match 'INFO adapter\.address virtual_ip=10\.100\.0\.1 ')
    Check 'crash: ping 10.100.0.9 works again' ((Test-Echo '10.100.0.9' 2) -eq 2)
    Quit-Client $b
    Check 'crash: no adapter after the second quit' (Wait-Until { (Get-OurAdapter).Count -eq 0 })
}

# ---------------------------------------------------------------- 실행

try {
    if ((Get-OurAdapter).Count -ne 0) {
        throw "an adapter with our GUID already exists. Another client may be running."
    }
    $script:testExe = Build-TestClient $Config $BuildDir $TestBuildDir
    if (-not (Start-Harness $Ddb)) { exit 1 }
    foreach ($s in @('Scenario-UpLeaveQuit', 'Scenario-Firewall', 'Scenario-Second', 'Scenario-Crash')) {
        Write-Host "--- $s"
        try { & $s } catch {
            Write-Host ("FAIL  {0} threw: {1}" -f $s, $_)
            $script:failed++
            foreach ($c in $script:clients) { if (-not $c.proc.HasExited) { $c.proc.Kill() } }
        }
    }
} finally {
    Stop-Harness
}

Write-Host ("logs: {0}" -f $work)
Write-Host ("{0} checks failed, {1} not run" -f $failed, $notrun)
if ($failed -gt 0) { exit 1 }
Write-Host 'adapter check passed'
