# Phase 1 Build Skeleton and Winsock2 Startup

**Target commit:** this commit
**Phase:** 1 (core UDP networking)

This is the first commit that stopped document work and started implementation. The repo was moved
to the target layout, the build and the tests were made to run, and Winsock2 startup and shutdown
went in. That is the first item in the `roadmap.md` Phase 1 task list.

## What the Repository Owner Decided

Two things were decided during the session. Both are user instructions.

**1. No revert.** There was a proposal to revert to the commit that added `control_plane.md`
(`3274ef7`), on the view that document review would not end. It was investigated and opposed. Three
commits since then carry fixes for 4 design audit 3 blockers, and more decisively, **the
instruments that Phase 1 verification uses live in those commits.** In the documents at `3274ef7`,
`rx.raw` and `probe200` appear 0 times. A revert goes back to a point where the code to be written
now has no verdict criteria. The user chose to keep HEAD, and only the unfinished work of turning
duplicated claims into links was put on a stash.

**2. Catch2 is used.** The basis is this judgment: what NFR-5 blocks is wrapping an external
library and submitting it as the deliverable, and a test library is not in that category. Following
this instruction, `spec.md` NFR-5 was split into product dependencies and test-only dependencies,
and [ADR 0005](../../decisions/0005-test-framework-catch2.md) was written. Raising warnings to errors
(`/WX`) was decided at the same time.

## What Changed

| Bundle | Content |
|------|------|
| Tree | `src/main.cpp` -> `client/src/main.cpp`. `client/include/`, `cmake/`, `tests/` added. `control-server/` and `telemetry-server/` are placeholders that hold only a `.gitkeep` |
| Build | `sangtachi_core` (static) + `sangtachi_client` (executable) + `sangtachi_tests`. `main()` does not go into the library |
| Warnings | `/W4 /WX /permissive- /utf-8` on the `sangtachi_warnings` interface target. It is linked only into targets of this repo |
| Tests | Catch2 v3.16.0 pinned by tag through `FetchContent`. Registered into CTest with `catch_discover_tests` |
| Code | `sangtachi::network::WsaContext`. The constructor calls `WSAStartup`, the destructor calls `WSACleanup` |
| Scripts | `scripts/build.ps1`, `test.ps1`, `vsdevshell.ps1`. They find VS with `vswhere`, so no path is baked into the repo |

## Decided by Measurement

### Build Output Goes Outside the Repo

The build broke with `LNK1201`, `C1041`, and `LNK1168`. Every one is a file lock on a freshly
written `.exe` or `.pdb`. The repo sits inside a file sync folder, which was the suspect, so the
same build was run in two locations.

| Location | Failures | Failure kinds |
|------|------|-----------|
| Inside the repo (`build/Debug`) | **4 / 18** | `LNK1201` 1, `C1041` 1, `LNK1168` 2 |
| Outside the repo (`%LOCALAPPDATA%\Sangtachi\build\Debug`) | 0 / 18 | none |

One-sided Fisher exact gives `p = 0.052`. **Statistics alone do not settle it.** What was seen with
it is that all 4 failures share one mechanism (file lock) and correlate only with the location. The
cost of moving is near 0, so the default location was set outside the repo. `-BuildDir` puts it
back.

**Which process holds the lock was not checked.** Whether it is the sync agent or the antivirus is
not asserted. Only the correlation with the location is known.

### Test Case Names Use ASCII Only

In the first draft, the case names were written in Korean and all 3 cases fell out with
`No tests ran`. `catch_discover_tests` registers the case name into CTest, and on run it passes
that name back to the executable as a command argument. The name is mangled through the console
code page, so it matches no case. For the same reason, the strings that the PowerShell scripts
**print** were fixed to ASCII. Comments stay inside the file, so they are left in Korean.

## Verification

4 Catch2 cases from `scripts/test.ps1` and 7 cases from `scripts/build.ps1 -SelfTest` all pass. Two
mutation tests were run on top of that.

| Mutation | Result |
|------|------|
| Remove `WSACleanup` from the destructor | **Only 1 of the 4 cases gave `FAIL`.** The other three passed |
| Remove the marker check from `Test-IsCmakeBuildDir` | **2 of the 7 cases gave `FAIL`,** exit code 1 |

