# Concurrency Model

**Project:** Direct-First P2P Virtual Network for Multiplayer Games
**Target platform:** Windows 10 / 11 x64 (client)
**Requirements:** [`spec.md`](spec.md) NFR-1, NFR-2, NFR-3
**Design:** [`architecture.md`](architecture.md) 3.1 Client / **Protocol:** [`protocol.md`](protocol.md) chapter 11 Timers
**Schedule:** [`roadmap.md`](roadmap.md) Phase 1

> Korean version: [`../kor/concurrency.md`](../kor/concurrency.md)

---

## 0. Where This Document Sits

**This is the single source for the client thread and loop structure.** It settles the
following.

- The thread set and the responsibility of each thread
- The wait set, the order inside one loop iteration, timers
- State ownership and queues
- The shutdown order

`architecture.md` 3.2 Concurrency Model is a summary that points here.

| In conflict | Wins |
|-------------|------|
| Thread set, wait and loop order, state ownership, shutdown order | this document |
| Tunnel wire format, session state, protocol timer values | `protocol.md` |
| Control plane operations and encoding, control request time limits | [`control_plane.md`](control_plane.md) |
| Module decomposition, data plane path, records and logs | `architecture.md` |
| Requirements and success criteria | `spec.md` |

If an implementation needs a value this document does not have, fix this document first instead
of deciding it in code.

---

## 1. Threads

**All code that touches tunnel state runs on one thread only.** Instead of sharing under a
lock, we do not share.

The work is not split into four threads (`[main]` / `[net_rx]` / `[tun_rx]` / `[timer]`).
There are two reasons.

- Four threads would modify session state and the send `sequence` without synchronization
- Telemetry upload would share the timer thread, so **a failure of the upload target** stops
  keepalive. That one violates NFR-3 at the design stage

| Thread | Responsibility | Shared state |
|--------|------|-----------|
| `[loop]` | UDP receive, Wintun receive, console commands, timers, session state, routing, all sending | **No tunnel state.** Shares only one end of four queues (telemetry, console, control request, control response) and the shutdown/cleanup events |
| `[telemetry]` | Consume the metric queue, upload to the telemetry service. Phase 9 onward | One queue |
| `[console]` | Read standard input. Scaffolding for Phase 1~5 | One command queue |
| `[control]` | Control plane TCP calls, DNS resolution. Phase 3 onward | One request queue and one response queue (chapter 8 The `[control]` Thread) |

**`[loop]` is the process main thread.** The rest exist only when that phase has their work.

| Span | Threads |
|------|---------|
| Phase 1~2 | `[loop]`, `[console]` |
| Phase 3~5 | `[loop]`, `[console]`, `[control]` |
| Phase 6~8 | `[loop]`, `[control]` |
| Phase 9 onward | `[loop]`, `[control]`, `[telemetry]` |

- `[console]` exists only in Phase 1~5. `[loop]` cannot block on standard input, so a
  separate thread reads it and pushes to a queue
- `[telemetry]` comes late because the place to upload to appears then. The telemetry service
  and the upload thread are deliverables of the same phase ([`roadmap.md`](roadmap.md))
- When the adapter arrives in Phase 6, the Wintun read event joins the `[loop]` wait set
  directly, so the thread count does not grow

---

## 2. Waiting

```text
WaitForMultipleObjects(n, handles, FALSE, timeout_ms)
```

| Rank | Handle | Creation | Valid range |
|------|------|------|-----------|
| 0 | Shutdown event | `CreateEvent` (manual reset) | Always |
| 1 | UDP socket event | `WSACreateEvent` + `WSAEventSelect(sock, ev, FD_READ)` | Always |
| 2 | Wintun read event | `WintunGetReadWaitEvent(session)` | Phase 6 onward |
| 3 | Console command event | `CreateEvent` (auto reset). Signaled by `[console]` | Phase 1~5 |
| 4 | Control response event | `CreateEvent` (auto reset). Signaled by `[control]` after pushing to the response queue | Phase 3 onward |

**The numbers above are logical ranks, not array indices.** `WaitForMultipleObjects`
requires an array densely filled with valid handles. Putting `NULL` in an empty slot produces
`WAIT_FAILED`. Whenever the composition changes, gather only the live handles into a dense
array and keep a separate mapping from logical source to actual index.

