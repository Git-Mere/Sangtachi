# CloudWatch capacity check

## Why

This is the last item left in the deployment group of the `roadmap.md` Phase 3 verification. It was waiting
because the server role had no CloudWatch read permission. The repository owner attached
`cloudwatch:GetMetricStatistics` to the role and **decided to keep it.** It is used when needed for
operational checks.

## Result

The interval is 02:43 ~ 02:52, which generously wraps the load of `verify.py --soak 180` (UTC 2026-10-07 02:45:34 ~
02:48:36) on both sides. The 1-minute `Sum` was read on the instance with `aws cloudwatch get-metric-statistics`.

| Minute (UTC) | Read `Sum` | /60 | Write `Sum` | /60 |
|--------------|-----------|-----|-------------|-----|
| 02:45 | 206 | 3.43 | 158 | 2.63 |
| 02:46 | 412 | 6.87 | 132 | 2.20 |
| 02:47 | 425 | 7.08 | 121 | 2.02 |
| 02:48 | 222 | 3.70 | 101 | 1.68 |
| Minutes before and after | 0 | 0 | 0 | 0 |

**Verdict.** Both read and write are 25 or less. Passed.

**Throttling.** There is not a single data point. It was read as follows.

| Metric | Dimensions | Result |
|--------|------------|--------|
| `ThrottledRequests` | `TableName` + `Operation` (each of `GetItem`, `Query`, `PutItem`, `UpdateItem`, `DeleteItem`, `TransactWriteItems`) | No data points for all six |
| `ReadThrottleEvents`, `WriteThrottleEvents` | `TableName` | No data points |
| `SampleCount` of `SuccessfulRequestLatency` | `TableName` + `Operation=Query` | 197, 412, 425, 222 at 02:45~02:48. A control showing that a query with the same dimensions is correct |
| `TransactionConflict` | `TableName` | 41 at 02:40, 33 at 02:41, 16 at 03:04. Only in the minutes when the concurrent join experiment and the verification before the rollback were run |

For metrics that count events, like `TransactionConflict`, data points appeared only in minutes when there
were events. So hundreds of successful `Query` calls in the same minutes with no data points in the
throttle metrics was judged as **no throttling**. Passed.

`ThrottledRequests` was first read with only the `TableName` dimension and gave an empty result. The AWS
documentation (the metrics and dimensions section of the DynamoDB developer guide) lists the dimensions of
this metric as `TableName, Operation`. That empty result could not be evidence, so it was read again. The
review caught this.

## What changed

| File | Content |
|------|---------|
| `control_plane.md` 7.6 | One line under the IAM table for the operational check permission. "Not measured" in the "Capacity" section changed to the measured values |
| `plan.md` | Removed the CloudWatch check from "Next to do" |

## Cross-model review

Codex.

| Round | Lenses | Result | Handling |
|-------|--------|--------|----------|
| d10 | record + readability | blocker 1. An empty result from reading `ThrottledRequests` with only `TableName` is not evidence. The dimensions of this metric are `TableName, Operation` | Accepted. Checked the dimensions in the AWS documentation and read again per operation. Added control metrics. Also fixed the procedure in the README |
| d11 | repair | `none` | - |
