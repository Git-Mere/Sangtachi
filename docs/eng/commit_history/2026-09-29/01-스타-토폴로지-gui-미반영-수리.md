# Repaired the Places Where the Star Topology and GUI Decisions Had Not Landed

- Phase: Documents
- Related: [ADR 0006](../../decisions/0006-star-topology-no-relay.md), [ADR 0007](../../decisions/0007-gui-core-scope-qt.md), [ADR 0008](../../decisions/0008-star-topology-followup-decisions.md), [ADR 0009](../../decisions/0009-room-host-lease.md)

## Why

The three preceding commits (`5795eac`, `f832e34`, `bdb30f0`) reflected the no-relay star with
five people per room and the minimum GUI. Auditing that result through three lenses left 14
blockers. `docgate.py` was at `VERDICT: pass` even then. The gate counts mirror pairs, headings,
table rows and link counts, and **does not look at whether a document contradicts itself.**
This commit closes those 14.

The English mirror had no omissions. Every Korean change from the three commits was in the
mirror (`MAX_PEERS` 11/11, `ROSTER` 12/12, `Qt` 8/8, ADR links 10/10), so all 14 were **defects
in the original**. The mirror was wrong in the same shape.

## What Was Fixed

### Places where five peers had not come down

| Place | Before | After |
|-------|--------|-------|
| `protocol.md` chapter 1 v1 fixed values | `two peers per room` | Five peers at most. One host and four players |
| `protocol.md` chapter 2 forged `HELLO` | Two values, `.1` and `.2` | Five, `.1` through `.5` |
| `protocol.md` 5.1, 8.1 check 7, 8.2 `HELLO`, 8.4 check 14 | "the peer" in the singular | "the other side of that session" |
| `architecture.md` chapter 12 | `mesh of three or more: two peers initially` | `mesh of six or more and between players` |
| `architecture.md` chapter 7 | No cap on participants | Four, `.2`~`.5`. Why players do not talk to each other, and where that is enforced |
| `architecture.md` chapter 2 diagram | Symmetric two-person | Host 1 + two players, no line between players |
| `concurrency.md` chapter 4 | `in the minimum scope there is one session` | Timer kinds multiplied by the session-count cap |
| `spec.md` NFR-2 | No scope of application | A note narrowing it to the client-server form |

The `protocol.md` chapter 1 row was the urgent one, because the `control_plane.md` 2.6 constant
**cites `protocol.md` chapter 1 as the source of 5**. The top source was contradicting the
document that cites it.

### Places where the GUI had not come down

| Place | What |
|-------|------|
| `roadmap.md` Phase 8 tasks | "there is no means of carrying member connection state" was replaced by `ROSTER` |
| `roadmap.md` Phase 8 verification | The last condition of T-7 was made judgeable. The deadline is loss detection plus one roster period, and a normal exit and a forced exit are each run |
| `roadmap.md` Phase 4 implementation | Added the routing table, the `ROSTER` send/receive and discard counters, and the three `ROSTER` send moments |
| `architecture.md` chapter 10 tree | Added the `ui/` line. The name matches the 3.1 module table |
| `windows-prereq.md` section 4 | Widened the title to `packaging the distributable` and split it into two subsections, `Wintun DLL and signing` / `Qt runtime` |
| `roadmap.md` requirement tracking | Added NFR-5 to the Phase 8 row |

### Judgement criteria and unenforced obligations

- The `roadmap.md` Phase 3 verification item `calling with a different nonce gives room_full` was
  **a criterion a correct implementation could not pass.** In a five-person room a different
  `client_nonce` is a new join, and `room_full` comes after the pool is full. This is what
  recurrence-prevention rule 3 aims at
- The four measurements the ADRs told us to hang on `roadmap.md` were hung. The host's four
  simultaneous punches and whether the host NAT warning applies (before Phase 4 starts), at most
  four `CLOSE` messages and the three-second grace of adapter cleanup (Phase 8), and the race of
  five simultaneous joins (Phase 3 verification)
- **The host NAT warning was the one item ADR 0006 left open that was hung nowhere.** ADR 0006
  has eight open bullets, ADR 0008 counted them as seven and closed six. The missing one is this.
  ADR 0008 makes nine decisions of its own, and that number is unrelated

### The guide file

The current state in `CLAUDE.md` said "Phase 1 in progress. `endpoint`, logs, the UDP socket
wrapper and the event loop are left." All four are in HEAD. The ADR count 5 became 9 and the
`docgate` test count 109 became 111 (`python -m unittest test_docgate` gives `Ran 111 tests OK`).
The decisions of ADR 0006~0009 went into the current state. No new rule was added.

