# Visual Studio 개발 셸에 들어간다. 다른 스크립트가 점 소스로 부른다.
# 이 스크립트가 출력하는 문자열은 ASCII 로만 적는다.
# 콘솔 코드 페이지가 UTF-8 이 아니면 한글이 깨져서 오류 메시지를 읽을 수 없다.
# 주석은 파일 안에만 머물므로 한국어로 적어도 된다.
#
# cmake 와 ninja 는 PATH 에 없다. Visual Studio 가 번들로 가지고 있고, 개발 셸이
# 그 경로를 PATH 에 넣는다. 그래서 경로를 이 레포에 박지 않는다.
#
# 통과 조건: 이 스크립트가 끝난 뒤 `cmake --version` 이 동작한다.

$ErrorActionPreference = 'Stop'

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) {
    throw "vswhere.exe not found. Check the Visual Studio installation: $vswhere"
}

# C++ 도구 집합이 있는 인스턴스만 고른다. 도구 집합 없이 IDE 만 깔린 인스턴스를
# 고르면 cl.exe 가 없어 설정 단계에서야 실패한다.
$installPath = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath

if (-not $installPath) {
    throw 'No Visual Studio instance with Microsoft.VisualStudio.Component.VC.Tools.x86.x64 was found.'
}

$devShell = Join-Path $installPath 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll'
if (-not (Test-Path $devShell)) {
    throw "Developer shell module not found: $devShell"
}

Import-Module $devShell
Enter-VsDevShell -VsInstallPath $installPath -SkipAutomaticLocation `
    -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    throw 'Entered the developer shell but cmake is not on PATH. Check the Visual Studio CMake component.'
}
