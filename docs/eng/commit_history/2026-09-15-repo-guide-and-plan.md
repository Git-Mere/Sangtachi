# 2026-09-15 Repo Guide and plan.md

## Changes

Fixed the problem that a fresh session cannot find the remaining work.

- Added `CLAUDE.md` at the repo root: document locations, session-start reading order, the role and
  precedence of each document, bilingual rules, commit conventions, rules for documentation work,
  and current state
- Filled in `docs/{kor,eng}/plan.md`, which was an empty template. Follow-ups 3 through 6 are
  expanded into items and verification criteria, and 3 documentation debts are recorded
- Corrected 4 factual errors in `design-audit.md` (below)

## Problem

The session-start reading list in the global convention is `docs/spec.md`,
`docs/commit_history/`, `docs/decisions/`, `docs/plan.md`. **None of those four paths exists since
the eng/kor split**, and that fact was written down nowhere. Even after finding the right paths,
`plan.md` was an empty template and `decisions/` was empty, so the design-audit follow-up work
lived only in section 5 of `design-audit.md`, a document not on the reading list.

A fresh session would have had to read `commit_history/`, notice the "remaining follow-ups" line at
the end, and follow it there by chance.

## Decisions

**`CLAUDE.md` goes at the repo root.** There was no project-level instruction file at all. At
minimum it has to record that the paths the global convention names are different in this repo.

**State that the remaining work is split across three places.** Design audit follow-ups are in
`design-audit.md` section 5 and `plan.md`, Phase implementation is in `roadmap.md`, and
documentation debt is in the last section of `plan.md`. `CLAUDE.md` carries this as a table so that
reading one of them is not mistaken for seeing all of it.

**Narrowed the `protocol.md` precedence rule.** "If they conflict, `protocol.md` is right" reads as
covering requirements and the concurrency model too. It is now limited to wire format, state
transitions, and protocol timer values, with `spec.md` named as the source for requirements and
`architecture.md` 3.2 for the thread structure.

**Added a real punch trial to follow-up 5.** It originally listed only a STUN Binding comparison,
but Binding reveals mapping behavior and says **nothing about filtering behavior.** Hole punching
depends on both, so deciding from the mapping alone misses the case where the mapping is fine and
filtering is what blocks it.

**Recorded that none of the follow-up 5 options works while the success criteria stand as written.**
The minimum success is direct UDP between two home networks: A is not that topology, B is not
direct, and C does not make the two required paths pass. Choosing an option and revising M-1/M-4 are
one unit of work.

## `design-audit.md` Corrections

Writing this surfaced 4 factual errors in the audit document.

| Location | Wrong statement | Correction |
|----------|-----------------|------------|
| blocker 18 | "Dropping P1 removes M-6" | M-6 is control-plane telemetry and belongs to P3 (Phase 9). Dropping P1 removes M-5. Dropping P3 removes M-6 along with the analysis |
| blocker 18 | "The only droppable tier is P2 (Minecraft)" | P2 is Wintun, virtual IP, and Minecraft together (Phases 6-8). It also needs the qualifier that P4 is a stretch tier outside the calculation |
| blocker 15 | "If `server-ip` is set, connections are refused" | Setting it to `10.100.0.1` works. Refusal happens when it is set to **a different interface address** |
| line 89 | "The central claim is that an ordinary user configures nothing" | The claim in `spec.md` is "without manual port forwarding", and that claim holds. Rewritten as a usability limitation rather than a goal-level contradiction |

## Verification

- eng/kor heading counts match (plan 7, design-audit 18, others unchanged)
- Every relative link except `experiments.md` resolves
- All 5 reading paths named in `CLAUDE.md` exist
- Checked `src/main.cpp` and `CMakeLists.txt` before writing "Phase 1 has only its CMake setup"

## Cross-Model Review

- Reviewer: Codex (GPT), 5 rounds
- Result: 7 blockers, 20 warnings. **All applied, none dismissed.** Round 5 returned
  `LGTM - no blockers`

| Round | Blockers | Warnings |
|-------|----------|----------|
| 1 | 3 | 6 |
| 2 | 1 | 9 |
| 3 | 2 | 4 |
| 4 | 1 | 1 |
| 5 | 0 | 0 |

All 7 blockers:

| Round | Finding | Applied |
|-------|---------|---------|
| 1 | Calling `design-audit.md` section 5 "the only source of remaining work" loses the Phase implementation and the debt | Added the three-places table to `CLAUDE.md` |
| 1 | The follow-up 4 check "remove each tier in turn and M-1 to M-6 still hold" directly contradicts that item's own problem statement; even a correct fix fails it | Changed to "does the droppable marking match reality" |
| 1 | Follow-up 5 completes on picking an option and documenting it, so an option that also fails is not caught | Added reproduction conditions and failure handling to the completion criteria |
| 2 | None of options A to C produces the minimum success while M-1 and M-4 stand as written | Stated that revising the success criteria is part of the choice |
| 3 | A STUN Binding comparison cannot reveal filtering behavior and so cannot decide whether hole punching works | Added a real punch trial to the measurement |
| 3 | "3 consecutive successes in a controllable environment" does not apply to option C (a mobile hotspot) | Split into per-option reproduction conditions |
| 4 | Blocker 18 attributes M-5 and M-6 to the wrong tiers | Corrected in both `design-audit.md` and `plan.md` |

Selected warnings (8 of 20; the rest were wording and accounting corrections of the same kind):

| Round | Finding | Applied |
|-------|---------|---------|
| 1 | "`protocol.md` wins on conflict" covers requirements and the concurrency model too | Limited to wire format, state transitions, and protocol timers |
| 1 | "Phase 1 not started" contradicts the CMake smoke commit | Changed to "CMake setup only; nothing from Winsock2 onward" |
| 2 | The follow-up 3 warning accounting is unclear: the EC2 public IP change was folded into blocker 16 and Wintun DLL loading is not an audit warning at all | Split the EC2 IP into its own warning row and removed the Wintun loading row |
| 2 | Follow-up 6 says "2 warnings" but the table has 4 rows | Labeled as "2 warnings (4 criteria)" |
| 2 | The stated reason for Phase 7 counter mismatch was "retransmission and reordering", neither of which changes byte totals | Replaced with capture point placement, NIC offload, and capture loss |
| 2 | Requiring sample size and a statistic for a qualitative criterion like A-2 recreates an unpassable criterion | Split completion conditions into quantitative and qualitative |
| 3 | The Windows target statement in `CLAUDE.md` covers the control server (Linux/EC2) too | Limited to the client |
| 4 | The `server-ip` statement is wrong | Stated that `10.100.0.1` works |

## Remaining Follow-ups

See `plan.md`. Steps 3 through 6 are open. Step 5 is recommended first.
