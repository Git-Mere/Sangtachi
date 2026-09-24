# Architecture

**Project:** Direct-First P2P Virtual Network for Multiplayer Games
**Target platform:** Windows 10 / 11 x64 (client), Linux/AWS EC2 (control server)

> Korean version: [`../kor/architecture.md`](../kor/architecture.md)

---

## 1. Design Principles

| # | Principle | Meaning |
|---|------|------|
| 1 | Direct-First | Game traffic flows over the direct peer-to-peer path whenever possible. AWS is for coordination only and is not on the data path. |
| 2 | Plane separation | The control plane (Python/AWS) and the data plane (C++/client) operate and are tested independently of each other. |
| 3 | Game-agnostic | No Minecraft-specific logic goes into the tunnel layer. Minecraft is only the validation target. |
| 4 | Observable | Every connection attempt records success/failure and the **failure stage**. |
| 5 | Minimal dependencies | Wintun handles virtual interface access only. STUN, NAT traversal, tunneling, and routing are implemented directly. |
| 6 | Risk first | The most uncertain part (NAT traversal) is validated first. The virtual adapter comes after. |

---

## 2. System Composition

```text
             AWS (1 EC2 instance, 2 processes, separate ports)
   +---------------------------+     +--------------------------+
   | Python Coordination Server|     | Python Telemetry Service |
   |  rooms / peers            |     |  telemetry ingest        |
   |  candidate exchange       |     |  connection results      |
   +-------------+-------------+     +------------+-------------+
                 |                                |
                 +------- Amazon DynamoDB --------+
                 |                                |
      coordination traffic (TCP/JSON)   metric upload (TCP/JSON)
                 |                                |
                 +----------------+---------------+
                                  |
              +-------------------+-------------------+
              |                                       |
      +-------v---------+                    +--------v--------+
      | Windows Client A|                    | Windows Client B|
      |  Wintun Adapter |                    |  Wintun Adapter |
      |  Router         |                    |  Router         |
      |  Tunnel         |<==================>|  Tunnel         |
      |  Hole Punch     |   Direct P2P UDP   |  Hole Punch     |
      |  STUN Client    |   (game traffic)   |  STUN Client    |
      |  Telemetry      |                    |  Telemetry      |
      +-------+---------+                    +--------+--------+
              |                                       |
      Minecraft Server                         Minecraft Client
        10.100.0.1:25565                          10.100.0.2
```

Even if the control plane dies, **an already established P2P tunnel keeps working.** The
control plane is needed only for session establishment.

**Telemetry collection is not the control plane's job.** It is a separate service with a
separate deployment unit. The two services run as different processes on the same EC2
instance, but there is no dependency in either direction. The boundary contract is owned by
3.4 Telemetry Service.

---

## 3. Components

### 3.1 Client (C++20)

| Module | Responsibility | External dependency |
|------|------|-----------|
| `network/udp_socket` | Winsock2 init/teardown, UDP socket creation, non-blocking send/receive, exposing the event handle (3.2 Concurrency Model) | Winsock2 |
| `network/endpoint` | `IP:Port` value type, parsing, comparison, serialization | None |
| `network/stun_client` | STUN Binding Request construction, transaction ID management, response parsing, `XOR-MAPPED-ADDRESS` decoding, timeout/retry | None (direct RFC 5389 implementation) |
| `peer/peer` | Peer identifier, virtual IP, candidate endpoint list | None |
| `peer/hole_punch` | Simultaneous bidirectional send, retry, success decision, failure reason classification | None |
| `peer/session` | Handshake, keepalive, connection state machine, RTT measurement | None |
| `tunnel/packet` | Tunnel header serialization/deserialization, corrupt packet validation | None |
| `tunnel/tunnel` | Dispatch by packet type, encapsulation/decapsulation, sequence management | None |
| `tunnel/router` | Virtual IP -> peer session mapping, destination decision | None |
| `adapter/wintun_adapter` | Virtual adapter create/open and packet read/inject (Wintun), virtual IP address and route setup (IP Helper) | **Wintun (approved)**, IP Helper |
| `telemetry/telemetry` | Metric collection, local buffering, transmission to the telemetry service | None |
| `control/control_client` | Control plane HTTP/JSON calls and DNS resolution. Owned by the `[control]` thread (3.2.8 The `[control]` Thread). Operations and encoding are in [`control_plane.md`](control_plane.md) | Winsock2 |

The client is a single process. Thread composition and state ownership are in 3.2 Concurrency Model.

**All UDP send/receive shares one local socket, and one receive loop owns that socket
exclusively.** This constraint holds from Phase 2 onward.

> **Why.** If different sockets are used, the NAT maps each to a different public port, so
> the endpoint discovered through STUN does not apply to the tunnel.

Sharing the socket means STUN responses and tunnel packets arrive in the same queue. If the
STUN client calls `recvfrom` directly, the two readers steal each other's packets. The STUN
client only registers requests; responses are delivered to it by the receive loop.
Classification rules and the required socket options (`SO_EXCLUSIVEADDRUSE`,
`SIO_UDP_CONNRESET` off, no `connect()` call) are in [`protocol.md`](protocol.md) chapter 6
Socket Ownership and chapter 7 Receive Classification.

### 3.2 Concurrency Model

**All code that touches tunnel state runs on one thread only.** Instead of sharing under a
lock, we do not share.

The work is not split into four threads (`[main]` / `[net_rx]` / `[tun_rx]` / `[timer]`).
There are two reasons.

- Four threads would modify session state and the send `sequence` without synchronization
- Telemetry upload would share the timer thread, so **a failure of the upload target** stops
  keepalive. That one violates NFR-3 at the design stage

#### 3.2.1 Threads

| Thread | Responsibility | Shared state |
|--------|------|-----------|
| `[loop]` | UDP receive, Wintun receive, console commands, timers, session state, routing, all sending | **No tunnel state.** Shares only one end of four queues (telemetry, console, control request, control response) and the shutdown/cleanup events |
| `[telemetry]` | Consume the metric queue, upload to the telemetry service. Phase 9 onward | One queue |
| `[console]` | Read standard input. Scaffolding for Phase 1~5 | One command queue |
| `[control]` | Control plane TCP calls, DNS resolution. Phase 3 onward | One request queue and one response queue (3.2.8 The `[control]` Thread) |

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

#### 3.2.2 Waiting

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

