# Phase 6 virtual adapter (ADR 0027)

## Why

The next item in `plan.md` was Phase 6. The repository owner asked for Phase 6 to be carried through to the end. The previous
commit (ADR 0026) closed the three items before starting, and ADR 0027 decided the points the documents left open during
implementation.

## What changed

| What | Where |
|------|-------|
| The build downloads the Wintun zip and judges the zip hash, the DLL hash, and the signature `Status` and `Subject`. It generates the expected-values header for the run-time check and places `wintun.dll` and the subnet conflict script next to the executable | `cmake/Wintun.cmake`, `client/CMakeLists.txt` |
| Adapter constants (name, tunnel type, GUID, range) and the face the loop sees, `adapter::Port` | `client/include/sangtachi/adapter.hpp` |
| Wintun and IP Helper. Administrator check, running the subnet conflict script, DLL hash check and loading, adapter creation, MTU and DAD, leftover address cleanup, session, read and inject, address and route | `client/include/sangtachi/platform/tun.hpp`, `client/src/platform/win32/tun.cpp` |
| Rank 2 of the wait set, `drain_wintun`, `inject_packet`, budget and `busy` | `loop.hpp`, `loop.cpp` |
| Counter `rx_err_adapter`. Count 56 -> 57 | `counters.hpp`, `counters.cpp`, `counters_test.cpp` |
| Action `AssignAddress`. `LeaveRoom` removes the address. Candidate collection receives our own Wintun GUID | `lobby.hpp`, `lobby.cpp`, `runner.hpp`, `runner.cpp` |
| Startup argument `--no-adapter` | `args.hpp`, `args.cpp`, `args_test.cpp` |
| ICMP echo responder of the test build | `testbuild/instrument.hpp`, `instrument.cpp`, `instrument_test.cpp` |
| Startup steps, adapter lifetime, steps (5)(6) of shutdown | `main.cpp` |
| Adapter MTU constant 1400 (`protocol.md` chapter 12) | `protocol_constants.hpp` |
| Wait, drain, budget, EOF, ring corruption, other errors and inject counters with a fake adapter | `tests/adapter_loop_test.cpp` (new file) |
| Same-machine tests pass `--no-adapter`. cli-check checks the startup failure when not elevated | `cli-check.ps1`, `e2e-check.ps1`, `harness-lib.ps1` |
| Phase 6 verification run in an administrator shell | `scripts/adapter-check.ps1` (new file) |
| `--no-adapter` input, module table, chapter 7 Adapter lifetime, chapter 10 target structure | `architecture.md` |
| Valid range of rank 2, `rx_err_adapter`, read path and injection deferred to Phase 7, state ownership | `concurrency.md` |
| Verification status and recorded constants of sections 1, 2, 3, 4 and 7, counts in section 0 | `windows-prereq.md` |
| Phase 6 verification script, Phase 7 firewall work | `roadmap.md` |
| Decisions and rejected alternatives | ADR 0027 |
| Next work becomes Phase 7. `--peer`/`raw` decision pending. Removed the slot for our own Wintun identifier | `plan.md` |

## Key decisions

The decisions and rejected alternatives are in ADR 0027. This record only notes the other judgements.

- **The tunnel type was changed to `Wintun`.** After the first administrator check, adding the subnet conflict script's own-adapter
  exclusion to the check made it fail. The adapter description was `Sangtachi Tunnel`, so the script excluded nothing
  (`not-wintun`). Instead of widening the script, the tunnel type was changed. The client now checks before creating the adapter,
  so this defect does not show at startup. It only makes a difference when a second startup on the same machine overlaps a process
  that holds the adapter
- **`--peer` and `raw` were not removed.** `architecture.md` 3.5 and `concurrency.md` chapter 1 said they go away in Phase 6, but
  `roadmap.md` has no such work and the Phase 1 verification script runs on the two. `plan.md` holds it pending the owner's decision
- The adapter-side events (`adapter.open`, `adapter.address`, `adapter.error`, `adapter.close`) are not fixed keys of chapter 9.
  The verification script reads them, but the fixed key table was not extended. The decision is made by OS queries and counters
- So that a failed address cleanup at shutdown gives one `WARN` line, `TunDevice::close` returns that failure and `main.cpp` emits
  the line. This keeps that row of the `architecture.md` chapter 7 table

## Confirmed facts

- Wintun 0.14.1 uses the requested GUID as the interface GUID as is. The GUID in `adapter.open` and the `InterfaceGuid` of
  `Get-NetAdapter` matched the constant
