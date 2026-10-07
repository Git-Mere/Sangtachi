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

# 케이스 표. expect 는 종료 코드, contains 는 표준 오류에 있어야 할 문자열이다.
$cases = @(
    @{ args = @('--server', $srv);                                    expect = 0; contains = 'INFO wsa.init version=' }
    @{ args = @('host', '--server', $srv);                            expect = 0; contains = 'INFO wsa.init version=' }
    @{ args = @('player', '--server', $srv, '--room', 'abcdef');      expect = 0; contains = 'INFO wsa.init version=' }
    @{ args = @('--server', $srv, '--peer', '192.0.2.5:30000');       expect = 0; contains = 'INFO wsa.init version=' }
    @{ args = @('--server', $srv, '--stun', 'a.example:3478');        expect = 0; contains = 'INFO wsa.init version=' }
    # 해석한 제어 서버 주소. IPv4 리터럴은 이름 해석을 거치지 않는다.
    @{ args = @('--server', $srv);                                    expect = 0; contains = 'INFO control.server host=127.0.0.1 resolved=127.0.0.1:9' }

    @{ args = @('bogus');                      expect = 2; contains = 'ERROR args.invalid reason=bad_role arg=bogus' }
    @{ args = @('host', 'player');             expect = 2; contains = 'reason=extra_positional' }
    @{ args = @('--nope');                     expect = 2; contains = 'reason=unknown_option arg=--nope' }
    @{ args = @('--peer');                     expect = 2; contains = 'reason=missing_value arg=--peer' }
    @{ args = @('--room', 'ABCDE0');           expect = 2; contains = 'reason=bad_room arg=ABCDE0' }
    @{ args = @('--peer', '192.0.2.5:0');      expect = 2; contains = 'reason=bad_port' }
    @{ args = @('--peer', 'a.example:3478');   expect = 2; contains = 'reason=bad_host' }
    @{ args = @('--stun', 'a.example');        expect = 2; contains = 'reason=missing_port' }
    @{ args = @('--server', '999.999.999.999'); expect = 2; contains = 'reason=bad_host' }
    # 방 코드는 create_room 응답으로만 생긴다 (architecture.md 3.5).
    @{ args = @('host', '--room', 'ABCDEF');   expect = 2; contains = 'reason=room_with_host arg=--room' }

    # Phase 3 이후의 필수 입력 (architecture.md 3.5 의 "필수" 열).
    @{ args = @();                                                    expect = 2; contains = 'ERROR args.invalid reason=missing_server arg=--server' }
    @{ args = @('host');                                              expect = 2; contains = 'reason=missing_server arg=--server' }
    @{ args = @('player', '--server', $srv);                          expect = 2; contains = 'reason=missing_room arg=--room' }
    @{ args = @('--room', 'abcdef', '--server', $srv);                expect = 2; contains = 'reason=room_without_role arg=--room' }
    # 제어 서버 주소를 해석하지 못하면 기동 실패다 (control_plane.md 8.2). 숫자 모양의 이름은
    # 묻지 않고 실패하므로 망이 없어도 결과가 같다 (platform/resolve.hpp).
    # 해석이 UDP bind 보다 먼저다 (control_plane.md 8.4 의 2번과 3번). 해석에 실패하면 socket.bind 줄이 없다.
    @{ args = @('--server', '1.0x7f');                                expect = 2; contains = 'ERROR control.server reason=resolve_failed host=1.0x7f port=8000'; absent = 'socket.bind' }

    # 값에 공백과 제어문자를 싣지 않는다. 둘 다 밑줄로 바꾼다 (architecture.md 9장).
    @{ args = @('a b');                        expect = 2; contains = 'arg=a_b' }
    @{ args = @("a`tb");                       expect = 2; contains = 'arg=a_b' }
    # 카운터 전량은 종료 절차에서 낸다. 값이 0 인 것도 함께 낸다.
    @{ args = @('--server', $srv);             expect = 0; contains = 'INFO counter name=drop_magic value=0' }
    @{ args = @('--server', $srv);             expect = 0; contains = 'INFO counter name=control_queue_dropped value=0' }
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

$failed = 0
foreach ($c in $cases) {
    # 인자가 맞으면 이 프로그램은 이벤트 루프로 들어가 quit 을 받을 때까지 끝나지 않는다.
    # 세션이 끝나도 로비로 갈 뿐 프로세스는 남는다 (concurrency.md 7장 로비). 프로세스를
    # 끝내는 것은 quit 하나이므로 표준 입력으로 그것을 넣고 닫는다.
    #
    # 표준 오류는 직접 읽는다. 파이프라인에 섞으면 PowerShell 이 ErrorRecord 로 감싸
    # 원래 줄을 그대로 볼 수 없다.
    $info = New-Object System.Diagnostics.ProcessStartInfo
    $info.FileName = $exe
    $info.Arguments = (($c.args | ForEach-Object { Format-Argument $_ }) -join ' ')
    $info.UseShellExecute = $false
    $info.RedirectStandardInput = $true
    $info.RedirectStandardError = $true
    $info.RedirectStandardOutput = $true
    $info.CreateNoWindow = $true

    # 두 스트림을 비동기로 비운다. 동기로 읽으면 끝나지 않는 클라이언트에서 그 읽기가 아래 상한보다 먼저
    # 막혀 상한이 듣지 않는다. 파이프 버퍼가 차서 자식이 멈추는 것도 막는다.
    $p = [System.Diagnostics.Process]::Start($info)
    try {
        $errTask = $p.StandardError.ReadToEndAsync()
        $outTask = $p.StandardOutput.ReadToEndAsync()
        # 기동 실패 케이스는 이미 끝났을 수 있다. 그때 닫힌 파이프에 쓰는 오류는 판정과 무관하다.
        try {
            $p.StandardInput.WriteLine('quit')
            $p.StandardInput.Close()
        } catch [System.IO.IOException] { }

        # 상한을 둔다. 매달리면 실패로 보고해야지 검사를 붙들면 안 된다.
        if (-not $p.WaitForExit(10000)) {
            $p.Kill()
            [void]$p.WaitForExit(5000)
            Write-Host ("FAIL  [{0}] did not exit within 10 seconds" -f ($c.args -join ' '))
            $failed++
            continue
        }
        # 프로세스가 끝났으므로 파이프가 닫혀 읽기도 곧 끝난다. 그래도 상한을 둔다.
        if (-not $errTask.Wait(5000)) {
            Write-Host ("FAIL  [{0}] stderr was not closed within 5 seconds after exit" -f ($c.args -join ' '))
            $failed++
            continue
        }
        [void]$outTask.Wait(5000)
        $err = $errTask.Result
        $code = $p.ExitCode
    } finally {
        if (-not $p.HasExited) { try { $p.Kill() } catch { } }
        $p.Dispose()
    }

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

Write-Host ("{0} passed, {1} failed of {2}" -f ($cases.Count - $failed), $failed, $cases.Count)

# 성공 코드를 명시한다. 마지막 케이스가 종료 코드 2 를 내는 프로그램이라, 적지 않으면
# 부르는 쪽의 $LASTEXITCODE 가 그 2 를 그대로 물고 간다.
if ($failed -gt 0) { exit 1 }
exit 0
