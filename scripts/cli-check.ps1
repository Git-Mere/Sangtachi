# 실행 파일의 기동 계약을 확인한다 (architecture.md 3.5 기동 입력).
# 이 스크립트가 출력하는 문자열은 ASCII 로만 적는다. 이유는 build.ps1 머리에 있다.
#
# 단위 시험이 볼 수 없는 것을 본다. 종료 코드와 표준 오류로 나가는 로그 줄이다.
#
#   pwsh -File scripts/cli-check.ps1
#   pwsh -File scripts/cli-check.ps1 -Config Release

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

# 케이스 표. expect 는 종료 코드, contains 는 표준 오류에 있어야 할 문자열이다.
# --server 는 Phase 3 이후 필수다 (architecture.md 3.5 기동 입력). 기동하는 케이스는 닫힌 루프백
# 포트를 준다. 로비 명령이 없으면 제어 요청이 나가지 않으므로 그 포트에 연결하지 않는다.
$srv = '127.0.0.1:9'
# 표시 이름. 기동하는 케이스가 쓴다. 없으면 기본 이름이다 (architecture.md 3.5 기동 입력).
$nm = 'Alice'

# 케이스 표. expect 는 종료 코드, contains 는 표준 오류에 있어야 할 문자열이다.
$cases = @(
    @{ args = @('--server', $srv, '--name', $nm);                                    expect = 0; contains = 'INFO wsa.init version='; absent = 'lobby.name' }
    @{ args = @('host', '--server', $srv, '--name', $nm);                            expect = 0; contains = 'INFO wsa.init version=' }
    @{ args = @('player', '--server', $srv, '--room', 'abcdef', '--name', $nm);      expect = 0; contains = 'INFO wsa.init version=' }
    @{ args = @('--server', $srv, '--name', $nm, '--stun', 'a.example:3478');        expect = 0; contains = 'INFO wsa.init version=' }
    # 해석한 제어 서버 주소. IPv4 리터럴은 이름 해석을 거치지 않는다.
    @{ args = @('--server', $srv, '--name', $nm);                                    expect = 0; contains = 'INFO control.server host=127.0.0.1 resolved=127.0.0.1:9' }

    @{ args = @('bogus');                      expect = 2; contains = 'ERROR args.invalid reason=bad_role arg=bogus' }
    @{ args = @('host', 'player');             expect = 2; contains = 'reason=extra_positional' }
    @{ args = @('--nope');                     expect = 2; contains = 'reason=unknown_option arg=--nope' }
    @{ args = @('--server');                   expect = 2; contains = 'reason=missing_value arg=--server' }
    @{ args = @('--room', 'ABCDE0');           expect = 2; contains = 'reason=bad_room arg=ABCDE0' }
    @{ args = @('--server', 'a.example:0');    expect = 2; contains = 'reason=bad_port' }
    # Phase 1 의 시험용 입력은 없다 (ADR 0028).
    @{ args = @('--peer', '192.0.2.5:30000');  expect = 2; contains = 'reason=unknown_option arg=--peer' }
    @{ args = @('--stun', 'a.example');        expect = 2; contains = 'reason=missing_port' }
    @{ args = @('--server', '999.999.999.999'); expect = 2; contains = 'reason=bad_host' }
    # 방 코드는 create_room 응답으로만 생긴다 (architecture.md 3.5).
    @{ args = @('host', '--room', 'ABCDEF');   expect = 2; contains = 'reason=room_with_host arg=--room' }

    # 표시 이름의 형식 (architecture.md 3.5, control_plane.md 2.7). 형식 위반은 필수 판정보다 먼저다.
    @{ args = @('--server', $srv, '--name', 'a-b');                 expect = 2; contains = 'ERROR args.invalid reason=bad_name arg=a-b' }
    @{ args = @('--server', $srv, '--name', 'a b');                 expect = 2; contains = 'reason=bad_name arg=a_b' }
    @{ args = @('--server', $srv, '--name', 'abcdefghijklmnopq');   expect = 2; contains = 'reason=bad_name arg=abcdefghijklmnopq' }
    @{ args = @('--server', $srv, '--name', '');                    expect = 2; contains = 'reason=bad_name' }
    @{ args = @('--server', $srv, '--name');                        expect = 2; contains = 'reason=missing_value arg=--name' }
    @{ args = @('--name', 'a-b');                                   expect = 2; contains = 'reason=bad_name arg=a-b' }
    # 대소문자를 바꾸지 않고 16자까지 받는다.
    @{ args = @('--server', $srv, '--name', 'Abcdefghijklmn_9');    expect = 0; contains = 'INFO wsa.init version=' }

    # 필수 입력 (architecture.md 3.5 의 "필수" 열). 시험 빌드에는 빌드에 넣은 서버 주소가 없어 --server 가 필수다.
    @{ args = @();                                                    expect = 2; contains = 'ERROR args.invalid reason=missing_server arg=--server' }
    @{ args = @('host');                                              expect = 2; contains = 'reason=missing_server arg=--server' }
    @{ args = @('player', '--server', $srv);                          expect = 2; contains = 'reason=missing_room arg=--room' }
    @{ args = @('--room', 'abcdef', '--server', $srv);                expect = 2; contains = 'reason=room_without_role arg=--room' }
    # --name 이 없으면 기본 이름으로 기동한다 (architecture.md 3.5). 이름은 로그 한 줄에 남는다.
    @{ args = @('--server', $srv);                                    expect = 0; contains = 'INFO lobby.name name=user_' }
    @{ args = @('host', '--server', $srv);                            expect = 0; contains = 'INFO lobby.name name=user_' }
    @{ args = @('player', '--server', $srv, '--room', 'abcdef');      expect = 0; contains = 'INFO lobby.name name=user_' }
    # 제어 서버 주소를 해석하지 못하면 기동 실패다 (control_plane.md 8.2). 숫자 모양의 이름은
    # 묻지 않고 실패하므로 망이 없어도 결과가 같다 (platform/resolve.hpp).
    # 해석이 UDP bind 보다 먼저다 (control_plane.md 8.4 의 2번과 3번). 해석에 실패하면 socket.bind 줄이 없다.
    @{ args = @('--server', '1.0x7f', '--name', $nm);                 expect = 2; contains = 'ERROR control.server reason=resolve_failed host=1.0x7f port=8000'; absent = 'socket.bind' }

    # 값에 공백과 제어문자를 싣지 않는다. 둘 다 밑줄로 바꾼다 (architecture.md 9장).
    @{ args = @('a b');                        expect = 2; contains = 'arg=a_b' }
    @{ args = @("a`tb");                       expect = 2; contains = 'arg=a_b' }
    # 카운터 전량은 종료 절차에서 낸다. 값이 0 인 것도 함께 낸다.
    @{ args = @('--server', $srv, '--name', $nm); expect = 0; contains = 'INFO counter name=drop_magic value=0' }
    @{ args = @('--server', $srv, '--name', $nm); expect = 0; contains = 'INFO counter name=control_queue_dropped value=0' }
)

