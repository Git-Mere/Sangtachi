# Plan

**이 파일은 다음 세션의 인수인계다.** 무엇이 남았고 무엇을 먼저 하는지만 적는다. 끝난 일의
경과는 [`commit_history/`](commit_history/README.md)가 갖는다.

## 남은 일이 어디에 있는가

| 축 | 출처 | 지금 |
|----|------|------|
| 구현 | [`roadmap.md`](roadmap.md) | Phase 2 까지 끝났다. **Phase 3 은 산출물 확인 하나만 남았다.** 서로 다른 네트워크의 두 클라이언트이고 두 번째 기기를 기다린다("대기 중인 것") |
| 대기 | 이 파일 "대기 중인 것" | 두 번째 기기나 측정이 있어야 풀린다 |
| 구현 때 같이 볼 자리 | 이 파일 같은 이름의 절 | `roadmap.md` 에 없는 것만 둔다 |
| 문서 부채 | 이 파일 "문서 부채" 절 | 10건 |

## 다음에 할 일

| 순서 | 무엇 | 왜 |
|:--:|------|-----|
| 1 | Phase 4 착수 전 결정 다섯 | `roadmap.md` Phase 4 작업 머리 |

**전체 시험이 DynamoDB local 을 요구한다.** `scripts/lobby-check.ps1` 이 제어 서버 하네스를 띄우기 때문이다.
띄우는 명령은 `control-server/README.md` 에 있다.

**AWS 쪽 실제 값은 `deploy/aws.local.md` 에 있다.** 계정, 리전, Elastic IP, 인스턴스, 역할, 테이블
이름이다. 공개 레포라 `.gitignore` 가 막고 이 기기에만 있다. SSH 키도 레포 루트에 ignore 된 채
있다. 확인한 사실과 판정은 `control_plane.md` 7.4 와 7.6 이 갖는다.

## 대기 중인 것

| 무엇 | 막힌 이유 | 풀리는 조건 |
|------|-----------|-------------|
| 방화벽 인바운드 실측 | 상대 피어가 필요하다 | 두 번째 기기 |
| Phase 1 의 두 머신 실측 | 한 기기의 여러 프로세스로는 돌렸다 | 두 번째 기기. 절차는 `scripts/e2e-check.ps1` 과 같고 `--peer` 만 상대 주소로 바꾼다. **그대로는 안 된다.** 로컬 포트를 고정하는 인자가 없어 양쪽이 기동 시점에 상대 포트를 같이 알 수 없고, 요청하지 않은 인바운드는 [`windows-prereq.md`](windows-prereq.md) 2절대로 막힌다. 3프로세스 사슬과 임시 인바운드 허용 규칙이 함께 필요하다 |
| 로비 체류가 NAT 매핑에 무엇을 하는가 | 측정한 적이 없다. [`concurrency.md`](concurrency.md) 7장 로비가 "단정하지 않는다" 로 적었다 | Phase 4 의 `tools/nat-probe` 재측정과 같이 본다 |
| nat-probe 후속 | Windows에서 이어간다 | [`../../tools/nat-probe/README.md`](../../tools/nat-probe/README.md) |
| [`windows-prereq.md`](windows-prereq.md) 실물 검증 | Wintun 어댑터가 아직 없다 | Phase 6(Wintun), 8(시연) |
| 서로 다른 네트워크의 두 클라이언트 | `roadmap.md` Phase 3 산출물이다. 배포된 서버로 한 NAT 뒤의 두 프로세스는 돌렸다 | 두 번째 기기 |
| NFR-5 를 두 종류로 나눈 것의 교수 확인 | 확인 대상은 Catch2 승인이 아니라 요구를 제품 의존성과 시험 전용 의존성으로 나눈 것이다 | 제출 전 확인. 결과가 다르면 [ADR 0005](decisions/0005-시험-프레임워크-catch2.md)를 뒤집고 새 ADR 을 쓴다 |

## 구현 때 같이 볼 자리

문서 계약은 정해졌고 코드가 아직 그 자리에 없다. `roadmap.md` 에 이미 걸린 작업은 여기에 다시
적지 않는다.

