# Plan

**이 파일은 다음 세션의 인수인계다.** 무엇이 남았고 무엇을 먼저 하는지만 적는다. 끝난 일의
경과는 [`commit_history/`](commit_history/README.md)가 갖는다.

## 남은 일이 어디에 있는가

| 축 | 출처 | 지금 |
|----|------|------|
| 구현 | [`roadmap.md`](roadmap.md) | Phase 3 까지 끝났다. Phase 4 의 코드는 [`protocol.md`](protocol.md) 15장 구현 적합성 목록과 대조해 빠진 것이 없다. Phase 4 에 남은 것은 캡처 C6~C9(`roadmap.md` Phase 5 작업, 아래 "다음에 할 일" 3)와 두 번째 기기가 있어야 하는 실측(아래 대기)이다. Phase 5 착수 전 결정은 [ADR 0025](decisions/0025-phase-5-착수-전-결정.md) 다. 아래 "다음에 할 일" 이 순서다 |
| 대기 | 이 파일 "대기 중인 것" | 두 번째 기기나 측정이 있어야 풀린다 |
| 구현 때 같이 볼 자리 | 이 파일 같은 이름의 절 | `roadmap.md` 에 없는 것만 둔다 |
| 문서 부채 | 이 파일 "문서 부채" 절 | 1건 |

## 다음에 할 일

Phase 5 다. 범위와 검증은 [`roadmap.md`](roadmap.md) Phase 5 가 갖는다.

| 순서 | 무엇 | 왜 |
|:--:|------|-----|
| 1 | 4.5 손실 집계와 `session.loss` 로그. 9.5 재협상 3번의 구간 합계(`Session::renegotiate`)를 같이 넣는다 | 나머지 검증 항목이 손실 값을 읽는다 |
| 2 | ASan 시험 빌드와 퍼즈 두 단계 | 퍼즈 판정이 그 빌드를 전제한다 |
| 3 | 하네스 가짜 피어의 C6~C9 모드와 캡처 | 한 기기에서 돌릴 수 있는 Phase 4 의 마지막 항목이다 |
| 4 | 장시간 시험 스크립트(제어 평면 중단 10분, 1시간 유지). 1시간 시험의 두 집 절차를 [`field-test/`](../../field-test/README.md) 에 더한다 | 오래 걸려 `scripts/test.ps1` 밖에서 돈다 |

**전체 시험이 DynamoDB local 을 요구한다.** `scripts/lobby-check.ps1` 이 제어 서버 하네스를 띄우기 때문이다.
띄우는 명령은 `control-server/README.md` 에 있다.

**AWS 쪽 실제 값은 `deploy/aws.local.md` 에 있다.** 계정, 리전, Elastic IP, 인스턴스, 역할, 테이블
이름이다. 공개 레포라 `.gitignore` 가 막고 이 기기에만 있다. SSH 키도 레포 루트에 ignore 된 채
있다. 확인한 사실과 판정은 `control_plane.md` 7.4 와 7.6 이 갖는다.

## 대기 중인 것

