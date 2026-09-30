# Plan

**이 파일은 다음 세션의 인수인계다.** 무엇이 남았고 무엇을 먼저 하는지만 적는다. 끝난 일의
경과는 [`commit_history/`](commit_history/README.md)가 갖는다.

## 남은 일이 어디에 있는가

| 축 | 출처 | 지금 |
|----|------|------|
| 구현 | [`roadmap.md`](roadmap.md) | **Phase 1 구현 완료.** 두 머신 실측만 대기다. 다음은 Phase 2 (STUN) |
| 문서 부채 | 이 파일 "문서 부채" 절 | 8건 |

**토폴로지 후속은 끝났다.** [ADR 0006](decisions/0006-무중계-스타-토폴로지.md),
[ADR 0007](decisions/0007-gui-핵심-범위-qt.md), [ADR 0008](decisions/0008-스타-토폴로지-후속-결정.md),
[ADR 0009](decisions/0009-방-호스트-임대.md)가 정한 것이 한국어 살아 있는 문서에 반영됐다.
코드 쪽은 지금 있는 자리만 따라갔다. `--rejoin` 인자가 사라졌고 나머지는 해당 Phase 의 몫이다.
그 넷이 남긴 미결 가운데 아직 값이 없는 것은 시점이 [`roadmap.md`](roadmap.md)에 걸려 있다.
호스트 NAT 경고 여부는 Phase 4 착수 전, GUI 스레드 모델과 GUI 기동 입력 경로는 Phase 8 착수
전이다. **설계 감사 후속도 남아 있지 않다.** 경과는 [`commit_history/`](commit_history/README.md)가 갖는다.

## 다음에 할 일

| 순서 | 무엇 | 왜 |
|:--:|------|-----|
| 1 | `docs/eng` 미러를 한국어에 맞춘다. 대상은 `spec.md`, `architecture.md`, `protocol.md`, `concurrency.md`, `roadmap.md`, `windows-prereq.md` 여섯과 [ADR 0010](decisions/0010-플랫폼-이식-이음새.md), `commit_history/` 의 최근 항목이다. `tools/` 의 README 는 미러를 두지 않는다 | 미러가 한국어보다 뒤에 있다. 게이트가 `parity` 로 막히는 것이 그 표시다. 푸시 전에 맞춘다 |
| 2 | Phase 2. STUN 클라이언트 ([`protocol.md`](protocol.md) 13장 STUN 사용 범위, [`architecture.md`](architecture.md) 3.5 기동 입력의 서버 선택) | Phase 1 이 닫혔다. 같은 소켓을 쓰므로 래퍼가 이미 있다 |
| 3 | Phase 2 를 끝낼 때 전문 대상 감사 | `CLAUDE.md` 가 Phase 를 끝낼 때마다 요구한다 |
| 4 | Phase 3 착수 전 항목 ([`roadmap.md`](roadmap.md)). Elastic IP·DNS, 자격 증명, 프리 티어 확인, 배포 설정 값, 시계 전제 둘 | 계정과 인스턴스가 필요한 일이라 문서로 끝나지 않는다 |

## 대기 중인 것

| 무엇 | 막힌 이유 | 풀리는 조건 |
|------|-----------|-------------|
| 방화벽 인바운드 실측 | 상대 피어가 필요하다 | 두 번째 기기 |
| Phase 1 의 두 머신 실측 | 한 기기의 여러 프로세스로는 돌렸다 | 두 번째 기기. 절차는 `scripts/e2e-check.ps1` 과 같고 `--peer` 만 상대 주소로 바꾼다 |
| 로비 체류가 NAT 매핑에 무엇을 하는가 | 측정한 적이 없다. [`concurrency.md`](concurrency.md) 7장 로비가 "단정하지 않는다" 로 적었다 | Phase 4 의 `tools/nat-probe` 재측정과 같이 본다 |
| [`windows-prereq.md`](windows-prereq.md) 실물 검증 | 어댑터가 아직 없다 | Phase 3(EC2), 6(Wintun), 8(시연) |
| nat-probe 후속 | Windows에서 이어간다 | [`../../tools/nat-probe/README.md`](../../tools/nat-probe/README.md) |
| NFR-5 를 두 종류로 나눈 것의 교수 확인 | 확인 대상은 Catch2 승인이 아니라 요구를 제품 의존성과 시험 전용 의존성으로 나눈 것이다 | 제출 전 확인. 결과가 다르면 [ADR 0005](decisions/0005-시험-프레임워크-catch2.md)를 뒤집고 새 ADR 을 쓴다 |

## 구현 때 같이 볼 자리

문서 계약은 정해졌고 코드가 아직 그 자리에 없다. 해당 Phase 에서 같이 쓴다.

