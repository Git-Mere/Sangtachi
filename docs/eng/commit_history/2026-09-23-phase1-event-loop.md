# Phase 1 Event Loop and Console

**Target commit:** this commit
**Phase:** 1 (core UDP networking) — last batch

The concurrency model in `architecture.md` 3.2 is now code. Every Phase 1 piece is tied together here.

## What Went In

| Target | Content |
|------|------|
| `timer.hpp` / `timer.cpp` | `next_timeout_ms`, `TimerSet`. A linear scan finds the earliest deadline |
| `console.hpp` / `console.cpp` | A 16-line SPSC queue. When it is full, a new line is dropped and the producer bumps a counter |
| `hash.hpp` / `hash.cpp` | SHA-256. It uses Windows CNG |
| `loop.hpp` / `loop.cpp` | `EventLoop`, `run_console_reader` |
| `main.cpp` | Opens the socket, emits `socket.bind`, and runs the loop |
| `scripts/e2e-check.ps1` | Checks that several processes really send and receive |

**We did not implement SHA-256 ourselves.** Windows CNG ships with the OS, so it is not a
third-party dependency under `spec.md` NFR-5, and it is the native Windows API that C-2 asks for.
Writing it by hand only adds one more thing to test and gains nothing.

**`next_timeout_ms` is kept a pure function.** That pins down, as test cases, the two underflows
that `architecture.md` 3.2.2 warns about. If the subtraction comes first when a deadline has
passed, the value becomes huge, and narrowing it to 32 bits lands on `INFINITE`, so the loop never
wakes again.

**Missed periods are not caught up.** The next deadline is `now + interval`, not
`previous deadline + interval`. The case name says that the axis of the verdict is the **interval**,
not the expiry **count**.

## Three Document Fixes

The implementation exposed these.

| What | Before | After |
|------|-----|-----|
| 3.2.1 thread count | "Phase 1~2 has three threads", which counted `[telemetry]` from Phase 1 | A per-range thread table. `[telemetry]` is Phase 9. `roadmap.md` puts that thread in Phase 9 work |
| 3.5 sender-side `rx.raw` | It only said "the sender emits the same line" and did not say what `from` is | It is our own local endpoint. The bind uses `INADDR_ANY`, so the address is `0.0.0.0` |
| 3.5 console commands | Only `raw` was written down | `counters` and `quit` were added. Chapter 9 asks for "all counters through a console command" and that command was nowhere |

## Phase 1 Verification Item Check

Each item in the `roadmap.md` Phase 1 verification list is matched to its evidence.

| Verification item | Evidence |
|-----------|------|
| Two processes send and receive both ways | `scripts/e2e-check.ps1`. It starts three processes in a chain, so the middle one both sends and receives in one run |
| Byte-for-byte match | The same script compares `len` and `sha256` of two `rx.raw` lines. It is not an eye check |
| The process survives a socket error | The same script. It sends to broadcast so `sendto` fails deterministically, then checks `socket.error op=sendto code=`, survival, and `tx_err_send=1` |
| Console queue bound with 17 lines | `tests/console_test.cpp`. A queue-level test with the consumer stopped. It also checks the order of the first 16 lines |
| bind contract | `tests/network/udp_socket_test.cpp` and the `socket.bind` log. The end-to-end script checks that two instances both start |
| Three `SO_RCVBUF` conditions | The same test file. Requested value 262144, applied value 94208 or more, and whether the applied value came from the socket |
| drain pattern | `tests/loop_test.cpp`. One turn handles several items |
| An oversized datagram does not break batching | The same file. 1473 (length compare) and 1474 (`WSAEMSGSIZE`) are placed between normal datagrams |
| Timer expiry under load | The same file. Under 10 seconds of load, `elapsed_ms` stays at 400 or less. Each datagram burns 200 microseconds |
| Zero `/W4` warnings | `cmake/HamychiWarnings.cmake` pins `/W4 /WX` |

**The two-machine measurement is not done.** The first row above was run with several processes on
one machine. The procedure where each side on two machines sends first is waiting for a second
machine and sits in the `plan.md` waiting list.

## Verification

**The numbers below are the test counts of the whole repository after this commit.** They are not
only what this diff added.

| What | Count | Added by this commit |
|------|:---:|:---:|
| Catch2 cases | 83 | 28 |
| CLI cases | 19 | 0 (only the call style was fixed) |
| End-to-end checks | 27 | 27 (new) |
| `build.ps1 -SelfTest` | 7 | 0 |

We ran 30 mutations.

| Target | Mutations | Killed |
|------|:---:|:---:|
| The `timer.cpp` family is pure functions, so cases cover it directly | - | - |
| `loop.cpp` | 9 | 9 |
| Recheck of earlier commits | 21 | 20 |

