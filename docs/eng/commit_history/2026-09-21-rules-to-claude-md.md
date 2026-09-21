# 2026-09-21 Moving the recurrence rules into CLAUDE.md

The source text of the 7 recurrence-prevention rules, held in chapter 6 of
`audit-history/design-audit.md`, moved to `CLAUDE.md` at the repository root. `CLAUDE.md` no longer
references the audit record, and the document role table now carries a role per folder. **Only
Korean was edited.**

## Changes

| File | Content |
|------|------|
| `CLAUDE.md` | All 7 rules in full (index -> source text). Audit record reference removed. New folder role table. Reading order and current status updated. 1 new rule added |
| `audit-history/design-audit.md` | The header note now says "the source text of the rules is `CLAUDE.md`". The link relationship description corrected |

## Decisions

**1. The source text of the rules lives in `CLAUDE.md`.**

Rules are read in order to follow them; an audit record holds how those rules were obtained. The
two have different readers. If the audit record is the source text, every session start has to open
the archive. Chapter 6 of `audit-history/` stays as it is. It falls under the historical-record
exception in rule 5.

**2. The audit cycle in rule 2 changes to a per-Phase cycle.**

The source text said "per milestone". The milestone split was removed the same day (commit
`44d9225`), so it became an instruction that cannot be executed. Per Phase, the cycle goes from
three times to as many times as there are Phases. **That cost is written inside the rule.** In
exchange the scope narrows to the documents and code that Phase touched, and a full-scope audit
runs only when the priority tiers in `roadmap.md` change.

**3. Links into the record folders are split into two kinds.**

`CLAUDE.md` may cite `commit_history/` and `decisions/` as evidence. Which commit a rule came from
exists only in those records. Two things are blocked: a live document linking to a record as
evidence, and `CLAUDE.md` pointing at `audit-history/`.

**4. `decisions/` joins the historical-record exception in rule 5, but with a condition.**

What an ADR holds uniquely is **the alternative that was dropped and why**, not the source of the
decision itself. Adding it to the exception without a condition invites the misreading "it is
written in the ADR, so the ADR is the source".

## The new rule

**When a review finding is rejected, write that decision inside the document.** Writing it only in
the commit record does not help: the next reviewer does not read it, so the same finding comes back
every round. The evidence is round 2 of `2026-09-21-followup4-spec-logic.md`.

## Verification

- `grep -n "design-audit" CLAUDE.md`: 0 markdown links. What remains is the folder name in the
  folder role table and the folder name in the rule 5 exception list, and neither points at that
  file as a source
- The body of all 7 rules was compared before and after the move. Only rule 2 changed meaning, and
  that fact and its cost are written inside the rule
- `CLAUDE.md` sits outside `docs/`, so `docgate.py` does not inspect it

## Cross-model review

Codex. 4 rounds.

| Round | Finding | Verdict |
|:--:|------|------|
| 1 | `CLAUDE.md` still writes the audit record path | **Accepted.** Path removed, replaced by "it remains in the audit record" |
| 1 | The header note of the audit record still reads as if it links to `CLAUDE.md` | **Accepted.** Both files were matched together. Fixing one side and dropping the other was a rule 5 violation |
| 1 | The evidence for the new rule is a follow-up 4 commit record, so a stale reference remains | **Rejected.** It is a commit record, not an audit record, and the evidence for the rule exists only there. The existing `CLAUDE.md` cites `2026-09-21-docgate.md` the same way. **The rejection reason was written inside `CLAUDE.md`** |
| 2 | Rule 2 changed from "per milestone" to "per Phase", so the meaning was not preserved | **Accepted.** Reverting makes it unexecutable, so the change and its cost are written inside the rule and the scope was narrowed |
| 3 | The historical-record exception in rule 5 omits `decisions/`, which conflicts with the new folder table | **Accepted.** Added, with the condition |
| 4 | None | `LGTM - no blockers` |

**Three rounds in a row found defects in the very rules I moved.** Moving a rule to another file is
not a plain copy. The moment it moves, the surrounding context changes, and the changed context
touches the meaning of the rule. The round 2 finding is the example. The edit that removed the
milestones changed the audit-cycle contract, and that contract was not redefined. Rule 4 was broken
again.
