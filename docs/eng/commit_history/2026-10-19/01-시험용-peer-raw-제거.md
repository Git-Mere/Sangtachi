# Remove the test-only `--peer`, `raw`, and `rx.raw` (ADR 0028)

## Why

The first next step in `plan.md` was to decide whether and when to remove the test-only `--peer` and `raw`. The repository owner decided
to remove both. For the two related choices the owner took the recommendation: `rx.raw` is removed too, and `scripts/e2e-check.ps1` is
kept and reduced. The decision and the rejected alternatives are ADR 0028.

## What changed

| What | Where |
|------|------|
| `--peer` parsing and `Args::peer` | `args.hpp`, `args.cpp`, `main.cpp` |
| The `raw` command, `send_raw`, the test byte string, the `rx.raw` line, `LoopOptions::peer` | `loop.hpp`, `loop.cpp` |
| `sha256_short` and `kShortHashChars`, used only by `rx.raw` | `hash.hpp`, `hash.cpp` |
| Removed the `--peer` cases and added one case where `--peer` is `UnknownOption` | `args_test.cpp` |
| Removed the three `raw` tests. Removed the short-form test and moved the different-digest test to `sha256_hex` | `loop_test.cpp`, `hash_test.cpp` |
| Three comments | `udp_socket_test.cpp`, `tunnel_test.cpp`, `lobby.hpp` |
| Moved the `--peer` cases to other arguments with the same error token, and one case where `--peer` is an unknown argument | `cli-check.ps1` |
| Removed the send/receive comparison, the third process in the chain, and the failed-send checks. Startup and bind of two processes, `counters`, an unknown command and `quit` remain | `e2e-check.ps1`, `test.ps1` comment |
| The 3.5 test-input table and the `rx.raw` section became a console command table. The `--peer` format row, the chapter 9 `rx.raw` row, and the uses of `hash` and CNG | `architecture.md` |
| The open sentence on `[console]` in chapter 1, the `raw` backlog sentence in chapter 3 | `concurrency.md` |
| The judging means of the Phase 1 verification, the command count in the Phase 4 test harness, this removal as a Phase 7 task | `roadmap.md` |
| Removed the pending-decision row and moved the order up. The `e2e-check.ps1` mention in the Phase 1 paragraph | `plan.md` |
| Decision and rejected alternatives | ADR 0028 |

## Important decisions

- **The two items of the `roadmap.md` Phase 1 verification were not removed.** Only the judging means was rewritten. The `DATA`
  round-trip match of Phase 4 now checks the same property. "On two machines each side sends once first" was the procedure for running
  two machines with `--peer`, so it went too. Hole punching already has both sides send at the same time
- **The `hash` module was kept.** The `wintun.dll` hash check and `data.tx` and `data.rx` in the test build use `sha256_hex`
- **The lost checks are written in the ADR.** The end-to-end test that checks byte match between two processes on one machine without
  administrator rights is gone. The failed send is now checked by the failed-send test in `loop_test.cpp` instead of `e2e-check.ps1`.
  A review finding added the `socket.error` line and console handling after the failure to that test

## Verification

- `pwsh -NoProfile -File scripts/test.ps1` passed. 622 ctest cases and the cli, e2e, lobby and session checks
  - In the first full run, thirteen `sizes` checks in `session-check.ps1` failed. The host's `timer.tick` showed `elapsed_ms=191859`:
    the loop did not run for about 192 seconds, the session hit the idle deadline as `TUNNEL_DROPPED`, and the later `send` was rejected
    as `not_connected`. This change does not touch that path, and rerunning only `session-check.ps1` passed everything. Taken as the
    machine stalling for a while
- Both mutants were killed
  - Making `--peer` an argument that takes a value again fails `args: reject case table`
  - Removing the `socket.error` line of a failed send fails `loop: send_datagram counts and logs a failed send`
- `python tools/docgate/docgate.py` and `--claims` passed

## Cross-model review

The staged diff was shown to Codex twice. Once for correctness of code, tests and scripts and for lost tests. Once for stale descriptions
across the whole repository, mirror mismatches, and whether the claims in the ADR and `roadmap.md` match the code. There were four
findings, all `warn`. All were applied.

| # | Finding | Disposition |
|---|------|------|
| 1 | Removing the failed-send checks from `e2e-check.ps1` leaves no test of the `socket.error` error code and loop survival. The unit test checks only `tx_err_send` | Applied. The failed-send test checks `op` and `code` on the `socket.error` line and that `counters` is handled after the failure |
| 2 | The ADR says the failed-send test checks that the loop keeps running, but that test never runs the loop (mirror included) | Applied. Widened the test by 1 and aligned the ADR sentence with it |
| 3 | "No path sends to any address without a session" is too broad. `--stun` sends Binding Requests to user-given addresses before a session (mirror included) | Applied. Narrowed to: no path sends arbitrary bytes, and only Binding Requests go to `--stun` addresses |
| 4 | The `lobby.hpp` comment still lists `raw` as a console command | Applied. Changed to `send` |
