# 클라이언트 [control] 스레드, 로비 명령, FAIL 줄

## 왜 했나

`roadmap.md` Phase 3 작업의 클라이언트 쪽이다. 서버와 배포는 앞 기록(08~11)에서 끝났다. 이번에 C++
클라이언트가 제어 평면을 부르고, 로비 명령으로 방을 만들고 참가하고, 실패를 `FAIL` 줄로 낸다.

## 무엇을 바꿨나

작업자 넷이 나눠 만들고 디렉터가 문서와 검증을 맡았다.

| 갈래 | 파일 | 무엇 |
|------|------|------|
| 코덱 | `client/*/control/{json,http,ops,constants}` | 직접 쓴 JSON 파서와 직렬화(NFR-5), 3.3 요청 바이트, 3.5 응답 검사, 연산 다섯의 요청과 해석, 8.3 분류, 받은 후보 위생(protocol.md 10.1), 4.6 호스트 행동 분류, `client_nonce` |
| `[control]` 스레드 | `platform/tcp`, `control/{exchange,channel}`, `spsc_ring.hpp`, `loop`, `main`, `args` | 요청마다 새 TCP 연결(연결 3초, 수신 단계 3초), SPSC 링 둘, 대기 집합 순위 4, 종료 때 join 2초 뒤 `_exit`. `--server` 필수, 역할 없는 `--room` 은 기동 실패 |
| 로비 | `control/{lobby,runner}`, `platform/stdout` | 스레드 없는 상태 기계와 그 실행기. 시도마다 새 `StunClient`. 기동 STUN 을 없앴다. 표준 출력은 리다이렉트면 UTF-8 + LF, 콘솔이면 유니코드 쓰기 |
| 하네스 | `control-server/tests/harness/`, `scripts/lobby-check.ps1` | 진짜 서버를 DynamoDB local 의 새 테이블에 띄우고 관리 포트로 지연, 오류, 커밋 뒤 실패, 시계, 속도 제한을 주입한다. `lobby-check.ps1` 이 roadmap 의 "로비" 와 "클라이언트 쪽 계약" 묶음 전 항목을 돈다. e2e 의 Phase 2 STUN 검사를 이리로 옮겼다 |

`scripts/test.ps1` 에 `-Jobs` 를 넣었다. 기본은 코어 수만큼 `ctest -j` 다. 필터 없는 실행은
`lobby-check.ps1` 까지 돈다.

## 문서를 먼저 정한 것

작업자가 "문서에 없다" 로 보고한 자리를 코드보다 먼저 문서에 정했다.

- `control_plane.md`
  - 2.4: CSPRNG 실패면 요청 없이 `CONTROL_PLANE_EXCHANGE_FAILED`
  - 2.6: `CLIENT_JSON_MAX_DEPTH = 32`. 응답 머리에도 `MAX_HEADER_BYTES`
  - 3.3: `Host` 값은 `--server` 의 이름과 포트
  - 3.5: 클라이언트 검사를 1~8 로 폈다. 형이 틀린 성공은 일시 오류, 표에 없는 코드는 확정 오류
  - 4.6: 호스트 오류 표에 "그 밖의 확정 오류" 와 형이 틀린 성공을 넣었다
  - 8.2: 송신은 호출별, 수신은 단계 전체 3초
  - 8.4: 호스트는 `ready: false` 상대를 `confirm` 한다. 위생 뒤 후보가 0 이면 준비 완료가 아니다
- `concurrency.md` 7장 `_exit` 종료 코드 0, 8장 응답 큐 가득, `[control]` 대기 실패, 50ms 종료 확인, 미결 중
  차례가 온 `host_report` 와 재시도
- `protocol.md` 10.1: 받은 목록의 순서(형, 위생, 중복, 상한)와 브로드캐스트 대역
- `architecture.md` 3.5: 낱말이 남는 명령, 표준 출력 인코딩. 8장: `CONTROL_PLANE_EXCHANGE_FAILED` 문장을
  호스트에게도 맞게 바꿨다. 9장: `unknown_code`, `self_in_peers`
- `roadmap.md` Phase 3: 이 Phase 의 클라이언트는 8.4 의 8번에서 멈춘다(리뷰가 8.4 에서 옮기게 했다).
  재시도 검증의 전송 오류를 루프백에서 만드는 법과 둘째 `internal` 의 주입점

## 중요한 결정

- **전체 시험이 DynamoDB local 을 요구한다.** `lobby-check.ps1` 때문이다. 저장소 소유자가 승인했다. 없으면
  건너뛰지 않고 실패한다. ADR 0014 가 승인한 시험 전용 의존성(Docker DynamoDB local)을 클라이언트 시험도 쓰는
  것이고 새 의존성이 아니다
- **Windows 루프백에서는 connect 타임아웃을 만들 수 없다.** 리슨 대기열을 채운 실측에서 약 2.03초 뒤
  `WinError 10061` 거부였다. 전송 오류는 무응답 서버와 닫힌 포트로 만든다
