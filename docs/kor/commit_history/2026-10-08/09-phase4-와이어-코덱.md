# Phase 4 1차 A: 와이어 코덱

## 무엇을 바꿨나

| 무엇 | 자리 |
|------|------|
| `tunnel/packet`: 4.1 헤더의 필드 단위 빅엔디언 직렬화, 8.1 공통 검사 1~6번(`check_common`), 5장 페이로드 코덱(`HELLO`/`HELLO_ACK`, `PING`/`PONG`, `CLOSE` reason, `ROSTER` 수신 검사 1~5), `control_plane.md` 2.7 이름 검사, 오류 종류를 카운터 이름으로 바꾸는 `counter_name` | `client/include/sangtachi/tunnel/packet.hpp`, `client/src/tunnel/packet.cpp` |
| 3장 상수 가운데 빠졌던 `kMaxNameLen`, 11장 타이머 블록 | `client/include/sangtachi/protocol_constants.hpp` |
| 시험 27건 | `tests/tunnel/packet_test.cpp` |
| `peer/epoch` 의 보관 시간 두 상수가 11장 블록을 가리키게 함 | `client/include/sangtachi/peer/epoch.hpp` |

## 중요한 결정

- **디코더는 순수 함수이고 카운터를 올리지 않는다.** 오류 종류만 돌려주고 카운터는 수신 파이프라인(2차 E)이 올린다.
  8.1 의 7번(`drop_unknown_peer`)과 8번(`drop_retired_epoch`)은 세션 표와 폐기 목록이 필요해 파이프라인 몫이다
- **11장 타이머의 이름은 코드가 붙였다.** 11장은 표로만 적었다. 이미 다른 헤더에 있는 값(STUN, `get_peers`, `host_report`)은
  옮기지 않았다. 같은 값을 두 곳에 두지 않는다
- **합칠 때 보관 시간 중복을 하나로 줄였다.** B 의 `peer/epoch` 이 2분을 따로 들고 있었다. 11장 블록의 값을 가리키게 했다
- **`ROSTER` 의 `generation` 수신 해석.** 마지막 값이 없으면 어떤 값이든 받고, 비교는 부호 없는 `>` 이며 순환을 고려하지 않는다.
  5.7 이 송신 쪽 첫 값을 정하지 않았다. 3차 I 착수 전에 정한다(`plan.md`)
- **수신 쪽 epoch 0 검사는 넣지 않았다.** 4.3 의 "0 은 쓰지 않는다" 는 송신 규칙이고 8.1 에 수신 검사가 없다
- 문서 공백 셋(타입별 검사와 8.2·8.3 의 순서, `generation` 첫 값, 이름 검사 중복)은 직전 커밋에서 `plan.md` 에 적었다
- `drop_roster_name` 카운터는 아직 `counters.hpp` 에 없다. 2차 E 에서 카운터를 붙일 때 넣고 `PENDING_COUNTERS` 에서 뺀다

## 검증

| 무엇 | 결과 |
|------|------|
| 전체 시험 (`scripts/test.ps1`, main 에 합친 뒤) | 단위 360건 통과, cli/e2e/lobby 검사 `0 checks failed`, `all tests passed` |
| 변이 (하위 에이전트) | 46개 모두 시험이 잡았다. 처음 생존 셋(M18, M20, M46)은 시험 결함이라 케이스를 고쳤다. 넷(M19, M21, M45, M46)은 단언이 아니라 디버그 빌드의 범위 검사 크래시로 잡혔다. 가드를 지우면 버퍼 밖 접근이 되는 자리이고 그것이 가드가 막는 결함이다 |
| `docgate.py`, `platformgate.py` | 둘 다 `VERDICT: pass` |

## 크로스 모델 리뷰

하위 에이전트가 worktree 에서 Codex 리뷰를 한 번 돌렸고 `LGTM - no blockers` 였다.

main 에 합친 뒤 staged diff 에 Codex 리뷰를 한 번 더 돌렸고 `LGTM - no blockers` 였다.
