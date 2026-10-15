# Phase 4~5 의 같은 기기 확인 (roadmap.md Phase 4·5 검증 가운데 프로세스 여럿과 제어 서버가 있어야 하는 짧은 것).
# 진짜 클라이언트 여럿을 진짜 제어 서버(control-server/tests/harness/cp_harness.py) 위에서 띄워 방을 만들고 펀치해
# CONNECTED 까지 가게 한 뒤, 표준 출력(FAIL 줄), 표준 오류(로그), 하네스 로그, 로컬 기록 파일로 판정한다.
# 같은 기기의 프로세스들은 서로의 로컬 후보(LAN 주소)로 연결된다. 하네스의 STUN 응답기가 적는 반사 주소는
# 문서용 대역이라 닿지 않는다.
#
# 실패를 만드는 상대는 하네스의 가짜 피어다(fake_host, fake_player). 모드의 뜻은 cp_harness.py 머리에 있다.
# 와이어에서만 보이는 항목은 tools/phase4-capture/README.md 의 손 절차다. 여기서 보지 않는다.
#
# 이 스크립트가 출력하는 문자열은 ASCII 로만 적는다. 이유는 build.ps1 머리에 있다.
# DynamoDB local 이 떠 있어야 한다 (control-server/README.md). 시험 빌드가 필요한 장면은 -SkipTestBuild 면 NOTRUN 이다.
#
#   pwsh -NoProfile -File scripts/session-check.ps1 [-BuildDir <dir>] [-Only <이름>,...]

