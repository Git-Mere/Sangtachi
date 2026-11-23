# Close Phase 9

## Why

All tasks and verification of `roadmap.md` Phase 9 were done. With [the earlier record of the same day](02-phase-9-실측-장과-그래프.md), the field measurements and analysis are in `experiments.md`. The repository owner decided to close Phase 9.

## Verification items checked

The record that is the evidence for each item of the `roadmap.md` Phase 9 verification.

| Item | Evidence |
|------|------|
| The tunnel holds for 10 minutes with both services down (NFR-3) | [2026-11-17 first field measurement](../2026-11-17/05-phase-9-상대-없는-검증과-첫-실측.md). `experiments.md` 6.5 |
| Room creation, join, and `get_peers` with only the telemetry service stopped (separation 4) | 2026-11-17 first field measurement |
| Metric upload with only the control server stopped (separation 2) | 2026-11-17 first field measurement |
| Case tables of the record check and room check, input limits | Telemetry service tests and mutations in [2026-11-17 telemetry implementation](../2026-11-17/01-phase-9-텔레메트리-구현.md) |
| The two checks of port 8001 | [2026-11-17 deployment check](../2026-11-17/02-phase-9-빌드와-텔레메트리-배포-확인.md), 2026-11-17 first field measurement |
| Load on the instance running both services | [2026-11-17 load check](../2026-11-17/04-phase-9-부하-확인.md) |
| One comparison of CloudWatch with the analysis script | [2026-11-17 analysis and comparison](../2026-11-17/06-phase-9-첫-두-환경-분석과-대조.md) |
| Drop direction and `telemetry_queue_dropped` when the queue is full | Client unit test. Noted in the 2026-11-17 first field measurement |
| At least 10 per environment, stage code and mapping behavior of failures, controlled and real failures counted apart | `experiments.md` 6.1~6.4 |
| The two controlled failure scenarios | 2026-11-17 first field measurement. `experiments.md` 6.4 |
| Success rate by mapping behavior, no NAT type asserted from Binding results | `experiments.md` 6.3 |
| Graphs and limits | `experiments.md` 6.2, 6.6 |

## What changed

| What | Where |
|------|------|
| Phases 1~9 done. Changed the next item to the owner's decision on whether to enter the stretch goals | `plan.md` |
| Keep passwordless `sudo` on `mere-galaxy` for the stretch goals (owner decision) | `plan.md` |

## Verification

| What | Result |
|------|------|
| `scripts/test.ps1 -Slow` | Passed. Unit tests 693/693, `cli-check` 49/49, `e2e-check` 0 failed, `lobby-check` and `session-check` 0 failed 0 not run, RTT baseline (PING median 0.272ms, DATA round-trip median 1.000ms) passed |
| `docgate.py`, `--claims` | `VERDICT: pass` |

## Cross-model review

Codex (`gpt-6.1-sol`). Lenses: evidence and the check table (A), Korean/English parity (B), consistency with `plan.md` and `roadmap.md` (C), public repo exposure (D). The test output summary was handed over as an uncommitted local file. `LGTM - no blockers`.
