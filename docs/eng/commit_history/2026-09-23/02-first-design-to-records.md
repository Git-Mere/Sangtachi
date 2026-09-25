# Moved first_design.md into the Records and Set the ADR Link Rule

**Target commit:** this commit
**Trigger:** the 3 items design audit 3 follow-up (`de1ef0c`) left to the owner's judgment. The owner decided all three.

## Three Decisions

| # | What | Decision | Reason |
|:-:|------|------|------|
| 1 | `**Term:** CSP 400, Fall 2026` in the `roadmap.md` header | **Leave it** | It is course metadata, not a date used as audit evidence. What the no-date rule for living documents blocks is taking "when it was resolved" as evidence |
| 2 | A living document linking a `decisions/` ADR | **Allow all of it** | Below |
| 3 | The location of `first_design.md` | **Keep it in `decisions/` and manage it as a record** | Below |

## 2. ADR Links Are Allowed

**A full survey found 19 of them.** Five living documents point at four ADRs.

| Document | Count | Which ADR |
|------|:----:|----------|
| `control_plane.md` | 9 | 0003 once, 0004 eight times |
| `roadmap.md` | 6 | 0001 twice, 0004 four times |
| `windows-prereq.md` | 2 | 0002 twice |
| `architecture.md` | 1 | 0004 |
| `protocol.md` | 1 | 0002 |

There were two kinds. Five were **evidence pointers** ("the dropped alternatives and their cost
are there") and fourteen were **fact citations** ("ADR 0004 decision 4. provisioned", "we could
not confirm whether the free tier is permanent"). The latter is what the `CLAUDE.md` line "a
living document must not link a record as evidence" catches literally.

**Three branches were put to the owner and the owner chose to allow all of it.** The other two
were remove everything (copy the ADR facts into the living documents and cut the links) and a
middle path (keep the links but do not use them as the source of facts).

The link rule in `CLAUDE.md` was changed. What it blocks is now `audit-history/` and
`commit_history/`. **The scope of the permission was nailed down in one line. The source of
values and rules is still the living document, and when it disagrees with an ADR the living
document is right.** Without that, "allow all of it" reads as "the ADR is the specification".

**Because this changed a rule, owner approval came first.** That is the procedure `CLAUDE.md`
requires.

## 3. first_design.md Is a Record

The owner had already moved it to `docs/kor/decisions/` in the working tree, and that direction
was confirmed. What this commit does is finish that move.

| What | Before | After |
|------|-----|-----|
| Location | `docs/{kor,eng}/first_design.md` | `docs/{kor,eng}/decisions/first_design.md` |
| Front matter of the living documents | `spec.md`, `roadmap.md` and `architecture.md` linked it as `**Reference document:**` | **The links were cut.** Three lines were deleted |
| Document role table (`CLAUDE.md`) | One row in the living document list | **Moved into the `decisions/` row of the record folder table.** "We do not fix it" was written with it |
| Repo structure figure (`architecture.md` chapter 10) | Directly under `kor/` | Included in the `decisions/` description |
| The document's own front matter | Relative links based on the same folder | Fixed to `../`. A sentence saying it is a record and is not fixed was added |

**The English side was moved with it.** It is structure, not translation, so it was separated
from the pre-push mirror work. Also `CLAUDE.md` says "`link` must stay at 0 even in between",
and moving only one side would leave the cross links of `docs/eng/first_design.md` broken.

## Verification

- `python tools/docgate/docgate.py` — **`link` 0.** The remaining findings are 2 `mirror`
  (`design-audit3.md` and the English mirror of its follow-up record do not exist yet) and
  `parity`, both normal in the pre-push state. Before the move there were 8 `link`.
- Checked in full with `grep -rn 'first_design' --exclude-dir=.git .`. **The extensions were not
  restricted.** The remaining mentions are listed per place. The ones that point at the old path
  (`docs/{kor,eng}/first_design.md`) are only inside `commit_history/`.

| Where | What | Why it stays |
|------|------|-------------|
| `CLAUDE.md` record folder table | States that the `decisions/` row holds this document | This commit added that description |
| `architecture.md` chapter 10 (both languages) | The `decisions/` description in the repo structure figure | Same |
| `decisions/first_design.md` (both languages) | The front matter pointing at each other | Mirror pair links |
| `plan.md` | One row in the decision table | The handover of this decision |
| Several files in `commit_history/` | The old path and the role it had then | **Facts of that time, so they are not fixed** |
- The 19 ADR links did not change path, so the gate's link check still covers them.

## Cross-model Review

Codex, 2 rounds.

| Round | Lens | Input | Result |
|:--:|------|------|------|
| 1 | Structure change (leftover old paths, `CLAUDE.md` self-consistency, record cross-check, relative link depth of the moved file) | full diff | warn 1 |
| 2 | Repair check | only the increment of this record | `LGTM - no blockers` |

| # | Finding | Handling |
|:--:|------|------|
| 1-1 | The "Verification" above said every remaining `first_design` mention is inside `commit_history/`, but they also stay in `CLAUDE.md`, `architecture.md`, `plan.md` and the moved file itself | **Accepted.** Changed to a per-place table. The accurate statement is that only the ones pointing at the old path are inside `commit_history/` |

**1-1 is a verification sentence that was stronger than the fact.** The remaining mentions were
all intended, but writing "all of them are only ..." makes the next session believe that sentence
and not count again. When numbers and lists are written, they are matched to the output of the
command that was actually run.
