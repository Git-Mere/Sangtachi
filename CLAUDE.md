# CLAUDE.md

CSP 400 캡스톤. Windows 우선 direct-first P2P 가상 네트워크.
클라이언트는 C++20 / Winsock2 / Wintun, 제어 평면은 Python / AWS EC2.

셸은 Linux지만 **클라이언트의 개발과 실행 대상은 Windows 10/11 x64다.** Linux에서 빌드되는지
걱정하지 않는다. 제어 서버는 Linux/AWS EC2가 대상이다.

## 문서 위치

전역 규약이 말하는 `docs/spec.md`, `docs/plan.md`, `docs/decisions/`, `docs/commit_history/`는
이 레포에 그 경로로 존재하지 않는다. 언어별로 한 단계 아래다.

```
docs/kor/    한국어. 원본
docs/eng/    영어. 미러
```

세션 시작 시 다음 순서로 읽는다.

1. `docs/kor/spec.md`
2. `docs/kor/commit_history/` 최신 1~2건
3. `docs/kor/decisions/`
4. `docs/kor/plan.md`
5. `docs/kor/design-audit.md` 5장 (후속 작업 추적표)

남은 작업은 세 군데로 나뉜다. 하나만 보고 전체를 안다고 판단하지 않는다.

| 무엇 | 어디 |
|------|------|
| 설계 감사 후속 3~6번 | `design-audit.md` 5장. `plan.md`가 작업 단위로 풀어 둔다 |
| Phase 1~9 구현 작업 | `roadmap.md` |
| 문서 부채 | `plan.md` 마지막 "부채" 절 |

## 문서 역할

| 문서 | 역할 |
|------|------|
| `spec.md` | 요구사항 FR/NFR/C, 성공 기준 M/T/A |
| `roadmap.md` | Phase 1~9. 목표, 작업, 산출물, 검증 |
| `architecture.md` | 시스템 구성, 모듈 분해, 동시성 모델(3.2), 데이터 평면 경로 |
| `protocol.md` | **터널/STUN 와이어 프로토콜의 단일 출처.** 상수, 오프셋, 타이머 값, 전이표, 검증 파이프라인. 14장은 제어 평면이 지켜야 할 계약만 적고 REST/JSON 인코딩은 정하지 않는다 |
| `design-audit.md` | 전면 점검 기록, blocker/warn 목록, 후속 계획 |
| `plan.md` | 현재 작업 단위 체크리스트 |
| `first_design.md` | 최초 기획서. **참고 자료이며 확정 사양이 아니다.** 충돌 시 다른 문서가 우선 |

**와이어 포맷, 상태 전이, 프로토콜 타이머 값**이 충돌하면 `protocol.md`가 맞다. 요구사항은
`spec.md`, 스레드와 루프 구조는 `architecture.md` 3.2가 각각 출처이며 `protocol.md`가 그것을
덮지 않는다. 구현 중 문서에 없는 값을 만나면 코드에서 임의로 정하지 말고 해당 문서를 먼저 고친다.

## 규칙

**한국어가 원본이고 영어가 미러다.** 한쪽만 고치지 않는다. 고친 뒤 헤딩 수가 양쪽 같은지 확인한다.

```bash
for f in architecture protocol roadmap spec design-audit plan; do
  echo "$f kor=$(grep -c '^#' docs/kor/$f.md) eng=$(grep -c '^#' docs/eng/$f.md)"
done
```

상대 링크가 실재 경로인지 확인한다. 미해결로 남아도 되는 것은 `experiments.md` 하나뿐이다
(Phase 9 산출물).

**의미 있는 커밋마다 `docs/{kor,eng}/commit_history/YYYY-MM-DD-주제.md`를 남긴다.**
변경, 결정, 검증, 크로스 모델 리뷰 결과(지적별로 반영 또는 기각 사유)를 적는다.

**설계 결정은 `docs/{kor,eng}/decisions/`에 ADR로 남긴다.** 현재 비어 있고, 채워야 할 부채다.

**커밋 전 크로스 모델 리뷰를 돌린다.** `cross-review` 스킬. 게이트 훅이 스테이징된 diff의
sha256으로 확인한다. 리뷰 없이 마커를 찍지 않는다.

## 문서 작업 시

`design-audit.md` 6장에 재발 방지 규칙 4개가 있다. 요약하면,

1. 완성 선언 전에 시나리오 하나를 끝에서 끝까지 추적한다
2. diff 리뷰는 국소 결함만 잡는다. 마일스톤마다 전문 감사를 따로 돌린다
3. 검증 기준은 **정상 구현이 통과하는지부터** 확인한다
4. 지적을 반영할 때 그 수정이 만드는 **새 계약을 함께 정의한다**

3번과 4번을 반복해서 어겼다. 리뷰 라운드는 깨끗한 결과가 나올 때까지 돌리고, 라운드마다
렌즈를 바꾼다. 같은 프롬프트를 반복하면 1차에서 잡힌 것만 다시 확인하게 된다.

## 현재 상태

코드는 `CMakeLists.txt`와 hello 수준 `src/main.cpp`뿐이다. Phase 1은 CMake 구성만 되어 있고
Winsock2 래퍼부터는 미착수다.
후속 1번(protocol.md)과 2번(동시성 모델) 완료. 3~6번 대기. `plan.md` 참고.
