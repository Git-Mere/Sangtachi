# 배포 GUI 빌드를 만들고 사용자가 받을 폴더 하나로 모은다 (windows-prereq.md 4절 배포물 패키징).
#
# 서버 주소는 레포에 두지 않는다. 배포 빌드를 만드는 사람이 -Server 로 준다 (ADR 0030).
# Qt 설치 위치도 기계마다 달라 -QtDir 로 받는다. MSVC 64비트 키트의 루트다(bin\windeployqt.exe 가 있는 곳의 부모).
# 이 스크립트가 출력하는 문자열은 ASCII 로만 적는다. 주석은 한국어로 적어도 된다.
#
#   pwsh -File scripts/package-gui.ps1 -Server <이름 또는 IPv4> -QtDir C:\Qt\6.12.0\msvc2022_64 [-Telemetry <이름 또는 IPv4>]
#
# -Telemetry 는 텔레메트리 서비스 주소의 기본값이다(architecture.md 3.5). 주지 않으면 텔레메트리가 꺼진 빌드다.
#
# 산출물은 빌드 루트(buildpath.ps1) 아래 gui-dist 다. 빌드 디렉터리는 그 옆 Release-gui 다.

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Server,
    [Parameter(Mandatory = $true)]
    [string]$QtDir,
    [string]$Telemetry = ''
)

$ErrorActionPreference = 'Stop'

$repo = Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'buildpath.ps1')
. (Join-Path $PSScriptRoot 'vsdevshell.ps1')

$windeployqt = Join-Path $QtDir 'bin\windeployqt.exe'
if (-not (Test-Path -LiteralPath $windeployqt -PathType Leaf)) { throw "windeployqt.exe not found under -QtDir: $windeployqt" }
$qtLicense = Join-Path $QtDir '..\..\Licenses\LICENSE'
if (-not (Test-Path -LiteralPath $qtLicense -PathType Leaf)) { throw "Qt LICENSE not found: $qtLicense" }
$qtVersion = Split-Path (Split-Path $QtDir -Parent) -Leaf

$buildDir = Resolve-BuildDir -Config 'Release-gui'
$outDir = Resolve-BuildDir -Config 'gui-dist'

# -D 값은 각각 한 인자로 넘긴다. PowerShell 은 따옴표 없는 -DX=1.2.3.4 를 첫 '.' 에서 자른다.
# --fresh 로 매번 설정 캐시를 버린다. 남은 캐시의 시험 빌드 장치나 다른 Qt 키트 경로가 배포 빌드에 섞이지 않게 한다.
$configure = @(
    '--fresh',
    '-S', $repo, '-B', $buildDir, '-G', 'Ninja',
    '-DCMAKE_BUILD_TYPE=Release',
    '-DSANGTACHI_BUILD_TESTS=OFF',
    '-DSANGTACHI_TEST_BUILD=OFF',
    '-DSANGTACHI_ASAN=OFF',
    '-DSANGTACHI_BUILD_GUI=ON',
    '-DSANGTACHI_REQUIRE_ADMIN=ON',
    "-DSANGTACHI_DEFAULT_SERVER=$Server",
    "-DSANGTACHI_DEFAULT_TELEMETRY=$Telemetry",
    "-DCMAKE_PREFIX_PATH=$QtDir"
)
& cmake @configure
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed ($LASTEXITCODE)" }
& cmake --build $buildDir --target sangtachi_gui
if ($LASTEXITCODE -ne 0) { throw "cmake build failed ($LASTEXITCODE)" }

# 지난 산출물을 지운다. 이 스크립트가 만든 폴더라는 증거(sangtachi_gui.exe)가 있을 때만 지운다.
if (Test-Path -LiteralPath $outDir) {
    if (-not (Test-Path -LiteralPath (Join-Path $outDir 'sangtachi_gui.exe') -PathType Leaf)) {
        throw "refusing to delete ${outDir}: it does not look like a previous gui-dist (no sangtachi_gui.exe)"
    }
    Remove-Item -LiteralPath $outDir -Recurse -Force
}
New-Item -ItemType Directory -Path $outDir | Out-Null

# 실행 파일과 그 옆에 있어야 하는 파일 셋 (architecture.md 7장 어댑터 수명).
foreach ($name in 'sangtachi_gui.exe', 'wintun.dll', 'Test-SubnetOverlap.ps1', 'Initialize-FirewallRules.ps1') {
    Copy-Item -LiteralPath (Join-Path $buildDir "client\$name") -Destination $outDir
}

