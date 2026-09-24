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

$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $BuildDir) {
    # 기본 위치의 근거는 build.ps1 머리에 있다.
    $BuildDir = Join-Path $env:LOCALAPPDATA "Sangtachi\build\$Config"
}
$exe = Join-Path $BuildDir 'client\sangtachi_client.exe'
if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
    throw "client executable not found: $exe. Run scripts/build.ps1 first."
}

# 케이스 표. expect 는 종료 코드, contains 는 표준 오류에 있어야 할 문자열이다.
$cases = @(
    @{ args = @();                             expect = 0; contains = 'INFO wsa.init version=' }
    @{ args = @('host');                       expect = 0; contains = 'INFO wsa.init version=' }
    @{ args = @('player');                     expect = 0; contains = 'INFO wsa.init version=' }
    @{ args = @('--peer', '192.0.2.5:30000');  expect = 0; contains = 'INFO wsa.init version=' }
    @{ args = @('--room', 'abcdef');           expect = 0; contains = 'INFO wsa.init version=' }
    @{ args = @('--server', 'a.example');      expect = 0; contains = 'INFO wsa.init version=' }
    @{ args = @('--stun', 'a.example:3478');   expect = 0; contains = 'INFO wsa.init version=' }

    @{ args = @('bogus');                      expect = 2; contains = 'ERROR args.invalid reason=bad_role arg=bogus' }
    @{ args = @('host', 'player');             expect = 2; contains = 'reason=extra_positional' }
    @{ args = @('--nope');                     expect = 2; contains = 'reason=unknown_option arg=--nope' }
    @{ args = @('--peer');                     expect = 2; contains = 'reason=missing_value arg=--peer' }
    @{ args = @('--room', 'ABCDE0');           expect = 2; contains = 'reason=bad_room arg=ABCDE0' }
    @{ args = @('--peer', '192.0.2.5:0');      expect = 2; contains = 'reason=bad_port' }
    @{ args = @('--peer', 'a.example:3478');   expect = 2; contains = 'reason=bad_host' }
    @{ args = @('--stun', 'a.example');        expect = 2; contains = 'reason=missing_port' }
    @{ args = @('--server', '999.999.999.999'); expect = 2; contains = 'reason=bad_host' }

    # 값에 공백과 제어문자를 싣지 않는다. 둘 다 밑줄로 바꾼다 (architecture.md 9장).
    @{ args = @('a b');                        expect = 2; contains = 'arg=a_b' }
    @{ args = @("a`tb");                       expect = 2; contains = 'arg=a_b' }
    # 카운터 전량은 종료 절차에서 낸다. 값이 0 인 것도 함께 낸다.
    @{ args = @();                             expect = 0; contains = 'INFO counter name=drop_magic value=0' }
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
    # 인자가 맞으면 이 프로그램은 이벤트 루프로 들어가 quit 을 받을 때까지 끝나지 않는다
    # (architecture.md 3.2.7 종료). 그래서 표준 입력으로 quit 을 넣고 닫는다.
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

    $p = [System.Diagnostics.Process]::Start($info)
    $p.StandardInput.WriteLine('quit')
    $p.StandardInput.Close()
    $err = $p.StandardError.ReadToEnd()
    $p.StandardOutput.ReadToEnd() | Out-Null

    # 상한을 둔다. 매달리면 실패로 보고해야지 검사를 붙들면 안 된다.
    if (-not $p.WaitForExit(10000)) {
        $p.Kill()
        Write-Host ("FAIL  [{0}] did not exit within 10 seconds" -f ($c.args -join ' '))
        $failed++
        continue
    }
    $code = $p.ExitCode

    $label = if ($c.args.Count -eq 0) { '(no arguments)' } else { ($c.args -join ' ') }
    $codeOk = ($code -eq $c.expect)
    $textOk = $err.Contains($c.contains)

    if ($codeOk -and $textOk) {
        Write-Host ("ok    [{0}]" -f $label)
    } else {
        $failed++
        Write-Host ("FAIL  [{0}] exit={1} expected={2}" -f $label, $code, $c.expect)
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