#### 3.2.3 One Loop Iteration

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
    drain_control()                           # 3.2.8. Phase 3 onward
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
Minecraft path in chapter 4 Data Plane Path is that case); if it is an unreliable protocol like
UDP, that packet is lost for good. The tunnel does not distinguish the inner protocol (NFR-2),
so this is not written as "always just a delay".

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
validation so that the test exercises the real receive path. This is the same reason chapter 7
pins the receive entry point to one place with the source hygiene test, and this is the
symmetric exit. `drop_no_sink` is a diagnosis in itself, because a non-zero value in a product
build means the design has drifted.

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
from the shutdown handle and follows the order of 3.2.7 Shutdown exactly. The "if not done yet"
in 3.2.7 (1) is this path.

**Send backlog created by the console counts toward `busy`.** `send <n>` (Phase 4~5) and `raw`
(3.5 Startup Inputs) are not pushed out in one iteration but split the same way as `MAX_DRAIN`,
so an iteration with backlog left must have `busy = true` for the next wait to return
immediately with timeout 0. Otherwise the remaining sends stretch out by one timer interval
each.

`drain_control` drains the results that `[control]` pushed to the response queue and applies
them to session state (3.2.8 The `[control]` Thread). Both queues are small and requests arrive
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


#### 3.2.4 Timers

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


#### 3.2.5 State Ownership

All of the following is owned solely by `[loop]`. No mutex and no atomic variable. None is needed.

- The session state machine, `session_epoch`, the send `sequence`, the replay-prevention
  bitmap, `peer_endpoint`, the last receive time, the candidate list, `pending_pings`, and all
  drop counters
- **The local record file handle and its writes** (chapter 9 Telemetry and Records)
- Values received from the control plane such as `room_id`, `peer_id`, `peer_token`, and the
  remote peer info. `[control]` only passes those values through the response queue; it does
  not hold them as its own state. The one exception is the resolved server address noted in
  3.2.8 The `[control]` Thread

**The send `sequence` increment happens only in `[loop]`.** `DATA` and `KEEPALIVE` share the
same counter, so incrementing from two threads sends out two packets with the same number. The
receiver's duplicate suppression ([`protocol.md`](protocol.md) 4.5 Duplicate Suppression and
Loss Accounting) correctly drops the later one. It is a forged duplicate we made ourselves.

No struct gets a "thread safe" comment. Single ownership is the only rule, and the moment one exception is allowed the rule is gone.

#### 3.2.6 Telemetry Isolation

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
worth, which covers a span where one upload hits the socket timeout (3.2.7 Shutdown) and
consumption falls behind by a few seconds.

**Bursts are not covered by this size.** Test-driven consecutive connection attempts, manual
RTT samples, and spans where drop counters pile up can produce several records in the same
second, and then it fills faster. Then new records are dropped. That is the intended behavior.

> **Why.** Metrics are a thing that tolerates loss, and raising the size to cover bursts only
> holds stale metrics longer during an outage.

**The console command queue holds 16 lines and uses the same policy.** When full, the new line
is dropped and `console_queue_dropped` is incremented. The producer is `[console]` and the
consumer is `[loop]`, one each, so no lock is needed here either.

- **This counter alone is an exception to the sole-ownership rule of 3.2.5 State Ownership.**
  The side that drops is the producer `[console]`, so that thread increments it. It is one
  atomic variable and `[loop]` only reads it (the `counter` event in chapter 9). Every other
  drop counter is incremented by `[loop]`
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


#### 3.2.7 Shutdown

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
| 3 | **if a session that has not ended yet had reached `CONNECTED`, write the end line to the local record file** (chapter 9. A session already in a terminal state has written that line and does not write it again) |
| 4 | **emit all counters as a `counter` event** (chapter 9) |
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


#### 3.2.8 The `[control]` Thread

Control plane TCP calls and DNS resolution are not done in `[loop]`. `getaddrinfo` is also a
synchronous call and sits in the same place.

> **Why.** If the control server is dead, a TCP `connect` burns tens of seconds in SYN retries.
> Phase 4 verification creates that situation deliberately. If that time passes in `[loop]`,
> `HELLO` retransmission (200ms), keepalive (15s), and response to the shutdown event all stop.
> It is the reason 3.2.6 Telemetry Isolation discarded the 4-thread design coming back through
> another door, and a violation of [`spec.md`](spec.md) NFR-3.

**Putting a non-blocking socket into the `[loop]` wait set is not used.** TCP connection, HTTP
parsing, and DNS would all have to be rewritten as state machines, and `getaddrinfo` has no
non-blocking form. One thread is cheap.

| Item | Value |
|------|-----|
| Queues | `[loop]` -> `[control]` request queue, `[control]` -> `[loop]` response queue. Each an SPSC ring of **8 entries**. Same implementation as the ring in 3.2.6 Telemetry Isolation, no lock |
| When full | If the request queue is full, `[loop]` drops that request, increments `control_queue_dropped`, and **ends that attempt with `CONTROL_PLANE_EXCHANGE_FAILED`.** Only one request is outstanding at a time (below), so it does not fill on the normal path. If it fills, `[control]` has stalled |
| Outstanding requests | **One at a time.** `[loop]` does not push the next request before receiving the response. `get_peers` polling is "500ms after response received". If the poll timer expires while the previous request is outstanding, that round is skipped |
| Wakeup | `[control]` waits on the shutdown event and the request event (auto reset, signaled by `[loop]`) with `WaitForMultipleObjects`. `[loop]` wakes on the response event (rank 4 in 3.2.2 Waiting) |
| Socket | A new TCP socket per request. Non-blocking `connect` + `select` for the connection time limit, `SO_SNDTIMEO`/`SO_RCVTIMEO` for the send/receive time limits. Values are in [`control_plane.md`](control_plane.md) 8.2 Time Limits |
| DNS | Once at startup. `[control]` holds the resulting single IPv4 address and uses it for all later requests. This is the only state `[control]` has |
| Shutdown | On seeing the shutdown event, close the socket and return without waiting for the response of the in-flight request. Because of the socket time limits it can be late by at most one request bound (`control_plane.md` 8.2 Time Limits), and that is fenced by the 2-second join bound and `_exit` in 3.2.7 Shutdown |

**`[control]` neither reads nor writes session state.** It sends what it takes from the request
queue and pushes what it receives to the response queue. Applying responses to session state is
`drain_control`, and that is `[loop]`. Response interpretation (JSON parsing, error
classification) may be done by `[control]`. The result is a value, not shared state.

