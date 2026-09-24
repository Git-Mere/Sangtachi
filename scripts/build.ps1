# 클라이언트와 시험 실행 파일을 빌드한다.
#
# 빌드 산출물은 기본으로 레포 밖(%LOCALAPPDATA%\Sangtachi\build)에 둔다.
# 레포가 파일 동기화 폴더 안에 있으면 갓 만든 .exe 와 .pdb 가 잠겨 링크가 실패한다
# (LNK1168, LNK1201, C1041). 같은 빌드를 두 위치에서 18회씩 돌려 레포 안 4건 실패,
# 레포 밖 0건을 봤다. 잠그는 프로세스가 무엇인지는 확인하지 않았다.
# 레포 안에 두려면 -BuildDir 로 경로를 준다.
# 이 스크립트가 출력하는 문자열은 ASCII 로만 적는다.
# 콘솔 코드 페이지가 UTF-8 이 아니면 한글이 깨져서 오류 메시지를 읽을 수 없다.
# 주석은 파일 안에만 머물므로 한국어로 적어도 된다.
#
#   pwsh -File scripts/build.ps1                 Debug 빌드
#   pwsh -File scripts/build.ps1 -Config Release
#   pwsh -File scripts/build.ps1 -Fresh          설정 캐시를 버리고 다시 설정한다

[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')]
    [string]$Config = 'Debug',
    [string]$BuildDir = '',
    [switch]$Fresh,
    [switch]$SelfTest
)

$ErrorActionPreference = 'Stop'

# -Fresh 의 판정. 이것이 참일 때만 재귀 삭제한다.
#
# 기본값은 "모르면 지우지 않는다" 다. -BuildDir 는 사용자가 주는 값이라 오타 하나가
# 남의 디렉터리를 지우는 쪽이고, 그래서 경로 접두사 같은 배제 목록이 아니라 증거를
# 요구하는 허용 목록으로 판정한다. CMakeCache.txt 는 CMake 가 설정할 때만 만든다.
function Test-IsCmakeBuildDir {
    param([string]$Path)

    if ([string]::IsNullOrWhiteSpace($Path)) { return $false }
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) { return $false }

    $marker = Join-Path $Path 'CMakeCache.txt'
    return [bool](Test-Path -LiteralPath $marker -PathType Leaf)
}

if ($SelfTest) {
    # 케이스 표. 판정 함수 하나를 돈다.
    $tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("sangtachi_selftest_" + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null
    try {
        $emptyDir = Join-Path $tmp 'no_cache';       New-Item -ItemType Directory -Force -Path $emptyDir | Out-Null
        $goodDir  = Join-Path $tmp 'real_build';     New-Item -ItemType Directory -Force -Path $goodDir  | Out-Null
        New-Item -ItemType File -Force -Path (Join-Path $goodDir 'CMakeCache.txt') | Out-Null
        $dirMarker = Join-Path $tmp 'cache_is_dir';  New-Item -ItemType Directory -Force -Path $dirMarker | Out-Null
        New-Item -ItemType Directory -Force -Path (Join-Path $dirMarker 'CMakeCache.txt') | Out-Null
        $plainFile = Join-Path $tmp 'a_file.txt';    New-Item -ItemType File -Force -Path $plainFile | Out-Null

        $cases = @(
            @{ name = 'empty string';                 path = '';                                 expect = $false }
            @{ name = 'whitespace only';              path = '   ';                              expect = $false }
            @{ name = 'nonexistent path';             path = (Join-Path $tmp 'does_not_exist');  expect = $false }
            @{ name = 'directory without the marker'; path = $emptyDir;                          expect = $false }
            @{ name = 'marker is a directory';        path = $dirMarker;                         expect = $false }
            @{ name = 'a file, not a directory';      path = $plainFile;                         expect = $false }
            @{ name = 'real cmake build directory';   path = $goodDir;                           expect = $true  }
        )

        $failed = 0
        foreach ($c in $cases) {
            $got = Test-IsCmakeBuildDir -Path $c.path
            if ($got -ne $c.expect) {
                Write-Host ("FAIL  {0}: expected {1}, got {2}" -f $c.name, $c.expect, $got)
                $failed++
            } else {
                Write-Host ("ok    {0}" -f $c.name)
            }
        }
        Write-Host ("{0} passed, {1} failed of {2}" -f ($cases.Count - $failed), $failed, $cases.Count)
        if ($failed -gt 0) { exit 1 }
        exit 0
    } finally {
        Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
    }
}

$repoRoot = Split-Path -Parent $PSScriptRoot
if ($BuildDir) {
    $buildDir = $BuildDir
} else {
    $buildDir = Join-Path $env:LOCALAPPDATA "Sangtachi\build\$Config"
}

. (Join-Path $PSScriptRoot 'vsdevshell.ps1')

if ($Fresh -and (Test-Path -LiteralPath $buildDir)) {
    if (-not (Test-IsCmakeBuildDir -Path $buildDir)) {
        throw "refusing to delete '$buildDir': no CMakeCache.txt, so this is not a CMake build directory. Remove it by hand if that is what you want."
    }
    # -LiteralPath 로 지운다. 검증은 리터럴로 하고 삭제는 와일드카드를 해석하면
    # 경로에 [ ] * ? 가 든 순간 검증한 것과 지우는 것이 달라진다.
    Remove-Item -LiteralPath $buildDir -Recurse -Force
}

cmake -S $repoRoot -B $buildDir -G Ninja "-DCMAKE_BUILD_TYPE=$Config"
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed ($LASTEXITCODE)" }

cmake --build $buildDir
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }

Write-Host "build ok: $buildDir"
