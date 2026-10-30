# Decisions before Phase 8 and a minimal GUI

## Why

On starting Phase 8, the five things `roadmap.md` says to decide before starting (the GUI thread model, the startup input path, the relation to
the rate limit, the timing of privilege elevation and leftover cleanup, and the Qt distribution form) and the gaps found while reading the documents were decided
with the repository owner. The decision is
[ADR 0034](../../decisions/0034-decisions-before-phase-8.md). A Qt Widgets GUI was built according to that decision.

On 2026-10-28 the owner ran one Minecraft session with a friend using `field-kit-p7b`. Its result was put in the new `experiments.md` 3.1.

## What changed

| What | Where |
|------|------|
| Decisions before starting and the rejected alternatives | ADR 0034 |
| Threads of the GUI build (`[gui]` is the main thread, `[loop]` is a worker thread), the screen queue, shutdown in the GUI build | `concurrency.md` chapters 1, 2, 6, and 7 |
| Startup inputs of the GUI executable, the mapping of console commands to the window, the member list, the module table and the repository tree | `architecture.md` 3.1, 3.5, and chapter 10 |
| The GUI does not block separately on the window side | `control_plane.md` 6.4 |
| The manifest of the GUI executable, the Qt runtime (distribution, how to build it, size), partial measurement of the Minecraft connection | `windows-prereq.md` sections 1, 4, 5, and 14 |
| Pre-start items to ADR 0034, the nat-probe re-measurement to a client rehearsal, mapping lifetime, the 3-second measurement in both builds, Wi-Fi drop recording, the block and chat criterion, aggregation and graphs for Phase 9 telemetry | `roadmap.md` Phase 8 and 9 |
| The measured results document is created now. Adapter MTU and the first Minecraft session | `experiments.md` |
| Removed the broken-link exception for `experiments.md` | `tools/docgate/`, a stale fact in `CLAUDE.md` |
| Phase 8 measurement list | `field-test/required-test.md` |
| Moved startup through cleanup into `run_client_process(Args, ClientUi&, shutdown event)`. The console and GUI executables each compile it | `client/src/client_process.cpp`, `main.cpp` |
| The player's last roster, the host roster accessor, the lobby's accessors for the screen and the last failure | `peer/session`, `tunnel/roster_publisher`, `control/lobby` |
| Screen state and the screen queue, display logic, the GUI-side `ClientUi`, the `WM_ENDSESSION` decision | `ui/view`, `ui/gui_ui`, `platform/session_end` (no Qt) |
| The Qt window and the GUI executable. `SANGTACHI_BUILD_GUI` (off by default) | `ui/main_window`, `ui/gui_main.cpp`, CMake |
| The release GUI build and gathering the distribution, the closure check, the LGPL notice | `scripts/package-gui.ps1` |

## Key decisions

- The screen queue carries the whole state the window shows, not events. Even when it is full, the last state is not dropped
- `[loop]` does not wake `[gui]`. A Qt queued call takes the event queue lock and brings a lock into `[loop]`. `[gui]` reads every 100ms
- Leaving out the Qt components the window does not use took the distribution from 58 files, about 94MiB, to 16 files, 30,155,983 bytes. The VC++ runtime goes in as DLLs
- 10 values and behaviors that were not in the documents were shown to the owner and kept as they are: 2 of the 4 screen queue slots reserved, one more iteration after 100ms while holding an entry, the `ERROR`
  line at 512 bytes, buttons off before ready, the last `ERROR` line, a dialog on startup failure even if closed during preparation, other rejection guides hidden, spaces not
  trimmed, check failure wording the same as the console, window text in English

## Verification

| What | Result |
|------|------|
| `scripts/test.ps1` | ctest 661/661, cli-check 43/43, e2e, lobby, and session failures 0. `/W4 /WX` warnings 0 |
| Mutation | All 18 for the screen queue and display logic and 15 on the GUI side were caught by the tests |
| `tools/platformgate` | `VERDICT: pass` |
| GUI smoke (not administrator) | The window comes up with the `not_elevated` startup failure dialog, exit code 2. The same with the reduced distribution |
| `scripts/package-gui.ps1` | The closure check passes. Confirmed that the server address is embedded in the executable |
| `docgate.py` | `VERDICT: pass` |

**Not confirmed.** The GUI paths that need administrator rights and the server (ready, creating and joining a room, the member table, closing, ending on its own, logoff),
the input prompt and guides in a console window, and a PC without the Qt and VC++ runtimes. Moved to `plan.md`.

## Cross-model review

Codex. The first round ran three lenses separately (threads and shutdown, preserving console behavior and the display logic, document consistency).

| Lens | Finding | Handling |
|------|------|------|
| Threads, behavior (both) | blocker. The first screen state is passed only after one loop iteration, so when the lobby has no timer the GUI stays at `Loading...` | Applied. The first state is passed before entering the loop. Written in `concurrency.md` chapter 6. There is no automated test. That code is outside the test executable, so it was handed to the owner's check |
| Documents | warn. `concurrency.md` said the worker thread resolves the control server name. `[control]` resolves it | Applied (Korean and English) |
| Documents | warn. The rule for when buttons are enabled overlapped between `control_plane.md` 6.4 and `architecture.md` 3.5 | Applied. 6.4 points to 3.5 |
| Documents | warn. The `plan.md` handover was stale | Applied |

The second round ran only on the changed parts (the packaging script, the first-screen-state fix).

| Lens | Finding | Handling |
|------|------|------|
| Packaging | warn. When the build directory is reused, a `SANGTACHI_TEST_BUILD=ON` left in the configure cache can leak into the distribution build | Applied. Configuring turns off `SANGTACHI_TEST_BUILD` and `SANGTACHI_ASAN` |
| Packaging | warn. Even if `-QtDir` changes, linking can use the Qt path in the cache | Applied. Configures with `--fresh` every time |

While syncing the English mirror, more Korean defects were found and fixed. The count of verification states in `windows-prereq.md` 0,
the "Qt bundling not done yet" sentences in 0 and 14, and the distribution size item in "What was not confirmed" of ADR 0034.

The third round ran once more over the whole staged diff to be committed. It returned `LGTM - no blockers`.