**It is not merged with `[telemetry]`.** Telemetry upload tolerates loss and does not report
failure. A control request needs a response, and its failure is `CONTROL_PLANE_EXCHANGE_FAILED`.
Mixing the two natures in one thread lets a telemetry service outage delay control requests and
eat into the `get_peers` deadline (60s). That would undo, inside the client, the reason the two
services were split (3.4 Telemetry Service).

---

### 3.3 Control Plane (Python)

**The single source for the control plane is [`control_plane.md`](control_plane.md).**
Operations, encoding, errors, identifiers, room and peer state transitions, DynamoDB table
design, storage contracts, server module structure, and the client-side call contract are all
in that document. This section records only architecture-level facts.

- Uses the Python standard library (`asyncio`, `json`) and the AWS SDK (`boto3`). No web
  framework. **`boto3` is not part of the standard library.** It was approved under
  [`spec.md`](spec.md) NFR-5
- State lives in Amazon DynamoDB, and the server does not hold room/peer state in memory. The
  five storage contracts are owned by `control_plane.md` 6.1 Storage Contracts
  - **Why this was decided and what was rejected** is owned by
    [ADR 0004](decisions/0004-state-store-dynamodb.md)
- So there is no "restart recovery" procedure. Keeping room state across a restart, as
  `spec.md` NFR-3 requires, holds through the storage contracts, not a recovery procedure
- It handles no game traffic, no metrics, and no UDP. The list of what it does not do is in
  `control_plane.md` 1.1 What It Does and Does Not Do


### 3.4 Telemetry Service (Python)

**A separate service, split from the control plane.** It is not a module of the control plane.

| Module | Responsibility |
|------|------|
| `server.py` | HTTP/JSON ingest endpoint |
| `ingest.py` | Receive, validate, and store connection results and performance metrics in DynamoDB |

The separation has four contracts.

| Contract | Content |
|------|------|
| Split 1 | The control plane does not call the telemetry service |
| Split 2 | The telemetry service does not call the control plane. It does not ask the control plane whether a received identifier is valid |
| Split 3 | The only contact point between the two services is the identifier namespace. `room_id` and `peer_id` are issued by the control plane and telemetry records them as opaque values. Cross-checking happens at analysis time |
| Split 4 | Even if the telemetry service process stops, control plane operations are not blocked **on the call path**. The contract's scope ends at inter-service calls. Shared resource exhaustion on the same instance is not covered by this contract |

**Because the contact point is identifiers only, the values telemetry receives cannot be
trusted.** A `room_id` the control plane never issued is stored as is. Blocking this at ingest
time would require telemetry to ask the control plane, which breaks Split 2. So it is not
blocked. Instead, at analysis time, filter by cross-checking against the control plane's
records. This limitation and the cross-check method are owned by [`roadmap.md`](roadmap.md)
Phase 9.

**Placing them on the same EC2 instance is deployment convenience, not coupling.** But while
on one machine they share CPU and disk.

**Split 4 does not cover that risk.** If telemetry exhausts disk or CPU, the control plane
stops with it, and that happens even when this contract is honored. Having no design dependency
and having isolated resources are different statements.

**There is no resource isolation yet.** What to split it with is owned by the pre-start items
of `roadmap.md` Phase 9.

---

### 3.5 Startup Inputs

The values the client receives at startup and how they are passed. **Phase 1~5 uses CLI
arguments only.** There is no configuration file. The values are few, and if values that change
per test live in a file, which file a run used does not appear in the record. When a file
becomes necessary is decided by [`roadmap.md`](roadmap.md).

| Input | Argument | Required | Value |
|------|------|:---:|-----|
| Role | First positional argument `host` or `player` | Yes from Phase 3. Phase 1~2 only accepts and stores it, and starts without it | `host` calls `create_room`, `player` calls `join_room` |
| Control server address | `--server <name or IPv4>[:<port>]` | Yes from Phase 3. Phase 1~2 only accepts and stores it, and starts without it | DNS name or IPv4 literal. If the port is omitted, `CONTROL_PORT` from [`control_plane.md`](control_plane.md) 2.6 Constants. No real address is hard-coded in documents |
| Room code | `--room <6 chars>` | `player` always. `host` when `--rejoin` is given. Phase 3 onward | Format from `control_plane.md` 2.1 `room_id`. Lowercase is accepted. The server normalizes to uppercase |
| Rejoin proof | `--rejoin <peer_id>:<peer_token>` | No | If present, `join_room` is called in its rejoin form (`control_plane.md` 4.3 `join_room`). Usable with the `host` role too. A restarted host is also a rejoin |
| STUN servers | `--stun <name or IPv4>:<port>`, repeatable | No | If given, it **replaces the default list below entirely**. The behavior when the list has one entry is below the table |

**A `--stun` list with one entry leaves one `WARN` line at startup.** That list cannot produce
responses from two different servers, so it ends as `STUN_DISCOVERY_FAILED` per "Server
selection" below.

> **Why.** The warning comes at startup because telling the user the input is wrong is better
> than waiting 5 seconds and then seeing the failure.

**STUN default list.** The source is `DEFAULT_STUN_SERVERS` in
[`tools/nat-probe/natprobe.py`](../../tools/nat-probe/natprobe.py). The 22 measurements ran
with that list. **The values in the two places have to be the same.** If one changes, change
the other with it.

```text
stun.l.google.com:19302
stun1.l.google.com:19302
stun.cloudflare.com:3478
stun.nextcloud.com:3478
```

**Server selection.** Query the first two servers in the list **simultaneously** on the same
socket. The transaction ID separates the responses ([`protocol.md`](protocol.md) chapter 13
STUN Usage Scope). The STUN deadline is per server (`protocol.md` chapter 11 Timers). If one
server hits its own deadline, switch to the next server in the list and query again, and that
server's retry schedule and deadline start fresh. The bound for the whole step is computed by
that document.

**If the list is exhausted without responses from two different servers, it is
`STUN_DISCOVERY_FAILED`.** Even one response only is a failure.

> **Why.** M-2 and the "supported network conditions" decision need two, and proceeding with
> one enters the punch without distinguishing destination-dependent mapping, so that failure
> gets recorded as a NAT failure.

**STUN server names are resolved once at startup.** The default list is all DNS names, so
resolution is needed, and that call (`getaddrinfo`) is synchronous, so it is not placed inside
`[loop]`.

- **The whole list is resolved before `[loop]` starts.** Phase 1~2 has no `[control]` thread
  (3.2.1 Threads), so that thread cannot be borrowed for it either
- If there are several results, the first IPv4 address is used. This is the same rule as
  `control_plane.md` 3.2 Address. `AAAA` is not used
