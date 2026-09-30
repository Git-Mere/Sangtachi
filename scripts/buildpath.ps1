# 빌드 산출물 디렉터리를 정한다. build.ps1, test.ps1, cli-check.ps1, e2e-check.ps1 이
# 같은 규칙을 쓰도록 한 자리에 둔다.
#
# 우선순위는 셋이다.
#   1. -BuildDir 로 준 값
#   2. scripts/build-root.local 의 첫 유효 줄 + \<Config>. 절대 경로여야 한다
#   3. %LOCALAPPDATA%\Sangtachi\build\<Config>
#
# 2번을 파일로 두는 이유는 기계마다 다른 절대 경로를 레포에 박지 않기 위해서다.
# 그 파일은 .gitignore 가 막는다. 환경 변수로 하지 않는 이유는 셸마다 값이 달라
# 같은 레포가 셸에 따라 다른 곳에 빌드되기 때문이다.
#
# 산출물을 레포 밖에 두는 근거는 build.ps1 머리에 있다.
# 이 파일이 출력하는 문자열은 없다. 판정만 한다.

$BuildPathScriptRoot = $PSScriptRoot

# 파일에서 빌드 루트를 읽는다. 없거나 유효한 줄이 없으면 빈 문자열이다.
#
# 기본값은 "모르면 값 없음" 이다. 읽을 수 없는 파일을 경로로 해석하면 엉뚱한
# 디렉터리에 빌드하거나 -Fresh 가 그것을 지우려 든다.
function Get-BuildRootFromFile {
    param([string]$Path)

    if ([string]::IsNullOrWhiteSpace($Path)) { return '' }
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return '' }

    foreach ($line in (Get-Content -LiteralPath $Path)) {
        $t = $line.Trim().Trim('"').Trim("'").Trim()
        if ($t -eq '') { continue }
        if ($t.StartsWith('#')) { continue }
        return $t
    }
    return ''
}

function Resolve-BuildDir {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Config,
        [string]$Explicit = '',
        [string]$RootFile = ''
    )

    if (-not [string]::IsNullOrWhiteSpace($Explicit)) { return $Explicit }

    if ([string]::IsNullOrWhiteSpace($RootFile)) {
        $RootFile = Join-Path $BuildPathScriptRoot 'build-root.local'
    }

    $root = Get-BuildRootFromFile -Path $RootFile
    if ($root -ne '') {
        # 상대 경로를 받지 않는다. 받으면 부르는 쪽의 현재 디렉터리에 따라 빌드 위치가
        # 달라지고, 그 자리에 한 번 빌드하고 나면 CMakeCache.txt 가 생겨 -Fresh 의
        # 안전장치도 통과한다. 즉 오타 하나가 엉뚱한 디렉터리를 지우는 경로가 된다.
        # 판정은 허용 목록이다. IsPathRooted 는 이름과 달리 '\noroot' 에도 참이다.
        # 그것은 현재 드라이브에 붙는 드라이브 상대 경로이므로 여기서는 절대가 아니다.
        # 받는 것은 드라이브 문자로 시작하는 경로와 UNC 둘뿐이다. UNC 는 서버와 공유
        # 이름을 모두 요구한다. '\\\\server' 만 받으면 Join-Path 가 '\\\\server\\Debug' 를
        # 만들어 공유 이름 자리에 빌드 구성 이름이 들어간다.
        $absolute = ($root -match '^[A-Za-z]:[\\/]') -or ($root -match '^\\\\[^\\/]+[\\/][^\\/]+')
        if (-not $absolute) {
            throw "build root must be a fully qualified path, got '$root' from $RootFile"
        }
        return (Join-Path $root $Config)
    }

    return (Join-Path $env:LOCALAPPDATA "Sangtachi\build\$Config")
}
