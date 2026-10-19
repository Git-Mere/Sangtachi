# 제어 서버 없이 도는 프로세스 수준 확인 (roadmap.md Phase 1 검증).
# 이 스크립트가 출력하는 문자열은 ASCII 로만 적는다. 이유는 build.ps1 머리에 있다.
#
# 단위 시험이 볼 수 없는 것을 본다. 실행 파일 둘을 한 기기에서 동시에 띄우고, 각자 0 이 아닌
# 서로 다른 포트에 bind 하는지, 콘솔 명령이 표준 입력으로 들어가 처리되는지, quit 으로 끝나는지다.
# 두 프로세스 사이의 송수신은 보지 않는다. 그것은 세션이 있어야 하는 Phase 4 의 DATA 왕복이 본다
# (ADR 0028).

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

# 이 스크립트는 로비 명령을 치지 않으므로 STUN 이 돌지 않는다 (control_plane.md 8.4). 그래도
# 목록은 기동 시 한 번 해석한다(architecture.md 3.5 기동 입력). 기본 목록이면 공개 서버의 이름을
# 묻게 되므로 응답하지 않는 로컬 주소 둘을 기본으로 준다. 직접 --stun 을 주는 호출은 그대로 쓴다.
$deadStun = @('--stun', '127.0.0.1:9', '--stun', '127.0.0.1:19')

function Start-Client([string]$name, [string[]]$clientArgs) {
    if ($clientArgs -notcontains '--stun') { $clientArgs = $deadStun + $clientArgs }
    # --server 는 Phase 3 이후 필수다 (architecture.md 3.5 기동 입력). 닫힌 루프백 포트를 준다.
    # 로비 명령을 치지 않으므로 이 스크립트에서는 제어 요청이 나가지 않는다.
    if ($clientArgs -notcontains '--server') { $clientArgs = @('--server', '127.0.0.1:9') + $clientArgs }
    # --name 은 Phase 4 이후 필수다 (architecture.md 3.5 기동 입력). 이 검사는 제어 요청을 보내지 않으므로 같은 이름이어도 된다.
    if ($clientArgs -notcontains '--name') { $clientArgs = @('--name', 'e2e') + $clientArgs }
    # 한 기기의 여러 프로세스다. 가상 어댑터를 만들지 않는다 (architecture.md 3.5 의 --no-adapter).
    $clientArgs = @('--no-adapter') + $clientArgs
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

    # 파일 열기부터 이벤트 등록까지가 한 가드다. 중간에 실패하면 등록한 이벤트를 풀고, 연 파일을 닫고, 띄운 자식을
    # 죽인 뒤 다시 던진다. 돌려준 뒤에는 부르는 쪽이 $procs 에 넣는다(그 사이에 실패할 수 있는 일이 없다).
    $writer = $null
    $p = $null
    $sourceId = "e2e-stderr-$name-" + [guid]::NewGuid().ToString('N')
    try {
        $writer = [System.IO.StreamWriter]::new($errPath, $false)
        $writer.AutoFlush = $true
        $p = [System.Diagnostics.Process]::Start($info)
        if ($null -eq $p) { throw "${name}: process did not start" }
        # 표준 오류를 비동기로 파일에 옮긴다.
        $handler = {
            if ($null -ne $EventArgs.Data) { $Event.MessageData.WriteLine($EventArgs.Data) }
        }
        Register-ObjectEvent -InputObject $p -EventName ErrorDataReceived -Action $handler `
            -MessageData $writer -SourceIdentifier $sourceId | Out-Null
        $p.BeginErrorReadLine()
        return @{ proc = $p; err = $errPath; writer = $writer; name = $name }
    } catch {
        Unregister-Event -SourceIdentifier $sourceId -ErrorAction SilentlyContinue
        if ($null -ne $p) {
            try { if (-not $p.HasExited) { $p.Kill(); [void]$p.WaitForExit(5000) } } catch { }
            $p.Dispose()
        }
        if ($null -ne $writer) { $writer.Dispose() }
        throw
    }
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

    $p2 = Start-Client 'p2' @()
    $procs['p2'] = $p2
    $port2 = Get-LocalPort $p2
    Check "p2 bound to a different port ($port2)" (($port2 -ne 0) -and ($port2 -ne $port1))

    # 같은 실행 파일을 한 기기에서 두 번 띄워도 둘 다 기동한다 (roadmap.md Phase 1 bind 계약).
    Check 'two clients run at the same time' ((-not $p1.proc.HasExited) -and (-not $p2.proc.HasExited))

    # 카운터 전량. 값이 0 인 것도 함께 낸다 (architecture.md 9장).
    Send-Command $p1 'counters'
    $m = Wait-ForLine $p1.err 'counter name=drop_magic value=0'
    Check 'the counters command dumps zero-valued counters' ($null -ne $m)

    # 알 수 없는 명령은 경고만 내고 죽지 않는다.
    Send-Command $p1 'nonsense'
    $m = Wait-ForLine $p1.err 'console\.rejected reason=unknown_command'
    Check 'an unknown command is rejected without exiting' (($null -ne $m) -and (-not $p1.proc.HasExited))

    # Phase 2 의 STUN 확인은 scripts/lobby-check.ps1 로 옮겼다. 통합 뒤 STUN 은 기동 시가 아니라 host 나
    # join 시도마다 돈다 (control_plane.md 8.4). 그래서 제어 서버가 있어야 밟을 수 있다.

    # quit 으로 정상 종료하고 종료 코드가 0 이다.
    foreach ($key in @('p2', 'p1')) {
        Send-Command $procs[$key] 'quit'
    }
    foreach ($key in @('p2', 'p1')) {
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
    Get-EventSubscriber | Unregister-Event -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host ("{0} checks failed" -f $failed)
if ($failed -gt 0) { exit 1 }
exit 0
