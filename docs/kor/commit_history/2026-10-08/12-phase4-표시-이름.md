# Phase 4 1차 D 와 D2: 표시 이름 (FR-16)

## 왜 했나

1차 D 는 제어 서버에 표시 이름을 넣었다. 서버가 이제 `create_room` 과 `join_room` 에 `name` 을 요구하므로 D 만 합치면
클라이언트가 그 계약을 못 지켜 `lobby-check` 가 깨진다. 그래서 4차 J 에 있던 클라이언트 쪽 이름 작업을 D2 로 떼어 같은 worktree 에서
이어 만들고, 둘을 한 커밋으로 합쳤다. 저장소 소유자가 이 진행을 골랐다.

## 무엇을 바꿨나

**제어 서버 (D).**

| 무엇 | 자리 |
|------|------|
| 2.7 이름 검사(63자 집합 직접 대조, 1~16자)와 비교 키, `MAX_NAME_LEN`, `name_taken` | `controlplane/ids.py`, `constants.py`, `errors.py` |
| `create_room`/`join_room` 의 `NAME#` 선점(취소 사유 3번, 트랜잭션 시도 한 번), 두 회수 경로의 `NAME#` 삭제(`peer_id` 로 찾음), `peers` 원소의 `name` | `controlplane/ops.py`, `store.py` |
| 후보 위생에 `0.0.0.0/8`, `240.0.0.0/4` | `controlplane/candidates.py` |
| 헤더 이름 안이나 이름과 콜론 사이의 공백·탭 거부 | `controlplane/server.py` |
| 2.7 케이스 표 `NAME_CASES` 와 변이, 기존 표와 변이 갱신, 낡은 주석과 시험 문구 | `control-server/tests/` |

**클라이언트 (D2).**

| 무엇 | 자리 |
|------|------|
| `--name` (Phase 4 부터 필수. 없으면 `missing_name`, 형식이 틀리면 `bad_name`, 종료 코드 2) | `client/src/args.cpp` |
| 로비 명령 `name` (로비에서만, 틀리면 `WARN` 한 줄이고 이름을 바꾸지 않음, 받으면 `INFO lobby.name`) | `client/src/control/lobby.cpp` |
| 요청 본문의 `name`, 응답 `peers` 원소의 `name` 형식 검사(3.5 의 7번), `name_taken` 확정 오류 | `client/src/control/ops.cpp`, `runner.cpp` |
| 형식 판정은 모두 1차 A 의 `tunnel::is_valid_name` | |
| 시험과 검사 스크립트 | `tests/`, `scripts/{cli,e2e,lobby}-check.ps1` |

**문서.** `control_plane.md` 7.1 `ids.py` 행, 2.7 이 새 케이스 표를 가리킴, 9장 케이스 표 목록에 2.7, 6.3 `NAME#` 키 형식. `plan.md`
는 닫힌 행 셋(서버 쪽 낡는 주석, 서버 위생 주석, 이름 검사 중복)을 지우고 제어 서버 배포를 다음 할 일로 올렸다.

## 중요한 결정

- **배포하지 않았다.** 배포 전에 만든 방은 `PEER#` 에 `name` 이 없어 그 방의 요청이 `internal` 이 된다(6.3). 배포는 승인 뒤다
- **문서가 정하지 않은 것을 코드가 정했다.** 문서 규칙과 어긋나지 않는다
  - 기동 인자의 누락 판정 순서에서 `missing_name` 을 맨 뒤에 두었다. Phase 3 까지의 오류 토큰이 같은 입력에서 그대로 나온다
  - 로비 `name` 의 `WARN` 토큰은 `join` 과 같은 꼴(`missing_name`, `bad_name`)이다
  - 끝난 시도의 요청이 아직 미결이어도 로비에 있으면 `name` 을 받는다. `architecture.md` 3.5 의 "받는 때" 가 "로비에 있을 때" 뿐이다
  - 새 로그 이벤트 `lobby.name`. 9장이 이벤트를 늘리는 것을 허용한다
- **같은 방에 곧바로 다시 들어오려면 이름을 바꿔야 한다.** 회수 전에는 같은 이름이 `name_taken` 이다(2.7). `lobby-check` 의 재참가
  장면이 이름을 바꾼 뒤 다시 참가한다
- `is_valid_name` 이 `tunnel/packet.hpp` 에 있어 인자와 제어 평면 코드가 터널 패킷 모듈을 포함한다. 중립 헤더로 옮기는 일을
  `plan.md` 에 남겼다

## 검증

| 무엇 | 결과 |
|------|------|
| 전체 시험 (`scripts/test.ps1`, main 에 합친 뒤) | 단위 395건 통과, cli/e2e/lobby 검사 실패 0, `all tests passed` |
| 서버 시험 (main, `pytest -q --store`) | 967 통과 |
| 서버 변이 (하위 에이전트) | `mutate.py --store` 572/572. 첫 실행의 ERROR 1건(`room-no-type` 의 old 가 두 번 나옴)은 변이 정의를 고쳐 풀었다 |
| 클라이언트 변이 (하위 에이전트) | C++ 17개 모두 단언 실패로 잡혔다. `lobby-check` 변이 2개도 떨어졌고, 그때 찾은 확인 약점 하나를 고쳤다 |
| `docgate.py` | `VERDICT: pass` |

## 크로스 모델 리뷰

하위 에이전트가 worktree 에서 Codex 리뷰를 돌렸다.

| 지적 | 처리 |
|------|------|
| D blocker. 새 형 검사 때문에 `room-no-type` 변이의 old 가 두 번 나와 ERROR 가 된다 | 반영. 변이의 old/new 를 늘려 한 번만 나오게 했다 |
| D warn. 서버 회수 시나리오가 지운 `NAME#` 를 갱신 목록에서 빼는지 보지 않는다 | 반영. 단언과 변이 `reclaim-removed-without-names` 를 더했다 |

D 의 2차와 D2 의 1차는 `LGTM - no blockers` 였다.

main 에 합친 뒤 staged diff 전체(서버와 클라이언트)에 Codex 리뷰를 한 번 더 돌렸고 `LGTM - no blockers` 였다.