# 인자 하나를 Windows 명령줄 규칙대로 감싼다.
#
# ProcessStartInfo.Arguments 는 문자열 하나다. 그냥 공백으로 이어 붙이면 공백이 든 인자가
# 둘로 쪼개져, 공백을 시험하려던 케이스가 그 공백을 프로그램에 전달하지 못한다. 실측으로
# 겪었다. Windows PowerShell 5.1 에는 ArgumentList 컬렉션이 없다.
function Format-Argument([string]$value) {
    if ($value -eq '') { return '""' }
    if ($value -notmatch '[\s"]') { return $value }
    # 따옴표 앞의 역슬래시를 두 배로 하고 따옴표를 이스케이프한다.
    $escaped = [regex]::Replace($value, '(\\*)"', '$1$1\"')
    # 끝의 역슬래시도 두 배로 한다. 닫는 따옴표를 먹지 않게 한다.
    $escaped = [regex]::Replace($escaped, '(\\+)$', '$1$1')
    return '"' + $escaped + '"'
}

# 클라이언트 하나를 띄운다. 표준 입력, 표준 오류, 표준 출력을 넘겨받는다.
#
# 인자가 맞으면 이 프로그램은 이벤트 루프로 들어가 quit 을 받을 때까지 끝나지 않는다.
# 세션이 끝나도 로비로 갈 뿐 프로세스는 남는다 (concurrency.md 7장 로비). 프로세스를
# 끝내는 것은 quit 하나이므로 Complete-Client 가 표준 입력으로 그것을 넣고 닫는다.
#
# 표준 오류는 직접 읽는다. 파이프라인에 섞으면 PowerShell 이 ErrorRecord 로 감싸
# 원래 줄을 그대로 볼 수 없다.
function Start-Client([string]$path, [string[]]$argList, [switch]$Adapter) {
    # 이 검사는 관리자 권한 없이 돈다. 가상 어댑터를 만들지 않는다 (architecture.md 3.5 의 --no-adapter).
    # 앞에 붙인다. 뒤에 붙이면 값이 빠진 옵션(`--server` 하나)이 이것을 값으로 읽어 다른 오류가 된다.
    if (-not $Adapter) { $argList = @('--no-adapter') + $argList }
    $info = New-Object System.Diagnostics.ProcessStartInfo
    $info.FileName = $path
    $info.Arguments = (($argList | ForEach-Object { Format-Argument $_ }) -join ' ')
    $info.UseShellExecute = $false
    $info.RedirectStandardInput = $true
    $info.RedirectStandardError = $true
    $info.RedirectStandardOutput = $true
    $info.CreateNoWindow = $true

    # 두 스트림을 비동기로 비운다. 동기로 읽으면 끝나지 않는 클라이언트에서 그 읽기가 아래 상한보다 먼저
    # 막혀 상한이 듣지 않는다. 파이프 버퍼가 차서 자식이 멈추는 것도 막는다.
    $startedUtc = [datetime]::UtcNow
    $p = [System.Diagnostics.Process]::Start($info)
    return @{ process = $p; id = $p.Id; startedUtc = $startedUtc; errTask = $p.StandardError.ReadToEndAsync(); outTask = $p.StandardOutput.ReadToEndAsync() }
}

