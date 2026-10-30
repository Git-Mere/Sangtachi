# Phase 8 착수 전 결정과 최소 GUI

## 왜 했나

Phase 8 을 시작하면서 `roadmap.md` 가 착수 전에 정하라고 한 다섯(GUI 스레드 모델, 기동 입력 경로, 속도 제한과의 관계, 권한
승격과 잔존물 정리 시점, Qt 배포 형태)과 문서를 읽다 나온 빈칸을 저장소 소유자와 정했다. 결정은
[ADR 0034](../../decisions/0034-phase-8-착수-전-결정.md) 다. 그 결정대로 Qt Widgets GUI 를 만들었다.

2026-10-28 에 소유자가 친구와 `field-kit-p7b` 로 Minecraft 세션을 한 번 돌렸다. 그 결과를 새 `experiments.md` 3.1 에 실었다.

## 무엇을 바꿨나

| 무엇 | 자리 |
|------|------|
| 착수 전 결정과 버린 대안 | ADR 0034 |
| GUI 빌드의 스레드(`[gui]` 가 주 스레드, `[loop]` 는 작업 스레드), 화면 큐, GUI 빌드의 종료 | `concurrency.md` 1장·2장·6장·7장 |
| GUI 실행 파일의 기동 입력, 콘솔 명령과 창의 대응, 멤버 목록, 모듈 표와 레포 트리 | `architecture.md` 3.1·3.5·10장 |
| GUI 는 창 쪽에서 따로 막지 않는다 | `control_plane.md` 6.4 |
| GUI 실행 파일의 매니페스트, Qt 런타임(배포물, 만드는 법, 크기), Minecraft 접속의 부분 실측 | `windows-prereq.md` 1절·4절·5절·14절 |
| 착수 전 항목을 ADR 0034 로, nat-probe 재측정을 클라이언트 리허설로, 매핑 수명, 3초 측정 두 빌드, Wi-Fi 끊김 기록, 블록·채팅 기준, Phase 9 텔레메트리의 집계·그래프 | `roadmap.md` Phase 8·9 |
| 실측 결과 문서를 지금 만든다. 어댑터 MTU 와 Minecraft 첫 세션 | `experiments.md` |
| `experiments.md` 끊긴 링크 예외를 지웠다 | `tools/docgate/`, `CLAUDE.md` 의 낡은 사실 |
| Phase 8 실측 목록 | `field-test/required-test.md` |
| 기동부터 정리까지를 `run_client_process(Args, ClientUi&, 종료 이벤트)` 로 뺐다. 콘솔과 GUI 실행 파일이 각자 컴파일한다 | `client/src/client_process.cpp`, `main.cpp` |
| 플레이어의 마지막 명부, 호스트 명부 접근자, 로비의 화면용 접근자와 마지막 실패 | `peer/session`, `tunnel/roster_publisher`, `control/lobby` |
| 화면 상태와 화면 큐, 표시 로직, GUI 쪽 `ClientUi`, `WM_ENDSESSION` 판정 | `ui/view`, `ui/gui_ui`, `platform/session_end` (Qt 없음) |
| Qt 창과 GUI 실행 파일. `SANGTACHI_BUILD_GUI`(기본 꺼짐) | `ui/main_window`, `ui/gui_main.cpp`, CMake |
| 배포 GUI 빌드와 배포물 모으기, 닫힘 검사, LGPL 고지 | `scripts/package-gui.ps1` |

## 중요한 결정

- 화면 큐는 사건이 아니라 창에 보일 상태 전체를 나른다. 가득 차도 마지막 상태는 버리지 않는다
- `[loop]` 는 `[gui]` 를 깨우지 않는다. Qt 의 큐 호출은 이벤트 큐 잠금을 잡아 `[loop]` 에 락을 들인다. `[gui]` 가 100ms 마다 읽는다
- 배포물은 창에 쓰지 않는 Qt 구성요소를 빼서 58개 약 94MiB 에서 16개 30,155,983바이트가 됐다. VC++ 런타임은 DLL 로 넣는다
- 문서에 없던 값과 동작 10개를 소유자에게 보이고 그대로 두었다. 화면 큐 4칸 중 2칸 예약, 들고 있으면 100ms 뒤 한 바퀴, `ERROR`
  줄 512바이트, 준비 전 버튼 꺼짐, 마지막 `ERROR` 줄, 준비 중 닫아도 기동 실패면 대화 상자, 다른 거부 안내 숨김, 공백을 지우지
  않음, 검사 실패 문구는 콘솔과 같게, 창 글자는 영어

