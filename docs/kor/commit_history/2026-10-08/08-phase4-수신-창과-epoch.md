# Phase 4 1차 B: 수신 창과 epoch

## 왜 했나

Phase 4 를 시작했다. 저장소 소유자가 `tools/nat-probe` 재측정 전에 시작하기로 했다. `roadmap.md` 는 재측정을 착수 전
항목으로 두지만, 측정 결과에 기대는 자리는 방 생성 전 호스트 NAT 경고 하나이고 이번 조각들 어디에도 없다. 그 결정과
조각 순서를 `plan.md` 에 적었다.

1차는 서로 다른 파일을 건드리는 조각 넷(A 와이어 코덱, B 수신 창과 epoch, C 로컬 후보와 위생, D 제어 서버)을 하위 에이전트가
각자의 git worktree 에서 병렬로 만들었다. 이 커밋은 B 다.

## 무엇을 바꿨나

| 무엇 | 자리 |
|------|------|
| `tunnel/replay`: `is_newer`(4.4), 수용 창(4.5). 판정(`check`)과 반영(`commit`)을 나눴고 정상 경로는 둘을 잇는 `accept` 다 | `client/include/sangtachi/tunnel/replay.hpp`, `client/src/tunnel/replay.cpp` |
| `peer/epoch`: `session_epoch` 생성(0 이면 다시 뽑음, 난수원 주입, 기본 `platform/random`), 폐기 목록(2분, 16개, 가장 오래된 것을 밀어냄), 시도 nonce 와 그 폐기 목록 | `client/include/sangtachi/peer/epoch.hpp`, `client/src/peer/epoch.cpp` |
| 시험 40건 | `tests/tunnel/replay_test.cpp`, `tests/peer/epoch_test.cpp` |
| 모듈 표와 트리에 두 모듈 | `architecture.md` 3.1, 10장 |
| Phase 4 조각 순서, 재측정 전 착수 결정, 구현 때 같이 볼 자리 다섯 | `plan.md` |

`plan.md` 의 새 "구현 때 같이 볼 자리" 가운데 셋(타입별 검사의 순서, `ROSTER` `generation` 첫 값, 이름 검사 중복)은 A 의 보고에서
나왔다. 같은 인수인계 문서라 여기서 함께 적었다.

## 중요한 결정

- **문서가 정하지 않은 동작 셋을 코드가 정했다.** 문서 규칙과 어긋나지 않는다
  - 같은 epoch 를 폐기 목록에 다시 넣으면 기존 항목을 지우고 맨 뒤에 넣는다. 2분도 다시 센다. 한 값이 두 칸을 차지하지 않게 한다
  - 난수원이 실패하면 값을 내지 않는다(`nullopt`). 예측 가능한 값으로 대신하지 않는다
  - 0 재추첨과 폐기 nonce 재추첨에 횟수 상한이 없다. 문서가 상한을 정하지 않았다
- **4.5 `pkt_pos` 는 첫 패킷보다 앞선 번호에서 감싸 돈다.** 4.5 공식을 그대로 따랐고 시험이 그 거동을 고정했다. Phase 5 손실
  집계 전에 문서를 고친다(`plan.md`)
- **8.3 을 지난 뒤 버려진 패킷도 창에 기록된다.** 4.5 의사 코드가 8.3 자리에서 갱신하므로 그것이 지금 규정이다. E 착수 때
  `protocol.md` 8.3 에 문장으로 적는다(`plan.md`)

## 검증

| 무엇 | 결과 |
|------|------|
| 전체 시험 (`scripts/test.ps1`, main 에 합친 뒤) | 단위 331건 통과, cli/e2e/lobby 검사 `0 checks failed`, `all tests passed` |
| 변이 (하위 에이전트) | 23종 모두 시험이 잡았다. 처음 하나(E6)가 살아남아 `size()` 단언을 더했다. 둘(R5, E8)은 첫 형태가 `/WX` 경고로 빌드에서 막혀 경고를 피하는 형태로 다시 돌렸다 |
| 리뷰 뒤 더한 두 케이스 | `-Filter "replay|epoch|nonce"` 43건 통과. 위치를 32비트로 자른 변이와 등록 시각을 32비트로 자른 변이를 각각 그 케이스가 잡았다 |
| `docgate.py`, `platformgate.py` | 둘 다 `VERDICT: pass` |

## 크로스 모델 리뷰

하위 에이전트가 worktree 에서 Codex 리뷰를 두 번 돌렸다.

| 지적 | 처리 |
|------|------|
| warn. 첫 패킷보다 앞선 재정렬의 `pkt_pos` 가 감싸 돈다 | 코드는 4.5 공식대로 두고 시험으로 고정했다. 문서 결정으로 넘겼다(위) |
| warn. 만료 단언이 구현 상수를 그대로 써서 TTL 을 바꿔도 통과한다 | 반영. `STATIC_REQUIRE` 로 120000 을 고정하고 경계를 리터럴로 적었다 |
| nit. 진짜 난수원의 두 epoch 가 다르다는 단언은 2^-32 확률로 정상 구현을 떨어뜨린다 | 반영. 그 단언을 지웠다 |

main 에 합친 뒤 staged diff 에 Codex 리뷰를 한 번 더 돌렸다.

| 지적 | 처리 |
|------|------|
| warn. `position` 이 2^32 를 넘는 시험이 없어 32비트로 자르는 구현이 살아남는다 | 반영. 2^31 미만씩 세 번 전진하는 케이스를 더했다. 변이로 확인했다 |
| warn. 폐기 목록의 등록 시각이 모두 32비트 안이라 시각을 자르는 구현이 살아남는다 | 반영. 2^32 를 넘는 시각에서 두 목록의 경계를 보는 케이스를 더했다. 변이로 확인했다 |
| warn. `plan.md` 의 새 행이 아직 main 에 없는 `tunnel/packet` 을 있는 것처럼 적었다 | 반영. "1차 A 가 만드는" 으로 고쳤다 |

2차 리뷰(그 수정 확인)는 `LGTM - no blockers` 였다.
