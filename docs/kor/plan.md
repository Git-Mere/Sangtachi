# Plan

**이 파일은 다음 세션의 인수인계다.** 무엇이 남았고 무엇을 먼저 하는지만 적는다. 끝난 일의
경과는 [`commit_history/`](commit_history/README.md)가 갖는다.

## 남은 일이 어디에 있는가

| 축 | 출처 | 지금 |
|----|------|------|
| 구현 | [`roadmap.md`](roadmap.md) | Phase 7 의 구현까지 끝났다. Phase 6 검증과 Phase 7 의 한 기기 확인은 관리자 셸의 `scripts/adapter-check.ps1` 로 통과했다. Phase 7 의 두 기기 검증이 남았다. Phase 4·5 의 두 집 실측은 2026-10-22 에 끝났다. 아래 "다음에 할 일" 이 순서다 |
| 실측 | [`field-test/required-test.md`](../../field-test/required-test.md) | 사람이 직접 하는 실측. 두 번째 기기나 시연 상대가 있어야 한다 |
| 대기 | 이 파일 "대기 중인 것" | 실측 밖의 대기 |
| 구현 때 같이 볼 자리 | 이 파일 같은 이름의 절 | `roadmap.md` 에 없는 것만 둔다 |
| 문서 부채 | 이 파일 "문서 부채" 절 | 1건 |

## 다음에 할 일

Phase 7 이다. 범위와 검증은 [`roadmap.md`](roadmap.md) Phase 7 이 갖는다. 착수 전 결정은
[ADR 0029](decisions/0029-phase-7-착수-전-결정.md) 로 닫았다.

2026-10-22 두 집 실측(키트 `field-kit-p7b`)에서 시험 1·5 와 시험 6 의 6-0~6-4 가 끝났다. 보고서는 `field-test/2026-10-22/`(로컬)에 있다.
남은 것은 아래 실측이다.

| 순서 | 무엇 | 왜 |
|:--:|------|-----|
| 1 | 6-5 다시 재기 | `roadmap.md` Phase 7 의 바이트 대조. 64 MiB 전송으로 대조하고 A 를 휴대폰 핫스팟에 붙여 잰다([ADR 0032](decisions/0032-바이트-대조를-파일-전송으로-한다.md)). 절차는 `field-test/README.md` 6-5, 순서는 `field-test/required-test.md`. 저장소 소유자가 시점을 알려 준다 |

**Phase 8 착수 전 결정 가운데 GUI 구조와 무관한 것을 먼저 닫고 구현했다.** 제어 서버 주소의 빌드 기본값, 기본 이름,
콘솔의 로비 안내, 영어 문구, 로그 파일, 배포 빌드의 관리자 권한 매니페스트다([ADR 0030](decisions/0030-기동-입력과-콘솔-화면.md)).
`roadmap.md` Phase 8 의 두 항목에 남은 것이 적혀 있다.

**전체 시험이 DynamoDB local 을 요구한다.** `scripts/lobby-check.ps1` 이 제어 서버 하네스를 띄우기 때문이다.
띄우는 명령은 `control-server/README.md` 에 있다.

**AWS 쪽 실제 값은 `deploy/aws.local.md` 에 있다.** 계정, 리전, Elastic IP, 인스턴스, 역할, 테이블
이름이다. 공개 레포라 `.gitignore` 가 막고 이 기기에만 있다. SSH 키도 레포 루트에 ignore 된 채
있다. 확인한 사실과 판정은 `control_plane.md` 7.4 와 7.6 이 갖는다.

## 대기 중인 것

| 무엇 | 막힌 이유 | 풀리는 조건 |
|------|-----------|-------------|
| nat-probe 후속 | Windows에서 이어간다 | [`../../tools/nat-probe/README.md`](../../tools/nat-probe/README.md) |
| [`windows-prereq.md`](windows-prereq.md) 실물 검증의 나머지 | 3절 저장된 주소의 잔존 | Phase 8(시연) |
| NFR-5 를 두 종류로 나눈 것의 교수 확인 | 확인 대상은 Catch2 승인이 아니라 요구를 제품 의존성과 시험 전용 의존성으로 나눈 것이다 | 제출 전 확인. 결과가 다르면 [ADR 0005](decisions/0005-시험-프레임워크-catch2.md)를 뒤집고 새 ADR 을 쓴다 |

Phase 1 의 두 머신 실측은 따로 기다리지 않는다. 실제 두 기기 사이의 송수신은 Phase 4 검증의 "같은 LAN 의
두 PC 에서도 `CONNECTED` 에 도달한다" 가 대신 본다.

## 구현 때 같이 볼 자리

문서 계약은 정해졌고 코드가 아직 그 자리에 없다. `roadmap.md` 에 이미 걸린 작업은 여기에 다시
적지 않는다.

| 자리 | 무엇 | 언제 |
|------|------|------|
| `tests/mutate.py` 의 시간 초과 | 상한에 걸리면 pytest 프로세스만 죽인다. 그 시험이 띄운 서버 프로세스가 남을 수 있다. 손자까지 거두려면 Windows Job Object 가 필요하다 | 변이 실행기를 다시 고칠 때 |
| `pending_pings` 상한 | 상한 분기(`protocol.md` 5.5 의 "그래도 가득 차면 새 `PING`을 보내지 않는다")를 밟는 시험이 없다. PING 간격과 항목 수명이 둘 다 5초라 지금 구성으로는 표가 둘 이상 차지 않는다 | 11장 PING 간격이나 5.5 항목 수명을 바꿀 때 |
| 콘솔 `send` 의 송신 버퍼 | 소켓 송신 버퍼가 차면 `WSAEWOULDBLOCK` 으로 작업을 끝낸다. 576·1452바이트는 한 번에 45~133개만 나간다. 다음 루프 바퀴에 이어 보내면 정한 개수를 채운다. 바이트 대조는 이것에 기대지 않는다(ADR 0032) | 콘솔 `send` 로 개수를 채워야 하는 시험이 생길 때 |
| `is_valid_name` 의 자리 | `control_plane.md` 2.7 검사가 `tunnel/packet.hpp` 에 있어 기동 인자와 제어 평면 코드가 터널 패킷 모듈을 포함한다. 중립 헤더로 옮기고 `packet.hpp` 가 그것을 쓰는 편이 의존 방향이 맞다 | 다음에 그 헤더를 고칠 때 |

## 문서 부채

| 항목 | 내용 |
|------|------|
| `experiments.md` 없음 | Phase 9 산출물. 게이트가 예외로 두는 유일한 끊긴 링크다 |

## 세션을 시작할 때

읽는 순서, 문서 역할, 재발 방지 규칙은 저장소 루트 `CLAUDE.md`에 있다. **여기에 옮겨 적지
않는다.**
