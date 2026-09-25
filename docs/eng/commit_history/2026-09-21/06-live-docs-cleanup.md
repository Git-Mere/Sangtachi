# 2026-09-21 Removing audit traces from the live documents

Dates, `blocker`/`warn` identifiers, and "resolved when" markings were stripped from `spec.md`,
`roadmap.md`, `architecture.md`, `protocol.md`, and `windows-prereq.md`. These five hold the
current specification; they are not audit records. **Only Korean was edited.**

## Changes

| File | What was removed |
|------|-------|
| `protocol.md` | The audit history line in the header. 2 dates in the measurement text |
| `roadmap.md` | The full-audit mention. `blocker 20` in 3 places. The Wintun approval date. The resolved rows in the risk table |
| `spec.md` | The Wintun approval date in `NFR-5` |
| `architecture.md` | The Wintun approval date. The "follow-up 3" reference |
| `windows-prereq.md` | `(blocker N)` and `(warn)` in 13 titles. The source and work-unit lines. The dates in the verification markings. The `plan.md` section number reference |

## Decisions

**1. A date is an index, not evidence.**

Almost every prescription from the reviewer was "restore the dates". It was not accepted. If a date
were the evidence, erasing it would make the statement untrue, and it does not. The evidence is
**what was measured and how**. The measurement text in `protocol.md` is the example. It writes the
3 network pairs that were measured and the observed behaviour right there, so it can be checked
without a date.

This convention was written into `CLAUDE.md`. Live documents hold the current specification only,
and when something was resolved belongs to `commit_history/` and `audit-history/`. `plan.md` is the
exception.

**2. Severity stays, without the audit identifiers.**

Removing `(blocker N)` erased the difference between sections 1-7 and sections 8-13. That is
operational information, not an audit trace. The first group blocks progress if absent; the second
group is a constraint you must know about. One sentence in chapter 0 keeps the titles clean and
keeps the distinction.

**3. The location of the raw measurements is written in chapter 0.**

Removing the dates cuts the trail. One line was added pointing at `commit_history/` and
`tools/nat-probe/records/`.

## 2 defects the cleanup caught as a side effect

Removing the dates exposed overstatement that was already there. This change did not create it.

- `protocol.md` 10.4 asserted that "this reason **was invalidated** by measurement", and then
  admitted two sentences later that "an actual rebinding was never triggered in a test". Rebinding
  was not tested, so it cannot be called invalidated. What the measurement showed is that **it is
  not grounds for separating the two approaches**. Weakened to "weakened"
- The very next paragraph said "all three cases converge automatically", which reversed the claim
  that had just been weakened. Narrowed so that only the first two cases converge, and rebinding
  requires a packet outside the candidate set to actually arrive

The second one is the typical rule 5 failure. **When a claim is weakened, every place that carries
it must be matched in one pass.** Fixing one paragraph and dropping the next one added a round.

The same wording sits in 2 places in `decisions/0002` and 1 place in a commit record, but **those
are records, so they are not changed.**

## Verification

- `grep` finds 0 occurrences of dates, `resolved`, `blocker`, `warn`, `follow-up N`, `full audit`,
  and `design audit` in the five documents
- 13 titles changed, so anchor links were swept. The repository has 0 occurrences of the
  `](...#...)` form. Every other document points by section number, and the numbers did not change
- `docgate.py` itself was not run. Only Korean was edited, so `mirror` and `parity` fail

## Cross-model review

Codex. 4 rounds. Round 1 was split into two lenses: a general check and "overstatement caused by
removing evidence".

| Round | Finding | Verdict |
|:--:|------|------|
| 1 (general) | None | `LGTM` |
| 1 (overstatement) | 9 losses of severity | **Accepted.** Restored as one sentence in chapter 0, without audit identifiers |
| 1 (overstatement) | 2 losses of measurement traceability | **Accepted.** The location of the raw data was written in chapter 0 |
| 1 (overstatement) | 5 approval and measurement claims without a date | **Rejected.** `NFR-5` does not require a date, and the measurement text writes its method on the spot. **The rejection reason was written into `CLAUDE.md` as a convention** |
| 2 | "was invalidated" conflicts with "was never tested" | **Accepted** |
| 3 | The paragraph right after the weakening reverses it with "all three cases converge" | **Accepted** |
| 4 | None | `LGTM - no blockers` |

**The round 1 general check was `LGTM`.** Changing the lens on the same diff produced 16 findings.
One clean pass does not mean it is clean. That holds especially for a change that deletes. What was
removed is visible in the diff; the hole left by the removal is not.