# quit 을 넣고 끝나기를 기다린다. 끝나지 않았으면 error 에 이유를 담는다.
function Complete-Client($run) {
    $p = $run.process
    $result = @{ id = $run.id; code = $null; err = ''; error = $null; startedUtc = $run.startedUtc; endedUtc = $null }
    try {
        # 기동 실패 케이스는 이미 끝났을 수 있다. 그때 닫힌 파이프에 쓰는 오류는 판정과 무관하다.
        try {
            $p.StandardInput.WriteLine('quit')
            $p.StandardInput.Close()
        } catch [System.IO.IOException] { }

        # 상한을 둔다. 매달리면 실패로 보고해야지 검사를 붙들면 안 된다.
        if (-not $p.WaitForExit(10000)) {
            $p.Kill()
            [void]$p.WaitForExit(5000)
            $result.error = 'did not exit within 10 seconds'
            return $result
        }
        # 프로세스가 끝났으므로 파이프가 닫혀 읽기도 곧 끝난다. 그래도 상한을 둔다.
        if (-not $run.errTask.Wait(5000)) {
            $result.error = 'stderr was not closed within 5 seconds after exit'
            return $result
        }
        [void]$run.outTask.Wait(5000)
        $result.err = $run.errTask.Result
        $result.code = $p.ExitCode
        $result.endedUtc = [datetime]::UtcNow
    } finally {
        if (-not $p.HasExited) { try { $p.Kill() } catch { } }
        $p.Dispose()
    }
    return $result
}

