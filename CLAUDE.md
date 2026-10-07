# CLAUDE.md

## 프로젝트

CSP 400 캡스톤. Windows 우선 direct-first P2P 가상 네트워크.

- 클라이언트: C++20 / Winsock2 / Wintun
- 제어 평면: Python / AWS EC2
- 상태 저장소: Amazon DynamoDB
- 클라이언트 실행 대상: Windows 10/11 x64
- 제어 서버 실행 대상: Linux / AWS EC2

셸이 Linux여도 클라이언트가 Linux에서 빌드되는지는 신경 쓰지 않는다.

## 문서 구조

이 레포는 전역 규약의 `docs/spec.md`, `docs/plan.md` 구조를 사용하지 않는다.

```text
docs/kor/    한국어 원본
docs/eng/    영어 미러
```

`plan.md`만 한국어 전용이다.

### 세션 시작

항상 모든 문서를 읽지 않는다.

먼저 읽는다:

1. `docs/kor/plan.md`
2. 현재 작업이 속한 `docs/kor/roadmap.md` Phase

그다음 현재 작업에 필요한 문서만 읽는다.

- 요구사항: `spec.md`
- 시스템 구조: `architecture.md`
- 터널/STUN 프로토콜: `protocol.md`
- 스레드/루프/상태 소유권: `concurrency.md`
- 제어 평면: `control_plane.md`
- 실행 환경/배포 전제: `windows-prereq.md`
- 과거 설계 판단이 필요한 경우: 관련 `decisions/` ADR
- 직전 변경의 맥락이 필요한 경우: 관련 `commit_history/`

현재 진행 상태와 남은 작업의 단일 출처는 `plan.md`다.
과거 기록을 현재 상태의 출처로 사용하지 않는다.

## 문서별 단일 출처

| 문서 | 책임 |
|------|------|
| `spec.md` | 요구사항 FR/NFR/C, 성공 기준 M/T/A |
| `roadmap.md` | Phase별 목표, 작업, 산출물, 검증, 작업 타이밍 |
| `architecture.md` | 시스템 구성, 모듈 분해, 데이터 평면 |
| `protocol.md` | 터널/STUN 와이어 프로토콜 |
| `concurrency.md` | 클라이언트 스레드, 루프, 상태 소유 |
| `control_plane.md` | 제어 평면 API, 상태 전이, DynamoDB, 서버/클라이언트 계약 |
| `windows-prereq.md` | 관리자 권한, 방화벽, Wintun/Qt, EC2 등 실행 전제 |
| `plan.md` | 현재 남은 일, 대기 중인 일, 문서 부채 |

충돌 시:

- 요구사항 -> `spec.md`
- 와이어 포맷, 프로토콜 상태 전이, 프로토콜 타이머 -> `protocol.md`
- 스레드와 루프 구조 -> `concurrency.md`
- 제어 평면 -> `control_plane.md`
- 작업 시점 -> `roadmap.md`
- 현재 진행 상태 -> `plan.md`

문서에 정의되지 않은 값이나 동작을 구현 중 임의로 정하지 않는다.
필요하면 먼저 올바른 단일 출처 문서를 수정한다.

## 기록 문서

다음은 현재 상태가 아니라 역사 기록이다.

```text
audit-history/
commit_history/
decisions/
```

- `audit-history/`: 당시 감사 결과
- `commit_history/`: 당시 변경, 결정, 검증, 리뷰 결과
- `decisions/`: ADR. 선택한 결정과 버린 대안 및 trade-off

기록 문서는 과거 사실이므로 현재 사양이 바뀌어도 다시 쓰지 않는다.
결정을 뒤집으면 기존 ADR을 수정하지 않고 새 ADR을 만든다.
`first_design.md`는 최초 기획 기록이며 수정하지 않는다.

살아 있는 문서는 `audit-history/` 또는 `commit_history/`를 현재 규칙의 출처로 링크하지 않는다.
`decisions/` 링크는 허용한다. 다만 현재 값과 규칙의 최종 출처는 항상 살아 있는 문서다.

## 문서 수정 규칙

한국어가 원본이고 영어가 미러다.

작업 중에는 `docs/kor`를 먼저 수정할 수 있으며 일시적인 mirror/parity 실패를 허용한다.
최종 리뷰, 커밋 또는 푸시 전에 영어 미러를 동기화하고 문서 게이트를 통과시킨다.
`plan.md`는 한국어 전용이며 `docs/eng`에 만들지 않는다.

문서 게이트:

```bash
python tools/docgate/docgate.py
python tools/docgate/docgate.py --claims
```

최종 판정은 `VERDICT: pass`여야 한다.
끊긴 링크 허용 예외는 Phase 9 산출물 `experiments.md`뿐이다.

살아 있는 문서에는 감사 날짜나 감사 식별자를 넣지 않는다. 현재 사양만 기록한다.
과목 메타데이터처럼 문서 자체의 정체성을 나타내는 날짜는 허용한다.