[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')]
    [string]$Config = 'Debug',
    [string]$BuildDir = '',
    [string]$TestBuildDir = '',
    [switch]$SkipTestBuild,
    [string[]]$Only = @(),
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

$scenarioNames = @('connect', 'data', 'sizes', 'host-stays', 'room-ended', 'departed-now', 'departed-retry', 'punch-fail',
    'handshake-fail', 'peer-close', 'records')
$Only = @($Only | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
foreach ($n in $Only) {
    if ($scenarioNames -notcontains $n) { throw "unknown scenario '$n'. Known: $($scenarioNames -join ', ')" }
}

$work = Join-Path ([System.IO.Path]::GetTempPath()) ("sangtachi_session_" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $work | Out-Null

$failed = 0
$notrun = 0
$clients = @()
$harness = $null

. (Join-Path $PSScriptRoot 'harness-lib.ps1')

# ---------------------------------------------------------------- 이 스크립트의 도구

$connected = 'session\.state peer_id=(\d+) from=\S+ to=CONNECTED'

# 플레이어를 그 방에 참가시킨다. 역할 인자 대신 로비 명령을 친다(같은 경로다. architecture.md 3.5).
function Join-Room($c, [string]$room) {
    $om = Get-Mark $c.out
    Send-Line $c "join $room"
    if ($null -eq (Wait-Match $c.out $om '^ROOM ' 10000)) { throw "$($c.name): no ROOM line after join" }
}

# 그 클라이언트의 로그에서 CONNECTED 에 닿은 상대 peer_id 를 기다린다. 표시 뒤의 첫 것이다.
function Wait-Connected($c, [int]$mark = 0, [int]$timeoutMs = 30000) {
    $m = Wait-Match $c.err $mark $connected $timeoutMs
    if ($null -eq $m) { return $null }
    return [uint32]$m.Groups[1].Value
}

# 호스트 하나와 플레이어 n 명을 붙인다. 모두 CONNECTED 에 닿기를 기다린다.
function New-Room([string]$prefix, [int]$players, [string]$exePath = $script:exe) {
    $h = Start-Client "$prefix-host" @('--name', "${prefix}H") $exePath
    $room = Start-Room $h
    $ps = @()
    for ($i = 1; $i -le $players; $i++) {
        $p = Start-Client "$prefix-p$i" @('--name', "${prefix}P$i") $exePath
        $hm = Get-Mark $h.err
        Join-Room $p $room
        $hostId = Wait-Connected $p 0
        if ($null -eq $hostId) { throw "$($p.name): did not reach CONNECTED" }
        $p.peerOfHost = $hostId
        $pid_ = Wait-Connected $h $hm
        if ($null -eq $pid_) { throw "$($h.name): did not reach CONNECTED with $($p.name)" }
        $p.id = $pid_
        $ps += $p
    }
    return @{ host = $h; room = $room; players = $ps }
}

# 그 클라이언트의 로컬 기록 파일의 줄 (architecture.md 9장). record.open 줄의 이름을 실행 파일 폴더 records\ 에서 찾는다.
function Get-Records($c, [string]$exePath = $script:exe) {
    $m = [regex]::Match((Read-Log $c.err), 'record\.open file=(\S+)')
    if (-not $m.Success) { throw "$($c.name): no record.open line" }
    $path = Join-Path (Join-Path (Split-Path -Parent $exePath) 'records') $m.Groups[1].Value
    $text = Read-Log $path
    $lines = @()
    foreach ($raw in ($text -split "`n")) {
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

$recordKeys = 'v kind ts attempt role peer result rtt_us setup_ms'

# 줄 하나가 9장 줄 형식인가. 필드 아홉이 그 순서이고 빈 값이 없다.
function Test-RecordLine($line) {
    return (($line.f.Keys -join ' ') -eq $recordKeys) -and (@($line.f.Values | Where-Object { $_ -eq '' }).Count -eq 0)
}

# quit 으로 끝내고 끝나기를 기다린다 (concurrency.md 7장 종료). 기록 줄은 종료 절차가 쓴다.
function Quit-Client($c) {
    Send-Line $c 'quit'
    if (-not $c.proc.WaitForExit(10000)) { throw "$($c.name): did not exit after quit" }
}

# ---------------------------------------------------------------- 장면

function Scenario-Connect {
    # 같은 LAN 의 두 PC 가 CONNECTED 에 닿는 것의 같은 기기 판. 학습한 주소가 상대의 로컬 후보다 (roadmap.md
    # Phase 4 직접 경로 수립). 플레이어가 ROSTER 를 받는다(5.7). 제품 빌드의 DATA 는 drop_no_sink 다.
    # 기록 파일이 프로세스마다 따로이고 수립 줄과 종료 줄이 9장 형식이다 (M-6).
    $r = New-Room 'cn' 1
    $h = $r.host; $p = $r.players[0]
    Check 'connect: host and player reached CONNECTED' $true
    foreach ($pair in @(@($h, $p), @($p, $h))) {
        $self = $pair[0]; $other = $pair[1]
        $peers = [regex]::Match((Read-Log $self.err), 'control\.peers peer_id=\d+ virtual_ip=\S+ candidates=(\S+)')
        $learned = [regex]::Match((Read-Log $self.err), 'session\.endpoint peer_id=\d+ endpoint=(\S+)')
        Check "connect: $($self.name) logged the learned endpoint" $learned.Success
        if ($peers.Success -and $learned.Success) {
            $cands = $peers.Groups[1].Value -split ','
            $ep = $learned.Groups[1].Value
            Check "connect: $($self.name) learned one of the received candidates ($ep)" ($cands -contains $ep)
            Check "connect: $($self.name) learned a local candidate, not the reflexive $script:mappedIp" (-not $ep.StartsWith("$script:mappedIp`:"))
            Check "connect: the learned port is the bound port of $($other.name)" ($ep.EndsWith(":$($other.port)"))
        }
    }
    Check 'connect: the player received a ROSTER with both names' (Wait-Count $p.err 0 'roster peer_id=\d+ generation=\d+ members=\S*cnH\S*cnP1' 1 20000)
    Send-Line $p 'send 3'
    Check 'connect: the player sent three DATA' (Wait-Count $p.err 0 'console\.send sent=3 requested=3' 1 5000)
    Start-Sleep -Milliseconds 500
    $em = Get-Mark $h.err
    Send-Line $h 'counters'
    Check 'connect: the product-build host drops them as drop_no_sink' (Wait-Count $h.err $em "counter name=drop_no_sink value=3 peer_id=$($p.id) " 1 5000)
    Quit-Client $p
    Quit-Client $h
    foreach ($pair in @(@($h, $p.id), @($p, $p.peerOfHost))) {
        $c = $pair[0]; $other = $pair[1]
        $lines = Get-Records $c
        Check "connect: $($c.name) has two record lines" ($lines.Count -eq 2)
        if ($lines.Count -eq 2) {
            Check "connect: $($c.name) lines are in the 9-field format" ((Test-RecordLine $lines[0]) -and (Test-RecordLine $lines[1]))
            $e = $lines[0].f; $z = $lines[1].f
            Check "connect: $($c.name) establish OK with a setup time and the peer" ($e.kind -eq 'establish' -and $e.result -eq 'OK' -and $e.peer -eq "$other" -and $e.setup_ms -match '^\d+$')
            Check "connect: $($c.name) end CLOSED with the same attempt and no setup time" ($z.kind -eq 'end' -and $z.result -eq 'CLOSED' -and $z.attempt -eq $e.attempt -and $z.setup_ms -eq '-')
        }
    }
    $hf = [regex]::Match((Read-Log $h.err), 'record\.open file=(\S+)').Groups[1].Value
    $pf = [regex]::Match((Read-Log $p.err), 'record\.open file=(\S+)').Groups[1].Value
    Check 'connect: the two processes write different files' ($hf -ne $pf)
}

# data.tx 나 data.rx 가운데 그 가상 IP 쪽 줄의 `len/sha256` 을 순서대로 (architecture.md 9장).
function Get-DataLines($c, [string]$event, [string]$key, [string]$vip) {
    $pattern = "$([regex]::Escape($event)) $key=$([regex]::Escape($vip)) len=(\d+) sha256=(\S+)"
    return @([regex]::Matches((Read-Log $c.err), $pattern) | ForEach-Object { "$($_.Groups[1].Value)/$($_.Groups[2].Value)" })
}

function Median([double[]]$values) {
    $s = @($values | Sort-Object)
    if ($s.Count -eq 0) { return [double]::NaN }
    if ($s.Count % 2) { return $s[[math]::Floor($s.Count / 2)] }
    return ($s[$s.Count / 2 - 1] + $s[$s.Count / 2]) / 2
}

function Scenario-Data {
    # M-5: 합성 내부 패킷의 DATA 왕복. 송신 측 data.tx 와 수신 측 data.rx 의 len 과 sha256 이 순서대로 같다.
    # RTT 기준선: PING/PONG 표본 20 의 중앙값과 DATA 에코 20 의 중앙값이 10ms 또는 30% 안이다 (roadmap.md Phase 4).
    if ($null -eq $script:testExe) {
        NotRun 'data: M-5 round trip and the RTT baseline' 'no test build (-SkipTestBuild)'
        return
    }
    $r = New-Room 'dt' 1 $script:testExe
    $h = $r.host; $p = $r.players[0]
    foreach ($c in @($h, $p)) {
        Check "data: $($c.name) is a test build" (Wait-Count $c.err 0 '^WARN build\.test build=test' 1 3000)
    }
    Send-Line $p 'send 20'
    Check 'data: the player sent 20' (Wait-Count $p.err 0 'console\.send sent=20 requested=20' 1 5000)
    Check 'data: 20 echoes came back' (Wait-Count $p.err 0 'data\.echo rtt_ms=\d+' 20 10000)
    Send-Line $h 'send 20 200'
    Check 'data: the host sent 20 of 200 bytes' (Wait-Count $h.err 0 'console\.send sent=20 requested=20' 1 5000)
    Check 'data: 20 echoes came back to the host' (Wait-Count $h.err 0 'data\.echo rtt_ms=\d+' 20 10000)
    Start-Sleep -Milliseconds 500
    $hostVip = [regex]::Match((Read-Log $p.err), 'control\.peers peer_id=\d+ virtual_ip=(\S+)').Groups[1].Value
    $playerVip = [regex]::Match((Read-Log $h.err), 'control\.peers peer_id=\d+ virtual_ip=(\S+)').Groups[1].Value
    # 요청과 에코가 두 방향으로 흐른다. 보낸 쪽의 data.tx(to=상대) 와 받은 쪽의 data.rx(from=보낸 쪽) 를 짝짓는다.
    $pairs = @(
        @{ what = 'player to host'; tx = (Get-DataLines $p 'data.tx' 'to' $hostVip); rx = (Get-DataLines $h 'data.rx' 'from' $playerVip) },
        @{ what = 'host to player'; tx = (Get-DataLines $h 'data.tx' 'to' $playerVip); rx = (Get-DataLines $p 'data.rx' 'from' $hostVip) }
    )
    foreach ($x in $pairs) {
        Check "data: $($x.what): 40 sent (20 requests and 20 echoes) ($($x.tx.Count))" ($x.tx.Count -eq 40)
        Check "data: $($x.what): what was received is exactly what was sent, in order" ($x.tx.Count -eq 40 -and ($x.tx -join ',') -eq ($x.rx -join ','))
    }
    # RTT 기준선. PING 은 진입 5초 뒤부터 5초마다다(11장). 20 표본에 100초가 넘는다.
    $need = 20
    $ok = Wait-Count $p.err 0 'session\.rtt peer_id=\d+ rtt_us=\d+' $need 120000
    Check "data: $need PING/PONG samples" $ok
    $ping = @([regex]::Matches((Read-Log $p.err), 'session\.rtt peer_id=\d+ rtt_us=(\d+)') | Select-Object -First $need | ForEach-Object { [double]$_.Groups[1].Value / 1000 })
    $echo = @([regex]::Matches((Read-Log $p.err), 'data\.echo rtt_ms=(\d+)') | Select-Object -First $need | ForEach-Object { [double]$_.Groups[1].Value })
    $mp = Median $ping; $me = Median $echo
    $abs = [math]::Abs($mp - $me)
    # 에코 중앙값이 0 이면 상대차가 뜻이 없다. 절대차 하나로만 본다.
    $within = ($abs -le 10) -or ($me -gt 0 -and $abs / $me -le 0.3)
    Check ("data: RTT baseline: PING median {0:N3} ms vs DATA echo median {1:N3} ms within 10 ms or 30%" -f $mp, $me) ($ping.Count -eq $need -and $echo.Count -eq $need -and $within)
}

function Get-DataLinesSince($c, [int]$mark, [string]$event, [string]$key, [string]$vip) {
    $pattern = "$([regex]::Escape($event)) $key=$([regex]::Escape($vip)) len=(\d+) sha256=(\S+)"
    return @([regex]::Matches((Get-Since $c.err $mark), $pattern) | ForEach-Object { "$($_.Groups[1].Value)/$($_.Groups[2].Value)" })
}

function Scenario-Sizes {
    # roadmap.md Phase 5 경계 크기 표의 프로세스 판. 20 은 편도다(방향 표시가 없어 에코하지 않는다). 576 과 1452 는
    # 왕복이다. 일치는 Phase 4 와 같은 비교 지점(data.tx, data.rx 의 len 과 sha256)으로 본다. 19 와 1453 은 send 가
    # 만들 수 없는 크기라 단위 시험(tests/tunnel/tunnel_test.cpp "the Phase 5 boundary sizes")이 본다.
    if ($null -eq $script:testExe) {
        NotRun 'sizes: the Phase 5 boundary sizes' 'no test build (-SkipTestBuild)'
        return
    }
    $r = New-Room 'sz' 1 $script:testExe
    $h = $r.host; $p = $r.players[0]
    $hostVip = [regex]::Match((Read-Log $p.err), 'control\.peers peer_id=\d+ virtual_ip=(\S+)').Groups[1].Value
    $playerVip = [regex]::Match((Read-Log $h.err), 'control\.peers peer_id=\d+ virtual_ip=(\S+)').Groups[1].Value
    foreach ($size in @(20, 576, 1452)) {
        $pm = Get-Mark $p.err
        $hm = Get-Mark $h.err
        Send-Line $p "send 1 $size"
        Check "sizes: the player sent one of $size bytes" (Wait-Count $p.err $pm 'console\.send sent=1 requested=1' 1 5000)
        Check "sizes: the host hook got $size bytes" (Wait-Count $h.err $hm "data\.rx from=$([regex]::Escape($playerVip)) len=$size " 1 5000)
        if ($size -eq 20) {
            Start-Sleep -Milliseconds 1000
            Check 'sizes: 20 bytes is one way, no echo comes back' ((Get-Count $p.err $pm 'data\.echo ') -eq 0)
        } else {
            Check "sizes: the echo of $size bytes came back" (Wait-Count $p.err $pm 'data\.echo rtt_ms=\d+' 1 5000)
            $back = Get-DataLinesSince $h $hm 'data.tx' 'to' $playerVip
            $got = Get-DataLinesSince $p $pm 'data.rx' 'from' $hostVip
            Check "sizes: the echo of $size bytes arrived byte for byte" ($back.Count -eq 1 -and ($back -join ',') -eq ($got -join ','))
        }
        $sent = Get-DataLinesSince $p $pm 'data.tx' 'to' $hostVip
        $recv = Get-DataLinesSince $h $hm 'data.rx' 'from' $playerVip
        Check "sizes: $size bytes arrived byte for byte" ($sent.Count -eq 1 -and ($sent -join ',') -eq ($recv -join ','))
    }
}

function Scenario-HostStays {
    # 호스트는 마지막 플레이어의 세션이 끝나도 방에 남는다 (concurrency.md 7장 로비). host_report 가 계속 나가고
    # 같은 방 코드로 다시 join 하면 연결된다.
    $r = New-Room 'hs' 1
    $h = $r.host; $p = $r.players[0]
    $em = Get-Mark $h.err
    Send-Line $p 'leave'
    Check 'host-stays: the host session ends CLOSED' (Wait-Count $h.err $em "session\.state peer_id=$($p.id) from=\S+ to=CLOSED" 1 10000)
    $hm = H-Mark
    Start-Sleep -Milliseconds 6500
    Check 'host-stays: host_report keeps going after the last player left' ((H-Count $hm (Req 'host_report' '-')) -ge 1)
    Check 'host-stays: the host did not go to the lobby' ((Get-Count $h.err 0 'attempt\.end ') -eq 0)
    $pm = Get-Mark $p.err
    Join-Room $p $r.room
    Check 'host-stays: the same player joins again and connects' ($null -ne (Wait-Connected $p $pm))
}

function Scenario-RoomEnded {
    # 방이 서버에서 끝나도 연결된 세션은 유지된다 (control_plane.md 4.6 오류 표, roadmap.md Phase 4).
    # DATA 왕복을 보려고 시험 빌드로 띄운다(에코 응답기). 없으면 제품 빌드로 돌고 그 확인만 NOTRUN 이다.
    $exeRe = if ($null -ne $script:testExe) { $script:testExe } else { $script:exe }
    $r = New-Room 're' 2 $exeRe
    $h = $r.host; $p1 = $r.players[0]; $p2 = $r.players[1]
    $om = Get-Mark $h.out
    $em = Get-Mark $h.err
    $hm = H-Mark
    Invoke-Admin 'clock offset_ms=200000' | Out-Null
    Check 'room-ended: host_report got room_expired' (Wait-Count $script:harness.err $hm (Req 'host_report' 'room_expired') 1 8000)
    Check 'room-ended: the host prints one FAIL CONTROL_PLANE_EXCHANGE_FAILED - line' (Wait-Count $h.out $om '^FAIL CONTROL_PLANE_EXCHANGE_FAILED - ' 1 3000)
    # 두 세션의 KEEPALIVE 가 계속된다. 15초 주기다.
    $km = Get-Mark $h.err
    Check 'room-ended: the sessions keep sending KEEPALIVE' (Wait-Count $h.err $km 'timer\.tick name=session\.keepalive\.\d+' 2 17000)
    Check 'room-ended: no session ended' ((Get-Count $h.err $em 'to=CLOSED|to=FAILED') -eq 0)
    if ($null -ne $script:testExe) {
        foreach ($p in @($p1, $p2)) {
            $dm = Get-Mark $p.err
            Send-Line $p 'send 3'
            Check "room-ended: DATA still round-trips for $($p.name) (three echoes)" (Wait-Count $p.err $dm 'data\.echo rtt_ms=\d+' 3 5000)
        }
    } else {
        NotRun 'room-ended: DATA round trips after the room ended' 'no test build (-SkipTestBuild)'
    }
    $hm = H-Mark
    $lm = Get-Mark $h.err
    Send-Line $p1 'leave'
    Check 'room-ended: the leaving pair ends' (Wait-Count $h.err $lm "session\.state peer_id=$($p1.id) from=\S+ to=CLOSED" 1 10000)
    Start-Sleep -Milliseconds 6000
    Check 'room-ended: no host_report after the room ended' ((H-Count $hm (Req 'host_report')) -eq 0)
    Check 'room-ended: the host is still in the room' ((Get-Count $h.err $em 'attempt\.end ') -eq 0)
    $p3 = Start-Client 're-p3' @('--name', 'reP3')
    $p3m = Get-Mark $p3.out
    Send-Line $p3 "join $($r.room)"
    Check 'room-ended: a new join fails with CONTROL_PLANE_EXCHANGE_FAILED' (Wait-Count $p3.out $p3m '^FAIL CONTROL_PLANE_EXCHANGE_FAILED - ' 1 10000)
    Check 'room-ended: the server said room_expired' ((H-Count $hm (Req 'join_room' 'room_expired')) -ge 1)
    Send-Line $p2 'leave'
    Check 'room-ended: when the last session ends the host goes to the lobby' (Wait-Count $h.err $em 'attempt\.end reason=room_ended' 1 10000)
    Check 'room-ended: exactly one FAIL line on the host' ((Get-Count $h.out $om '^FAIL ') -eq 1)
    Invoke-Admin 'clock offset_ms=0' | Out-Null

    # 따로 한 번 더. 두 세션이 남은 호스트가 leave 하면 세션을 닫고 바로 로비로 간다.
    $r = New-Room 're2' 2
    $h = $r.host
    $em = Get-Mark $h.err
    $pms = @($r.players | ForEach-Object { Get-Mark $_.err })
    Send-Line $h 'leave'
    Check 'room-ended: a host leave goes to the lobby' (Wait-Count $h.err $em 'attempt\.end reason=leave' 1 5000)
    for ($i = 0; $i -lt 2; $i++) {
        Check "room-ended: player $($i + 1) sees the session CLOSED and goes to the lobby" (Wait-Count $r.players[$i].err $pms[$i] 'attempt\.end reason=closed' 1 10000)
    }
}

function Scenario-DepartedNow {
    # 세션이 끝나면 host_report 가 다음 주기를 기다리지 않고 나간다 (control_plane.md 4.6). 정원이 찬 방에서
    # 플레이어 하나가 leave 하면 새 참가가 room_full 이 아니다. 판정은 호스트의 CLOSED 와 서버의
    # peer.released by=host 사이가 20초 이내인 것이다. leave 는 주기 호출의 응답 직후에 친다.
    $r = New-Room 'dn' 4
    $h = $r.host; $p1 = $r.players[0]
    $hm = H-Mark
    if (-not (Wait-Count $script:harness.err $hm (Req 'host_report' '-') 1 35000)) { throw 'no periodic host_report' }
    $em = Get-Mark $h.err
    $hm = H-Mark
    Send-Line $p1 'leave'
    $closed = Wait-Count $h.err $em "session\.state peer_id=$($p1.id) from=\S+ to=CLOSED" 1 10000
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $released = Wait-Count $script:harness.err $hm "peer\.released peer_id=$($p1.id) \S+ by=host" 1 25000
    $sw.Stop()
    Check 'departed-now: the host session ended' $closed
    Check "departed-now: the server released the peer by=host within 20 s of CLOSED ($($sw.ElapsedMilliseconds) ms)" ($released -and $sw.ElapsedMilliseconds -le 20000)
    $p5 = Start-Client 'dn-p5' @('--name', 'dnP5')
    $hm = H-Mark
    Join-Room $p5 $r.room
    Check 'departed-now: the next join is answered, not room_full' ((H-Count $hm (Req 'join_room' '-')) -ge 1)
}

function Scenario-DepartedRetry {
    # 호스트가 보고하지 못한 departed 는 다음 호출에 다시 담긴다 (control_plane.md 4.6). 첫 즉시 보고를 2초 붙들었다가
    # 실패시키고, 그 사이에 다른 세션이 끝나게 한다. 다음 성공 호출 하나가 두 피어를 다 회수해야 한다. 붙드는 시간은
    # 클라이언트의 수신 상한(3초, control_plane.md 8.2)보다 짧아야 한다. 길면 클라이언트가 먼저 끊고 다음 요청을 보내
    # 서버에서 두 요청이 겹친다.
    $r = New-Room 'dr' 2
    $h = $r.host; $p1 = $r.players[0]; $p2 = $r.players[1]
    $hm = H-Mark
    Invoke-Admin 'delay op=host_report count=1 ms=2000 when=before' | Out-Null
    Invoke-Admin 'error op=host_report count=1 code=internal when=before' | Out-Null
    Send-Line $p1 'leave'
    if (-not (Wait-Count $script:harness.err $hm '^HARNESS inject kind=delay op=host_report' 1 15000)) { throw 'the first host_report was not held' }
    $em = Get-Mark $h.err
    Send-Line $p2 'leave'
    Check 'departed-retry: the second session ended while the first report was held' (Wait-Count $h.err $em "session\.state peer_id=$($p2.id) from=\S+ to=CLOSED" 1 1500)
    Check 'departed-retry: the held host_report failed with internal' (Wait-Count $script:harness.err $hm (Req 'host_report' 'internal') 1 10000)
    [void](Wait-Count $script:harness.err $hm "peer\.released peer_id=$($p2.id) \S+ by=host" 1 20000)
    # 실패한 요청 뒤 첫 성공 host_report 의 줄까지에 두 회수 줄이 다 있어야 한다. 서버는 회수 줄을 내고 나서 그 요청의 줄을 낸다.
    $text = Get-Since $script:harness.err $hm
    $fail = [regex]::Match($text, '^INFO http\.request op=host_report status=\d+ error=internal .*$', 'Multiline')
    $next = if ($fail.Success) { [regex]::Match($text.Substring($fail.Index + $fail.Length), '^INFO http\.request op=host_report status=\d+ error=- .*$', 'Multiline') } else { $null }
    $between = if ($next -and $next.Success) { $text.Substring($fail.Index + $fail.Length, $next.Index) } else { '' }
    Check 'departed-retry: the next successful host_report released the first peer by=host' ($between -match "peer\.released peer_id=$($p1.id) \S+ by=host")
    Check 'departed-retry: and in the same call the second peer by=host' ($between -match "peer\.released peer_id=$($p2.id) \S+ by=host")
}

# 가짜 상대에게 붙는 플레이어 하나. expect 줄을 기다린다. 기다림에는 준비까지의 폴링(가짜 호스트의 host_report 가 5초
# 주기다), 펀치 지연 1초, 펀치 마감 10초가 든다.
function Join-Fake([string]$mode, [string]$fakeName, [int]$waitMs, [string]$expect) {
    $fh = Invoke-Admin "fake_host name=$fakeName mode=$mode"
    $p = Start-Client "fake-$mode" @('--name', "pf$($mode -replace '[^a-z0-9]', '')")
    $om = Get-Mark $p.out
    $em = Get-Mark $p.err
    Join-Room $p $fh.room
    $ok = Wait-Count $p.err $em $expect 1 $waitMs
    return @{ p = $p; ok = $ok; om = $om; em = $em; fake = $fh }
}

function Scenario-PunchFail {
    # 응답 없는 엔드포인트 -> HOLE_PUNCH_TIMEOUT. FAIL 줄의 <상대> 는 그 상대의 이름이고 프로세스는 끝나지 않는다.
    # 플레이어는 로비로, 호스트는 방에 남는다. 호스트의 다른 쌍은 그대로 CONNECTED 다. 끝난 세션의 HELLO
    # 재전송이 멈춘다 (protocol.md 9.6. 와이어 확인은 캡처 체크리스트).
    $x = Join-Fake 'silent' 'FakeHost' 35000 'attempt\.end reason=failed'
    $p = $x.p
    Check 'punch-fail: the player prints FAIL HOLE_PUNCH_TIMEOUT FakeHost' (Wait-Count $p.out $x.om '^FAIL HOLE_PUNCH_TIMEOUT FakeHost ' 1 1000)
    Check 'punch-fail: the player went to the lobby' $x.ok
    Check 'punch-fail: the player process stays' (-not $p.proc.HasExited)

    $r = New-Room 'pf' 1
    $h = $r.host; $real = $r.players[0]
    $om = Get-Mark $h.out
    $em = Get-Mark $h.err
    $fp = Invoke-Admin "fake_player room=$($r.room) name=FakeP mode=silent"
    Check 'punch-fail: the host prints FAIL HOLE_PUNCH_TIMEOUT FakeP' (Wait-Count $h.out $om '^FAIL HOLE_PUNCH_TIMEOUT FakeP ' 1 30000)
    Check 'punch-fail: the host stays in the room' ((Get-Count $h.err $em 'attempt\.end ') -eq 0)
    Check 'punch-fail: the other pair is still CONNECTED' ((Get-Count $h.err $em "session\.state peer_id=$($real.id) from=\S+ to=(CLOSED|FAILED)") -eq 0)
    $tm = Get-Mark $h.err
    Start-Sleep -Milliseconds 1500
    Check 'punch-fail: no HELLO retransmit timer after the failure' ((Get-Count $h.err $tm "timer\.tick name=session\.hello\.$($fp.peer_id) ") -eq 0)
    Invoke-Admin 'fake_clear' | Out-Null
}

function Scenario-HandshakeFail {
    # 한 방향만 열림 -> PEER_HANDSHAKE_FAILED (protocol.md 9.6). 상대가 HELLO_ACK 만 보내는 길과 HELLO 만 보내는 길.
    foreach ($mode in @('ack_only', 'hello_only')) {
        $x = Join-Fake $mode "Fake$($mode -replace '_', '')" 35000 'attempt\.end reason=failed'
        Check "handshake-fail ($mode): FAIL PEER_HANDSHAKE_FAILED with the fake host name" (Wait-Count $x.p.out $x.om '^FAIL PEER_HANDSHAKE_FAILED Fake\S+ ' 1 1000)
        Invoke-Admin 'fake_clear' | Out-Null
        Quit-Client $x.p
        $lines = Get-Records $x.p
        Check "handshake-fail ($mode): one establish line PEER_HANDSHAKE_FAILED for the fake peer" ($lines.Count -eq 1 -and $lines[0].f.result -eq 'PEER_HANDSHAKE_FAILED' -and $lines[0].f.peer -eq "$($x.fake.peer_id)")
    }
}

function Scenario-PeerClose {
    # 수립 중에 상대의 CLOSE 를 받는다. 0x01 이면 ABORTED, 0x03 이면 PEER_FAILED (architecture.md 9장 결과 값).
    # FAIL 줄은 없다. 이쪽의 실패가 아니다.
    foreach ($row in @(@{ mode = 'close01'; result = 'ABORTED' }, @{ mode = 'close03'; result = 'PEER_FAILED' })) {
        $x = Join-Fake $row.mode "Fake$($row.mode)" 25000 'attempt\.end reason=closed'
        Check "peer-close ($($row.mode)): the player goes to the lobby without FAIL" ($x.ok -and (Get-Count $x.p.out $x.om '^FAIL ') -eq 0)
        Invoke-Admin 'fake_clear' | Out-Null
        Quit-Client $x.p
        $lines = Get-Records $x.p
        Check "peer-close ($($row.mode)): one establish line $($row.result)" ($lines.Count -eq 1 -and $lines[0].f.kind -eq 'establish' -and $lines[0].f.result -eq $row.result)
    }
}

function Scenario-Records {
    # 기록 줄 수 (architecture.md 9장 "무엇이 시도 하나인가", roadmap.md Phase 4 기록과 실패 코드).
    # (1) 호스트에 플레이어 둘. 하나는 연결되고 하나(가짜, silent)는 실패한다. 모두 끝내면 시도 둘, 줄 셋.
    $r = New-Room 'rc' 1
    $h = $r.host
    $om = Get-Mark $h.out
    $fr = Invoke-Admin "fake_player room=$($r.room) name=FakeR mode=silent"
    Check 'records: the fake pair timed out on the host' (Wait-Count $h.out $om '^FAIL HOLE_PUNCH_TIMEOUT FakeR ' 1 30000)
    Invoke-Admin 'fake_clear' | Out-Null
    Quit-Client $r.players[0]
    Quit-Client $h
    $lines = Get-Records $h
    $attempts = @($lines | ForEach-Object { $_.f.attempt } | Sort-Object -Unique)
    Check "records: host with one connected and one failed pair has 2 attempts and 3 lines ($($attempts.Count), $($lines.Count))" ($attempts.Count -eq 2 -and $lines.Count -eq 3)
    Check 'records: every host line has the 9 fields' (@($lines | Where-Object { -not (Test-RecordLine $_) }).Count -eq 0)
    $fake = @($lines | Where-Object { $_.f.peer -eq "$($fr.peer_id)" })
    Check 'records: the fake pair has one establish line HOLE_PUNCH_TIMEOUT' ($fake.Count -eq 1 -and $fake[0].f.kind -eq 'establish' -and $fake[0].f.result -eq 'HOLE_PUNCH_TIMEOUT')
    $real = @($lines | Where-Object { $_.f.peer -eq "$($r.players[0].id)" })
    Check 'records: the real pair has OK then CLOSED' ($real.Count -eq 2 -and $real[0].f.result -eq 'OK' -and $real[1].f.result -eq 'CLOSED')

    # (2) 수립 중에 leave 하면 수립 줄 하나가 ABORTED 다. 펀치 지연과 마감 안에 친다.
    $fh = Invoke-Admin 'fake_host name=FakeL mode=silent'
    $p = Start-Client 'rc-leaver' @('--name', 'rcL')
    $em = Get-Mark $p.err
    Join-Room $p $fh.room
    [void](Wait-Count $p.err $em 'session\.state peer_id=\d+ from=IDLE to=PUNCHING' 1 15000)
    Send-Line $p 'leave'
    [void](Wait-Count $p.err $em 'attempt\.end reason=leave' 1 5000)
    Invoke-Admin 'fake_clear' | Out-Null
    Quit-Client $p
    $lines = Get-Records $p
    Check 'records: leave while establishing writes one ABORTED line' ($lines.Count -eq 1 -and $lines[0].f.result -eq 'ABORTED' -and $lines[0].f.peer -eq "$($fh.peer_id)")

    # (3) 방을 만들고 아무도 오지 않은 호스트는 줄이 없다.
    $h = Start-Client 'rc-alone' @('--name', 'rcA')
    Start-Room $h | Out-Null
    Quit-Client $h
    Check 'records: a room nobody joined writes no line' ((Get-Records $h).Count -eq 0)

    # (4) 제어 평면이 없으면 CONTROL_PLANE_EXCHANGE_FAILED 한 줄이고 peer 는 '-' 다.
    $closed = Get-FreeTcpPort
    $c = Start-Client 'rc-nocp' @('--server', "127.0.0.1:$closed")
    $om = Get-Mark $c.out
    Send-Line $c 'join ABCDEF'
    Check 'records: no control plane gives FAIL CONTROL_PLANE_EXCHANGE_FAILED -' (Wait-Count $c.out $om '^FAIL CONTROL_PLANE_EXCHANGE_FAILED - ' 1 25000)
    Quit-Client $c
    $lines = Get-Records $c
    Check 'records: and one establish line with that code and peer=-' ($lines.Count -eq 1 -and $lines[0].f.result -eq 'CONTROL_PLANE_EXCHANGE_FAILED' -and $lines[0].f.peer -eq '-')
}

# ---------------------------------------------------------------- 실행

try {
    $script:testExe = $null
    if (-not $SkipTestBuild) {
        $script:testExe = Build-TestClient $Config $BuildDir $TestBuildDir
    }
    if (Start-Harness $Ddb) {
        $plan = [ordered]@{
            'connect' = { Scenario-Connect }; 'data' = { Scenario-Data }; 'sizes' = { Scenario-Sizes }
            'host-stays' = { Scenario-HostStays }
            'room-ended' = { Scenario-RoomEnded }; 'departed-now' = { Scenario-DepartedNow }
            'departed-retry' = { Scenario-DepartedRetry }; 'punch-fail' = { Scenario-PunchFail }
            'handshake-fail' = { Scenario-HandshakeFail }; 'peer-close' = { Scenario-PeerClose }; 'records' = { Scenario-Records }
        }
        foreach ($name in $plan.Keys) {
            if ($Only.Count -and $Only -notcontains $name) { continue }
            Write-Host "--- $name"
            Reset-Harness
            try {
                & $plan[$name]
            } catch {
                Write-Host ("FAIL  {0}: scenario aborted: {1}" -f $name, ($_.Exception.Message -replace '[^\x20-\x7E]', '?'))
                $failed++
            }
            foreach ($c in $script:clients) { Stop-Client $c }
            try { Invoke-Admin 'fake_clear' | Out-Null } catch { }
        }
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
