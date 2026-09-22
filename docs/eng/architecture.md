# Architecture

**Project:** Direct-First P2P Virtual Network for Multiplayer Games
**Reference document:** [`first_design.md`](first_design.md)
**Target platform:** Windows 10 / 11 x64 (client), Linux / AWS EC2 (control server)

> Korean version: [`../kor/architecture.md`](../kor/architecture.md)

---

## 1. Design Principles

| # | Principle | Meaning |
|---|-----------|---------|
| 1 | Direct-first | Game traffic flows directly between peers whenever possible. AWS is for coordination only and is never on the data path. |
| 2 | Plane separation | The control plane (Python/AWS) and the data plane (C++/client) run and are tested independently. |
| 3 | Game-agnostic | No Minecraft-specific logic goes into the tunnel layer. Minecraft is only the validation target. |
| 4 | Observable | Every connection attempt records success/failure and the **stage at which it failed**. |
| 5 | Minimal dependencies | Wintun provides virtual interface access only. STUN, NAT traversal, tunneling, and routing are implemented directly. |
| 6 | Risk first | Validate the most uncertain thing (NAT traversal) first. The virtual adapter comes after. |

---

## 2. System Overview

```text
                        AWS Control Plane (EC2)
                    +---------------------------+
                    | Python Coordination Server|
                    |  rooms / peers            |
                    |  candidate exchange       |
                    |  telemetry ingest         |
                    |  SQLite                   |
                    +-------------+-------------+
                                  |
                     coordination traffic only (TCP/JSON)
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

If the control plane goes down, **already established P2P tunnels must keep working**. The control plane is needed only for session establishment and telemetry collection.

---

## 3. Components

### 3.1 Client (C++20)

| Module | Responsibility | External dependency |
|--------|----------------|---------------------|
| `network/udp_socket` | Winsock2 init/teardown, UDP socket creation, non-blocking send/receive, event handle exposure (3.2) | Winsock2 |
| `network/endpoint` | `IP:Port` value type, parsing, comparison, serialization | none |
| `network/stun_client` | STUN Binding Request construction, transaction ID management, response parsing, `XOR-MAPPED-ADDRESS` decoding, timeout/retry | none (RFC 5389 implemented directly) |
| `peer/peer` | Peer identifier, virtual IP, candidate endpoint list | none |
| `peer/hole_punch` | Simultaneous bidirectional send, retries, success determination, failure classification | none |
| `peer/session` | Handshake, keepalive, connection state machine, RTT measurement | none |
| `tunnel/packet` | Tunnel header serialization/deserialization, malformed packet validation | none |
| `tunnel/tunnel` | Per-type packet dispatch, encapsulation/decapsulation, sequence management | none |
| `tunnel/router` | Virtual IP -> peer session mapping, destination resolution | none |
| `adapter/wintun_adapter` | Virtual adapter create/open and packet read/inject (Wintun); virtual IP address and route configuration (IP Helper) | **Wintun (approved)**, IP Helper |
| `telemetry/telemetry` | Metric collection, local buffering, upload to control plane | none |
| `control/control_client` | Control plane REST/JSON calls | Winsock2 |

The client is a single process. The thread layout and state ownership are in 3.2.

**All UDP sends and receives share one local socket, and that socket is owned exclusively by a single receive loop.** Using different sockets makes the NAT map a different public port for each, so the endpoint discovered through STUN does not apply to the tunnel. This constraint must hold from Phase 2 onward.

Sharing the socket means STUN responses and tunnel packets arrive in the same queue. If the STUN client calls `recvfrom` itself, the two readers steal each other's packets. The STUN client only registers a request and receives the response from the receive loop. The classification rule and the required socket options (`SO_EXCLUSIVEADDRUSE`, `SIO_UDP_CONNRESET` off, never calling `connect()`) are in sections 6 and 7 of [`protocol.md`](protocol.md).

### 3.2 Concurrency Model

**All code that touches tunnel state runs on a single thread.** Rather than sharing state under a lock, we do not share it.

The initial layout was four threads: `[main]`, `[net_rx]`, `[tun_rx]`, `[timer]`. It was discarded for two reasons. First, four threads mutate session state and the outbound `sequence` with no synchronization. Second, telemetry upload shares the timer thread, so a control-plane outage stops keepalives. The second violates NFR-3 at design time.

#### 3.2.1 Threads

| Thread | Responsibility | Shared state |
|--------|----------------|--------------|
| `[loop]` | UDP receive, Wintun receive, console commands, timers, session state, routing, all sends | **No tunnel state.** It shares only the telemetry queue and the shutdown/cleanup events |
| `[telemetry]` | Drains the metrics queue, uploads to the control plane | One queue |
| `[console]` | Reads standard input. Phase 1-5 scaffolding | One command queue |

Phase 6 onward uses two threads; Phases 1-5 use three. `[loop]` is the process main thread. `[console]` exists only in Phases 1-5: `[loop]` cannot block on standard input, so a separate thread reads it and pushes into a queue. When the adapter arrives in Phase 6 the Wintun read event joins `[loop]`'s wait set directly, so no thread is added.

#### 3.2.2 Waiting

```text
WaitForMultipleObjects(n, handles, FALSE, timeout_ms)
```

| Rank | Handle | Created by | Valid during |
|------|--------|------------|--------------|
| 0 | Shutdown event | `CreateEvent` (manual reset) | Always |
| 1 | UDP socket event | `WSACreateEvent` + `WSAEventSelect(sock, ev, FD_READ)` | Always |
| 2 | Wintun read event | `WintunGetReadWaitEvent(session)` | Phase 6 onward |
| 3 | Console command event | `CreateEvent` (auto reset), signaled by `[console]` | Phases 1-5 |

**These numbers are a logical rank, not array indices.** `WaitForMultipleObjects` requires an array of valid handles with no gaps; a `NULL` in an empty slot produces `WAIT_FAILED`. Whenever the set changes, build a compact array of the live handles only and keep a separate map from logical source to actual index. In Phases 1-5 there is no Wintun event, so the array length is 3.

`timeout_ms` is computed as below. **Do not subtract first.** `GetTickCount64()` is unsigned, so if the deadline has already passed the subtraction underflows into a huge value, and narrowing it to `DWORD` can land on `0xFFFFFFFF`, which is `INFINITE`. The loop would then never wake again.

```text
next_timeout:
    if no timer pending:  return INFINITE
    t = now()                            # read once
    d = next_deadline()
    if d <= t:            return 0
    return (DWORD) min(d - t, INFINITE - 1)
