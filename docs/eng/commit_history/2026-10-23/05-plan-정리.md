# plan.md cleanup, NFR-5 split confirmation closed

## Why

The repository owner found `plan.md` cluttered and asked for a review. They said the three paragraphs under the next steps
need not be in the handoff. They also said the split of NFR-5 into product and test-only dependencies had already been
approved by the professor.

## What changed

| What | Where |
|------|-------|
| Removed: the Phase 8 pre-start decision paragraph (ADR 0030 and `roadmap.md` Phase 8 hold it), the DynamoDB local paragraph (`control-server/README.md`, each test script header), the AWS real values paragraph (`control_plane.md` chapter 7). Also the sentences on finished field tests and on ADR 0029 | `plan.md` next steps |
| Removed two waiting items: NFR-5 professor confirmation (received), nat-probe follow-up (its README has nothing pending, and the nat-probe re-measurement finished on 2026-10-14) | `plan.md` waiting items |
| Removed the Phase 1 two-machine paragraph. `roadmap.md` Phase 1 verification already allows "two processes on one machine" | `plan.md` |
| The implementation cell now reads "Phases 1–6 done, Phase 7 only 6-5". Next steps say Phase 8 follows | `plan.md` |
| The preamble of the places to check during implementation now matches its rows | `plan.md` |
| The NFR-5 paragraph saying the split was unconfirmed now says the professor accepted it | `spec.md` the two kinds of NFR-5 |

ADR 0005 was not edited. Its "not confirmed" part is the record of that time, and the fact of confirmation lives in this
record and `spec.md`.

## Verification

| What | Result |
|------|--------|
| `docgate.py`, `--claims` | `VERDICT: pass` |
| Whether the removed paragraphs' content exists elsewhere | The places in the table above were checked with `grep` |

## Cross-model review

Codex, with the obligation-preservation lens for removed sentences and the mirror lens in one run. It returned
`LGTM - no blockers`.
