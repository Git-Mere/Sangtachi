# Phase 5 의 오래 걸리는 같은 기기 확인 (roadmap.md Phase 5 검증의 제어 평면 중단 10분, 유휴 타임아웃, 1시간 유지).
# ctest 케이스 상한에 들지 않으므로 scripts/test.ps1 이 돌리지 않는다. 손으로 돌린다.
# 하네스와 클라이언트를 띄우는 도구는 harness-lib.ps1 이고 판정 방식은 session-check.ps1 과 같다. 장면마다 하네스를 새로 띄운다.
# cp-down 이 제어 서버를 내리기 때문이다.
#
# 이 스크립트가 출력하는 문자열은 ASCII 로만 적는다. 이유는 build.ps1 머리에 있다.
# DynamoDB local 이 떠 있어야 한다 (control-server/README.md). 먼저 scripts/build.ps1 로 제품 빌드를 짓는다.
#
#   pwsh -NoProfile -File scripts/soak-check.ps1 [-Only cp-down,idle-drop,hour] [-CpDownMinutes 10] [-HourMinutes 60]
#
# 장면                         걸리는 시간
#   idle-drop                  약 3분
#   cp-down                    -CpDownMinutes 에 1분 남짓
#   hour                       -HourMinutes 에 1분 남짓
# 분 인자를 줄여 돌리는 것은 스크립트 확인용이다. roadmap 의 판정은 기본값(10분, 60분)으로 돈 실행만 한다.

[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')]
    [string]$Config = 'Debug',
    [string]$BuildDir = '',
    [string]$TestBuildDir = '',
    [string[]]$Only = @(),
    [ValidateRange(1, 600)]
    [int]$CpDownMinutes = 10,
    [ValidateRange(1, 600)]
    [int]$HourMinutes = 60,
    [string]$Ddb = 'http://127.0.0.1:8001'
)

$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'buildpath.ps1')

$repoRoot = Split-Path -Parent $PSScriptRoot
$BuildDir = Resolve-BuildDir -Config $Config -Explicit $BuildDir
$exe = Join-Path $BuildDir 'client\sangtachi_client.exe'
if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
    throw "client executable not found: $exe. Run scripts/build.ps1 first."
}
$py = Join-Path $repoRoot 'control-server\.venv\Scripts\python.exe'
if (-not (Test-Path -LiteralPath $py -PathType Leaf)) {
    throw "control-server venv not found: $py. See control-server/README.md (Tests) to create it."
}

