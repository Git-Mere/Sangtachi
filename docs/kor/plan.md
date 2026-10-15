# Plan

**이 파일은 다음 세션의 인수인계다.** 무엇이 남았고 무엇을 먼저 하는지만 적는다. 끝난 일의
경과는 [`commit_history/`](commit_history/README.md)가 갖는다.

## 남은 일이 어디에 있는가

| 축 | 출처 | 지금 |
|----|------|------|
| 구현 | [`roadmap.md`](roadmap.md) | Phase 3 까지 끝났다. Phase 3 산출물은 `field-test` 시험 1 이 확인했다. Phase 4 진행 중이다. 아래 "다음에 할 일" 이 순서다 |
| 대기 | 이 파일 "대기 중인 것" | 두 번째 기기나 측정이 있어야 풀린다 |
| 구현 때 같이 볼 자리 | 이 파일 같은 이름의 절 | `roadmap.md` 에 없는 것만 둔다 |
| 문서 부채 | 이 파일 "문서 부채" 절 | 1건 |

## 다음에 할 일

| 순서 | 무엇 | 왜 |
|:--:|------|-----|
| 1 | `field-test` 시험 1 의 남은 둘을 다시 돈다. 로비 체류(아래 대기)와 D2 의 포트 하나 확인(`a-ports.txt` 가 없었다) | 나머지 항목은 통과했다. 절차와 보고서는 [`field-test/`](../../field-test/README.md) 에 있다 |

**전체 시험이 DynamoDB local 을 요구한다.** `scripts/lobby-check.ps1` 이 제어 서버 하네스를 띄우기 때문이다.
띄우는 명령은 `control-server/README.md` 에 있다.

**AWS 쪽 실제 값은 `deploy/aws.local.md` 에 있다.** 계정, 리전, Elastic IP, 인스턴스, 역할, 테이블
이름이다. 공개 레포라 `.gitignore` 가 막고 이 기기에만 있다. SSH 키도 레포 루트에 ignore 된 채
있다. 확인한 사실과 판정은 `control_plane.md` 7.4 와 7.6 이 갖는다.

## 대기 중인 것

| 무엇 | 막힌 이유 | 풀리는 조건 |
|------|-----------|-------------|
| 방화벽 인바운드 실측 | 상대 피어가 필요하다 | 두 번째 기기 |
| 로비 체류가 NAT 매핑에 무엇을 하는가 | [`concurrency.md`](concurrency.md) 7장 로비가 "단정하지 않는다" 로 적었다. `field-test` 시험 1 에서 체류가 약 50~70초라 이 기기 집 하한 58초를 넘지 못했다. 두 집 NAT 가 포트를 유지하므로 포트 비교로는 가르지 못하고, 볼 것은 긴 체류 뒤에도 다시 `CONNECTED` 되는가다 | 3분 이상 체류로 시험 1-2 를 다시 돈다 |
| 시연 상대 망의 매핑 수명 | 두 집을 쟀다. 이 기기 집 Wi-Fi 하한 58초, 친구 집 Wi-Fi 300초 이상. 최솟값 58초로 [`protocol.md`](protocol.md) 11장 규칙(`간격 × 3 < 최솟값`)을 지킨다. 시연 상대가 이 둘이 아니면 그 망은 아직 모른다 | 시연 상대의 망에서 `tools/nat-lifetime` 을 돌린다. 그때마다 보안 그룹을 그 공인 IP `/32` 로 잠시 연다 |
| 캡처 체크리스트의 보류 C6~C9 | 종료 상태 무응답, 재협상 뒤 `HELLO`, 잠정 경로, 무검증 예산을 와이어에서 보려면 지금 가짜 피어 모드로 만들 수 없는 상대가 필요하다. 논리는 단위 시험이 본다 | 그 상대를 만드는 가짜 피어 모드 ([`tools/phase4-capture`](../../tools/phase4-capture/README.md) 의 C6~C9 절) |
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
| 4.5 `pkt_pos` 의 언더플로 | 첫 수용 패킷의 `position` 이 0 이라 그보다 앞선 번호의 재정렬 수용에서 `position - back` 이 64비트로 감싸 돈다. `tunnel/replay` 는 4.5 공식을 그대로 따르고 시험이 그 거동을 고정했다. 손실 집계가 `baseline` 이하를 배제한다는 4.5 의 주장과 어긋난다. `protocol.md` 4.5 를 먼저 고친다 | Phase 5 손실 집계 착수 전 |
| `is_valid_name` 의 자리 | `control_plane.md` 2.7 검사가 `tunnel/packet.hpp` 에 있어 기동 인자와 제어 평면 코드가 터널 패킷 모듈을 포함한다. 중립 헤더로 옮기고 `packet.hpp` 가 그것을 쓰는 편이 의존 방향이 맞다 | 다음에 그 헤더를 고칠 때 |

## 문서 부채

| 항목 | 내용 |
|------|------|
| `experiments.md` 없음 | Phase 9 산출물. 게이트가 예외로 두는 유일한 끊긴 링크다 |

## 세션을 시작할 때

읽는 순서, 문서 역할, 재발 방지 규칙은 저장소 루트 `CLAUDE.md`에 있다. **여기에 옮겨 적지
않는다.**