```

Do not call `now()` twice. If the clock passes `d` between the comparison and the subtraction, the subtraction underflows, the timeout becomes about 49.7 days, and the loop effectively stops.

`WSAPoll` is not used because the Wintun read wait is a Win32 event handle, not a socket. Mixing a socket and an event handle into one wait requires `WaitForMultipleObjects`. `WSAEventSelect` puts the socket into non-blocking mode automatically, so `ioctlsocket(FIONBIO)` is not called separately.

#### 3.2.3 One iteration

```text
busy = false
loop:
    r = WaitForMultipleObjects(n, handles, FALSE, busy ? 0 : next_timeout())

    if r == WAIT_FAILED:                      # unrecoverable
        log, shutdown(); break                # same cleanup as the normal path
    if r == WAIT_OBJECT_0 + idx(SHUTDOWN):
        shutdown(); break

    WSAEnumNetworkEvents(sock, udp_ev, &ne)   # resets the socket event. required
    u = drain_udp(MAX_DRAIN)                  # do not branch on r
    w = drain_wintun(MAX_DRAIN)
    if w == RESTART:
        recreate the adapter session -> on failure shutdown(); break
        on success rebuild the array and index map from the live handles
    drain_console()
    run_expired_timers(now())
    busy = (u == BUDGET) or (w == BUDGET)