$scenarioNames = @('idle-drop', 'cp-down', 'hour')
$Only = @($Only | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
foreach ($n in $Only) {
    if ($scenarioNames -notcontains $n) { throw "unknown scenario '$n'. Known: $($scenarioNames -join ', ')" }
}

$work = Join-Path ([System.IO.Path]::GetTempPath()) ("sangtachi_soak_" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $work | Out-Null

$failed = 0
$notrun = 0
$clients = @()
$harness = $null

. (Join-Path $PSScriptRoot 'harness-lib.ps1')

# ---------------------------------------------------------------- 이 스크립트의 도구 (session-check.ps1 과 같은 모양)

$connected = 'session\.state peer_id=(\d+) from=\S+ to=CONNECTED'
$ended = 'session\.state peer_id=\d+ from=\S+ to=(CLOSED|FAILED)'

function Join-Room($c, [string]$room) {
    $om = Get-Mark $c.out
    Send-Line $c "join $room"
    if ($null -eq (Wait-Match $c.out $om '^ROOM ' 10000)) { throw "$($c.name): no ROOM line after join" }
}

function Wait-Connected($c, [int]$mark = 0, [int]$timeoutMs = 30000) {
    $m = Wait-Match $c.err $mark $connected $timeoutMs
    if ($null -eq $m) { return $null }
    return [uint32]$m.Groups[1].Value
}

# 호스트 하나와 플레이어 하나를 붙여 둘 다 CONNECTED 에 닿기를 기다린다.
function New-Pair([string]$prefix, [string]$exePath) {
    $h = Start-Client "$prefix-host" @('--name', "${prefix}H") $exePath
    $room = Start-Room $h
    $p = Start-Client "$prefix-p1" @('--name', "${prefix}P1") $exePath
    $hm = Get-Mark $h.err
    Join-Room $p $room
    $p.peerOfHost = Wait-Connected $p 0
    if ($null -eq $p.peerOfHost) { throw "$($p.name): did not reach CONNECTED" }
    $p.id = Wait-Connected $h $hm
    if ($null -eq $p.id) { throw "$($h.name): did not reach CONNECTED with $($p.name)" }
    return @{ host = $h; player = $p; room = $room }
}

function Get-Records($c, [string]$exePath) {
    $m = [regex]::Match((Read-Log $c.err), 'record\.open file=(\S+)')
    if (-not $m.Success) { throw "$($c.name): no record.open line" }
    $path = Join-Path (Join-Path (Split-Path -Parent $exePath) 'records') $m.Groups[1].Value
    $lines = @()
    foreach ($raw in ((Read-Log $path) -split "`n")) {
        if ($raw -eq '') { continue }
        $fields = [ordered]@{}
        foreach ($pair in ($raw -split ' ')) {
            $k, $v = $pair -split '=', 2
            $fields[$k] = $v
        }
        $lines += [pscustomobject]@{ raw = $raw; f = $fields }
    }
    return ,$lines
}

function Quit-Client($c) {
    Send-Line $c 'quit'
    if (-not $c.proc.WaitForExit(10000)) { throw "$($c.name): did not exit after quit" }
}

# 한 쪽이 `send n` 을 치고 에코 n 개가 돌아오는지 본다. 시험 빌드의 에코 응답기다 (roadmap.md Phase 4 RTT 기준선).
function Test-Echo($c, [int]$n, [string]$what) {
    $m = Get-Mark $c.err
    Send-Line $c "send $n"
    return (Wait-Count $c.err $m 'data\.echo rtt_ms=\d+' $n 10000)
}

# counters 를 쳐서 그 상대의 session.loss 줄 하나를 읽는다 (architecture.md 9장).
function Get-Loss($c, [uint32]$peerId) {
    $m = Get-Mark $c.err
    Send-Line $c 'counters'
    $x = Wait-Match $c.err $m "session\.loss peer_id=$peerId expected=(\d+) accepted=(\d+) lost=(\d+)" 5000
    if ($null -eq $x) { return $null }
    return @{ expected = [uint64]$x.Groups[1].Value; accepted = [uint64]$x.Groups[2].Value; lost = [uint64]$x.Groups[3].Value }
}

# ---------------------------------------------------------------- 장면

function Scenario-IdleDrop {
    # 유휴 타임아웃 (roadmap.md Phase 5, FR-13). 상대 프로세스를 강제로 끝내 CLOSE 없이 송신을 끊는다.
    # 남은 쪽은 50초 유휴 뒤 CONNECTED -> FAILED 로 TUNNEL_DROPPED 를 기록한다 (protocol.md 9.6, 11장).
    # 자동 재시도는 없다. 플레이어는 로비로 가고 스스로 다시 참가하지 않는다. 호스트는 방에 남는다 (protocol.md 9.6).

    # (1) 플레이어가 사라진 호스트
    $r = New-Pair 'idh' $script:exe
    $h = $r.host; $p = $r.player
    $om = Get-Mark $h.out
    $em = Get-Mark $h.err
    $p.proc.Kill()
    [void]$p.proc.WaitForExit(5000)
    Check 'idle-drop: the host records TUNNEL_DROPPED for the vanished player' (Wait-Count $h.err $em "session\.failed peer_id=$($p.id) code=TUNNEL_DROPPED" 1 70000)
    Check 'idle-drop: the host transition is CONNECTED to FAILED' ((Get-Count $h.err $em "session\.state peer_id=$($p.id) from=CONNECTED to=FAILED") -eq 1)
    Check 'idle-drop: the host prints FAIL TUNNEL_DROPPED with the player name' (Wait-Count $h.out $om '^FAIL TUNNEL_DROPPED idhP1 ' 1 3000)
    $hm = H-Mark
    Start-Sleep -Milliseconds 6500
    Check 'idle-drop: the host stays in the room (host_report goes on, no lobby)' (((H-Count $hm (Req 'host_report' '-')) -ge 1) -and ((Get-Count $h.err $em 'attempt\.end ') -eq 0))
    Quit-Client $h
    $lines = Get-Records $h $script:exe
    Check 'idle-drop: the host record ends TUNNEL_DROPPED' ($lines.Count -eq 2 -and $lines[1].f.kind -eq 'end' -and $lines[1].f.result -eq 'TUNNEL_DROPPED')

    # (2) 호스트가 사라진 플레이어
    $r = New-Pair 'idp' $script:exe
    $h = $r.host; $p = $r.player
    $om = Get-Mark $p.out
    $em = Get-Mark $p.err
    $h.proc.Kill()
    [void]$h.proc.WaitForExit(5000)
    Check 'idle-drop: the player records TUNNEL_DROPPED for the vanished host' (Wait-Count $p.err $em "session\.failed peer_id=$($p.peerOfHost) code=TUNNEL_DROPPED" 1 70000)
    Check 'idle-drop: the player transition is CONNECTED to FAILED' ((Get-Count $p.err $em "session\.state peer_id=$($p.peerOfHost) from=CONNECTED to=FAILED") -eq 1)
    Check 'idle-drop: the player prints FAIL TUNNEL_DROPPED with the host name' (Wait-Count $p.out $om '^FAIL TUNNEL_DROPPED idpH ' 1 3000)
    Check 'idle-drop: the player goes to the lobby' (Wait-Count $p.err $em 'attempt\.end ' 1 5000)
    $jm = Get-Mark $p.err
    $hm = H-Mark
    Start-Sleep -Seconds 20
    Check 'idle-drop: the player does not join again on its own' (((Get-Count $p.err $jm 'control\.result op=join_room') -eq 0) -and ((H-Count $hm (Req 'join_room')) -eq 0))
    Quit-Client $p
    $lines = Get-Records $p $script:exe
    Check 'idle-drop: the player record ends TUNNEL_DROPPED' ($lines.Count -eq 2 -and $lines[1].f.kind -eq 'end' -and $lines[1].f.result -eq 'TUNNEL_DROPPED')
}

function Scenario-CpDown {
    # 제어 평면 중단 (roadmap.md Phase 5, spec.md NFR-3). CONNECTED 뒤 제어 서버를 내려도 DATA 왕복과 세션 상태가
    # -CpDownMinutes 동안 유지된다. 30초마다 양쪽에서 에코 3개를 본다.
    $r = New-Pair 'cd' $script:testExe
    $h = $r.host; $p = $r.player
    $hm0 = Get-Mark $h.err
    $pm0 = Get-Mark $p.err
    Invoke-Admin 'quit' | Out-Null
    Check 'cp-down: the control server is down' ($script:harness.proc.WaitForExit(30000))
    $deadline = [Environment]::TickCount64 + $CpDownMinutes * 60000
    $rounds = 0
    $bad = 0
    while ([Environment]::TickCount64 -lt $deadline) {
        Start-Sleep -Seconds 30
        $rounds++
        if (-not (Test-Echo $p 3 'player')) { $bad++ }
        if (-not (Test-Echo $h 3 'host')) { $bad++ }
    }
    Check "cp-down: DATA round trips kept working for $CpDownMinutes minutes ($rounds rounds, $bad misses)" ($rounds -ge 1 -and $bad -eq 0)
    Check 'cp-down: no session ended while the control server was down' (((Get-Count $h.err $hm0 $ended) -eq 0) -and ((Get-Count $p.err $pm0 $ended) -eq 0))
    Check 'cp-down: the host did try the control server and failed (it was really down)' ((Get-Count $h.err $hm0 'control\.result op=host_report ok=false') -ge 1)
    Quit-Client $p
    Quit-Client $h
    foreach ($c in @($h, $p)) {
        $lines = Get-Records $c $script:testExe
        Check "cp-down: $($c.name) record is OK then CLOSED" ($lines.Count -eq 2 -and $lines[0].f.result -eq 'OK' -and $lines[1].f.result -eq 'CLOSED')
    }
}

function Scenario-Hour {
    # 장시간 유지 (roadmap.md Phase 5). -HourMinutes 동안 세션이 CONNECTED 에 머문다. 1분마다 양쪽에서 에코 5개를 보고,
    # 끝에 session.loss 를 읽는다. 같은 기기 안의 송수신이므로 lost=0 을 기대한다. 손실이 나면 실패로 보고 그 실행을 살핀다.
    $r = New-Pair 'hr' $script:testExe
    $h = $r.host; $p = $r.player
    $hm0 = Get-Mark $h.err
    $pm0 = Get-Mark $p.err
    $deadline = [Environment]::TickCount64 + $HourMinutes * 60000
    $rounds = 0
    $bad = 0
    while ([Environment]::TickCount64 -lt $deadline) {
        Start-Sleep -Seconds 60
        $rounds++
        if (-not (Test-Echo $p 5 'player')) { $bad++ }
        if (-not (Test-Echo $h 5 'host')) { $bad++ }
    }
    Check "hour: DATA round trips kept working for $HourMinutes minutes ($rounds rounds, $bad misses)" ($rounds -ge 1 -and $bad -eq 0)
    Check 'hour: the sessions stayed CONNECTED' (((Get-Count $h.err $hm0 $ended) -eq 0) -and ((Get-Count $p.err $pm0 $ended) -eq 0))
    Check 'hour: KEEPALIVE kept going' ((Get-Count $h.err $hm0 'timer\.tick name=session\.keepalive\.\d+') -ge [math]::Floor($HourMinutes * 60 / 15) - 1)
    foreach ($pair in @(@($h, $p.id), @($p, $p.peerOfHost))) {
        $loss = Get-Loss $pair[0] $pair[1]
        Check "hour: $($pair[0].name) session.loss has traffic and no loss" ($null -ne $loss -and $loss.expected -gt 0 -and $loss.lost -eq 0)
    }
    Quit-Client $p
    Quit-Client $h
}

# ---------------------------------------------------------------- 실행

try {
    $script:testExe = Build-TestClient $Config $BuildDir $TestBuildDir
    $plan = [ordered]@{ 'idle-drop' = { Scenario-IdleDrop }; 'cp-down' = { Scenario-CpDown }; 'hour' = { Scenario-Hour } }
    foreach ($name in $plan.Keys) {
        if ($Only.Count -and $Only -notcontains $name) { continue }
        Write-Host "--- $name"
        if (-not (Start-Harness $Ddb)) { break }
        try {
            & $plan[$name]
        } catch {
            Write-Host ("FAIL  {0}: scenario aborted: {1}" -f $name, ($_.Exception.Message -replace '[^\x20-\x7E]', '?'))
            $failed++
        }
        Stop-Harness
        $script:clients = @()
        $script:harness = $null
    }
} finally {
    Stop-Harness
    if ($failed -gt 0) {
        Write-Host "logs kept in $work"
    } else {
        Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Write-Host ("{0} checks failed, {1} not run" -f $failed, $notrun)
if ($failed -gt 0) { exit 1 }
exit 0
