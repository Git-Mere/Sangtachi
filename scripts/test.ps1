# 시험을 돌린다. 빌드가 없거나 낡았으면 먼저 빌드한다.
# 이 스크립트가 출력하는 문자열은 ASCII 로만 적는다.
# 콘솔 코드 페이지가 UTF-8 이 아니면 한글이 깨져서 오류 메시지를 읽을 수 없다.
# 주석은 파일 안에만 머물므로 한국어로 적어도 된다.
#
#   pwsh -File scripts/test.ps1
#   pwsh -File scripts/test.ps1 -Filter wsa      이름에 wsa 가 든 케이스만
#   pwsh -File scripts/test.ps1 -Jobs 1          케이스를 하나씩 돌린다
#   pwsh -File scripts/test.ps1 -Asan            ASan 빌드로 단위 시험만 돌린다 (roadmap.md Phase 5 퍼즈)
#
# 케이스는 기본으로 코어 수만큼 동시에 돈다 (ctest -j). 케이스마다 30초 상한은 그대로다.

[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')]
    [string]$Config = 'Debug',
    [string]$BuildDir = '',
    [string]$Filter = '',
    [ValidateRange(0, 256)]
    [int]$Jobs = 0,
    [switch]$Asan
)

$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'buildpath.ps1')

# 기본 위치의 근거는 build.ps1 머리에 있고 판정은 buildpath.ps1 이 한다.
$dirName = if ($Asan) { "$Config-asan" } else { $Config }
$buildDir = Resolve-BuildDir -Config $dirName -Explicit $BuildDir

& (Join-Path $PSScriptRoot 'build.ps1') -Config $Config -BuildDir $buildDir -Asan:$Asan
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }

# 0 이면 코어 수다.
if ($Jobs -eq 0) { $Jobs = [Environment]::ProcessorCount }
$ctestArgs = @('--test-dir', $buildDir, '--output-on-failure', '-j', $Jobs)
if ($Filter) {
    $ctestArgs += @('-R', $Filter)
}

ctest @ctestArgs
if ($LASTEXITCODE -ne 0) { throw "tests failed ($LASTEXITCODE)" }

# 실행 파일의 기동 계약. 단위 시험이 볼 수 없는 종료 코드와 로그 줄을 본다.
# -Filter 를 준 실행은 단위 시험만 좁혀 보는 것이므로 건너뛴다. -Asan 도 건너뛴다. 그 빌드의 일은
# 단위 시험 안의 메모리 오류 판정이고, 아래 스크립트는 같은 실행 파일의 계약을 이미 일반 빌드로 본다.
if (-not $Filter -and -not $Asan) {
    & (Join-Path $PSScriptRoot 'cli-check.ps1') -Config $Config -BuildDir $buildDir
    if ($LASTEXITCODE -ne 0) { throw "cli check failed ($LASTEXITCODE)" }

    # 프로세스 여럿이 실제로 데이터그램을 주고받는지 본다.
    & (Join-Path $PSScriptRoot 'e2e-check.ps1') -Config $Config -BuildDir $buildDir
    if ($LASTEXITCODE -ne 0) { throw "end-to-end check failed ($LASTEXITCODE)" }

    # 로비와 제어 평면 클라이언트 계약. 진짜 제어 서버를 DynamoDB local 위에 띄운다. 없으면 실패한다.
    & (Join-Path $PSScriptRoot 'lobby-check.ps1') -Config $Config -BuildDir $buildDir
    if ($LASTEXITCODE -ne 0) { throw "lobby check failed ($LASTEXITCODE)" }

    # Phase 4 의 같은 기기 세션 확인. 프로세스 여럿이 방을 만들고 펀치해 CONNECTED 까지 간다. 같은 하네스를 쓴다.
    & (Join-Path $PSScriptRoot 'session-check.ps1') -Config $Config -BuildDir $buildDir
    if ($LASTEXITCODE -ne 0) { throw "session check failed ($LASTEXITCODE)" }
}

Write-Host 'all tests passed'
