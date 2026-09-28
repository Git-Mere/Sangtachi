# Raised the Minimum GUI into the Core Scope and Chose Qt

## Changes

- `docs/{kor,eng}/decisions/0007-*.md` **new.** 6 decisions and 3 dropped alternatives
- [`spec.md`](../../spec.md) in 9 places. The included scope list, the stretch goals, **FR-15 new**,
  **T-7 new**, the NFR-5 approval list and the product dependency table, two rows of the priority
  tier table, the cost of priority removal table
- [`roadmap.md`](../../roadmap.md) in 5 places. Two lines of the priority block, the stretch table,
  the goal, tasks and verification of Phase 8
- [`architecture.md`](../../architecture.md) in 4 places. The module table row `ui/main_window`, the
  open thread paragraph under it, Qt in the external dependency table, deletion of "**GUI**: console
  only." in chapter 12
- Changed the "waiting on the GUI scope decision" section of `plan.md` into "GUI follow-up"

**Unlike ADR 0006, the living documents were fixed in the same commit.** The places to fix are
narrow, 18 of them, and the boundary is clear. Deferring would create a stretch where the ADR and
`spec.md` say the opposite of each other.

## Decisions

**It went into target success (T), not minimum success (M).** M is the proof that a direct
connection holds and it can be judged from the command line. Putting the GUI into M makes minimum
success fail on nothing more than a window not opening.

**The tier is P2 and its cost is written in the cost of priority removal table.** FR-15 and T-7 go
into the list of what disappears when P2 is dropped. If the scope is exceeded, the GUI is cut whole
together with the Minecraft validation. There is no middle step.

**No new Phase was made; it was absorbed into Phase 8.** Putting in a new Phase pushes Phase 9 back
and every Phase reference in the documents has to be fixed.

**The Qt approval was written the way ADR 0005 wrote Catch2.** The instructor approved it and that
was passed on by the repository owner. The scope of the approval (the version, the linking method,
the license conditions) was not passed on, so it is left under "What we could not check" of the ADR.

**Deleting "GUI: console only" moved the duty that line was carrying.** Deleting it alone leaves the
GUI defined in no document. A module row and an open thread paragraph went into `architecture.md`
3.1, and the time it is decided was hung on the pre-start item of `roadmap.md` Phase 8.

## We Made a Criterion That Cannot Be Judged Today

**T-7 cannot be judged with the current design.** Its last condition is "when one side is cut, the
other side's member list shows that member as disconnected", and there is no device that carries
that state. The peer states are only `joined` and `registered`, and neither of them is whether a
peer is connected ([`control_plane.md`](../../control_plane.md) 5.3 Peer).

It was not hidden; it was written in three places. "What This ADR Does Not Decide" and "What we pay"
of ADR 0007, and the GUI follow-up section of `plan.md`. **It is one question with the same open
item of [ADR 0006](../../decisions/0006-star-topology-no-relay.md).**

## Verification

- `docgate.py` gives `VERDICT: pass`. `pairs=66`, `links=633`, `findings=0`
- The English mirror was made in two parts and **a split term was caught.** The body side had
  `minimal GUI` and the ADR side had `minimum GUI`. `spec.md` writes minimum success as
  `Minimum success`, so it was unified to `minimum` to keep one word from being carried over two
  ways. 6 places in 3 body files
- After unifying, `minimal GUI` was confirmed to be 0 in the English tree
- Line endings did not change. All 3 fixed English files are LF and CRLF is 0
- After fixing the 3 blockers of review round 1, **FR-15 and T-7 were confirmed to be in the
  Requirement Traceability table.** The rule of that table is that no ID is left untracked

## Cross-Model Review

It was run with Codex, two lenses per call. Findings had to carry the lens name. The repair round
sent only the unstaged repairs.

| Call | Scope | Lenses | Result |
|------|------|------|------|
| 1 | The whole staged diff | tier arithmetic, criteria that cannot be judged | **blocker 3**, warn 3 |
| 2 | The repair increment, 33 lines in 4 files | repair soundness, contract completeness | `LGTM - no blockers` |

**The 3 blockers had one root.** FR-15 and T-7 require the connection state of a member and there is
no means that carries that state, and **that limit was written only in the ADR and `plan.md`.** A
person who reads `spec.md` first reads an unexecutable requirement as an executable one.

| Lens | Finding | Handling |
|------|------|------|
| criteria that cannot be judged | FR-15 requires connection state and there is no device for it | **Reflected.** A limit paragraph was added under the FR table of `spec.md` |
| criteria that cannot be judged | The current design cannot make the last condition of T-7 | **Reflected.** The same paragraph writes T-7 together |
| criteria that cannot be judged | Phase 8 verification asks to observe the disconnected state, while the task list has no item that fixes that means | **Reflected.** "Decide the means that carries member connection status first" was put at the head of the Phase 8 tasks |
| tier arithmetic | FR-15 and T-7 are not in the Requirement Traceability table | **Reflected.** They went into the Phase 8 row. That table is the one that says "No ID may be left untracked" |

**The limit was written in `spec.md` and the time in `roadmap.md`.** `CLAUDE.md` sets that
`roadmap.md` owns work timing, so no Phase number is written in `spec.md`; it only says "when it is
decided is owned by `roadmap.md`". The English mirror is the same.

**The mechanism of the mistake this commit made is written down.** While making a new criterion, we
**hung the contract that criterion leans on nowhere as a task.** That is what recurrence-prevention
rule 4 of `CLAUDE.md` says, and it also hits rule 3 ("a verification criterion is first checked for
whether a correct implementation passes it"). One lens of round 2 was set as "contract completeness"
so that it would look for the same mechanism in another place inside the increment, and there were
no findings.