- **An entry that fails to resolve is removed from the list and leaves one `WARN` line.** If
  fewer than two entries remain, it is `STUN_DISCOVERY_FAILED` per the rule above

Unlike the control server address, a resolution failure here is not a startup failure. The list
has several entries, so one dead entry still allows progress.

**Keeping the rejoin proof.** The client prints the `peer_id` and `peer_token` from the
`join_room` and `create_room` responses as one line on **standard output**. A human copies it
and passes it with `--rejoin`. It is not stored automatically in a file. Automatic retry is
undecided (`protocol.md` 10.4 Endpoint Learning), and the storage method is decided together
with it.

**The room code also goes to standard output.** The host has to pass the `room_id` from the
`create_room` response to the other side. It is not put in the standard error log (chapter 9).

> **Why.** If the log file becomes the list of room codes, reading the log is room hijacking.
> Standard output is the screen a human watches, and not redirecting it is the default.

```text
ROOM <room_id>
REJOIN <peer_id>:<peer_token>
```

The first word of each of the two lines is fixed. Tests read the values with it.

**Two test-only arguments in Phase 1~2.** The table above is the product path input. Phase 1
confirms send and receive with "two known endpoints", without the control plane and without
STUN (`roadmap.md` Phase 1), so it needs a place to take the remote address.

| Input | Argument | Value |
|------|------|-----|
| Remote endpoint | `--peer <IPv4>:<port>` | Phase 1~2 test only. Without it the client only receives and does not send. What this argument sends is a test byte string with no tunnel header, different from `DATA` in Phase 4 onward |
| Raw send | Console command `raw <byte count>` | Sends 1 datagram of that length to the address given by `--peer`. The content is a byte string increasing by 1 from `0x00`, so the receiver can check it by computation. The length is 1 or more and `MAX_DATAGRAM` (1472) or less. Out of range, nothing is sent and a `WARN` is left |
| Counter dump | Console command `counters` | Emits every counter at once. Chapter 9 fixes three points at which counters are emitted, and this is one of them |
| Shutdown | Console command `quit` | Signals the shutdown event. 3.2.3 One Turn of the Loop owns that path |

**The receiving side emits one `rx.raw` log line.** The fields are `from` (source
`IPv4:port`), `len`, and `sha256` (the first 16 hex characters of the received byte string's
hash). The byte string itself is not put in the log. **The sending side emits the same line.**
If `len` and `sha256` match on both lines, the Phase 1 "matches byte for byte" holds.

**On the sending side `from` is its own local endpoint.** The source of that datagram is
itself. Since the bind is `INADDR_ANY`, the address part is `0.0.0.0` (`protocol.md` chapter 6).
The decision reads `len` and `sha256`; `from` is what lets a person tell the two lines apart.

> **Why.** A procedure that compares bytes by eye is not used, because it becomes a decision
> that differs per person.

**Both go away in Phase 6.** They have the same lifetime as the `[console]` scaffolding
(3.2.1).

**What is not a startup input.** The local record file path is decided together with the
chapter 9 contract when that is settled. The telemetry service address is Phase 9. That both
are absent from the table above is the current state.

**Behavior on argument errors.** An unknown argument, a missing required argument, or a format
violation all leave **one `ERROR` line and fail startup with exit code 2**. Nothing is guessed
and corrected.

- If the same argument is given twice, `--stun` accumulates into the list and the rest take the
  last value
- **An argument that is given is checked even when that span does not use it.** "Only stored" in
  the table above means the value is not used, not that it is not checked

> **Why.** Deferring the check means an input that passed in Phase 1 is refused only in Phase 3,
> and in between the record cannot tell which format the tests ran with.

**The "required" column of the table above decides what counts as missing.** Missing is the
absence of a value in a span that column requires. The role may be absent in Phase 1~2 and the
process still starts. But a value that is neither `host` nor `player` is a format violation in
every span.

**The source of each format differs per value.** The rules are not copied here.

| Value | Source |
|-------|--------|
| `--room` | `control_plane.md` 2.1 `room_id`, its normalization and checks |
| `peer_id` in `--rejoin` | `control_plane.md` 2.2 `peer_id`. Written in decimal |
| `peer_token` in `--rejoin` | `control_plane.md` 2.3 `peer_token` |
| The port in `--server` | If omitted, `CONTROL_PORT` in `control_plane.md` 2.6 Constants |
| Every port | 1~65535 |
| The address in `--peer` | An IPv4 literal only. A name is not accepted |
| The address in `--stun` | A name or an IPv4 literal. The port cannot be omitted |

**The tests own the counterexample list.** The case tables for IPv4 literals, the port range,
`--room`, and `--rejoin` live in `tests/`. Keeping them in the document means matching the same
table in two places, and counterexamples grow as the implementation grows.

---


## 4. Data Plane Path

The full path a game packet takes to reach the remote machine.

```text
Minecraft (TCP, 10.100.0.2 -> 10.100.0.1:25565)
   |
   v  Windows routing table sends 10.100.0.0/24 to the virtual adapter
Wintun Adapter (sending side)
   |
   v  wintun_adapter reads the original IP packet
Router: destination virtual IP -> peer session decision
   |
   v  tunnel attaches the DATA header
[TunnelHeader][original IP packet]
   |
   v  udp_socket sends over the direct P2P path
Internet (UDP, public endpoint -> public endpoint)
   |
   v  remote udp_socket receives
tunnel: header validation and removal
   |
   v  wintun_adapter injects the original IP packet
Wintun Adapter (receiving side)
   |
   v
Minecraft Server
```

Minecraft Java Edition uses TCP for gameplay, but the tunnel itself uses UDP. This is not a
problem because the original TCP packet is carried as payload. TCP retransmission and congestion
control remain the job of the game's TCP stack.

### MTU Handling

The tunnel header is added, so the available payload size shrinks.

```text
1500 (assumed path MTU)
 - 20 (outer IPv4 header)
 -  8 (outer UDP header)
 - 20 (TunnelHeader)
= 1452 bytes (maximum inner IP packet)
```

The default virtual adapter MTU is owned by [`protocol.md`](protocol.md) chapter 12. It is a value
with margin below the 1452 bytes above, and the basis of the margin and the DF bit handling are in
that chapter too. **The number is not copied here.** That value is subject to confirmation by
measurement (`roadmap.md` Phase 7), and a copy would make this document stale at confirmation
time.