```

`WAIT_FAILED` goes through `shutdown()` too. Exiting directly there skips the `CLOSE` send and the adapter cleanup, so the peer holds a dead session for 50 seconds and the adapter and routes are left behind. **An abnormal exit is exactly the case that needs cleanup.**

**`WSAEnumNetworkEvents` must be called.** The event `WSAEventSelect` creates is manual-reset, and `recvfrom` only re-arms the `FD_READ` notification; it does not reset an already-signaled event object. Omit this call and the event stays signaled after the first datagram, `WaitForMultipleObjects` returns immediately every time, and the loop spins burning CPU. This one call handles both the event reset and the network event lookup.

**Do not branch on `r`.** With `bWaitAll = FALSE`, `WaitForMultipleObjects` returns **only the lowest signaled index**. Branching on the return value starves the Wintun event for as long as the UDP event, which ranks ahead of it, keeps signaling. That is exactly what happens under heavy game traffic. Draining both every iteration makes index order irrelevant, and the cost when both are empty is one `WSAEWOULDBLOCK` and one `ERROR_NO_MORE_ITEMS`.

**Both drains carry a budget.** Without one, draining until empty means `drain_udp` never returns whenever arrival outpaces processing, and the timers never run at all. Keepalives and `HELLO` retransmits stop, and the "receives first, timers last" ordering established just above turns into "timers never run". `MAX_DRAIN` is 64 per source per iteration. At 1472 bytes that is 94 KB, and parsing plus sending costs under 1 ms, which is negligible against the shortest timer (the 200 ms `HELLO` retransmit). If a drain stops on its budget, `busy` becomes true and the next wait returns immediately with a zero timeout. The remaining data is handled on the next iteration, and **a timer pass happens in between.**

```text
drain_udp(budget) -> {EMPTY, BUDGET}:
    repeat budget times:
        n = recvfrom(sock, buf, MAX_DATAGRAM, &src)
        if n < 0:
            e = WSAGetLastError()
            if e == WSAEWOULDBLOCK:  return EMPTY      # source is empty
            count and return EMPTY                     # the socket stays in use
        classify(buf, n, src)                          # protocol.md section 7
    return BUDGET                                      # budget spent, more remains
```

`FD_READ` is level-like. If data remains after a `recvfrom`, the event is signaled again, so reading one datagram per iteration does not lose datagrams. **Draining is about cost, not correctness.** A wait plus a `WSAEnumNetworkEvents` round trip per datagram scales wakeups with the received packet count and pushes timer handling back by the same amount. The correctness requirement is the `WSAEnumNetworkEvents` call above; draining is a batching policy layered on top of it.

```text
drain_wintun(budget) -> {EMPTY, BUDGET, RESTART}:
    if session == NULL:  return EMPTY                  # Phases 1-5, no adapter
    repeat budget times:
        p = WintunReceivePacket(session, &size)
        if p == NULL:
            e = GetLastError()
            ERROR_NO_MORE_ITEMS -> return EMPTY         # ring is empty, normal
            ERROR_HANDLE_EOF    -> signal shutdown event and return EMPTY
            ERROR_INVALID_DATA  -> return RESTART       # ring corrupt, see below
            otherwise           -> count and return EMPTY
        guard = release WintunReleaseReceivePacket(session, p) on scope exit
        route_and_send(p, size)
    return BUDGET