$failed = 0
foreach ($c in $cases) {
    $r = Complete-Client (Start-Client $exe $c.args)
    if ($r.error) {
        Write-Host ("FAIL  [{0}] {1}" -f ($c.args -join ' '), $r.error)
        $failed++
        continue
    }
    $err = $r.err
    $code = $r.code

    $label = if ($c.args.Count -eq 0) { '(no arguments)' } else { ($c.args -join ' ') }
    $codeOk = ($code -eq $c.expect)
    $textOk = $err.Contains($c.contains)
    # absent 가 있으면 표준 오류에 그 문자열이 없어야 한다.
    $absentOk = (-not $c.ContainsKey('absent')) -or (-not $err.Contains($c.absent))

    if ($codeOk -and $textOk -and $absentOk) {
        Write-Host ("ok    [{0}]" -f $label)
    } else {
        $failed++
        Write-Host ("FAIL  [{0}] exit={1} expected={2}" -f $label, $code, $c.expect)
        if (-not $absentOk) {
            Write-Host ("      stderr should not contain: {0}" -f $c.absent)
        }
        if (-not $textOk) {
            Write-Host ("      stderr did not contain: {0}" -f $c.contains)
            Write-Host ("      stderr was: {0}" -f ($err -replace "`r?`n", ' | ').Trim())
        }
    }
}

# 관리자가 아니면 어댑터를 만들기 전에 기동 실패다 (architecture.md 7장 어댑터 수명, windows-prereq.md 1절).
# 관리자로 돌린 실행에서는 어댑터가 실제로 생기므로 이 검사를 하지 않는다. 그쪽은 adapter-check.ps1 이 본다.
$elevated = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $elevated) {
    $r = Complete-Client (Start-Client $exe @('--server', $srv, '--name', $nm) -Adapter)
    $label = 'without --no-adapter and not elevated'
    if ($r.error) {
        Write-Host ("FAIL  [{0}] {1}" -f $label, $r.error)
        $failed++
    } elseif ($r.code -eq 2 -and $r.err.Contains('ERROR adapter.error reason=not_elevated') -and
              -not $r.err.Contains('socket.bind')) {
        Write-Host ("ok    [{0}]" -f $label)
    } else {
        Write-Host ("FAIL  [{0}] exit={1} stderr: {2}" -f $label, $r.code, ($r.err -replace "`r?`n", ' | ').Trim())
        $failed++
    }
}

# 로컬 기록 파일의 기동 계약 (architecture.md 9장 로컬 기록 파일의 경로). 케이스 표가 보지 않는 것을 본다.
# 파일이 실제로 생겼는지, 이름의 PID 가 그 프로세스의 것인지, 기동 실패에서 파일이 없는지다.
$total = $cases.Count
function Report-Check([string]$label, [bool]$ok, [string]$detail) {
    $script:total++
    if ($ok) {
        Write-Host ("ok    [record] {0}" -f $label)
    } else {
        $script:failed++
        Write-Host ("FAIL  [record] {0}: {1}" -f $label, $detail)
    }
}

# 정상 기동 한 번의 결과에서 기록 파일을 확인한다. 고친 것이 없으면 빈 문자열이다.
# 역할이 없으면 시도가 없으므로 줄도 없다 (9장 "무엇이 시도 하나인가").
# 이름의 기동 시각은 UTC 다. 띄우기 전과 끝난 뒤의 UTC 사이에 있어야 한다(초 단위).
function Test-RecordOpened($r, [string]$exeDir) {
    if ($r.error) { return $r.error }
    if ($r.code -ne 0) { return "exit=$($r.code)" }
    $m = [regex]::Match($r.err, '(?m)^INFO record\.open file=(\S+)\r?$')
    if (-not $m.Success) { return 'no record.open line' }
    $name = $m.Groups[1].Value
    $nm2 = [regex]::Match($name, '^\d{8}T\d{6}Z-(\d+)\.log$')
    if (-not $nm2.Success) { return "bad file name $name" }
    if ([int64]$nm2.Groups[1].Value -ne $r.id) { return "pid in $name is not $($r.id)" }
    $stamp = [datetime]::ParseExact($name.Substring(0, 16), "yyyyMMdd'T'HHmmss'Z'",
        [System.Globalization.CultureInfo]::InvariantCulture,
        [System.Globalization.DateTimeStyles]::AdjustToUniversal -bor [System.Globalization.DateTimeStyles]::AssumeUniversal)
    $lower = $r.startedUtc.AddTicks(-($r.startedUtc.Ticks % [TimeSpan]::TicksPerSecond))
    if ($stamp -lt $lower -or $stamp -gt $r.endedUtc) { return "start time in $name is outside $($r.startedUtc.ToString('o'))..$($r.endedUtc.ToString('o'))" }
    $path = Join-Path (Join-Path $exeDir 'records') $name
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { return "file not found: $path" }
    if ((Get-Item -LiteralPath $path).Length -ne 0) { return "file is not empty: $path" }
    return ''
}

