# Phase 5 ASan test build and the two fuzz stages

## Why

The next Phase 5 item of `plan.md`. The ASan build judges "no memory error" in the fuzz tests (ADR 0025, `roadmap.md` Phase 5).

## What changed

| What | Where |
|------|------|
| CMake option `SANGTACHI_ASAN`. When on, `/fsanitize=address` on every target (Catch2 included), `/RTC` removed from the Debug default flags, and `/INCREMENTAL:NO` | `CMakeLists.txt` |
| `-Asan` switch. The build directory is a separate `<Config>-asan`. The on/off value is always passed explicitly so a cached value cannot silently win | `scripts/build.ps1` |
| `-Asan` switch. Runs only the unit tests with that build. The process-level scripts are already covered by the normal build | `scripts/test.ps1` |
| Fuzz stage 1. 100,000 random datagrams of 1-2000 bytes from a fixed seed are sent through a loopback socket for the loop to receive. They go out 32 at a time, and the next batch goes only after every one was counted as handled. One in eight hits the STUN classification fields (top two bits, magic cookie) | `tests/tunnel/tunnel_test.cpp` |
| Fuzz stage 2. 100,000 datagrams whose header has the right magic, version, `peer_id`, and pinned epoch, with the rest random, go into the receive injection entry point. `type`, body length, `payload_length`, and `sequence` are varied too. Half of the `DATA` bodies start as a plausible inner IPv4 packet with a few bytes changed. When the session ends by `CLOSE`, it is set up again with a new epoch | Same file |
| One line pointing at `scripts/test.ps1 -Asan` | `roadmap.md` Phase 5 tasks |
| Removed this item from the next work | `plan.md` |

## Important decisions

- The fuzz tests also run in the normal build. There they only see crashes; the ASan build judges memory errors
- Stage 1 goes through the socket as roadmap says. The batch size (32 x 2000 bytes) stays inside the 256KB `SO_RCVBUF` so the kernel
  does not drop any, and after each batch the received count (`drop_oversize_datagram` + `drop_bad_source` + `drop_unclassified` + the
  tunnel branch + the STUN branch) must equal the sent count. The session deadline is always in the past so waits cannot be infinite
- Random bytes reach the STUN classification with probability 2^-34, so the first run saw zero in the STUN branch. One in eight now
  hits those fields. Stage 1 of roadmap looks at "classification", so all three branches of it are exercised
- Whether stage 2 walked the pipeline is read from counters. `drop_magic` is 0, and the three 8.1 reasons, the two 8.3 counters, two 8.4
  counters, `drop_no_sink`, `drop_bad_reason`, `drop_unmatched_pong`, and the `ROSTER` structure `drop_type_length` in the session table
  must be greater than 0
- The seed, count, and length range go to standard error as one line at the start. They stay in the ctest output even on a crash

## Verification

| What | Result |
|------|------|
| `pwsh -NoProfile -File scripts/test.ps1 -Asan` | 608/608 passed. Under load, fuzz stage 1 took 11.6 s and stage 2 12.1 s (ctest per-case limit 30 s) |
| `pwsh -NoProfile -File scripts/test.ps1` (with DynamoDB local) | `all tests passed` |
| Does ASan really catch it | An exact-size heap copy was made at the tunnel receive entry and one byte past it was read. Fuzz stage 2 passed in the normal build, and the ASan build failed, reporting `heap-buffer-overflow` at that line. Reverted |
| Is the ASan build really instrumented | Its `build.ninja` has `/fsanitize=address` 185 times, `RTC1` 0 times, and `/INCREMENTAL:NO` |
| `docgate.py`, `--claims` | `VERDICT: pass` |

## Cross-model review

Codex, with the lenses: flag handling and caching of the ASan option, whether the two fuzz stages follow roadmap as written, whether
the stage-1 wait can hang or pass vacuously, the meaning of the reach assertions, and reproducibility. `LGTM - no blockers`.