```

`WintunReleaseReceivePacket` goes in **a scope guard.** Placing the call after `route_and_send` leaks the ring packet whenever routing returns early or throws. Every miss permanently shrinks the ring buffer by that much, and receive eventually stops.

`RESTART` is handled by `[loop]`. A corrupt ring requires a new session, and **a new session hands out a new read event handle.** Session recreation and wait-array rebuild are therefore one unit. Restarting inside `drain_wintun` would leave the caller waiting on a handle it just invalidated. If recreation fails, go to `shutdown()` rather than continue without an adapter. The array is rebuilt compactly, so the Wintun handle simply drops out and the wait keeps working normally. That is what makes it dangerous: **the tunnel is dead but pretends to be alive.** The session stays `CONNECTED` and keepalives keep flowing while not a single game packet moves.

`drain_console` drains the command lines `[console]` pushed into the queue. The event is auto-reset, so no separate reset call is needed. Both the handle and the thread disappear from Phase 6 onward.

The Wintun documentation suggests spinning on `ERROR_NO_MORE_ITEMS` for a while under heavy load before waiting on the event. **We do not spin.** This loop also owns the timers, so spinning delays keepalives and `HELLO` retransmits by that much. Spinning is a throughput optimization and our target is latency (NFR-1). Revisit if Phase 7 measurements show receive as the bottleneck.

#### 3.2.4 Timers

- The monotonic clock is `GetTickCount64()`: milliseconds, 64-bit, so no wraparound in practice (about 584 million years). Only RTT measurement uses `QueryPerformanceCounter`, and **values from the two clocks are never compared against each other.**
- The timer set is small: roughly six per session, and the minimum scope has one session. A linear scan each iteration finds the earliest deadline. No timer wheel is needed.
- Section 11 of [`protocol.md`](protocol.md) requires strict inequality on deadline comparisons and defines priority by **dequeue time**: a receive event dequeued in a given iteration is handled ahead of that iteration's timer expiry. **Draining receives first and running timers last is the implementation of that rule.** Why dequeue time rather than arrival time is the drain budget, and the reasoning is in protocol.md section 11.

#### 3.2.5 State ownership

All of the following is solely owned by `[loop]`. No mutex, no atomic. None is needed.

The session state machine, `session_epoch`, the outbound `sequence`, the replay bitmap, `peer_endpoint`, the last-receive timestamp, the candidate list, `pending_pings`, and every drop counter.

**Incrementing the outbound `sequence` must happen only on `[loop]`.** `DATA` and `KEEPALIVE` share the counter, so incrementing it from two threads sends two packets carrying the same number. The receiver's duplicate suppression ([`protocol.md`](protocol.md) 4.5) correctly discards the second one. It is a forged duplicate of our own making.

No struct gets a "thread-safe" comment. Single ownership is the only rule, and the rule disappears the moment one exception is allowed.

#### 3.2.6 Telemetry isolation

`[telemetry]` is a separate thread for **isolation**, not parallelism.

- `[loop]` pushes records into a fixed-size SPSC ring buffer. With exactly one producer and one consumer it is implemented **lock-free** with atomic head and tail. When it is full, **the new record is dropped** and a loss counter is incremented. No waiting, no failure reported.
- Dropping the oldest would be better for metric quality, but that requires the producer to move the consumer's tail, which requires a lock. **We do not put a lock on `[loop]` to save one more metric.** A mutex would rarely block given how short the critical section is, but "rarely" is not a guarantee: if the consumer is descheduled holding the lock, `[loop]` stalls for that long.
- **There is no lock anywhere in the data plane.**
- `[telemetry]` drains the queue and uploads to the control plane. Whether the upload blocks or fails is irrelevant to `[loop]`.
- Telemetry uses a control-plane TCP socket. The single-socket constraint in section 6 of [`protocol.md`](protocol.md) applies **to the UDP data plane only**.

The Phase 5 "control server down for 10 minutes" test (NFR-3) cannot pass without this isolation. Under the four-thread layout, a TCP upload to an unresponsive control plane blocks the timer thread for tens of seconds; no keepalive goes out during that window and the NAT mapping expires. A control-plane outage takes the tunnel down with it.

#### 3.2.7 Shutdown

- `[telemetry]` wakes once a second via `WaitForSingleObject(shutdown_ev, 1000)` and drains the queue. No event is signaled per push. The shutdown event is manual-reset, so signaling it wakes this wait at the same time.
- `[telemetry]` sets `SO_SNDTIMEO` and `SO_RCVTIMEO` to 1.5 seconds on the upload socket and checks the shutdown event between uploads. At 3 seconds a single in-flight upload could outlast the 2-second join bound below.
- `SetConsoleCtrlHandler` signals the shutdown event. **The handler never touches `[loop]`-owned state**, because it runs on another thread's context.
- **When the handler returns depends on the control signal.** For `CTRL_C_EVENT` and `CTRL_BREAK_EVENT` it may signal the event and return `TRUE` immediately. But for `CTRL_CLOSE_EVENT`, `CTRL_LOGOFF_EVENT`, and `CTRL_SHUTDOWN_EVENT`, **Windows terminates the process the moment the handler returns.** Signaling and returning would skip both the `CLOSE` send and adapter cleanup. In those three cases the handler signals the shutdown event, then waits on a cleanup-complete event within the OS grace period (3 seconds) before returning. Closing the console window is a normal way to stop this program, so this is the main path, not an edge case.
- `[loop]`'s `shutdown()` performs, in order: (1) signal the shutdown event if it is not already signaled, (2) one `CLOSE` per session ([`protocol.md`](protocol.md) 5.6), (3) close the adapter session, (4) clean up the adapter, address, and routes, (5) **signal the cleanup-complete event**, (6) join `[telemetry]`.
- **No sentinel is pushed into the queue.** A full queue drops new entries, so a sentinel dropped at exactly that moment would leave `[telemetry]` never seeing the shutdown. Shutdown travels only on the out-of-band event.
- **Cleanup comes before the join.** That ordering lets the console handler return without waiting on telemetry, and if the OS kills the process at that point all that is lost is a few metric records.
- The `[telemetry]` join has a 2-second bound. If it is exceeded, the remaining records are abandoned and the process exits immediately via `_exit`, skipping the join. This is safe because cleanup already finished at step (5). **What actually enforces the bound is this `_exit`, not the socket timeouts.** The socket timeouts only make the common case exit cleanly; `_exit` is what keeps shutdown from ever being held hostage to control-plane availability.
- Cleanup of the adapter, virtual IP address, and routes, along with leftovers from an abnormal exit, is in section 3 of [`windows-prereq.md`](windows-prereq.md).

---

### 3.3 Control Plane (Python)

| Module | Responsibility |
|--------|----------------|
| `server.py` | HTTP/JSON endpoints, request dispatch |
| `room.py` | Room creation/joining, virtual IP pool management, room state |
| `peer.py` | Peer registration, candidate endpoint storage, peer list queries |
| `telemetry.py` | Connection result and performance metric intake, SQLite storage |

Standard library only (`asyncio`, `json`, `sqlite3`). A web framework is introduced only after the need is demonstrated and approved.

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
Router: destination virtual IP -> peer session
   |
   v  tunnel prepends the DATA header
[TunnelHeader][original IP packet]
   |
   v  udp_socket sends over the direct P2P path
Internet (UDP, public endpoint -> public endpoint)
   |
   v  remote udp_socket receives
tunnel: validate and strip the header
   |
   v  wintun_adapter injects the original IP packet
Wintun Adapter (receiving side)
   |
   v
Minecraft Server
```