| Range | Array length | Handles joining and leaving |
|------|:---:|-----|
| Phase 1~2 | 3 | |
| Phase 3~5 | 4 | the control response event joins |
| Phase 6 onward | 4 | the console event leaves and the Wintun event joins |

`timeout_ms` is computed as below. **Do not subtract first.**

> **Why.** `GetTickCount64()` is unsigned, so when the deadline has already passed the
> subtraction underflows to a huge value, and narrowing to `DWORD` can land on `0xFFFFFFFF`,
> which is `INFINITE`. Then the loop never wakes.

```text
next_timeout:
    if no pending timers:     return INFINITE
    t = now()                              # read once
    d = next_deadline()
    if d <= t:                return 0
    return (DWORD) min(d - t, INFINITE - 1)
```

Do not call `now()` twice.

> **Why.** If the clock passes `d` between the comparison and the subtraction, it underflows,
> the timeout becomes about 49.7 days, and the loop effectively stops.

`WSAPoll` is not used because the Wintun read wait is a Win32 event handle, not a socket. To
mix sockets and event handles in one wait, it has to be `WaitForMultipleObjects`.
`WSAEventSelect` automatically switches the socket to non-blocking mode, so
`ioctlsocket(FIONBIO)` is not called separately.

---

## 3. One Loop Iteration

```text
busy = false
loop:
    r = WaitForMultipleObjects(n, handles, FALSE, busy ? 0 : next_timeout())

    if r == WAIT_FAILED:                      # unrecoverable
        log, then shutdown(); break           # goes through the same cleanup as the normal path
    if r == WAIT_OBJECT_0 + idx(SHUTDOWN):
        shutdown(); break

    WSAEnumNetworkEvents(sock, udp_ev, &ne)   # resets the socket event. Required
    u = drain_udp(MAX_DRAIN)                  # do not branch on r
    w = drain_wintun(MAX_DRAIN)
    if w == RESTART:
        recreate adapter session -> on failure shutdown(); break
        on success rebuild the array and index mapping from live handles
    drain_console()
    drain_control()                           # chapter 8. Phase 3 onward
    run_expired_timers(now())
    busy = (u == BUDGET) or (w == BUDGET)
```

`WAIT_FAILED` also goes through `shutdown()`. Exiting directly here skips the `CLOSE` send and
adapter cleanup, so the peer holds a dead session for 50 seconds and the adapter and route are
left behind. **Abnormal exit is exactly the case that needs cleanup.**

**Always call `WSAEnumNetworkEvents`.** This one call handles both the event reset and the
network event query.

> **Why.** The event created by `WSAEventSelect` is manual reset, and `recvfrom` only re-arms
> the `FD_READ` notification; it does not reset an already signaled event object. If this call
> is missed, the event stays signaled after the first datagram, `WaitForMultipleObjects`
> returns immediately every time, and the loop spins burning CPU.

**Do not branch on `r`.** Draining both every iteration makes the index order irrelevant, and
the cost when empty is one `WSAEWOULDBLOCK` and one `ERROR_NO_MORE_ITEMS`.

> **Why.** With `bWaitAll = FALSE`, `WaitForMultipleObjects` returns only the lowest index
> among the signaled handles. Branching on the return value starves the Wintun event placed
> after the UDP event while the UDP event keeps being signaled. That is exactly the situation
> under heavy game traffic.

**Both drains have a budget.** `MAX_DRAIN` is 64 per source per iteration. At 1472 bytes that
is 94KB, and parsing plus send cost is under 1ms, negligible against the shortest timer (200ms
`HELLO` retransmission). When a drain stops at the budget, `busy` becomes true and the next
wait returns immediately with timeout 0. The remaining data is processed in the next
iteration, and **the timers run once in between.**

> **Why.** If they run until empty with no budget, then when the arrival rate exceeds the
> processing rate `drain_udp` never returns and the timers never run. Keepalive and `HELLO`
> retransmission stop, and the "receive first, timers after" order set just above turns into
> "timers never run".

