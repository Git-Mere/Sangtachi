# GUI 로그오프의 정리 시간을 로그에 남긴다

## 왜 했나

`roadmap.md` Phase 8 작업 "호스트 종료가 OS 유예 안에 드는지 잰다" 의 GUI 쪽을 `mere-galaxy` 를 플레이어로 두고 쟀다. 플레이어가
`CLOSE` 를 받은 때부터 GUI 로그의 마지막 쓰기까지 약 0.11초였다. 그러나 OS 유예는 `WM_ENDSESSION` 을 받은 때부터 센다. 그 앞부분은
로그에 시각이 없어 재지 못했다. Codex 리뷰가 두 번 blocker 로 그 점을 짚었고, 저장소 소유자가 제대로 재는 안을 골랐다. 그 측정의
기록은 다음 커밋이다.

## 무엇을 바꿨나

| 무엇 | 자리 |
|------|------|
| `end_session` 이 받은 시각을 들고, 정리 완료(`cleanup_done`) 때 `session_end.cleanup elapsed_ms=<밀리초>` 한 줄을 낸다. 창 닫기로 끝날 때는 내지 않는다 | `client/src/ui/gui_ui.cpp`, `client/include/sangtachi/ui/gui_ui.hpp` |
| 시계를 생성자로 받는다. 기본은 `platform::monotonic_ms` 다 | 같은 곳 |
| 고정 키 표에 `session_end.cleanup` 을, 형식 표에 `elapsed_ms` 를 더했다 | `architecture.md` 9장 로그 출력 |
| 고정 키 수를 "열여덟" 에서 "스무" 로 고쳤다. 표는 이미 열아홉 행이었다 | 같은 곳 |

- 줄은 작업 스레드가 낸다. `cleanup_done` 은 `adapter.close` 뒤, join 앞이다. 그래서 join 상한의 `_exit` 로 끝나도 줄이 남는다. `[gui]` 가 기다림에서 돌아온 뒤 내면 그 경우 줄을 잃는다
- 잰 구간은 `[gui]` 가 `WM_ENDSESSION` 을 받아 `end_session` 에 든 때부터 작업 스레드가 `cleanup_done` 에 든 때(`adapter.close` 뒤)까지다. 그 뒤의 이 줄 쓰기, 화면 큐 넣기, 정리 완료 신호는 들지 않는다. 그래서 `[gui]` 가 기다리는 구간 전체가 아니다. 빠진 몫은 파일 쓰기 한 번과 큐 넣기다

## 검증

| 무엇 | 결과 |
|------|------|
| 새 시험 둘 | `end_session` 뒤 정리면 `INFO session_end.cleanup elapsed_ms=150`(가짜 시계 1000 에서 1150). 창 닫기 뒤 정리면 그 줄이 없다 |
| 변이 셋 | 조건 없이 늘 낸다, 빼기를 지운다, `end_session` 이 표시를 세우지 않는다. 셋 다 새 시험이 실패한다 |
| `scripts/test.ps1` | 통과 |
| `docgate.py`, `--claims` | `VERDICT: pass` |

`-Full` 은 돌리지 않았다. 바꾼 것은 GUI 화면 쪽(`ui/`)이고 `CLAUDE.md` 가 `-Full` 을 요구하는 경로가 아니다.

## 크로스 모델 리뷰

Codex(`gpt-6.1-sol`). 정확성·동시성(A)과 문서·기록(B) 렌즈를 한 번에 돌렸다.

| 지적 | 처리 |
|------|------|
| warn [A] `elapsed_ms` 는 줄 쓰기와 `publish_end` 앞에서 끝나는데 문서는 정리 완료 신호까지라고 적는다. 느린 파일 쓰기가 신호를 유예 밖으로 밀어도 값은 작게 나온다 | 반영. 끝점을 "정리 완료 신호를 준비하기 시작한 때" 로 고치고 빠진 몫을 적었다. 신호 뒤에 줄을 쓰면 `[gui]` 가 돌아가 프로세스가 끝나며 줄을 잃을 수 있어 자리는 그대로 둔다 |

고친 뒤 전체 staged 차이를 같은 렌즈로 다시 돌렸다.