$exeDir = Split-Path -Parent $exe
$okArgs = @('--server', $srv, '--name', $nm)

# 같은 기기의 두 프로세스가 서로 다른 파일을 만든다 (roadmap.md Phase 4 기록과 실패 코드).
$runA = Start-Client $exe $okArgs
$runB = Start-Client $exe $okArgs
$resA = Complete-Client $runA
$resB = Complete-Client $runB
$whyA = Test-RecordOpened $resA $exeDir
$whyB = Test-RecordOpened $resB $exeDir
Report-Check 'normal start opens records\<UTC start>-<PID>.log next to the executable' ($whyA -eq '') $whyA
Report-Check 'a second process at the same time opens its own file' ($whyB -eq '' -and $resA.id -ne $resB.id) "$whyB pids=$($resA.id),$($resB.id)"

# 인자 오류로 기동에 실패하면 파일을 만들지 않는다. 인자 검사 뒤에 연다.
# 앞선 실행의 파일이 쌓여 있고 PID 는 다시 쓰인다. 그래서 띄우기 전에 없던 파일만 본다.
function Get-RecordNames([string]$dir) {
    return @(Get-ChildItem -LiteralPath (Join-Path $dir 'records') -Filter '*.log' -ErrorAction SilentlyContinue | ForEach-Object { $_.Name })
}
$before = Get-RecordNames $exeDir
$bad = Complete-Client (Start-Client $exe @('bogus'))
$leftover = @(Get-RecordNames $exeDir | Where-Object { $before -notcontains $_ -and $_.EndsWith("-$($bad.id).log") })
# 실패가 인자 검사의 것이어야 한다. 앞서 열다가 이름이 부딪혀 실패한 것이면 새 파일이 없어도 통과로 보지 않는다.
Report-Check 'an argument error creates no record file' ($bad.code -eq 2 -and $leftover.Count -eq 0 -and $bad.err.Contains('ERROR args.invalid reason=bad_role arg=bogus') -and -not $bad.err.Contains('record.open') -and -not $bad.err.Contains('socket.error')) "exit=$($bad.code) files=$($leftover.Count) stderr=$(($bad.err -replace "`r?`n", ' | ').Trim())"

# 폴더나 파일을 만들 수 없으면 기동 실패다. 새 실행 파일 폴더에 실행 파일을 복사하고 현재 사용자에게
# 만들기를 거부하는 ACE 를 단다. 먼저 거부 없이 기동해 복사본이 그 자체로 도는지 본다. 그래야 아래
# 실패가 거부 때문이라고 읽을 수 있다.
$sid = [System.Security.Principal.WindowsIdentity]::GetCurrent().User.Value
function Invoke-Icacls([string[]]$icaclsArgs) {
    $out = & icacls @icaclsArgs 2>&1
    if ($LASTEXITCODE -ne 0) { throw "icacls $($icaclsArgs -join ' ') failed: $out" }
}

