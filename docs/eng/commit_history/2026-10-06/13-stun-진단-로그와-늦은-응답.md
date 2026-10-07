# STUN diagnostic log tests, STUN responses after the stage ends

## Why

These are the four items in `plan.md` "Places to check during implementation" whose timing is Phase 3.

## What changed

| Item | What |
|------|------|
| Tests for the STUN diagnostic logs | Added the sink `LogSink` and `set_log_sink` to `log.hpp`. The product path does not install one, so output stays on standard error. Test helper `tests/log_capture.hpp`. Added tests that compare the whole line for all six events (`stun.timeout`, `stun.rejected`, `stun.error`, `stun.unresolved`, `stun.duplicate`, `timer.rejected`) |
| Success path when the list has exactly two entries | One `stun_client` unit test |
| A STUN response that arrives after the stage ends | Fixed one code defect and fixed the counting table in `protocol.md` chapter 13. Below |
| `StunClient` lifetime in `main.cpp` | Not changed. Below |

## A STUN response that arrives after the stage ends

In the Phase 2 public-server observation, a late response from the third server showed up only in `rx.raw`. Result of the comparison:

- **The observed case is correct.** At the moment the stage succeeds, a request still in flight on the
  other slot is reaped by `release` and goes into the list of finished transactions. Its response answers
  a request we sent, so it is not counted
- **Chapter 7 Receive Classification does not decide this interval.** Chapter 7 classifies it as STUN and
  hands it to chapter 13
- **Two rows of the chapter 13 table overlapped.** "There is no pending slot at all (after the stage ends)
  -> counted" and "a slot already finished (response, deadline) -> not counted". A response for a slot
  reaped at stage end matched both, and the parentheses of the latter row did not include stage end
- **Code defect.** `StunClient::on_datagram` returned at the very top once the stage was done. So it
  also did not count **a stranger's transaction** that arrived after the stage ended. That disagrees with
  "after the stage ends -> counted" in the chapter 13 table. The early return was removed. A finished stage
  has no pending slot, so the checks below it become exactly the table

### Decided (approved by the repository owner)

- Moved the "finished slot" row of the chapter 13 table to the top and added "reaped when the stage
  ended" to its parentheses. The table is read from the top and the first matching row applies
- **Finished slots are remembered for one attempt.** If the attempt ends before STUN ends, the memory is
  cleared right then. If it ends after STUN ends, the memory is cleared when the next attempt's STUN
  starts. A response of an earlier attempt that arrives after that is counted. This is what the code
  already does. `Lobby` emits `CancelStun` only during STUN, and `ControlRunner` keeps a finished
  `StunClient` until the next `StartStun`

The first draft said "responses that arrive after `FAIL` or `leave` are counted". The cross review pointed
out that the object stays after a `leave` that comes after STUN ends, and the code does that. The
explanation given to the repository owner (option A) missed that difference. The meaning of option A, not
remembering across attempts, is unchanged. The two clearing times are now written in the document. Two
runner tests pin those two times

Rejected alternative: remember across attempts. That list would need its own cap or expiry and there is
no basis for a value. What is lost is only that `drop_stun_parse` may rise by a few right after an attempt
is restarted quickly.

## `StunClient` lifetime in `main.cpp`

`StunClient` is no longer in `main`. `ControlRunner` makes one per attempt. The same ordering problem
moved to `runner`, and the current order is correct.

- The four handlers the loop holds are cleared after `loop.run()`
- `runner` is declared after `loop`, so it is destroyed first, and its destructor removes the STUN timers
  from the loop's timer set. The loop is still alive then
- No path calls a handler after `run()` returns. `[control]` responses also reach `runner` only when the
  loop drains them

The condition for another look ("the loop lives longer") has not happened, so the item was removed from
`plan.md`.

## Verification

| What | Result |
|------|--------|
| `scripts/test.ps1` without a filter | unit 293/293, `cli-check` passed, `e2e` 0 failures, `lobby-check` 0 failures |
| Does the new test fail first | The stranger-after-stage-end test failed with `0 == 1` on the code before the fix |

Mutations were applied by hand, run with `scripts/test.ps1 -Filter`, and reverted. All 15 were caught as failures.

| Mutation | Test that caught it |
|------|-----------|
| `refill` checks list exhaustion before two responses | exactly-two success, three runner tests |
| Remove the `stun.timeout` line | `stun.timeout` test |
| Remove the `stun.rejected` line | `stun.rejected` test |
| `stun.error` code always `-` | `stun.error` code test |
| Remove the `stun.unresolved` line | resolve log test |
| `stun.duplicate` `same_as` uses its own name | resolve log test |
| Remove the `timer.rejected` line | `timer.rejected` test |
| `emit` ignores the sink | all log tests |
| `set_log_sink` does not return the previous one | restore test |
| `break` instead of `return` in the finished-transaction check | after-stage-end test, after-deadline test, duplicate-answer test |
| Restore the early `if (done()) return;` | after-stage-end test (same as the failure before the fix) |
| `fail()` does not reap the slots in flight | the two random-failure tests |
| `Lobby` emits `CancelStun` at every attempt end | the memory-scope runner test, sixteen lobby tests |
| `ControlRunner` does not drop the object on `CancelStun` | the two leave-during-STUN tests |
| `ControlRunner` drops the STUN object on every lobby command | the after-`host` check of the memory-scope runner test |

## Cross-model review

Codex. Two lenses (code, docs-code agreement), one run each, then one more run on the repaired areas.

| Lens | Finding | Handling |
|------|---------|----------|
| docs | warn. The "one attempt" text in chapter 13 disagrees with `ControlRunner`. After a `FAIL` or `leave` that comes after STUN ends, the object stays and nothing is counted until the next `StartStun` | Applied. Wrote the two clearing times in chapter 13 (Korean, English) and in this record, and added two runner tests |
| docs | nit. "A response that arrives after the stage ends matches both the first and the third row" is wrong for a stranger's transaction | Applied. Narrowed to "a transaction of a finished slot that arrives after the stage ends" |
| code | warn. The sink test claims "instead of standard error" but does not look at standard error. The no-sink path has no test either | Partly applied. Renamed the test to "an installed sink receives the line". The standard-error path is checked by cli-check, e2e, and lobby-check, which read the real process's standard error. No standard-error capture device is added to the unit tests |
| code | warn. After the early return was removed, the slot cleanup in `fail()` is the only thing guarding a finished stage, but no test delivers a response after a random-source failure | Applied. One test and one mutation |
| repaired areas | warn. The memory-scope runner test also passes an implementation that clears on the next `host` command. It does not pin the `StartStun` moment | Applied. Also checks that an earlier-attempt response arriving after `host` but before the `create_room` reply is not counted |
