# Sangtachi

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

산출물은 **레포 밖**에 생깁니다. 위치는 세 단계로 정해집니다.

| 순위 | 무엇 | 예 |
|:--:|------|-----|
| 1 | `-BuildDir` 로 준 값 | 한 번만 다른 곳에 두고 싶을 때 |
| 2 | `scripts/build-root.local` 의 첫 유효 줄 + `\<Config>` | `C:\me\build` 라고 적으면 `C:\me\build\Debug` |
| — | 그 줄은 **드라이브 문자로 시작하는 경로나 UNC** 여야 합니다. UNC 는 서버와 공유 이름을 모두 적습니다 | `..\build`, `\build`, `\\server` 는 거부됩니다 |
| 3 | 없으면 `%LOCALAPPDATA%\Sangtachi\build\<Config>` | 기본 |

`build-root.local` 은 기계마다 경로가 달라서 커밋하지 않습니다 (`.gitignore`).
레포 밖에 두는 이유는 `scripts/build.ps1` 첫머리에 있습니다.

### 시험

```powershell
powershell -ExecutionPolicy Bypass -File scripts/test.ps1
```

- 단위 시험(`ctest`)은 코어 수만큼 동시에 돈다. `-Jobs 1` 이면 하나씩 돈다
- 필터 없이 돌리면 단위 시험 뒤에 `cli-check.ps1`, `e2e-check.ps1`, `lobby-check.ps1` 이 이어 돈다
- `lobby-check.ps1` 은 제어 서버 하네스(`control-server/tests/harness/`)를 띄운다. DynamoDB local
  컨테이너와 `control-server/.venv` 가 있어야 한다. 띄우는 명령은 `control-server/README.md` 에 있다.
  없으면 건너뛰지 않고 실패한다

### 실행

```powershell
<빌드 경로>\Debug\client\sangtachi_client.exe
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
