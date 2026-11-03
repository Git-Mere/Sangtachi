# Phase 8 을 닫는다

## 왜 했나

`roadmap.md` Phase 8 의 작업과 검증을 모두 돌았다. 저장소 소유자가 시연(2026-11-05) 전에 Phase 8 을 닫기로 했다. 시연은 Phase 8 목표의
문장이지만 검증 항목이 아니고, 같은 시연 망의 리허설이 이미 통과했다([`experiments.md`](../../experiments.md) 3.4).

## 무엇을 바꿨나

| 무엇 | 자리 |
|------|------|
| Phase 1~8 끝. 다음은 Phase 9. 시연과 `mere-galaxy` 정리를 Phase 9 전의 일정으로 남겼다 | `plan.md` |
| 대기 행 "`windows-prereq.md` 실물 검증의 나머지(3절 저장된 주소의 잔존)" 를 지웠다 | `plan.md` |

## 결정

- **시연 전에 닫는다.** 소유자 결정이다
- **`windows-prereq.md` 3절의 저장된 주소 잔존은 관측하지 않은 채 둔다.** 소유자 결정이다. 클라이언트는 기동 때 그 GUID 의 IPv4 주소를
  모두 지우지만 몇 개를 지웠는지 로그에 남기지 않는다(`client/src/platform/win32/tun.cpp` 의 `delete_all_ipv4_addresses`). 그래서 시연을 해도
  로그로는 관측할 수 없다. 3절은 이미 "부분 실측" 이고 그 경로를 관측하지 못했다고 적는다. 그 문장을 그대로 둔다. 일부러 주소를 남겨
  재는 안은 관리자 작업과 새 절차가 필요해 버렸다

## 검증

| 무엇 | 결과 |
|------|------|
| `scripts/test.ps1 -Slow` | 통과. 단위 시험 667/667, 기록 파일 시험 43/43, `cli-check`·`e2e-check`·`lobby-check`·`session-check` 실패 0, RTT 기준선(PING 중앙값 0.318ms, DATA 왕복 중앙값 1.000ms) 통과. 첫 실행은 DynamoDB local 이 떠 있지 않아 `lobby-check` 의 하네스가 시작하지 못했다. 띄운 뒤 처음부터 다시 돌렸다 |
| `docgate.py`, `--claims` | `VERDICT: pass` |

## 크로스 모델 리뷰

Codex(`gpt-6.1-sol`). 근거·계약(A)과 한영·기록 일관성(B) 렌즈를 한 번에 돌렸다. 지적이 없었다. 이 절을 채운 뒤 전체 staged 차이를 같은 렌즈로 다시 돌렸다.
