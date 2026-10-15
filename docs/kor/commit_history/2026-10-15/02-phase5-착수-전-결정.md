# Phase 5 착수 전 결정과 15장 구현 적합성 목록

## 왜 했나

Phase 5 를 시작하기 전에 문서를 훑어 걸리는 자리 여섯을 찾았다. 저장소 소유자가 자리마다 선택지를 골랐다. 같이
`protocol.md` 15장을 Phase 일정 없는 목록으로 바꾸고, Phase 4 의 코드를 그 목록과 대조했다.

## 무엇을 바꿨나

| 무엇 | 자리 |
|------|------|
| `position` 이 `REPLAY_WINDOW` 에서 시작한다. 수용 창에 시간 기반 만료가 없다는 문장을 roadmap 에서 옮겨 왔다. 재협상 구간의 세션 합계 | `protocol.md` 4.5, 9.5 3번, `replay.cpp`, `replay_test.cpp` |
| 자동 재시도는 하지 않는다 | `protocol.md` 9.6, 10.4. `control_plane.md` 10장의 미정 행을 지웠다 |
| 15장을 구현 적합성 목록으로. 체크박스와 Phase 문구를 지우고 10.3 펀치, 5.5 `PING`/`PONG`, 4.5 손실 집계, 퍼즈 방어를 항목으로 넣었다 | `protocol.md` 15장 |
| Phase 4 구현 목록이 15장을 가리키고, 15장과 겹치던 항목을 지웠다. 이 Phase 에서 정해지는 것만 남겼다 | `roadmap.md` Phase 4 |
| Phase 5 선행 조건, 작업(손실 집계와 퍼즈 방어, ASan 빌드, C6~C9 하네스 모드), 검증(ASan, 장시간 시험의 자리, `session.loss`, 언더플로 변이, 재협상 합계, 재시도 없음, 1시간 시험의 두 집 관측) | `roadmap.md` Phase 5 |
| 로그 고정 이벤트 `session.loss` 와 그 필드 형식, 내는 때 | `architecture.md` 9장 로그 출력 |
| 15장 이름을 인용하던 주석 | `replay.hpp`, `session.cpp`, `natprobe.py` |
| C6~C9 의 하네스 모드가 Phase 5 작업이라는 것 | `tools/phase4-capture/README.md` |
| 결정과 버린 대안 | ADR 0025 |
| 다음 할 일을 Phase 5 로, 실측 대기 둘 추가, C6~C9 대기 삭제, `pkt_pos` 자리 삭제, 자기 Wintun 식별자와 시험 공백 자리 추가 | `plan.md` |

## 중요한 결정

결정과 버린 대안은 ADR 0025 다. 이 기록에는 그 밖의 판단만 적는다.

- 15장에 10.3 펀치와 5.5 `PING`/`PONG` 을 새 항목으로 넣었다. roadmap Phase 4 에서 지운 항목 가운데 15장에 없던 둘이다. 지우면서
  그 의무가 사라지지 않게 옮겼다
- 손실 집계가 "창에는 시간 기반 만료가 없다" 는 문장을 roadmap 에서 `protocol.md` 4.5 로 옮겼다. 규칙이라 사양 문서가 갖는다
- 선택지를 제시할 때 C6~C9 의 하네스 모드와 퍼즈 2단계 주입기가 "같은 도구" 라고 했는데 정확하지 않았다. 하네스 가짜 피어는
  Python 이고 와이어로 보내며, 퍼즈 2단계는 클라이언트 안의 수신 주입 진입점이다. roadmap 에는 둘을 따로 적었다
- 15장의 코드 대조는 하위 에이전트 셋이 항목을 나눠 읽기 전용으로 했다. 핵심 근거 몇은 직접 열어 확인했다(`pending_pings` 상한,
  자기 Wintun 식별자)

## 15장 대조 결과

