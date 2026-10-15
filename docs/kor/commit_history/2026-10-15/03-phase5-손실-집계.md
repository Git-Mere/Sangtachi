# Phase 5 손실 집계와 `session.loss`

## 왜 했나

`plan.md` 의 Phase 5 첫 일이다. 나머지 Phase 5 검증 항목이 손실 값을 읽는다. 계약은 직전 커밋이 정했다(`protocol.md` 4.5, 9.5 3번,
`architecture.md` 9장 로그 출력, ADR 0025).

## 무엇을 바꿨나

| 무엇 | 자리 |
|------|------|
| 세션의 손실 집계. `CONNECTED` 진입에서 baseline 을 창의 위치로 잡고, 창이 수용한 패킷 가운데 `pkt_pos` 가 baseline 보다 큰 것을 센다 | `peer/session.hpp`, `session.cpp` |
| 재협상 3번이 구간 값을 세션 합계에 더한 뒤 창을 지운다. 구간 밖이면 더할 것이 없다 | `session.cpp` `renegotiate` |
| `LossStats`(`expected`, `accepted`, `lost()`)와 `Session::loss()` | `peer/session.hpp` |
| 세션 표를 내는 두 자리(종료 전이, `emit_session_counters`)가 같은 함수로 `session.loss` 한 줄을 같이 낸다. 종료 절차와 `counters` 명령은 `emit_session_counters` 를 거친다 | `tunnel/tunnel.cpp` |
| 세션 시험 12건, 터널 시험 2건(세션 표와 같이 나가는지, 수신 주입 진입점으로 누락·재정렬·첫 패킷보다 앞선 번호·32비트 경계·재협상 합계) | `tests/peer/session_test.cpp`, `tests/tunnel/tunnel_test.cpp` |
| 다음 할 일에서 이 항목을 지웠다 | `plan.md` |

## 중요한 결정

- `accepted` 가 세는 "수용" 을 8.3 창의 수용으로 읽었다. 그 뒤 타입별 검사에서 버려진 패킷도 센다. 도착한 패킷이지 잃은 패킷이 아니다.
  4.5 의 "이후 수용될 때마다" 가 창 의사 코드의 "수용" 과 같은 낱말이다. 세션 머리의 "이 코드가 정한 것" 표에 적었다
- baseline 은 전이시킨 패킷의 `pkt_pos` 가 아니라 창의 `position` 이다. 4.5 의 `baseline = position` 그대로다. 전이 패킷이 재정렬이면
  둘이 다르다

## 검증

| 무엇 | 결과 |
|------|------|
| `pwsh -NoProfile -File scripts/test.ps1` (DynamoDB local 포함) | `all tests passed`. 리뷰 뒤 더한 세션 시험 둘은 `-Filter "session:\|tunnel:\|replay:"` 로 다시 돌려 통과했다 |
| 변이 여섯 | 전부 잡았다. 아래 표 |
| `docgate.py`, `--claims` | `VERDICT: pass` |

| 변이 | 잡은 시험 |
|------|-----------|
| 재협상에서 합계에 더하지 않음 | "session: renegotiation folds the segment into the session totals" |
| 창이 수용한 것을 baseline 과 비교 없이 셈 | "session: a number older than the first packet is not counted after CONNECTED" |
| 창의 `position` 을 0 에서 시작 | 위와 같은 시험 |
| 재협상 뒤 baseline 을 남겨 둠 | "session: renegotiation folds the segment into the session totals" |
| 종료 전이에서 `session.loss` 를 내지 않음 | "tunnel: session.loss goes out with each session table" |
| 합계를 마지막 구간으로 덮어씀 | "session: every finished segment stays in the session totals" |

처음 변이 실행은 ctest 필터를 `loss|replay` 로 줬다. ctest 는 태그가 아니라 이름으로 고르므로 이름에 `loss` 가 없는 새 시험이 빠졌고
변이 셋이 살아남은 것으로 나왔다. 필터를 `session:|tunnel:` 로 바꿔 다시 돌렸다.

재정렬 전이 시험은 변이로 돌리지 않았다. 그 시험은 전이 직후 `{0, 0}` 을 단정한다. baseline 을 전이 패킷의 `pkt_pos` 로 잡으면 그 자리에서
`expected` 가 2 라서 걸린다.

## 크로스 모델 리뷰

Codex. 정확성과 시험이 실제로 잡는가를 렌즈로 줬다.

| 지적 | 처리 |
|------|------|
| warn. 전이 패킷이 늘 창을 전진시켜, baseline 을 그 패킷의 `pkt_pos` 로 잡는 구현도 통과한다 | 반영. 재정렬 `HELLO_ACK` 이 전이시키는 시험을 더했다 |
| warn. 0 이 아닌 구간을 하나만 합쳐, 합계를 마지막 구간으로 덮어쓰는 구현도 통과한다 | 반영. 재협상 두 번의 시험을 더했고 그 변이를 잡는 것을 확인했다 |
