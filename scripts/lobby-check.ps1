# Phase 3 의 로비와 클라이언트 쪽 계약을 끝에서 끝까지 본다 (roadmap.md Phase 3 검증의 "로비" 와
# "클라이언트 쪽 계약"). Phase 2 의 STUN 확인 셋도 여기 있다. 통합 뒤 STUN 은 기동 시가 아니라 시도마다
# create_room 이나 join_room 뒤에 돌기 때문이다 (control_plane.md 8.4).
# 이 스크립트가 출력하는 문자열은 ASCII 로만 적는다. 이유는 build.ps1 머리에 있다.
#
# 진짜 제어 서버를 띄운다. control-server/tests/harness/cp_harness.py 가 controlplane 의 Server, Service,
# Store 를 DynamoDB local 의 새 테이블 위에서 127.0.0.1 에만 묶어 돌리고, 따로 연 관리 포트로 주입을 받는다.
# 주입의 목록과 자리는 그 파일 머리에 있다. STUN 응답기 둘도 그 프로세스가 연다. 공개 STUN 서버를 부르지
# 않는다.
#
# **요청 수는 하네스의 로그로 센다.** 서버의 http.request 줄과 하네스의 HARNESS 줄이다. 클라이언트가 낸
# 줄로 세지 않는다. 클라이언트가 보내지 않은 요청을 보냈다고 적는 결함을 그것으로는 못 잡는다.
# 판정은 표준 출력의 ROOM, FAIL 줄과 표준 오류의 로그로 한다. 종료 코드로 하지 않는다 (architecture.md 3.5).
#
# DynamoDB local 이 떠 있어야 한다 (control-server/README.md "DynamoDB local"). 없으면 건너뛰지 않고 실패한다.
# probe200 확인은 시험 빌드가 필요하다 (CMakeLists.txt 의 SANGTACHI_TEST_BUILD). 이 스크립트가
# <BuildDir>-testbuild 에 클라이언트 대상만 시험 빌드로 만든다. -SkipTestBuild 면 그 확인을 NOTRUN 으로 낸다.
#
#   pwsh -NoProfile -File scripts/lobby-check.ps1 [-BuildDir <dir>] [-Only <이름>,...] [-Sabotage <이름>]
#
# -Sabotage 는 이 스크립트 자체의 변이 시험이다. 이름이 가리키는 주입 하나를 걸지 않는다. 그 주입에 기대는
# 확인이 FAIL 해야 한다. 통과하면 그 확인은 아무것도 지키지 않는 것이다.

