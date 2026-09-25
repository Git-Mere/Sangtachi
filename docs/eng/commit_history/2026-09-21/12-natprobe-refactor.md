# 2026-09-21 Sharing the natprobe loop and splitting main

`natprobe.py` was audited, the duplicated loop was merged into one, and `main` was split. Along
the way it turned out the two measurement functions had no tests at all, so those were added.

## Audit result

| Item | Value |
|------|-------|
| Total | 1399 lines (code 853, docstrings 200, comments 140, blank 206) |
| Functions | 33, **0 dead** |
| Explanation share | 24% |

**It is not badly bloated.** `_disable_udp_connreset` at 48 lines and `setup_console` at 24 look
large, but every line carries a reason: `socket.ioctl()` blocks this command so `ws2_32.dll` is
called directly, and a cp1252 console kills the first `print`. Deleting them means hitting both
again.

Three things stood out: `main` at 210 lines with 7 responsibilities, the 29% overlap between
`run_punch` and `run_unsolicited`, and `check-peer` at 42 lines with no caller. **`check-peer`
stays.** It is the only way to validate a peer address and Phases 3 to 4 may use it again.

## Changes

| Function | Before | After |
|----------|--------|-------|
| `main` | 210 | 159 |
| `run_punch` | 156 | 138 |
| `run_unsolicited` | 145 | 129 |
| Overlap of the two loops | 36 lines (29%) | 24 lines (22%) |
| Code lines | 853 | 838 |

Three new functions: `_timed_send_recv`, `_run_check_peer`, `_new_record`.

**The file grew from 1399 to 1424 lines.** The code shrank by 15 lines and the docstrings of the
new functions are larger than that. It is written down rather than hidden.

## Decisions

**The timing rule lives in one place.** `_timed_send_recv` decides **when** only; the caller does
the sending and the parsing. The no-catch-up rule and the swallowed `recvfrom` `OSError` moved
into it. They used to sit in both functions, where fixing one side splits the two measurements.

The generator does not catch `KeyboardInterrupt`. How an interrupt is reported differs per caller,
and swallowing one raised inside the caller body would change existing behaviour.

## An untested area turned up

**None of the existing 151 cases touched `run_punch` or `run_unsolicited`.** Their heart was then
extracted into a shared function, creating an untested single point of failure.

12 cases that run over real UDP sockets were added: 8 for `_timed_send_recv` (deadline, send
interval, `seq` increment, `ts_ns` monotonicity, receive events, `duration=0`, an interval longer
than the duration, and the no-catch-up rule) and 4 for a `run_punch` loopback round trip.
151 -> 163.

## Writing a case is not the same as verifying it

The "no catch-up" case was written and then **a defect was deliberately introduced.** With the
catch-up guard deleted, **the test still passed.** It judged by the number of sends, and the slow
processing itself suppressed that number, so the two situations were indistinguishable.

It was redesigned around the send **interval**: only the first two events are processed slowly to
push the schedule back, and the test then checks whether sends with a near-zero gap follow. The
mutation now produces a `FAIL`.

## Verification

- `test_natprobe` 163/163 passed
- Two mutation runs. Removing the catch-up guard -> `FAIL`; removing the `continue` -> a crash
  during collection
- `check-peer` exit codes checked directly: `8.8.8.8` 0, `192.88.99.1` 2, stdin 0
- `python tools/docgate/docgate.py`: `VERDICT: pass`

## Cross-model review

Codex, 2 rounds.

| Round | Finding | Verdict |
|:--:|---------|---------|
| 1 | **blocker.** `run_punch` falls through after a send event and unpacks a 2-tuple into 3, crashing on the first send | **Dismissed.** The `continue` is there. `run_punch` was run against a loopback peer and returned `success` |
| 2 | None | `LGTM - no blockers` |

**A wrong finding pointed at a real gap.** The suspicion was possible because nothing exercised
`run_punch`. The dismissal came with 4 loopback cases, and a mutation removing the `continue`
confirmed that the crash the reviewer predicted does happen. **Dismissing is not discarding.**
