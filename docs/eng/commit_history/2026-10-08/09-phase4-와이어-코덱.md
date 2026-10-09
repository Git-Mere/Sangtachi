# Phase 4 Wave 1 A: Wire Codec

## What changed

| What | Where |
|------|-------|
| `tunnel/packet`: field-by-field big-endian serialization of the 4.1 header, 8.1 common checks 1-6 (`check_common`), section 5 payload codecs (`HELLO`/`HELLO_ACK`, `PING`/`PONG`, `CLOSE` reason, `ROSTER` receive checks 1-5), the `control_plane.md` 2.7 name check, and `counter_name`, which maps an error kind to its counter name | `client/include/sangtachi/tunnel/packet.hpp`, `client/src/tunnel/packet.cpp` |
| `kMaxNameLen`, missing from the section 3 constants, and the section 11 timer block | `client/include/sangtachi/protocol_constants.hpp` |
| 27 test cases | `tests/tunnel/packet_test.cpp` |
| The two retention constants of `peer/epoch` now refer to the section 11 block | `client/include/sangtachi/peer/epoch.hpp` |

## Important decisions

- **The decoders are pure functions and do not raise counters.** They return only the error kind, and the receive pipeline (wave
  2 E) raises the counter. 8.1 checks 7 (`drop_unknown_peer`) and 8 (`drop_retired_epoch`) need the session table and the retired
  list, so they belong to the pipeline
- **The section 11 timer names were given by the code.** Section 11 is only a table. Values already held by other headers (STUN,
  `get_peers`, `host_report`) were not copied. The same value is not kept in two places
- **The duplicated retention time was reduced to one when merging.** B's `peer/epoch` held its own 2 minutes. It now refers to the
  section 11 block
- **Receive-side reading of `ROSTER` `generation`.** With no previous value any value is accepted, and the comparison is unsigned `>`
  without wraparound. 5.7 does not define the sender's first value. It is settled before wave 3 I starts (`plan.md`)
- **No receive-side check for epoch 0.** "0 is never used" in 4.3 is a sending rule, and 8.1 has no receive check for it
- The three document gaps (order of per-type checks against 8.2 and 8.3, the first `generation`, duplicate name checks) were
  written into `plan.md` in the previous commit
- The `drop_roster_name` counter is not in `counters.hpp` yet. It is added, and removed from `PENDING_COUNTERS`, when wave 2 E
  wires the counters

## Verification

| What | Result |
|------|--------|
| Full test run (`scripts/test.ps1`, after merging into main) | 360 unit tests pass, cli/e2e/lobby checks `0 checks failed`, `all tests passed` |
| Mutation (subagent) | The tests caught all 46. The three that survived at first (M18, M20, M46) were test defects, and the cases were fixed. Four (M19, M21, M45, M46) were caught by debug-build bounds-check crashes, not assertions. Removing those guards turns into out-of-buffer access, which is the defect the guards prevent |
| `docgate.py`, `platformgate.py` | Both `VERDICT: pass` |

## Cross-model review

The subagent ran Codex review once in its worktree, and it returned `LGTM - no blockers`.

After merging into main, Codex reviewed the staged diff once more and returned `LGTM - no blockers`.