However, the calculation above depends on the **assumption that the outer path MTU is 1500.**
Paths over PPPoE lines or through other tunnels are smaller, and in that case the outer UDP packet
is fragmented. Measurement on paths that break the assumption and the record of limits are also
defined by the same chapter.

---

## 5. Tunnel Protocol

### 5.1 Header

```text
 offset  size  field             description
   0      4   magic             0x53414E47 ("SANG")
   4      1   version           0x01
   5      1   type              PacketType
   6      2   payload_length    number of bytes after the header
   8      4   peer_id           sending peer identifier
  12      4   session_epoch     identifier of the sending peer's current session attempt
  16      4   sequence          per-direction monotonically increasing counter
  = 20 bytes, network byte order
```

`session_epoch` is **per session attempt**, not per process. Without this field, delayed
packets from a previous attempt are accepted into the new session after a restart or retry.

The receiver pins the remote epoch from the first valid packet; afterwards a `HELLO` with a
different epoch means the remote has started a new attempt, so it is handled as renegotiation
rather than a drop, and the other types are dropped. The draw method, the drop list, and the
renegotiation procedure are owned by [`protocol.md`](protocol.md) 4.3 `session_epoch`, 9.3
Epoch Rules, and 9.5 Renegotiation.

All 4-byte fields are placed at offsets that are multiples of 4, so no padding appears even
under MSVC default alignment. Still, the struct is not `memcpy`ed as is. Byte order conversion
is needed, and relying on alignment happening to match breaks silently. **Serialize field by
field** and fix the length as a constant.

> **Details are in `protocol.md`.** Constant values, payload layouts, the receive validation
> pipeline, candidate nomination, the full transition table, and confirmed timer values have
> that document as their only source. This section is a summary, and on conflict `protocol.md`
> wins.

### 5.2 Packet Types

| Type | Direction | Payload | Purpose |
|------|------|----------|------|
| `HELLO` `0x01` | Both | nonce(16) + virtual IP(4) | Hole punch attempt and handshake start |
| `HELLO_ACK` `0x02` | Both | echo nonce(16) + virtual IP(4) | Proves the remote actually received my packet |
| `KEEPALIVE` `0x03` | Both | None | NAT mapping maintenance (15-second period) |
| `DATA` `0x04` | Both | Inner IPv4 packet | Game traffic delivery |
| `PING` `0x05` | Both | ping_id(8) | RTT measurement. The timestamp is not carried on the wire |
| `PONG` `0x06` | Both | ping_id echo(8) | RTT measurement response |
| `CLOSE` `0x07` | Both | reason(1) | Normal shutdown notice. Without it the remote holds a dead session until the 50-second idle timeout |

### 5.3 Session State Machine

```text
IDLE
  | peer candidates received from the control plane
  v
PUNCHING  --- punch deadline --->  FAILED(HOLE_PUNCH_TIMEOUT)
  | one of got_ack or sent_ack is set (which one is not fixed)
  v
HANDSHAKING --- punch deadline --->  FAILED(PEER_HANDSHAKE_FAILED)
  | the remaining one is set
  v
CONNECTED
  | idle timeout (no packet received from the remote)
  v
FAILED(TUNNEL_DROPPED)
```

Success of UDP `sendto` does not guarantee delivery. Therefore both transitions are decided
**only by received packets.**

- `HANDSHAKING -> CONNECTED`: **two independent flags have to both be set.** `got_ack` (a
  `HELLO_ACK` echoing the nonce we sent was received; confirms the us -> remote path) and
  `sent_ack` (a `HELLO_ACK` was sent for the remote's valid `HELLO`; confirms the remote -> us
  path).
  - Which one is set first is not fixed. Assuming an order either misjudges a one-way path as
    connected or times out both sides in the normal order
- `CONNECTED -> FAILED`: decided not by keepalive send failure, but by receiving not a single
  valid packet from the remote for 50 seconds. Keepalive is at 15-second intervals and one is
  sent immediately on entering `CONNECTED`, so it tolerates 3 losses.
- The two transitions on the punch deadline use the same deadline, and the code differs **only
  by the number of flags set at that moment.** The basis of the decision is in chapter 8
  Failure Diagnosis.

**There is one backward transition not in the diagram.** When the remote starts a new attempt
(a `HELLO` with a different `session_epoch`), the session, even from `CONNECTED`, clears the
round-trip evidence and goes back to before `CONNECTED`. The procedure and the state decision
at that time are owned by [`protocol.md`](protocol.md) 9.5 Renegotiation. Implementing
transitions as one-way from this document alone misses that path.

The diagram above is the skeleton of the normal path. The transitions of the **three active
states** (`PUNCHING`, `HANDSHAKING`, `CONNECTED`) for every received packet are in
`protocol.md` 9.4 Transition Table, and receive handling in `IDLE` and the terminal states
(`FAILED`, `CLOSED`) is covered separately by 9.4.1. In particular, `HELLO_ACK` arriving first
in `PUNCHING` and answering `HELLO` again in `CONNECTED` are normal behavior, and missing them
times out both sides.

---

## 6. Control Plane Interface

### 6.1 Operations

There are four operations: `create_room`, `join_room`, `register_candidate`, `get_peers`.
Inputs, outputs, errors, and encoding are owned by [`control_plane.md`](control_plane.md)
chapter 4 Operations. **There is no `register_peer`.** Rejoin is one form of `join_room`.

**`report_connection` and `report_telemetry` are not here.** Both are operations of the
telemetry service and belong to 6.3 Telemetry Service Interface. The connection result is the
first row of the chapter 9 metrics table (`connection success/failure + failure stage`), so it
goes to the same place as the metrics.

### 6.2 Connection Establishment Sequence

```text
Host                 AWS (coordination / telemetry)     Player
 |                         |                          |
 |--- create_room -------->|                          |
 |<-- room_id, 10.100.0.1--|                          |
 |                         |<------- join_room -------|
 |                         |-- room_id, 10.100.0.2 -->|
 |                         |                          |
 |-- STUN Binding Req ->(public STUN server)<- STUN Binding Req --|
 |<- XOR-MAPPED-ADDRESS -                - XOR-MAPPED-ADDRESS ->|
 |                         |                          |
 |-- register_candidate -->|<-- register_candidate ---|
 |                         |                          |
 |--- get_peers ---------->|<-------- get_peers ------|
 |<-- peer endpoint -------|--- peer endpoint ------->|
 |                         |                          |
 |=========== simultaneous UDP HELLO (hole punching) ===|
 |=========== Direct P2P Tunnel established ==========|
 |                         |                          |
 |--- report_connection -->|<--- report_connection ---|
```

