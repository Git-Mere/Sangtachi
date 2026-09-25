# 2026-09-21 작업 타이밍을 roadmap.md 로 모음

`spec.md`, `architecture.md`, `protocol.md` 에서 "Phase X 에서 정한다" 같은 **작업 타이밍
지시**를 걷어냈다. 그 셋은 요구사항, 설계, 와이어 프로토콜의 출처이지 일정표가 아니다.
`tools/nat-probe/NEXT-ON-WINDOWS.md` 를 지우고 그 파일을 가리키던 링크를 정리했다.

## 옮긴 것

| 문서 | 뺀 문구 |
|------|---------|
| `spec.md` M-6 | "Phase 4 착수 전에 정한다" |
| `architecture.md` 9장 | "Phase 4 착수 전에 이 문서에서 먼저 정한다" |
| `architecture.md` 10장 | "이 재배치는 Phase 1의 첫 작업으로 처리한다" |
| `architecture.md` 6장 | "Phase 7에서 실측으로 확정하며" |
| `protocol.md` 11장 | "Phase 4의 매핑 수명 실측" |
| `protocol.md` 12장 | "Phase 7에서 실측해 기본값을 확정하고" |
| `protocol.md` 10.4 | "자동 재시도 여부는 미정이며 Phase 3~5에서 정해야 한다" |

## 남긴 것

**범위 서술은 타이밍이 아니다.** "Phase 1~5에는 어댑터가 없다", "Phase 6 이후는 두
스레드다", "`SIO_UDP_CONNRESET` 를 빠뜨리면 Phase 4에서 터진다" 는 그 단계에서 설계가 어떤
모습인지를 말한다. 언제 무엇을 하라는 지시가 아니므로 그대로 둔다.

**`protocol.md` 15장도 그대로 둔다.** "Phase 4 에서 아래가 전부 코드에 있어야 한다" 는
엄밀히 타이밍이지만, 그 장 자체가 구현 체크리스트의 Phase 4/5 분할이다. 빼면 장의 목적이
사라진다. 판단이 필요한 자리로 남겨 둔다.

## 빼기 전에 갈 곳을 확인했다

| 옮긴 의무 | 이미 있던 자리 |
|-----------|----------------|
| 소스 트리 재배치 | `roadmap.md` Phase 1 작업 |
| MTU 실측 확정 | `roadmap.md` Phase 7 작업과 검증 |
| 매핑 수명 실측 | `roadmap.md` Phase 4 검증 |
| 로컬 기록 파일 형식 | `roadmap.md` Phase 4 착수 전 항목 (앞 커밋에서 추가) |
| **자동 재시도 여부** | **없었다. `roadmap.md` Phase 5 작업에 새로 넣었다** |

재발 방지 규칙 4다. 빼면서 그 의무가 어디에 걸리는지 같이 정한다. 다섯 중 하나는 어디에도
없었고, 확인하지 않고 지웠으면 조용히 사라졌을 것이다.

## `NEXT-ON-WINDOWS.md` 삭제

레포 소유자가 지웠다. 살아 있는 문서 두 곳이 그 파일을 링크하고 있었다.

- `plan.md` "대기 중인 것" 의 nat-probe 행 -> `tools/nat-probe/README.md` 로
- `tools/nat-probe/README.md` 머리의 "먼저 읽는다" 줄 -> 삭제

`commit_history/` 의 언급 11곳은 두었다. 마크다운 링크가 아니라 평문이고, 그 시점의 사실을
적는 기록이다.

**작업 중 판단 착오가 하나 있었다.** `git status` 에서 그 파일의 `D` 를 보고 원인을 묻지
않고 `git checkout` 으로 되살렸다. "내가 지운 적이 없으니 사고" 라고 단정한 것이다. 파일이
사라진 것을 보면 되살리기 전에 왜 사라졌는지부터 확인해야 한다.

## 검증

- `python tools/docgate/docgate.py`: `VERDICT: pass`. 짝 32개, 링크 224개, findings 0건
- 옮긴 의무 다섯 개가 `roadmap.md` 어디에 걸리는지 각각 확인했다
- `grep` 으로 `NEXT-ON-WINDOWS` 참조를 전수로 훑었다

## 크로스 모델 리뷰

Codex 1라운드.

| 지적 | 판정 |
|------|------|
| 로컬 기록 파일 형식의 타이밍을 뺐는데 `roadmap.md` 의 대응 작업이 이 diff 에 없다 (kor/eng 2건) | **기각.** 앞 커밋 `f406be0` 에서 이미 넣었고 `roadmap.md` 160행에 양쪽 다 있다. 리뷰어는 diff 만 보고 HEAD 를 보지 않는다 |

**오늘 같은 기각을 두 번째 한다.** 리뷰어는 diff 밖을 보지 않는다. 앞선 커밋에서 이미 고친
것을 "이 diff 에 없다" 로 지적한다. 프롬프트에 "HEAD 를 확인하라" 를 넣어도 diff 로만
판단하는 경향이 남는다.
