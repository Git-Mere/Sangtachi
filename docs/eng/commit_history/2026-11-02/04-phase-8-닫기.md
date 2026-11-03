# Close Phase 8

## Why

All tasks and verification of `roadmap.md` Phase 8 have been run. The repository owner decided to close Phase 8 before the demo (2026-11-05). The
demo is in the sentence of the Phase 8 goal but is not a verification item, and the rehearsal on the same demo networks has already passed
([`experiments.md`](../../experiments.md) 3.4).

## What changed

| What | Where |
|------|-------|
| Phases 1–8 done. Next is Phase 9. The demo and the `mere-galaxy` cleanup are left as the schedule before Phase 9 | `plan.md` |
| Removed the waiting row "rest of the hands-on verification of `windows-prereq.md` (section 3 leftover stored addresses)" | `plan.md` |

## Decisions

- **Close before the demo.** This is the owner's decision
- **The leftover stored addresses of `windows-prereq.md` section 3 stay unobserved.** This is the owner's decision. The client deletes all IPv4
  addresses of that GUID at startup but does not log how many it deleted (`delete_all_ipv4_addresses` in `client/src/platform/win32/tun.cpp`). So
  the demo cannot observe it through the logs either. Section 3 is already "partly measured" and says that path was not observed. That sentence
  stays as is. Leaving addresses on purpose to measure it was rejected because it needs administrator work and a new procedure

## Verification

| What | Result |
|------|--------|
| `scripts/test.ps1 -Slow` | Pass. Unit tests 667/667, record-file tests 43/43, `cli-check`, `e2e-check`, `lobby-check`, and `session-check` with 0 failures, RTT baseline (PING median 0.318ms, DATA echo median 1.000ms) passed. The first run could not start the `lobby-check` harness because DynamoDB local was not running. It was started and the whole run was repeated |
| `docgate.py`, `--claims` | `VERDICT: pass` |

## Cross-model review

Codex (`gpt-6.1-sol`). The evidence and contract lens (A) and the mirror and record consistency lens (B) were run together. There were no findings. After filling this section, the whole staged diff was run again with the same lenses.