```text
drain_udp(budget) -> {EMPTY, BUDGET}:
    repeat budget times:
        n = recvfrom(sock, buf, MAX_DATAGRAM + 1, &src)   # buffer is 1473 bytes
        if n < 0:
            e = WSAGetLastError()
            if e == WSAEWOULDBLOCK:  return EMPTY      # source is empty
            if e == WSAEMSGSIZE:                       # even 1473 bytes was not enough
                drop_oversize_datagram++; continue     # the datagram is already consumed
            increment counter, then return EMPTY       # the socket stays in use
        if n > MAX_DATAGRAM:                           # exactly 1473 bytes arrived
            drop_oversize_datagram++; continue
        classify(buf, n, src)                          # protocol.md chapter 7
    return BUDGET                                      # budget exhausted. More remains
```

**Do not stop draining on an oversize datagram.** A datagram over `MAX_DATAGRAM` (1472) has
already left the socket queue by the time it is returned. Doing `return EMPTY` there ends that
iteration's drain after one item, so mixing in oversize datagrams alone would defeat batching.
Drop it and keep draining. For other errors the cause is unknown, so end that iteration.

**The receive buffer is `MAX_DATAGRAM + 1` (1473) bytes.** One byte larger, the decision
becomes the length comparison `n > MAX_DATAGRAM`, and a test can send 1473 bytes to reproduce
this path deterministically. If the buffer is sized to `MAX_DATAGRAM`, an oversize packet can
be detected only through the error code `WSAEMSGSIZE`, and that decision depends on Winsock's
truncation behavior.

**Still, the `WSAEMSGSIZE` branch is not removed.** Datagrams over 1473 bytes still come up as
that error. The two places are the same event, so they share the same counter.

`FD_READ` is level-triggered in nature. If data remains after `recvfrom`, the event is signaled
again, so reading one per iteration would not lose datagrams. **The reason for draining is
cost, not correctness.** Round-tripping the wait and `WSAEnumNetworkEvents` for every datagram
multiplies the wakeups by the number of received packets and delays timer processing by as
much. The correctness requirement is the `WSAEnumNetworkEvents` call above; draining is a
batching policy layered on top.

```text
drain_wintun(budget) -> {EMPTY, BUDGET, RESTART}:
    if session == NULL:  return EMPTY                  # Phase 1~5. No adapter
    repeat budget times:
        p = WintunReceivePacket(session, &size)
        if p == NULL:
            e = GetLastError()
            ERROR_NO_MORE_ITEMS -> return EMPTY         # ring is empty. Normal
            ERROR_HANDLE_EOF    -> signal shutdown event, then return EMPTY
            ERROR_INVALID_DATA  -> return RESTART       # ring corrupted. See below
            otherwise           -> increment counter, then return EMPTY
        guard = WintunReleaseReceivePacket(session, p) on scope exit
        route_and_send(p, size)
    return BUDGET
```

`WintunReleaseReceivePacket` is **attached as a scope guard.** If it is placed after
`route_and_send` in call order, an early return or exception inside leaks the ring packet. Each
miss permanently shrinks the ring buffer by that much, and eventually receiving stops.

`RESTART` is handled by `[loop]`. A corrupted ring requires a new session, and **a new session
gives a new read event handle.** Therefore session recreation and wait array rebuild are one
unit. Restarting inside `drain_wintun` leaves the caller waiting on the handle it just
invalidated.

If recreation fails, go to `shutdown()` instead of continuing without the adapter. The array is
rebuilt densely, so the Wintun handle simply drops out and the wait itself keeps working. That
is what makes it more dangerous. **The tunnel is dead but pretends to be alive.** The session
stays `CONNECTED` and keepalives keep going out, but not one game packet flows.

**Inject failure is a drop.** Once `DATA` passes [`protocol.md`](protocol.md) 8.4 DATA Inner
Validation, `WintunAllocateSendPacket` reserves a slot in the send ring and `WintunSendPacket`
places it. Allocation failing because the send ring has no room is not a fault; it happens
normally under high load. Drop that packet and increment `drop_inject_error`.

**Do not retry.**

> **Why.** This thread is also responsible for the timers and both receive paths, so waiting
> for a slot stops keepalive and `HELLO` retransmission in the meantime. When consumption has
> fallen far enough behind to fill the ring, retrying on the spot blocks at the same place
> again.