대상은 HEAD `71d6b55` 의 `client/` 와 `tests/` 다. 32항목이 전부 코드에 있다. 시험이 비어 있는 자리는 `plan.md` 로 옮겼다.
새로 넣은 항목 셋 가운데 10.3 펀치는 "session: punch start sends HELLO to every candidate and arms both timers", 5.5 는 "session: PING
starts 5s after CONNECTED and PONG yields one RTT sample" 이 본다. `position` 시작값은 이 변경이 넣었다.

| 항목 (15장 순서) | 판정 | 근거 요약 |
|------|------|-----------|
| 1 3장 상수 | 있음 | `protocol_constants.hpp`. 값을 하나씩 고정하는 시험은 없고 동작 시험이 본다 |
| 2 헤더 직렬화와 라운드트립 | 있음 | `packet.cpp`. "packet: header round-trips through serialize and deserialize" |
| 3 소켓 옵션 넷 | 있음 | `udp_socket.cpp`. `SO_EXCLUSIVEADDRUSE`, `SO_REUSEADDR` 미사용, `connect()` 미호출은 시험 없음(호출 부재) |
| 4 bind, `getsockname`, `SO_RCVBUF` | 있음 | "udp: open binds to any address on an ephemeral port" 외 |
| 5 단일 수신 루프의 소켓 소유 | 있음 | 복사 금지와 이동으로 구조가 강제한다. `recv_from` 호출이 한 곳 |
| 6 출발지 위생과 분류 | 있음 | `loop.cpp`. "classify: the source hygiene table decides before the class" 외 |
| 7 8.1 공통 검사 8단계 | 있음 | "packet: check_common case table" 외 |
| 8 8.2 epoch/출발지 규칙 | 있음 | "session: HELLO_ACK checks virtual_ip, then nonce, then epoch" 외 |
| 9 `virtual_ip` 검사가 epoch 판정보다 먼저 | 있음 | "session: a HELLO with the wrong virtual_ip is dropped before it can pin" |
| 10 4.5 중복 억제, 부수 효과보다 먼저 | 있음 | `replay.cpp`, "replay: matches a set-based reference model" |
| 11 4.5 경계 넷 | 있음 | 경계마다 replay 시험 |
| 12 `is_newer` 의 `a != b` | 있음 | "replay: is_newer is false for equal values" |
| 13 `session_epoch` 와 폐기 목록 | 있음 | "epoch: retired list holds at most MAX_RETIRED_EPOCHS and evicts the oldest" 외. 새 시도의 epoch 가 이전과 다른지는 시험 없음 |
| 14 시도 nonce 와 probe nonce | 있음 | "nonce: a retired nonce is not reused" 외 |
| 15 10.1 후보 판정과 케이스 표 셋 | 있음 | 세 표 전 행. 자기 Wintun 식별자는 제품 경로가 아직 비워 넘긴다(어댑터가 Phase 6) |
| 16 10.2 랑데부 | 있음 | "lobby: a ready player reply opens one session with the delay of 10.2" |
| 17 10.4 엔드포인트 학습 | 있음 | "session: four probe paths at most, the fifth packet is still processed" 외 |
| 18 9.2 이중 플래그 | 있음 | "session: sent_ack is not set when sending HELLO_ACK fails" 외 |
| 19 9.4 전이표와 9.4.1 | 있음 | 행마다 시험이 흩어져 있다. 칸 단위 케이스 표와 `FAILED` 수신 시험은 없다 |
| 20 9.4 무검증 목적지 예산 | 있음 | "session: the unverified budget counts tokens as the roadmap does" |
| 21 9.5 재협상 10단계 | 있음 | "session: renegotiation from CONNECTED follows the ten steps". 3번의 손실 집계 초기화는 집계와 함께 Phase 5 |
| 22 9.5 재협상 상한 둘 | 있음 | "session: the ninth renegotiation is drop_reneg_limit" 외 |
| 23 11장 타이머 | 있음 | 세션, STUN, 로비 타이머 시험 |
| 24 `CLOSE` | 있음 | 받은 쪽의 라우팅 칸 제거는 보낸 쪽과 같은 경로이고 직접 시험은 없다 |
| 25 9.6 실패 전이 셋 | 있음 | "session: idle timeout fails CONNECTED as TUNNEL_DROPPED and sends CLOSE 0x03" 외 |
| 26 `tx_err_send`, `tx_err_ack` | 있음 | "session: a failed send of any type counts tx_err_send in the session table" 외 |
| 27 `pending_pings` 상한과 정리 | 있음 | 상한 분기를 밟는 시험은 없다. PING 간격과 항목 수명이 둘 다 5초라 지금 구성으로는 닿지 않는다 |
| 28 13장 STUN 범위 | 있음 | "stun: parse_binding_response case table" 외 |
| 29 `DATA`, 8.4, 8.5 송신 측 | 있음 | "tunnel: inner IPv4 case table", "tunnel: 8.5 checks run in order and count in the right table" 외 |
| 30 8.5 라우팅 표 | 있음 | "tunnel: the 8.5 routing case table". 4행은 2행과 같은 입력으로만 덮인다 |
| 31 `ROSTER` 수신 검사와 카운터 넷 | 있음 | "packet: decode_roster case table" 외 |
| 32 `ROSTER` 송신 시점 셋 | 있음 | "roster: a change is resent twice 5s apart, then every 15s" 외 |

