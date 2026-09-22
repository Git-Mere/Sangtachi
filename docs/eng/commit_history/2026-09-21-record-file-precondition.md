# 2026-09-21 The record file decision becomes a Phase 4 precondition

The decision about the local record file format was taken out of the "what to do next" list in
`plan.md` and moved into the pre-start task list of Phase 4 in `roadmap.md`. It is a precondition
of Phase 4, not an immediate task.

## Changes

| File | Detail |
|------|--------|
| `roadmap.md` (kor/eng) | A pre-start item was added to the Phase 4 tasks. Until the path and the line format are fixed, M-6 cannot be judged |
| `plan.md` | Item 1 of "what to do next" was removed. Implementation Phase 1 is the only one left |
| `plan.md` | The documentation debt count was corrected from 3 to 2 |

## Decision

**Where the decision would live was settled before removing it.**

Deleting it from `plan.md` alone would leave "decide before Phase 4 starts" only in chapter 9 of
`architecture.md` and in M-6 of `spec.md`. Neither is a document someone opens when starting
Phase 4. So it moved into the pre-start items of Phase 4 in `roadmap.md`, right after the
`tools/nat-probe` re-measurement.

That is recurrence-prevention rule 4. A change defines the new contract it creates.

## Fixed along the way

`plan.md` stated the documentation debt as **3 items**. The table holds 2. The previous commit
removed the `docs/eng` lag row and left the summary number alone. **A summary and a table
disagreed inside one file.**

## Verification

- `python tools/docgate/docgate.py`: `VERDICT: pass`
- The counts stated inside `plan.md` were checked against its own tables again

## Cross-model review

Codex, 1 round. The lens was "is anything lost by the removal, do Korean and English say the same
thing, and do the numbers inside the file match its own tables". `LGTM - no blockers`.