The cost of dropping is one inner packet. **Whether that cost is recovered depends on the inner
protocol.** If the inner protocol is TCP, retransmission fills the gap, so it is a delay (the
Minecraft path in [`architecture.md`](architecture.md) chapter 4 Data Plane Path is that case);
if it is an unreliable protocol like UDP, that packet is lost for good. The tunnel does not
distinguish the inner protocol (NFR-2), so this is not written as "always just a delay".

**What is fixed now is one counter and four behavior contracts.** The counter is
`drop_inject_error`, one only. The behavior is: **when inject fails, drop that packet,
increment the counter, do not retry, and do not stop the loop.** These four hold under any API
shape.

**Where a `DATA` that passed 8.4 DATA Inner Validation goes in a build without the adapter
(Phase 4~5) is also decided here.** In that range there is no adapter to inject into, so the
next step that section points to with "just before Wintun injection" is empty.

| Build | `DATA` that passed 8.4 |
|------|------------------|
| Adapter present (Phase 6 onward) | the injection path above |
| No adapter, test build | handed to the **payload hook**. The hook is a single function that takes the extracted payload bytes and length as they are, and the M-5 byte-for-byte comparison and the echo responder of [`roadmap.md`](roadmap.md) Phase 4 attach at that point |
| No adapter, product build | dropped, and `drop_no_sink` is incremented |

**The hook is not written as a test-only path.** It has to hang at the same point after 8.1~8.4
validation so that the test exercises the real receive path. This is the same reason
`protocol.md` chapter 7 Receive Classification pins the receive entry point to one place with
the source hygiene test, and this is the symmetric exit. `drop_no_sink` is a diagnosis in
itself, because a non-zero value in a product build means the design has drifted.

**Splitting "ring full" into a separate counter is done after confirmation.** Which call reports
the failure and in what form (return value, `GetLastError`, or whether it is reported at all) is
decided by the Wintun header in use and its documentation. This repo does not have that header
and it has not been checked. Detailed counters are added to this section after confirmation.
When that confirmation happens is owned by `roadmap.md`.

> **Why.** Writing an unconfirmed API shape here makes the implementation trust it as is.

`drain_console` drains the command lines that `[console]` pushed to the queue. The event is auto
reset, so no separate reset call is needed. From Phase 6 onward both this handle and the thread
go away.

**The `quit` command signals the shutdown event.** It does not call `shutdown()` directly. That
way `[telemetry]` and `[control]` wake on the same signal, and the next iteration's wait returns
from the shutdown handle and follows the order of chapter 7 Shutdown exactly. The "if not done yet"
in chapter 7 (1) is this path.

**Send backlog created by the console counts toward `busy`.** `send <n>` (Phase 4~5) and `raw`
(`architecture.md` 3.5 Startup Inputs) are not pushed out in one iteration but split the same
way as `MAX_DRAIN`, so an iteration with backlog left must have `busy = true` for the next wait
to return immediately with timeout 0. Otherwise the remaining sends stretch out by one timer
interval each.

`drain_control` drains the results that `[control]` pushed to the response queue and applies
them to session state (chapter 8 The `[control]` Thread). Both queues are small and requests arrive
at human speed, so neither has a budget.

The Wintun documentation recommends spinning briefly on `ERROR_NO_MORE_ITEMS` under high load
before waiting on the event. **We do not spin.** This loop is also responsible for the timers,
so spinning delays keepalive and `HELLO` retransmission by that much. Spinning is a throughput
optimization and our goal is latency (NFR-1). If Phase 7 measurements show receive as the
bottleneck, revisit it then.

**Adapter reads are counted by `adapter_rx`.** It goes up every time `WintunReceivePacket`
returns one packet. This is the value that the Phase 6 verification reads as the "adapter read
counter" (`roadmap.md`). The injection side does not count successes. Only failures are counted,
by `drop_inject_error`.

**The counters in this section are at a different layer from the counters in `protocol.md`.**
`drop_oversize_datagram`, `drop_inject_error`, `drop_no_sink`, and `adapter_rx` count results
returned by the socket and adapter APIs, so this document defines them. The `drop_*` counters in
`protocol.md` chapter 7 Receive Classification and chapter 8 Receive Validation Pipeline belong
to wire classification and the validation pipeline. Do not mix the two sources when making a
list.


---

## 4. Timers

