# Phase 4 Round 4 J2 and J3: Local Candidate Wiring, Rendezvous and Lobby Integration

## Why

This is the rest of Phase 4 round 4 J. Rounds 1 to 3 built the session, the receive pipeline, the routing table, and
`ROSTER`, but the product path had no place that opened a session. This commit connects the control plane rendezvous
(`control_plane.md` 8.4 steps 6 to 10, `protocol.md` 10.2) to the session, and connects the session's outcome to the
lobby (`concurrency.md` chapter 7) and to the local record file of J1 (`architecture.md` chapter 9). With it, two
processes on the same machine create a room, punch, reach `CONNECTED`, and leave an establishment line and an end line in
their record files.

J2 (wiring local candidate collection) and J3 (integration) were first meant to be separate commits. Both change the
same places in `Lobby` and `ControlRunner`, so they were merged into one commit.

## What Changed

| What | Where |
|------|-------|
| Open the session on the ready reply (`OpenSession`) and punch after the punch delay (`StartPunch`, timer `control.punch.<peer_id>`). Each pair keeps its name. At the session's end: `FAIL <code> <peer>`, the lobby, `departed` and an immediate `host_report`, waiting after the room ended on the server, starting `ROSTER` and its member list, the establishment line of an attempt that ended without a session, the shutdown event | `control/lobby.*` |
| Collect local candidates once right before `register_candidate` and hand the own-subnet set to the loop | the collector function of `control/lobby.*`, `control/runner.*` |
| Execute the new actions, copy session transitions and handle them after the tunnel call (`flush`), the record lines of pairs that had a session, and the `record_result` table | `control/runner.*` |
| The transition callback now covers every transition, not only terminal ones (`set_transition_handler`) | `tunnel/tunnel.*` |
| Create the tunnel before the runner, and wire the collector, the port, the own-subnet set, and the record file. `flush` after receive and timers, steps (2) and (3) of shutdown | `main.cpp` |
| The `<peer>` column of the `FAIL` line. `-` before a session or for a failure of the whole room | `fail_line` in `control/lobby.*` |
| The reason name `room_ended` for a host with a set-up room going to the lobby | `scripts/lobby-check.ps1` |
| Three gaps in the documents | `architecture.md` chapter 9 (when a host pair's attempt starts), `control_plane.md` 8.4 (the host opens sessions on responses after its room is set up), `protocol.md` 9.6 (failing to create a session is `CONTROL_PLANE_EXCHANGE_FAILED`). Korean and English |
| Tests | `tests/control/lobby_test.cpp`, `tests/control/runner_test.cpp`, `tests/tunnel/tunnel_test.cpp` |
| Removed the three round-4-J rows and wrote K as next | `plan.md` |

## Key Decisions

**The repository owner approved four interpretations before the work started.** Two had no place in the documents, so
they were written into the documents first as in the table above.

1. A host pair's attempt starts when the host sees the ready reply and opens the session. It has an establishment line even if it ends during the punch delay
2. The host does not open sessions from ready pairs in responses before the room is set up (before `register_candidate` succeeds)
3. Every way to the lobby is leaving the room. Retained terminal sessions are removed then (`protocol.md` 5.6)
4. `Lobby` stays pure code. It takes session transitions as input and returns opening a session, leaving the room, the member list, and record lines as actions

**Session transitions are not handled where they happen.** The tunnel's transition callback runs inside a session
method. Doing `Lobby`'s reaction (`LeaveRoom`) there would touch a session that is being removed. The runner copies the
values into a queue, and `[loop]` calls `flush` after every tunnel call. Transitions caused by the runner's own actions
are handled after that action list.

**Record lines are split by whether there was a session.** `Lobby` writes attempts that ended without a session. The
runner writes pairs that had a session, from that session's transitions. The result values are the `record_result`
table (`architecture.md` chapter 9 result values).

**Small things the documents do not fix were decided in code.** They are in the tables of `lobby.hpp` and `runner.hpp`
with their grounds. A punch delay of 0 or less punches at once without a timer. Every control plane failure of a host
with a set-up room (a full request queue and a mismatched reply included) is handled like the room ending on the server.
A pair is opened only once in a room. The shutdown event sends no request and no `LeaveRoom`.

**One sentence that disagreed with the document was fixed.** The code's `CONTROL_PLANE_EXCHANGE_FAILED` sentence was
missing the last sentence of the table in `architecture.md` chapter 8 ("방에 같은 이름이 있으면 다른 이름을 쓰세요.").
The table is the source, so the code was made to match it.

## Verification

| What | Result |
|------|--------|
| Full tests (`scripts/test.ps1`) | 591 unit tests passed, 0 failures in the cli/e2e/lobby checks, `all tests passed`. Run on DynamoDB local |
| Two processes on the same machine | Started `host` and `player` on the control server harness (DynamoDB local). Both reached `CONNECTED` and the player received `ROSTER`. The player's `send 3` sent three, and the product-build host received them as `drop_no_sink` 3. After the player's `leave`, the host ended in `CLOSED` and sent a `host_report` carrying `departed`. Both record files have a `result=OK` establishment line and a `result=CLOSED` end line with the same attempt number. The peer candidates were the reflexive address the harness reports (`192.0.2.1`, a documentation range that is not reachable) and a local candidate (`10.0.0.49`). There is no log yet of which address was learned, so that was not checked (it is K's job) |
| Mutations (clean copy) | 32. 29 were caught from the start. Of the remaining three, M26 (writing another establishment line on re-entering `CONNECTED` after renegotiation) was caught after the renegotiation test the review asked for was added. M31 (removing the lobby check in `on_session_event`) and M32 (removing the active check of the punch timer) were equivalent mutants. `pairs_` is cleared on the way to the lobby and the timer is removed when a pair ends, so the two checks guarded nothing. They were removed, together with the same kind of lobby check in `on_session_open_failed`. After the review fixes, a mutant measuring setup_ms at `flush` time and a mutant dropping the end line's RTT were added; both were caught |
| `docgate.py` | `VERDICT: pass` |

## Cross-Model Review

Codex ran with two lenses separately (contract and correctness, test sufficiency and document consistency). The contract lens was `LGTM - no blockers`.

| Finding | Handling |
|---------|----------|
| warn. No test re-enters `CONNECTED` after renegotiation, so writing another establishment line or a new attempt number passes | Fixed. Re-entry after renegotiation keeps one establishment line and the end line uses the first number. Mutant M26 is caught by this test |
| warn. No test checks the RTT of the end line | Fixed. One matching `PONG` makes a sample; the establishment line is empty and the end line is `rtt_us=23000` |
| warn. The transition and `flush` share a clock value, so measuring at `flush` time passes | Fixed. The clock moves between the transition and `flush` |
| warn. An implementation where the pending `host_report` reply clears all of `departed` passes | Fixed. When 22 ends while 21 is being reported, the next report carries 22 |

The fix-confirmation review (those four and the three checks removed by mutation) was `LGTM - no blockers`.