Minecraft Java Edition uses TCP for gameplay, but the tunnel itself uses UDP. This is fine because the original TCP packets ride inside as payload. TCP retransmission and congestion control remain the job of the game's own TCP stack.

### MTU Handling

The tunnel header reduces the available payload size.

```text
1500 (assumed path MTU)
 - 20 (outer IPv4 header)
 -  8 (outer UDP header)
 - 20 (TunnelHeader)
= 1452 bytes (maximum inner IP packet)
```

Set the virtual adapter MTU down to around 1400 to avoid fragmentation.

Note that the calculation above depends on the **assumption that the outer path MTU is 1500**. Paths over PPPoE lines or through another tunnel are smaller, and in that case the outer UDP packet fragments. The exact value is fixed by measurement; if a conservative fixed value is used, state that limitation in [`experiments.md`](experiments.md).

---

## 5. Tunnel Protocol

### 5.1 Header

```text
 offset  size  field             description
   0      4   magic             0x53414E47 ("SANG")
   4      1   version           0x01
   5      1   type              PacketType
   6      2   payload_length    bytes following the header
   8      4   peer_id           sending peer identifier
  12      4   session_epoch     sender's process instance
  16      4   sequence          per-direction monotonic counter
  = 20 bytes, network byte order
```

Without `session_epoch`, delayed packets from a previous instance are accepted into the new session after a restart. The receiver pins the peer's epoch during the handshake and drops packets carrying a different one afterwards.

