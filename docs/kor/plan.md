# Plan

**이 파일은 다음 세션의 인수인계다.** 무엇이 남았고 무엇을 먼저 하는지만 적는다. 끝난 일의
경과는 [`commit_history/`](commit_history/README.md)가 갖는다.

## 남은 일이 어디에 있는가

| 축 | 출처 | 지금 |
|----|------|------|
| 구현 | [`roadmap.md`](roadmap.md) | Phase 5 까지 끝났다. Phase 4·5 에 남은 것은 두 번째 기기가 있어야 하는 실측이다(아래 실측 행). 한 기기의 캡처 C0~C9 와 Phase 5 의 같은 기기 검증(`scripts/soak-check.ps1` 포함)은 통과했다. 아래 "다음에 할 일" 이 순서다 |
| 실측 | [`field-test/required-test.md`](../../field-test/required-test.md) | 사람이 직접 하는 실측. 두 번째 기기나 시연 상대가 있어야 한다 |
| 대기 | 이 파일 "대기 중인 것" | 실측 밖의 대기 |
| 구현 때 같이 볼 자리 | 이 파일 같은 이름의 절 | `roadmap.md` 에 없는 것만 둔다 |
| 문서 부채 | 이 파일 "문서 부채" 절 | 1건 |

## 다음에 할 일

Phase 6 이다. 범위와 검증은 [`roadmap.md`](roadmap.md) Phase 6 이 갖는다.

| 순서 | 무엇 | 왜 |
|:--:|------|-----|
| 1 | 빌드가 Wintun 배포물을 받아 `cmake/Wintun.cmake` 의 기대값과 비교하는 단계 | 착수 전 셋은 [ADR 0026](decisions/0026-phase-6-착수-전-결정.md) 으로 닫혔다. 어댑터 코드가 링크할 DLL 과 헤더가 여기서 나온다 |
| 2 | 어댑터 생성부터 roadmap Phase 6 작업 순서대로 | roadmap 이 범위와 검증을 갖는다 |

**전체 시험이 DynamoDB local 을 요구한다.** `scripts/lobby-check.ps1` 이 제어 서버 하네스를 띄우기 때문이다.
띄우는 명령은 `control-server/README.md` 에 있다.

**AWS 쪽 실제 값은 `deploy/aws.local.md` 에 있다.** 계정, 리전, Elastic IP, 인스턴스, 역할, 테이블
이름이다. 공개 레포라 `.gitignore` 가 막고 이 기기에만 있다. SSH 키도 레포 루트에 ignore 된 채
있다. 확인한 사실과 판정은 `control_plane.md` 7.4 와 7.6 이 갖는다.

## 대기 중인 것

| 무엇 | 막힌 이유 | 풀리는 조건 |
|------|-----------|-------------|
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
| `pending_pings` 상한 | 상한 분기(`protocol.md` 5.5 의 "그래도 가득 차면 새 `PING`을 보내지 않는다")를 밟는 시험이 없다. PING 간격과 항목 수명이 둘 다 5초라 지금 구성으로는 표가 둘 이상 차지 않는다 | 11장 PING 간격이나 5.5 항목 수명을 바꿀 때 |
| 서브넷 검사의 `-AllowShadow` | [`windows-prereq.md`](windows-prereq.md) 7절은 대역이 실제로 비어 있다는 것을 사람이 알 때 쓰라고 이 우회를 둔다. 클라이언트가 그 검사를 부를 때 이 우회를 사용자에게 열지는 정하지 않았다 | Phase 6 에서 서브넷 검사를 클라이언트에 이을 때 |
| `is_valid_name` 의 자리 | `control_plane.md` 2.7 검사가 `tunnel/packet.hpp` 에 있어 기동 인자와 제어 평면 코드가 터널 패킷 모듈을 포함한다. 중립 헤더로 옮기고 `packet.hpp` 가 그것을 쓰는 편이 의존 방향이 맞다 | 다음에 그 헤더를 고칠 때 |

## 문서 부채

| 항목 | 내용 |
|------|------|
| `experiments.md` 없음 | Phase 9 산출물. 게이트가 예외로 두는 유일한 끊긴 링크다 |

## 세션을 시작할 때

읽는 순서, 문서 역할, 재발 방지 규칙은 저장소 루트 `CLAUDE.md`에 있다. **여기에 옮겨 적지
않는다.**
