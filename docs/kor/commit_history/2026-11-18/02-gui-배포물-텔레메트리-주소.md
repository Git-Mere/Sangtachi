# GUI 배포물에 텔레메트리 주소 넣기

## 왜 했나

[`architecture.md`](../../architecture.md) 3.5 기동 입력은 배포 빌드를 만드는 사람이 제어 서버 주소와 텔레메트리 서비스 주소를 둘 다 넣는다고 정한다. `scripts/package-gui.ps1` 은 서버 주소만 받았다. 그래서 GUI 배포물은 늘 텔레메트리가 꺼진 빌드였다. 소유자가 텔레메트리를 켠 GUI 배포물을 요청했다.

## 무엇을 바꿨나

| 무엇 | 자리 |
|------|------|
| 선택 인자 `-Telemetry`. `SANGTACHI_DEFAULT_TELEMETRY` 로 넘긴다. 주지 않으면 빈 값이라 지금처럼 텔레메트리가 꺼진 빌드다 | `scripts/package-gui.ps1` |
| 만드는 법의 명령과 `-Telemetry` 설명 | `windows-prereq.md` 4절 |

이 PC 의 `latest_build` 를 채우는 레포 밖 스크립트도 이 인자를 넘기게 고쳤다. 레포 밖이라 이 커밋에 들지 않는다.

## 검증

| 무엇 | 결과 |
|------|------|
| `docgate.py` | `VERDICT: pass` |
| `package-gui.ps1 -Telemetry <Elastic IP>` 로 배포물을 만들었다 | 생성 헤더의 `kDefaultTelemetry` 가 그 주소다. `gui_main.cpp` 가 다시 컴파일됐다. 16개 파일, 닫힘 검사 통과. GUI 는 관리자 권한이 필요해 띄워 보지 않았다 |

## 크로스 모델 리뷰

Codex(`gpt-6.1-sol`). 스크립트 정확성(A), 한영 일치(B), 문서와 스크립트의 일치(C), 공개 레포 노출(D) 렌즈. 첫 번에 `LGTM - no blockers`.
