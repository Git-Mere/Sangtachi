# Phase 4 4차 J1: 로컬 기록 파일

## 왜 했나

Phase 4 4차 J 의 첫 조각이다. `spec.md` M-6 의 입력인 로컬 기록 파일을 연다. 계약은 `architecture.md` 9장
로컬 기록 파일(경로, 줄 형식, 시도 번호, 쓰기와 실패)과 ADR 0016 이 정했다. 줄을 실제로 쓰는 호출(수립, 세션
종료, 종료 절차의 (3))은 랑데부·로비 연결과 같이 붙는다. 이 조각은 파일을 열고 쓰는 도구까지다.

## 무엇을 바꿨나

| 무엇 | 자리 |
|------|------|
| 줄 하나의 모양, `ts` 꼴, 파일 이름, 시도 번호, 쓰기 실패 뒤 정지. 쓰기와 벽시계는 주입받는다 | `client/include/sangtachi/record.hpp`, `client/src/record.cpp` |
| 실행 파일 폴더 아래 `records` 폴더와 새 파일(`CREATE_NEW`), 쓰기 호출 하나로 줄 하나, 일부 쓰기 = 실패, UTC 벽시계, PID | `platform/record_file.hpp`, `client/src/platform/win32/record_file.cpp` |
| 카운터 `record_write_failed` (프로세스 표) | `counters.hpp`, `counters.cpp`. `docgate.py` 의 `PENDING_COUNTERS` 에서 뺐다 |
| 인자 검사 뒤에 열고 `record.open` 한 줄. 열지 못하면 `ERROR` 한 줄과 종료 코드 2. `run_client` 가 `RecordWriter` 를 받는다(아직 쓰지 않는다) | `client/src/main.cpp` |
| 기동 계약 검사. 정상 기동의 파일과 이름의 UTC 시각·PID, 동시에 띄운 두 프로세스, 인자 오류에서 파일 없음, `icacls` 로 폴더·파일 만들기를 막은 기동 실패 | `scripts/cli-check.ps1` |
| 단위 시험 | `tests/record_test.cpp`, `tests/platform/record_file_test.cpp`, `tests/counters_test.cpp` |
| `record`, `platform/record_file` 모듈 행과 트리. 3.5 의 낡은 "경로는 9장 계약이 정해지면 정한다" 문장을 9장을 가리키게 고쳤다 | `architecture.md` 3.1, 3.5, 10장. 영어 미러 |

## 중요한 결정

**문서가 정하지 않은 작은 것은 코드가 정했다.** `record.hpp` 와 `platform/record_file.hpp` 의 "이 코드가 정한 것" 표에
근거와 함께 있다.

- 실패한 쓰기 한 번에 `record_write_failed` 1 과 `WARN record.write_failed kind=<종류> attempt=<번호>` 한 줄이다. 그 뒤
  넘긴 줄은 쓰기 시도가 아니므로 세지 않는다. 9장 "판정할 수 있는 실행" 은 그 값이 0 인지만 본다
- 실패 뒤에도 시도 번호는 오른다. 벽시계는 줄을 쓰기 직전에 한 번 읽고 실패 뒤에는 읽지 않는다
- 실행 파일 경로는 32768 와이드 문자 버퍼로 한 번 얻고, 다 채우면 잘린 것으로 보고 실패다
- 파일은 `FILE_APPEND_DATA` 하나와 `FILE_SHARE_READ` 로 연다. 실행 중에 읽을 수 있다
- 열기 실패 줄은 main 의 기존 기동 실패 관례 `ERROR socket.error op=<호출> code=<오류>` 다
- 11장 외부 의존성에 행을 더하지 않았다. 새 호출은 전부 kernel32 이고, 11장은 별도 라이브러리만 적는다

**일부 쓰기 시험의 한계.** 자리가 모자란 논블로킹 파이프는 이 기기에서 `0 < n < size` 가 아니라 성공과 0 바이트를
돌려줬다. 시험은 "성공을 돌려주며 다 쓰지 못한 쓰기" 를 보고, 그 전제를 같은 케이스 안에서 원시 `WriteFile` 로 먼저
확인한다. 판정(`written == size`)은 두 경우를 가르지 않는다.

