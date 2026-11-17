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
- 필터 없이 돌리면 단위 시험 뒤에 `cli-check.ps1`, `e2e-check.ps1` 이 이어 돈다
- `-Full` 을 주면 그 뒤에 `lobby-check.ps1`, `session-check.ps1` 의 대표 사례까지 돈다
- `-Slow` 는 `-Full` 을 포함해 두 스크립트의 나머지 사례와 RTT 기준선 20표본까지 돈다. 실행 시점은 `CLAUDE.md` 에 있다
- `-Full`, `-Slow` 는 `-Filter`, `-Asan` 과 함께 쓰지 않는다
- `lobby-check.ps1` 과 `session-check.ps1` 은 제어 서버 하네스(`control-server/tests/harness/`)를 띄운다. DynamoDB local
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

## 텔레메트리

클라이언트는 연결 품질 기록을 텔레메트리 서비스에 보낼 수 있습니다. 서비스 주소는 `--telemetry` 인자나 빌드 기본값으로 줍니다.
둘 다 없으면 보내지 않습니다. 동의 창은 없습니다. 무엇을 보내는지가 이 절입니다. 필드 정의는
[`docs/kor/architecture.md`](docs/kor/architecture.md) 6.3 텔레메트리 서비스 인터페이스에 있습니다.

**보내는 것.**

- 연결 시도마다: 결과(성공 또는 실패 코드), 수립 시간, NAT 관측값 셋(로컬 주소와 공인 주소가 다른가, 두 STUN 서버가 같은
  엔드포인트를 봤는가, 상대와 같은 공인 IP 인가). 셋 다 참/거짓 값이고, 관측하기 전에 끝난 시도에서는 값 없음(`null`)입니다. 주소 자체는 싣지 않습니다
- 연결된 동안 60초마다와 끝날 때: RTT 표본, 지터, 패킷 손실, 주고받은 바이트 수, 세션 지속 시간
- 묶음 키: 실행할 때마다 새로 만드는 무작위 값, 시도 번호, 전송 순번
- 방 코드, 제어 평면이 준 `peer_id`, 역할(호스트 또는 플레이어)
- OS(Windows 또는 Linux), GUI 인지 콘솔인지, 시험 빌드인지

**보내지 않는 것.** IP 주소와 포트, 연결 후보, 표시 이름, 망 정보, 기기 식별자. 텔레메트리 서비스는 로그에 출발지 IP 를 남기지
않습니다.

## 레포 구조

```
client/          C++ 클라이언트. include/ 와 src/
control-server/  제어 평면 (Python)
telemetry-server/ 텔레메트리 서비스 (Python)
cmake/           CMake 모듈
tests/           클라이언트 시험
scripts/         빌드와 시험 스크립트
docs/kor/        스펙, 계획, 설계 결정, 커밋 기록 (한국어)
docs/eng/        같은 문서의 영어판
tools/           문서 게이트, 사전 조건 검사, NAT 실측, 배포, 텔레메트리 분석
```

## 라이선스

MIT. [LICENSE](LICENSE) 참고.