- **루프백 STUN 매핑은 서버 위생이 거부한다.** 하네스의 STUN 응답기는 `192.0.2.1` 과 출발지 포트를 적는다.
  Phase 4 의 펀치를 이 하네스로 돌리려면 다른 후보가 필요하다
- 실행기의 재진입 막이를 지운 변이(F12)는 살아남는다. 큐가 FIFO 라 안쪽 `run` 이 바깥의 남은 행동을 같은
  순서로 비운다. 코드를 읽어 동등으로 판정했다

## 검증

| 무엇 | 결과 |
|------|------|
| `scripts/test.ps1` 필터 없이 | 단위 280/280, `cli-check` 통과, `e2e` 실패 0, `lobby-check` 실패 0 건너뜀 0 |
| `control-server` `pytest -q --store` | 877 passed |
| 플랫폼 게이트 | `VERDICT: pass` |
| 변이 | 코덱 85, 로비 71, `[control]` 46(F12 동등 하나 남음), 하네스와 스크립트는 하네스 변이, 주입 끄기, DynamoDB 없음, 클라이언트 결함 넣기, 부분 실패 넣기가 전부 FAIL. 수와 표는 작업자 보고서가 출처다 |
| 실제 서버 | 배포된 EC2(b20224e)에 이 기기의 호스트와 플레이어 프로세스 둘. 공개 STUN 둘의 응답, `create_room`, `join_room`, `register_candidate`, 호스트 `host_report` 둘, 플레이어 `get_peers` 열여섯 뒤 양쪽이 `control.peers` 한 줄씩 냈다. 가상 IP 는 호스트 `10.100.0.1`, 플레이어 `10.100.0.2`. `FAIL` 없음, 둘 다 `quit` 로 종료 코드 0 |

서로 다른 네트워크의 두 클라이언트(Phase 3 산출물)는 두 번째 기기가 있어야 한다. 이번은 한 NAT 뒤의 두 프로세스다.

## 크로스 모델 리뷰

Codex. 범위 다섯(코덱, `[control]` 스레드, 로비, 하네스와 스크립트, 문서)에 렌즈 둘씩.

| 라운드 | 범위 | 결과 | 처리 |
|--------|------|------|------|
| c1 | 코덱 | warn 2. 9번째 후보가 전부 유효해 형 검사 전에 자르는 구현이 통과한다. 깊이 시험이 구현 상수에서 나온다 | 반영. 형이 틀린 9번째 케이스, 문서 값 32 를 직접 적었다. 변이 둘이 새 케이스에서 떨어진다 |
| c1 | `[control]` 스레드 | warn 2. 자동 리셋 이벤트를 여덟 알림으로 센다. bind 가 DNS 해석보다 먼저다(8.4 의 2번과 3번) | 반영. 누적 수를 마감과 함께 기다린다. 해석 뒤 bind. cli-check 가 해석 실패 때 `socket.bind` 가 없음을 본다 |
| c1 | 로비 | warn 1. `host_report` 의 자기 `peer_id` 전달이 시험되지 않는다 | 반영. 실행기 시험 추가, 변이가 떨어진다 |
| c1 | 하네스와 스크립트 | blocker 2, warn 3. 주입 지연이 종료를 막아 테이블이 남는다. 역할 없는 `--room` 확인이 무한히 멈출 수 있다. 기동 실패 정리, 하네스 종료 코드 미확인 | 반영. 지연을 깨울 수 있게 하고 지연 중 종료 시험을 더했다. 나머지도 고치고 변이로 확인 |
| c1 | 문서 | warn 3, nit 2. 송신 지연 설명, 전송 오류 일반화, Phase 범위를 `control_plane.md` 가 가짐, 실험 이력, 절 제목 없는 참조 | 반영. Phase 3 범위를 `roadmap.md` 로 옮겼다 |
| c2 | 고친 자리 | warn 2. `cli-check.ps1` 에 같은 블로킹 읽기가 남았다. 하네스 fixture 의 기동 정리가 일부만 감싼다 | 반영. 같은 꼴을 `scripts/` 전체에서 찾아 하나뿐임을 확인 |
| c3 | 고친 자리 | warn 1. `lobby-check` 와 `e2e-check` 의 프로세스 시작 도우미가 부분 실패 때 스트림과 자식을 남긴다 | 반영. 자원을 얻는 도우미 전부를 표로 판정했다. 남은 하나(`mutate.py` 시간 초과가 손자 프로세스를 남길 수 있음)는 `plan.md` 에 적었다 |
| c4 | 고친 자리 | warn 3. `test_server.py` 의 시험 안 소켓과 파이프가 부분 실패 때 닫히지 않는다 | 기각. 이번에 닫은 것은 실행 뒤에도 남는 자원(프로세스, 테이블, 임시 폴더)이다. 이 셋은 pytest 프로세스 안의 핸들이라 프로세스가 끝나면 회수되고, 이미 실패하는 시험에서만 생긴다 |
