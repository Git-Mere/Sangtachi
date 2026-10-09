# Phase 4 Wave 1 C: Local Candidates and Hygiene

## What changed

| What | Where |
|------|-------|
| `peer/candidates`: the 10.1 local candidate decision (active = `OperStatus` Up and `DadState` Preferred, exclusion list), the own-subnet set, the default route, the registration list (reflexive, default-route interface, the rest, dedupe, 8 entries), collection failure handling and warnings | `client/include/sangtachi/peer/candidates.hpp`, `client/src/peer/candidates.cpp` |
| `platform/interfaces`: `GetAdaptersAddresses`, `GetIpForwardTable2` and `GetIpInterfaceEntry`. Links `iphlpapi` | `client/include/sangtachi/platform/interfaces.hpp`, `client/src/platform/win32/interfaces.cpp`, `client/CMakeLists.txt` |
| Received candidate hygiene gains `0.0.0.0/8`, `240.0.0.0/4` and the own-subnet row. The hygiene and interpret functions take the own-subnet set as a required argument | `client/src/control/ops.cpp`, `ops.hpp`, `runner.*`, `main.cpp` |
| The own-subnet directed broadcast row of section 7 source hygiene. `classify_datagram` takes the set and `EventLoop` owns it | `client/src/loop.cpp`, `loop.hpp` |
| HTTP responses reject a header line with a space or tab inside the name (`control_plane.md` 3.5) | `client/src/control/http.cpp` |
| Tests | `tests/peer/candidates_test.cpp`, `tests/platform/interfaces_test.cpp`, `tests/control/{ops,runner,http}_test.cpp`, `tests/loop_test.cpp` |
| Module table, section 10 tree, section 11 IP Helper row | `architecture.md` |
| The received-hygiene row now keeps only the server part, and a row for wiring local candidate collection was added | `plan.md` |

## Important decisions

- **Actually collecting and registering local candidates is deferred to wave 4 J.** `Lobby::on_stun_done` still registers only the
  reflexive candidate. **Until then the own-subnet set on the product path is empty, so the section 7 directed broadcast row and the
  own-subnet row of received hygiene block nothing.** The remaining work is in `plan.md`. The own Wintun identifier stays empty until
  Phase 6
- **The code settled things the documents do not define.** None contradicts a documented rule
  - The route lookup API is `GetIpForwardTable2` with `GetIpInterfaceEntry`. 10.1 defines only the rule (`0.0.0.0/0`, the sum of the two
    metrics, ties go to enumeration order)
  - If the GUID conversion of one adapter fails, the whole enumeration is treated as failed. If the interface lookup of one default route
    line fails, it is treated as a route lookup failure. Both fall into results 10.1 already defines (a warning, no retry). Guessing another
    adapter without knowing the GUID could send our own Wintun address as a candidate
  - The warning event names `candidates.enumerate_failed` and `candidates.route_failed`. `architecture.md` section 9 allows adding events
- **The re-hygiene inside `control_peers_fields` runs with an empty set.** It is a log line function without the set, and hygiene only
  filters, so the interpreted result is not affected

## Verification

| What | Result |
|------|--------|
| Full test run (`scripts/test.ps1`, after merging into main) | 383 unit tests pass, 0 failed cli/e2e/lobby checks, `all tests passed` |
| Mutation (subagent) | The tests caught all 28. They include the five set by `roadmap.md` Phase 4 (M1-M5) and the per-rule mutants of the section 7 (a) layer (M6-M11). For two (M14, M18), the first form was blocked at build time by unused-argument warnings, so they were rerun in a form that avoids the warning |
| `docgate.py`, `platformgate.py` | Both `VERDICT: pass` |

## Cross-model review

The subagent ran Codex review twice in its worktree.

| Finding | Disposition |
|---------|-------------|
| warn. The `IfType` 24 row was masked by the loopback range rule, so removing the `IfType` 24 rule still passed | Applied. Added an `IfType` 24 case with an address outside that range. Confirmed with mutant M27 |
| warn. The registration dedupe test cannot catch an implementation that compares only the IP | Applied. Added a case with the same IP and a different port. Confirmed with mutant M28 |

Round 2 returned `LGTM - no blockers`.

After merging into main, Codex reviewed the staged diff once more.

| Finding | Disposition |
|---------|-------------|
| warn. With an own subnet as wide as 10.1.2.3/1, its broadcast is 127.255.255.255, and loopback, which section 7 passes, becomes `BadSource` | Applied. Loopback now passes before the own-subnet row. The case was written first and seen to fail |
| warn. Tests still pass if `drain_udp` passes an empty set instead of the loop's set | Deferred. An ordinary socket cannot produce a directed-broadcast source, so receive-path injection is needed. Wave 2 E builds that entry point. `plan.md` records that the section 7 (b) layer test includes the own-subnet row to catch this mutant |

Round 2 (confirming that fix) returned `LGTM - no blockers`.
