# Phase 5 wrap-up: boundary sizes, test gaps, long-running checks

## Why

The last Phase 5 item of `plan.md` and the remaining verification items: the boundary-size table, the places where the Chapter 15
comparison found no test, the 10-minute control-plane outage, the idle timeout, and the 1-hour hold.

## What changed

| What | Where |
|------|------|
| Boundary-size unit test. 19 is `drop_type_length` on receive (process table), 20, 576, 1452 reach the payload hook byte for byte, 1453 is `tx_drop_oversize` on send | `tests/tunnel/tunnel_test.cpp` "the Phase 5 boundary sizes" |
| Scenario `sizes`. With two test-build processes, 20 one way and 576 and 1452 round trip are compared by `len` and `sha256` of `data.tx`/`data.rx` | `scripts/session-check.ps1` |
| Four test gaps. `drop_terminal_state` when receiving in `FAILED`, the routing slot cleared on a received `CLOSE`, the epoch of a new attempt, routing case table row 4 (no route to another player even after a `ROSTER`) | `tests/tunnel/tunnel_test.cpp` |
| Long-running check script. Scenarios `idle-drop`, `cp-down`, `hour`. A fresh harness per scenario | `scripts/soak-check.ps1` (new file) |
| The two-home observation procedure for the 1-hour hold (test 5). Removed the header sentence "captures C6-C9 are not here" | `field-test/README.md` |
| The name of the separate script, wording that points at field-test test 5 | `roadmap.md` Phase 5 verification |
| Phase 5 done, next work is the Phase 6 pre-start items, the condition of the two-home observation wait, the test-gap item reduced to the `pending_pings` cap | `plan.md` |

## Important decisions

- 19 and 1453 are sizes `send` cannot build (20-`MAX_INNER`), so unit tests cover them. They are checked at the place the roadmap table
  expects (19 on receive, 1453 on send)
- `idle-drop` force-kills the peer process so its sending stops without `CLOSE`. It checks the surviving side's `TUNNEL_DROPPED`,
  `CONNECTED` → `FAILED`, the `FAIL` line, and the end line of the record file, and checks through both the client log and the harness
  log that the player does not join again for 20 seconds (9.6 no automatic retry)
- `cp-down` brings the harness down with an admin command. Then the host's `host_report` failing confirms it really went down
- The loss verdict of `hour` is `lost=0`. It is a verdict that expects no loss for traffic within one machine, and this run observed exactly that. It does not mean loss is impossible on one machine
- The `pending_pings` cap cannot be reached with the current configuration (interval 5 seconds, lifetime 5 seconds). The configuration
  was not changed for a test; the item stays in `plan.md`

## Verification

| What | Result |
|------|------|
| `pwsh -NoProfile -File scripts/test.ps1` (with DynamoDB local) | `all tests passed`. ctest 613, the `sizes` scenario's 14 checks passed |
| `pwsh -NoProfile -File scripts/soak-check.ps1` (defaults 10 minutes, 60 minutes) | `0 checks failed, 0 not run`. 73 minutes. `idle-drop` 11, `cp-down` 6 (three echoes each way every 30 seconds, 20 rounds, 0 misses), `hour` 5 (five echoes each way every minute, 60 rounds, 0 misses, `lost=0` in both `session.loss` lines) |
| Do the `soak-check` verdicts really fail | In the first run, two verdicts that had the field order of the `session.failed` line wrong came out as failures. They passed after the fix |
| `docgate.py`, `--claims` | `VERDICT: pass` |

## Cross-model review

Codex, with the lenses: can `soak-check` pass vacuously, hang, leak processes, or misjudge; the `sizes` scenario; the new unit tests;
field-test test 5 and the wording.

| Finding | Handling |
|------|------|
| warn. field-test test 5 asks for "the time of the last `KEEPALIVE` (`timer.tick`)", but log lines have no time and `timer.tick` is the elapsed time of our own timer, not a receive time | Accepted. A now keeps a capture on as in test 1, and the time comes from the capture. Added `t5.pcapng` to the collection table |

The final confirmation of the whole staged diff raised one warn: stating the basis of `lost=0` in `hour` as "loopback, so there is no way to lose packets" goes too far. Accepted. It now says it is the value expected and observed within one machine (record in both languages, `soak-check.ps1` comment). The final confirmation after the fix is `LGTM - no blockers`.
