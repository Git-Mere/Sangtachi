# 2026-09-21 Recording the session's lessons in CLAUDE.md

What this session actually ran into was written into `CLAUDE.md` as rules, so the next session
does not repeat it. `CLAUDE.md` sits outside `docs/`, so the mirror convention and the document
gate do not cover it.

## Two new sections

**`Running a review`**

| Rule | Basis |
|------|-------|
| Use a different lens each time. One `LGTM` from one lens does not mean clean | On the same diff a general pass returned `LGTM` and an "over-claiming after evidence removal" pass returned 16 findings |
| A deleting diff especially so | What was removed is visible in the diff; the hole it leaves is not |
| A reviewer can miss things outside the diff. Check HEAD before judging a "missing" finding | Twice it reported something already fixed in an earlier commit as absent |
| A wrong finding can still point at a gap. Dismissing is not discarding | A non-existent defect was reported in `run_punch`, which had no test exercising it at all |
| Do not add a warning the reviewer invented | Adding a warning that does not exist is the same defect as removing one that does |

**`Working on code`**

| Rule | Basis |
|------|-------|
| Writing a case is not verifying it. Introduce the defect and see the case `FAIL` | The "no catch-up" case passed even with that code deleted. It judged on the wrong axis |
| When a refactor creates a shared point, check whether a test covers it first | The 151 existing cases covered neither measurement function |
| Measurement code fails quietly | It does not crash; it returns a wrong value. The lens for a refactor review is behaviour preservation |

## Added to existing sections

| Section | Rule |
|---------|------|
| Rules | Work timing belongs to `roadmap.md`. A statement of scope is not timing and stays |
| Working on documents | Do not restrict `grep` by file extension |
| Working on documents | Before deleting something, decide where its obligation lands |
| Working on documents | Before shortening, list what must survive |
| Working on documents | A dismissal is written in **two** places: the document and the next review prompt (an extension of the existing rule) |

## Current-state refresh

Three stale facts were corrected: the claim that `docs/eng` lags behind (it is in sync now), the
docgate case count of 105 (it is 109), and the missing per-tool test counts. The three tools are
now a table.

## Two failures in this work itself

**The lessons were broken while being written down.**

- "A reviewer never looks outside the diff" was asserted. That is false. In the README review the
  reviewer did read the scripts and check flags and exit codes against them. **A whole day spent
  weakening over-strong claims, and the new rule over-claimed in exactly the same way.** It was
  weakened to "can miss" with an actionable step
- The rule about recording a dismissal already existed under "working on documents", and a second
  version of the same idea was created under "running a review". **That breaks rule 5, which the
  same file quotes.** The two were merged, stating that the document and the prompt each do a
  different job

## Verification

- `python tools/docgate/docgate.py`: `VERDICT: pass`
- Every factual claim in the new rules was checked against the repository: test counts, tool
  names, paths, and the cited commit records

## Cross-model review

Codex, 2 rounds.

| Round | Finding | Verdict |
|:--:|---------|---------|
| 1 | "A reviewer never looks outside the diff" is too absolute and false | **Accepted** |
| 1 | The rule about writing a dismissal into the prompt duplicates the document rule right above it | **Accepted** |
| 2 | None | `LGTM - no blockers` |
