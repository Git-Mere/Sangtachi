# CloudWatch 용량 확인

## 왜 했나

`roadmap.md` Phase 3 검증의 배포 묶음에 남은 마지막 항목이다. 서버 역할에 CloudWatch 읽기 권한이
없어 대기였다. 저장소 소유자가 역할에 `cloudwatch:GetMetricStatistics` 를 붙였고 **계속 두기로
정했다.** 운영 확인에 필요할 때 쓴다.

## 결과

구간은 `verify.py --soak 180` 의 부하(UTC 2026-10-07 02:45:34 ~ 02:48:36)를 앞뒤로 넉넉히 감싼 02:43 ~
02:52 다. 인스턴스에서 `aws cloudwatch get-metric-statistics` 로 1분 `Sum` 을 읽었다.

| 분 (UTC) | 읽기 `Sum` | /60 | 쓰기 `Sum` | /60 |
|----------|-----------|-----|-----------|-----|
| 02:45 | 206 | 3.43 | 158 | 2.63 |
| 02:46 | 412 | 6.87 | 132 | 2.20 |
| 02:47 | 425 | 7.08 | 121 | 2.02 |
| 02:48 | 222 | 3.70 | 101 | 1.68 |
| 앞뒤 분 | 0 | 0 | 0 | 0 |

**판정.** 읽기와 쓰기 모두 25 이하다. 통과.

**스로틀.** 데이터점이 하나도 없다. 아래처럼 읽었다.

| 지표 | 차원 | 결과 |
|------|------|------|
| `ThrottledRequests` | `TableName` + `Operation`(`GetItem`, `Query`, `PutItem`, `UpdateItem`, `DeleteItem`, `TransactWriteItems` 각각) | 여섯 모두 데이터점 없음 |
| `ReadThrottleEvents`, `WriteThrottleEvents` | `TableName` | 데이터점 없음 |
| `SuccessfulRequestLatency` 의 `SampleCount` | `TableName` + `Operation=Query` | 02:45~02:48 에 197, 412, 425, 222. 같은 차원의 질의가 맞다는 대조다 |
| `TransactionConflict` | `TableName` | 02:40 에 41, 02:41 에 33, 03:04 에 16. 동시 참가 실험과 되돌리기 전의 검증을 돌린 분에만 있다 |

`TransactionConflict` 처럼 사건 수를 세는 지표는 사건이 있던 분에만 데이터점이 생겼다. 그래서 같은 분에
`Query` 가 수백 건 성공하고 스로틀 지표에 데이터점이 없는 것을 **스로틀 없음**으로 판정했다. 통과.

`ThrottledRequests` 를 처음에는 `TableName` 차원 하나로만 읽어 빈 결과를 받았다. AWS 문서(DynamoDB 개발자
가이드의 지표와 차원 절)가 이 지표의 차원을 `TableName, Operation` 으로 적는다. 그 빈 결과는 근거가 될 수
없어 다시 읽었다. 리뷰가 잡았다.

## 무엇을 바꿨나

| 파일 | 내용 |
|------|------|
| `control_plane.md` 7.6 | IAM 표 아래에 운영 확인용 권한 한 줄. "용량" 절의 "재지 않았다" 를 잰 값으로 |
| `plan.md` | 다음에 할 일에서 CloudWatch 확인을 지웠다 |

## 크로스 모델 리뷰

Codex.

| 라운드 | 렌즈 | 결과 | 처리 |
|--------|------|------|------|
| d10 | record + readability | blocker 1. `ThrottledRequests` 를 `TableName` 하나로 읽은 빈 결과는 근거가 아니다. 이 지표의 차원은 `TableName, Operation` 이다 | 반영. AWS 문서에서 차원을 확인하고 연산별로 다시 읽었다. 대조 지표를 더했다. README 의 절차도 고쳤다 |
| d11 | repair | `none` | - |