$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("sangtachi-cli-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $tmp | Out-Null
try {
    $tmpExe = Join-Path $tmp 'sangtachi_client.exe'
    Copy-Item -LiteralPath $exe -Destination $tmpExe

    # 하위 폴더를 만들 수 없는 폴더. `records` 가 없다.
    $sub = Join-Path $tmp 'nodir'
    New-Item -ItemType Directory -Path $sub | Out-Null
    Copy-Item -LiteralPath $exe -Destination (Join-Path $sub 'sangtachi_client.exe')
    # 파일을 만들 수 없는 `records` 가 이미 있는 폴더.
    $sub2 = Join-Path $tmp 'nofile'
    New-Item -ItemType Directory -Path (Join-Path $sub2 'records') | Out-Null
    Copy-Item -LiteralPath $exe -Destination (Join-Path $sub2 'sangtachi_client.exe')

    $copy = Complete-Client (Start-Client $tmpExe $okArgs)
    $whyCopy = Test-RecordOpened $copy $tmp
    Report-Check 'a copied executable opens its record file in its own folder' ($whyCopy -eq '') $whyCopy

    Invoke-Icacls @($sub, '/deny', "*${sid}:(AD)")
    $noDir = Complete-Client (Start-Client (Join-Path $sub 'sangtachi_client.exe') $okArgs)
    Invoke-Icacls @($sub, '/remove:d', "*$sid")
    Report-Check 'no right to create records\ is a startup failure' ($noDir.code -eq 2 -and $noDir.err.Contains('ERROR socket.error op=CreateDirectoryW') -and -not (Test-Path -LiteralPath (Join-Path $sub 'records'))) "exit=$($noDir.code) stderr=$(($noDir.err -replace "`r?`n", ' | ').Trim())"

    $recDir = Join-Path $sub2 'records'
    Invoke-Icacls @($recDir, '/deny', "*${sid}:(WD)")
    $noFile = Complete-Client (Start-Client (Join-Path $sub2 'sangtachi_client.exe') $okArgs)
    Invoke-Icacls @($recDir, '/remove:d', "*$sid")
    Report-Check 'no right to create the file is a startup failure' ($noFile.code -eq 2 -and $noFile.err.Contains('ERROR socket.error op=CreateFileW') -and @(Get-ChildItem -LiteralPath $recDir).Count -eq 0) "exit=$($noFile.code) stderr=$(($noFile.err -replace "`r?`n", ' | ').Trim())"
} finally {
    # 거부 ACE 가 남아 있으면 지우기가 막힌다. 위에서 이미 걷었어도 다시 걷는다. 걷기와 지우기는 모든 자리에
    # 시도하고, 남은 것은 아래에서 실패로 알린다.
    $cleanup = @()
    foreach ($d in @((Join-Path $tmp 'nodir'), (Join-Path $tmp 'nofile\records'))) {
        # Windows PowerShell 5.1 은 Stop 아래에서 네이티브 표준 오류를 예외로 바꾼다. 하나가 던져도 나머지를 시도한다.
        try {
            if (Test-Path -LiteralPath $d) {
                $out = & icacls $d '/remove:d' "*$sid" 2>&1
                if ($LASTEXITCODE -ne 0) { $cleanup += "icacls /remove:d $d failed: $out" }
            }
        } catch {
            $cleanup += "icacls /remove:d $d failed: $_"
        }
    }
    try {
        Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
    } catch { }
    if (Test-Path -LiteralPath $tmp) { $cleanup += "left behind: $tmp" }
}
Report-Check 'deny ACEs and the temporary folder are cleaned up' ($cleanup.Count -eq 0) ($cleanup -join '; ')

Write-Host ("{0} passed, {1} failed of {2}" -f ($total - $failed), $failed, $total)

# 성공 코드를 명시한다. 마지막 케이스가 종료 코드 2 를 내는 프로그램이라, 적지 않으면
# 부르는 쪽의 $LASTEXITCODE 가 그 2 를 그대로 물고 간다.
if ($failed -gt 0) { exit 1 }
exit 0
