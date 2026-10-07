# 영어 미러, windows-prereq 6절 실물 검증

## 왜 했나

`plan.md` 다음에 할 일의 둘이다. 영어 미러는 푸시 전에 해야 하고 문서 게이트가 `mirror` 와 `parity`
로 막혀 있었다. `windows-prereq.md` 6절은 서버를 배포해야 돌릴 수 있었고 이제 배포돼 있다.

## 무엇을 바꿨나

| 파일 | 무엇 |
|------|------|
| `docs/eng/control_plane.md` | `2c1d684` 뒤 한국어 변경 전부. 케이스 표를 `control-server/tests/` 링크로 바꾼 것, 상수, 7.2 의사 코드, 7.6 배포와 호출 설정과 CloudWatch, 8~9장 |
| `docs/eng/architecture.md`, `concurrency.md`, `protocol.md`, `roadmap.md`, `spec.md` | 같은 범위의 한국어 변경 |
| `docs/eng/decisions/0014-control-server-tests-pytest-and-docker.md` | 새 미러. 파일 이름은 다른 영어 ADR 처럼 영어다. 게이트는 번호로 짝을 짓는다 |
| `docs/eng/commit_history/2026-10-06/05~12` | 새 미러 여덟 |
| `docs/{kor,eng}/control_plane.md` 6.3 | "예외는 둘이다" 를 "셋이다" 로. 뒤에 항목이 셋이다. 번역하던 작업자가 찾았다 |
| `docs/{kor,eng}/windows-prereq.md` 6절 | 검증 상태를 "부분" 에서 "실측" 으로. 머리의 상태 집계를 실측 5, 부분 실측 4, 미검증 4 로 |
| `docs/kor/plan.md` | 다음에 할 일 둘을 지웠다. 문서 부채의 영어 미러 항목을 지웠다 |

미러 작업은 하위 작업자 셋이 나눠 했다(`control_plane.md`, 나머지 살아 있는 문서와 ADR, 기록).
한국어는 고치지 않고 번역만 하게 했다.

## windows-prereq 6절

읽기만 하는 명령 둘이다.

| 판정 | 결과 |
|------|------|
| 인스턴스 안 `ss -ltnp4 'sport = :8000'` | `0.0.0.0:8000`. 같은 조회의 IPv4 전용이 아닌 쪽도 같다. `net.ipv6.bindv6only = 0`, 서비스 `active` |
| 이 기기의 `Test-NetConnection -Port 8000` | `TcpTestSucceeded : True` |

공인 주소는 `deploy/aws.local.md` 에만 있고 여기 적지 않는다.

## 작업자가 짚었고 고치지 않은 것

- ADR 0014 결정 3 과 기록 10 이 "`CLAUDE.md` 규칙 6" 을 가리킨다. 지금 `CLAUDE.md` 에는 번호
  규칙이 없고 그 규칙은 `sangtachi-document-workflow` 스킬의 6이다. 둘 다 기록 문서이고 당시에는
  맞았으므로 고치지 않는다
- `control_plane.md` 8.4 의 코드 블록이 영어 쪽이 한 줄 길다. 이번 동기화 전부터 그랬고 뜻은 같다

## 검증

| 무엇 | 결과 |
|------|------|
| `python tools/docgate/docgate.py` | `VERDICT: pass` |
| `python tools/docgate/docgate.py --claims` | `VERDICT: pass` |

## 앞 기록(13)의 리뷰 입력

13 의 크로스 리뷰 세 번은 diff 를 표준 입력으로 넘기는 명령이 셸에서 경로를 잘못 풀어(`type` 과
역슬래시) **표준 입력이 비어 있었다.** 리뷰어가 읽기 전용으로 저장소에서 staged diff 를 직접 읽고
판정했다. 지적의 파일과 줄 번호가 그때의 staged 내용과 맞는다. 이번 커밋부터는 `cat` 으로 넘긴다.

## 크로스 모델 리뷰

Codex. 렌즈 둘(`control_plane.md` 번역 충실도, 나머지 파일의 번역 충실도와 `windows-prereq`, `plan.md`).

| 렌즈 | 결과 | 처리 |
|------|------|------|
| `control_plane.md` | `LGTM` | - |
| 나머지 | nit 1. 기록 08 영어의 "20 sends" 가 "20번씩"(`503` 과 `413` 을 각각 20번)을 잃었다 | 반영 |