- The monotonic clock is `GetTickCount64()`. It is a 64-bit millisecond value, so there is
  effectively no wraparound (about 580 million years). Only RTT measurement uses
  `QueryPerformanceCounter`, and **values from the two clocks are never compared with each
  other.**
- The timer set is small. The table in [`protocol.md`](protocol.md) chapter 11 Timers is a
  single-digit count, and in the minimum scope there is one session. Every iteration does a
  linear scan for the earliest deadline. No timer wheel is needed.
  - The "expiry value" table in the same chapter (discard list, nonce, probe, `pending_pings`)
    holds **a lifetime attached to each item**, not a session timer, so it is checked while
    walking those items
- `protocol.md` chapter 11 Timers uses a strict inequality for deadline comparison and defines
  priority by dequeue time. Receive events dequeued in the same iteration are processed before
  that iteration's timers. **This loop order, drain receives first and run timers after, is the
  implementation of that rule.**
  - The reason it is not arrival time is the drain budget, and the rationale is in
    `protocol.md` chapter 11


---

## 5. State Ownership

All of the following is owned solely by `[loop]`. No mutex and no atomic variable. None is needed.

- The session state machine, `session_epoch`, the send `sequence`, the replay-prevention
  bitmap, `peer_endpoint`, the last receive time, the candidate list, `pending_pings`, and all
  drop counters
- **The local record file handle and its writes**
  ([`architecture.md`](architecture.md) chapter 9 Telemetry and Records)
- Values received from the control plane such as `room_id`, `peer_id`, `peer_token`, and the
  remote peer info. `[control]` only passes those values through the response queue; it does
  not hold them as its own state. The one exception is the resolved server address noted in
  chapter 8 The `[control]` Thread

**The send `sequence` increment happens only in `[loop]`.** `DATA` and `KEEPALIVE` share the
same counter, so incrementing from two threads sends out two packets with the same number. The
receiver's duplicate suppression ([`protocol.md`](protocol.md) 4.5 Duplicate Suppression and
Loss Accounting) correctly drops the later one. It is a forged duplicate we made ourselves.

No struct gets a "thread safe" comment. Single ownership is the only rule, and the moment one exception is allowed the rule is gone.

---

## 6. Telemetry Isolation

`[telemetry]` is a separate thread for **isolation**, not parallelism.

- `[loop]` pushes records into a fixed-size SPSC ring buffer. With exactly one producer and one
  consumer, it is implemented **without a lock** using only atomic head/tail
- When full, the new record is dropped and `telemetry_queue_dropped` is incremented. No
  waiting, no failure report
- `[telemetry]` drains the queue and uploads to the telemetry service. Whether the upload
  blocks or fails is irrelevant to `[loop]`
- Telemetry uses **its own TCP socket to the telemetry service.** It is not shared with the
  control plane socket. The single-socket constraint in [`protocol.md`](protocol.md) chapter 6
  Socket Ownership applies only to the UDP data plane
- **There is not a single lock in the data plane.**

**The ring holds 256 entries.** The basis is the background production rate of automatic
periodic metrics. Looking only at fixed-period items like `PING` at 5 seconds and keepalive at
15 seconds, the average is under 1 record/second, and at that rate 256 slots hold minutes'
worth, which covers a span where one upload hits the socket timeout (chapter 7 Shutdown) and
consumption falls behind by a few seconds.

**Bursts are not covered by this size.** Test-driven consecutive connection attempts, manual
RTT samples, and spans where drop counters pile up can produce several records in the same
second, and then it fills faster. Then new records are dropped. That is the intended behavior.

> **Why.** Metrics are a thing that tolerates loss, and raising the size to cover bursts only
> holds stale metrics longer during an outage.

**The console command queue holds 16 lines and uses the same policy.** When full, the new line
is dropped and `console_queue_dropped` is incremented. The producer is `[console]` and the
consumer is `[loop]`, one each, so no lock is needed here either.

- **This counter alone is an exception to the sole-ownership rule of chapter 5 State Ownership.**
  The side that drops is the producer `[console]`, so that thread increments it. It is one atomic
  variable and `[loop]` only reads it (the `counter` event in
  [`architecture.md`](architecture.md) chapter 9). Every other drop counter is incremented by
  `[loop]`
- **The two queues do not share one counter name.** Without names, Phase 1 verification cannot
  say what it is looking at