Field order places every 4-byte field on a multiple-of-4 offset, so no padding appears even under MSVC default alignment. Even so, never `memcpy` the struct directly: byte order conversion is still required, and relying on the alignment happening to work breaks silently. **Serialize field by field** and fix the header length as a constant.

> **The detail lives in [`protocol.md`](protocol.md).** Constant values, payload layouts, the receive validation pipeline, candidate nomination, the full transition table, and the fixed timer values have that document as their single source. This section is a summary; where they conflict, `protocol.md` wins.

### 5.2 Packet Types

| Type | Direction | Payload | Purpose |
|------|-----------|---------|---------|
| `HELLO` `0x01` | both | nonce(16) + virtual IP(4) | Hole punching attempt and handshake initiation |
| `HELLO_ACK` `0x02` | both | echo nonce(16) + virtual IP(4) | Proves the peer actually received my packet |
| `KEEPALIVE` `0x03` | both | none | Maintains the NAT mapping (15 s interval) |
| `DATA` `0x04` | both | inner IPv4 packet | Carries game traffic |
| `PING` `0x05` | both | ping_id(8) | RTT measurement. No timestamp goes on the wire |
| `PONG` `0x06` | both | echoed ping_id(8) | RTT measurement reply |
| `CLOSE` `0x07` | both | reason(1) | Graceful shutdown notice. Without it the peer holds a dead session until the 50 s idle timeout |

### 5.3 Session State Machine

```text
IDLE
  | peer candidates received from control plane
  v
PUNCHING  --- timeout --->  FAILED(HOLE_PUNCH_TIMEOUT)
  | peer HELLO received (own HELLO still being retransmitted)
  v
HANDSHAKING --- timeout --->  FAILED(PEER_HANDSHAKE_FAILED)
  | HELLO_ACK carrying own nonce received
  v
CONNECTED
  | idle timeout (no packets from peer)
  v
FAILED(TUNNEL_DROPPED)
```

A successful UDP `sendto` does not guarantee delivery. Both transitions are therefore decided **only by packets received**.

- `HANDSHAKING -> CONNECTED`: **two independent flags must both be set.** `got_ack` (a `HELLO_ACK` echoing a nonce we sent, confirming our -> peer path) and `sent_ack` (we sent a `HELLO_ACK` for a valid peer `HELLO`, confirming peer -> our path). Which one is set first is not determined. Assuming an order either misreads a one-directional opening as connected, or times both sides out on a normal ordering.
- `CONNECTED -> FAILED`: decided not by keepalive send failure, but by receiving no valid packet from the peer for 50 s. Keepalive runs at 15 s with one sent immediately on entering `CONNECTED`, so three losses are tolerated.

Transitions for every state crossed with every received packet are in the 9.4 transition table of [`protocol.md`](protocol.md). In particular, `HELLO_ACK` arriving first in `PUNCHING` and re-answering `HELLO` in `CONNECTED` are both normal behavior; omitting either times both sides out.

---

## 6. Control Plane Interface

### 6.1 Operations