**The first mutation exposed a hole.** The 3 cases written first all passed with `WSACleanup`
deleted. The Winsock reference count simply does not drop, and `socket()` keeps succeeding. So a
case was added that checks whether `socket()` fails with `WSANOTINITIALISED` after the last context
is gone, and only that case fell out under the mutation. **The rule that writing a case is not the
same as verifying caught this in the very first commit.**

## Cross-Model Review

Four rounds were run with Codex, with the lens split per round. All 7 findings were taken and none
were rejected. Round 4 re-read only the repairs from round 3 and gave `LGTM`.

### Round 1 — Code Lens (Correctness and Robustness)

| Grade | Finding | Handling |
|------|------|------|
| warn | If the `std::string` allocation after a successful `WSAStartup` throws, the object is not finished, the destructor does not run, and the Winsock reference count leaks | **Taken.** Instead of catching the exception, **the throwing work was removed.** The version string is not held as a member; `negotiated_version()` builds it when called. After `WSAStartup`, the constructor does one `WORD` assignment and nothing else |
| blocker | `-Fresh` deletes an arbitrary `-BuildDir` path recursively with no check | **Taken.** `Test-IsCmakeBuildDir` was added and deletion happens only when `CMakeCache.txt` is there. It is an allow list that demands evidence, not an exclude list of path prefixes. The 7-row case table can be run with `-SelfTest` |

### Round 2 — Document Lens (Consistency)

Three findings shared one root. `spec.md` and `architecture.md` wrote Catch2 as "approved", while
the ADR and `plan.md` wrote "waiting on the professor", so the state did not line up.

| Grade | Finding | Handling |
|------|------|------|
| warn | The approval list in `spec.md` contradicts the waiting text in `plan.md` and the ADR | **Taken.** The three were lined up in one pass. It was nailed down that the item to confirm is not whether Catch2 is approved but **the split of NFR-5 into two kinds** |
| warn | The approval state in the `architecture.md` dependency table does not match the ADR | **Taken.** The state column now points at `spec.md` NFR-5. The nuance is not copied into two places |
| warn | The ADR says "the repository owner approved" without saying where that approval is written | **Taken.** It now points at this record |

**Sentences that leaned on a definition I made were not fixed by me.** NFR-5 was fixed and three
other documents that use that definition were left as they were. `CLAUDE.md` writes this mechanism
down as "when a definition changes, list the sentences that lean on it and read them one by one".

### Round 3 — Final (Repair Lens + Record Lens)

Two lenses were run on the whole diff. They look at whether the repairs of the first two rounds made
new defects, and whether this record and `plan.md` match the diff.

| Grade | Finding | Handling |
|------|------|------|
| warn | `-Fresh` checks with `-LiteralPath` but deletes with `Remove-Item`, which expands wildcards. If the path holds `[ ] * ?`, the thing checked and the thing deleted differ | **Taken.** Deletion was changed to `-LiteralPath` too. The temporary directory cleanup in `-SelfTest` was lined up with it |
| warn | This record says it created `control-server/` and `telemetry-server/`, but git cannot hold an empty directory, so they are not in the diff | **Taken.** A `.gitkeep` was put in both, and the text of this table was fixed. The `.gitkeep` files in `client/include/` and `scripts/`, which are no longer empty, were deleted |

**The repair round caught a defect in a repair.** The guard added while fixing the round 1 blocker
used two different path interpretations for the check and the delete.

### Round 4 — Repair Check

Only the repairs of round 3 were cut out and run again. The result is `LGTM`.

## Documents

| Document | What |
|------|------|
| `spec.md` | NFR-5 split into product dependencies and test-only dependencies. What was not confirmed is stated |
| `architecture.md` | Catch2 in the chapter 11 external dependency table |
| `roadmap.md` | Test executable and CTest registration in the Phase 1 tasks. `/WX` and its scope in the warning item of the verification |
| `decisions/0005` | New |
| `plan.md` | Next work as the four Phase 1 bundles. Waiting items added |
| `README.md` | Requirements, build, test, run, repo layout |
| `CLAUDE.md` | Current state and the ADR count. Only stale facts were fixed; the rules were not touched |

## What Is Left

The `docs/eng` mirror is not in this commit. Korean is fixed first, and the mirror is made when
there is an instruction to push. The document gate blocks on `mirror` and `parity`, and `link` is
0.

The rest of Phase 1 is `endpoint` and the startup arguments, logs and counters, the UDP socket
wrapper, and the `[loop]` event loop. The order and the reasons are held by `plan.md`.