## How It Was Done

Both the audit and the repair were split across subagents. The audit ran three lenses (the
five-peer axis, the GUI axis, the guide-and-mirror axis); the repair was split four ways by file
ownership so that no file was touched by two agents. The repair prompts all carried the same
instructions: only touch `docs/kor`, do not touch the record folders, do not invent values.

**Two things were left rather than invented.** The stretch where the `IDLE` premise of
`protocol.md` 9.4.1 and the `IDLE` definition of 9.1 diverge, and the judgement input of the host
NAT warning. Neither is settled by any document, and neither was asserted to fill the gap. The
first is in the `plan.md` document debt, the second in `roadmap.md` before Phase 4 starts.

## Verification

- `python tools/docgate/docgate.py` -> 0 findings of kind `link`. The 4 `parity` findings come
  from the English mirror not being made yet, a state `CLAUDE.md` declares normal
- Exhaustive check of stale phrasings. `한 방에 2 피어`, `초기는 2 피어`, `세션은 하나`,
  `상대 피어 ID`, `상대 피어의 가상 IP`, `다른 nonce 로 부르면` are 0 in the Korean documents
- `ROSTER` in `roadmap.md` went from 0 to 5, `Qt` in `windows-prereq.md` from 0 to 7
- The kinds in the `protocol.md` chapter 11 timer table were counted to check whether the new
  supporting sentence in `concurrency.md` chapter 4 is true. 13 timer kinds, 5 expiry values
- Count claims cross-checked. `plan.md` document debt 8 items against 8 table rows, 9 ADRs
  against 9 files

## Five Items That Came Out of the Scratch Cleanup

Before deleting the working files in the parent folder (`star-followup-*.md`,
`sangtachi-scratch/`), we went through all of them looking for anything that had not made it into
the repo. There were no missing owner decisions and no discarded alternatives absent from the
ADRs, but **five open items were hung nowhere**. They were moved before the delete.

| What | Where |
|------|-------|
| The owner of the routing table. `protocol.md` 8.5 points at `concurrency.md` chapter 5, and that list had no such entry | `concurrency.md` chapter 5, state ownership |
| The rationale for a ring of 256 assumed a single session. The per-session periodic traffic was rewritten against the session-count cap | `concurrency.md` chapter 6 |
| The `send <n> [size]` contract has no destination. With several sessions it is undefined which session it means | `roadmap.md` Phase 4 test rig |
| The relation between GUI operations and control-plane rate limits. `control_plane.md` 6.4 does not cover the GUI | `roadmap.md` Phase 8, before start |
| Privilege elevation from the GUI and when leftovers are cleaned up | `roadmap.md` Phase 8, before start |

The working files held no measurement originals. Not one public IP, credential or instance
address was in them, so they were deleted as they were.

## Cross-Model Review

Run against Codex with the lens changed each time. The full diff got scenario tracing and
self-contradiction, then the full diff including `plan.md` and this record got removal and
counts, then the increment of this record got the record lens, then the five items moved out of
the scratch got derivation and obligation. The last round is `LGTM`. Dismissed findings were
written into the next round's prompt so the same finding would not come up again. No call
carried three or more lenses.

| Finding | Verdict |
|---------|---------|
| `roadmap.md` Phase 8 verification. If the host is the one cut off, there is nobody to send the roster, so that criterion does not hold | **Accepted.** The criterion was narrowed to a player leaving, and host exit was written separately as returning to the lobby |
| `plan.md`. The GUI is before its Phase 8 implementation, so "reflected in the code" is an overstatement | **Accepted.** The code side is only the `--rejoin` removal; the rest belongs to the relevant Phase, and that is what it now says |
| Adding files under `commit_history/` violates "records are not edited" | **Dismissed.** That rule is about **not editing records that already exist**, and `CLAUDE.md` requires a new record for every meaningful commit. The reviewer read the repo rule backwards |
| ADR 0009 is missing from the `Related` list of the record | **Accepted.** ADR 0009 was added to this record's `Related` list, because this change wrote that decision into `CLAUDE.md` as well |
| "nine follow-up decisions" in `CLAUDE.md` conflicts with "six of seven" in the record | **Dismissed.** They count different things. Nine is the number of decisions ADR 0008 makes; seven and six are how ADR 0008 counted the open items of ADR 0006. It can still be read that way, so that sentence of the record was spelled out |