## 검증

| 무엇 | 결과 |
|------|------|
| `scripts/test.ps1` | ctest 661/661, cli-check 43/43, e2e·lobby·session 실패 0. `/W4 /WX` 경고 0 |
| 변이 | 화면 큐·표시 로직 18개, GUI 쪽 15개 모두 시험이 잡았다 |
| `tools/platformgate` | `VERDICT: pass` |
| GUI 스모크(관리자 아님) | 창이 뜨고 `not_elevated` 기동 실패 대화 상자, 종료 코드 2. 줄인 배포물로도 같다 |
| `scripts/package-gui.ps1` | 닫힘 검사 통과. 서버 주소가 실행 파일에 들어간 것을 확인했다 |
| `docgate.py` | `VERDICT: pass` |

**확인하지 못한 것.** 관리자 권한과 서버가 필요한 GUI 경로(준비 완료, 방 만들기와 참가, 멤버 표, 닫기, 스스로 끝남, 로그오프),
콘솔 창에서의 입력 자리 표시와 안내, Qt 와 VC++ 런타임이 없는 PC. `plan.md` 에 옮겼다.

## 크로스 모델 리뷰

Codex. 1차는 세 렌즈(스레드와 종료, 콘솔 동작 보존과 표시 로직, 문서 일관성)로 따로 돌렸다.

| 렌즈 | 지적 | 처리 |
|------|------|------|
| 스레드, 동작 (둘 다) | blocker. 첫 화면 상태를 루프 한 바퀴 뒤에야 넘겨, 로비에 타이머가 없으면 GUI 가 `Loading...` 에서 멈춘다 | 반영. 루프에 들어가기 전에 첫 상태를 넘긴다. `concurrency.md` 6장에 적었다. 자동 시험은 없다. 그 코드가 시험 실행 파일 밖에 있어 소유자 확인에 넘겼다 |
| 문서 | warn. `concurrency.md` 가 제어 서버 이름 해석을 작업 스레드가 한다고 적었다. 해석은 `[control]` 이 한다 | 반영(한·영) |
| 문서 | warn. 버튼이 켜지는 규칙이 `control_plane.md` 6.4 와 `architecture.md` 3.5 에 겹쳤다 | 반영. 6.4 는 3.5 를 가리킨다 |
| 문서 | warn. `plan.md` 인수인계가 낡았다 | 반영 |

2차는 바뀐 부분(패키징 스크립트, 첫 화면 상태 수정)만 돌렸다.

| 렌즈 | 지적 | 처리 |
|------|------|------|
| 패키징 | warn. 빌드 디렉터리를 다시 쓰면 설정 캐시에 남은 `SANGTACHI_TEST_BUILD=ON` 이 배포 빌드에 섞일 수 있다 | 반영. 설정할 때 `SANGTACHI_TEST_BUILD` 와 `SANGTACHI_ASAN` 을 끈다 |
| 패키징 | warn. `-QtDir` 를 바꿔도 캐시의 Qt 경로로 링크될 수 있다 | 반영. 매번 `--fresh` 로 설정한다 |

영어 미러를 맞추면서 한국어 결함을 더 찾아 고쳤다. `windows-prereq.md` 0절의 검증 상태 개수, 0절과 14절의 "Qt 동봉은 아직" 문장,
ADR 0034 "확인하지 못한 것" 의 배포물 크기 항목이다.

3차는 커밋할 전체 staged diff 에 한 번 더 돌렸다. `LGTM - no blockers` 였다.
