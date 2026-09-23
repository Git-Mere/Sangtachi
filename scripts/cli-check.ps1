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
    $BuildDir = Join-Path $env:LOCALAPPDATA "Hamychi\build\$Config"
}
$exe = Join-Path $BuildDir 'client\hamychi_client.exe'
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

    @{ args = @('bogus');                      expect = 2; contains = 'ERROR args.invalid reason=bad_role arg="bogus"' }
    @{ args = @('host', 'player');             expect = 2; contains = 'reason=extra_positional' }
    @{ args = @('--nope');                     expect = 2; contains = 'reason=unknown_option arg="--nope"' }
    @{ args = @('--peer');                     expect = 2; contains = 'reason=missing_value arg="--peer"' }
    @{ args = @('--room', 'ABCDE0');           expect = 2; contains = 'reason=bad_room arg="ABCDE0"' }
    @{ args = @('--peer', '192.0.2.5:0');      expect = 2; contains = 'reason=bad_port' }
    @{ args = @('--peer', 'a.example:3478');   expect = 2; contains = 'reason=bad_host' }
    @{ args = @('--stun', 'a.example');        expect = 2; contains = 'reason=missing_port' }
    @{ args = @('--server', '999.999.999.999'); expect = 2; contains = 'reason=bad_host' }

    # 값에 공백이 들어도 한 줄이 k=v 로 읽혀야 한다. 값을 따옴표로 감싸는 이유다.
    @{ args = @('a b');                        expect = 2; contains = 'arg="a b"' }
    # 제어문자는 공백 하나로 바꿔 싣는다 (architecture.md 9장 로그 출력).
    @{ args = @("a`tb");                       expect = 2; contains = 'arg="a b"' }
)

$failed = 0
foreach ($c in $cases) {
    $stderrFile = [System.IO.Path]::GetTempFileName()
    try {
        # Start-Process 를 쓰지 않는다. 빈 -ArgumentList 를 거부한다.
        # 표준 오류를 파일로 보낸다. 파이프라인에 섞으면 PowerShell 이 그것을
        # ErrorRecord 로 감싸 원래 줄을 그대로 볼 수 없다.
        $previous = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        if ($c.args.Count -eq 0) {
            & $exe 2>$stderrFile | Out-Null
        } else {
            & $exe @($c.args) 2>$stderrFile | Out-Null
        }
        $code = $LASTEXITCODE
        $ErrorActionPreference = $previous

        $err = Get-Content -LiteralPath $stderrFile -Raw -ErrorAction SilentlyContinue
        if ($null -eq $err) { $err = '' }
    } finally {
        Remove-Item -LiteralPath $stderrFile -Force -ErrorAction SilentlyContinue
    }

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