**전체 시험이 찾은 결함 하나.** 인자 오류 검사가 처음에는 `records\*-<PID>.log` 가 있는지만 봤다. 앞선 실행의
파일이 빌드 폴더에 쌓이고(9장: 지우는 것은 사람이다) PID 는 다시 쓰이므로, 전체 시험에서 옛 파일의 PID 와 겹쳐 거짓 실패가
났다. 띄우기 전에 없던 파일만 보고, 실패가 인자 검사의 `ERROR args.invalid` 이고 열기 실패 줄이 없는지도 보도록 고쳤다. 그리고 인자 검사 전에 여는 변이가 여전히 잡히는 것을 확인했다.

**관찰.** 같은 PID 의 두 프로세스가 같은 초에 뜨면 `CREATE_NEW` 가 거절해 기동 실패다. 9장이 정한 동작이다. PID 를 0 으로
바꾼 변이에서 실제로 일어났다. 실제 PID 로는 같은 초 안의 재사용이 있어야 한다.

## 검증

| 무엇 | 결과 |
|------|------|
| 전체 시험 (`scripts/test.ps1`) | 단위 시험 전부 통과, cli/e2e/lobby 검사 실패 0, `all tests passed`. DynamoDB local 위에서 돌렸다 |
| 변이 (깨끗한 사본) | 14종 모두 잡혔다. 단위: 실패 뒤 정지 제거, 일부 쓰기를 성공으로, 없는 값을 `0` 으로, `rtt_us`·`setup_ms` 순서, 밀리초 0 채움, 넘긴 줄마다 카운터, 시도 번호 0 부터, `CREATE_NEW` → `OPEN_ALWAYS`, 종료 줄의 `setup_ms`, 실패 뒤 벽시계 읽기. 기동: 열기 실패 무시, PID 0, 인자 검사 전 열기, 파일 이름에 현지 시각 |
| `docgate.py`, `platformgate.py` | 둘 다 `VERDICT: pass` |

## 크로스 모델 리뷰

Codex 를 렌즈 둘(계약과 정확성, 시험 충분성과 문서·기록 일관성)로 따로 돌렸다. 계약 렌즈는 LGTM 이었고, 같은 렌즈를 Win32
호출에 맞춰 한 번 더 돌린 것도 LGTM 이었다.

| 지적 | 처리 |
|------|------|
| warn. cli-check 의 파일 이름 검사가 숫자 꼴만 보아 UTC 기동 시각이 아니어도 통과한다 | 반영. 띄우기 전과 끝난 뒤의 UTC 사이에 있는지 초 단위로 본다. 현지 시각 변이로 확인 |
| warn. `icacls` 걷기와 임시 폴더 지우기의 실패가 조용히 묻힌다 | 반영. 남은 것을 실패 검사로 알린다 |
| warn. 벽시계를 쓴 줄마다 한 번, 실패 뒤에는 읽지 않는다는 헤더의 말을 시험이 보지 않는다 | 반영. 세는 벽시계. 실패 뒤 읽기 변이로 확인 |
| warn (수정 확인). Windows PowerShell 5.1 은 Stop 아래 네이티브 표준 오류를 예외로 바꿔 정리가 중간에 멈출 수 있다 | 반영. 정리 동작마다 따로 잡고 남은 것을 알린다 |
| warn (수정 확인). 벽시계 횟수를 합계로만 본다. 성공한 종료 줄과 처음부터 실패한 종료 줄의 횟수를 보지 않는다 | 반영. 호출마다 바로 보고, 종료 줄 케이스를 더했다 |
| warn (위 결함 수정 확인). 같은 초의 PID 재사용으로 인자 검사 전 열기가 이름 충돌로 실패하면 새 파일 없이 종료 코드 2 라 검사가 통과한다 | 반영. 실패 줄이 `ERROR args.invalid reason=bad_role` 이고 `socket.error` 가 없어야 한다 |

마지막 확인 리뷰(스테이지된 변경 전체, 두 렌즈)는 `LGTM - no blockers` 였다.
