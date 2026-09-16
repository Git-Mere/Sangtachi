# 2026-09-15 Concurrency Model Settled

## Changes

Executed follow-up 2 from the design audit ([`../design-audit.md`](../design-audit.md)). It resolves
blocker 4 (data races) and blocker 5 (telemetry blocking keepalives), plus 2 warnings.

- Added `architecture.md` 3.2: thread layout, wait mechanism, one loop iteration, timers,
  state ownership, telemetry isolation, shutdown sequence
- Removed the four-thread diagram from `architecture.md` 3.1 and dropped `WSAPoll` from the
  `udp_socket` responsibilities
- Moved the former 3.2 Control Plane to 3.3
- Aligned the telemetry collection points and upload description in `architecture.md` section 9
  with 3.2
- Added a back-reference from `protocol.md` section 6 to `architecture.md` 3.2
- Changed timer priority in `protocol.md` section 11 from arrival time to **dequeue time**
- Added the event loop skeleton task and 2 verification items to `roadmap.md` Phase 1
- Added the telemetry thread separation task and 2 verification items to `roadmap.md` Phase 9
- Recorded this review in `design-audit.md` section 8

## Decisions

**Four threads to a single event loop.** The initial layout was `[main]`/`[net_rx]`/`[tun_rx]`/`[timer]`.
Rather than sharing state under a lock, we do not share it. The final layout is `[loop]` plus
`[telemetry]`, with `[console]` added only in Phases 1-5.

**`WaitForMultipleObjects` instead of `WSAPoll`.** `WintunGetReadWaitEvent` hands out a Win32 event
handle, so mixing it with the socket into one wait requires this call. It also means Wintun reads
need no separate thread, and no thread is added when the adapter arrives in Phase 6.

**Drains carry a budget.** Instead of draining until empty, each source is capped at 64 per
iteration. Without a cap, the timers stop running entirely the moment arrival outpaces processing,
and keepalives stop with them. The cost is that a datagram arriving just before a deadline can slip
to the next iteration, so the priority rule in `protocol.md` section 11 was redefined from arrival
time to dequeue time.

**The telemetry queue is a lock-free SPSC ring that drops the newest record when full.** Dropping
the oldest would be better for metric quality, but it requires the producer to move the consumer's
tail, which requires a lock. We do not put a lock in the data plane to save one more metric.

**The console handler returns at different points depending on the signal.** For the
`CTRL_CLOSE_EVENT` family, Windows kills the process the moment the handler returns. Closing the
console window is a normal way to stop this program, so it is treated as the main path, not an edge
case.

**Cleanup precedes the `[telemetry]` join.** That ordering lets the handler return without waiting
on telemetry. Exceeding the 2-second join bound ends the process via `_exit`. What actually enforces
the bound is that `_exit`, not the socket timeouts.

## Verification

- Confirmed the signatures of `WintunGetReadWaitEvent`, `WintunReceivePacket`, and
  `WintunReleaseReceivePacket` and their documented error codes (`ERROR_NO_MORE_ITEMS`,
  `ERROR_HANDLE_EOF`, `ERROR_INVALID_DATA`) against the upstream wintun.h
- eng/kor heading counts match (architecture 31, protocol 43, roadmap 45, design-audit 18, spec 15)
- Every relative link except `experiments.md` resolves. That one is the only unresolved link and is
  identical on both sides (a Phase 9 deliverable)

## Cross-Model Review

- Reviewer: Codex (GPT), 6 rounds. Rounds 1 to 4 each used a different lens; rounds 5 and 6 were convergence checks
- Result: 9 blockers, 11 warnings, 2 nits. **All applied, none dismissed.** Round 6 returned
  `LGTM - no blockers`

| Round | Lens | Blockers | Warnings |
|-------|------|----------|----------|
| 1 | Windows API correctness | 2 | 2 (plus 1 nit) |
| 2 | Internal design consistency | 3 | 2 |
| 3 | Side effects of the fixes, pseudocode precision | 2 | 4 |
| 4 | What the third-round fixes broke | 1 | 2 (plus 1 nit) |
| 5 | Convergence check (focused on the newly changed points) | 1 | 1 |
| 6 | Convergence check (final) | 0 | 0 |

