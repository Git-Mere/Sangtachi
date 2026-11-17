# Phase 9 Linux build, GUI build, and telemetry service deployment check

## Why

Three things were not checked in the Phase 9 implementation commit: the Linux build (`mere-galaxy` was unreachable), the GUI build, and
`deploy.py --target tm` and `verify.py --target tm` on the real instance. The repository owner approved deploying the telemetry service.

## What was done

| What | Result |
|------|--------|
| Linux build. HEAD `5d5b594` moved to `mere-galaxy` (Ubuntu 26.04, g++ 15) with `git archive`, Debug, `SANGTACHI_BUILD_TESTS=ON` | Built with no warnings or errors. `ctest` 75 passed, 0 failed. One adapter test was skipped for lack of root |
| That adapter test with `sudo ctest -R linux_tun` | First failed with `TUNSETIFF code=16` (EBUSY). A host started earlier on that machine held the `Sangtachi` device. With the owner's approval that host was stopped with SIGINT, the device went away, and the 4 `linux_tun` tests passed |
| GUI build. Release, `SANGTACHI_BUILD_GUI=ON`, Qt 6.12.0 `msvc2022_64`, Ninja | Linked `sangtachi_gui.exe`. Not started |
| `deploy.py --target tm` | `VERDICT: pass`. `server.started` on port 8001, journald limit written, smoke test `bad_request` |
| `verify.py --target tm` | `VERDICT: pass` |
| Instance state | `sangtachi-tm` and `sangtachi-cp` both `active`. `MemoryMax` 256M, `CPUQuotaPerSecUSec` 200ms |

## What changed

| What | Where |
|------|-------|
| Removed three rows from the remaining-work table and wrote that the service is deployed | `plan.md` |
| The new build folder on `mere-galaxy` and the condition under which the adapter test fails with EBUSY | `plan.md` test machine section |
| Wrote that `--target tm` deployment and its smoke test ran on the real instance. `--tm-load` has not | `tools/cp-deploy/README.md` |

## Not checked

- `verify.py --tm-load`. Its test records stay in the real telemetry table
- The path that accepts a valid `report` and writes to the CloudWatch `Sangtachi` namespace. This also confirms the instance role's `PutMetricData` allowance there
- The two port 8001 checks in the `roadmap.md` Phase 9 verification

## Verification

| What | Result |
|------|--------|
| `docgate.py`, `--claims` | `VERDICT: pass` |

## Cross-model review

Codex (`gpt-6.1-sol`). The factual consistency (A), mirror consistency (B) and public-repo account and address exposure (C) lenses were run together. `LGTM - no blockers`. After this section was filled in, the final staged diff was run again with the same lenses.

| Finding | Handling |
|---------|----------|
| [A] nit. "only with `sudo ctest`" in `plan.md` overstates it. The test checks `CAP_NET_ADMIN` | Applied. Wrote that it is skipped without that capability and that this machine runs it with `sudo ctest` |

After the fix, the final staged diff was run once more with the same lenses.