| Operation | Input | Output |
|-----------|-------|--------|
| `create_room` | host identifier | room_id, assigned virtual IP |
| `join_room` | room_id, peer identifier | assigned virtual IP, room info |
| `register_peer` | room_id, peer_id | registration ack. Performed internally by `create_room` / `join_room`, so clients do not call it separately (used only on reconnect) |
| `register_candidate` | room_id, peer_id, local/public endpoints | registration ack |
| `get_peers` | room_id, peer_id | other peers' virtual IPs and candidate endpoints |
| `report_connection` | room_id, peer_id, result, failure stage, establishment time | ack |
| `report_telemetry` | room_id, peer_id, RTT/loss/jitter/volume | ack |

### 6.2 Connection Establishment Sequence

```text
Host                      AWS                       Player
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
 |<-- peer endpoints -------|--- peer endpoints ------>|
 |                         |                          |
 |========= simultaneous UDP HELLO (hole punching) ====|
 |========= Direct P2P tunnel established =============|
 |                         |                          |
 |--- report_connection -->|<--- report_connection ---|
```

---

## 7. Virtual Network Design

- Subnet: `10.100.0.0/24`
- `10.100.0.1`: room creator (host). The game server runs here.
- `10.100.0.2` and above: participants. Assigned sequentially by the control plane.
- Virtual IPs are valid per room and are reclaimed when the room disappears.

The player types `10.100.0.1:25565` into the Minecraft server address field. They never need to know a public IP or port.

Windows routing is handled by attaching a `10.100.0.0/24` route to the virtual adapter. The client sets this route itself through the Windows IP Helper API when it creates the adapter, and removes it on shutdown. Wintun does not configure addresses or routes.

---

## 8. Failure Diagnosis

Connection failures must be classified by stage. "It did not connect" is not accepted as a record.

| Code | Stage | Basis |
|------|-------|-------|
| `STUN_DISCOVERY_FAILED` | public endpoint discovery | no STUN response, or parse failure |
| `CONTROL_PLANE_EXCHANGE_FAILED` | candidate exchange | control plane request failed, or no peer candidates |
| `HOLE_PUNCH_TIMEOUT` | hole punching | no peer `HELLO` within the time limit |
| `PEER_HANDSHAKE_FAILED` | handshake | peer `HELLO` arrived but no `HELLO_ACK` within the time limit |
| `TUNNEL_DROPPED` | maintenance | no packets from the peer for the idle timeout after establishment |

Each failure also records whatever NAT environment information is available (local subnet, public endpoint, per-STUN-server mapping differences). The public endpoint is absent for `STUN_DISCOVERY_FAILED`, where discovery never succeeded; record the fields that exist rather than requiring all of them.

**Do not assert a NAT type.** RFC 5389 Binding results and port changes alone cannot determine NAT type or filtering behavior. The Phase 9 analysis classifies by **observed mapping behavior** (does the public port change with the destination?) and **connection success**, not by "NAT type". Determining a type would additionally require an RFC 5780 capable server or controlled multi-destination testing, which is out of initial scope.

---

## 9. Telemetry

| Metric | Collection point | Purpose |
|--------|------------------|---------|
| Connection success/failure + failure stage | end of session establishment | reliability analysis |
| Connection establishment time | STUN start to `CONNECTED` | perceived experience analysis |
| RTT | periodic `PING`/`PONG` | latency analysis |
| Packet loss rate | missing `sequence` gaps | quality analysis |
| Jitter | deviation between consecutive RTTs | quality analysis |
| Keepalive local send errors | `[loop]` send path | stability analysis. This counts local `sendto` errors only; it is not evidence of delivery failure, which is why disconnection is decided by idle timeout instead |
| Idle timeout events | receive path | stability analysis |
| Direct connection uptime | duration in `CONNECTED` | stability analysis |
| Tunnel throughput | send/receive byte counters | performance analysis |
| Session duration | full game session | Minecraft stability |

The client buffers into a local ring buffer first and a dedicated thread uploads to the control plane periodically. The upload is fully separated from the data plane, so a control plane outage never affects the tunnel. The isolation structure is in 3.2.6.

