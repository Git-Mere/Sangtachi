# Phase 4 4차 K: 같은 기기 검증 스크립트와 캡처 체크리스트

## 왜 했나

Phase 4 4차의 마지막 조각이다. J 가 랑데부와 로비를 세션에 이어 같은 기기의 프로세스들이 방을 만들고 펀치해 `CONNECTED`
까지 간다. `roadmap.md` Phase 4 검증 가운데 프로세스 여럿과 제어 서버가 있어야 판정되는 항목을 스크립트로 돌리고, 패킷
캡처로만 판정되는 항목은 사람이 돌릴 절차로 남긴다.

## 무엇을 바꿨나

| 무엇 | 자리 |
|------|------|
| 같은 기기 세션 확인. 장면 열: `connect`, `data`, `host-stays`, `room-ended`, `departed-now`, `departed-retry`, `punch-fail`, `handshake-fail`, `peer-close`, `records`. 전체 시험이 lobby-check 뒤에 돈다 | `scripts/session-check.ps1`, `scripts/test.ps1` |
| lobby-check 와 session-check 가 같이 쓰는 함수(로그 읽기, 프로세스 띄우기, 하네스의 시작과 끝, 시험 빌드)를 옮겼다. lobby-check 는 그것을 읽는다 | `scripts/harness-lib.ps1`, `scripts/lobby-check.ps1` |
| 가짜 피어. 관리 명령 `fake_host`, `fake_player`, `fake_clear` 와 모드 `silent`, `ack_only`, `hello_only`, `close01`, `close03`. 하네스의 HTTP 로 방에 들어가 LAN 주소를 후보로 등록하고 터널 헤더를 조금 말한다 | `control-server/tests/harness/cp_harness.py`, `control-server/tests/test_harness.py`, `control-server/README.md` |
| 로그 셋. `session.endpoint`(학습한 주소), `session.rtt`(`PONG` 표본), 시험 빌드의 `data.echo`(에코 왕복) | `peer/session.cpp`, `testbuild/instrument.*`, `main.cpp`, `architecture.md` 9장. 영어 미러 |
| 합성 패킷의 송신 시각을 RTT 시계로 채운다 | `main.cpp`, `roadmap.md` Phase 4 시험 장치의 레이아웃 표. 영어 미러 |
| 캡처로만 판정하는 항목의 손 절차와, roadmap 항목마다 어디서 판정하는지의 대조 표 | `tools/phase4-capture/README.md` |
| 두 자리를 가리키는 한 단락 | `roadmap.md` Phase 4 시험 장치. 영어 미러 |
| 시험 | `tests/peer/session_test.cpp`, `tests/testbuild/instrument_test.cpp` |
| 다음 할 일을 다시 적었다 | `plan.md` |

## 중요한 결정

**학습한 주소를 로그에 싣는다.** `roadmap.md` 의 같은 LAN 항목은 "학습된 `peer_endpoint` 가 상대의 로컬 후보인 것을 로그로
확인한다" 인데 그것을 싣는 줄이 없었다. 캡처 조각이 이것을 찾았다. 문서를 "캡처로" 로 바꾸는 대신 `session.endpoint` 를
더했다. 같은 기기에서도 판정되고 두 PC 에서도 같은 줄로 본다.

**RTT 기준선의 송신 시각은 RTT 시계다.** 처음에는 타이머 시계(`GetTickCount64`)로 채웠다. 해상도가 약 15ms 라 같은 기기의
에코가 0 이나 15 로만 재졌고, 한 번은 중앙값이 15ms 가 되어 `PING` 중앙값(0.5ms)과 10ms 넘게 갈렸다. `concurrency.md` 4장이
"RTT 측정만 `QueryPerformanceCounter`" 라 하므로 그 시계의 밀리초로 채우고 레이아웃 표의 말을 맞췄다.

**가짜 피어는 하네스 안에 둔다.** 제어 서버를 HTTP 로 부르고 STUN 응답기처럼 같은 프로세스에서 돈다. 장면마다 하네스를 새로
띄우지 않고 관리 명령으로 만든다. 서버 코드(`controlplane/`)는 고치지 않았고 배포물에 들지 않는다.

**캡처로만 판정하는 항목은 사람이 돌린다.** 이 기기에는 Wireshark, Npcap 이 없고 pktmon 은 관리자 권한이 없다. 절차는
실행해 보지 않은 것이고 README 가 그렇게 적었다. 첫 실행에서 어느 인터페이스에 잡히는지부터 확인하게 했다. 와이어에서
재협상, 종료 상태 무응답, 잠정 경로, 무검증 예산을 보려면 지금 모드로는 만들 수 없는 주입기가 필요해 보류로 적었다(C6~C9).

**문서가 정하지 않은 작은 것은 각 자리가 정했다.** 가짜 피어는 `cp_harness.py` 머리 표, 캡처 판정의 허용 폭(`T - 50ms` ~
`T + 200ms`)과 관찰 창(10초)은 README 의 "이 문서가 정한 것" 표에 근거와 함께 있다.

## 검증

