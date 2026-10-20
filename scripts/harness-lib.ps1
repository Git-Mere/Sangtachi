# 하네스 위에서 클라이언트를 띄우는 시험 스크립트의 공통 함수. scripts/lobby-check.ps1 과 scripts/session-check.ps1 이
# 점 소스로 읽는다. 혼자 돌리지 않는다.
#
# 읽기 전에 부르는 쪽이 정해 둘 것: $work(로그 폴더), $exe, $py, $repoRoot, $failed, $notrun, $clients, $harness.
# $Sabotage 는 lobby-check 만 쓴다(없으면 주입을 늘 건다).
# 이 파일이 출력하는 문자열은 ASCII 로만 적는다. 이유는 build.ps1 머리에 있다.

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

# 클라이언트마다 다른 표시 이름. --name 은 Phase 4 이후 필수이고 방 안에서 유일해야 한다 (architecture.md 3.5,
# control_plane.md 2.7). 장면 이름에는 2.7 밖의 글자(붙임표)가 있어 그대로 쓰지 않는다.
$script:nameSeq = 0

# -Adapter 면 가상 어댑터를 만든다. 관리자 권한이 있어야 하고 어댑터 기동 단계(서브넷 검사 포함)가 몇 초 걸린다
# (architecture.md 7장 어댑터 수명). 아니면 --no-adapter 를 붙인다.
function Start-Client([string]$name, [string[]]$clientArgs = @(), [string]$exePath = $script:exe, [switch]$Adapter) {
    if ($clientArgs -notcontains '--server') { $clientArgs = @('--server', "127.0.0.1:$script:cpPort") + $clientArgs }
    if ($clientArgs -notcontains '--name') {
        $script:nameSeq++
        $clientArgs = @('--name', "c$($script:nameSeq)") + $clientArgs
    }
    if ($clientArgs -notcontains '--stun') {
        $clientArgs = $clientArgs + @('--stun', "127.0.0.1:$($script:stunPorts[0])", '--stun', "127.0.0.1:$($script:stunPorts[1])")
    }
    # 한 기기의 여러 프로세스다. 가상 어댑터를 만들지 않는다 (architecture.md 3.5 의 --no-adapter).
    if (-not $Adapter -and $clientArgs -notcontains '--no-adapter') { $clientArgs = @('--no-adapter') + $clientArgs }
    $c = Start-Logged $name $exePath (($clientArgs | ForEach-Object { Quote-Arg $_ }) -join ' ') $script:repoRoot
    $script:clients += $c
    $m = Wait-Match $c.err 0 'socket\.bind local=0\.0\.0\.0:(\d+)' $(if ($Adapter) { 60000 } else { 10000 })
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

# 지금 닫혀 있는 루프백 TCP 포트 하나. 연결 거부를 만들 때 쓴다.
function Get-FreeTcpPort {
    $l = New-Object System.Net.Sockets.TcpListener([System.Net.IPAddress]::Loopback, 0)
    $l.Start()
    try { return $l.LocalEndpoint.Port } finally { $l.Stop() }
}

# ---------------------------------------------------------------- 시험 빌드와 하네스의 시작과 끝

# 시험 빌드 클라이언트. 클라이언트 대상만 짓는다. 결과는 따로 둔 폴더에 남아 다음 실행이 증분으로 짓는다.
# 지은 실행 파일의 경로를 돌려준다.
function Build-TestClient([string]$Config, [string]$BuildDir, [string]$TestBuildDir) {
    if (-not $TestBuildDir) { $TestBuildDir = $BuildDir.TrimEnd('\', '/') + '-testbuild' }
    . (Join-Path $PSScriptRoot 'vsdevshell.ps1')
    # 실패하면 마지막 줄들을 보인다. 버리면 무엇이 실패했는지 다시 지어 봐야 안다.
    $out = cmake -S $script:repoRoot -B $TestBuildDir -G Ninja "-DCMAKE_BUILD_TYPE=$Config" -DSANGTACHI_TEST_BUILD=ON -DSANGTACHI_BUILD_TESTS=OFF 2>&1
    if ($LASTEXITCODE -ne 0) { $out | Select-Object -Last 40 | ForEach-Object { Write-Host $_ }; throw "test-build configure failed ($LASTEXITCODE)" }
    $out = cmake --build $TestBuildDir --target sangtachi_client 2>&1
    if ($LASTEXITCODE -ne 0) { $out | Select-Object -Last 40 | ForEach-Object { Write-Host $_ }; throw "test-build build failed ($LASTEXITCODE)" }
    return (Join-Path $TestBuildDir 'client\sangtachi_client.exe')
}

# 하네스를 띄우고 ready 줄을 기다린다. 뜨면 참이고 $cpPort, $adminPort, $stunPorts, $mappedIp 를 채운다.
# 뜨지 않으면 실패 하나를 세고 거짓이다.
function Start-Harness([string]$Ddb) {
    $script:harness = Start-Logged 'harness' $script:py ("tests\harness\cp_harness.py --exit-on-stdin-eof --stun 2 --ddb " + $Ddb) (Join-Path $script:repoRoot 'control-server')
    $ready = Wait-Match $script:harness.err 0 '^HARNESS ready cp_port=(\d+) admin_port=(\d+) stun_ports=(\d+),(\d+) table=\S+ mapped_ip=(\S+)\r?$' 90000
    if ($null -eq $ready) {
        Write-Host 'FAIL  the control-plane harness did not start. Its output:'
        Write-Host (Read-Log $script:harness.err)
        Write-Host 'If DynamoDB local is not running, start it with:'
        Write-Host '  docker run -d --rm --name sangtachi-ddb -p 127.0.0.1:8001:8000 amazon/dynamodb-local:3.3.1 -jar DynamoDBLocal.jar -inMemory -sharedDb'
        $script:failed++
        return $false
    }
    $script:cpPort = [int]$ready.Groups[1].Value
    $script:adminPort = [int]$ready.Groups[2].Value
    $script:stunPorts = @([int]$ready.Groups[3].Value, [int]$ready.Groups[4].Value)
    # 응답기는 출발지 포트와 이 주소를 매핑으로 적는다. 루프백이면 서버의 후보 위생이 거부한다 (cp_harness.py).
    $script:mappedIp = $ready.Groups[5].Value
    Write-Host ("harness: control plane 127.0.0.1:{0}, STUN 127.0.0.1:{1},{2}" -f $script:cpPort, $script:stunPorts[0], $script:stunPorts[1])
    return $true
}

# 클라이언트를 죽이고 하네스를 멈춘다. finally 에서 부른다.
function Stop-Harness {
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
}