> **Why 16 lines.** A situation where 16 lines of human-typed commands back up means `[loop]`
> is already blocked, and a bigger queue would not get processed then.

Dropping the oldest is better for metric quality, but that requires the producer to move the
consumer's tail, which needs a lock. **We do not bring a lock into `[loop]` to save one more
metric.** Even with a mutex the critical section is short and would rarely block in practice,
but "rarely" is not a guarantee. If the consumer is descheduled while holding the lock,
`[loop]` stalls for that long.

The Phase 5 "server down for 10 minutes" test (NFR-3) cannot pass without this isolation. In
the 4-thread design, a TCP upload to an unresponsive **telemetry service** blocks the timer
thread for tens of seconds, keepalive does not go out in the meantime, and the NAT mapping
expires. A remote service outage tears down the tunnel.

**Even with the services split in two, this isolation is still needed.** The split only keeps a
telemetry outage from blocking the control plane; inside the client, only thread isolation
stops the upload from blocking `[loop]`.


---

## 7. Shutdown

- `[telemetry]` wakes every second with `WaitForSingleObject(shutdown_ev, 1000)` and drains the
  queue. The event is not signaled on every push. The shutdown event is manual reset, so
  signaling it wakes this wait as well, immediately.
- `[telemetry]` sets `SO_SNDTIMEO` / `SO_RCVTIMEO` to 1.5 seconds on the upload socket. It
  checks the shutdown event between uploads.

**1.5 seconds does not bring the total time of one upload under 2 seconds.** The two timeouts
are per operation, so if send and receive each hit theirs the sum goes up to 3 seconds. So what
keeps the join bound is not this value but the `_exit` below. The reason for 1.5 seconds is
**to finish the common case fast**, and going larger loses even that effect.

`SetConsoleCtrlHandler` signals the shutdown event. **The handler never touches state owned by
`[loop]`.** It runs in a different thread context.

**The return timing differs by control signal type.**

| Signal | Handler |
|------|--------|
| `CTRL_C_EVENT`, `CTRL_BREAK_EVENT` | signal the event and return `TRUE` immediately |
| `CTRL_CLOSE_EVENT`, `CTRL_LOGOFF_EVENT`, `CTRL_SHUTDOWN_EVENT` | signal the shutdown event, then wait for the cleanup-complete event up to inside the OS grace period (3 seconds) before returning |

For the latter three signals, **Windows terminates the process the moment the handler
returns.** Signaling and returning means neither the `CLOSE` send nor the adapter cleanup runs.
Closing the console window is a normal way to exit this program, so this is the main path, not
exception handling.

`[loop]`'s `shutdown()` runs in the order below. `[control]` exists only from Phase 3 onward,
so before that (8) is `[telemetry]` only.

| # | Action |
|---|---------|
| 1 | signal the shutdown event (if not already) |
| 2 | send `CLOSE` once per target session as defined by [`protocol.md`](protocol.md) 5.6 `CLOSE`. That section defines the targets and reason values. The conditions are not restated here |
| 3 | **if a session that has not ended yet had reached `CONNECTED`, write the end line to the local record file** ([`architecture.md`](architecture.md) chapter 9. A session already in a terminal state has written that line and does not write it again) |
| 4 | **emit all counters as a `counter` event** (`architecture.md` chapter 9) |
| 5 | end the adapter session |
| 6 | clean up the adapter/address/route |
| 7 | **signal the cleanup-complete event** |
| 8 | join `[telemetry]` and `[control]` |

- **Reaching a terminal state ends the process too.** When a session becomes `FAILED` or
  `CLOSED` (`protocol.md` 9.6 Failure Transitions, 5.6 `CLOSE`), `[loop]` leaves that session's
  record line and counters and then goes to `shutdown()` on its own. In the minimum scope there
  is one session, so once it ends the process has nothing left to do. It does not wait for the
  user's `quit`. **This line is revisited once automatic retry is decided** (the open item in
  `protocol.md` 10.4 Endpoint Learning). With retry, `FAILED` would no longer mean shutdown.
- **No shutdown marker goes into the queue.** The queue drops new items when full, so if the
  marker is dropped at exactly that moment, `[telemetry]` never sees the shutdown. Shutdown is
  delivered only through the event outside the queue.
