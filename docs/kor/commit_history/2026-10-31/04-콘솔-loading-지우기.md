# 콘솔의 `Loading...` 을 준비가 끝나면 지운다

## 왜 했나

저장소 소유자가 콘솔에서 본 것이다. 기동 안내 `Loading...` 이 준비가 끝나고 로비 안내가 나온 뒤에도 화면 위에 남아 있었다.
한 줄로 내고 줄을 바꿨기 때문이다.

## 무엇을 바꿨나

| 무엇 | 자리 |
|------|------|
| `Loading...` 은 줄을 바꾸지 않는다. 그 자리에 다음 줄이나 입력 자리가 나오기 전에 지운다. 띄우는 때를 인자 검사 직후에서 로그 파일을 연 직후로 옮겼다 | `architecture.md` 3.5 로비 안내 |
| 화면 쪽에 `loading` 을 더했다. 로그 받는 곳을 단 직후에 부른다. GUI 는 아무것도 하지 않는다 | `client_process.hpp`, `client_process.cpp`, `gui_ui.hpp` |
| 입력 자리 표시에 `status`, `clear_status` 를 더했다. `line`, `show`, `close` 가 먼저 지운다 | `console.hpp`, `console.cpp` |
| 콘솔 화면이 `Loading...` 을 `status` 로 낸다. 입력 자리 표시를 `shared_ptr` 로 들어 로그 화면 함수와 나눈다. 표준 오류의 기동 실패 줄, 실행 파일 옆 파일 안내, 기동 실패의 창 안내도 먼저 지운다 | `main.cpp` |

- 지우는 것은 `\r`, 글자 수만큼의 공백, `\r` 이다. 커서 이동 같은 터미널 제어 문자를 쓰지 않는다
- `clear_status` 만 다른 스레드에서 불러도 된다. 표준 오류의 로그 화면이 부르기 때문이다. 지우는 쪽이 하나가 되게 원자 교환으로 꺼낸다
- 표준 출력이 리다이렉트되면 전처럼 `Loading...` 을 내지 않는다

## 검증

| 무엇 | 결과 |
|------|------|
| `console_test` 에 한 케이스 | `status` 뒤 첫 출력이 한 번만 지운다 |
| `mere-galaxy` 의 가상 터미널(`script`)에서 실제 바이트 | `Loading...\r          \r` 뒤에 로비 안내가 나온다. 권한 없이 어댑터를 쓰게 띄우면 `Loading...\r          \rERROR adapter.error reason=not_elevated` 다 |
| Windows `scripts/test.ps1` | `all tests passed`. ctest 665건 |
| `docgate.py`, `--claims` | `VERDICT: pass` |

## 크로스 모델 리뷰

Codex(`gpt-6.1-sol`). 정확성·스레드 렌즈와 문서 일관성 렌즈를 한 번에 돌렸다. blocker 하나, warn 둘. 고친 차이만 회귀 렌즈로 다시 돌렸고 LGTM 이었다.

| 지적 | 처리 |
|------|------|
| blocker: 로그 받는 곳은 프로세스 수명인데 화면 함수가 스택의 콘솔 화면 객체를 붙든다. `main` 뒤의 로그가 해제된 것을 건드린다 | 반영. `ClientUi::log_screen` 의 계약("이 객체를 붙들지 않는다")을 어긴 것이다. 입력 자리 표시를 `shared_ptr` 로 들고 화면 함수가 그 사본을 갖는다 |
| warn: 기록 파일과 로그 파일을 열지 못한 `ERROR` 는 받는 곳을 달기 전이라 `Loading...` 뒤에 그대로 붙는다 | 반영. `Loading...` 을 로그 파일을 연 직후에 띄운다. 그 앞의 실패는 `Loading...` 이 뜨기 전이다 |
| warn: 원자 교환이 지우기를 쓰기 전에 가져가서, 다른 스레드의 `ERROR` 가 지우기보다 먼저 나올 수 있다 | 기각. 기동 중의 `ERROR` 는 화면 쪽과 같은 스레드에서 나온다. 다른 스레드(`[control]`)가 그 짧은 틈에 `ERROR` 를 낼 때만 겹치고, 그때도 화면 모양만 어긋난다. 모든 출력을 잠금으로 묶는 비용이 더 크다 |
