# Phase 5 마무리: 경계 크기, 시험 공백, 장시간 시험

## 왜 했나

`plan.md` 의 Phase 5 마지막 일과 남은 검증 항목이다. 경계 크기 표, 15장 대조에서 시험이 비었던 자리, 제어 평면 중단 10분,
유휴 타임아웃, 1시간 유지다.

## 무엇을 바꿨나

| 무엇 | 자리 |
|------|------|
| 경계 크기 단위 시험. 19 는 수신 `drop_type_length`(프로세스 표), 20·576·1452 는 페이로드 훅에 바이트 그대로, 1453 은 송신 `tx_drop_oversize` | `tests/tunnel/tunnel_test.cpp` "the Phase 5 boundary sizes" |
| 장면 `sizes`. 시험 빌드 두 프로세스로 20 은 편도, 576 과 1452 는 왕복을 `data.tx`/`data.rx` 의 `len`·`sha256` 으로 대조한다 | `scripts/session-check.ps1` |
| 시험 공백 넷. `FAILED` 수신의 `drop_terminal_state`, 받은 `CLOSE` 의 라우팅 칸 제거, 새 시도의 epoch, 라우팅 케이스 표 4행(`ROSTER` 뒤에도 다른 플레이어로 가는 길이 없다) | `tests/tunnel/tunnel_test.cpp` |
| 장시간 시험 스크립트. 장면 `idle-drop`, `cp-down`, `hour`. 장면마다 하네스를 새로 띄운다 | `scripts/soak-check.ps1` (새 파일) |
| 1시간 유지의 두 집 관측 절차(시험 5). 머리의 "캡처 C6~C9 는 여기 없다" 문장을 지웠다 | `field-test/README.md` |
| 별도 스크립트의 이름, field-test 시험 5 를 가리키는 문구 | `roadmap.md` Phase 5 검증 |
| Phase 5 끝, 다음 할 일은 Phase 6 착수 전 항목, 두 집 관측 대기의 조건, 시험 공백 자리를 `pending_pings` 상한 하나로 | `plan.md` |

## 중요한 결정

- 19 와 1453 은 `send` 가 만들 수 없는 크기다(20~`MAX_INNER`). 그래서 단위 시험이 본다. roadmap 표의 기대(19 는 수신 쪽, 1453 은
  송신 쪽)와 같은 자리에서 본다
- `idle-drop` 은 상대 프로세스를 강제로 끝내 `CLOSE` 없이 송신을 끊는다. 남은 쪽의 `TUNNEL_DROPPED`, `CONNECTED` → `FAILED`,
  `FAIL` 줄, 기록 파일의 종료 줄을 보고, 플레이어가 20초 동안 다시 참가하지 않는 것을 클라이언트 로그와 하네스 로그 둘로 본다(9.6
  자동 재시도 없음)
- `cp-down` 은 하네스를 관리 명령으로 내린다. 그 뒤 호스트의 `host_report` 가 실패하는 것으로 정말 내려갔음을 확인한다
- `hour` 의 손실 판정은 `lost=0` 이다. 같은 기기 안의 송수신이라 손실이 없을 것으로 기대한 판정이고, 이번 실행에서 그대로 관측했다. 같은 기기에서도 손실이 날 수 없다는 뜻은 아니다
- `pending_pings` 상한은 지금 구성으로 닿지 않는다(간격 5초, 수명 5초). 시험을 위해 구성을 바꾸지 않고 `plan.md` 에 남겼다

## 검증

| 무엇 | 결과 |
|------|------|
| `pwsh -NoProfile -File scripts/test.ps1` (DynamoDB local 포함) | `all tests passed`. ctest 613, `sizes` 장면 14 확인 통과 |
| `pwsh -NoProfile -File scripts/soak-check.ps1` (기본값 10분, 60분) | `0 checks failed, 0 not run`. 73분. `idle-drop` 11, `cp-down` 6(30초마다 양쪽 에코 3개, 20바퀴 실패 0), `hour` 5(1분마다 양쪽 에코 5개, 60바퀴 실패 0, 양쪽 `session.loss` 의 `lost=0`) |
| `soak-check` 의 판정이 실제로 떨어지는가 | 첫 실행에서 `session.failed` 줄의 필드 순서를 틀리게 적은 두 판정이 실패로 나왔다. 고친 뒤 통과했다 |
| `docgate.py`, `--claims` | `VERDICT: pass` |

## 크로스 모델 리뷰

Codex. `soak-check` 가 헛통과·멈춤·프로세스 누수·오판을 할 수 있는가, `sizes` 장면, 새 단위 시험, field-test 시험 5 와 문구를
렌즈로 줬다.

| 지적 | 처리 |
|------|------|
| warn. field-test 시험 5 가 "마지막 `KEEPALIVE` 의 시각(`timer.tick`)" 을 적으라고 하는데 로그에는 시각이 없고 `timer.tick` 은 받은 시각이 아니라 자기 타이머의 경과다 | 반영. A 가 시험 1 처럼 캡처를 켜고, 시각은 캡처로 본다고 고쳤다. 모으기 표에 `t5.pcapng` 를 더했다 |

전체 staged diff 의 최종 확인에서 warn 하나가 나왔다. `hour` 의 `lost=0` 근거를 "루프백이라 잃을 길이 없다" 로 적은 것이 지나치다는 것이다. 반영했다. 같은 기기 안이라 기대한 값이고 관측한 값이라고 고쳤다(기록 한·영, `soak-check.ps1` 주석). 고친 뒤 다시 돌린 최종 확인은 `LGTM - no blockers` 다.
