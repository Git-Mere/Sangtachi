# Phase 1 의 끝에서 끝까지 확인 (roadmap.md Phase 1 검증).
# 이 스크립트가 출력하는 문자열은 ASCII 로만 적는다. 이유는 build.ps1 머리에 있다.
#
# 단위 시험이 볼 수 없는 것을 본다. 프로세스 여럿이 실제로 데이터그램을 주고받고,
# 양쪽이 낸 rx.raw 두 줄의 len 과 sha256 이 같은지다. 눈으로 바이트를 비교하지 않는다.
#
# 포트는 OS 가 고른다 (protocol.md 6장). 그래서 상대 주소를 기동 시점에 알 수 없고,
# 먼저 뜬 쪽의 socket.bind 줄에서 읽어 다음 프로세스에 넘긴다. 사슬로 셋을 띄우면
# 가운데 프로세스가 한 번의 실행에서 보내기와 받기를 모두 돈다.
#
#   P1 <-- P2 <-- P3
#   P2 가 P1 에게 보내고, P3 가 P2 에게 보낸다.

[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')]
    [string]$Config = 'Debug',
    [string]$BuildDir = ''
)

$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'buildpath.ps1')

# 기본 위치의 근거는 build.ps1 머리에 있고 판정은 buildpath.ps1 이 한다.
$BuildDir = Resolve-BuildDir -Config $Config -Explicit $BuildDir
$exe = Join-Path $BuildDir 'client\sangtachi_client.exe'
if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
    throw "client executable not found: $exe. Run scripts/build.ps1 first."
}