| 무엇 | 결과 |
|------|------|
| 전체 시험 (`scripts/test.ps1`) | 단위 592건 통과, cli/e2e/lobby/session 검사 실패 0, `all tests passed`. DynamoDB local 위에서 돌렸다 |
| 제어 서버 시험 | `pytest -q --store` 1024 passed. `PYTHONUTF8=1` 로 돌렸다. 없으면 이 변경 전부터 넷이 이 기기의 cp949 표준 오류 때문에 떨어진다 |
| session-check 의 장면 | 모두 통과. 학습한 주소는 로컬 후보(`10.0.0.49`)였고 반사 주소가 아니었다. RTT 기준선은 `PING` 중앙값 0.417ms 대 에코 1.000ms 였다. 세션이 끝난 뒤 서버의 `peer.released by=host` 까지 65ms 였다 |
| 가짜 피어 | 모드마다 진짜 클라이언트에서 기대한 결과가 났다(`HOLE_PUNCH_TIMEOUT`, `PEER_HANDSHAKE_FAILED` 둘, 기록 줄 `ABORTED`·`PEER_FAILED`). 하네스 시험의 변이 6종이 모두 잡혔다 |
| session-check 의 판정력 (깨끗한 사본에서 클라이언트 변이) | 8종 모두 잡혔다. 즉시 보고 빼기(`departed-now`), 플레이어가 실패 뒤 로비로 가지 않기(`punch-fail`), 수립 전 이쪽 종료를 `PEER_FAILED` 로 적기(`peer-close`), 방이 끝나면 늘 로비로 가기(`room-ended`), 실패한 보고에서 `departed` 비우기(`departed-retry`), 에코 응답기 빼기(`data`), `session.endpoint` 가 반사 후보를 싣기(`connect`), `FAIL` 줄의 `<상대>` 를 늘 `-` 로(`punch-fail`) |
| `docgate.py` | `VERDICT: pass` |

**전체 시험의 `probe200` 실패 한 번.** lobby-check 장면 `loop-alive` 의 `held: every probe200 elapsed_ms <= 400` 이 두 번
떨어졌다(453ms, 485ms). 둘 다 하위 에이전트가 같은 기기에서 빌드와 시험을 동시에 돌리던 때다. 기기가 한가할 때 그 장면만 두 번
돌려 둘 다 최대 219ms 로 통과했고, 전체 시험도 통과했다. 이 변경은 그 장면이 지나는 경로(미결 `create_room`)를 고치지 않았다.

## 크로스 모델 리뷰

Codex 를 렌즈 둘(같은 기기 검증의 계약과 정확성, 문서 일관성)로 따로 돌렸다.

| 지적 | 처리 |
|------|------|
| blocker. 에코 중앙값이 0 이면 상대차가 0 이 되어 RTT 기준선이 무엇이든 통과한다 | 반영. 그때는 절대차 하나로만 본다 |
| warn. `room-ended` 가 방이 끝난 뒤 `DATA` 왕복을 보지 않는다 | 반영. 그 방을 시험 빌드로 띄워 두 플레이어의 에코를 본다 |
| warn. `departed-retry` 가 서버 처리 전에 실패시키고 둘째 세션을 실패 응답 뒤에 끝내, 따로 된 두 성공 호출로도 통과한다 | 반영. 첫 즉시 보고를 2초 붙든 사이에 둘째 세션을 끝내고, 실패한 요청 다음 첫 성공 요청의 줄까지에 두 회수 줄이 다 있는지 본다. 처음 3초로 붙들었더니 클라이언트의 수신 상한 3초가 먼저 끊고 다음 요청을 보내 서버에서 두 요청이 겹쳤다. 붙드는 시간을 2초로 줄이고 그 이유를 적었다 |
| warn. `records` 가 타임아웃 기다림의 결과를 버리고 기록 줄의 결과 값을 보지 않는다 | 반영. 기다림을 확인으로 바꾸고, 가짜 쌍은 `HOLE_PUNCH_TIMEOUT` 한 줄, 진짜 쌍은 `OK` 와 `CLOSED` 인지 본다 |
| nit. 로그 표의 키가 열일곱인데 "열네 개" 라고 적혀 있다 | 반영. 한국어와 영어 둘 다 |
| warn. 대조 표가 `room-ended` 와 `departed-retry` 의 범위를 넘겨 적었다 | 반영. `DATA` 는 위 수정으로 덮었다. `departed-retry` 는 서버 처리 전 실패만 보고, 처리 뒤 응답만 잃는 경우는 회수가 멱등이라 서버 로그에 드러나지 않아 단위 시험만 본다고 적었다 |
| nit. "roadmap 원문" 이라 한 문장이 원문 그대로가 아니다 | 반영. 원문을 그대로 옮겼다 |

수정 확인 리뷰는 `LGTM - no blockers` 였다.

마지막 확인 리뷰(스테이지된 변경 전체)는 warn 하나를 냈다. 캡처 README 의 C4-3, C4-4 가 "그 주소로" 를 가르려면 상대 후보가 둘
있어야 하는데 가짜 피어는 하나만 등록한다. 반영했다. 두 장면은 개수와 `reason` 만 판정하고 목적지는 관측값으로만 적으며, 수립
중 세션의 목적지 판정은 보류로 적었다. 기록 양식도 맞췄다. 그 수정의 확인 리뷰가 표와 기록 양식의 어긋남 둘을 더 찾았고 고친
뒤의 확인은 `LGTM - no blockers` 였다.
