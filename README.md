# Hamychi

CSP400 Project

## 개요

(프로젝트가 무엇을 하는지, 왜 만드는지 한두 문단으로 작성)

## 스크린샷

(추후 추가)

## 시작하기

### 요구사항

- Windows 10 / 11 x64
- Visual Studio 2022 이상, **C++ 데스크톱 개발 워크로드**. CMake 와 Ninja 가 같이 설치됩니다
- 첫 빌드에는 네트워크가 필요합니다. 시험 프레임워크를 내려받습니다

### 빌드

```powershell
powershell -ExecutionPolicy Bypass -File scripts/build.ps1
```

산출물은 기본으로 `%LOCALAPPDATA%\Hamychi\build\Debug` 에 생깁니다. 레포 안에 두려면
`-BuildDir` 로 경로를 줍니다. 이유는 `scripts/build.ps1` 첫머리에 있습니다.

### 시험

```powershell
powershell -ExecutionPolicy Bypass -File scripts/test.ps1
```

### 실행

```powershell
%LOCALAPPDATA%\Hamychi\build\Debug\client\hamychi_client.exe
```

### 환경 변수

`.env.example`을 `.env`로 복사한 뒤 값을 채웁니다. `.env`는 커밋하지 않습니다.

```bash
cp .env.example .env
```

## 레포 구조

```
client/          C++ 클라이언트. include/ 와 src/
control-server/  제어 평면 (Python). 미착수
telemetry-server/ 텔레메트리 서비스 (Python). 미착수
cmake/           CMake 모듈
tests/           클라이언트 시험
scripts/         빌드와 시험 스크립트
docs/kor/        스펙, 계획, 설계 결정, 커밋 기록 (한국어)
docs/eng/        같은 문서의 영어판
tools/           문서 게이트, 사전 조건 검사, NAT 실측
```

## 라이선스

MIT. [LICENSE](LICENSE) 참고.
