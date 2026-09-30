# Brought the Phase 1 Code in Line with the Changed Documents

## Why

After the Phase 1 implementation (`8ad10b9`) only the documents changed, and they changed a lot:
ADR 0006 (no-relay star, five per room), 0007 (minimum GUI and Qt), 0008 (nine follow-up
decisions), 0009 (the room host lease), 0010 (platform seams), and the commits reflecting them.
`architecture.md` moved 628 lines, `concurrency.md` 535 (a new document), `control_plane.md` 508,
`protocol.md` 229 in that window. **Finding where the code diverges from those documents before
moving on to Phase 2** is what this work is.

Three read-only audits were run, one per lens: constants, counters and log tokens; startup input,
CLI and console vocabulary; structure and concurrency. Findings were graded four ways: `now`
(diverging today), `phaseN` (a place that Phase builds), `doc` (the code is right and the document
is stale), `unknown` (could not check).

**Not counting an absence as a defect** was fixed in the judgement rules first. The Phase 1 scope
is sockets, logs, counters, timers, the event loop and the console, and it is normal that there is
no session, no adapter and no control plane yet.

## What Was Fixed

| # | What | Where |
|---|------|-------|
| 1 | `MAX_PEERS = 5` was not among the code constants (ADR 0006 put it in the chapter 3 constant block) | `protocol_constants.hpp` |
| 2 | No counter was raised on a recv error. **The document gave no name, so the document was fixed first** | `concurrency.md` chapter 3, `counters.*`, `loop.cpp` |
| 3 | `protocol.md` 5.7 was missing from the counter name source comment | `counters.hpp`, `counters_test.cpp` |
| 4 | Without `SetConsoleCtrlHandler`, Ctrl+C and closing the window did not run the shutdown procedure | `platform/console_ctrl.*`, new |
| 5 | The shutdown turn returned before reflecting `console_queue_dropped` | `loop.cpp` |
| 6 | The console took `raw500` as `raw` and actually sent it | `loop.cpp` |
| 7 | A comment in `timer.hpp` cited the document as saying the opposite | `timer.hpp` |

**The counter name was settled as `rx_err_recv`.** It pairs with `tx_err_send` and is not split
per error code. The code did not name it first; it was written into `concurrency.md` chapter 3 and
the code followed.

**Item 4 went into `client/src/platform/win32/`.** It uses OS headers, so that is the place set by
[ADR 0010](../../decisions/0010-platform-porting-seams.md). The handler body was pulled out into a
function taking the state as an argument, so the code the tests run is the code that actually runs.

## Fixed in the Documents Only

| What | How |
|------|-----|
| The timer deadline comparison | `protocol.md` chapter 11 and `concurrency.md` chapter 4 said "strict inequality" while the code has `deadline <= now`. **The code is right.** With strict, on the turn where the deadline is exactly now the timer does not fire and `next_timeout` returns 0, so the same turn repeats for 1ms |
| The send remainder of `raw` | `concurrency.md` chapter 3 said `raw` is sent in pieces, but `architecture.md` 3.5 says one send. There is no remainder |
| The module table and the tree | Implemented modules were missing from `architecture.md` 3.1 and chapter 10. `loop`, `timer`, `console`, `log`, `counters`, `args`, `hash`, `platform/wait`, `platform/console_ctrl` were added |
| The input the lobby takes | [ADR 0008](../../decisions/0008-star-topology-followup-decisions.md) decision 8 delegated this to `architecture.md` 3.5, and that description is absent. It was hung as a `roadmap.md` Phase 3 before-start item. Without it, `leave` becomes a command that makes the process unusable |
| When `TimerSet` is needed | `plan.md` said Phase 4, but **Phase 2 calls it first.** STUN sets per-server retransmits and a deadline, has to cancel on a response, and queries two servers at once |
| Two control-signal contracts | A failed handler install is a startup failure. A signal not in the table is not handled and is passed to the next handler. **What the code settled first was moved into the documents** |

## Places Phase 2 Needs First (Not Fixed Here)

These came from the structure lens of the three audits. They are Phase 2 work itself, so they were
not done early.

- `TimerSet` has no key, no removal and no one-shot
- `EventLoop` has no send seam. Sending is only the private `send_raw` bound to `--peer`, while
  `protocol.md` chapter 6 requires STUN to use the same socket through the loop
- `drain_udp` has no source hygiene and no classification
- There is no startup resolution of STUN names and no warning when the `--stun` list has one entry

## Verification

| What | Result |
|------|--------|
| Build | `build ok` (Debug) |
| Tests | `0 checks failed` / `all tests passed`. 91 -> **101**. e2e checks 27 -> 28 |
| `platformgate` | `VERDICT: pass` |
| `docgate` | 0 findings of kind `link` |

9 mutants were run and all were caught. One dies at **compile time** rather than at runtime
(`STATIC_REQUIRE(kCleanupWaitMs == 3000)`).

## Cross-Model Review (Codex)

**The tooling failed once.** The `codex` CLI died with `unknown variant 'max'` while re-reading
the model list and produced no output file. The exit code was 0. "Judge that a review is running
only by the output file" in `CLAUDE.md` paid off exactly there. It was run again to get the
result.

### Accepted

| Finding | How |
|---------|-----|
| **`signal_cleanup_done()` sits inside the `try` block, so it signals before the destructors.** If the window-close handler sees it and returns, the OS ends the process before the destructors run | Accepted. The signal was moved outside `try`/`catch`. **The loss in Phase 1 is small, but in Phase 6 the adapter and the routes are left behind.** That reason is in a comment |
| `console_ctrl` is missing from the tree and the module table in `architecture.md` | Accepted. Added to both. **A sentence I wrote in this same commit lagged the code of this same commit** |

### Risks Left

- **There is no test on registering `SetConsoleCtrlHandler` or on the path that delivers the OS
  value.** Only indirect evidence. An install failure is treated as a startup failure, so if every
  client in the e2e starts, the install happened
- **The 3-second cap has not been checked at runtime.** The value is a compile-time assertion, and
  the waiting path runs at 150ms
- The real path was not exercised with `GenerateConsoleCtrlEvent`. That call goes to the process
  group and reaches the harness on the same console too, and `CTRL_CLOSE_EVENT` cannot be produced
  at all. That is exactly the signal where waiting is needed
- Two handles are deliberately not closed. The handler can run even while `main` is exiting

## What This Round Confirmed

- **There are places only a review can catch.** The ordering of the cleanup-done signal is
  entangled with destructors and the OS ending the process, and a unit test has no way to observe
  it. Mutation testing does not catch it either
- **Pointing the reviewer at what to look at paid off.** "Can completion be signalled before
  cleanup finishes" was in the prompt, and that is where it came from
- **The subagent made a better choice than the instruction.** The instruction was to reflect the
  counters "just before `emit_all`" on shutdown; it put them at the two exits inside the loop. The
  `WAIT_FAILED` exit had the same defect, and inside the loop a test can judge that path with
  `run_once()`
