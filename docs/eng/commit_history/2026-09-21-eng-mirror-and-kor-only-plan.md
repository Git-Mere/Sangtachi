# 2026-09-21 The English mirror, and plan.md as Korean only

The English mirror for the 4 commits Korean was ahead by is now written. `plan.md` is kept in
Korean only, and `docgate.py` gained the exception for it. **The document gate passes again.**

## Changes

| What | Detail |
|------|--------|
| All of `docs/eng` | `spec.md`, `roadmap.md`, `protocol.md`, `architecture.md`, `windows-prereq.md` updated. `design-audit.md` moved into `audit-history/`. 4 new commit records |
| `docs/eng/plan.md` | **Deleted.** `plan.md` is kept in Korean only |
| `tools/docgate/docgate.py` | Added the `KOR_ONLY` exception. It lists the documents allowed to have no mirror pair |
| `tools/docgate/test_docgate.py` | 4 cases added. 105 -> 109 |
| Stale references | 12 places pointing at the old chapter numbers of `plan.md` now point at something that exists |

## Decisions

**1. `plan.md` is kept in Korean only.**

It is the handover for the next session, so there is no reason to keep a translation. A late
translation leaves a stale state in two places, which is the very problem the mirror rule exists
to prevent.

**2. The exception means "may be absent", not "may be present".**

`KOR_ONLY` forgives **only a missing** mirror pair. If the same document **appears** in
`docs/eng`, the gate fails. A loose exception invites someone to add a translation later and then
update only one side. The case table was written before the verdict changed (rule 7).

| Case | Expected |
|------|----------|
| On the `KOR_ONLY` list and no English pair | pass |
| Not on the list and no English pair | fail |
| On the list and also present in English | fail |
| A different depth such as `docs/kor/sub/plan.md` | fail. The full path is matched, not the file name |

**3. Stale references move to what is alive. Records stay.**

Rewriting `plan.md` without numbered chapters killed 12 references to "`plan.md` chapter 5":
the body of ADR 0001 in both languages, 5 in `tools/nat-probe/README.md`, and 6 in
`RECORD-TEMPLATE.md`. All now point at design audit follow-up 5 or ADR 0001. **The frozen
measurement records under `tools/nat-probe/records/` and `commit_history/` were left alone.**

## Verification

- `python tools/docgate/docgate.py`: `VERDICT: pass`. 30 pairs, 219 links, 0 findings
- `python -m unittest test_docgate`: 109 tests OK
- Five sub-agents each counted heading order, table rows, code blocks and links for their own
  files using the verdict functions in `docgate.py`, and confirmed the kor/eng match
- The 4 links that pointed at `docs/eng/plan.md` were found with `grep` and fixed

## Three cross-file mismatches the sub-agents caught

They were outside each agent's assigned files, so the agents reported them instead of touching
them. They were fixed here.

- English `spec.md` C-5 and the priority block in `roadmap.md` used different wording. C-5 itself
  requires the two to agree
- Only the English `windows-prereq.md` header said `> 한국어 원본:`. The other six say
  `> Korean version:`
- The pair link in Korean `audit-history/design-audit.md` broke when the English file moved

## Cross-model review

Codex. 3 lenses, 6 rounds.

| Round | Finding | Verdict |
|:--:|---------|---------|
| Translation fidelity | None | `LGTM` |
| Decidability | **blocker.** The 1453-byte `DATA` receive expectation is unreachable. The datagram exceeds `MAX_DATAGRAM` (1472), so `recvfrom` fails with `WSAEMSGSIZE` and the validation pipeline is never reached | **Accepted.** The receive side was taken out of the scope of that row |
| Decidability | C-5 says "identical", but one side is a sentence and the other a code block | **Accepted.** Changed to "the tier contents must match" |
| Decidability | M-6 cannot be decided. The record file format is undecided | **Accepted.** That fact and where it gets decided are now written down |
| Decidability | The Phase 7 "largest count with zero loss" is not a finite procedure | **Accepted.** The search ladder is now explicit |
| Decidability | It is unclear whether both A-2 scenarios are required | **Accepted.** Both are required and each carries its expected code |
| Confirmation | `plan.md` still lists the English mirror as the next task | **Accepted.** Updated on both sides |
| Confirmation | The `plan.md` chapter 5 reference in ADR 0001 is stale | **Accepted.** `grep` then found 11 more and they were fixed too |
| Gate exception | The audit record says `plan.md` does not link it, but it does | **Dismissed.** It is a code span. It is not a link and the gate does not count it |
| Gate exception | The 4 English commit records say "only Korean was edited" while the English files exist (6 findings) | **Dismissed.** That sentence is a fact about those commits. `CLAUDE.md` states that `commit_history/` is not updated. Editing records to match the present turns a record into a copy of the current state |

## What this taught

**The translation lens found nothing, and the decidability lens found a blocker.** The translation
was accurate. The source was wrong, and the English copied it accurately. A mirror review must not
look only at the translation.

**When structure changes, every reference to it has to be found with `grep`.** Rewriting `plan.md`
killed 12 references, and the reviewer saw only the 1 that was inside the diff. A reviewer does not
look outside the diff.