| 자리 | 무엇 | 언제 |
|------|------|------|
| `tests/mutate.py` 의 시간 초과 | 상한에 걸리면 pytest 프로세스만 죽인다. 그 시험이 띄운 서버 프로세스가 남을 수 있다. 손자까지 거두려면 Windows Job Object 가 필요하다 | 변이 실행기를 다시 고칠 때 |
| `EventLoop` 의 대기 | 루프 수준 동작 보존을 지키는 시험이 없다. `busy_ ? 0 :` 를 지워도 시험 전량이 통과한다. 대기 자체를 주입할 수 있어야 풀리고, 지금은 `platform::wait_any` 가 그 자리다 ([ADR 0010](decisions/0010-플랫폼-이식-이음새.md) 이음새 2). 변이 시험과 크로스 모델 리뷰가 각각 따로 짚었다 | Phase 6. 대기 집합이 바뀌는 시점이다 |

## 문서 부채

| 항목 | 내용 |
|------|------|
| `experiments.md` 없음 | Phase 9 산출물. 게이트가 예외로 두는 유일한 끊긴 링크다 |
| `docgate.py` 개수 검사 미구현 | `CLAUDE.md` "리뷰를 돌릴 때" 가 **셀 수 있는 검사를 스크립트가 맡으라고** 정했다. 대상은 문서가 적은 개수(계약 수, 항목 수, ADR 건수)와 실제 개수의 대조, 그리고 문서 안의 산술이다 |
| `31e4240` 기록 없음 | CMake 스모크 커밋. `commit_history/` 항목 미작성 |
| 중복 주장 링크화 미완 | 살아 있는 문서 여덟에서 같은 주장이 두 곳에 있는 자리를 링크로 바꾸는 작업이 중간에 멈춰 있다 (규칙 5). `git stash list` 로 확인하고 `git stash pop` 으로 잇는다. **스타 토폴로지 반영이 같은 문서를 크게 고쳤으므로 그대로 적용되지 않는다.** 충돌을 풀면서 잇는다 |
| `IDLE` 정의의 긴장 | [`protocol.md`](protocol.md) 9.4.1 종료 상태 폐기는 `IDLE` 이면 상대 `peer_id` 도 모른다고 전제하는데, 9.1 상태는 `IDLE` 을 "아직 상대 후보를 받지 못함" 으로 정의한다. `get_peers` 가 `peer_id` 는 주고 후보는 아직 비어 있는 구간에서 둘이 갈린다. 어느 문서도 그 구간에서 세션이 `peer_id` 를 기억하는지 정하지 않았다. 9.1 이나 9.4.1 에 한 줄이 필요하다 |
| 종료 세션 객체의 수명 | [`protocol.md`](protocol.md) 5.6 `CLOSE` 는 받은 쪽 세션을 `CLOSED` 로 남겨 늦은 패킷을 `drop_terminal_state` 로 세게 한다. [`concurrency.md`](concurrency.md) 7장 종료는 끝난 세션을 목록에서 지운다. 같은 장 로비 표의 `drop_unknown_peer` 는 뒤쪽을 전제한다. Phase 4 의 세션 구현 전에 정한다 |
| Phase 8 제목 | `## Phase 8: Minecraft 검증` 인데 목표가 GUI 시연을 포함한다. 제목 문자열에 기대는 자리는 `roadmap.md` 의 한국어와 영어 제목 줄 둘뿐이고 `#phase-8` 앵커는 없다. 바꿀지는 정하지 않았다 |
| 후보 위생의 대역 | 서버는 [`protocol.md`](protocol.md) 10.1 목록 그대로 거부한다. `0.0.0.0/8` 의 나머지와 `240.0.0.0/4` 는 저장한다(`control_plane.md` 4.4). 그 목록을 허용 목록으로 바꿀지, 두 대역을 더할지는 정하지 않았다. 바꾸면 10.1 이 먼저이고 클라이언트 쪽 위생도 같이 바뀐다 |
| 헤더 이름 가운데 공백 | `control_plane.md` 3.3 의 헤더 줄 문법은 이름과 콜론 사이의 공백만 거부한다. `X Foo: bar` 처럼 이름 가운데 공백이 있는 줄은 표 밖 헤더로 무시된다. 거부할지는 정하지 않았다 |
| 카운터 이름의 문서-코드 대조 | 이름이 [`protocol.md`](protocol.md) 와 [`concurrency.md`](concurrency.md) 에 흩어져 있고 `counters.hpp` 가 그것을 옮겨 적었다. 둘이 어긋나도 알려 주는 것이 없다. **지금은 문서 쪽이 둘 많다.** `endpoint_learned` 는 Phase 4, `telemetry_upload_failed` 는 Phase 9 에 들어온다. `docgate.py` 개수 검사와 같이 붙인다 |

## 세션을 시작할 때

읽는 순서, 문서 역할, 재발 방지 규칙은 저장소 루트 `CLAUDE.md`에 있다. **여기에 옮겨 적지
않는다.**