**Only `report_connection` on the last line has a different destination.** It goes to the
telemetry service, not the coordination server. AWS is drawn as one box because the two services
are on the same instance, not because they are the same service.

### 6.3 Telemetry Service Interface

| Operation | Input | Output |
|------|------|------|
| `report_connection` | room_id, peer_id, result, failure stage, establishment time | Acknowledgement |
| `report_telemetry` | room_id, peer_id, RTT/loss/jitter/throughput | Acknowledgement |

**The client does not retry either operation on failure.** But the two differ in periodicity.

- `report_telemetry` is a periodic upload, so on failure it moves on to the next period
- **`report_connection` happens once per connection attempt, so there is no next period to move
  on to.** That attempt's upload ends there, and the same content is already in the local
  record file (chapter 9)

The basis is [`spec.md`](spec.md) NFR-3 and the chapter 9 local record contract. What is used
for the decision is the local record, not this upload.

**Failures are not swallowed.** When an upload fails, `[telemetry]` increments the counter
`telemetry_upload_failed` and leaves one line in the log. This is a different counter from
dropping because the queue is full (`telemetry_queue_dropped`, 3.2.6 Telemetry Isolation).

> **Why.** The former is the remote not receiving; the latter is dropping before sending.
> Counting both under one name makes a telemetry service outage indistinguishable from client
> overload.

**This failure is not written to the local record file.** The chapter 9 local record is a
contract of one line per connection attempt, and an upload failure is not a connection
attempt.

---

## 7. Virtual Network Design

- Range: `10.100.0.0/24`
- `10.100.0.1`: the room creator (host). The game server runs here.
- `10.100.0.2` and up: participants. The control plane assigns them sequentially.
- A virtual IP is valid per room and is reclaimed when the room disappears. When a room
  disappears, and why slots are not reclaimed while it is alive, are owned by
  [`control_plane.md`](control_plane.md) 2.5 Virtual IP Pool and 5.1 Room.

Players enter `10.100.0.1:25565` in the Minecraft server address field. They need not know any
public IP or port.

Windows routing is handled by attaching a `10.100.0.0/24` route to the virtual adapter. The
client sets this route directly through the Windows IP Helper API when creating the adapter and
removes it on exit. Wintun does not configure addresses or routes.

---

## 8. Failure Diagnosis

Connection failures are recorded classified by stage, without exception. "Could not connect"
is not accepted as a record.

| Code | Stage | Decision basis |
|------|------|-----------|
| `STUN_DISCOVERY_FAILED` | Public endpoint discovery | No STUN response or parse failure |
| `CONTROL_PLANE_EXCHANGE_FAILED` | Candidate exchange | `get_peers` polling deadline exceeded, control plane error, or `peer_id` collision. The list of conditions is owned by [`protocol.md`](protocol.md) 9.6 Failure Transitions |
| `HOLE_PUNCH_TIMEOUT` | Hole punching | `got_ack` and `sent_ack` **both unset** at the punch deadline |
| `PEER_HANDSHAKE_FAILED` | Handshake | **Only one of the two set** at the punch deadline |
| `TUNNEL_DROPPED` | Maintenance | No remote packet received for the idle timeout after establishment |

**The last two codes are split by the number of flags set, not by packet type.** The meaning
of the two flags, and that which one is set first is not fixed, are in 5.3 Session State
Machine; the source of the decision rule is `protocol.md` 9.6 Failure Transitions.

> **Why.** Splitting by "was the remote `HELLO` received" records a session that hit the
> deadline with only `got_ack` set, on the normal path where `HELLO_ACK` arrives first, as
> `HOLE_PUNCH_TIMEOUT`. That session already proved our packet reached the remote, so hole
> punching did not fail. If the code is wrong, the Phase 9 failure stage distribution is wrong
> by that much.

Each failure is recorded with the NAT environment information that can be obtained (local
range, public endpoint, per-STUN-server mapping differences). `STUN_DISCOVERY_FAILED` is the
case where discovery itself failed, so there is no public endpoint. Do not require every field;
record only what exists.

**Do not assert a NAT type.** RFC 5389 Binding results and port changes alone cannot determine
NAT type or filtering behavior. The Phase 9 analysis classifies not by "NAT type" but by
observed mapping behavior (does the public port change with destination) and connection
success. If type determination becomes necessary, an RFC 5780-capable server or a controlled
multi-destination test is needed in addition, and that is outside the initial scope.

---

## 9. Telemetry and Records

| Metric | Collection point | Use |
|------|-----------|------|
| Connection success/failure + failure stage | At the end of session establishment | Reliability analysis |
| Connection establishment time | STUN start ~ `CONNECTED` | Perceived-latency analysis |
| RTT | Periodic `PING`/`PONG` measurement | Latency analysis |
| Packet loss rate | Count of gaps in `sequence` | Quality analysis |
| Jitter | Deviation between consecutive RTTs | Quality analysis |
| Keepalive local send errors | `[loop]` send path. The counter is `tx_err_send` ([`protocol.md`](protocol.md) chapter 6) | Stability analysis. Counts only local `sendto` errors and is not evidence of delivery failure. That is why disconnection is decided by idle timeout |
| Idle timeout occurrences | Receive path | Stability analysis |
| Direct connection hold time | Duration in `CONNECTED` | Stability analysis |
| Tunnel throughput | Sent/received byte counters | Performance analysis |
| Session duration | Whole game session | Minecraft stability |

The client accumulates into a local ring buffer first, and a dedicated thread periodically
sends to the **telemetry service**. Upload is fully separated from the data plane, so a
telemetry service outage does not affect the tunnel. The isolation structure is in 3.2.6
Telemetry Isolation.

### Local Record File

**Separate from upload, there is a local record file.** This is what [`spec.md`](spec.md) M-6
requires, not control plane storage. The contract has four items.

| Item | Contract |
|------|------|
| Content | Per connection attempt, the result (success or a failure code from `spec.md` FR-13) and RTT. **RTT is left empty for attempts where it could not be measured** |
| Timing | Twice. One line **when establishment ends** (success or failure), and one more line when the session ends |
| Method | Append-only. Earlier lines are not modified |
| Independence | Does not depend on the telemetry queue or the upload path. The record is written even when the control plane and the telemetry service are **both** absent |

**Some attempts have no RTT.** `PING` sending starts on entering `CONNECTED` (chapter 11), so
before that no `PING` has been sent, and therefore `pending_pings` has no entries. If a `PONG`
arrives in that span, the lookup fails per [`protocol.md`](protocol.md) 5.5 `PING` / `PONG` and
it is dropped as `drop_unmatched_pong`.