[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')]
    [string]$Config = 'Debug',
    [string]$BuildDir = '',
    [string]$TestBuildDir = '',
    [switch]$SkipTestBuild,
    [string[]]$Only = @(),
    [ValidateSet('none', 'no-delay', 'no-after-commit', 'no-rate-freeze', 'no-error', 'no-clock', 'no-hold')]
    [string]$Sabotage = 'none',
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

$scenarioNames = @('no-role', 'bad-room-join', 'rejoin', 'in-room', 'leave-pending', 'host-report-stop',
    'host-report-rate', 'retry-definite', 'retry-transient', 'retry-after-commit', 'loop-alive', 'stun')
# pwsh -File 은 쉼표 목록을 문자열 하나로 넘긴다. 여기서 가른다.
$Only = @($Only | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
foreach ($n in $Only) {
    if ($scenarioNames -notcontains $n) { throw "unknown scenario '$n'. Known: $($scenarioNames -join ', ')" }
}

$work = Join-Path ([System.IO.Path]::GetTempPath()) ("sangtachi_lobby_" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $work | Out-Null

$failed = 0
$notrun = 0
$clients = @()
$harness = $null

# ---------------------------------------------------------------- 공통

function Check([string]$what, [bool]$ok) {
    if ($ok) {
        Write-Host ("ok    {0}" -f $what)
    } else {
        Write-Host ("FAIL  {0}" -f $what)
        $script:failed++
    }
}

function NotRun([string]$what, [string]$why) {
    Write-Host ("NOTRUN {0}: {1}" -f $what, $why)
    $script:notrun++
}

function Read-Log([string]$path) {
    # 자식이 쓰고 있는 파일이다. 공유 모드로 열어야 읽힌다.
    if (-not (Test-Path -LiteralPath $path)) { return '' }
    $stream = [System.IO.File]::Open($path, 'Open', 'Read', 'ReadWrite')
    try {
        $reader = New-Object System.IO.StreamReader($stream, [System.Text.Encoding]::UTF8)
        return $reader.ReadToEnd()
    } finally {
        $stream.Dispose()
    }
}

# 표시. 그 뒤에 쓰인 것만 보려고 지금 길이를 적어 둔다.
function Get-Mark([string]$path) { return (Read-Log $path).Length }

function Get-Since([string]$path, [int]$mark) {
    $text = Read-Log $path
    if ($mark -ge $text.Length) { return '' }
    return $text.Substring($mark)
}

function Get-Count([string]$path, [int]$mark, [string]$pattern) {
    return [regex]::Matches((Get-Since $path $mark), $pattern, 'Multiline').Count
}

# 표시 뒤에 그 줄이 n 번 나올 때까지 기다린다. 나오면 참.
function Wait-Count([string]$path, [int]$mark, [string]$pattern, [int]$n = 1, [int]$timeoutMs = 5000) {
    $deadline = [Environment]::TickCount64 + $timeoutMs
    while ($true) {
        if ((Get-Count $path $mark $pattern) -ge $n) { return $true }
        if ([Environment]::TickCount64 -ge $deadline) { return $false }
        Start-Sleep -Milliseconds 50
    }
}

function Wait-Match([string]$path, [int]$mark, [string]$pattern, [int]$timeoutMs = 5000) {
    $deadline = [Environment]::TickCount64 + $timeoutMs
    while ($true) {
        $m = [regex]::Match((Get-Since $path $mark), $pattern, 'Multiline')
        if ($m.Success) { return $m }
        if ([Environment]::TickCount64 -ge $deadline) { return $null }
        Start-Sleep -Milliseconds 50
    }
}

# 프로세스 하나를 띄우고 표준 출력과 표준 오류를 바이트 그대로 파일로 옮긴다. 표준 출력은 UTF-8 이다
# (architecture.md 3.5). 옮기는 것은 .NET 의 스트림 복사 작업이다. PowerShell 이벤트(Register-ObjectEvent)로
# 옮기면 줄 순서가 바뀌는 것을 이 스크립트의 첫 실행에서 봤다(하네스 로그에서 응답 줄이 그 원인인 주입 줄보다
# 먼저 나왔다). 표시 뒤의 줄을 세는 판정이 그 순서에 기댄다. 파일은 버퍼 1 바이트로 열어 쓰는 즉시 읽히게 한다.
function Start-Logged([string]$name, [string]$file, [string]$arguments, [string]$cwd) {
    $info = New-Object System.Diagnostics.ProcessStartInfo
    $info.FileName = $file
    $info.Arguments = $arguments
    $info.WorkingDirectory = $cwd
    $info.UseShellExecute = $false
    $info.RedirectStandardInput = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.CreateNoWindow = $true
    $outPath = Join-Path $work "$name.out"
    $errPath = Join-Path $work "$name.err"
    # 파일 열기부터 복사 시작까지가 한 가드다. 중간에 실패하면 연 파일을 닫고 띄운 자식을 죽인 뒤 다시 던진다.
    # 돌려준 뒤에는 부르는 쪽이 정리 목록에 넣는다(그 사이에 실패할 수 있는 일이 없다).
    $outFile = $null
    $errFile = $null
    $p = $null
    try {
        $outFile = [System.IO.FileStream]::new($outPath, 'Create', 'Write', 'ReadWrite', 1)
        $errFile = [System.IO.FileStream]::new($errPath, 'Create', 'Write', 'ReadWrite', 1)
        $p = [System.Diagnostics.Process]::Start($info)
        if ($null -eq $p) { throw "${name}: process did not start" }
        $copies = @($p.StandardOutput.BaseStream.CopyToAsync($outFile), $p.StandardError.BaseStream.CopyToAsync($errFile))
        return @{ proc = $p; out = $outPath; err = $errPath; files = @($outFile, $errFile); copies = $copies; name = $name }
    } catch {
        if ($null -ne $p) {
            try { if (-not $p.HasExited) { $p.Kill(); [void]$p.WaitForExit(5000) } } catch { }
            $p.Dispose()
        }
        foreach ($f in @($outFile, $errFile)) { if ($null -ne $f) { $f.Dispose() } }
        throw
    }
}

# 프로세스가 끝난 뒤 복사를 마저 끝내고 파일을 닫는다.
function Close-Logged($x) {
    if ($null -eq $x) { return }
    foreach ($t in $x.copies) { try { [void]$t.Wait(3000) } catch { } }
    foreach ($f in $x.files) { $f.Dispose() }
}

function Quote-Arg([string]$a) {
    if ($a -match '[\s"]') { return '"' + ($a -replace '"', '\"') + '"' }
    return $a
}

function Send-Line($client, [string]$line) {
    $client.proc.StandardInput.WriteLine($line)
    $client.proc.StandardInput.Flush()
}

# ---------------------------------------------------------------- 하네스

function Invoke-Admin([string]$line) {
    $tcp = New-Object System.Net.Sockets.TcpClient
    try {
        $tcp.Connect('127.0.0.1', $script:adminPort)
        $tcp.ReceiveTimeout = 30000
        $stream = $tcp.GetStream()
        $bytes = [System.Text.Encoding]::ASCII.GetBytes($line + "`n")
        $stream.Write($bytes, 0, $bytes.Length)
        $reader = New-Object System.IO.StreamReader($stream, [System.Text.Encoding]::ASCII)
        $reply = $reader.ReadLine() | ConvertFrom-Json
    } finally {
        $tcp.Dispose()
    }
    if (-not $reply.ok) { throw "harness admin '$line' failed: $($reply.error)" }
    return $reply
}

# 주입 하나. -Sabotage 가 그 종류를 가리키면 걸지 않는다 (이 스크립트의 변이 시험).
function Invoke-Inject([string]$kind, [string]$line) {
    if ($Sabotage -eq "no-$kind") {
        Write-Host ("SABOTAGE skipped: {0}" -f $line)
        return
    }
    Invoke-Admin $line | Out-Null
}

# 장면마다 처음에 주입, 시계, 속도 제한 표를 비운다.
function Reset-Harness { Invoke-Admin 'clear' | Out-Null }

function H-Mark { return Get-Mark $script:harness.err }
function H-Count([int]$mark, [string]$pattern) { return Get-Count $script:harness.err $mark $pattern }

# 하네스 로그의 요청 한 줄 (control_plane.md 7.5 http.request).
function Req([string]$op, [string]$code = '\S+') { return "^INFO http\.request op=$op status=\d+ error=$code " }

# ---------------------------------------------------------------- 클라이언트

$alphabet = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789'

# 없는 방 코드. 형식은 맞고(control_plane.md 2.1) 하네스의 테이블에 없다.
function New-MissingRoom {
    while ($true) {
        $code = -join (1..6 | ForEach-Object { $alphabet[(Get-Random -Maximum $alphabet.Length)] })
        if ($null -eq (Invoke-Admin "room id=$code").room) { return $code }
    }
}

function Start-Client([string]$name, [string[]]$clientArgs = @(), [string]$exePath = $script:exe) {
    if ($clientArgs -notcontains '--server') { $clientArgs = @('--server', "127.0.0.1:$script:cpPort") + $clientArgs }
    if ($clientArgs -notcontains '--stun') {
        $clientArgs = $clientArgs + @('--stun', "127.0.0.1:$($script:stunPorts[0])", '--stun', "127.0.0.1:$($script:stunPorts[1])")
    }
    $c = Start-Logged $name $exePath (($clientArgs | ForEach-Object { Quote-Arg $_ }) -join ' ') $script:repoRoot
    $script:clients += $c
    $m = Wait-Match $c.err 0 'socket\.bind local=0\.0\.0\.0:(\d+)' 10000
    if ($null -eq $m) { throw "$($name): no socket.bind line. stderr: $(Read-Log $c.err)" }
    $c.port = [int]$m.Groups[1].Value
    return $c
}

function Stop-Client($c) {
    if ($c.proc.HasExited) { return }
    try { Send-Line $c 'quit' } catch { }
    if (-not $c.proc.WaitForExit(5000)) { $c.proc.Kill() }
}

# host 를 치고 방을 세울 때까지 기다린다. ROOM 줄과 register_candidate 성공까지다 (control_plane.md 8.4).
function Start-Room($c) {
    $outMark = Get-Mark $c.out
    $errMark = Get-Mark $c.err
    Send-Line $c 'host'
    $m = Wait-Match $c.out $outMark '^ROOM ([A-Z0-9]{6})\r?$' 10000
    if ($null -eq $m) { throw "$($c.name): no ROOM line after host. stdout: $(Read-Log $c.out)" }
    if (-not (Wait-Count $c.err $errMark 'control\.result op=register_candidate ok=true' 1 15000)) {
        throw "$($c.name): register_candidate did not succeed. stderr tail: $((Get-Since $c.err $errMark))"
    }
    return $m.Groups[1].Value
}

$failCpef = '^FAIL CONTROL_PLANE_EXCHANGE_FAILED '
$failStun = '^FAIL STUN_DISCOVERY_FAILED '

# ---------------------------------------------------------------- 장면
#
# 장면 하나가 로드맵 항목 하나 또는 몇 개를 본다. 장면마다 새 클라이언트를 띄우고 끝에 끈다.

function Scenario-NoRole {
    # 역할 없이 기동하면 제어 요청이 하나도 나가지 않고 로비에서 기다린다. 로비에서 친 leave 는 WARN 한 줄이다.
    $hm = H-Mark
    $c = Start-Client 'norole'
    Start-Sleep -Milliseconds 1500
    Check 'no role: zero control requests reach the server' ((H-Count $hm '^INFO http\.request ') -eq 0)
    Check 'no role: zero connections held or dropped' ((H-Count $hm '^HARNESS (held|dropped) ') -eq 0)
    Check 'no role: nothing on stdout' ((Read-Log $c.out).Trim() -eq '')
    $em = Get-Mark $c.err
    Send-Line $c 'leave'
    Check 'leave in the lobby is one WARN line' (Wait-Count $c.err $em '^WARN console\.rejected reason=in_lobby command=leave')
    Start-Sleep -Milliseconds 300
    Check 'leave in the lobby: exactly one WARN' ((Get-Count $c.err $em '^WARN ') -eq 1)
    Check 'no role: still zero requests after leave' ((H-Count $hm '^INFO http\.request ') -eq 0)
    Check 'no role: the process waits in the lobby' (-not $c.proc.HasExited)
    Stop-Client $c

    # --room 만 주면 종료 코드 2 로 기동 실패다. 다른 클라이언트와 같이 Start-Logged 로 띄워 출력을 비동기로
    # 옮기고 정리 목록에 넣는다. 기다림에 상한을 둔다. 받아들이는 결함이면 프로세스가 끝나지 않으므로 상한에서
    # 끊고 FAIL 이다(끝의 정리가 죽인다).
    $hm = H-Mark
    $x = Start-Logged 'room-only' $script:exe "--server 127.0.0.1:$script:cpPort --room ABCDEF" $script:repoRoot
    $script:clients += $x
    $exited = $x.proc.WaitForExit(10000)
    if (-not $exited) { $x.proc.Kill(); [void]$x.proc.WaitForExit(5000) }
    Check '--room without a role exits within 10s' $exited
    Check '--room without a role exits with code 2' ($exited -and $x.proc.ExitCode -eq 2)
    Check '--room without a role names the reason' (Wait-Count $x.err 0 'reason=room_without_role' 1 2000)
    Check '--room without a role sends nothing' ((H-Count $hm '^INFO http\.request ') -eq 0)
}

function Scenario-BadRoomJoin {
    # 없는 방 코드로 join 하면 FAIL CONTROL_PLANE_EXCHANGE_FAILED 줄이 한 줄이고 프로세스가 남는다. 이어서 맞는
    # 방 코드로 join 하면 참가된다.
    $hostC = Start-Client 'brj-host'
    $room = Start-Room $hostC
    $p = Start-Client 'brj-player'
    $missing = New-MissingRoom
    $hm = H-Mark
    $om = Get-Mark $p.out
    Send-Line $p "join $missing"
    Check 'missing room: the server answered room_not_found' ((Wait-Count $script:harness.err $hm (Req 'join_room' 'room_not_found'))) 
    Check 'missing room: one FAIL CONTROL_PLANE_EXCHANGE_FAILED line' (Wait-Count $p.out $om $failCpef)
    Start-Sleep -Milliseconds 1500
    Check 'missing room: exactly one stdout line' (((Get-Since $p.out $om).Trim() -split "`n").Count -eq 1)
    Check 'missing room: exactly one join_room request (no retry)' ((H-Count $hm (Req 'join_room')) -eq 1)
    Check 'missing room: the process stays' (-not $p.proc.HasExited)
    $hm = H-Mark
    $om = Get-Mark $p.out
    Send-Line $p "join $($room.ToLower())"
    Check 'then a valid join is answered 200' (Wait-Count $script:harness.err $hm (Req 'join_room' '-'))
    Check 'then a valid join prints the ROOM line' (Wait-Count $p.out $om "^ROOM $room\r?$")
    Stop-Client $p
    Stop-Client $hostC
}

function Scenario-Rejoin {
    # leave 뒤 같은 방에 다시 join 하면 새 peer_id 를 받고 STUN 요청이 새로 나간다 (control_plane.md 2.4, 8.4).
    $hostC = Start-Client 'rj-host'
    $room = Start-Room $hostC
    $p = Start-Client 'rj-player'
    $hm = H-Mark
    $em = Get-Mark $p.err
    Send-Line $p "join $room"
    $first = Wait-Match $script:harness.err $hm '^HARNESS issued op=join_room peer_id=(\d+) ' 10000
    Check 'first join: issued' ($null -ne $first)
    Check 'first join: STUN reached both responders' (Wait-Count $script:harness.err $hm "^HARNESS stun\.request port=\d+ src=127\.0\.0\.1:$($p.port)\r?$" 2 10000)
    Check 'first join: register_candidate succeeded' (Wait-Count $p.err $em 'control\.result op=register_candidate ok=true' 1 15000)
    Send-Line $p 'leave'
    Check 'leave ends the attempt' (Wait-Count $p.err $em 'attempt\.end reason=leave')
    $hm = H-Mark
    Send-Line $p "join $room"
    $second = Wait-Match $script:harness.err $hm '^HARNESS issued op=join_room peer_id=(\d+) ' 10000
    Check 'rejoin: issued' ($null -ne $second)
    if ($null -ne $first -and $null -ne $second) {
        Check "rejoin gets a new peer_id ($($first.Groups[1].Value) -> $($second.Groups[1].Value))" ($first.Groups[1].Value -ne $second.Groups[1].Value)
    }
    Check 'rejoin: STUN requests go out again' (Wait-Count $script:harness.err $hm "^HARNESS stun\.request port=\d+ src=127\.0\.0\.1:$($p.port)\r?$" 2 10000)
    $roomItems = Invoke-Admin "room id=$room"
    Check 'rejoin: the server holds two player peers (old seat not yet reclaimed)' (@($roomItems.peers | Where-Object { $_.virtual_ip -ne '10.100.0.1' }).Count -eq 2)
    Stop-Client $p
    Stop-Client $hostC
}

function Scenario-InRoom {
    # 방에 있는 동안 친 host 와 join 은 WARN 한 줄이고 요청이 나가지 않는다.
    $c = Start-Client 'inroom'
    $room = Start-Room $c
    $hm = H-Mark
    $em = Get-Mark $c.err
    Send-Line $c 'host'
    Check 'host in a room is one WARN' (Wait-Count $c.err $em '^WARN console\.rejected reason=not_in_lobby command=host')
    $em2 = Get-Mark $c.err
    Send-Line $c "join $room"
    Check 'join in a room is one WARN' (Wait-Count $c.err $em2 '^WARN console\.rejected reason=not_in_lobby command=join')
    Start-Sleep -Milliseconds 1000
    Check 'host and join in a room send no create_room or join_room' ((H-Count $hm (Req '(create_room|join_room)')) -eq 0)
    Check 'host and join in a room: exactly two WARN lines' ((Get-Count $c.err $em '^WARN console\.rejected ') -eq 2)
    Stop-Client $c
}

function Scenario-LeavePending {
    # 응답이 미결인 동안의 leave (concurrency.md 8장). 응답을 늦추는 주입점으로 host 를 미결로 둔다.
    $c = Start-Client 'pend'
    $hm = H-Mark
    $om = Get-Mark $c.out
    $em = Get-Mark $c.err
    # 클라이언트의 수신 상한 3초(control_plane.md 8.2) 안이어야 응답이 늦게라도 도착한다.
    Invoke-Inject 'delay' 'delay op=create_room count=1 ms=2000 when=after'
    Send-Line $c 'host'
    $issued = Wait-Count $script:harness.err $hm '^HARNESS issued op=create_room ' 1 5000
    Check 'pending: the server processed create_room' $issued
    Send-Line $c 'leave'
    Check 'pending: leave ends the attempt' (Wait-Count $c.err $em 'attempt\.end reason=leave')
    Send-Line $c 'host'
    $warned = Wait-Count $c.err $em '^WARN console\.rejected reason=request_pending command=host' 1 1500
    $answered = Wait-Count $script:harness.err $hm (Req 'create_room' '-') 1 5000
    Check 'pending: the delayed response was sent after leave' $answered
    Check 'pending: host before the response is a WARN' $warned
    # 늦은 응답을 받은 뒤 반영이 있다면 나올 시간을 준다. STUN 은 바로 나가고 register 는 STUN 뒤다.
    Start-Sleep -Milliseconds 2500
    Check 'pending: no ROOM line after the late response' ((Get-Count $c.out $om '^ROOM ') -eq 0)
    Check 'pending: no STUN request after the late response' ((H-Count $hm "^HARNESS stun\.request port=\d+ src=127\.0\.0\.1:$($c.port)\r?$") -eq 0)
    Check 'pending: no register_candidate after the late response' ((H-Count $hm (Req 'register_candidate')) -eq 0)
    Check 'pending: exactly one create_room so far' ((H-Count $hm (Req 'create_room')) -eq 1)
    $em = Get-Mark $c.err
    Send-Line $c 'host'
    Check 'pending: host after the response is accepted' (Wait-Count $script:harness.err $hm (Req 'create_room' '-') 2 5000)
    Check 'pending: the accepted host prints ROOM' (Wait-Count $c.out $om '^ROOM ' 1 5000)
    Check 'pending: no WARN for the accepted host' ((Get-Count $c.err $em '^WARN console\.rejected ') -eq 0)
    Stop-Client $c
}

function Scenario-HostReportStop {
    # host_report 첫 행 오류 넷 (control_plane.md 4.6 오류 표). 각각 FAIL 한 번, 그 뒤 host_report 없음, 로비.
    foreach ($code in @('room_expired', 'room_not_found', 'unauthorized', 'bad_request')) {
        Reset-Harness
        $c = Start-Client "hrs-$code"
        Start-Room $c | Out-Null
        $hm = H-Mark
        $om = Get-Mark $c.out
        $em = Get-Mark $c.err
        if ($code -eq 'room_expired') {
            # 임대는 마지막 갱신에서 ROOM_LEASE_S(120초)다. 서버 시계를 그보다 멀리 민다.
            Invoke-Inject 'clock' 'clock offset_ms=200000'
        } else {
            Invoke-Inject 'error' "error op=host_report count=1 code=$code when=before"
        }
        $got = Wait-Count $script:harness.err $hm (Req 'host_report' $code) 1 8000
        Check "${code}: host_report got $code from the server" $got
        Check "${code}: one FAIL CONTROL_PLANE_EXCHANGE_FAILED line" (Wait-Count $c.out $om $failCpef 1 3000)
        $after = H-Mark
        # 열린 방의 주기는 5초다 (HOST_REPORT_OPEN_S). 한 주기를 넘게 본다.
        Start-Sleep -Milliseconds 7000
        Check "${code}: no host_report after the FAIL" ((H-Count $after (Req 'host_report')) -eq 0)
        Check "${code}: exactly one FAIL line" ((Get-Count $c.out $om '^FAIL ') -eq 1)
        Check "${code}: back in the lobby" ((Get-Count $c.err $em 'attempt\.end reason=failed') -eq 1)
        $em = Get-Mark $c.err
        Send-Line $c 'leave'
        Check "${code}: leave now warns in_lobby" (Wait-Count $c.err $em '^WARN console\.rejected reason=in_lobby command=leave')
        Stop-Client $c
    }
}

function Scenario-HostReportRate {
    # rate_limited 는 FAIL 이 없고 다음 주기에 다시 부른다. 보충 시계를 풀면 성공한다 (control_plane.md 4.6, 6.4).
    $c = Start-Client 'hrr'
    Start-Room $c | Out-Null
    $om = Get-Mark $c.out
    $em = Get-Mark $c.err
    Invoke-Inject 'rate' 'rate_freeze'
    if ($Sabotage -ne 'no-rate-freeze') {
        $burn = Invoke-Admin 'burn n=10'
        Check 'rate: ten missing-room join_room spent the budget' ((@($burn.statuses) | Where-Object { $_ -eq 404 }).Count -eq 10)
    } else {
        Write-Host 'SABOTAGE skipped: burn n=10'
    }
    $hm = H-Mark
    Check 'rate: the next host_report is rate_limited' (Wait-Count $script:harness.err $hm (Req 'host_report' 'rate_limited') 1 8000)
    Check 'rate: the cycle goes on (a second rate_limited host_report)' (Wait-Count $script:harness.err $hm (Req 'host_report' 'rate_limited') 2 8000)
    Check 'rate: no FAIL line while rate_limited' ((Get-Count $c.out $om '^FAIL ') -eq 0)
    Invoke-Admin 'rate_thaw' | Out-Null
    Invoke-Admin 'rate_advance s=60' | Out-Null
    $hm = H-Mark
    Check 'rate: after the budget refills the next host_report succeeds' (Wait-Count $script:harness.err $hm (Req 'host_report' '-') 1 8000)
    Check 'rate: still no FAIL line' ((Get-Count $c.out $om '^FAIL ') -eq 0)
    Check 'rate: the attempt did not end' ((Get-Count $c.err $em 'attempt\.end ') -eq 0)
    Stop-Client $c
}

function Scenario-RetryDefinite {
    # rate_limited, room_full, unauthorized 는 재시도 없이 즉시 FAIL (control_plane.md 8.3).
    $hostC = Start-Client 'rd-host'
    $room = Start-Room $hostC
    $cases = @(
        @{ op = 'join_room'; code = 'rate_limited' }, @{ op = 'join_room'; code = 'room_full' },
        @{ op = 'join_room'; code = 'unauthorized' }, @{ op = 'create_room'; code = 'rate_limited' },
        @{ op = 'create_room'; code = 'unauthorized' }
    )
    foreach ($k in $cases) {
        $c = Start-Client "rd-$($k.op)-$($k.code)"
        $hm = H-Mark
        $om = Get-Mark $c.out
        Invoke-Inject 'error' "error op=$($k.op) count=1 code=$($k.code) when=before"
        if ($k.op -eq 'join_room') { Send-Line $c "join $room" } else { Send-Line $c 'host' }
        $what = "$($k.op) $($k.code)"
        Check "${what}: the server answered $($k.code)" (Wait-Count $script:harness.err $hm (Req $k.op $k.code) 1 5000)
        Check "${what}: one FAIL CONTROL_PLANE_EXCHANGE_FAILED line" (Wait-Count $c.out $om $failCpef 1 3000)
        # 재시도 간격은 1초다. 그보다 넉넉히 본다.
        Start-Sleep -Milliseconds 2500
        Check "${what}: exactly one request (no retry)" ((H-Count $hm (Req $k.op)) -eq 1)
        Check "${what}: no ROOM line" ((Get-Count $c.out $om '^ROOM ') -eq 0)
        Stop-Client $c
        Reset-Harness
    }
    Stop-Client $hostC
}

function Scenario-RetryTransient {
    # internal, unavailable 은 같은 본문으로 첫 시도를 포함해 총 3회 (control_plane.md 8.3). 저장소 앞에서 내는
    # 오류라 nonce 의 동일성은 저장소에 남지 않는다. 그것은 retry-after-commit 이 본다.
    $hostC = Start-Client 'rt-host'
    $room = Start-Room $hostC
    foreach ($code in @('internal', 'unavailable')) {
        foreach ($op in @('create_room', 'join_room')) {
            foreach ($n in @(2, 3)) {
                Reset-Harness
                $c = Start-Client "rt-$op-$code-$n"
                $hm = H-Mark
                $om = Get-Mark $c.out
                Invoke-Inject 'error' "error op=$op count=$n code=$code when=before"
                if ($op -eq 'join_room') { Send-Line $c "join $room" } else { Send-Line $c 'host' }
                $what = "$op $code x$n"
                if ($n -eq 2) {
                    Check "${what}: third attempt succeeds" (Wait-Count $script:harness.err $hm (Req $op '-') 1 8000)
                    Check "${what}: one ROOM line" (Wait-Count $c.out $om '^ROOM ' 1 3000)
                    Check "${what}: no FAIL line" ((Get-Count $c.out $om '^FAIL ') -eq 0)
                } else {
                    Check "${what}: FAIL after three attempts" (Wait-Count $c.out $om $failCpef 1 8000)
                }
                Start-Sleep -Milliseconds 2000
                Check "${what}: exactly three requests" ((H-Count $hm (Req $op)) -eq 3)
                Check "${what}: $n of them were $code" ((H-Count $hm (Req $op $code)) -eq $n)
                Stop-Client $c
            }
        }
    }
    Stop-Client $hostC
}

function Scenario-RetryAfterCommit {
    # 커밋 뒤 응답 직전의 internal 로 재시도를 돌린다 (roadmap Phase 3 클라이언트 쪽 계약). create_room 은 ROOM 항목
    # 수 1, join_room 은 같은 peer_id 와 가상 IP 이고 VIP# 항목 수 1 이다.
    $c = Start-Client 'rac-host'
    $before = (Invoke-Admin 'count').items.ROOM
    $hm = H-Mark
    $om = Get-Mark $c.out
    # 첫 시도는 store 의 커밋 뒤에, 둘째 시도는 같은 nonce 의 재생 뒤에 internal 이다. store 주입점은 쓰기가 있어야
    # 걸리므로 재생에는 걸리지 않는다. 그래서 둘째는 하네스의 dispatch 뒤 오류로 만든다 (cp_harness.py 의 dispatch).
    Invoke-Inject 'after-commit' 'after_commit label=create_room count=1'
    Invoke-Inject 'after-commit' 'error op=create_room count=1 code=internal when=after'
    Send-Line $c 'host'
    $m = Wait-Match $c.out $om '^ROOM ([A-Z0-9]{6})\r?$' 10000
    Check 'create_room after-commit: ROOM line' ($null -ne $m)
    Check 'create_room after-commit: two internal then one success' (((H-Count $hm (Req 'create_room' 'internal')) -eq 2) -and ((H-Count $hm (Req 'create_room' '-')) -eq 1))
    Check 'create_room after-commit: the store-level injection fired' ((H-Count $hm '^HARNESS inject kind=after_commit label=create_room ') -eq 1)
    $after = (Invoke-Admin 'count').items.ROOM
    Check "create_room after-commit: exactly one ROOM item created ($before -> $after)" (($after - $before) -eq 1)
    Check 'create_room after-commit: one ROOM line' ((Get-Count $c.out $om '^ROOM ') -eq 1)
    if ($null -eq $m) { Stop-Client $c; return }
    $room = $m.Groups[1].Value
    # 방을 세운 뒤에 참가한다. 호스트의 register_candidate 와 겹치지 않게 한다.
    Wait-Count $c.err 0 'control\.result op=register_candidate ok=true' 1 15000 | Out-Null

    $p = Start-Client 'rac-player'
    $hm = H-Mark
    $om = Get-Mark $p.out
    Invoke-Inject 'after-commit' 'after_commit label=join_room count=1'
    Invoke-Inject 'after-commit' 'error op=join_room count=1 code=internal when=after'
    Send-Line $p "join $room"
    # 둘째 시도도 재생으로 issued 줄을 낸 뒤 실패한다. 셋째(성공)를 기다리고 마지막 issued 줄을 쓴다.
    Check 'join_room after-commit: the third attempt succeeds' (Wait-Count $script:harness.err $hm (Req 'join_room' '-') 1 10000)
    $all = [regex]::Matches((Get-Since $script:harness.err $hm), '^HARNESS issued op=join_room peer_id=(\d+) virtual_ip=(\S+)\r?$', 'Multiline')
    $issued = if ($all.Count) { $all[$all.Count - 1] } else { $null }
    Check 'join_room after-commit: issued after retries' ($null -ne $issued)
    Check 'join_room after-commit: two internal then one success' (((H-Count $hm (Req 'join_room' 'internal')) -eq 2) -and ((H-Count $hm (Req 'join_room' '-')) -eq 1))
    Check 'join_room after-commit: the store-level injection fired' ((H-Count $hm '^HARNESS inject kind=after_commit label=join_room ') -eq 1)
    $items = Invoke-Admin "room id=$room"
    $players = @($items.peers | Where-Object { $_.virtual_ip -ne '10.100.0.1' })
    $vips = @($items.vips | Where-Object { $_.virtual_ip -ne '10.100.0.1' })
    Check "join_room after-commit: exactly one player VIP# item ($($vips.Count))" ($vips.Count -eq 1)
    Check "join_room after-commit: exactly one player PEER# item ($($players.Count))" ($players.Count -eq 1)
    if ($null -ne $issued -and $players.Count -eq 1) {
        Check 'join_room after-commit: the answer has the committed peer_id' ([string]$players[0].peer_id -eq $issued.Groups[1].Value)
        Check 'join_room after-commit: the answer has the committed virtual IP' ($players[0].virtual_ip -eq $issued.Groups[2].Value)
    }
    Check 'join_room after-commit: one ROOM line' (Wait-Count $p.out $om '^ROOM ' 1 3000)
    Stop-Client $p
    Stop-Client $c
}

function Get-FreeTcpPort {
    $l = New-Object System.Net.Sockets.TcpListener([System.Net.IPAddress]::Loopback, 0)
    $l.Start()
    try { return $l.LocalEndpoint.Port } finally { $l.Stop() }
}

# 미결 구간의 probe200 간격 (architecture.md 9장 timer.tick). 판정은 Phase 1 과 같은 400ms 다.
# 창은 host 를 친 때부터 FAIL 줄까지다. FAIL 줄이 없으면 창의 끝을 모르므로 미결이었다고 판정하지 않는다.
function Check-Ticks([string]$label, [string]$window, [long]$windowMs, [bool]$ended) {
    $ticks = [regex]::Matches($window, 'timer\.tick name=probe200 elapsed_ms=(\d+)')
    $values = @($ticks | ForEach-Object { [int]$_.Groups[1].Value })
    $max = if ($values.Count) { ($values | Measure-Object -Maximum).Maximum } else { -1 }
    Check "${label}: the control request stayed pending for at least 5s, ended by FAIL ($windowMs ms)" ($ended -and $windowMs -ge 5000)
    # 200ms 주기면 창 하나에 windowMs/200 번이다. 절반 이상을 요구한다. 루프가 멈추면 줄 수가 준다.
    Check "${label}: probe200 kept ticking ($($values.Count) ticks)" ($values.Count -ge [math]::Floor($windowMs / 400))
    Check "${label}: every probe200 elapsed_ms <= 400 (max $max)" ($values.Count -gt 0 -and $max -le 400)
}

function Scenario-LoopAlive {
    # 제어 서버가 응답하지 않아도 [loop] 가 멈추지 않는다 (concurrency.md 8장). 시험 빌드가 필요하다.
    if ($null -eq $script:testExe) {
        NotRun 'loop-alive (probe200 while a control request hangs)' 'no test build (-SkipTestBuild)'
        NotRun 'transport retries: three attempts on connect failure and on io timeout' 'no test build (-SkipTestBuild)'
        return
    }
    # (1) 무응답. 하네스가 연결을 받고 답하지 않는다. 클라이언트의 수신 상한 3초에 걸린다. 세 번이다.
    $c = Start-Client 'alive-hold' @() $script:testExe
    Check 'test build: the client says so at startup' (Wait-Count $c.err 0 '^WARN build\.test build=test' 1 3000)
    $hm = H-Mark
    $om = Get-Mark $c.out
    $em = Get-Mark $c.err
    Invoke-Inject 'hold' 'hold count=3'
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    Send-Line $c 'host'
    $failedLine = Wait-Count $c.out $om $failCpef 1 25000
    $sw.Stop()
    Check 'held: FAIL CONTROL_PLANE_EXCHANGE_FAILED after the retries' $failedLine
    Check 'held: the server saw exactly three attempts' ((H-Count $hm '^HARNESS held op=create_room') -eq 3)
    Check 'held: no answered create_room' ((H-Count $hm (Req 'create_room')) -eq 0)
    Check-Ticks 'held' (Get-Since $c.err $em) $sw.ElapsedMilliseconds $failedLine
    Stop-Client $c

    # (2) 연결 거부. 닫힌 루프백 포트다. Windows 는 거부를 받은 뒤 SYN 을 다시 보내 약 2초 걸린다.
    #     연결이 서버에 닿지 않으므로 시도 수는 클라이언트의 control.result 로만 볼 수 있다.
    $closed = Get-FreeTcpPort
    $c = Start-Client 'alive-refused' @('--server', "127.0.0.1:$closed") $script:testExe
    $om = Get-Mark $c.out
    $em = Get-Mark $c.err
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    Send-Line $c 'host'
    $failedLine = Wait-Count $c.out $om $failCpef 1 25000
    $sw.Stop()
    Check 'refused: FAIL CONTROL_PLANE_EXCHANGE_FAILED after the retries' $failedLine
    Check 'refused: three transport failures (client log; nothing reaches a server)' ((Get-Count $c.err $em 'control\.result op=create_room ok=false error=transport') -eq 3)
    Check-Ticks 'refused' (Get-Since $c.err $em) $sw.ElapsedMilliseconds $failedLine
    Stop-Client $c
}

function Scenario-Stun {
    # Phase 2 의 STUN 확인 셋. 통합 뒤에는 host 시도가 STUN 을 부른다.
    # (1) 응답 없는 서버 둘. 마감 뒤 실패하고 프로세스는 산다. 상한은 5s x ceil(2 / 2) = 5s (protocol.md 11장).
    $c = Start-Client 'stun-silent' @('--stun', '127.0.0.1:9', '--stun', '127.0.0.1:19')
    $hm = H-Mark
    $om = Get-Mark $c.out
    $em = Get-Mark $c.err
    Send-Line $c 'host'
    Check 'silent STUN list: the room is created first' (Wait-Count $c.out $om '^ROOM ' 1 5000)
    Check 'silent STUN list ends in session.failed STUN_DISCOVERY_FAILED' (Wait-Count $c.err $em 'session\.failed code=STUN_DISCOVERY_FAILED' 1 15000)
    Check 'silent STUN list: one FAIL STUN_DISCOVERY_FAILED line' (Wait-Count $c.out $om $failStun 1 2000)
    Check 'silent STUN list: no register_candidate' ((H-Count $hm (Req 'register_candidate')) -eq 0)
    Check 'the process survives the STUN failure' (-not $c.proc.HasExited)
    Stop-Client $c

    # (2) 목록이 하나면 기동 시 WARN 이고, host 하면 마감을 기다리지 않고 실패한다 (architecture.md 3.5).
    $c = Start-Client 'stun-single' @('--stun', '127.0.0.1:9')
    Check 'a one-entry STUN list warns at startup' (Wait-Count $c.err 0 'WARN stun\.config reason=single_server' 1 3000)
    $om = Get-Mark $c.out
    $em = Get-Mark $c.err
    Send-Line $c 'host'
    Check 'one-entry list: the room is created' (Wait-Count $c.out $om '^ROOM ' 1 5000)
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $ok = Wait-Count $c.out $om $failStun 1 3000
    Check "a one-entry STUN list fails without waiting for the deadline ($($sw.ElapsedMilliseconds) ms after ROOM)" $ok
    Stop-Client $c

    # (3) 응답기 둘. stun.result 두 줄이고 실패가 없다.
    $c = Start-Client 'stun-ok'
    $hm = H-Mark
    $em = Get-Mark $c.err
    Start-Room $c | Out-Null
    # STUN 이 실제로 돌았다는 증거. 응답기 둘이 이 클라이언트의 포트에서 온 요청을 받았다.
    foreach ($port in $script:stunPorts) {
        Check "STUN responder $port received a request from the client" ((H-Count $hm "^HARNESS stun\.request port=$port src=127\.0\.0\.1:$($c.port)\r?$") -ge 1)
    }
    foreach ($port in $script:stunPorts) {
        Check "STUN server 127.0.0.1:$port reports the mapped endpoint" ((Get-Count $c.err $em "stun\.result server=127\.0\.0\.1:$port mapped=$([regex]::Escape($script:mappedIp)):$($c.port)") -eq 1)
    }
    Check 'two answers mean no STUN failure' ((Get-Count $c.err $em 'session\.failed') -eq 0)
    Stop-Client $c
}

# ---------------------------------------------------------------- 실행

try {
    # 시험 빌드 클라이언트. 클라이언트 대상만 짓는다. 결과는 따로 둔 폴더에 남아 다음 실행이 증분으로 짓는다.
    $script:testExe = $null
    if (-not $SkipTestBuild) {
        if (-not $TestBuildDir) { $TestBuildDir = $BuildDir.TrimEnd('\', '/') + '-testbuild' }
        . (Join-Path $PSScriptRoot 'vsdevshell.ps1')
        cmake -S $repoRoot -B $TestBuildDir -G Ninja "-DCMAKE_BUILD_TYPE=$Config" -DSANGTACHI_TEST_BUILD=ON -DSANGTACHI_BUILD_TESTS=OFF | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "test-build configure failed ($LASTEXITCODE)" }
        cmake --build $TestBuildDir --target sangtachi_client | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "test-build build failed ($LASTEXITCODE)" }
        $script:testExe = Join-Path $TestBuildDir 'client\sangtachi_client.exe'
    }

    $script:harness = Start-Logged 'harness' $py ("tests\harness\cp_harness.py --exit-on-stdin-eof --stun 2 --ddb " + $Ddb) (Join-Path $repoRoot 'control-server')
    $ready = Wait-Match $harness.err 0 '^HARNESS ready cp_port=(\d+) admin_port=(\d+) stun_ports=(\d+),(\d+) table=\S+ mapped_ip=(\S+)\r?$' 90000
    if ($null -eq $ready) {
        Write-Host 'FAIL  the control-plane harness did not start. Its output:'
        Write-Host (Read-Log $harness.err)
        Write-Host 'If DynamoDB local is not running, start it with:'
        Write-Host '  docker run -d --rm --name sangtachi-ddb -p 127.0.0.1:8001:8000 amazon/dynamodb-local:3.3.1 -jar DynamoDBLocal.jar -inMemory -sharedDb'
        $failed++
    }
    $started = $null -ne $ready
    if ($started) {
        $script:cpPort = [int]$ready.Groups[1].Value
        $script:adminPort = [int]$ready.Groups[2].Value
        $script:stunPorts = @([int]$ready.Groups[3].Value, [int]$ready.Groups[4].Value)
        # 응답기는 출발지 포트와 이 주소를 매핑으로 적는다. 루프백이면 서버의 후보 위생이 거부한다 (cp_harness.py).
        $script:mappedIp = $ready.Groups[5].Value
        Write-Host ("harness: control plane 127.0.0.1:{0}, STUN 127.0.0.1:{1},{2}" -f $cpPort, $stunPorts[0], $stunPorts[1])

        $plan = [ordered]@{
            'no-role' = { Scenario-NoRole }; 'bad-room-join' = { Scenario-BadRoomJoin }; 'rejoin' = { Scenario-Rejoin }
            'in-room' = { Scenario-InRoom }; 'leave-pending' = { Scenario-LeavePending }
            'host-report-stop' = { Scenario-HostReportStop }; 'host-report-rate' = { Scenario-HostReportRate }
            'retry-definite' = { Scenario-RetryDefinite }; 'retry-transient' = { Scenario-RetryTransient }
            'retry-after-commit' = { Scenario-RetryAfterCommit }; 'loop-alive' = { Scenario-LoopAlive }; 'stun' = { Scenario-Stun }
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
        }

        # 서버 로그에 방 코드와 토큰이 없다 (roadmap Phase 3, control_plane.md 7.5). 이 실행에서 클라이언트가 받은
        # 방 코드 전부로 하네스 로그를 grep 한다. 토큰은 클라이언트가 내지 않으므로 여기서 볼 수 없다.
        $codes = @()
        foreach ($c in $script:clients) {
            $codes += [regex]::Matches((Read-Log $c.out), '^ROOM ([A-Z0-9]{6})', 'Multiline') | ForEach-Object { $_.Groups[1].Value }
        }
        $hlog = Read-Log $harness.err
        $leaks = @($codes | Sort-Object -Unique | Where-Object { $hlog.Contains($_) })
        Check "server log carries none of the $(@($codes | Sort-Object -Unique).Count) room codes seen" ($leaks.Count -eq 0)
    }
} finally {
    foreach ($c in $script:clients) {
        if (-not $c.proc.HasExited) { $c.proc.Kill() }
    }
    if ($null -ne $script:harness) {
        $hp = $script:harness.proc
        $killed = $false
        if (-not $hp.HasExited) {
            try { Invoke-Admin 'quit' | Out-Null } catch { }
            try { $hp.StandardInput.Close() } catch { }
            if (-not $hp.WaitForExit(30000)) { $hp.Kill(); [void]$hp.WaitForExit(5000); $killed = $true }
        }
        # 하네스가 기동했으면 스스로 끝나고, 종료 코드가 0 이고, 테이블을 지웠다고 적어야 한다. 아니면 DynamoDB
        # local 에 테이블이 남았을 수 있다. 실패로 세고 로그를 남긴다. 줄을 읽기 전에 복사가 끝나기를 기다린다.
        foreach ($t in $script:harness.copies) { try { [void]$t.Wait(5000) } catch { } }
        $hl = Read-Log $script:harness.err
        if ($hl -match 'HARNESS ready ') {
            Check 'the harness stopped by itself (not killed)' (-not $killed)
            Check 'the harness exit code is 0' ((-not $killed) -and $hp.ExitCode -eq 0)
            Check 'the harness reported deleting its table' ($hl -match 'HARNESS table\.deleted ')
        }
    }
    foreach ($p in @($script:clients) + @($script:harness)) { Close-Logged $p }
    if ($failed -gt 0) {
        Write-Host "logs kept in $work"
    } else {
        Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Write-Host ("{0} checks failed, {1} not run" -f $failed, $notrun)
if ($failed -gt 0) { exit 1 }
exit 0