작업 타이밍은 `roadmap.md`가 소유한다.
다른 사양 문서에 "Phase N 전에 정한다" 같은 일정 규칙을 복제하지 않는다.
같은 주장을 여러 살아 있는 문서에 복사하지 않는다. 한 곳을 단일 출처로 두고 나머지는 참조한다.

문서 작업의 상세 검증 절차는 `sangtachi-document-workflow` skill을 사용한다.

## 변경 기록

의미 있는 커밋마다 다음 위치에 기록을 남긴다.

```text
docs/{kor,eng}/commit_history/YYYY-MM-DD/NN-주제.md
```

기록에는 필요한 경우 다음을 포함한다.

- 변경 내용
- 중요한 결정
- 검증 결과
- 크로스 모델 리뷰 결과
- 리뷰 지적을 반영하거나 기각한 이유

설계 결정은 `docs/{kor,eng}/decisions/`의 ADR로 남긴다.

## CLAUDE.md 변경

이 파일은 다음 세션의 행동을 결정하는 영구 규칙이다.

새 규칙을 추가하거나 기존 규칙의 의미를 바꾸기 전에 저장소 소유자의 승인을 받는다.

다음은 승인 없이 할 수 있다.

- 낡은 사실 수정
- 잘못된 경로 수정
- 이미 승인된 현재 상태 갱신
- 명백한 오탈자 수정

한 세션에서 배운 관찰을 바로 영구 규칙으로 만들지 않는다.
승인되지 않은 관찰과 교훈은 우선 `commit_history/`에 기록한다.

## Engineering workflow

비사소한 변경은 가능한 한 다음 흐름을 따른다.

```text
SPEC -> PLAN -> BUILD -> TEST -> REVIEW -> SHIP
```

### 새 기능 또는 큰 동작 변경

사용:

- `spec-driven-development`
- `planning-and-task-breakdown`
- `incremental-implementation`
- `test-driven-development`
- `code-review-and-quality`

기존 승인된 spec/plan이 있으면 새 문서를 만들지 말고 그것을 따른다.

### 버그

사용:

- `debugging-and-error-recovery`
- `test-driven-development`

증상을 없애는 것과 원인을 고치는 것을 구분한다.

### 문서 작업

사용: `sangtachi-document-workflow`

### 리뷰

사용:

- `sangtachi-review-workflow`
- `cross-review` (저장소에 제공되는 경우)
- `code-review-and-quality`

### 테스트, mutation, 측정 코드 또는 위험한 리팩터

사용:

- `sangtachi-validation`
- `test-driven-development`

## 코드 변경 규칙

구현 전에 현재 spec, plan, 관련 소스와 테스트를 확인한다.
승인된 요구사항이나 아키텍처를 구현 편의를 위해 조용히 변경하지 않는다.

새 공유 abstraction 또는 공통 실패 지점을 만들기 전에 해당 경로가 테스트되는지 확인한다.
측정/판정 코드는 특히 보수적으로 다룬다. 크래시하지 않고 잘못된 값을 반환할 수 있기 때문이다.

테스트 케이스가 존재한다는 것만으로 검증됐다고 판단하지 않는다.
중요한 판정 로직은 가능한 경우 mutation 또는 반례로 테스트가 실제 실패하는지 확인한다.

Windows 테스트 실행은 저장소의 canonical test entrypoint를 우선한다.
mutation 또는 hang 가능성이 있는 테스트는 직접 실행 파일을 호출하지 않는다.

```powershell
pwsh -NoProfile -File scripts/test.ps1
```

`ctest`의 timeout을 우회하지 않는다.
상세 검증 절차는 `sangtachi-validation` skill을 따른다.

## 리뷰와 커밋

커밋 전 `cross-review`를 수행한다.
게이트가 staged diff의 SHA-256으로 리뷰 여부를 확인한다.
실제 리뷰 산출물 없이 review marker를 만들지 않는다.

리뷰는 diff만 믿지 않는다.
"없다", "누락됐다" 종류의 지적은 필요하면 HEAD를 직접 확인한다.
셀 수 있는 검사는 스크립트가 한다. 리뷰어에게 단순 개수 계산이나 목록 대조를 맡기지 않는다.

상세 리뷰 렌즈, diff 범위, 반복 리뷰 절차는 `sangtachi-review-workflow` skill을 따른다.

## 위험한 작업

다음은 명시적인 승인을 얻기 전에 실행하지 않는다.

- production 배포
- push 또는 merge
- destructive database 변경
- 데이터 삭제
- secret 또는 credential 변경
- IAM/permission 범위 확대
- 되돌리기 어려운 인프라 변경

권한은 작업에 필요한 최소 범위만 요청한다.

## 현재 상태

현재 상태를 이 파일에 복제하지 않는다.
현재 상태와 남은 작업은 `docs/kor/plan.md`에서 읽는다.
Phase별 범위와 완료 조건은 `docs/kor/roadmap.md`에서 읽는다.
도구의 사용법과 현재 검사 수는 각 도구 README와 실제 테스트 결과를 기준으로 한다.

이 파일에 Phase 진행도, 테스트 개수, mutation 개수 또는 현재 배포 상태를 하드코딩하지 않는다.