- When a second process creates with the same GUID, `WintunCreateAdapter` fails
- After a forced kill, no adapter with that GUID remains. Starting again brings the adapter and address back
- The network category of the new adapter is `Public`
- Attaching a `/24` address creates the on-link route, so none was created separately (`adapter.address route=on_link`)
- Wintun writes the adapter description as `<tunnel type> Tunnel`
- While the adapter was being deleted, `Get-NetAdapter` once gave a "registry key marked for deletion" error. The verification
  script asks again on the next round
- The OS sends packets as soon as the adapter is up, so `adapter_rx` was already non-zero before the ping. The check looks at the
  increase
- Windows PowerShell 5.1 cannot load modules when it inherits pwsh's `PSModulePath`. The build, the client and the verification
  script all remove that value before calling it

## Verification

- `pwsh -NoProfile -File scripts/test.ps1` passed. 627 ctest cases and the cli, e2e, lobby and session checks. cli-check gained
  the startup-failure check when not elevated
- `scripts/adapter-check.ps1` was run four times in an administrator shell. The first run passed entirely. The second, which added
  the subnet exclusion check, failed that one check (the tunnel type above), and the fixed third run passed entirely. The fourth
  run, after the review fixes, also passed all 43 checks
- All eight mutations were caught. Removing rank 2 from the wait set, removing the adapter from `busy`, dropping the ring slot
  release, counting ring-full as `drop_inject_error`, dropping shutdown on EOF, dropping the ICMP checksum refill, narrowing the
  fragment check, dropping the ICMP checksum check
- Changing the zip hash, the DLL hash and the `Subject` in `cmake/Wintun.cmake` one at a time stopped the configure. Restoring
  them let the build through
- `python tools/platformgate/platformgate.py` and `python tools/docgate/docgate.py` passed

## Cross-model review

The staged diff was shown to Codex twice. Once to look at Win32 and Wintun API use, the loop's drain and budget, shutdown order,
and mismatches between docs and code. Once assuming a defect exists, to look for stale sentences, mirror mismatches, PowerShell and
CMake defects, and tests that cannot fail. There were thirteen findings, all `warn`. Merging the three both reviews raised gives
ten. All were applied.

| # | Finding | Handling |
|---|---------|----------|
| 1 | `clear_address` forgets the address and route even when deletion fails, so the next call cannot retry (both reviews) | Applied. It forgets only on success or `ERROR_NOT_FOUND` |
| 2 | Shutdown deletes the address before the session, the reverse of steps (5)(6) of `concurrency.md` chapter 7 | Applied. `close` runs session, address and route, adapter in that order and returns the failure. `main` emits the `WARN` from it |
| 3 | CMake puts the DLL path into a PowerShell single-quoted string as is (both reviews) | Applied. Single quotes are doubled |
| 4 | The documented startup order (hash then subnet) differs from the code (subnet then hash). "Section 3 cleanup first" and cleanup after adapter creation also look inconsistent (both reviews) | Applied. The docs follow the code order and state why our adapter does not exist at the subnet check |
| 5 | The sentence that no adapter remains after a forced kill reads as a guarantee, while the check accepts `Not Present` | Applied. Rewritten as an observation, and the check narrowed to "no adapter with that GUID" |
| 6 | The subnet script is copied after linking, so a build that only changed the script leaves a stale decision | Applied. Switched to `configure_file` |
| 7 | `concurrency.md` chapter 3 said the console handle and thread go away in Phase 6 | Applied. Now says they stay in the console build. The sentence predates this commit |
| 8 | The sentence that `--peer` and `raw` go away in Phase 6 does not match the pending decision in `plan.md` | Applied. `architecture.md` 3.5, `concurrency.md` chapter 1 and the `loop.cpp` comment now say "not decided" |
| 9 | The head of `windows-prereq.md` still says the adapter does not exist and Phase 1 implementation has not started | Applied. Fixed the two paragraphs of section 0 and the last sentence of section 14 |
| 10 | `roadmap.md` says `adapter-check.ps1` runs all items above, but it does not run the configure rejection items | Applied. The first item is listed separately as configuring again |

The review fixes alone were shown to Codex once more. Two findings, both `warn`.

| # | Finding | Handling |
|---|---------|----------|
| 11 | `configure_file` copies into `client/`, but Visual Studio and Ninja Multi-Config put the executable in `client/<config>/` | Rejected. This repository builds only with the single-config Ninja of `scripts/build.ps1`, and the `CMakeLists.txt` comment states that premise. No mechanism for other generators was added |
| 12 | "Only item 2 is not implemented" in `windows-prereq.md` section 14 leaves out the Qt bundling of item 4 (mirror included) | Applied. The end of section 14 and the section 0 paragraph now say Qt bundling is not done yet |
