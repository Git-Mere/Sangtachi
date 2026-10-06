# 0014. 제어 서버 시험은 pytest 로 돌리고 DynamoDB local 은 Docker 로 띄운다

- 상태: 채택
- 날짜: 2026-10-06
- 관련: [`../spec.md`](../spec.md) NFR-5, [`../control_plane.md`](../control_plane.md) 9장, [`../roadmap.md`](../roadmap.md) Phase 3, [ADR 0004](0004-상태-저장소-dynamodb.md), [ADR 0005](0005-시험-프레임워크-catch2.md)

## 맥락

Phase 3 의 첫 작업은 `control_plane.md` 의 케이스 표를 `control-server/tests/` 로 옮기고 표마다
변이 시험을 붙이는 것이다. 표는 행 하나가 시험 하나다. 행마다 이름을 붙여 돌리고, 변이를 넣었을
때 **어느 행이** 떨어졌는지를 기계가 읽을 수 있어야 한다.

Python 도구 셋(`tools/docgate/`, `tools/platformgate/`, `tools/nat-probe/`)은 표준 라이브러리
`unittest` 위에 있다. 제어 서버도 그렇게 할 수 있었다.

DynamoDB local 은 ADR 0004 와 `roadmap.md` Phase 3 가 로컬 시험 수단으로 정했다. 그러나 어떻게
띄우는지와, 그것이 [`spec.md`](../spec.md) NFR-5 의 승인 대상인지는 적지 않았다.

## 결정

**1. 제어 서버의 시험 실행기는 pytest 다.** 저장소 소유자가 승인했다. 시험 전용 의존성이므로
NFR-5 의 승인 주체가 저장소 소유자다. 버전은 `control-server/requirements-dev.txt` 에 고정한다.

**2. 배포하는 서버 프로세스는 pytest 를 import 하지 않는다.** 제품 의존성은
`control-server/requirements.txt` 에, 시험 전용 의존성은 `requirements-dev.txt` 에 나눠 둔다.
NFR-5 의 판정("배포하는 프로세스가 그것을 import 하지 않는다")이 이 구분 위에 선다.

**3. DynamoDB local 은 Docker 컨테이너로 띄운다.** 저장소 소유자가 정했다. DynamoDB local 도
시험 전용 의존성으로 NFR-5 목록에 올린다. 이미지 태그와 실행 명령은 **실제로 한 번 띄운 뒤**
도구에 넣고 문서는 가리킨다. 아직 실행하지 않은 절차를 문서에 두지 않는다는 `CLAUDE.md` 규칙 6
때문이다.

**4. 변이 시험은 pytest 위의 실행기로 돈다.** `control-server/tests/mutate.py` 가 원본을 임시
폴더에 복사해 한 곳을 고치고, 그 변이가 떨어뜨려야 할 행이 실제로 떨어지는지 pytest 출력에서
읽는다. 판정은 넷(KILLED, PARTIAL, SURVIVED, ERROR)이고 기본값은 판정 불가다.

## 대안

**대안 1. `unittest`.**
버렸다. 저장소 소유자의 판단이다. 의존성이 없다는 장점이 있고 처음의 기본안이었다. 버린 이유는
케이스 표를 행 단위 시험으로 펼치는 매개변수화와, 실패한 행을 노드 id 로 보고하는 출력이다.
`unittest` 의 `subTest` 는 실패를 보고하지만 행을 따로 골라 돌리거나 노드 id 로 지목할 수 없어
변이 실행기가 "어느 행이 떨어졌는가" 를 읽기 어렵다.

**대안 2. DynamoDB local 을 jar 로 띄운다.**
버렸다. 저장소 소유자의 판단이다. Docker 없이 돌지만 Java 런타임이 필요하고, 받은 jar 의
버전을 레포 밖에서 따로 관리해야 한다. Docker 는 이미지 태그로 버전을 고정한다.

## 결과

**얻는 것.**

- 케이스 표의 행이 pytest 매개변수 하나다. 실패한 행이 노드 id 로 나오므로 변이 시험의 판정이
  기계로 남는다
- DynamoDB local 의 버전이 이미지 태그 하나로 고정된다

**치르는 것.**

- NFR-5 의 승인 목록이 여섯이 됐다. 시험 전용이 셋이다
- **로컬 시험에 Docker 데몬이 필요하다.** 이 기기에서 Docker 클라이언트는 있으나 데몬이 떠 있지
  않은 상태를 확인했다. 저장소가 필요한 시험은 데몬을 띄운 뒤에만 돈다. 케이스 표 가운데 순수
  함수로 판정하는 행은 데몬 없이 돈다
- 이미지를 처음 받을 때 네트워크가 필요하다
- DynamoDB local 이 일관성 결함과 트랜잭션 충돌을 드러내지 못하는 한계는 그대로다. ADR 0004
  "치르는 것" 과 `control_plane.md` 7.6 설정과 배포가 적었다. 이 결정이 바꾸지 않는다

**확인하지 못한 것.**

NFR-5 를 제품 의존성과 시험 전용 의존성으로 나눈 것이 과목에 받아들여지는지. ADR 0005 와 같은
질문이고 확인 시점은 `plan.md` 가 갖는다.
