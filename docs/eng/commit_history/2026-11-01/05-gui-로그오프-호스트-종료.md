# Measured host shutdown at GUI logoff

## Why

This is the GUI side of the `roadmap.md` Phase 8 task "measure whether host shutdown fits inside the OS grace period". The GUI build measures the
`WM_ENDSESSION` path with a logoff. The same-machine test was mixed up by a console player running alongside it ([issue 0005](../../issue_history/0005-콘솔-로그오프-지연-경고.md)),
so the player was put on `mere-galaxy`. It ran three times.

## Run 1: from `CLOSE` receipt to cleanup

Run with the distribution built on 10-29 (it does not include `573f56a` and `faf06df`). Room `T6AQP2`.

- The GUI log ends with `attempt.end reason=shutdown`, `to=CLOSED`, `adapter.close`, in that order. The player received `CLOSE` and wrote `attempt.end reason=closed`
- Both clocks were compared against the same NTP server (`time.google.com`) and the difference subtracted. This machine has the Windows Time service stopped and was about 0.55 s fast. From the player receiving `CLOSE` to the GUI log's last write was about 0.11 s
- The OS grace period counts from receiving `WM_ENDSESSION`. The GUI log carries no times, so that front part could not be measured. A Codex review flagged it as a blocker twice, so this run did not close the item and the `session_end.cleanup` line was added ([`04-gui-로그오프-정리-시간-로그.md`](04-gui-로그오프-정리-시간-로그.md))
- A Debug console (`--no-adapter`, lobby) started in the earlier investigation was still running on this machine and ended at logoff with `0x40010004`. By design it does not exit at the end of standard input. There was no `winsrvext` 100

## Run 2: from `WM_ENDSESSION` to cleanup

Run with the distribution `scripts/package-gui.ps1` built from HEAD `9c94d30`. Room `4PH9QA`. The player was the Linux console client on `mere-galaxy`,
with the adapter. After `CONNECTED`, `ping` to `10.100.0.1` was 3/3, average 41 ms. The repository owner ended it by signing out from the Start menu.

| Check | Result |
|------|------|
| `session_end.cleanup` | `elapsed_ms=140`. Inside 3 seconds |
| End of the GUI log | `attempt.end reason=shutdown`, `to=CLOSED`, `adapter.close name=Sangtachi`, `session_end.cleanup elapsed_ms=140`, in that order |
| Player | Went from `CONNECTED` to `CLOSED` and wrote `attempt.end reason=closed` |
| System log | No `winsrvext` 100 around the logoff. Winlogon 7002 and 7001 are there |
| Leftovers | After signing back in, 0 Wintun adapters and 0 `10.100.0.*` addresses |

- 140 ms runs from receiving `WM_ENDSESSION` to when preparing the cleanup-complete signal begins. The line write, the screen queue push, and the signal after it are not included (`architecture.md` chapter 9 Log Output)
- There was one session. `roadmap.md` asks to measure up to four `CLOSE`s. A Codex review flagged it as a blocker, so run 3 followed

## Run 3: four players

Same distribution. Room `HPEGAG`. Four players (`galaxy1` to `galaxy4`) all ran on `mere-galaxy` with `--no-adapter`. A room holds at most
five (`MAX_PEERS`), so the host and four players fill it. The host log had four `CONNECTED`s.

| Check | Result |
|------|------|
| `session_end.cleanup` | `elapsed_ms=125`. Inside 3 seconds |
| End of the GUI log | `attempt.end reason=shutdown`, `to=CLOSED` for the four sessions, `adapter.close name=Sangtachi`, `session_end.cleanup elapsed_ms=125`, in that order |
| Four players | All four went from `CONNECTED` to `CLOSED` and wrote `attempt.end reason=closed`. They received all four of the host's `CLOSE`s |
| System log | No `winsrvext` 100 |
| Leftovers | After signing back in, 0 Wintun adapters and 0 `10.100.0.*` addresses |

- The players had no adapter. What host shutdown does (sending `CLOSE` and cleaning up the host adapter) does not depend on whether the players have one

## What changed

| What | Where |
|------|-------|
| The GUI logoff measurement item is removed and the numbering closed up | `plan.md` |
| The "host shutdown time at GUI logoff" row is removed | `field-test/required-test.md` |

## Verification

| What | Result |
|------|--------|
| Run 1 GUI log `20261101T174302Z-36688.log` | 255 lines. Backed up under `.prime/`; rebuilding the distribution deleted `gui-dist` |
| Run 2 GUI log `gui-dist\logs\20261101T175839Z-35380.log` | 241 lines. Ends in the order of the table above |
| Run 3 GUI log `gui-dist\logs\20261101T180409Z-27524.log` | 699 lines. Ends in the order of the run 3 table |
| Player logs `mere-galaxy:~/sangtachi-b/lplayer-ts.err` (runs 1 and 2), `p1.err` to `p4.err` (run 3) | All have `to=CLOSED` and `attempt.end reason=closed` |
| `mere-galaxy` after the measurement | The players were ended with `SIGINT`. There is no `Sangtachi` device and no client process left |
| `docgate.py`, `--claims` | `VERDICT: pass` |

## Cross-model review

Codex (`gpt-6.1-sol`). The evidence (A) and Korean/English and record-consistency (B) lenses ran in one pass.

| Finding | Disposition |
|------|------|
| blocker [A] (draft record of run 1) From `CLOSE` receipt to cleanup leaves out the part from `WM_ENDSESSION` to `CLOSE` receipt, so it cannot show the shutdown fits in the OS grace period. Raised twice | Accepted. The `session_end.cleanup` line was added (`04`) and run 2 followed |
| blocker [A] (record through run 2) `roadmap.md` asks to measure up to four `CLOSE`s, but there was one session | Accepted. Run 3 with four players followed |

The full staged diff with run 3 added went through the same lenses again. One nit (state the omitted interval as `WM_ENDSESSION` to `CLOSE` receipt) was accepted.