Blockers and how each was handled:

| Round | Finding | Applied |
|-------|---------|---------|
| 1 | `WSAEventSelect` events are manual-reset and `recvfrom` does not reset the event object; without `WSAEnumNetworkEvents` the loop spins after the first datagram | Added `WSAEnumNetworkEvents` to the loop and corrected the "draining is the only correct pattern" rationale to a batching policy |
| 1 | For `CTRL_CLOSE`/`LOGOFF`/`SHUTDOWN` the process dies when the handler returns, so signaling and returning skips cleanup entirely | Split the return point by signal type; wait up to 3 seconds on the cleanup-complete event before returning |
| 2 | Uncapped drain loops starve the timers under load | `MAX_DRAIN` of 64 plus `busy` re-entry |
| 2 | `drain_wintun` called unconditionally although Phases 1-5 have no adapter; nothing serviced the console event, spinning the loop | NULL session guard, new `drain_console`, auto-reset event |
| 2 | A mutex queue cannot guarantee "never blocks", making the roadmap criterion unpassable | Replaced with a lock-free SPSC ring |
| 3 | `next_timeout` called `now()` twice; if the clock passes the deadline in between, the subtraction underflows into a 49.7-day timeout | Read `t = now()` once |
| 3 | The `ERROR_INVALID_DATA` branch had no return, letting a `NULL` pointer reach `route_and_send` | Introduced a `RESTART` return value; `[loop]` handles session recreation and wait-array rebuild as one unit |
| 4 | The drain budget contradicts the "arrival before deadline wins" rule in `protocol.md` section 11 | Redefined section 11 by dequeue time with the reasoning stated |
| 5 | Section 11 moved to dequeue time but the arrival-time wording survived in `architecture.md` 3.2.4, so the two documents stated different rules | Synchronized 3.2.4 |

Warnings and how each was handled:

| Round | Finding | Applied |
|-------|---------|---------|
| 1 | A `NULL` filling an empty slot in the handle array produces `WAIT_FAILED` | Stated that the table numbers are a logical rank and the actual array is built compactly |
| 1 | An underflowing `GetTickCount64` subtraction can land on `INFINITE` after the `DWORD` conversion | Compare before subtracting and clamp to `INFINITE - 1` in the `next_timeout` pseudocode |
| 2 | If an upload hangs, cleanup-complete may never be signaled after the join bound | Moved cleanup ahead of the join and specified the socket timeout and `_exit` paths |
| 2 | "`[loop]` shares no state" ignored the queue and the events | Corrected to "no tunnel state; shares only the queue and the shutdown/cleanup events" |
| 3 | `0..budget` iterates `budget + 1` times | Changed the notation to `repeat budget times` |
| 3 | `WintunReleaseReceivePacket` sat after `route_and_send`, leaking the ring packet on an early return | Changed to a scope guard |
| 3 | A shutdown sentinel in a lossy queue is discarded exactly when the queue is full | Removed the sentinel; shutdown travels only on the out-of-band event |
| 3 | `WAIT_FAILED` exited directly, skipping cleanup | Routed through `shutdown()` |
| 4 | A 3-second socket timeout outlasts the 2-second join bound | Reduced to 1.5 seconds and stated that `_exit` is what enforces the bound |
| 4 | The adapter recreation failure path was undefined | `shutdown()` on failure, with the reason quietly running adapterless is worse stated |
| 5 | The recreation failure rationale claimed a `NULL` slot causes `WAIT_FAILED`, contradicting the compact-array rule | Replaced with "the wait keeps working, so the tunnel is dead while pretending to be alive" |

## Remaining Follow-ups

Steps 3 through 6 in `design-audit.md` section 5. Step 5 (contingency for direct connection being
impossible) carries the largest semester risk.