**Separate from the upload there is a local record file.** This is what M-6 of [`spec.md`](spec.md) requires, not control plane storage. There are four contracts.

| Item | Contract |
|------|----------|
| Content | For each connection attempt, the result (success, or the FR-13 failure code of [`spec.md`](spec.md)) and the RTT |
| Timing | At the end of session establishment. Written on success and on failure alike |
| Method | Append-only. Earlier lines are not modified |
| Independence | No dependency on the telemetry queue or the upload path. The record remains even with no control plane |

**The path and the line format are not decided yet.** They are decided in this document. They are not chosen arbitrarily in code. **When they have to be decided belongs to [`roadmap.md`](roadmap.md).**

---

## 10. Repository Layout

### Target layout

```text
Sangtachi/
+-- client/
|   +-- include/
|   +-- src/
|   |   +-- main.cpp
|   |   +-- network/     udp_socket, endpoint, stun_client
|   |   +-- peer/        peer, hole_punch, session
|   |   +-- tunnel/      packet, tunnel, router
|   |   +-- adapter/     wintun_adapter
|   |   +-- telemetry/   telemetry
|   |   +-- control/     control_client
|   +-- CMakeLists.txt
+-- control-server/
|   +-- server.py
|   +-- room.py
|   +-- peer.py
|   +-- telemetry.py
+-- tests/
+-- scripts/
+-- docs/
|   +-- eng/                 English documents (this tree)
|   |   +-- architecture.md      this document
|   |   +-- spec.md              requirements and success criteria
|   |   +-- roadmap.md           phased development plan
|   |   +-- plan.md              handover for the next session
|   |   +-- first_design.md      initial proposal (reference only)
|   |   +-- protocol.md          fixed tunnel protocol
|   |   +-- experiments.md       experiment design and results (written in Phase 9)
|   |   +-- audit-history/       archive of full design audit records
|   |   +-- decisions/           design decision records
|   |   +-- commit_history/      per-work-unit change records
|   +-- kor/                 Korean documents, same structure
+-- README.md
```

Both language trees hold the same files. When a document changes, update both sides in the same commit.

### Difference from the current state

The source tree currently contains only `src/main.cpp` and `CMakeLists.txt` at the root (`docs/` is already populated). The relocation that moves them under `client/` and creates `control-server/` is still pending.

---

## 11. External Dependencies

| Dependency | Scope provided | Approval status |
|------------|----------------|-----------------|
| Wintun | Windows virtual network interface access only. Adapter creation, packet read/inject | **Approved** |
| Windows IP Helper / NetIO API | IP address and route configuration for the virtual adapter. **Wintun does not provide this** | OS built-in |
| Winsock2 | Windows standard socket API | OS built-in |
| Public STUN servers | Binding Response replies. The server is not implemented, only used | external public service |
| Python standard library | `asyncio`, `json`, `sqlite3` | standard library |

Be explicit about what Wintun does **not** provide. Peer discovery, STUN, NAT traversal, hole punching, the tunnel protocol, routing decisions, session management, monitoring, and diagnostics are all implemented by this project. Wintun is only a means of access to a kernel-mode virtual NIC driver.

---

## 12. Out of Initial Scope

The following are deliberately kept out of the initial architecture. If any becomes necessary, record the decision in [`decisions/`](decisions/) before adopting it.

- **Encryption and peer authentication**: tunnel payloads are plaintext. Stretch goal.
- **Relay fallback (TURN-like)**: no alternate path when a direct connection fails. Stretch goal.
- **Full ICE**: only a minimal establishment procedure initially. Phase 9 performs a conceptual comparison with ICE, nothing more.
- **Mesh of three or more**: two peers initially. The routing table structure stays extensible but is not implemented.
- **GUI**: console only.
- **macOS / mobile**: not supported. Linux interoperability is examined only if time allows.