## 검증

| 무엇 | 결과 |
|------|------|
| `pwsh -NoProfile -File scripts/test.ps1 -Filter replay` | 25/25 통과 |
| 변이: `position` 과 첫 `pkt_pos` 를 0 으로 되돌림 | replay 시험 14건 실패. 잡았다. 되돌렸다 |
| `pwsh -NoProfile -File scripts/test.ps1` (DynamoDB local 포함) | `all tests passed` |
| `docgate.py`, `--claims` | `VERDICT: pass`. 추가한 줄의 강한 주장 문구를 따로 훑었다 |

## 크로스 모델 리뷰

Codex. 렌즈 셋을 따로 돌리고, 고친 자리는 같은 렌즈로 다시 돌렸다.

| 렌즈 | 지적 | 처리 |
|------|------|------|
| 정확성과 계약 | warn. 구간 밖(첫 `CONNECTED` 전, 재협상 뒤 다음 `CONNECTED` 전)의 집계 값이 정해지지 않았다 | 반영. 세션 합계는 0 에서 시작하고, 재협상 3번은 구간 안일 때만 더하고, 구간 밖의 지금 값은 0 이라고 4.5 에 적었다(한·영) |
| 정확성과 계약 | warn. `lost / expected` 가 `expected` 0 에서 0 으로 나눈다. 4.5 의 0 분모 규칙과 어긋난다 | 반영. `architecture.md` 9장에 `expected` 가 0 이면 0 이라고 적었다(한·영) |
| 정확성과 계약 (2회차) | warn. 세션이 끝날 때 마지막 구간 값이 남는지가 읽기에 따라 갈린다 | 반영. 세션이 끝나면 마지막 구간 값이 그대로 남는다고 적었다(한·영). 3회차 `LGTM - no blockers` |
| 의무 보존과 낡은 참조 | warn. `control_plane.md` 1.1 표가 "누가 언제 재시도하는지는 미정" 으로 남았다(한·영) | 반영. 9.6 을 가리키게 고쳤다. 처음 검색이 "자동 재시도" 문구만 찾아 놓쳤다. "미정"·"undecided" 와 "재시도" 를 함께 다시 검색했고 살아 있는 문서에 남은 것은 없다 |
| 의무 보존과 낡은 참조 | warn. `plan.md` 가 Phase 4 에 실측만 남았다고 하는데 C6~C9 도 남았다 | 반영 |
| 한영 미러 | `LGTM - no blockers` | - |