| 무엇 | 막힌 이유 | 풀리는 조건 |
|------|-----------|-------------|
| 방화벽 인바운드 실측 | 상대 피어가 필요하다 | 두 번째 기기 |
| `field-test` 시험 1 의 D2 포트 하나 확인 | 그 실행에 `a-ports.txt` 가 없었다. 나머지 항목은 통과했다 | 두 번째 기기로 시험 1 을 다시 돈다. 아래 로비 체류와 같이 돈다 |
| Phase 5 1시간 시험의 두 집 관측 | 같은 기기에는 NAT 가 없다 | 두 번째 기기 |
| 로비 체류가 NAT 매핑에 무엇을 하는가 | [`concurrency.md`](concurrency.md) 7장 로비가 "단정하지 않는다" 로 적었다. `field-test` 시험 1 에서 체류가 약 50~70초라 이 기기 집 하한 58초를 넘지 못했다. 두 집 NAT 가 포트를 유지하므로 포트 비교로는 가르지 못하고, 볼 것은 긴 체류 뒤에도 다시 `CONNECTED` 되는가다 | 3분 이상 체류로 시험 1-2 를 다시 돈다 |
| 시연 상대 망의 매핑 수명 | 두 집을 쟀다. 이 기기 집 Wi-Fi 하한 58초, 친구 집 Wi-Fi 300초 이상. 최솟값 58초로 [`protocol.md`](protocol.md) 11장 규칙(`간격 × 3 < 최솟값`)을 지킨다. 시연 상대가 이 둘이 아니면 그 망은 아직 모른다 | 시연 상대의 망에서 `tools/nat-lifetime` 을 돌린다. 그때마다 보안 그룹을 그 공인 IP `/32` 로 잠시 연다 |
| nat-probe 후속 | Windows에서 이어간다 | [`../../tools/nat-probe/README.md`](../../tools/nat-probe/README.md) |
| [`windows-prereq.md`](windows-prereq.md) 실물 검증 | Wintun 어댑터가 아직 없다 | Phase 6(Wintun), 8(시연) |
| NFR-5 를 두 종류로 나눈 것의 교수 확인 | 확인 대상은 Catch2 승인이 아니라 요구를 제품 의존성과 시험 전용 의존성으로 나눈 것이다 | 제출 전 확인. 결과가 다르면 [ADR 0005](decisions/0005-시험-프레임워크-catch2.md)를 뒤집고 새 ADR 을 쓴다 |

Phase 1 의 두 머신 실측은 따로 기다리지 않는다. `roadmap.md` Phase 1 검증은 한 기기의 두 프로세스도
받고, 그것은 `scripts/e2e-check.ps1` 로 돌렸다. 실제 두 기기 사이의 송수신은 Phase 4 검증의 "같은 LAN 의
두 PC 에서도 `CONNECTED` 에 도달한다" 가 대신 본다.

## 구현 때 같이 볼 자리

문서 계약은 정해졌고 코드가 아직 그 자리에 없다. `roadmap.md` 에 이미 걸린 작업은 여기에 다시
적지 않는다.

| 자리 | 무엇 | 언제 |
|------|------|------|
| `tests/mutate.py` 의 시간 초과 | 상한에 걸리면 pytest 프로세스만 죽인다. 그 시험이 띄운 서버 프로세스가 남을 수 있다. 손자까지 거두려면 Windows Job Object 가 필요하다 | 변이 실행기를 다시 고칠 때 |
| 자기 Wintun 식별자 | 후보 수집이 `protocol.md` 10.1 제외 단계의 자기 Wintun 식별자를 비워 넘긴다(`client/src/control/runner.cpp`). 판정 함수와 시험은 있다. 어댑터가 생기면 그 값을 잇는다 | Phase 6 어댑터 생성 |
| 15장 대조에서 시험이 비어 있는 자리 | 코드는 있고 그것을 직접 보는 시험이 없다. `FAILED` 상태에서 받은 패킷의 `drop_terminal_state`, `CLOSE` 를 받은 쪽의 라우팅 칸 제거, 새 시도의 epoch 가 이전과 다름, 라우팅 케이스 표 4행(`ROSTER` 를 받은 뒤의 다른 플레이어 가상 IP), `pending_pings` 상한. 마지막은 PING 간격과 항목 수명이 둘 다 5초라 지금 구성으로는 닿지 않는다 | Phase 5 의 9.4 상태 머신 시험 |
| `is_valid_name` 의 자리 | `control_plane.md` 2.7 검사가 `tunnel/packet.hpp` 에 있어 기동 인자와 제어 평면 코드가 터널 패킷 모듈을 포함한다. 중립 헤더로 옮기고 `packet.hpp` 가 그것을 쓰는 편이 의존 방향이 맞다 | 다음에 그 헤더를 고칠 때 |

## 문서 부채

| 항목 | 내용 |
|------|------|
| `experiments.md` 없음 | Phase 9 산출물. 게이트가 예외로 두는 유일한 끊긴 링크다 |

## 세션을 시작할 때

읽는 순서, 문서 역할, 재발 방지 규칙은 저장소 루트 `CLAUDE.md`에 있다. **여기에 옮겨 적지
않는다.**
