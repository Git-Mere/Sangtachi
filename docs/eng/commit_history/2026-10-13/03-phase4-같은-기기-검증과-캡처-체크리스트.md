# Phase 4 Round 4 K: Same-Machine Verification Script and Capture Checklist

## Why

This is the last piece of Phase 4 round 4. J connected the rendezvous and the lobby to the session, so processes on the
same machine create a room, punch, and reach `CONNECTED`. The items of `roadmap.md` Phase 4 verification that need
several processes and the control server now run as a script, and the items that can only be judged from a packet
capture are left as a procedure a person runs.

## What Changed

| What | Where |
|------|-------|
| Same-machine session check. Scenarios: `connect`, `data`, `host-stays`, `room-ended`, `departed-now`, `departed-retry`, `punch-fail`, `handshake-fail`, `peer-close`, `records`. The full test run runs it after lobby-check | `scripts/session-check.ps1`, `scripts/test.ps1` |
| Moved the functions lobby-check and session-check share (reading logs, starting processes, starting and stopping the harness, the test build). lobby-check reads them | `scripts/harness-lib.ps1`, `scripts/lobby-check.ps1` |
| Fake peer. Admin commands `fake_host`, `fake_player`, `fake_clear` with modes `silent`, `ack_only`, `hello_only`, `close01`, `close03`. It joins a room through the harness's HTTP, registers the LAN address as a candidate, and speaks a little of the tunnel header | `control-server/tests/harness/cp_harness.py`, `control-server/tests/test_harness.py`, `control-server/README.md` |
| Three log events. `session.endpoint` (the learned address), `session.rtt` (a `PONG` sample), and the test build's `data.echo` (an echo round trip) | `peer/session.cpp`, `testbuild/instrument.*`, `main.cpp`, `architecture.md` chapter 9. Korean and English |
| The send time of the synthetic packet is filled from the RTT clock | `main.cpp`, the layout table of `roadmap.md` Phase 4 test fixtures. Korean and English |
| The manual procedure for items judged only by capture, and a table mapping each roadmap item to where it is judged | `tools/phase4-capture/README.md` |
| A paragraph pointing to the two places | `roadmap.md` Phase 4 test fixtures. Korean and English |
| Tests | `tests/peer/session_test.cpp`, `tests/testbuild/instrument_test.cpp` |
| Rewrote the next tasks | `plan.md` |

## Key Decisions

**The learned address goes into the log.** The same-LAN item of `roadmap.md` says "학습된 `peer_endpoint` 가 상대의 로컬 후보인
것을 로그로 확인한다", but no line carried it. The capture piece found this. Instead of changing the document to "by
capture", `session.endpoint` was added. It is judged on one machine and is read the same way on two PCs.

**The RTT baseline's send time is the RTT clock.** It was first filled from the timer clock (`GetTickCount64`). Its
resolution is about 15ms, so a same-machine echo measured only 0 or 15, and once the median came out 15ms and differed
from the `PING` median (0.5ms) by more than 10ms. `concurrency.md` chapter 4 says "only RTT measurement uses
`QueryPerformanceCounter`", so the field is filled with that clock's milliseconds and the layout table says so.

**The fake peer lives inside the harness.** It calls the control server over HTTP and runs in the same process like the
STUN responders. Scenarios create it with admin commands instead of restarting the harness. The server code
(`controlplane/`) was not changed and is not part of the deployment.

**Items judged only by capture are run by a person.** This machine has no Wireshark or Npcap, and pktmon has no
administrator rights. The procedure has not been run, and the README says so. Its first run checks which interface
the packets appear on. Seeing renegotiation, no response in a terminal state, provisional paths, and the unverified
budget on the wire needs an injector the current modes cannot make, so those are written as on hold (C6 to C9).

**Small things the documents do not fix were decided where they live.** The fake peer in the table at the head of
`cp_harness.py`; the capture tolerance (`T - 50ms` to `T + 200ms`) and the observation window (10 seconds) in the
README's "decided by this document" table, with their grounds.

