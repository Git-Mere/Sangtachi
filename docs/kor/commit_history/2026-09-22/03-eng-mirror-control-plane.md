# 영어 미러 갱신: control_plane.md 신설분과 밀린 커밋 3건 (2026-09-22)

푸시 지시를 받아 `CLAUDE.md` 규약대로 영어 미러를 먼저 맞췄다. 커밋 `3274ef7`(제어 평면 문서)과
그 앞의 `6b33466`(텔레메트리 분리·DynamoDB), `3f2c2a9`(감사 2차 후속 1차)까지 미러가 밀려 있었다.

## 변경

| 종류 | 파일 |
|------|------|
| 신설 | `eng/control_plane.md`, `eng/decisions/0003-telemetry-service-split.md`, `eng/decisions/0004-state-store-dynamodb.md`, `eng/audit-history/design-audit2.md`, `eng/commit_history/2026-09-21/15-design-audit2-followup1.md`, `eng/commit_history/2026-09-22/01-텔레메트리-분리-dynamodb.md`, `eng/commit_history/2026-09-22/02-control-plane-doc.md` |
| 전체 재번역 | `eng/architecture.md`, `eng/protocol.md`, `eng/roadmap.md`, `eng/spec.md`, `eng/windows-prereq.md`. 패치가 아니라 현재 한국어 전문을 다시 옮겼다 |
| 한국어 | `kor/control_plane.md` 에 `> English version:` 줄 하나 |

**패치가 아니라 전체 재번역인 이유.** 세 커밋 분량이 밀려 있어 diff 를 따라가면 빠뜨린다. 한국어
전문을 옮기고 게이트로 구조를 맞추는 쪽이 확실하다.

## 방법

번역 작업자 5개를 파일 단위로 병렬로 돌렸다. 각 작업자에게 준 제약은 `docgate.py` 의 `parity`
그대로다. 헤딩 구조, 표 행 수, 코드 블록 수, 링크 수 일치. 코드 블록 내용은 바꾸지 않는다.
한국어 파일 이름을 쓰는 ADR 은 영어 이름(`0003-telemetry-service-split.md`,
`0004-state-store-dynamodb.md`)으로 짝을 맞췄고, `commit_history/` 는 이름으로 짝을 맞추므로
`2026-09-22/01-텔레메트리-분리-dynamodb.md` 는 영어 쪽도 같은 이름이다. `plan.md` 링크는 영어 쪽에서
`../kor/plan.md` 를 가리킨다. 그 문서는 한국어만 둔다.

영어 파일에 남은 한글은 셋이다. `windows-prereq.md` 의 실측 출력에 나오는 어댑터 이름 `이더넷 4`,
그리고 기록 파일 안의 한국어 ADR 파일 이름. 둘 다 사실이라 번역하지 않았다.

## 검증

- `docgate.py`: **`VERDICT: pass`.** pairs 44, links 629, findings 0. HEAD(`3274ef7`)는 `mirror` 7,
  `parity` 12 로 실패했다
- 한글 잔존 검사: 영어 파일 전체를 `[가-힣]` 로 훑어 위 셋만 남았다

## 크로스 모델 리뷰

Codex 에 번역 정확성 렌즈 1회. 입력은 `eng/control_plane.md` 전문이고 `architecture.md` 3.2.8·3.5 도
한국어와 대조하게 했다. 의미 반전, 부정 누락, 수치·절 번호 변경, 강도 변화, 문장 생략을 봤다.
결과 **`LGTM`**. 구조 일치는 게이트가 이미 판정했으므로 리뷰어에게 맡기지 않았다.
