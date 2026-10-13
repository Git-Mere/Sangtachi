# Phase 4 Round 3 I: The Host's `ROSTER` Sending and Its Timing

## Why

This is the last slice of Phase 4 round 3. The pre-start decisions were made by ADR 0023. With this, round 3 (G, H, I) is done.

## What changed

| What | Where |
|------|-------|
| The host's roster, `generation`, the three send times, the two Chapter 11 `ROSTER` timers | `client/src/tunnel/roster_publisher.cpp`, `roster_publisher.hpp` |
| The tunnel holds it, reports `CONNECTED` entry and exit, and runs its deadlines and sends after the session timers. Leaving the room turns it off | `tunnel/tunnel.*` |
| `Session::send_roster`. One `roster` line per roster a player accepts | `peer/session.*` |
| The `members` value of the `roster` line | `format_roster_members` in `tunnel/packet.*` |
| Put the destination of `ROSTER` under the 10.4 rule (the learned `peer_endpoint`, or every candidate before learning) | `protocol.md` 10.4. English mirror |
| Module row and directory tree for `tunnel/roster_publisher` | `architecture.md` 3.1, Chapter 10 |
| Removed the round 3 row and wrote where round 4 J connects (`start_roster`, `set_roster_peers`) | `plan.md` |

## Key decisions

**One document gap was written into the documents first.** Where `ROSTER` goes was in neither 10.4 nor 5.7. Like `DATA`, `KEEPALIVE`,
and `PING`, it is a periodic send we start, so it was put under the same rule.

**Small points that do not conflict with the document rules were decided in code.** They are in the "decided by this code" table of
`roster_publisher.hpp` with their basis.

- Triggers (state transitions, `set_peers`) only mark, and the send happens at the timer step of the same round. Nothing is sent again inside receive processing
- Several changes within one round are folded into one. `generation` rises by 1 and one copy goes out. A member that leaves and comes back in the
  same round counts as a change even if the content looks the same, and the leaving session's timers are removed on the spot
- When a session not on the roster enters `CONNECTED`, the content is unchanged, so only that session gets one (Chapter 11 "once immediately on entry")
- Timer names are `roster.period.<peer_id>` and `roster.resend.<peer_id>`
- More than five members are not sent (`encode_roster` refuses)

## Verification

| What | Result |
|------|--------|
| Full test run (`scripts/test.ps1`) | 547 unit tests passed, 0 cli/e2e/lobby check failures, `all tests passed`. Run on DynamoDB local |
| New tests | One send on entry, two resends and the 15-second period (including deadlines and `elapsed_ms`), same content, transitions unrelated to bit 0, a name change, entry and same-round re-entry of a session outside the roster, exit and notifying other sessions, a new change replacing resends, members before the first round, leaving and returning in the same round, a player, leaving the room and a new room, more than five, every candidate before learning, the receive line |
| Mutation (clean copy) | Round 1: 14, round 2: 4. All killed. The one round-1 survivor (not resetting `generation` to 0 for a new room) was killed in round 2 after adding a test. The other three in round 2 are the review repair points (ignoring the forced change, keeping timers on exit, counting any transition as entry) |
| `docgate.py`, `platformgate.py` | Both `VERDICT: pass` |

## Cross-model review

Codex ran with two separate lenses (contract and correctness; test adequacy and records).

| Finding | Resolution |
|---------|------------|
| blocker. Leaving and returning to `CONNECTED` within one receive batch folds into one mark, losing the change, the timer cancellation, and the entry send | Accepted. On exit that session's timers are removed at once, and for a member it counts as a change. Test and mutation |
| blocker. `start` leaves the previous roster empty, so members received before the first round become `generation` 0 | Accepted. `start` sets the host-only roster 0. Test |
| warn. Not checked that a new change replaces pending resends | Accepted |
| warn. Not checked that a change reaches every `CONNECTED` session | Accepted |
| warn. Content after a name change not checked | Accepted |
| warn. Timer deadlines not checked | Accepted. Resend deadlines are checked |
| warn. No case of leaving and setting up a room again with timers armed | Accepted |
| warn. `elapsed_ms` of `timer.tick` not checked | Accepted. Including a late round |
| warn. Fanout of `ROSTER` to every candidate before learning not checked | Accepted |
| warn. The line for a second accepted roster and no output for a dropped roster not checked | Accepted |
| warn. The header said the `roster` line comes "after idle refresh and learning" with no test of that order | Fixed the header. That order is not a documented contract |

The fix-confirmation review was `LGTM - no blockers`.