## Verification

| What | Result |
|------|--------|
| Full tests (`scripts/test.ps1`) | 592 unit tests passed, 0 failures in the cli/e2e/lobby/session checks, `all tests passed`. Run on DynamoDB local |
| Control server tests | `pytest -q --store` 1024 passed. Run with `PYTHONUTF8=1`; without it, four tests already failed before this change because of this machine's cp949 standard error |
| session-check scenarios | All passed. The learned address was the local candidate (`10.0.0.49`), not the reflexive address. The RTT baseline was a `PING` median of 0.417ms against an echo median of 1.000ms. From the session's end to the server's `peer.released by=host` took 65ms |
| Fake peer | Each mode gave the expected result on a real client (`HOLE_PUNCH_TIMEOUT`, `PEER_HANDSHAKE_FAILED` twice, record lines `ABORTED` and `PEER_FAILED`). All 6 mutants of the harness tests were caught |
| Discriminating power of session-check (client mutants in a clean copy) | All 8 caught. Removing the immediate report (`departed-now`), a player not going to the lobby after a failure (`punch-fail`), writing this side's close before establishment as `PEER_FAILED` (`peer-close`), always going to the lobby when the room ends (`room-ended`), clearing `departed` on a failed report (`departed-retry`), removing the echo responder (`data`), `session.endpoint` carrying the reflexive candidate (`connect`), and the `<peer>` of the `FAIL` line always `-` (`punch-fail`) |
| `docgate.py` | `VERDICT: pass` |

**One `probe200` failure in the full run.** The lobby-check scenario `loop-alive` check `held: every probe200
elapsed_ms <= 400` failed twice (453ms, 485ms). Both times subagents were building and testing on the same machine at
once. With the machine idle, that scenario alone ran twice and passed both times with a maximum of 219ms, and the full
run passed. This change does not touch the path that scenario takes (a pending `create_room`).

## Cross-Model Review

Codex ran with two lenses separately (contract and correctness of the same-machine verification, document consistency).

| Finding | Handling |
|---------|----------|
| blocker. When the echo median is 0 the relative difference is 0, so the RTT baseline passes anything | Fixed. Then only the absolute difference counts |
| warn. `room-ended` does not check `DATA` round trips after the room ended | Fixed. That room runs on the test build and both players' echoes are checked |
| warn. `departed-retry` fails before the server processes and ends the second session after the failed response, so two separate successful calls also pass | Fixed. The second session ends while the first immediate report is held for 2 seconds, and both release lines must appear up to the line of the first successful request after the failed one. Holding for 3 seconds first let the client's 3-second receive limit cut in and send the next request, so the server saw two overlapping requests. The hold is now 2 seconds and the reason is written down |
| warn. `records` discards the result of the timeout wait and does not check the result values of the record lines | Fixed. The wait is a check, the fake pair has one `HOLE_PUNCH_TIMEOUT` line, and the real pair has `OK` and `CLOSED` |
| nit. The log table has seventeen keys but says "fourteen" | Fixed, in Korean and English |
| warn. The mapping table overstated the coverage of `room-ended` and `departed-retry` | Fixed. `DATA` is covered by the fix above. `departed-retry` covers only a failure before server processing; losing only the response after processing does not show in the server log because reclaim is idempotent, so only the unit tests see it |
| nit. A sentence called "roadmap 원문" was not verbatim | Fixed. The original is quoted as is |

The fix-confirmation review was `LGTM - no blockers`.

The final confirmation review (the whole staged change) raised one warn. C4-3 and C4-4 of the capture README need two
peer candidates to tell "to that address" from "to every candidate", but the fake peer registers only one. Fixed. Those
two scenes judge only the count and the `reason`, write the destination as an observation only, and leave the
destination judgment of an establishing session on hold. The record form was aligned too. The confirmation review of
that fix found two more mismatches between the table and the record form, and after fixing them it was `LGTM - no blockers`.
