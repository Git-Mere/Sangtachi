# Phase 4 Wave 2 E+F: Session and Receive Pipeline

## Why

This is Phase 4 wave 2. 8.2 reads and changes session state, so one subagent built the session (F) first and then the receive
pipeline (E). The five document gaps and the per-session counters that came up during the work were settled by the previous
documentation commit (ADR 0022).

## What changed

| What | Where |
|------|-------|
| `peer/session`: the section 9 transition table, dual flags (`sent_ack` only after a successful send), 9.3 epoch rules, 9.4.1 terminal states, the ten steps and two caps of 9.5 renegotiation, the three 9.6 failure transitions, section 11 timers, 5.5 `PING`/`PONG`, 5.6 `CLOSE` and reasons, 4.4 send `sequence`, 10.4 endpoint learning inside the candidate set. Clock, send, and random source are injected | `client/include/sangtachi/peer/session.hpp`, `client/src/peer/session.cpp` |
| `tunnel/tunnel`: the order 8.1 checks 7 and 8, 9.4.1, 8.2, 9.5, 8.3, per-type checks (8.4 checks 10-15, 5.5, 5.6, 5.7), side effects. Terminal sessions kept for 2 minutes, the cap, deletion on leaving the room. One retired list per process | `client/include/sangtachi/tunnel/tunnel.hpp`, `client/src/tunnel/tunnel.cpp` |
| The test receive entry point `EventLoop::receive_datagram` wired to `drain_udp`, session deadlines merged into the loop wait | `client/src/loop.cpp`, `loop.hpp`, `main.cpp` |
| Per-session counter tables (ADR 0022). New names `drop_roster_name`, `dup_ack`, `endpoint_learned`. Two removed from `PENDING_COUNTERS` | `counters.*`, `tools/docgate/docgate.py` |
| A microsecond monotonic clock for RTT | `platform/wait.hpp`, `platform/win32/wait.cpp` |
| Tests | `tests/peer/session_test.cpp`, `tests/tunnel/tunnel_test.cpp`, `tests/session_fixture.hpp`, `tests/counters_test.cpp`, `tests/platform/wait_test.cpp` |
| `peer_id` on `session.state` and `session.failed`, the `platform/wait` row | `architecture.md` 3.1, section 9 |
| Removed the wave 2 row and rewrote the wave 3 row. The seams left by wave 2 and the spots for wave 4 J. Removed the closed "own-subnet set on the receive path" row | `plan.md` |

## Important decisions

- **The five document gaps were settled by the previous commit.** The place of the terminal-state check, one retired list,
  per-session counters, the RTT clock, renegotiation before the punch
- **The code settled small things the documents do not define.** None contradicts a documented rule
  - If the user ends a session before an endpoint is learned, the session is still `CLOSED`. No `CLOSE` is sent (5.6)
  - `endpoint_learned` rises every time the value changes. The first learning counts as 1
  - The first row of the 4.3 registration table and the 5.1 attempt-nonce retirement are registered when the next attempt is created.
    The epoch of a deleted session is not registered
  - The minimum renegotiation interval passes when `basis + 1000 <= now`, following the inequality of the section 11 deadlines. "within
    1000ms" in 9.5 does not pin the boundary
  - `DATA` that passed 8.4 with no hook refreshes the idle timer and learning, then is `drop_no_sink`
  - Renegotiation does not reset `ping_id` or the last `ROSTER` `generation`. They are not in the list of 9.5 step 3
  - No new session is created while a live session with the same `peer_id` exists
- **The seams handed to waves 3 and 4 are in `plan.md`.** Until out-of-set path validation and the unverified budget arrive, sources
  outside the candidate set are not learned and replies go out without a budget

## Verification

| What | Result |
|------|--------|
| Full test run (`scripts/test.ps1`, after merging into main) | 492 unit tests pass, 0 failed cli/e2e/lobby checks, `all tests passed`. After the review fixes below, 495 unit tests, same result |
| Mutation (subagent, clean copy) | 112 kinds. The tests caught 110. The two survivors give the same result as the original. The `pending_pings` cap cannot be reached with the current timers because the `PING` interval and the entry lifetime are both 5 seconds. The tunnel's terminal-state check duplicates the same check in the session, so removing one leaves the other to catch it (the mutant removing the session one was caught) |
| A pitfall in the mutation run | Some intermediate runs copied the tree preserving modification times, so object files of the previous mutant were not rebuilt. Those results were discarded and all 112 were rerun on a copy with fresh times |
| `docgate.py`, `platformgate.py` | Both `VERDICT: pass`. The counter check included |

## Cross-model review

The subagent ran Codex review three times in its worktree.

| Finding | Disposition |
|---------|-------------|
| blocker. On a session already `CONNECTED` before the punch starts, `start_punch` re-arms the timers and fails it 10 seconds later | Applied. Nothing is armed when `CONNECTED`. Test and mutant |
| warn. The microsecond clock test cannot tell a millisecond clock apart | Applied. It checks steps below 1ms. Mutant |
| warn. No test of send `sequence` continuity across renegotiation | Applied. Test and mutant |
| warn. On the next attempt, the kept terminal session is deleted before the epoch and nonce are drawn | Applied. It is deleted only after both succeed. Test and mutant |
| warn. No test looks at the console `counters` and the session tables together | Applied. Test and mutant |
| warn. No test for an epoch draw that succeeds while the nonce draw fails | Applied. Test and mutant |
| warn. No test of `ping_id` continuity after renegotiation | Applied. Test and mutant |

After merging into main, Codex reviewed the staged diff.

| Finding | Disposition |
|---------|-------------|
| warn. Session timers do not emit `timer.tick` when they expire. `architecture.md` section 9 says every expiring timer does | Applied. The five session timers emit `name=session.<hello\|punch\|keepalive\|ping\|idle>.<peer peer_id>` and `elapsed_ms`. The names follow the shape of the STUN timers. Two tests and seven mutants |
| warn. The loop deadline test has no incoming packet, so running session timers before receive still passes (`protocol.md` section 11) | Applied. Added a loop integration test with a valid `KEEPALIVE` queued at the idle deadline. A mutant reversing the order fails |

Round 2 (confirming those fixes) returned `LGTM - no blockers`.
