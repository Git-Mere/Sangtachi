# Remove the "places to check during implementation" section from plan.md

## Why

Phases 1-9 are done. Only stretch goals remain. Every row in this section was a conditional note for "when that code is changed again", not a next task. The owner decided these can be considered when working on stretch goals. The content is not moved to code comments; it is kept only in this record.

## Removed content

| Place | What | When it was meant to be checked |
|------|------|------|
| Timeout in `control-server/tests/mutate.py` (`telemetry-server/tests/mutate.py` uses the same runner) | On hitting the limit it kills only the pytest process. Server processes started by that test can remain. Reaping grandchildren needs a Windows Job Object | When the mutation runner is changed again |
| `pending_pings` cap | No test reaches the branch in `protocol.md` 5.5 "if it is still full, do not send a new `PING`". PING interval and entry lifetime are both 5 seconds, so the table never holds more than one entry | When the chapter 11 PING interval or the 5.5 entry lifetime changes |
| Send buffer of console `send` | When the send buffer fills, it ends the job with `WSAEWOULDBLOCK`. At 576 and 1452 bytes only 45-133 packets go out at once. If sending continues on the next loop iteration, the set count is reached. Byte comparison does not rely on this (ADR 0032) | When a test needs console `send` to reach an exact count |
| Location of `is_valid_name` | The `control_plane.md` 2.7 check lives in `tunnel/packet.hpp`, so startup arguments and control plane code include the tunnel packet module. Moving it to a neutral header that `packet.hpp` uses gives the right dependency direction | Next time that header is changed. GUI name-field validation may be the trigger |

The row with the same name in the top table of `plan.md` was removed too. The reference to this section was also removed from a comment in `tests/timer_test.cpp`. Only a comment changed, so tests were not run.

## Verification

| What | Result |
|------|------|
| `docgate.py` | `VERDICT: pass` |
| `docgate.py --claims` | `VERDICT: pass` |

## Cross-model review

Codex (`gpt-6.1-sol`). Lenses: Korean/English parity, whether the removed content was carried over without loss, remaining references in living documents.

| Finding | Handling |
|------|------|
| The `is_valid_name` row lost "`packet.hpp` uses it" and "GUI name-field validation may be the trigger" | Applied |
| "if sending continues" became an unconditional claim | Applied. Restored the original wording |
| (re-run) A comment in `tests/timer_test.cpp` points to the removed section | Applied |

Re-ran with the same lenses after the fixes. `LGTM - no blockers`.
