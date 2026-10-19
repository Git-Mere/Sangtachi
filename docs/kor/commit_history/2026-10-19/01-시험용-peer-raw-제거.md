# 시험용 `--peer`, `raw`, `rx.raw` 제거 (ADR 0028)

## 왜 했나

`plan.md` 의 다음 할 일 첫 줄이 시험용 `--peer` 와 `raw` 를 지울지, 언제 지울지 정하는 것이었다. 저장소 소유자가 둘 다 지우기로
정했다. 함께 정할 것 둘은 소유자가 권장안을 받았다. `rx.raw` 도 지운다. `scripts/e2e-check.ps1` 은 남기고 줄인다.
결정과 버린 대안은 ADR 0028 이다.

## 무엇을 바꿨나

| 무엇 | 자리 |
|------|------|
| `--peer` 파싱과 `Args::peer` | `args.hpp`, `args.cpp`, `main.cpp` |
| `raw` 명령, `send_raw`, 시험 바이트열, `rx.raw` 줄, `LoopOptions::peer` | `loop.hpp`, `loop.cpp` |
| `rx.raw` 만 쓰던 `sha256_short` 와 `kShortHashChars` | `hash.hpp`, `hash.cpp` |
| `--peer` 케이스를 지우고 `--peer` 가 `UnknownOption` 인 케이스 하나 | `args_test.cpp` |
| `raw` 시험 셋을 지웠다. 앞 16자 형식 시험을 지우고 해시가 다른지 보는 시험을 `sha256_hex` 로 | `loop_test.cpp`, `hash_test.cpp` |
| 주석 셋 | `udp_socket_test.cpp`, `tunnel_test.cpp`, `lobby.hpp` |
| `--peer` 케이스를 같은 오류 토큰의 다른 인자로 옮기고 `--peer` 가 알 수 없는 인자인 케이스 하나 | `cli-check.ps1` |
| 송수신 대조, 사슬 셋째 프로세스, 송신 실패 확인을 지웠다. 두 프로세스의 기동, bind, `counters`, 알 수 없는 명령, `quit` 이 남는다 | `e2e-check.ps1`, `test.ps1` 주석 |
| 3.5 의 시험용 입력 표와 `rx.raw` 절을 콘솔 명령 표로. `--peer` 형식 행, 9장 `rx.raw` 행, `hash` 와 CNG 의 용도 | `architecture.md` |
| 1장 `[console]` 의 미결 문장, 3장 `raw` 잔량 문장 | `concurrency.md` |
| Phase 1 검증의 판정 수단, Phase 4 시험 장치의 명령 수, Phase 7 작업에 이 제거 | `roadmap.md` |
| 결정 대기 행을 지우고 순서를 당겼다. Phase 1 문단에서 `e2e-check.ps1` 언급 | `plan.md` |
| 결정과 버린 대안 | ADR 0028 |

## 중요한 결정

- **`roadmap.md` Phase 1 검증의 두 항목은 지우지 않았다.** 판정 수단만 바꿔 적었다. 같은 성질은 지금 Phase 4 의 `DATA` 왕복
  일치가 본다. "두 머신이면 양쪽이 먼저 한 번씩 보낸다" 는 `--peer` 로 두 기기를 돌릴 때의 절차라 같이 지웠다. 홀펀칭은 원래
  양쪽이 동시에 보낸다
- **`hash` 모듈은 남겼다.** `wintun.dll` 해시 확인과 시험 빌드의 `data.tx`, `data.rx` 가 `sha256_hex` 를 쓴다
- **잃는 확인을 ADR 에 적었다.** 관리자 권한 없이 한 기기의 두 프로세스 사이 바이트 일치를 보는 끝에서 끝까지 시험이
  없어진다. 송신 실패는 `e2e-check.ps1` 대신 `loop_test.cpp` 의 송신 실패 시험이 본다. 리뷰 지적으로 그 시험에 `socket.error`
  줄과 실패 뒤의 콘솔 처리를 더했다

## 검증

- `pwsh -NoProfile -File scripts/test.ps1` 통과. ctest 622건과 cli, e2e, lobby, session 확인
  - 첫 전체 실행에서 `session-check.ps1` 의 `sizes` 확인 열셋이 떨어졌다. 호스트의 `timer.tick` 이 `elapsed_ms=191859` 로,
    루프가 약 192초 돌지 않아 세션이 idle 마감으로 `TUNNEL_DROPPED` 가 되고 그 뒤 `send` 가 `not_connected` 로 거부됐다.
    이 변경이 닿지 않는 경로이고 `session-check.ps1` 만 다시 돌려 전부 통과했다. 기기가 잠시 멈춘 것으로 본다
- 변이 둘이 모두 잡혔다
  - `--peer` 를 값을 받는 인자로 되돌리면 `args: reject case table` 이 떨어진다
  - 송신 실패의 `socket.error` 줄을 빼면 `loop: send_datagram counts and logs a failed send` 가 떨어진다
- `python tools/docgate/docgate.py` 와 `--claims` 통과

## 크로스 모델 리뷰

Codex 로 staged diff 를 두 번 보였다. 한 번은 코드, 시험, 스크립트의 정확성과 잃는 시험을 보게 했다. 한 번은 저장소 전체에서
낡은 설명, 미러 불일치, ADR 과 `roadmap.md` 의 주장이 코드와 맞는지를 보게 했다. 지적은 넷이고 모두 `warn` 이다. 모두 반영했다.

| # | 지적 | 처리 |
|---|------|------|
| 1 | `e2e-check.ps1` 의 송신 실패 확인을 지우면 `socket.error` 의 오류 코드와 루프 생존을 보는 시험이 없다. 단위 시험은 `tx_err_send` 만 본다 | 반영. 송신 실패 시험이 `socket.error` 줄의 `op` 와 `code` 를 보고, 실패 뒤 `counters` 를 처리하는지 본다 |
| 2 | ADR 이 송신 실패 시험이 루프가 계속 도는 것을 본다고 적지만 그 시험은 루프를 돌리지 않는다 (미러 포함) | 반영. 1 로 시험을 넓히고 ADR 문장을 그 시험에 맞췄다 |
| 3 | "세션 없이 아무 주소로 보내는 경로가 없다" 가 넓다. `--stun` 은 사용자가 준 주소로 세션 전에 Binding Request 를 보낸다 (미러 포함) | 반영. 임의의 바이트열을 보내는 경로가 없고 `--stun` 주소에는 Binding Request 만 나간다로 좁혔다 |
| 4 | `lobby.hpp` 주석이 아직 `raw` 를 콘솔 명령으로 적는다 | 반영. `send` 로 고쳤다 |