# Qt 런타임. Widgets 창에 쓰지 않는 것은 넣지 않는다: 번역, 소프트웨어 OpenGL, D3D/DXC 컴파일러, 네트워크, SVG,
# 그림 형식과 터치 플러그인. VC 런타임은 설치 파일 대신 DLL 을 아래에서 직접 넣는다.
$deploy = @(
    '--release',
    '--no-translations', '--no-opengl-sw', '--no-system-d3d-compiler', '--no-system-dxc-compiler',
    '--no-compiler-runtime', '--no-network', '--no-svg',
    '--skip-plugin-types', 'imageformats,iconengines,generic,networkinformation,tls',
    (Join-Path $outDir 'sangtachi_gui.exe')
)
& $windeployqt @deploy
if ($LASTEXITCODE -ne 0) { throw "windeployqt failed ($LASTEXITCODE)" }

# 폴더 안 PE 파일이 가져오는 DLL 이름. dumpbin 은 개발 셸에 있다.
function Get-Dependents {
    param([string]$Dir)
    $names = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    foreach ($pe in Get-ChildItem -LiteralPath $Dir -Recurse -Include *.exe, *.dll) {
        $lines = & dumpbin /nologo /dependents $pe.FullName
        if ($LASTEXITCODE -ne 0) { throw "dumpbin failed on $($pe.FullName)" }
        foreach ($line in $lines) {
            if ($line -match '^\s+(\S+\.dll)\s*$') { [void]$names.Add($Matches[1]) }
        }
    }
    return $names
}

# VC 런타임 DLL 을 실행 파일 옆에 둔다. 넣는 것은 폴더 안 파일이 실제로 가져오는 것뿐이다.
$crt = Get-ChildItem -LiteralPath (Join-Path $env:VCToolsRedistDir 'x64') -Directory -Filter 'Microsoft.VC*.CRT' | Select-Object -First 1
if ($null -eq $crt) { throw "VC runtime folder not found under $env:VCToolsRedistDir" }
$crtNames = (Get-ChildItem -LiteralPath $crt.FullName -Filter *.dll).Name
do {
    $added = 0
    $needed = Get-Dependents -Dir $outDir
    foreach ($name in $crtNames) {
        if (($needed -contains $name) -and -not (Test-Path -LiteralPath (Join-Path $outDir $name))) {
            Copy-Item -LiteralPath (Join-Path $crt.FullName $name) -Destination $outDir
            $added++
        }
    }
} while ($added -gt 0)

# 닫힘 검사. 가져오는 DLL 이 폴더에 있거나 Windows 에 있어야 한다. VC 런타임과 Qt 는 Windows 에 있다고 보지 않는다.
$system = Join-Path $env:SystemRoot 'System32'
$missing = @()
foreach ($name in (Get-Dependents -Dir $outDir)) {
    if (Test-Path -LiteralPath (Join-Path $outDir $name)) { continue }
    $isRuntime = ($crtNames -contains $name) -or ($name -like 'Qt6*')
    if (-not $isRuntime -and ((Test-Path -LiteralPath (Join-Path $system $name)) -or ($name -like 'api-ms-win-*') -or ($name -like 'ext-ms-*'))) { continue }
    $missing += $name
}
if ($missing.Count -gt 0) { throw ("missing DLLs: " + ($missing -join ', ')) }

# LGPLv3 고지 (windows-prereq.md 4절 Qt 런타임).
Copy-Item -LiteralPath $qtLicense -Destination (Join-Path $outDir 'LICENSE-Qt.txt')
@(
    "This program uses the Qt $qtVersion libraries (Qt6*.dll and the plugins in this folder),",
    'licensed under the GNU Lesser General Public License version 3 (see LICENSE-Qt.txt).',
    'Qt source code: https://download.qt.io/official_releases/qt/',
    'You may replace these Qt DLLs with your own build of the same Qt version.',
    'The Microsoft Visual C++ runtime DLLs in this folder are redistributed under the Visual Studio license terms.',
    '',
    # 수집 항목 안내 (README.md 텔레메트리 절, architecture.md 6.3).
    'Telemetry: if this build has a telemetry service address built in, it sends connection quality records:',
    'connection result and setup time, three NAT observations (true, false, or empty), RTT, jitter, packet loss,',
    'byte counts, session duration, room code, peer id, role, OS, UI kind (GUI or console), build type,',
    'a random value made at each start, and attempt and sequence numbers. It does not send IP addresses, ports,',
    'connection candidates, display names, network information, or device identifiers.'
) | Set-Content -LiteralPath (Join-Path $outDir 'NOTICE.txt') -Encoding ascii

$files = Get-ChildItem -LiteralPath $outDir -Recurse -File
foreach ($f in $files) { '{0,12:N0}  {1}' -f $f.Length, $f.FullName.Substring($outDir.Length + 1) }
'{0} files, {1:N0} bytes' -f $files.Count, ($files | Measure-Object -Property Length -Sum).Sum
"output: $outDir"
