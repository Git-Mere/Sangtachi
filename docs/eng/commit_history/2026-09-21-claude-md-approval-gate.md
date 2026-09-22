# 2026-09-21 An approval gate for new rules in CLAUDE.md

Adding a **new rule** to `CLAUDE.md` now requires the repository owner's approval first.
Correcting stale facts and refreshing the current-state section do not.

## Change

Two paragraphs in `CLAUDE.md`: the approval gate itself, and the first rule to pass
through it.

**Every blockquote in this record is a summary.** The full text lives in `CLAUDE.md` and is not
copied here (rule 5).

**1. The `Rules` section.**

> Getting a new rule into this file requires the repository owner's approval first. A
> sub-agent's agreement or a cross-model review `LGTM` is not approval. Propose what goes in,
> which section it goes in, and on what basis, and write down only what was approved. An
> observation that was not approved stays in `commit_history/` only. Correcting a stale fact and
> refreshing the current state do not need approval.

**2. The `Working on code` section.** This is the rule approved under "first use" below.

> Check the handle state before saying a process has started. And do not put an edit, a state
> change and a process start in one cell. If an early line raises, the later lines vanish quietly.

## Decisions

**The scope of approval was drawn.** Without it, an obvious correction such as "test count 105 ->
109" would also be blocked, freezing the file in a stale state and deadlocking with the duty to
keep it accurate. Approval covers **creating a new rule or changing what one means**, nothing else.

**The approver is named.** The first wording said only "get approval". Without naming who, a
sub-agent's agreement or a reviewer's `LGTM` could be read as approval. The cross-model review
caught it.

## Why it is needed

In the previous commit `834bfab`, writing the session's lessons down as rules **broke those
lessons twice.** One new rule was an over-claim ("a reviewer never looks outside the diff", which
is false) and another duplicated an existing rule. The review caught both, but the owner would
have caught them sooner. When one session's impression becomes a permanent rule without review,
the next session follows a wrong rule.

## It persists beyond this session

`CLAUDE.md` exists only in this repository, and the same behaviour is needed elsewhere, so it was
also stored as a global policy for the agent. A proposal carries three things: **what**, **which
section**, and **on what basis**. The basis has to be something the session actually ran into.
A generality is not a basis.

## First use

The procedure was used immediately after it was created. The lesson from item 2 below was
**proposed and not written down** until the owner approved it. This is the sentence that went into
the `Working on code` section.

> Check the handle state before saying a process has started. And do not put an edit, a state
> change and a process start in one cell. If an early line raises, the later lines vanish quietly.

## Two mistakes during the work

**1. A global entry looked like it had not been saved.** The query returned an empty list. In fact
that API defaults to local-only entries, and global ones need an argument. **Telling a failed
query apart from an actual absence** is the rule from chapter 0 of `windows-prereq.md`, and it
applied here too.

**2. A review that never started was reported as running.** An edit, a state change and a process
start sat in one cell, and the second line raised. The following four lines never ran and the
review handle was never created, yet the report said "round 2 is running". **The handle state was
not checked; an intention was reported as a result.** It surfaced only because the owner asked
"is it running right now?".

## Verification

- `python tools/docgate/docgate.py`: `VERDICT: pass`
- The global policy entry was queried to confirm it is stored, including the named approver and
  the sentence excluding `LGTM`

## Cross-model review

Codex, 2 rounds.

| Round | Finding | Verdict |
|:--:|---------|---------|
| 1 | "get approval first" never says whose approval, so another agent's or a reviewer's sign-off could count | **Accepted.** The repository owner is named and `LGTM` is excluded |
| 2 | None | `LGTM - no blockers` |
