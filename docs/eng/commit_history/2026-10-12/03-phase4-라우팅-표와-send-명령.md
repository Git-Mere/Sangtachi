# Phase 4 Round 3 H: Routing Table, 8.5 Transmit-Side Validation, the `send` Command, Test-Build Instrumentation

## Why

This is the second slice of Phase 4 round 3. The pre-start decisions were made in the preceding docs commit (ADR 0023).

## What changed

| What | Where |
|------|-------|
| 8.5 routing table. A fixed six-slot array with a two-step lookup. The `tunnel/router` place of `architecture.md` 3.1 | `client/src/tunnel/router.cpp`, `router.hpp` |
| The slot is filled when a session is created and emptied, before record lines and counters, when it reaches a terminal state. No session is created outside step 1, for the own virtual IP, or into a slot holding an active session | `tunnel/tunnel.cpp` |
| 8.5 transmit-side validation `Tunnel::send_data`. 1-5 go to the process table, 6 to the chosen session's table. On pass, after the transmit observer slot (`data.tx` in the test build), that session sends it as `DATA` | `tunnel/tunnel.*`, `peer/session.*` |
| Console `send <n> [size] [to]`. The 100-byte layout, command-stage filtering, `MAX_DRAIN` per round, stopping at a `sendto` failure, the closing line `console.send` | `tunnel/data_sender.*`, `loop.*`, `main.cpp` |
| Test-build instrumentation `data.tx`, `data.rx`, echo responder. The executable compiles it only with `SANGTACHI_TEST_BUILD` | `testbuild/instrument.*`, `client/CMakeLists.txt` |
| Module rows and the directory tree for `tunnel/data_sender` and `testbuild/instrument` | `architecture.md` 3.1, Chapter 10 |
| `[slow]` tests run alone (below) | `tests/CMakeLists.txt` |
| Removed H | `plan.md` |

## Key decisions

**Small points the documents did not define were decided in code.** They are in each header's "decided by this code" table with their basis.

- In `send`, the word containing a dot is `to`. A size below 100 cuts the front of the 100-byte layout
- A new `send` while a job runs is not accepted (`send_busy`). If two jobs' packet numbers mix, the comparison script cannot line up the lines
- If the slot picked by `to` has no session, the command stage does not filter it; 8.5 counts it as `tx_drop_no_route`
- The own virtual IP of 8.5 check 4 is the value of the last session created and is forgotten on leaving the room
- When `DATA` goes to every candidate before learning, it stops where `sendto` fails. `KEEPALIVE` and `PING` keep going to the remaining candidates
- Echo replies are queued and sent in the loop's send share before job packets. Nothing is sent again during receive processing. Only the synthetic layout (IHL 5) is echoed

**The product build was confirmed to contain no test instrumentation.** The product executable has zero occurrences of the string `data.rx`.

**`[slow]` tests run alone.** Once this slice's tests were added, the timer-under-load test failed almost every time under the parallel run of
`scripts/test.ps1` (-j 16) (worst interval 750ms, limit 400ms). Run alone it is 219ms both before and after this slice. The pre-slice code passed under
the same conditions in a worktree, so the cause is taken to be more processes running during those 10 seconds as the test count grew. A timing test that
competes with other processes for CPU measures that competition, so it runs alone with `RUN_SERIAL`. The limit (30 seconds) is unchanged.

## Verification

| What | Result |
|------|--------|
| Full test run (`scripts/test.ps1`) | 533 unit tests passed, 0 cli/e2e/lobby check failures, `all tests passed`. Run on DynamoDB local |
| New tests | Every row of the routing case table, session creation refusals, slot emptying order, 8.5 order and counter scope (including overlapping faults), the transmit observer timing, `send` argument case table, filtering, split sending, size and clock, stop on failure, echo order, the synthetic layout table, the echo responder, `data.tx`/`data.rx` lines, the loop's `send` wiring and leftover-work busy |
| Mutation (clean copy) | Round 1: 36, round 2: 10, round 3: 2. In the end all were killed, except one equivalent change rejected as below. Of the six round-1 survivors, four were killed by added tests (network-address slot, overwriting a taken slot, own virtual IP on leaving the room, counting a non-`CONNECTED` session as a destination), and one was a mutation that changed nothing, rebuilt and killed (fixed packet number). The mutation removing the echo checksum recomputation is equivalent: swapping source and destination leaves the ones' complement sum unchanged, and the changed direction mark is outside the header. In round 2 the mutation removing the echo IHL condition survived and a test was added. The mutation removing `busy_ ? 0 :` was at first killed only by the leftover-work test hitting the 30-second limit. The test's guard timer was made periodic so it fails instead of hanging. Because this mutation is killed, the "`EventLoop` wait" row of `plan.md` was removed |
| `docgate.py`, `platformgate.py` | Both `VERDICT: pass` |

## Cross-model review

Codex ran with two separate lenses (contract and correctness; test adequacy and records).

| Finding | Resolution |
|---------|------------|
| blocker. When `DATA` goes to every candidate before learning, it keeps sending after a `sendto` failure. The `send` contract stops at the failure | Accepted. `Session::send_data` stops at the failure. Test and mutation |
| warn. `data.tx` is emitted only after a successful send. The documents put it just before encapsulation | Accepted. The observer slot moved to the tunnel and is called right after 8.5 passes, before sending. Test and mutation |
| warn. For a header with options (IHL 6 or more), the echo refills the checksum over the first 20 bytes only | Accepted. Only the synthetic layout (IHL 5) is echoed. Test and mutation |
| warn. No test of 8.5 order with overlapping faults | Accepted |
| warn. Not checked that the session picked by `to` actually encapsulates | Accepted. Checked by session epoch and inner destination |
| warn. Not checked that a filtered command sends nothing | Accepted |
| warn. No test of echo order, failure isolation, busy with only a queue | Accepted |
| warn. No test of the command's size and per-packet clock | Accepted |
| warn. No test of fanout to every candidate before learning and its failure | Accepted |
| warn. No test that leaving the room forgets the own virtual IP | Accepted |
| warn. The new timing test lacks `[slow]` and is exposed to parallel contention | Accepted |
| warn. `plan.md`'s "removing `busy_ ? 0 :` passes everything" conflicts with the new test | Accepted. Confirmed by mutation and removed that row |
| warn. No test that send work runs after timers | Rejected. That order is not a documented contract. Only busy and the budget are |
| warn (the second lens raised the same `data.tx` timing again) | Same as the accepted item above |

The fix-confirmation review was `LGTM - no blockers`.