**We ran the mutation that `roadmap.md` asks for by name.** The item reads "check that a mutation
removing the drain budget code makes this criterion `FAIL`". With the budget gone, the loop cannot
leave while receives keep coming, so the timer never fires and that case dies on its time bound.

### A Mutation That Survived Until a Case Was Added

The mutation that deletes the `WSAEnumNetworkEvents` call survived at first. **That defect is a
spin, so a test that runs a fixed number of turns cannot see it.** The event stays signaled and the
wait returns at once every time, but the result is the same.

So we added a case that checks **whether it really waits when there is nothing to do**. It sets no
timer so the wait timeout becomes `INFINITE`, then checks both that the turn after draining does
not return within 300ms and that the shutdown signal does make it return. With that case in, the
mutation died.

## Two Test Harness Fixes

**One, `ctest` had no time bound.** The mutation that removed `WSAEventSelect` left the socket
blocking, `recvfrom` never returned, and the whole harness hung. We set `TIMEOUT 30` and put a
bound in the mutation driver too. **A mutation can show up as an endless wait instead of a `FAIL`.**

**Two, `cli-check.ps1` did not follow the new program behavior.** Once `main.cpp` ran the event
loop, the program no longer exits by itself when the arguments are valid, but that check assumed it
does. We fixed it to write `quit` on standard input and close it, and put a 10 second bound on
`WaitForExit`.

While doing that, arguments were joined into one string, so **an argument with a space was split in
two.** The case meant to test a space never passed that space to the program. We added a function
that quotes per the Windows command line rules. Windows PowerShell 5.1 has no `ArgumentList`
collection.

## Cross-Model Review

Codex got two lenses (concurrency and lifetime, scenario trace). Of 7 findings, **6 were taken and
1 was rejected.** **Three blockers shared one root.**

### blocker — A detached thread referenced the stack

`[console]` is not joined. Shutdown in 3.2.7 decided that and gives the reason. There is no way to
cancel a standard input read from outside, so a join would hold shutdown until the user types one
more line.

**But not joining means that thread can wake up after `main` has left.** The first implementation
put the command queue and the stop flag on the `main` stack and the event handle in `EventLoop`.
All three die before that thread does.

| Grade | Finding | Handling |
|------|------|------|
| blocker | The stop flag is a non-atomic `bool` while two threads read and write it | **Taken** |
| blocker | The detached thread references the queue, the flag, and the event handle, and all three can be destroyed first | **Taken** |
| blocker | `run_console_reader` takes a `const bool&` and reads freed memory | **Taken** |

One fix covered all three. `ConsoleSession` owns the queue, the atomic stop flag, and an auto-reset
event, and the thread takes a `shared_ptr` **by value** so it holds its own lifetime. `EventLoop`
only borrows that event and does not close it.

> **The line in 3.2.7 that says "it owns only the command queue, so nothing is lost" still holds.**
> What that sentence does not say is **where** that queue must live. On the stack, nothing is lost
> only because it touches freed memory instead.

### Other Findings

| Grade | Finding | Handling |
|------|------|------|
| warn | `valid()` looks only at the shutdown event. If the console event or the socket event is dead, the "live handle" rule of the wait array breaks | **Taken.** It looks at all three. The three handles in Phase 1~2 are always present, so one check makes the array dense by construction |
| warn | `run_once` puts three handles into the array with no null check | **Taken.** The `valid()` above blocks that, and the reason is written in the header |
| blocker | After `quit`, the reader thread can still be alive while `main` destroys the queue and the event | **Taken.** Same fix as above |
| warn | The turn that handled `quit` also runs the timers and returns true. Shutdown is pushed to the next turn | **Rejected.** That is the behavior set by one loop turn in 3.2.3. `quit` does not call `shutdown()` directly but signals the shutdown event, and the text says "the wait of the next turn returns on the shutdown handle and follows the order of shutdown in 3.2.7". That is what lets `[telemetry]` and `[control]` wake on the same signal. The tests assert that two-turn behavior as written |

**The scenario lens produced the blockers.** As in earlier commits, the lens that follows one run
from end to end, not the one that looks at components apart, caught the lifetime problem.

### Two False Positives in the Repair Check Round

In the round that sent only the repairs, the input was cut to `client`, `tests`, and
`docs/kor/commit_history`. The reviewer then reported "this record says it added
`scripts/e2e-check.ps1` but it is not in the diff" and "it says three documents were fixed but they
are not in the diff". **Both were outside the range I cut.**

I checked with `git diff --cached --name-only` and rejected them. `scripts/e2e-check.ps1` and
`docs/kor/architecture.md` are staged.

> **Cutting the range brings "it is missing" false positives with it.** That does not mean do not
> cut. Save the input you cut, and take or reject only the "it is missing" findings after checking
> HEAD directly. The final round runs on the whole diff.

## What Is Left

The Phase 1 implementation is done. The two-machine measurement and the `docs/eng` mirror remain.
