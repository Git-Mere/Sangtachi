# 0011. Define the Lobby by Room Membership, Not by the Session List

- Status: accepted
- Date: 2026-10-06
- Related: [ADR 0008](0008-star-topology-followup-decisions.md) decision 1, 8, [`../concurrency.md`](../concurrency.md) chapter 1, chapter 7, chapter 8, [`../architecture.md`](../architecture.md) 3.5, [`../control_plane.md`](../control_plane.md) 8.4, [`../roadmap.md`](../roadmap.md) Phase 3, 4

## Context

Decision 1 of ADR 0008 wrote the lobby in terms of the session list. "When the list is empty it goes
to the lobby. A player is the case where the list has 1 entry." The lobby of `concurrency.md`
chapter 7 took the same definition and wrote it as "the state where the session list is empty".

**It holds for a player and does not hold for the host.** There are two moments when the host's
session list becomes empty.

| Moment | Under that definition |
|--------|-----------------------|
| The room was just set up and nobody has attached yet | The host is already in the lobby |
| The last player left | The host goes to the lobby, `host_report` stops, and the room disappears |

Both go against other decisions of ADR 0008. Late join (decision 2) and slot reclaim (decision 5)
presume that the room stays alive while players come and go.

This hole came out while deciding "the input the lobby accepts", an item to settle before Phase 3
starts. To write the lobby commands `host` and `join` as "not accepted while in a room", "being in a
room" has to be defined separately from the session count.

## Decision

The repository owner decided the following.

**1. The lobby is the state where the process does not belong to a room.** It is not defined by the
session list.

**2. Once the host has set up the room, it stays in the room even when the session list is empty.**
Only `leave` and `quit` leave the room. The room is set up at the moment `register_candidate`
succeeds. A failure before that goes to the lobby, the same as for a player.

**3. A player stays as it is today.** When the session with the host ends or the attempt fails, it
goes to the lobby.

**4. From the lobby, the console commands `host` and `join <room code>` enter a room again.** The
role argument of the CLI is the same as typing that command once right after startup. The role
argument is no longer required.

**5. When an attempt fails it emits a `FAIL` line and the process does not end.** The exit code does
not carry the result of the attempt.

**6. The `[console]` thread stays in the console build after Phase 6 too.** It is the only place
that receives the commands of decision 4. What disappears in Phase 6 is the test-only `--peer` and
`raw`.

**7. When `leave` aborts an attempt that has a pending control request, that response is
discarded.** Until that response arrives, `host` and `join` of the next attempt are not accepted.

**8. Rejoining from the lobby runs STUN again.** Whether to skip it is not decided until it is
measured.

ADR 0008 is not edited. This ADR narrows "when the list is empty it goes to the lobby" of decision 1.

## What This ADR Does Not Decide

The timing is owned by [`../roadmap.md`](../roadmap.md).

| Item | When |
|------|------|
| Whether the host stays in the room when `host_report` gets a definite error after the room is set up | Before Phase 3 starts |
| Whether the `FAIL` line of a host-side pair failure carries which player it belongs to | Before Phase 4 starts |
| How an attempt aborted by `leave` is written to the local record file | Before Phase 4 starts. Together with the line format |
| The thread layout of the GUI build | Before Phase 8 starts. An open item of ADR 0007 |

## Alternatives

**Alternative 1. Keep the session list definition and write the host alone as an exception.**
Dropped. The result is the same, but the definition "lobby = the list is empty" stays false for the
host. Whoever writes a sentence leaning on that definition misses the exception.

**Alternative 2. The host also goes to the lobby when the last player leaves.**
Dropped. Late join and slot reclaim become useless. Even for one player to step out briefly and come
back in, a new room code has to be obtained.

**Alternative 3. End the process when an attempt fails.**
Dropped. Its strength was that a script could judge by the exit code. The price is reverting
decision 8 of ADR 0008, and a failure and a `leave` would get different lifetime rules.

**Alternative 4. Attach an attempt number to the response to tell apart the responses of aborted
attempts.**
Dropped. The state `[control]` holds grows and the queue item format changes. The existing rule that
there is one pending request at a time gets the same result on its own. The price is that right
after `leave`, the next `host` / `join` is refused for up to the upper bound of one request.

## Consequences

**What we gain.**

- The lobby definition is true for both the host and the player
- The CLI and the console enter a room by the same path. There is one set of rules after a failure
- The console build stays usable in the lobby after Phase 6 too

**What we pay.**

- `[console]` stops being scaffolding and stays in the console build for good
- `--room` with no role becomes a startup failure from Phase 3 on. The current code accepts it
- The three items of "What This ADR Does Not Decide" above were added to the before-start items of Phase 3 and 4
- Rejoining from the lobby exposed a hole that piles up the slots of participants who left without
  candidates. Its reclaim was decided by
  [ADR 0012](0012-server-reclaims-candidateless-peers.md)
