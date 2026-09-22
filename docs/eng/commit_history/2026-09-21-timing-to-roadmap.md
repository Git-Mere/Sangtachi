# 2026-09-21 Work timing is gathered into roadmap.md

**Work timing instructions** such as "decide this in Phase X" were removed from `spec.md`,
`architecture.md` and `protocol.md`. Those three are the sources for requirements, design and the
wire protocol. They are not a schedule. `tools/nat-probe/NEXT-ON-WINDOWS.md` was deleted and the
links to it were cleaned up.

## What moved

| Document | Removed wording |
|----------|-----------------|
| `spec.md` M-6 | "decided before Phase 4 starts" |
| `architecture.md` chapter 9 | "decided in this document before Phase 4 starts" |
| `architecture.md` chapter 10 | "this relocation is handled as the first task of Phase 1" |
| `architecture.md` chapter 6 | "fixed by measurement in Phase 7" |
| `protocol.md` chapter 11 | "the measured mapping lifetime in Phase 4" |
| `protocol.md` chapter 12 | "Phase 7 measures and fixes the default" |
| `protocol.md` 10.4 | "whether it is automatic is undecided and must be settled in Phases 3 to 5" |

## What stayed

**A statement of scope is not timing.** "There is no adapter in Phases 1-5", "two threads from
Phase 6 on", and "omitting `SIO_UDP_CONNRESET` guarantees a failure in Phase 4" say what the
design looks like at that stage. They do not tell anyone when to do work, so they stay.

**Chapter 15 of `protocol.md` also stays.** "All of the following must exist in code in Phase 4"
is strictly timing, but that chapter is the Phase 4 / Phase 5 split of the implementation
checklist. Removing it would remove the point of the chapter. It is left as a judgement call.

## Where each obligation went was checked before removing it

| Moved obligation | Already anchored at |
|------------------|---------------------|
| Source tree relocation | `roadmap.md` Phase 1 tasks |
| MTU fixed by measurement | `roadmap.md` Phase 7 tasks and verification |
| Mapping lifetime measurement | `roadmap.md` Phase 4 verification |
| Local record file format | `roadmap.md` Phase 4 pre-start item (added in the previous commit) |
| **Whether retries are automatic** | **Nowhere. It was added to `roadmap.md` Phase 5 tasks** |

That is recurrence-prevention rule 4. A removal defines where the obligation now lives. One of the
five was anchored nowhere, and removing it without checking would have dropped it silently.

## Deleting `NEXT-ON-WINDOWS.md`

The repository owner deleted it. Two live documents linked to it.

- The nat-probe row under "what is waiting" in `plan.md` now points at `tools/nat-probe/README.md`
- The "read this first" line at the top of `tools/nat-probe/README.md` was removed

The 11 mentions in `commit_history/` were left alone. They are plain text rather than links, and
they record a fact about that moment.

**There was one misjudgement during the work.** Seeing the `D` for that file in `git status`, the
file was restored with `git checkout` without asking why it was gone. The assumption was "I never
deleted it, so this is an accident." When a file disappears, the first step is finding out why,
not restoring it.

## Verification

- `python tools/docgate/docgate.py`: `VERDICT: pass`. 32 pairs, 224 links, 0 findings
- Each of the five moved obligations was checked against where it lands in `roadmap.md`
- `grep` swept every reference to `NEXT-ON-WINDOWS`

## Cross-model review

Codex, 1 round.

| Finding | Verdict |
|---------|---------|
| The Phase 4 timing for the local record file format was removed, but the matching roadmap task is not in this diff (2 findings, kor and eng) | **Dismissed.** It was added in the previous commit `f406be0` and sits at line 160 of `roadmap.md` in both languages. The reviewer reads the diff, not HEAD |

**This is the second dismissal of that kind today.** A reviewer does not look outside the diff. It
reports something already fixed in an earlier commit as missing. Telling it to check HEAD in the
prompt does not fully remove the tendency to judge from the diff alone.
