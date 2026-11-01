# Log the cleanup time of a GUI logoff

## Why

The GUI side of the `roadmap.md` Phase 8 task "measure whether host shutdown fits inside the OS grace period" was measured with `mere-galaxy` as
the player. From when the player received `CLOSE` to the last write of the GUI log was about 0.11 s. But the OS grace period counts from receiving
`WM_ENDSESSION`, and that front part could not be measured because the log carries no times. A Codex review flagged it as a blocker twice, and the
repository owner chose to measure it properly. The record of that measurement is the next commit.

## What changed

| What | Where |
|------|-------|
| `end_session` keeps the time it was entered, and at cleanup completion (`cleanup_done`) one line `session_end.cleanup elapsed_ms=<milliseconds>` is written. It is not written when shutdown came from closing the window | `client/src/ui/gui_ui.cpp`, `client/include/sangtachi/ui/gui_ui.hpp` |
| The clock is taken by the constructor. The default is `platform::monotonic_ms` | Same |
| `session_end.cleanup` is added to the fixed-key table and `elapsed_ms` to the format table | `architecture.md` chapter 9 Log Output |
| The fixed-key count changes from "eighteen" to "twenty". The table already had nineteen rows | Same |

- The line is written by the worker thread. `cleanup_done` comes after `adapter.close` and before the join, so the line survives even if the join cap's `_exit` ends the process. Writing it after `[gui]` returns from its wait would lose it in that case
- The interval measured runs from `[gui]` entering `end_session` on `WM_ENDSESSION` to the worker thread entering `cleanup_done` (after `adapter.close`). Writing this line, the screen queue push, and the cleanup-complete signal that follow are not included, so it is not the whole interval `[gui]` waits. What is left out is one file write and one queue push

## Verification

| What | Result |
|------|--------|
| Two new tests | Cleanup after `end_session` gives `INFO session_end.cleanup elapsed_ms=150` (fake clock 1000 to 1150). Cleanup after a window close has no such line |
| Three mutations | Always write it, drop the subtraction, `end_session` does not set the flag. Each makes a new test fail |
| `scripts/test.ps1` | Passed |
| `docgate.py`, `--claims` | `VERDICT: pass` |

`-Full` was not run. What changed is the GUI screen side (`ui/`), not a path for which `CLAUDE.md` requires `-Full`.

## Cross-model review

Codex (`gpt-6.1-sol`). The correctness/concurrency (A) and docs/record (B) lenses ran in one pass.

| Finding | Disposition |
|------|------|
| warn [A] `elapsed_ms` stops before the line write and `publish_end`, but the docs say it runs to the cleanup-complete signal. A slow file write could push the signal past the grace period while the value stays small | Accepted. The end point is now "when preparing the cleanup-complete signal begins", and what is left out is written. The position stays: writing the line after the signal could lose it, since `[gui]` returns and the process ends |

After the fix, the full staged diff went through the same lenses again.
