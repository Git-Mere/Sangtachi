# plan.md cleanup, A-4 removal, loss accounting investigation

## Why

The owner asked for it. `plan.md` had accumulated finished work and progress notes. The reproduction trial criterion (A-4) is removed because the course does not require it. The reason real-environment loss came out as only 0 to 2 per session was also investigated.

## What changed

| What | Where |
|------|------|
| Only next work is left. The rows for finished field measurement, CloudWatch, running the analysis tool, and the reproduction trial were removed. Past builds and progress were removed from the test device section | `plan.md` |
| A rule to update `plan.md` right after finishing each task and to keep only next work there. Approved by the owner | `CLAUDE.md` |
| Analysis criterion A-4 and its judgment method were removed. The A range in the priority table and the requirement tracing table is now A-1 to A-3 | `spec.md` |
| The reproduction trial line was removed from Phase 9 verification. The requirement tracing table was aligned | `roadmap.md` |
| How to run `--save` in CloudShell was moved out of `plan.md` | `tools/tm-analysis/README.md` |
| Loss accounting investigation | [Issue 0009](../../issue_history/0009-닿지-않는-후보로-보낸-hello-가-손실로-세졌다.md) |

No ADR is written for the A-4 removal. The owner said it is a criterion the course does not require, so not a decision to record.

## Loss accounting investigation

The campus network vs home network setup was run again with captures on both sides. On the second attempt the host reported a loss of 1. The missing number was a `HELLO` the player sent to the host's private candidate, sent after the host had already started accounting. Details are in issue 0009. Whether to fix it is decided next session.

The three captured runs remain in the telemetry as attempts outside any batch.

## Verification

| What | Result |
|------|------|
| `docgate.py` | `VERDICT: pass` |
| Remaining references to A-4 and the reproduction trial | None in living documents (`grep`) |

## Cross-model review

Codex (`gpt-6.1-sol`). Lenses: captures and records (A), Korean-English agreement (B), remaining dependence on A-4 and the new `plan.md` rule (C), public repo exposure (D).

| Finding | Handling |
|------|------|
| [C] warn. "Phase 1~8 은 끝났다" in `plan.md` is finished work | Applied. Only the remaining Phase 9 is written |

After the fix, the final staged diff was run again with the same lenses. `LGTM - no blockers`.