| 자리 | 무엇 | 언제 |
|------|------|------|
| `TimerSet` | 세션 키도 제거 함수도 없다. `HELLO` 재전송과 keepalive 를 세션 수만큼 올리면 이름이 충돌한다 | Phase 4 |
| 라우팅 표 | 6칸 고정 배열과 두 단계 조회 ([`protocol.md`](protocol.md) 8.5). 코드에 없다 | Phase 4 |
| `ROSTER` | 타입 `0x08` 송수신과 전용 카운터 셋 (`protocol.md` 5.7) | Phase 4 |
| 로비 | 세션 목록이 비면 로비로 간다 ([`concurrency.md`](concurrency.md) 7장 로비). 지금 `main.cpp` 는 `loop.run()` 한 번이다 | Phase 3 |
| 콘솔 `leave` | 로비로 가는 명령. 지금 어휘에 없다 | Phase 3 |
| `EventLoop` 의 대기 | 루프 수준 동작 보존을 지키는 시험이 없다. `busy_ ? 0 :` 를 지워도 시험 91건이 전부 통과한다. 대기 자체를 주입할 수 있어야 풀리고, 지금은 `platform::wait_any` 가 그 자리다 ([ADR 0010](decisions/0010-플랫폼-이식-이음새.md) 이음새 2). 변이 시험과 크로스 모델 리뷰가 각각 따로 짚었다 | Phase 6. 대기 집합이 바뀌는 시점이다 |
| `host_report` | 호스트의 주기 호출 ([`control_plane.md`](control_plane.md) 4.6) | Phase 3 |

## 문서 부채

| 항목 | 내용 |
|------|------|
| `experiments.md` 없음 | Phase 9 산출물. 게이트가 예외로 두는 유일한 끊긴 링크다 |
| `docgate.py` 개수 검사 미구현 | `CLAUDE.md` "리뷰를 돌릴 때" 가 **셀 수 있는 검사를 스크립트가 맡으라고** 정했다. 대상은 문서가 적은 개수(계약 수, 항목 수, ADR 건수)와 실제 개수의 대조, 그리고 문서 안의 산술이다 |
| `31e4240` 기록 없음 | CMake 스모크 커밋. `commit_history/` 항목 미작성 |
| 케이스 표 이관 | `control_plane.md` 의 케이스 표(2.1, 3.3, 4.4, 4.5, 4.6, 5.1, 6.4, 7.4)는 Phase 3 착수 시 `control-server/tests/` 로 옮긴다 |
| 중복 주장 링크화 미완 | 살아 있는 문서 여덟에서 같은 주장이 두 곳에 있는 자리를 링크로 바꾸는 작업이 중간에 멈춰 있다 (규칙 5). `git stash list` 로 확인하고 `git stash pop` 으로 잇는다. **스타 토폴로지 반영이 같은 문서를 크게 고쳤으므로 그대로 적용되지 않는다.** 충돌을 풀면서 잇는다 |
| `IDLE` 정의의 긴장 | [`protocol.md`](protocol.md) 9.4.1 종료 상태 폐기는 `IDLE` 이면 상대 `peer_id` 도 모른다고 전제하는데, 9.1 상태는 `IDLE` 을 "아직 상대 후보를 받지 못함" 으로 정의한다. `get_peers` 가 `peer_id` 는 주고 후보는 아직 비어 있는 구간에서 둘이 갈린다. 어느 문서도 그 구간에서 세션이 `peer_id` 를 기억하는지 정하지 않았다. 9.1 이나 9.4.1 에 한 줄이 필요하다 |
| Phase 8 제목 | `## Phase 8: Minecraft 검증` 인데 목표가 GUI 시연을 포함한다. 제목 문자열에 기대는 자리는 `roadmap.md` 의 한국어와 영어 제목 줄 둘뿐이고 `#phase-8` 앵커는 없다. 바꿀지는 정하지 않았다 |
| 카운터 이름의 문서-코드 대조 | 이름이 48개다. `protocol.md` 와 [`concurrency.md`](concurrency.md) 에 흩어져 있고 `counters.hpp` 가 그것을 옮겨 적었다. 둘이 어긋나도 알려 주는 것이 없다. `docgate.py` 개수 검사와 같이 붙인다 |

## GUI 후속

[ADR 0007](decisions/0007-gui-핵심-범위-qt.md)이 범위와 등급과 툴킷을 정했다. **연결 상태를
나르는 수단은 정해졌다.** 호스트가 `ROSTER` 로 보낸다(`protocol.md` 5.7). 남은 미결은 둘이다.

| 미결 | 정하는 시점 |
|------|-------------|
| GUI 가 어느 스레드에서 도는가. Qt 이벤트 루프와 `[loop]` 주 스레드 규칙 | [`roadmap.md`](roadmap.md) Phase 8 착수 전 |
| GUI 가 쓰는 기동 입력 경로 | `roadmap.md` Phase 8 착수 전 |

**호스트가 종료할 때 `CLOSE` 최대 4개와 어댑터 정리가 3초 유예에 드는지**는 정하는 것이
아니라 재는 것이다. Phase 8 에서 측정한다. Qt 런타임의 배포 형태도 같은 Phase 에서 확인하고
그 결과를 [`windows-prereq.md`](windows-prereq.md) 4절 배포물 패키징에 적는다.

## 세션을 시작할 때

읽는 순서, 문서 역할, 재발 방지 규칙은 저장소 루트 `CLAUDE.md`에 있다. **여기에 옮겨 적지
않는다.**