- **Cleanup comes before join.** Because of this order, the console handler can return without
  waiting for telemetry, and if the OS kills the process at that point, all that is lost is a
  few metrics.
- The join bound is 2 seconds for both threads combined. Past it, abandon the remaining records
  and the in-flight control request, skip the join, and end immediately with `_exit`. Cleanup
  already finished at (7), so this is safe. **What actually enforces the bound is this `_exit`,
  not the socket timeouts.** The socket timeouts only end the common case cleanly; what keeps
  shutdown from ever being held hostage to telemetry service availability is `_exit`.
- **`[console]` is not joined.** This thread blocks on reading standard input. **This design
  does not use any means to cancel that read from outside.** So joining would stall shutdown
  until the user types one more line. Signal the shutdown event, leave it, and let process exit
  take care of it. `[console]` owns only the command queue and has no external resources to
  clean up, so nothing is lost.
- Cleanup of the adapter, virtual IP address, and route, and handling of leftovers after
  abnormal exit, are in [`windows-prereq.md`](windows-prereq.md) section 3.


---

## 8. The `[control]` Thread

Control plane TCP calls and DNS resolution are not done in `[loop]`. `getaddrinfo` is also a
synchronous call and sits in the same place.

> **Why.** If the control server is dead, a TCP `connect` burns tens of seconds in SYN retries.
> Phase 4 verification creates that situation deliberately. If that time passes in `[loop]`,
> `HELLO` retransmission (200ms), keepalive (15s), and response to the shutdown event all stop.
> It is the reason chapter 6 Telemetry Isolation discarded the 4-thread design coming back through
> another door, and a violation of [`spec.md`](spec.md) NFR-3.

**Putting a non-blocking socket into the `[loop]` wait set is not used.** TCP connection, HTTP
parsing, and DNS would all have to be rewritten as state machines, and `getaddrinfo` has no
non-blocking form. One thread is cheap.

| Item | Value |
|------|-----|
| Queues | `[loop]` -> `[control]` request queue, `[control]` -> `[loop]` response queue. Each an SPSC ring of **8 entries**. Same implementation as the ring in chapter 6 Telemetry Isolation, no lock |
| When full | If the request queue is full, `[loop]` drops that request, increments `control_queue_dropped`, and **ends that attempt with `CONTROL_PLANE_EXCHANGE_FAILED`.** Only one request is outstanding at a time (below), so it does not fill on the normal path. If it fills, `[control]` has stalled |
| Outstanding requests | **One at a time.** `[loop]` does not push the next request before receiving the response. `get_peers` polling is "500ms after response received". If the poll timer expires while the previous request is outstanding, that round is skipped |
| Wakeup | `[control]` waits on the shutdown event and the request event (auto reset, signaled by `[loop]`) with `WaitForMultipleObjects`. `[loop]` wakes on the response event (rank 4 in chapter 2 Waiting) |
| Socket | A new TCP socket per request. Non-blocking `connect` + `select` for the connection time limit, `SO_SNDTIMEO`/`SO_RCVTIMEO` for the send/receive time limits. Values are in [`control_plane.md`](control_plane.md) 8.2 Time Limits |
| DNS | Once at startup. `[control]` holds the resulting single IPv4 address and uses it for all later requests. This is the only state `[control]` has |
| Shutdown | On seeing the shutdown event, close the socket and return without waiting for the response of the in-flight request. Because of the socket time limits it can be late by at most one request bound (`control_plane.md` 8.2 Time Limits), and that is fenced by the 2-second join bound and `_exit` in chapter 7 Shutdown |

**`[control]` neither reads nor writes session state.** It sends what it takes from the request
queue and pushes what it receives to the response queue. Applying responses to session state is
`drain_control`, and that is `[loop]`. Response interpretation (JSON parsing, error
classification) may be done by `[control]`. The result is a value, not shared state.

**It is not merged with `[telemetry]`.** Telemetry upload tolerates loss and does not report
failure. A control request needs a response, and its failure is `CONTROL_PLANE_EXCHANGE_FAILED`.
Mixing the two natures in one thread lets a telemetry service outage delay control requests and
eat into the `get_peers` deadline (60s). That would undo, inside the client, the reason the two
services were split (`architecture.md` 3.4 Telemetry Service).