$work = Join-Path ([System.IO.Path]::GetTempPath()) ("sangtachi_e2e_" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $work | Out-Null

$procs = @{}
$jobs = @()
$failed = 0

function Read-Log([string]$path) {
    # 자식이 쓰고 있는 파일이다. 공유 모드로 열어야 읽힌다.
    if (-not (Test-Path -LiteralPath $path)) { return '' }
    $stream = [System.IO.File]::Open($path, 'Open', 'Read', 'ReadWrite')
    try {
        $reader = New-Object System.IO.StreamReader($stream)
        return $reader.ReadToEnd()
    } finally {
        $stream.Dispose()
    }
}

function Wait-ForLine([string]$path, [string]$pattern, [int]$timeoutMs = 5000) {
    $deadline = [Environment]::TickCount64 + $timeoutMs
    while ([Environment]::TickCount64 -lt $deadline) {
        $text = Read-Log $path
        $match = [regex]::Match($text, $pattern)
        if ($match.Success) { return $match }
        Start-Sleep -Milliseconds 50
    }
    return $null
}

# STUN 질의가 켜진 채로 도는 것이 기본이다 (architecture.md 3.5 기동 입력). 이 스크립트는
# **공개 STUN 서버를 부르지 않는다.** 망이 없는 기계에서 깨지고, 남의 서버에 시험 트래픽을
#보내게 되며, 그 응답이 rx.raw 로 섞여 Phase 1 의 대조를 어지럽힌다. 그래서 응답하지 않는
# 로컬 주소 둘을 기본으로 준다. 직접 --stun 을 주는 호출은 그대로 쓴다.
$deadStun = @('--stun', '127.0.0.1:9', '--stun', '127.0.0.1:19')

function Start-Client([string]$name, [string[]]$clientArgs) {
    if ($clientArgs -notcontains '--stun') { $clientArgs = $deadStun + $clientArgs }
    $errPath = Join-Path $work "$name.err"
    $info = New-Object System.Diagnostics.ProcessStartInfo
    $info.FileName = $exe
    $info.Arguments = ($clientArgs -join ' ')
    $info.UseShellExecute = $false
    $info.RedirectStandardInput = $true
    $info.RedirectStandardError = $false
    $info.CreateNoWindow = $true

    # 표준 오류를 파일로 보낸다. cmd 를 거치면 인용이 한 겹 더 붙으므로 직접 연다.
    $info.RedirectStandardError = $true
    $p = [System.Diagnostics.Process]::Start($info)
    $writer = [System.IO.StreamWriter]::new($errPath, $false)
    $writer.AutoFlush = $true
    # 표준 오류를 비동기로 파일에 옮긴다.
    $handler = {
        if ($null -ne $EventArgs.Data) { $Event.MessageData.WriteLine($EventArgs.Data) }
    }
    Register-ObjectEvent -InputObject $p -EventName ErrorDataReceived -Action $handler `
        -MessageData $writer | Out-Null
    $p.BeginErrorReadLine()

    return @{ proc = $p; err = $errPath; writer = $writer; name = $name }
}

function Get-LocalPort($client) {
    $m = Wait-ForLine $client.err 'socket\.bind local=0\.0\.0\.0:(\d+)'
    if ($null -eq $m) { throw "$($client.name): no socket.bind line" }
    return [int]$m.Groups[1].Value
}

function Send-Command($client, [string]$line) {
    $client.proc.StandardInput.WriteLine($line)
    $client.proc.StandardInput.Flush()
}

function Get-RxRaw($client, [int]$index) {
    # index 번째 rx.raw 줄의 len 과 sha256.
    $deadline = [Environment]::TickCount64 + 5000
    while ([Environment]::TickCount64 -lt $deadline) {
        $text = Read-Log $client.err
        $matches = [regex]::Matches($text, 'rx\.raw from=(\S+) len=(\d+) sha256=(\S+)')
        if ($matches.Count -gt $index) {
            $m = $matches[$index]
            return @{ from = $m.Groups[1].Value; len = [int]$m.Groups[2].Value; sha = $m.Groups[3].Value }
        }
        Start-Sleep -Milliseconds 50
    }
    return $null
}

# 자유 UDP 포트 하나. bind 해 보고 OS 가 고른 번호를 읽는다 (protocol.md 6장과 같은 방법).
# 닫은 뒤 그 번호를 응답기가 다시 bind 하므로 그 사이에 남이 가져갈 수 있다. 시험 기계의
# 이야기라 재시도를 두지 않고, 실패하면 응답기 작업이 죽어 아래 검사가 FAIL 로 드러난다.
function Get-FreeUdpPort {
    $probe = New-Object System.Net.Sockets.UdpClient(0, [System.Net.Sockets.AddressFamily]::InterNetwork)
    try { return $probe.Client.LocalEndPoint.Port } finally { $probe.Close() }
}

# 최소 STUN 응답기. Binding Request 를 받으면 XOR-MAPPED-ADDRESS 하나를 실은 Binding
# Success Response 를 돌려준다 (protocol.md 13장 STUN 사용 범위, RFC 5389).
#
# 검증은 클라이언트가 한다. 이 응답기는 트랜잭션 ID 를 그대로 되비추고 출발지를 매핑으로
# 적는 것 말고는 아무것도 하지 않는다.
$responder = {
    param([int]$port)
    $server = New-Object System.Net.Sockets.UdpClient($port, [System.Net.Sockets.AddressFamily]::InterNetwork)
    $from = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)
    $deadline = [Environment]::TickCount64 + 60000
    try {
        while ([Environment]::TickCount64 -lt $deadline) {
            if ($server.Available -le 0) { Start-Sleep -Milliseconds 20; continue }
            $data = $server.Receive([ref]$from)
            if ($data.Length -lt 20) { continue }
            $resp = New-Object byte[] 32
            $resp[0] = 0x01; $resp[1] = 0x01      # Binding Success Response
            $resp[2] = 0x00; $resp[3] = 0x0C      # 속성 12바이트
            $resp[4] = 0x21; $resp[5] = 0x12; $resp[6] = 0xA4; $resp[7] = 0x42
            [Array]::Copy($data, 8, $resp, 8, 12) # 트랜잭션 ID 를 되비춘다
            $resp[20] = 0x00; $resp[21] = 0x20    # XOR-MAPPED-ADDRESS
            $resp[22] = 0x00; $resp[23] = 0x08
            $resp[24] = 0x00; $resp[25] = 0x01    # family IPv4
            $xport = $from.Port -bxor 0x2112
            $resp[26] = [byte](($xport -shr 8) -band 0xFF)
            $resp[27] = [byte]($xport -band 0xFF)
            $addr = $from.Address.GetAddressBytes()
            $resp[28] = [byte]($addr[0] -bxor 0x21)
            $resp[29] = [byte]($addr[1] -bxor 0x12)
            $resp[30] = [byte]($addr[2] -bxor 0xA4)
            $resp[31] = [byte]($addr[3] -bxor 0x42)
            [void]$server.Send($resp, $resp.Length, $from)
        }
    } finally {
        $server.Close()
    }
}

function Check([string]$what, [bool]$ok) {
    if ($ok) {
        Write-Host ("ok    {0}" -f $what)
    } else {
        Write-Host ("FAIL  {0}" -f $what)
        $script:failed++
    }
}

try {
    $p1 = Start-Client 'p1' @()
    $procs['p1'] = $p1
    $port1 = Get-LocalPort $p1
    Check "p1 bound to a non-zero port ($port1)" ($port1 -ne 0)

    $p2 = Start-Client 'p2' @('--peer', "127.0.0.1:$port1")
    $procs['p2'] = $p2
    $port2 = Get-LocalPort $p2
    Check "p2 bound to a different port ($port2)" (($port2 -ne 0) -and ($port2 -ne $port1))

    # 같은 실행 파일을 한 기기에서 두 번 띄워도 둘 다 기동한다 (roadmap.md Phase 1 bind 계약).
    Check 'two clients run at the same time' ((-not $p1.proc.HasExited) -and (-not $p2.proc.HasExited))

    # P2 -> P1
    Send-Command $p2 'raw 500'
    $sent = Get-RxRaw $p2 0
    $recv = Get-RxRaw $p1 0
    Check 'p2 logged the sent datagram' ($null -ne $sent)
    Check 'p1 logged the received datagram' ($null -ne $recv)
    if ($null -ne $sent -and $null -ne $recv) {
        Check "p2->p1 length matches ($($sent.len))" ($sent.len -eq $recv.len -and $sent.len -eq 500)
        Check "p2->p1 sha256 matches ($($sent.sha))" ($sent.sha -eq $recv.sha)
        Check 'p1 saw the datagram come from loopback' ($recv.from -match '^127\.0\.0\.1:\d+$')
        Check 'p2 logged its own local endpoint as the source' ($sent.from -match '^0\.0\.0\.0:\d+$')
    }

    # P3 -> P2. 가운데 프로세스가 한 실행에서 보내기와 받기를 모두 돈다.
    $p3 = Start-Client 'p3' @('--peer', "127.0.0.1:$port2")
    $procs['p3'] = $p3
    Get-LocalPort $p3 | Out-Null
    Send-Command $p3 'raw 700'
    $sent3 = Get-RxRaw $p3 0
    $recv2 = Get-RxRaw $p2 1   # p2 의 첫 줄은 자기가 보낸 것이다
    Check 'p3 logged the sent datagram' ($null -ne $sent3)
    Check 'p2 logged the received datagram' ($null -ne $recv2)
    if ($null -ne $sent3 -and $null -ne $recv2) {
        Check "p3->p2 length matches ($($sent3.len))" ($sent3.len -eq $recv2.len -and $sent3.len -eq 700)
        Check "p3->p2 sha256 matches ($($sent3.sha))" ($sent3.sha -eq $recv2.sha)
    }

    # 다른 길이는 다른 해시를 낸다. 대조가 길이만 보는 것이 아님을 확인한다.
    if ($null -ne $sent -and $null -ne $sent3) {
        Check 'different payloads give different digests' ($sent.sha -ne $sent3.sha)
    }

    # 카운터 전량. 값이 0 인 것도 함께 낸다 (architecture.md 9장).
    Send-Command $p1 'counters'
    $m = Wait-ForLine $p1.err 'counter name=drop_magic value=0'
    Check 'the counters command dumps zero-valued counters' ($null -ne $m)

    # 알 수 없는 명령은 경고만 내고 죽지 않는다.
    Send-Command $p1 'nonsense'
    $m = Wait-ForLine $p1.err 'console\.rejected reason=unknown_command'
    Check 'an unknown command is rejected without exiting' (($null -ne $m) -and (-not $p1.proc.HasExited))

    # raw 로 시작할 뿐인 낱말은 어휘 밖이다 (architecture.md 3.5 기동 입력).
    # 접두만 보는 구현에서는 이것이 500바이트를 실제로 보낸다.
    Send-Command $p2 'raw500'
    $m = Wait-ForLine $p2.err 'console\.rejected reason=unknown_command command=raw500'
    Check 'raw500 is rejected instead of sending' ($null -ne $m)

    # 소켓 오류가 나도 프로세스가 죽지 않고 오류 코드를 로그에 남긴다
    # (roadmap.md Phase 1 검증).
    #
    # 브로드캐스트 주소를 고른 이유는 실패가 결정적이기 때문이다. SO_BROADCAST 를 켜지
    # 않은 소켓에서 그리로 보내면 sendto 가 실패한다.
    $p4 = Start-Client 'p4' @('--peer', '255.255.255.255:9')
    $procs['p4'] = $p4
    Get-LocalPort $p4 | Out-Null
    Send-Command $p4 'raw 10'
    $m = Wait-ForLine $p4.err 'socket\.error op=sendto code=(\d+)'
    Check 'a failed send logs socket.error with the code' ($null -ne $m)
    Check 'the process survives a failed send' (-not $p4.proc.HasExited)

    # 그 실패가 tx_err_send 로 센다 (protocol.md 6장).
    Send-Command $p4 'counters'
    $m = Wait-ForLine $p4.err 'counter name=tx_err_send value=1'
    Check 'the failed send raised tx_err_send' ($null -ne $m)

    # --- STUN (roadmap.md Phase 2 검증) ---------------------------------------
    #
    # 공개 서버를 부르지 않는다. 응답하지 않는 로컬 주소로 실패 경로를, 이 스크립트가
    # 띄우는 응답기로 성공 경로를 본다.

    # 응답 없는 서버 둘에 질의하면 마감 뒤 실패가 로그에 남고 프로세스는 살아 있다.
    # 상한은 5s x ceil(2 / 2) = 5s 다 (protocol.md 11장 타이머).
    $m = Wait-ForLine $p1.err 'session\.failed code=STUN_DISCOVERY_FAILED' 15000
    Check 'a silent STUN server list ends in STUN_DISCOVERY_FAILED' ($null -ne $m)
    Check 'the process survives the STUN failure' (-not $p1.proc.HasExited)

    # 목록이 하나면 기동 시 경고를 내고, 두 서버의 응답이 나올 수 없으므로 바로 실패다
    # (architecture.md 3.5 기동 입력).
    $p5 = Start-Client 'p5' @('--stun', '127.0.0.1:9')
    $procs['p5'] = $p5
    $m = Wait-ForLine $p5.err 'WARN stun\.config reason=single_server'
    Check 'a one-entry STUN list warns at startup' ($null -ne $m)
    $m = Wait-ForLine $p5.err 'session\.failed code=STUN_DISCOVERY_FAILED' 3000
    Check 'a one-entry STUN list fails without waiting for the deadline' ($null -ne $m)

    # 성공 경로. 응답기 둘을 띄우고 그 둘에 질의한다.
    $portA = Get-FreeUdpPort
    $portB = Get-FreeUdpPort
    $jobs += Start-Job -ScriptBlock $responder -ArgumentList $portA
    $jobs += Start-Job -ScriptBlock $responder -ArgumentList $portB
    # 응답기가 bind 를 마칠 시간을 준다. 늦어도 클라이언트의 재시도(500ms, 1s, 2s)가 덮는다.
    Start-Sleep -Milliseconds 1500

    $p6 = Start-Client 'p6' @('--stun', "127.0.0.1:$portA", '--stun', "127.0.0.1:$portB")
    $procs['p6'] = $p6
    $port6 = Get-LocalPort $p6
    $m = Wait-ForLine $p6.err "stun\.result server=127\.0\.0\.1:$portA mapped=127\.0\.0\.1:$port6" 15000
    Check 'the first STUN server reports the mapped endpoint' ($null -ne $m)
    $m = Wait-ForLine $p6.err "stun\.result server=127\.0\.0\.1:$portB mapped=127\.0\.0\.1:$port6" 15000
    Check 'the second STUN server reports the mapped endpoint' ($null -ne $m)
    # 두 서버가 답했으므로 실패 줄이 없다.
    $text = Read-Log $p6.err
    Check 'two answers mean no STUN failure' (-not ($text -match 'session\.failed'))

    # quit 으로 정상 종료하고 종료 코드가 0 이다.
    foreach ($key in @('p6', 'p5', 'p4', 'p3', 'p2', 'p1')) {
        Send-Command $procs[$key] 'quit'
    }
    foreach ($key in @('p6', 'p5', 'p4', 'p3', 'p2', 'p1')) {
        $exited = $procs[$key].proc.WaitForExit(5000)
        Check "$key exited after quit" $exited
        if ($exited) {
            Check "$key exit code is zero" ($procs[$key].proc.ExitCode -eq 0)
        }
    }
} finally {
    foreach ($client in $procs.Values) {
        if (-not $client.proc.HasExited) { $client.proc.Kill() }
        $client.writer.Dispose()
    }
    foreach ($job in $jobs) {
        Stop-Job -Job $job -ErrorAction SilentlyContinue
        Remove-Job -Job $job -Force -ErrorAction SilentlyContinue
    }
    Get-EventSubscriber | Unregister-Event -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host ("{0} checks failed" -f $failed)
if ($failed -gt 0) { exit 1 }
exit 0
