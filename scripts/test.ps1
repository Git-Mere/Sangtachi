# 시험을 돌린다. 빌드가 없거나 낡았으면 먼저 빌드한다.
# 이 스크립트가 출력하는 문자열은 ASCII 로만 적는다.
# 콘솔 코드 페이지가 UTF-8 이 아니면 한글이 깨져서 오류 메시지를 읽을 수 없다.
# 주석은 파일 안에만 머물므로 한국어로 적어도 된다.
#
#   pwsh -File scripts/test.ps1
#   pwsh -File scripts/test.ps1 -Filter wsa      이름에 wsa 가 든 케이스만

[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')]
    [string]$Config = 'Debug',
    [string]$BuildDir = '',
    [string]$Filter = ''
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
if ($BuildDir) {
    $buildDir = $BuildDir
} else {
    # 기본 위치의 근거는 build.ps1 머리에 있다.
    $buildDir = Join-Path $env:LOCALAPPDATA "Hamychi\build\$Config"
}

& (Join-Path $PSScriptRoot 'build.ps1') -Config $Config -BuildDir $buildDir
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }

$ctestArgs = @('--test-dir', $buildDir, '--output-on-failure')
if ($Filter) {
    $ctestArgs += @('-R', $Filter)
}

ctest @ctestArgs
if ($LASTEXITCODE -ne 0) { throw "tests failed ($LASTEXITCODE)" }

# 실행 파일의 기동 계약. 단위 시험이 볼 수 없는 종료 코드와 로그 줄을 본다.
# -Filter 를 준 실행은 단위 시험만 좁혀 보는 것이므로 건너뛴다.
if (-not $Filter) {
    & (Join-Path $PSScriptRoot 'cli-check.ps1') -Config $Config -BuildDir $buildDir
    if ($LASTEXITCODE -ne 0) { throw "cli check failed ($LASTEXITCODE)" }

    # 프로세스 여럿이 실제로 데이터그램을 주고받는지 본다.
    & (Join-Path $PSScriptRoot 'e2e-check.ps1') -Config $Config -BuildDir $buildDir
    if ($LASTEXITCODE -ne 0) { throw "end-to-end check failed ($LASTEXITCODE)" }
}

Write-Host 'all tests passed'