**That is, no RTT sample is produced before `CONNECTED`.** This does not contradict
`protocol.md` 9.4 Transition Table answering `PING` in that span and feeding `PONG` into the
matching procedure. So attempts that ended before then (`STUN_DISCOVERY_FAILED`,
`CONTROL_PLANE_EXCHANGE_FAILED`, `HOLE_PUNCH_TIMEOUT`, `PEER_HANDSHAKE_FAILED`) have no value.

**What is absent is not written as 0.** Write it empty, and decide the notation for empty
together with the line format.

> **Why.** 0ms looks like a measured value and pollutes the Phase 9 aggregation.

A `TUNNEL_DROPPED` line carries the last RTT sample before the disconnection, and **if there is
not a single sample, that line is empty too.** Being cut right after `CONNECTED`, before the
first `PONG`, is that case. There is one rule. If absent, leave it empty. No exceptions.

**Why two lines.** `TUNNEL_DROPPED` is a code that appears **after** establishment has ended
(chapter 8). With a single timing at the end of establishment, there is no place to write that
line, and rewriting an already written line breaks the append-only contract above.

**RTT needs two lines for the same reason.** `PING` sending starts on entering `CONNECTED`
(chapter 11), so **at the moment establishment ends there is no RTT sample yet.** The RTT of
the first line is therefore usually empty. The second line is written when the session ends, so
that is where the value goes.

> **Why.** The establishment line is not delayed until the first sample, because if the process
> dies in between, no record is left at all.

- **Establishment line**: written for every attempt. The result code (success or failure). RTT
  is written if a sample exists at that moment, empty otherwise
- **End line**: **written only for attempts that reached `CONNECTED`.** The end code and the
  last RTT sample, empty if none. The end code is `CLOSED` or one of the failure codes in
  `spec.md` FR-13
  - `TUNNEL_DROPPED` is the common case. If renegotiation goes back to before `CONNECTED` and
    then the punch deadline is hit, `HOLE_PUNCH_TIMEOUT` or `PEER_HANDSHAKE_FAILED` comes to
    the end line (`protocol.md` 9.5 Renegotiation, 9.6 Failure Transitions). Pinning the code
    set to two values leaves nothing to write in that case

**An attempt that failed establishment has one line.** `STUN_DISCOVERY_FAILED`,
`CONTROL_PLANE_EXCHANGE_FAILED`, `HOLE_PUNCH_TIMEOUT`, and `PEER_HANDSHAKE_FAILED` never
reached `CONNECTED`, so there is no session to end. The establishment line is that attempt's
last line. **Whether it got there can be read from the line count.** One line means it ended
before establishment; two lines mean it connected and then ended.

How the two lines are tied to one attempt is decided together with the line format.

**The path and line format are not yet decided.** They are decided in this document. Not
arbitrarily in code. When they have to be decided is owned by [`roadmap.md`](roadmap.md).


### Log Output

The Phase 2~3 verification items use "`STUN_DISCOVERY_FAILED` recorded" and "confirm in the log"
as their means of decision ([`roadmap.md`](roadmap.md)). Without what is emitted, where, and in
what shape, whether that verification passed cannot be decided. **The "record" those items refer
to is this log.** The local record file is a Phase 4 deliverable and does not exist yet in Phase
2~3. The minimum contract has four items.

| Item | Contract |
|------|------|
| Destination | Standard error (stderr). It is not trapped in buffering, so the line just before a crash survives, and it can be redirected separately from standard output |
| Levels | Three: `INFO`, `WARN`, `ERROR`. No finer split |
| Unit | One event per line. **A value carries no space and no control character.** Both become a single underscore. No escape notation and no quoting |
| Failure notation | The chapter 8 failure code strings **as is**. Not translated or reworded |

The reason for three levels is that the only axes used for decisions are "normal progress /
abnormal but can continue / that attempt failed". A finer split makes the boundaries differ per
implementation, while the verification items still look for a single string.

> **Why an underscore.** Replacing with a space removes the `name=value` boundary. A field
> carries a user-supplied string as is, so that actually happens. Quoting restores the boundary
> but then a value containing a quote needs escaping, and the reader has to interpret it.

**The replacement applies to values only and cannot be undone.** A decision that needs the
original text is not made from the log. The log is a diagnostic for people to read, and the
input to a decision is the local record file.

**The guarantee is at byte level.** What changes is the ASCII space (`0x20`) and the control
bytes (`0x00`~`0x1F`, `0x7F`) only. `0x80` and above go in as they are. Those bytes do not break
the line and do not blur the `name=value` boundary.

> **Limit.** A Unicode line separator (such as U+2028) is not changed. A tool that splits the log
> on Unicode line breaks is not used for decisions. Decisions split on `0x0A`.

**Events that verification looks for get fixed keys.** The contract above alone does not settle
what a "confirm in the log" verification item should look for. A line has the shape
`<level> <event key> <name>=<value> ...`, and the event keys and fields used by verification are fixed
below. Value formats are not fixed. Verification searches by key and field name.

| Event key | Required fields | Which verification uses it |
|----------|-----------|--------------------|
| `socket.bind` | `local` (the actual bound endpoint), `rcvbuf_requested`, `rcvbuf_applied` | Phase 1 bind contract, applied `SO_RCVBUF` value, [`spec.md`](spec.md) M-1 startup decision |
| `stun.result` | `server`, `mapped` (the observed public endpoint) | Phase 2, M-2 |
| `session.state` | `from`, `to` (the state name strings from [`protocol.md`](protocol.md) 9.1 States as is) | Phase 4 transition check |
| `session.failed` | `code` (chapter 8 string) | Phase 2~5 failure code check |
| `counter` | `name`, `value` | Every item that looks at drop counters |
| `socket.error` | `op` (call name), `code` (Winsock error code) | Phase 1: the process does not die on socket errors |
| `control.result` | `op`, `ok`, `error` (failure code string, `-` on success) | Phase 3 operation verification, `CONTROL_PLANE_EXCHANGE_FAILED` cause check. **Does not carry `room_id` or `peer_token`** (3.5) |
| `control.peers` | `peer_id`, `virtual_ip`, `candidates` (remote candidates as `ip:port` joined by commas) | `spec.md` M-3, Phase 3 `get_peers` verification |
| `timer.tick` | `name`, `elapsed_ms` (milliseconds actually elapsed since the previous expiry) | Phase 1 drain budget, Phase 3 `[loop]` survival while the control plane is dead |
| `rx.raw` | `from`, `len`, `sha256` | Phase 1 raw send/receive comparison (3.5) |

