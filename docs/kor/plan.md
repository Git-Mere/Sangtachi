# Plan

**이 파일은 다음 세션의 인수인계다.** 무엇이 남았고 무엇을 먼저 하는지만 적는다. 끝난 일의
경과는 [`commit_history/`](commit_history/README.md)가 갖는다.

## 설계 감사 후속

**2차 감사가 나왔다.** 근거는 `audit-history/design-audit2.md` (blocker 6 / warn 24 / info 24).
제어 평면 묶음은 별도 스키마 문서 작업이 커서 뒤로 뺐고, **그와 무관한 것을 Phase 1 착수
전에 먼저 처리했다.** 그 뒤 스키마 문서(`control_plane.md`)를 쓰면서 제어 평면 묶음을 처리했다.

### 1차: 제어 평면과 무관한 것 — **끝났다**

경과와 지적별 처리는
[`commit_history/2026-09-21-design-audit2-followup1.md`](commit_history/2026-09-21-design-audit2-followup1.md)
에 있다. 크로스 모델 리뷰 **15라운드**(호출 41회, 지적 139건)를 돌렸고 기각은 7건이며
전부 사유를 적었다.

### 2차: 제어 평면 묶음 — **끝났다**

[`control_plane.md`](control_plane.md) 를 신설해 스키마를 확정하면서 함께 처리했다. 항목별 처리는
[`commit_history/2026-09-22-control-plane-doc.md`](commit_history/2026-09-22-control-plane-doc.md)
에 있다. **B-2 는 제어 평면 묶음에서 빠졌다.** 절차 본문은 여전히 `roadmap.md` Phase 4 착수 전
항목이고 아직 쓰지 않았다.

## 남은 일이 어디에 있는가

두 축이고 서로 다른 문서가 출처다. 하나만 보고 전체를 안다고 판단하지 않는다.

| 축 | 출처 | 지금 |
|----|------|------|
| 구현 | [`roadmap.md`](roadmap.md) | Phase 1은 CMake 구성만. Winsock2 래퍼부터 미착수 |
| 감사 후속 | 이 파일 "설계 감사 후속" 절 | 2차 감사 54건. **1차, 2차 모두 완료.** 남은 것은 Phase 착수 전 항목으로 `roadmap.md` 에 있다 |
| 문서 부채 | 이 파일 마지막 절 | 5건 |

## 다음에 할 일

| 순서 | 무엇 | 왜 |
|:--:|------|-----|
| 1 | 구현 Phase 1. Winsock2 래퍼 | 설계 쪽에서 Phase 1을 막는 것이 없다 |
| 2 | Phase 3 착수 전 항목 (`roadmap.md`). Elastic IP·DNS, 자격 증명, 프리 티어 확인, 배포 설정 값 | 계정과 인스턴스가 필요한 일이라 문서로 끝나지 않는다 |

## 대기 중인 것

| 무엇 | 막힌 이유 | 풀리는 조건 |
|------|-----------|-------------|
| 방화벽 인바운드 실측 | 상대 피어가 필요하다 | 두 번째 기기 |
| [`windows-prereq.md`](windows-prereq.md) 실물 검증 | 어댑터가 아직 없다. 지금 상태는 문서화이고 실물 확인이 아니다 | Phase 3(EC2), 6(Wintun), 8(시연) |
| nat-probe 후속 | Windows에서 이어간다 | [`../../tools/nat-probe/README.md`](../../tools/nat-probe/README.md) |

## 문서 부채

| 항목 | 내용 |
|------|------|
| `experiments.md` 없음 | Phase 9 산출물. 게이트가 예외로 두는 유일한 끊긴 링크다 |
| `docgate.py` 개수 검사 미구현 | `CLAUDE.md` "리뷰를 돌릴 때" 가 **셀 수 있는 검사를 `docgate.py` 나 스크립트가 맡으라고** 정했다. 아직 규칙만 있고 검사가 없다. 대상은 문서가 적은 개수(계약 수, 항목 수, ADR 건수)와 실제 개수의 대조다 |
| `31e4240` 기록 없음 | CMake 스모크 커밋. `commit_history/` 항목 미작성 |
| 영어 미러 | 텔레메트리 분리·DynamoDB 변경과 이번 `control_plane.md` 신설(그리고 그에 따른 `architecture.md`, `protocol.md`, `spec.md`, `roadmap.md`, `windows-prereq.md` 변경)을 `docs/eng` 에 아직 반영하지 않았다. `control_plane.md` 는 미러가 생길 때 다른 문서처럼 `> English version:` 줄을 붙인다. 게이트가 `mirror` 와 `parity` 로 막히는 것이 정상이다 |
| 케이스 표 이관 | `control_plane.md` 의 케이스 표 6벌은 Phase 3 착수 시 `control-server/tests/` 로 옮긴다. 그때까지는 문서가 유일한 사본이다 |

## 세션을 시작할 때

읽는 순서, 문서 역할, 재발 방지 규칙은 저장소 루트 `CLAUDE.md`에 있다. **여기에 옮겨 적지
않는다.**
