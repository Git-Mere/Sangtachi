# Phase 4 Round 4 J1: The Local Record File

## Why

This is the first slice of Phase 4 round 4 J. It opens the local record file, the input of `spec.md` M-6. The contract is chapter 9
Local Record File of `architecture.md` (path, line format, attempt number, writes and failures) and ADR 0016. The calls that actually
write lines (establishment, session end, step (3) of shutdown) come with the rendezvous and lobby wiring. This slice stops at the
tools that open the file and write to it.

## What changed

| What | Where |
|------|-------|
| The shape of one line, the `ts` form, the file name, attempt numbers, stopping after a write failure. The write and the wall clock are injected | `client/include/sangtachi/record.hpp`, `client/src/record.cpp` |
| The `records` folder and a new file (`CREATE_NEW`) under the executable folder, one line per write call, partial write = failure, UTC wall clock, PID | `platform/record_file.hpp`, `client/src/platform/win32/record_file.cpp` |
| The counter `record_write_failed` (process table) | `counters.hpp`, `counters.cpp`. Removed from `PENDING_COUNTERS` in `docgate.py` |
| Opened after the argument check with one `record.open` line. If it cannot be opened, one `ERROR` line and exit code 2. `run_client` receives the `RecordWriter` (not used yet) | `client/src/main.cpp` |
| Startup contract checks. The file of a normal start and the UTC time and PID in its name, two processes started together, no file on an argument error, startup failure when `icacls` blocks creating the folder or the file | `scripts/cli-check.ps1` |
| Unit tests | `tests/record_test.cpp`, `tests/platform/record_file_test.cpp`, `tests/counters_test.cpp` |
| Module rows and tree for `record` and `platform/record_file`. The stale 3.5 sentence "the path is decided when the chapter 9 contract is settled" now points to chapter 9 | `architecture.md` 3.1, 3.5, Chapter 10. English mirror |

## Key decisions

**Small points the documents do not fix were decided in code.** They are in the "decided by this code" tables of `record.hpp` and
`platform/record_file.hpp` with their basis.

- One failed write gives `record_write_failed` 1 and one `WARN record.write_failed kind=<kind> attempt=<number>` line. Lines skipped
  after that are not write attempts and are not counted. Chapter 9 "runs that can be judged" only checks whether that value is 0
- Attempt numbers keep rising after a failure. The wall clock is read once right before writing a line and not after a failure
- The executable path is obtained once with a 32768 wide-character buffer. A full buffer is treated as truncated, which is a failure
- The file is opened with `FILE_APPEND_DATA` only and `FILE_SHARE_READ`. It can be read while running
- The open-failure line follows main's existing startup-failure convention `ERROR socket.error op=<call> code=<error>`
- No row was added to chapter 11 External Dependencies. All new calls are kernel32, and chapter 11 lists only separate libraries

**Limit of the partial-write test.** On this machine a non-blocking pipe without room returned success and 0 bytes, not `0 < n < size`.
The test checks "a write that reports success without writing everything", and first confirms that premise inside the same case with a
raw `WriteFile`. The verdict (`written == size`) does not distinguish the two.

**One defect found by the full test run.** The argument-error check first only looked for `records\*-<PID>.log`. Files from
earlier runs pile up in the build folder (chapter 9: a person deletes them) and PIDs are reused, so in a full run it matched an old
file's PID and failed falsely. It now only looks at files that did not exist before launch, and also requires that the failure is the argument check's `ERROR args.invalid` with no open-failure line. And the mutation that opens before the
argument check is still caught.

**Observation.** If two processes with the same PID start in the same second, `CREATE_NEW` refuses and startup fails. That is the
behavior chapter 9 defines. It actually happened in the mutation that set the PID to 0. With real PIDs it needs reuse within one second.

## Verification

| What | Result |
|------|--------|
| Full tests (`scripts/test.ps1`) | All unit tests pass, cli/e2e/lobby checks 0 failures, `all tests passed`. Run on DynamoDB local |
| Mutations (clean copy) | All 14 caught. Unit: removed stop after failure, partial write as success, missing value as `0`, `rtt_us`/`setup_ms` order, millisecond zero padding, counter per skipped line, attempts from 0, `CREATE_NEW` -> `OPEN_ALWAYS`, `setup_ms` on the end line, reading the clock after failure. Startup: ignoring the open failure, PID 0, opening before the argument check, local time in the file name |
| `docgate.py`, `platformgate.py` | Both `VERDICT: pass` |

## Cross-model review

Codex was run separately with two lenses (contract and correctness, test sufficiency and document/record consistency). The contract
lens returned LGTM, and a second run of the same lens aimed at the Win32 calls also returned LGTM.

| Finding | Disposition |
|---------|-------------|
| warn. The cli-check file-name check only looks at the digit shape, so it passes even if it is not the UTC startup time | Fixed. Checks at second precision that it lies between UTC before launch and after exit. Confirmed with a local-time mutation |
| warn. Failures of `icacls` removal and temporary-folder deletion are silently swallowed | Fixed. What is left is reported as a failing check |
| warn. No test checks the header's claim that the wall clock is read once per written line and not after failure | Fixed. A counting clock. Confirmed with a read-after-failure mutation |
| warn (fix confirmation). Windows PowerShell 5.1 turns native stderr into exceptions under Stop, which can stop cleanup midway | Fixed. Each cleanup action is caught separately and leftovers are reported |
| warn (fix confirmation). The clock count is only checked as a total. Successful end lines and end lines that fail from the start are not counted | Fixed. Checked right after each call, and an end-line case was added |
| warn (confirmation of the defect fix above). With same-second PID reuse, opening before the argument check can fail on a name collision, exiting 2 without a new file, and the check passes | Fixed. The failure line must be `ERROR args.invalid reason=bad_role` with no `socket.error` |

The final confirmation review (the whole staged change, both lenses) was `LGTM - no blockers`.