Events not listed here may be added freely. **Only these ten keys and their field names do not
change.** Changing them makes the verification items stale with them.

**`timer.tick` is emitted by a timer that exists only in the test build.** It is not in the
product build. The test build puts one 200ms periodic timer (`name=probe200`) into the `[loop]`
timer set and emits this line on every expiry. **`elapsed_ms` is there so that the interval can
be decided even though the log line carries no timestamp.** That is the only value verification
looks at, so the timestamp format of the line itself does not have to be fixed. It is turned on
by the same CMake option as the echo responder (`roadmap.md` Phase 4), and a build with it on
leaves one `WARN` line at startup.

**Fields whose values verification compares also have a fixed format.** Without a format, the verifier ends up deciding which of `256KB`, `262144`, and `0x40000` to compare.

| Field | Format |
|------|------|
| `rcvbuf_requested`, `rcvbuf_applied` | Decimal byte count. No unit suffix |
| `local`, `mapped` | `IPv4:port` in decimal notation |
| `virtual_ip` | Dotted-decimal IPv4 |
| `candidates` | `IPv4:port` joined by commas. No spaces |
| `code` | A chapter 8 failure code string, or in `socket.error` a decimal error code |
| `error` (`control.result`) | An error code string from [`control_plane.md`](control_plane.md) 4.1 Common Envelope, `transport` for a transport error, `-` on success |
| `value` (`counter`) | Decimal integer |

The value formats of the remaining fields are not fixed. They are human-read diagnostics.

**When the `counter` event is emitted is also fixed.** Without a timing, a verification item that looks at
counters cannot know what to wait for. There are three moments.

- When a session reaches a terminal state (`FAILED`, `CLOSED`), all counters of that session
- In the process shutdown procedure (3.2.7), all of them
- Immediately, when requested by a `[console]` command, all of them

**Not on every increment.** Drop counters can rise thousands of times per second under attack traffic,
and writing a log line each time makes the log a load on the data plane. Counters with value 0 are emitted too. Distinguishing
absent from zero is what lets verification decide "it did not rise".

**The log is a different thing from the local record file above.** The log is a human-read
diagnostic, so lines may be added and wording may change; the record file is the input to the
`spec.md` M-6 decision and keeps the contract above.

**The M-6 decision is not read from the log, and diagnostics are not added to the record
file.** Mixing the two makes the M-6 decision shake every time a diagnostic wording is fixed.

**Phase 1~3 verification reading the fixed event keys above is a different matter.** The record
file does not exist yet in those Phases, which is why the keys and field names are fixed.

---

## 10. Repository Structure

### Target Structure

```text
Sangtachi/
+-- client/
|   +-- include/
|   +-- src/
|   |   +-- main.cpp
|   |   +-- args.cpp     startup argument parsing (3.5)
|   |   +-- network/     wsa, udp_socket, endpoint, stun_client
|   |   +-- peer/        peer, hole_punch, session
|   |   +-- tunnel/      packet, tunnel, router
|   |   +-- adapter/     wintun_adapter
|   |   +-- telemetry/   telemetry
|   |   +-- control/     control_client
|   +-- CMakeLists.txt
+-- control-server/      module responsibilities are in control_plane.md 7.1
|   +-- server.py
|   +-- ops.py
|   +-- ids.py
|   +-- candidates.py
|   +-- clock.py
|   +-- store.py
|   +-- tests/
+-- telemetry-server/
|   +-- server.py
|   +-- ingest.py
+-- tests/
+-- scripts/
+-- docs/
|   +-- kor/                 Korean documents
|   |   +-- architecture.md      this document
|   |   +-- spec.md              requirements and success criteria
|   |   +-- roadmap.md           phased development plan
|   |   +-- plan.md              handover for the next session
|   |   +-- protocol.md          confirmed tunnel protocol
|   |   +-- control_plane.md     confirmed control plane
|   |   +-- experiments.md       experiment design and measurement results (written in Phase 9)
|   |   +-- audit-history/       archive of full design audit records
|   |   +-- decisions/           design decision records. ADRs and first_design.md
|   |   +-- commit_history/      per-commit change records
|   +-- eng/                 English documents, same structure (this tree)
+-- README.md
```

The two language trees hold the same files. When editing a document, modify both sides in the same commit.

### Difference from the Current State

The current source tree is only `src/main.cpp` and `CMakeLists.txt` at the root (`docs/` is
already populated). The relocation under `client/` and the creation of `control-server/` and
`telemetry-server/` remain.

---

## 11. External Dependencies

| Dependency | Scope provided | Approval status |
|--------|-----------|-----------|
| Wintun | Windows virtual network interface access only. Adapter creation, packet read/inject | **Approved** |
| Windows IP Helper / NetIO API | IP address and route configuration of the virtual adapter. **Wintun does not provide this** | OS built-in |
| Winsock2 | Windows standard socket API | OS built-in |
| Public STUN servers | Binding Response. The server is not implemented, only used | External public service |
| Python standard library | `asyncio`, `json` | Standard library |
| AWS SDK for Python (`boto3`) | DynamoDB access for the control server and the telemetry service | **Approved** |
| Catch2 v3 | Test case registration, execution, failure reporting. Linked only into the test executable | **Approved.** [`spec.md`](spec.md) NFR-5 owns who approves |
| Amazon DynamoDB | Persistent storage of room/peer state and metrics | External managed service |

Be clear about what Wintun does **not** provide. Peer discovery, STUN, NAT traversal, hole
punching, the tunnel protocol, routing decisions, session management, monitoring, and diagnostics
are all implemented directly by this project. Wintun is only a means of access to a kernel-mode
virtual NIC driver.

---

## 12. Out of Initial Scope

The following are deliberately left out of the initial architecture. When they become necessary, leave a decision record in [`decisions/`](decisions/) and adopt them.

- **Encryption and peer authentication**: tunnel payload is plaintext. Stretch goal.
- **Relay fallback (TURN-like)**: no alternative path when direct connection fails. Stretch goal.
- **Full ICE**: initially only the minimum connection establishment procedure. Phase 9 does only a comparative analysis against ICE concepts.
- **Mesh of 3 or more**: initially 2 peers. The routing table structure is left extensible but not implemented.
- **GUI**: console only.
- **macOS / mobile**: not supported. Linux only gets an interoperability review if time remains.
