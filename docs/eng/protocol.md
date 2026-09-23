# Tunnel Protocol v1

**Project:** Direct-First P2P Virtual Network for Multiplayer Games
**Status:** Fixed before implementation. If this document and the code differ, the code is wrong.
**Design background:** [`architecture.md`](architecture.md) / **Requirements:** [`spec.md`](spec.md)

> Korean version: [`../kor/protocol.md`](../kor/protocol.md)

The goal is that **the implementer never has to invent anything.** If a value is left undecided anywhere, that is a defect of this document.

---

## 1. Scope and Assumptions

| Item | v1 fixed value |
|------|-----------|
| Outer transport | IPv4 UDP only |
| Inner payload | IPv4 packets only. Inner IPv6 is dropped and a counter is incremented |
| Byte order | Every multi-byte integer is **big-endian** |
| Peer count | 2 peers per room |
| Encryption | None (Chapter 2) |

IPv6 support is v2 or later. v1 opens sockets with `AF_INET` only and resolves STUN servers to IPv4 only.

---

## 2. Security Model

**v1 provides no confidentiality, no integrity, no peer authentication, and no replay-attack defense.** This is not an omission. It is an explicit choice.

An on-path or off-path attacker who knows or guesses `peer_id` and `session_epoch` can do the following.

- Eavesdrop on and tamper with game traffic
- Inject arbitrary packets
- Force session termination with a forged `CLOSE`
- Hijack traffic by abusing endpoint roaming (10.5)

**Forcing session renegotiation with a forged `HELLO` has a weaker precondition. `peer_id` alone is enough.**
The trigger for renegotiation is "a `HELLO` with an epoch different from the pinned one" (9.3), so the
attacker does not need to know the epoch and can put in any value. The only remaining check a `HELLO` has
to pass is the `virtual_ip` match (5.1), and that value is predictable: `10.100.0.1` and `10.100.0.2`.

**One shot ends the session on both sides. This is certain, not merely reachable.** There are four paths.

- Step 1 of 9.5 Renegotiation puts the real peer's live epoch on the retired list. Every packet the peer
  sends after that is dropped by check 8 of 8.1 Common Checks with `drop_retired_epoch`
- If the peer is `CONNECTED`, it has already stopped `HELLO` retransmission per 10.3 Punch. Even if the peer
  is still punching, its retransmitted `HELLO` carries the retired epoch and is dropped by the same check
- Check 8 comes before 8.2 Epoch and Source Checks and 9.3 Epoch Rules. So that `HELLO` cannot become a
  renegotiation trigger
- The peer draws a new epoch only when it starts a fresh attempt from `IDLE` per 9.6 Failure Transitions.
  That is after the peer has burned the 50s idle timeout, which is later than our 10s punch deadline

The result is **`PEER_HANDSHAKE_FAILED` after 10s on our side and `TUNNEL_DROPPED` after 50s on the peer's side**.
The caps of 9.5 Renegotiation (count and interval) only make the repeat cost finite. They do not stop this one shot.

**The final report must state this precondition as `peer_id` alone.**

> **Why.** Writing that both are needed overstates the attack difficulty.

The `DATA` inner validation of 8.4 is **not an attack defense.** It is a hygiene device against accidental injection from misdelivery, bugs, and wrong routing.

The basis for this choice is the non-goals in `spec.md`. Developing our own cryptographic algorithm
and authenticated peer sessions are stretch goals outside v1 scope. The final report must state this
limitation.

**This chapter covers only threats on the tunnel path.** The trust assumptions of the control plane
path (plaintext TCP, unauthenticated callers, `room_id` as the only secret) and their consequences
belong to [`control_plane.md`](control_plane.md) 1.2 Trust Assumptions. The final report must state
both limitations side by side.

That said, **paths that harm third parties are either blocked or capped. The document states which is which.**

| Path | What v1 does |
|------|--------------|
| Amplification — a candidate or a receive source is a broadcast or multicast address | **Blocked.** This is why the candidate hygiene of 10.1 Candidate Collection and Hygiene and the source hygiene of chapter 7 are mandatory |
| Unicast reflection — an arbitrary third-party address enters the candidate list | **Not blocked. Only capped.** 8 candidates (`MAX_CANDIDATES`) x 10s of punching / 200ms = at most 400 shots, and retransmission stops once `CONNECTED` is reached |
| Unicast reflection — replies to a forged source (`HELLO_ACK`, `PONG`, verification `HELLO`) | **Not blocked. Only capped.** The shared budget for unverified destinations in 9.4 Transition Table limits this to 5 packets per second and 10 packets in a burst per session |

**The control plane is not the only party that can insert candidates.** A peer that joined the room can
register any unicast address with `register_candidate`, and the server checks only the format. That a
malicious server can do the same is stated in `control_plane.md` 1.2 Trust Assumptions.
**The two documents describe the same path.** Here we state the cap; that document states the trust assumption.

---

## 3. Constants

```cpp
constexpr uint32_t TUNNEL_MAGIC   = 0x53414E47;  // "SANG"
constexpr uint8_t  TUNNEL_VERSION = 0x01;
constexpr size_t   HEADER_SIZE    = 20;
constexpr size_t   MAX_INNER      = 1452;
constexpr size_t   MAX_DATAGRAM   = HEADER_SIZE + MAX_INNER;   // 1472
constexpr uint32_t STUN_COOKIE    = 0x2112A442;                // RFC 5389
constexpr size_t   MAX_CANDIDATES = 8;                         // cap per peer
constexpr size_t   REPLAY_WINDOW  = 64;                        // 4.5. moves together with the seen_bits width
constexpr size_t   MAX_PENDING_PINGS   = 16;
constexpr size_t   MAX_PROBE_PATHS     = 4;                    // 10.4 (c) cap on concurrent tentative paths
constexpr size_t   MAX_RETIRED_EPOCHS  = 16;                   // 4.3 retired list cap
constexpr size_t   MAX_RENEGOTIATIONS  = 8;                    // 9.5 renegotiation cap per attempt
constexpr uint32_t MIN_RENEG_INTERVAL_MS = 1000;               // 9.5 minimum renegotiation interval
constexpr size_t   UNVERIFIED_TX_BUCKET  = 10;                 // 9.4 unverified-destination transmit budget capacity
constexpr size_t   UNVERIFIED_TX_REFILL  = 5;                  // 9.4 refill per second
```

**Four tables grow from peer input, and all four carry a cap.** The candidate list (`MAX_CANDIDATES`),
`pending_pings` (`MAX_PENDING_PINGS`), tentative paths (`MAX_PROBE_PATHS`), the retired list
(`MAX_RETIRED_EPOCHS`). **Do not create a new uncapped table that grows from peer input.**
We do not authenticate the peer, so every input that grows such a table can be forged.

**A table that grows only from our own attempts carries no cap.** The attempt-nonce retired list of
5.1 `HELLO` is that table. An entry appears only when we start a new attempt and disappears 2 minutes
later, so an attacker cannot grow it. It is not that the cap is missing; **it is not a target for a cap.**
The duplicate suppression state (4.5) is not a table either. It is one state for one peer epoch, and
renegotiation resets it (9.5 step 3, 8.3).

The first byte of `TUNNEL_MAGIC`, `0x53`, has top two bits `01`, and a STUN message always has top
two bits `00`. This property is the basis of the classification in Chapter 7, so it must be
preserved if the magic value is ever changed.

---

## 4. Header

### 4.1 Wire Layout

```text
 Offset  Size  Field             Type        Description
   0      4   magic             uint32 BE   fixed 0x53414E47
   4      1   version           uint8       fixed 0x01
   5      1   type              uint8       PacketType (Chapter 5)
   6      2   payload_length    uint16 BE   number of bytes after the header
   8      4   peer_id           uint32 BE   sending peer identifier
  12      4   session_epoch     uint32 BE   identifier of the sending peer's current session attempt
  16      4   sequence          uint32 BE   per-direction monotonically increasing counter
  = 20 bytes
```

Every 4-byte field sits at an offset that is a multiple of 4, so MSVC default alignment adds no
padding. Even so, do not `memcpy` a struct. Byte-order conversion is required, and relying on
alignment that happens to line up breaks silently. **Serialize field by field** and use the
`HEADER_SIZE` constant for the length.

### 4.2 peer_id

Assigned by the control plane when joining a room. **Unique within a room.** If both peers had the
same `peer_id`, sender identification and self/peer distinction would be impossible, so the control
plane never makes such an assignment, and the client aborts with `CONTROL_PLANE_EXCHANGE_FAILED` if
it finds its own value in the `get_peers` response.

### 4.3 session_epoch

Drawn **per session attempt**. Not per process. A new value is drawn every time a new attempt starts from `IDLE`, and it does not change until that attempt ends. 0 is not used.

Without this field, delayed packets from a previous attempt would be accepted into the new session
after a restart or retry. The receiver pins the peer epoch on the first valid packet, and packets
with a different epoch afterwards are handled by the rules of Chapter 9.

**Drawn from the OS CSPRNG** (`BCryptGenRandom`, same source as the nonce. 5.1). If 0 comes out, draw again.
With a weak random source, the probability of the same value on each retry grows, and the retired list below
fails to filter packets from the previous attempt. A predictable value lowers the threat precondition of Chapter 2 by that much.

It is a 32-bit random number, so collisions are possible. An epoch is kept on a **retired list** for
up to 2 minutes, and every packet carrying an epoch on the retired list is dropped (check 8 of 8.1
Common Checks).

**What we keep is the peer's epoch.** Check 8 looks at the `session_epoch` of the received packet,
and that is **the sender's value, that is, the peer's.** Putting our own past epoch there matches no
received packet, so it filters nothing. There are two registration cases.

| When | What is registered |
|------|--------------------|
| We start a new attempt from `IDLE` | **The peer epoch pinned in the previous attempt.** An attempt that ended before pinning has no value to register |
| 9.5 Renegotiation | The peer's immediately previous epoch (9.5 step 1) |

**The retired list is not the only line of defense on our retry direction.** If the previous attempt ended
before pinning the peer epoch, there is no value to register. Delayed packets from that interval are
stopped by 8.2 Epoch and Source Checks.

- A non-`HELLO` packet has no pinned epoch, so it is `drop_no_epoch`
- A `HELLO_ACK` that echoes the nonce of the previous attempt is `drop_bad_nonce`. This is why 5.1 `HELLO`
  never reuses a previous nonce

The retired list covers **attempts that did reach pinning**.

**The retired list holds at most `MAX_RETIRED_EPOCHS` (16) entries.** When a new entry arrives while the
list is full, the oldest one is evicted. Entries come only from our own previous attempts (1 per retry) and
from the peer's previous epochs (1 per renegotiation). The count and interval of renegotiation are limited
by 9.5 Renegotiation.

> **Why.** Without the cap, the table that check 8 of 8.1 consults on every received packet grows without bound.

**The cap comes before the 2-minute retention. So the 2 minutes is best-effort, not a guarantee.** If more
than 16 entries arrive within 2 minutes, the oldest is evicted before its time is up. This does happen when
fast local retries and renegotiations overlap.

If a packet carrying an evicted epoch arrives late, it passes check 8 of 8.1. At that point **the epoch pin
comparison remains as a second line of defense.**

- It differs from the pinned value, so any type other than `HELLO` is dropped with `drop_stale_epoch`
  (9.3 Epoch Rules)
- Only `HELLO` is the exception that can trigger a 9.5 renegotiation. That damage is bounded by the count
  and interval caps of 9.5 Renegotiation

**The two lines together are finite**; that is all this design claims. It does not claim "filtered without
fail for 2 minutes".

### 4.4 sequence

- **One counter per direction, per session attempt.** Types are not distinguished.
- **The first packet sent in an attempt has `sequence = 0`.** Assign the current value right before sending, then add 1. Retransmissions also get a new number.
- Wraps at 32 bits.

```cpp
// Is a strictly newer than b. a == b is false.
bool is_newer(uint32_t a, uint32_t b) {
    return a != b && (uint32_t)(a - b) < 0x80000000u;
}
```

If it returned true for `a == b`, a duplicate would be classified as a new packet and replay defense would be defeated.

### 4.5 Duplicate Suppression and Loss Accounting

The receiver keeps the state below per peer epoch.

```cpp
uint32_t highest;      // highest sequence received so far (32-bit wrap)
uint64_t position;     // wrap-free 64-bit extended position of highest
uint64_t seen_bits;    // receive bitmap for back=1..REPLAY_WINDOW. back=b is bit (b-1)
bool     initialized;
```

Received packets are processed in the following order. **This decision comes before any side
effect.** Idle timer refresh, state transitions, Wintun injection, and `PONG` matching all happen
only after passing this step.

**There is one exception: 9.5 Renegotiation.** When 8.2 Epoch and Source Checks routes a `HELLO`
with a different epoch to 9.5, renegotiation runs before this decision, and its step 3 resets the
duplicate suppression state. This step then decides against the state of the new epoch. The reason
is in 8.3 Duplicate Suppression.

```text
if initialized is false:
    highest = seq; position = 0; seen_bits = 0; initialized = true
    -> accept (advance). This packet's pkt_pos = 0

if seq == highest:
    -> drop, drop_duplicate

if is_newer(seq, highest):                         // advance
    shift = (uint32_t)(seq - highest)              // 1 .. 2^31-1
    if      shift >  64:  seen_bits = 0
    else if shift == 64:  seen_bits = uint64_t{1} << 63
    else:                 seen_bits = (seen_bits << shift) | (uint64_t{1} << (shift-1))
    highest   = seq
    position += shift
    -> accept (advance). This packet's pkt_pos = position

otherwise (old):
    back = (uint32_t)(highest - seq)               // 1 .. 2^31
    back > REPLAY_WINDOW                 -> drop, drop_too_old
    seen_bits & (uint64_t{1}<<(back-1))  -> drop, drop_duplicate
    seen_bits |= uint64_t{1} << (back-1)
    -> accept (no advance). This packet's pkt_pos = position - back
```

Four boundaries are easy to get wrong.

- **`seq == highest` must be filtered first.** `is_newer` is false for `a == b`, so this case would
  fall into the branch below, `back == 0`, and `uint64_t{1} << (0-1)` is undefined behavior. Handle it
  first with an explicit duplicate check.
- **The three branches must be mutually exclusive.** Written as `if / if / else`, `shift > 64` clears the bitmap and then also enters `else`, performing a shift over 64. Write `if / else if / else`.
- **Do not clear the bitmap at `shift == 64`.** If cleared, the previous `highest` is left unrecorded
  at position `back == 64`, and that packet is accepted again even though it is a duplicate.
  `shift == 64` is the case that sets bit 63. `seen_bits << 64` is undefined behavior in C++, so handle
  it in a separate branch.
- **Do every bit operation in `uint64_t`.** `1 << 63` is an `int` shift and undefined behavior.

The literals `64` and `63` in the branches above are `REPLAY_WINDOW` and `REPLAY_WINDOW - 1`. Because `seen_bits` is
`uint64_t`, **the window size and the bitmap width move together.** To change the window, change both;
changing only the constant while leaving the bitmap makes the shift undefined behavior.

**Loss accounting is initialized on entering `CONNECTED`.** During the punch phase, the `HELLO`s
sent to several candidates each consume a number, but only one path actually delivers, so counting
the gaps of that phase as loss pollutes the measurement.

Take the baseline **immediately after** processing the packet that transitioned to `CONNECTED`.

```text
baseline = position          // value after processing the transitioning packet
accepted = 0                 // number of unique packets accepted after baseline

on each acceptance afterwards: if that packet's pkt_pos is greater than baseline, accepted += 1

expected = position - baseline
loss      = expected - accepted
loss rate = loss / expected if expected > 0, else 0
```

It is computed from the wrap-free 64-bit `position`, not the 32-bit `sequence`, so the values do not collapse when the sequence wraps around.

`pkt_pos` is the updated `position` for an advancing packet and `position - back` for a reordered packet.
Reordered packets at or below `baseline` are excluded exactly.

> **Why.** If the state value `position` were used as is, reordered packets sent before `baseline` would
> also be counted in `accepted`, giving `accepted > expected` and making `loss` underflow to a negative number.

Packets dropped with `drop_too_old` are already confirmed as loss and are not reverted. The loss rate is snapshotted every 10 seconds and uploaded to the telemetry service.

---

## 5. Packet Types

| Value | Name | Payload length | Purpose |
|----|------|---------------|------|
| `0x01` | `HELLO` | 20 | Hole punching and handshake initiation |
| `0x02` | `HELLO_ACK` | 20 | Proves the peer's `HELLO` was actually received |
| `0x03` | `KEEPALIVE` | 0 | Keeps the NAT mapping alive |
| `0x04` | `DATA` | 20 ~ 1452 | Carries an inner IPv4 packet |
| `0x05` | `PING` | 8 | RTT measurement request |
| `0x06` | `PONG` | 8 | RTT measurement response |
| `0x07` | `CLOSE` | 1 | Graceful shutdown notice |

A value not in the list is dropped and `drop_unknown_type` is incremented.

### 5.1 HELLO (20 bytes)

```text
 Offset  Size  Field        Description
   0     16   nonce        128-bit random number for this attempt
  16      4   virtual_ip   virtual IP assigned to the sender (uint32 BE)
```

The `nonce` is drawn from the **OS CSPRNG** (`BCryptGenRandom`). One per session attempt;
retransmissions use the same value. If it changed on each retransmission, an arriving `HELLO_ACK`
could not be matched to the nonce it answers. The nonce of a previous attempt is kept on the retired
list for 2 minutes and not reused.

Consumer of `virtual_ip`: the receiver compares it with the peer's virtual IP given by the control plane. If they differ, drop and increment `drop_vip_mismatch`.

**probe nonce.** The path-validation `HELLO` of 10.4 (c) uses a separate **probe nonce**, not the
attempt nonce. One is drawn per tentative path and kept in a separate table for 5 seconds. The "one
per attempt" rule above applies only to the attempt nonce. If the two kinds are mixed in one table,
path-validation responses cannot be told apart from handshake responses.

### 5.2 HELLO_ACK (20 bytes)

```text
 Offset  Size  Field        Description
   0     16   echo_nonce   the nonce of the HELLO being answered, copied verbatim
  16      4   virtual_ip   virtual IP assigned to the sender (uint32 BE)
```

`virtual_ip` handling is the same as 5.1 `HELLO`.

### 5.3 KEEPALIVE (0 bytes)

No payload.

### 5.4 DATA (20 ~ 1452 bytes)

The payload is a **complete inner IPv4 packet**. No Ethernet header, no additional wrapping.

### 5.5 PING / PONG (8 bytes each)

```text
 Offset  Size  Field     Description
   0      8   ping_id   uint64 BE
```

`ping_id` starts at 0 per session attempt and increments by 1. **No timestamp is carried on the wire.** The send time is kept only in a local table.

```cpp
uint64_t id = next_ping_id++;
pending_pings[id] = steady_clock::now();
```

- `pending_pings` holds at most `MAX_PENDING_PINGS` (16) entries. Before inserting, remove entries older than 5 seconds; if it is still full, do not send a new `PING`.
- On receiving a `PONG`, look up by `ping_id`. If absent, drop and increment `drop_unmatched_pong`. If present, compute the RTT and **remove the entry.** Each `ping_id` is valid once.
- Entries past 5 seconds are removed and their sample discarded.

`PONG` returns the received `ping_id` unchanged.

### 5.6 CLOSE (1 byte)

```text
 Offset  Size  Field     Description
   0      1   reason    0x00 graceful shutdown, 0x01 user cancel, 0x02 configuration error, 0x03 establishment failure
```

Values other than `0x00`~`0x03` are dropped and `drop_bad_reason` is incremented. On graceful shutdown it is sent once, best-effort. No retransmission, no waiting for a response.

**The receiver removes the routing entry immediately and leaves the session in `CLOSED`.** If the session
object were deleted too, a packet arriving afterwards would fall to `drop_unknown_peer` instead of the
`drop_terminal_state` defined by 9.4.1 Receiving in `IDLE` and the Terminal States, and the place that
counts post-shutdown receives would disappear. The socket stays open until the process exits (chapter 6),
so those packets really do keep coming.

**A session in which we sent `CLOSE` also transitions to `CLOSED`, whether or not the send succeeded.**
Sessions that do not send (before learning and `FAILED` in the table below) are finished by the shutdown
procedure as they are, and `FAILED` stays `FAILED`. Without this transition, the state in which a session
ends on the graceful shutdown path is undefined, so the exit line code and the counter dump moment of
[`architecture.md`](architecture.md) chapter 9 would differ between implementations.

**Where it is sent. Sent once, only in sessions that have learned `peer_endpoint`.** The criterion is
whether it was learned, not the state.

| Session | Sent? |
|------|----------|
| `peer_endpoint` learned (`CONNECTED`, and `HANDSHAKING`·`PUNCHING` after learning) | **Sent.** One packet to one verified address |
| Before learning (`IDLE`, `PUNCHING` before learning) | Not sent |
| `FAILED`, `CLOSED` | Not sent |

**Why sending only in `CONNECTED` is wrong.** The two flags do not rise at the same moment on both sides (9.2).
If the peer's `HELLO_ACK` is lost, we end up in a state where we are `HANDSHAKING` and the peer is `CONNECTED`.
The peer being `CONNECTED` means we already sent a `HELLO_ACK` (that is the peer's
`got_ack`), so at that point we have received the peer's `HELLO` and `peer_endpoint` is learned.
If we do not send `CLOSE` in this case, **the peer waits out the 50-second idle timeout.** That is exactly the time
`CLOSE` is meant to save.

**The reason for not sending before learning is reflection.** Before learning, the send target is the whole candidate set (10.4),
so the shutdown notice would be sprayed to several unverified addresses. It is the same kind of path that 10.4 (c) caps.
A session before learning also has no idle timeout to save, since the peer is not `CONNECTED` yet either.

**Which value is sent.** Receive handling is the same for all four, so this distinction is used only in the peer's diagnostic record.

| reason | When sent |
|--------|-------------|
| `0x00` | A `CONNECTED` session is closed through the normal shutdown path (user shutdown request, termination signal) |
| `0x01` | The user terminated a session that **has not yet reached `CONNECTED`**. Cancellation during establishment |
| `0x02` | This session cannot continue because of local configuration. The received virtual IP collides with a local range, or adapter configuration failed |
| `0x03` | **This session ended in failure.** Sent by 9.6 Failure Transitions. Nobody cancelled it |

**The same shutdown command yields a different value depending on state.** When the user orders shutdown, a session in
`CONNECTED` gives `0x00`, and one in `PUNCHING` or `HANDSHAKING` gives `0x01`. Without this distinction the two values would
point at the same situation, and the peer's diagnostic record could not separate "graceful shutdown" from "cancelled during establishment".

**`0x03` exists for the same reason.** The failure transition of 9.6 also sends a `CLOSE` during
establishment, so without its own value it would use `0x01`, and then "a person switched it off" and
"the handshake failed" would be the same value in the peer's record. The first is an event to exclude from
the Phase 9 aggregate; the second is one to count. It is the same place where [`spec.md`](spec.md) A-2
asks for controlled failures and real failures to be counted separately.

In a Phase where no situation calls for `0x02`, it is not used. The value is not removed. If the wire enumeration
is extended later, existing implementations drop it with `drop_bad_reason`.

Without `CLOSE`, graceful shutdown and a crash cannot be distinguished, and the peer holds a dead session until the 50-second idle timeout. The idle timeout stays as the crash fallback.

---

## 6. Socket Ownership

**One local UDP socket is owned exclusively by one receive loop.** The thread layout, wait
mechanism, and state ownership of that loop are in [`architecture.md`](architecture.md) 3.2
Concurrency Model.

- The STUN client does not call `recvfrom` directly. It registers a request and receives the response from the receive loop. Reading in two places consumes each other's packets.
- The socket is bound once at process start and kept until exit. Rebinding changes the NAT mapping.
- **The bind target is `INADDR_ANY`, port 0.** With port 0 the OS chooses. The price is that the local
  port changes on every run, and that consequence is written in the restart limitation of 10.4 Endpoint
  Learning
  - Binding to a specific interface address means packets arriving on another interface are not received.
    That makes the local candidates of 10.1 Candidate Collection and Hygiene meaningless
  - Fixing the port makes startup fail when it is already in use, and prevents tests that run two
    processes on the same machine
- Right after bind, read the actual port with `getsockname` and use it in the local candidates of 10.1 Candidate Collection and Hygiene. Do not register port 0 as is.
- **`SO_SNDBUF` uses the default.** This loop sets the send rate itself (200ms retransmission, 15s
  keepalive, game traffic), and if the buffer runs short `sendto` reports an error. If evidence for
  raising it appears, fix this line then.
- **`SO_RCVBUF` is set explicitly.** The requested value is `262144` (256KiB). Written as a byte count,
  not with a unit suffix
  - The criterion is larger than `MAX_DRAIN` × `MAX_DATAGRAM`. `MAX_DRAIN` is owned by
    `architecture.md` 3.2.3 One Loop Iteration, and computed with that document's current value it is
    94208 bytes. If the budget changes, revisit this value
  - After setting, read the actually applied value with `getsockopt` and record it
  - The default is not used because it varies by OS and version and may be smaller than one receive loop
    iteration budget. If it cannot hold one iteration's worth, the budget design loses its meaning
  - The applied value is recorded because the OS may not grant the requested value as is. Without the
    record the cause of loss cannot be found later
- `SO_REUSEADDR` is not used. On Windows another socket could bind the same endpoint and delivery becomes non-deterministic. Use `SO_EXCLUSIVEADDRUSE`.
- `connect()` is not called. A connected UDP socket filters out datagrams from other sources, so the STUN server and several candidates could not be handled at once.
- **`SIO_UDP_CONNRESET` is turned off.** If it is left on, ICMP port unreachable from an unresponsive
  candidate surfaces as `WSAECONNRESET` and terminates the receive loop. Hole punching keeps firing at
  unresponsive candidates by design, so leaving this out will blow up in Phase 4 without fail.

```cpp
BOOL off = FALSE;
DWORD bytes = 0;
WSAIoctl(sock, SIO_UDP_CONNRESET, &off, sizeof(off), nullptr, 0, &bytes, nullptr, nullptr);
```

**A `sendto` failure increments `tx_err_send` regardless of type and the packet is discarded. There is no
retransmission.** The next periodic retransmission or keepalive takes its place, and the disconnect
decision is made by the idle timeout, not by a send failure (9.6 Failure Transitions).

**Only a `HELLO_ACK` failure also increments `tx_err_ack`.** That failure is counted separately because it
is the cause of `sent_ack` not rising in 9.2 `CONNECTED` Condition. It is not that one more counter was needed.

Logging is handled by `socket.error` of `architecture.md` chapter 9. The counter is also the value read by
the "keepalive local send error" row of the metric table in chapter 9 of that document.

---

## 7. Receive Classification

The receive loop classifies each datagram **by the top two bits of the first byte only**. Magic and
version are validation items, not classification criteria.

> **Why.** If magic were checked at the classification step, packets with a wrong magic would never reach
> the tunnel pipeline. The `drop_magic` counter would stay at 0 forever, and malicious traffic aimed at
> the tunnel would blend into `drop_unclassified` and become invisible.

```text
Source hygiene (table below). If it fails
      -> drop, drop_bad_source
(buf[0] & 0xC0) == 0x00  and  len >= 20  and  buf[4..8) == STUN_COOKIE
      -> STUN message. Match to a pending request by transaction ID (Chapter 13)
(buf[0] & 0xC0) == 0x40
      -> tunnel candidate. To the Chapter 8 validation pipeline
otherwise
      -> drop, drop_unclassified
```

**Why source hygiene comes before classification.** The source of a received datagram becomes the
destination we reply to. The `HELLO_ACK` of 9.4 Transition Table and 9.5 Renegotiation goes "to the source
of that datagram", and so does the validation `HELLO` of 10.4 (c). Sources can be forged, so the reflection
and amplification paths that 10.1 Candidate Collection and Hygiene blocked for the candidate list
reopen in the replies. Filtering before classification gives the STUN path and the tunnel path the same protection.

| Source | Verdict | Basis |
|--------|------|------|
| Multicast `224.0.0.0/4` | Drop | One reply is amplified by the number of group subscribers |
| Limited broadcast `255.255.255.255` | Drop | Same reason |
| Unspecified `0.0.0.0` | Drop | Nowhere to reply |
| Port 0 | Drop | Nowhere to reply |
| Loopback `127.0.0.0/8` | **Pass** | The reply goes only to ourselves, so there is no amplification. Tests that run two processes on the same machine use this path |
| Otherwise | Pass | |

**This is not the same table as the candidate hygiene of 10.1 Candidate Collection and Hygiene.** That section rejects loopback; here it passes. Candidates are
a list of destinations we send to first, so a loopback there is either meaningless or a trick, but a loopback as a receive source
has a legitimate case: another process on the same machine. **The two tables are not merged into one.**

**Subnet broadcast (directed broadcast) splits in two.**

| What | Verdict | Why |
|------|------|-----|
| Broadcast address of **our own** active IPv4 subnet | Drop | 10.1 Candidate Collection and Hygiene already reads each interface's address and netmask when building local candidates. It can be computed from those. This is also where amplification actually happens |
| Broadcast address of a remote subnet | Pass (cannot be blocked) | We do not know that subnet's netmask. There is no way to tell it from unicast |

The remote subnet case is **recorded as a residual risk.** Routers not forwarding directed broadcast
is the default behavior RFC 2644 requires, so for amplification to actually occur the path would have to violate that requirement.
That is not counted as a defense. It is someone else's configuration that we cannot verify.

`buf[a..b)` is a half-open interval. `buf[4..8)` is the 4 bytes at offsets 4,5,6,7.

Datagrams exceeding `MAX_DATAGRAM` (1472) do not reach this step. The receive loop drops them first
([`architecture.md`](architecture.md) 3.2.3).

---

## 8. Receive Validation Pipeline

Tunnel candidates are checked in the order below. Each step has its own drop counter. If any check fails, drop immediately.

### 8.1 Common Checks

| # | Check | Failure counter |
|---|------|-------------|
| 1 | `len >= HEADER_SIZE` | `drop_short` |
| 2 | `magic == TUNNEL_MAGIC` | `drop_magic` |
| 3 | `version == TUNNEL_VERSION` | `drop_version` |
| 4 | `type` is in the Chapter 5 list | `drop_unknown_type` |
| 5 | `payload_length == len - HEADER_SIZE` | `drop_length` |
| 6 | Per-type length rule met: `HELLO`/`HELLO_ACK` exactly 20, `KEEPALIVE` 0, `PING`/`PONG` 8, `CLOSE` 1, `DATA` 20~1452 | `drop_type_length` |
| 7 | `peer_id` matches the peer ID of this room's other peer | `drop_unknown_peer` |
| 8 | `session_epoch` is not on the retired list | `drop_retired_epoch` |

### 8.2 Epoch and Source Checks

`HELLO` and `HELLO_ACK` have to pass even before the epoch is pinned. The remaining types are valid only
after the epoch is pinned. The two types pass for different reasons.

- **`HELLO_ACK` proves itself with `echo_nonce`.** It returns the 128-bit random number we sent, which is
  evidence that the packet came from the side that actually received our `HELLO`
- **`HELLO` proves nothing.** The nonce is a fresh value drawn by the sender. `HELLO` passes because it is
  the trigger that pins the epoch, and this property is why Chapter 2 states the precondition of a forged
  `HELLO` as `peer_id` alone

**`HELLO` and `HELLO_ACK` are checked against `virtual_ip` before the epoch decision (5.1 `HELLO`).** This
check precedes every row of the table below; on mismatch the packet is dropped and `drop_vip_mismatch` is
incremented.

> **Why.** Placed after epoch pinning, a single forged `HELLO` would pin the epoch first.

| Type | Rule |
|------|------|
| `HELLO` | `virtual_ip` must match the peer's virtual IP (5.1). Accepted regardless of whether the epoch is pinned. The source is not required to be in the candidate set (Chapter 7 source hygiene has already passed). The epoch rules of 9.3 apply |
| `HELLO_ACK` (attempt nonce echo, epoch unpinned or matching the pinned value) | `echo_nonce` matches **the nonce we sent in this attempt**. Used as handshake evidence (`got_ack`), and if the epoch is unpinned, this packet pins it |
| `HELLO_ACK` (attempt nonce echo, epoch differs from the pinned value) | Drop, `drop_stale_epoch` |
| `HELLO_ACK` (probe nonce echo) | `echo_nonce` matches one of the active probe nonces and the source equals the address the probe was sent to. **Validates only that tentative path.** Does not set `got_ack` and does not pin the epoch |
| `HELLO_ACK` (otherwise) | Drop, `drop_bad_nonce` |
| Everything else | If the epoch is unpinned, drop, `drop_no_epoch`. If it differs from the pinned value, drop, `drop_stale_epoch` |

Without this rule, the statement in 9.4 Transition Table that "a leading `HELLO_ACK` is normal" and
the epoch check would contradict each other, and both sides would time out in the normal situation
where the peer started punching first.

**A `HELLO_ACK` with a valid nonce that arrives after the epoch is already pinned is dropped.** This case
really happens. If the peer crashes and restarts right after its first `HELLO` pinned the epoch during the
punch, the new instance echoes our nonce with a new epoch in reply to our retransmitted `HELLO`.

> **Why.** Accepting it would raise `got_ack` under the epoch of an attempt that has already ended, and a
> session in which every subsequent `DATA` hits `drop_stale_epoch` would become `CONNECTED`. Dropping loses
> nothing. If the peer is alive, its new `HELLO` arrives soon and the 9.5 renegotiation handles the same
> thing through the normal path.

### 8.3 Duplicate Suppression

Apply the decision of 4.5 Duplicate Suppression and Loss Accounting. A packet dropped with `drop_duplicate` or `drop_too_old` **does not refresh the idle timer and changes no state.**

**Duplicate suppression state is looked up by the epoch the packet carries.** Not the pinned epoch. This is what 4.5 Duplicate Suppression and Loss Accounting means by
"keeps the state per peer epoch". When the peer restarts and sends again from `sequence = 0` with a new epoch,
that epoch has no state yet, so `initialized` is false and the first packet is
accepted. **If judged against the pinned epoch's state, that packet is dropped with `drop_too_old` and the 9.5 renegotiation
loses its trigger.**

**Epoch handling comes before duplicate suppression.** When 8.2 Epoch and Source Checks sends a `HELLO` with a different epoch to 9.5, the renegotiation
resets the duplicate suppression state in step 3. 8.3 Duplicate Suppression then judges against the new epoch's state. Reversing the order
would catch the restarted peer's first `HELLO` in the previous attempt's window.

### 8.4 DATA Inner Validation

`DATA` has to pass everything below right before Wintun injection.

| # | Check | Failure counter |
|---|------|-------------|
| 9 | State is `CONNECTED` | `drop_data_early` |
| 10 | Inner IP version nibble == 4 | `drop_inner_not_ipv4` |
| 11 | Inner IHL is 5~15 and `IHL*4 <= payload_length` | `drop_inner_ihl` |
| 12 | Inner IPv4 header checksum valid | `drop_inner_checksum` |
| 13 | Inner `total_length == payload_length` | `drop_inner_length` |
| 14 | Inner source IP == the peer's virtual IP | `drop_inner_src` |
| 15 | Inner destination IP == our own virtual IP | `drop_inner_dst` |

Checks 14 and 15 are hygiene devices against accidental injection from misdelivery and routing bugs. As stated in Chapter 2, they are **not an attack defense.**

### 8.5 Transmit-Side Validation

An inner packet has to pass the checks below **regardless of its origin** before being encapsulated as `DATA`. From Phase 6 on the origin is
a Wintun read; before that it is a synthetic packet built by tests. The checks are the same.

> **Why.** If each origin had different checks, tests run with synthetic packets would not validate the real path.

| # | Check | Failure counter |
|---|------|-------------|
| 1 | State is `CONNECTED` | `tx_drop_not_connected` |
| 2 | Length >= 20 | `tx_drop_short` |
| 3 | IP version nibble == 4 | `tx_drop_not_ipv4` |
| 4 | Length <= `MAX_INNER` | `tx_drop_oversize` |
| 5 | Source IP == our own virtual IP | `tx_drop_bad_src` |
| 6 | Destination IP == the peer's virtual IP | `tx_drop_no_route` |

**Check 2 comes before checks 5 and 6.** Source and destination sit at offsets 12~20, so reading them before confirming the 20-byte lower bound
reads outside the buffer. On the receive side, check 6 of 8.1 guarantees the same lower bound, but the transmit side
had no counterpart. Wintun rarely hands out fewer than 20 bytes, but a decision function does not leave rare
cases as assumptions.

---

## 9. Session

### 9.1 States

| State | Meaning |
|------|------|
| `IDLE` | Peer candidates not yet received |
| `PUNCHING` | Peer candidates received, no round-trip evidence |
| `HANDSHAKING` | Evidence in one direction only |
| `CONNECTED` | Evidence in both directions |
| `FAILED` | Terminated with a failure code |
| `CLOSED` | Graceful shutdown |

**`PUNCHING` is entered when candidates are received, and `HELLO` retransmission starts later than that.**
Retransmission starts only after the punch delay of 10.2 Rendezvous expires. The waiting interval in
between is also `PUNCHING`, and the `PUNCHING` column of 9.4 Transition Table applies as is. Because of
rendezvous skew (10.2, bounded at about 1 second), a peer `HELLO` arriving exactly in this interval is the
normal path.

> **Why.** A separate state for the interval would add one column to the transition table, and that
> column's rules would equal `PUNCHING`. So instead of adding a state, the entry point was moved earlier.

The punch deadline (Chapter 11) counts from the **actual local punch start**, not from `PUNCHING` entry. This keeps the waiting interval
from eating into the deadline.

### 9.2 CONNECTED Condition

```text
got_ack   = received a HELLO_ACK echoing our nonce               (our -> peer path confirmed)
sent_ack  = sent a HELLO_ACK in reply to the peer's valid HELLO   (peer -> our path confirmed)

CONNECTED  <=>  got_ack && sent_ack
```

`sent_ack` is set only after `sendto` returns success. On failure, increment `tx_err_ack` and do not set it.

Only one of the two set is `HANDSHAKING`. **Which one rises first is not defined.** Assuming an order either misjudges a one-way-open path as connected, or makes both sides time out in a normal order.

On entering `CONNECTED`, initialize the loss accounting state (4.5).

### 9.3 Epoch Rules

| Situation | Action |
|------|------|
| Valid `HELLO` or nonce-matching `HELLO_ACK` received while the epoch is unpinned | Pin that packet's epoch |
| Packet with the same epoch as the pinned one | Normal processing |
| `HELLO` with an epoch different from the pinned one | **The peer retried.** The 9.5 Renegotiation procedure |
| Any other packet with an epoch different from the pinned one | Drop, `drop_stale_epoch` |
| Epoch on the retired list | Drop, `drop_retired_epoch` |

### 9.4 Transition Table

The table below covers the **three active states**. `IDLE` and the terminal states are handled by the section after the table.

| Received \ State | `PUNCHING` | `HANDSHAKING` | `CONNECTED` |
|---|---|---|---|
| `HELLO` (epoch matches or unpinned) | Reply `HELLO_ACK` **to the source of that datagram**, `sent_ack=true`, learn the source (10.4 conditions) | Same (if duplicate, only re-reply `HELLO_ACK`) | Same. **The peer's ACK was lost; ignoring it makes the peer time out** |
| `HELLO` (different epoch) | 9.5 renegotiation | 9.5 renegotiation | 9.5 renegotiation |
| `HELLO_ACK` (attempt nonce echo, passed 8.2) | `got_ack=true`, learn the source (10.4 conditions), pin the epoch if unpinned | Same | Duplicate. Increment `dup_ack` and leave the flags as they are. **The idle timer refresh and source learning still happen** (it is a packet that passed every validation) |
| `HELLO_ACK` (probe nonce echo) | Path validation of 10.4 (c). Validates only that tentative path; does not touch `got_ack` or the epoch | Same | Same |
| `HELLO_ACK` (otherwise) | Drop, `drop_bad_nonce` or `drop_stale_epoch` (8.2) | Same | Same |
| `KEEPALIVE` | Learn the source (10.4 conditions). There is no idle timer | Same | Refresh the idle timer, learn the source (10.4 conditions) |
| `PING` | Reply `PONG` | Same | Same |
| `PONG` | 5.5 matching | Same | Same |
| `DATA` | Drop, `drop_data_early` | Drop, `drop_data_early` | Inject after 8.4 validation |
| `CLOSE` | Transition to `CLOSED` | Transition to `CLOSED` | Transition to `CLOSED` |

**A `HELLO_ACK` arriving first in `PUNCHING` is normal.** The peer started punching first, our `HELLO` arrived, but the peer's `HELLO` has not yet.

**A caveat on the `PING`/`PONG` rows.** `PING` transmission starts from `CONNECTED` (Chapter 11). Before that,
`pending_pings` is empty, so an arriving `PONG` fails the 5.5 lookup and ends as `drop_unmatched_pong`.
That is, **no RTT sample is produced before `CONNECTED`.** Answering `PING` with `PONG`
is done in the earlier states too, but a `PONG` to an unverified destination uses the common budget below.

**Replies go to the source of the received datagram, not to `peer_endpoint`.** Same reason 9.5 Renegotiation gives
for the renegotiation reply. It is one reply with no amplification, so it is safe, and it reaches the peer even when
its path changed. Switching `peer_endpoint`, by contrast, has to pass the conditions of 10.4 Endpoint Learning. A single reply and
a bulk switch are different decisions.

**Transmissions to unverified destinations share a common budget.** One forged `HELLO` yields one reply, so
there is no amplification, but if the attacker keeps sending packets with the same epoch, that many transmissions go
to the forged address. In `CONNECTED` there is no end point either.

An **unverified destination** is an address that is neither `peer_endpoint` nor in the approved candidate set. Transmissions
to such addresses are of three kinds, and **all three draw from one budget.**

| Transmission | Trigger |
|------|------|
| `HELLO_ACK` | `HELLO` from a source outside the candidate set (9.4, 9.5 step 6) |
| `PONG` | `PING` from a source outside the candidate set |
| Validation `HELLO` | New tentative path of 10.4 (c) |

**The budget is a token bucket.** Capacity `UNVERIFIED_TX_BUCKET` (10), refill `UNVERIFIED_TX_REFILL` (5) per second, one per session. Each time one of the three above is sent,
one token is spent; with no token, **do not send** and increment `drop_unverified_tx`. Transmissions to the candidate set
or to `peer_endpoint` spend no token.

**Separate budgets for the three paths get the combined cap wrong.** One `HELLO` from a source outside the candidate set can
trigger a `HELLO_ACK` and a tentative-path validation `HELLO` at the same time, so counting "one reply" per path
underestimates what actually goes out. Bundled into one, the total a session sends to unverified addresses is
fixed at once: **5 packets per second, 10 in a burst.**

"5 per second" must not be counted in a fixed window. **Normal retransmission is exactly 5 per second** (200ms interval),
so the 5th would be blocked at the boundary. A bucket of capacity 10 keeps passing those 5 and also absorbs short bursts.
The case where rebinding moved the peer's source outside the candidate set uses exactly this path, so not blocking
the normal path matters.

**The `HELLO_ACK` of a 9.5 renegotiation uses the same budget.** With no token it is not sent, and since it was not sent,
**`sent_ack` is not set** (9.2 `CONNECTED` Condition sets it only after `sendto` success). 9.5 step 7 derives the state at that moment
from the flags, so nothing extra needs deciding. An exception that bypasses the budget would itself become
the bypass.

**Why the budget is per session, not per source.** Sources can be forged, so per-source
buckets are bypassed by an attacker who changes the address. The price is that while the attacker drains the budget, the real
peer's new path gets no reply in that interval. It is the same kind of accepted limitation as tentative-path exhaustion in 10.4 (c),
and even then, paths to the candidate set and to `peer_endpoint` are not limited.

**The idle timer starts on entering `CONNECTED` (Chapter 11).** Earlier states have no timer to refresh.
That is why the `PUNCHING`/`HANDSHAKING` cells of the table do not say "refresh the idle timer". The deadline in that interval
is the punch deadline, not the idle timeout.

**In `CONNECTED`, only a packet that passed every validation for its type refreshes the idle timer.**
The notation in Chapter 11 Timers means the same.

- 8.1 Common Checks through 8.3 Duplicate Suppression apply to every type
- `DATA` additionally goes through 8.4 `DATA` Inner Validation
- `PONG` additionally goes through the `ping_id` match of 5.5 `PING` / `PONG`
- `CLOSE` additionally goes through the reason value check

Before `CONNECTED` there is no timer to refresh, so this sentence does not apply there. Valid packets in
that interval are used only for state transitions and source learning.

### 9.4.1 Receiving in `IDLE` and the Terminal States

| State | Handling of arriving packets |
|------|--------------------|
| `IDLE` | The peer `peer_id` is not yet known, so everything falls to `drop_unknown_peer` at check 7 of 8.1. No separate rule needed |
| `FAILED` | Drop everything regardless of type, `drop_terminal_state` |
| `CLOSED` | Drop everything regardless of type, `drop_terminal_state` |

**Terminal states do not accept `HELLO` either.** A retry, as 9.6 Failure Transitions defines it, starts from `IDLE` with a new epoch and a new
nonce; it is not a finished session coming back to life. If a session in a terminal state answered a `HELLO`,
it would have to revive routing and counters that were already cleaned up, and no document defines
that path. **Dropping is the defined behavior.**

The socket is kept until process exit (Chapter 6), so packets keep arriving after a terminal state is reached.
`drop_terminal_state` continuing to rise is normal; it means the peer has not yet received our `CLOSE` or
is waiting out its idle timeout.

### 9.5 Renegotiation

Receiving a `HELLO` with an epoch different from the pinned one means the peer started a new attempt.

**Check the caps first.** Renegotiation can be triggered by a single forged `HELLO` (Chapter 2), and each one fills one slot of the retired list
and rolls the punch deadline back. Without caps the deadline keeps being pushed, and the session neither reaches `CONNECTED`
nor ends in failure.

| Condition | Action |
|------|------|
| Within `MIN_RENEG_INTERVAL_MS` (1000ms) of the **last accepted** renegotiation | Drop, `drop_reneg_rate` |
| Renegotiation count of this attempt already at `MAX_RENEGOTIATIONS` (8) | Drop, `drop_reneg_limit` |

**The count does not recover until the attempt ends. The consequence is stated here.** If forged `HELLO`s exhaust the 8,
then **even the real peer's restart is dropped with `drop_reneg_limit`.** That session hits the punch deadline,
ends per 9.6, and the only recovery is a new attempt from `IDLE`. This is an availability limitation v1 accepts.
Recovering would need a rule like "count only renegotiations from a verified source", and source verification
is exactly what this protocol does not yet have.

**The normal path does not hit these caps.** Once a renegotiation happens, step 4 pins the new epoch, so
`HELLO` retransmissions of the same attempt are no longer "a different epoch" and are handled as ordinary `HELLO`s. The interval
limit catches only the case where yet another epoch arrives within 1 second, and even that gets through on the peer's 200ms retransmission
1 second later. That is, a normal retry on the same socket is delayed by at most 1 second and does not fail.

**A process restart is different.** That case follows the limitation of 10.4 Endpoint Learning regardless of
these caps, and v1 does not guarantee recovery.

**The reference time is the time of the accepted renegotiation.** A `HELLO` dropped by the caps does not update
that time. If it did, an attacker could extend the cooldown forever just by continuing to send, blocking even
a normal retry.

Once both caps pass, increment the renegotiation count by 1 and atomically perform the following.

1. Register **the peer's** previous epoch on the retired list for 2 minutes
2. Reset `got_ack` and `sent_ack` to false
3. Reset the duplicate suppression state, `pending_pings`, and the loss accounting
4. Pin the peer epoch to the new value
5. **Keep our own `session_epoch` and nonce unchanged**
6. Send a `HELLO_ACK` for the received `HELLO` directly to the source of that datagram. Do not change
   `peer_endpoint`. **If the send succeeds, `sent_ack = true`.** The rule of 9.2 `CONNECTED` Condition
   applies as is
7. Reset the punch deadline to 10 seconds from this point. **The state is determined by the flag count of
   9.2 `CONNECTED` Condition.** If step 6 succeeded it is `HANDSHAKING` with only `sent_ack` set; if it
   failed it is `PUNCHING` with both false
8. Reuse the existing candidate list without going through the control plane again
9. The `HELLO` retransmission target is **the whole existing candidate set**. If the source of the
   `HELLO` that triggered the renegotiation is outside the candidate set, only register it as a
   tentative path of 10.4 (c); do not add it to the retransmission target
10. **Restore the timers of the punch interval.** Restart `HELLO` retransmission (200ms) and stop
    `KEEPALIVE`, `PING`, and the idle timeout. The three start again on the next entry into `CONNECTED`
    as Chapter 11 Timers defines

The basis for the ten rules above follows, in rule number order.

If the resets of steps 2~3 are skipped, a `got_ack` or `sent_ack` obtained under the previous epoch remains and makes the new session `CONNECTED` without verification.

Step 5 is the crux. If we also changed our epoch in reaction to the peer's retry, the peer would
recognize that as another retry and change again, and **the two peers would exchange epochs forever
and never connect.** The local epoch changes only when we start a new attempt from `IDLE`.

Not changing `peer_endpoint` in step 6 matters. The source of the `HELLO` that triggered the
renegotiation may not be in the candidate list, and switching right there would let a single forged
retry `HELLO` redirect all traffic to an arbitrary address. That would bypass the path validation of
10.4 Endpoint Learning.

**Single replies and bulk switches are distinguished.**

- Sending a `HELLO_ACK` back to the source of the received datagram is safe. Even if it goes to a forged
  address, it is one reply with no amplification, and in a legitimate NAT rebinding the peer receives it
- Switching `peer_endpoint` means every subsequent `DATA` goes there, so it has to pass the 10.4 (c) path
  validation. If the source is inside the candidate set, it switches automatically on an advancing packet
  per 10.4 Endpoint Learning; if outside, only after path validation

**Why step 7 does not name the state directly.** 9.2 `CONNECTED` Condition defined the state as a value
derived from the flags.
Step 6 sends a `HELLO_ACK` for a valid `HELLO`, so if that send succeeds `sent_ack` is set,
and then the state is `HANDSHAKING` by definition. Fixing `PUNCHING` here would make **the same single packet
point at two states.** What renegotiation does is erase the evidence and reset the deadline; the state
follows as a result.

**Why step 9 is needed.** The 10.4 Endpoint Learning rule "to all candidates before learning" does not hold
here. Renegotiation erases the evidence while keeping `peer_endpoint` (step 6). So a state arises where the equation "punching = before learning"
is broken, and if retransmission went only to the single learned `peer_endpoint`, a retry in which the peer's path
changed would fire only at a dead address. Sending to the whole candidate set covers that case.

**The reason for not adding the triggering source to the retransmission target is reflection.** That source may be
forged. If it were added, `HELLO`s would go every 200ms to the forged address — an arbitrary third party — for the punch
interval, adding up to dozens. The tentative path of 10.4 (c) sends the validation `HELLO` **only once** and has a cap
(`MAX_PROBE_PATHS`), so it achieves the same purpose without reflection.

**Without step 10, a renegotiation that came from `CONNECTED` fails without fail.** Chapter 11 sets the
cancellation trigger of `HELLO` retransmission at reaching `CONNECTED`, so if retransmission is not
restarted, not one of our `HELLO`s goes out after the renegotiation. Then the peer never gets `sent_ack`
and we never get `got_ack`, and both hit the punch deadline. Step 9 defines only the retransmission
**target** because it assumes that retransmission is running again, and that assumption is nailed down here.

**Stopping the three has the same basis.** 9.4 Transition Table sets `PING` to start from `CONNECTED`, so
if it is not stopped, samples appear in `HANDSHAKING` and the statement in [`architecture.md`](architecture.md)
chapter 9 that "no RTT sample is produced before `CONNECTED`" breaks. The idle timeout is worse. 9.4
Transition Table does not refresh that timer in `HANDSHAKING`, so if it is not stopped and less than 10
seconds of idle time remained just before the renegotiation, `TUNNEL_DROPPED` fires before the punch
deadline and **a session under establishment is recorded as a disconnect.**

### 9.6 Failure Transitions

| Condition | Recorded code |
|------|-----------|
| The STUN server list is exhausted without responses from two different servers ([`architecture.md`](architecture.md) 3.5) | `STUN_DISCOVERY_FAILED` |
| `get_peers` polling deadline exceeded, control plane error, or `peer_id` collision | `CONTROL_PLANE_EXCHANGE_FAILED` |
| Neither `got_ack` nor `sent_ack` set by the punch deadline | `HOLE_PUNCH_TIMEOUT` |
| Only one set by the punch deadline | `PEER_HANDSHAKE_FAILED` |
| Idle timeout after `CONNECTED` | `TUNNEL_DROPPED` |

**Three things happen on the way to `FAILED`.**

1. Stop `HELLO` retransmission and every remaining session timer
2. If the session learned `peer_endpoint`, send `CLOSE` `0x03` once to that address. The state afterwards
   is `FAILED`, not `CLOSED`
3. Write the record (`architecture.md` chapter 9)

`FAILED` is a terminal state. A retry starts as a new attempt from `IDLE` and draws a new epoch and a new nonce.
**What the process does after that is decided by `architecture.md` 3.2.7 Shutdown.**

The basis for the three rules above follows.

**Rule 1 does not conflict with the rule inside a live attempt.** The statement in 10.3 Punch that "the only
trigger that stops retransmission is reaching `CONNECTED`" is a rule inside a live attempt, and when the
attempt ends the retransmission ends too. If it did not stop, a finished session would keep firing at the
whole candidate set every 200ms.

**The reason in rule 2 is `0x03`.** The table of 5.6 `CLOSE` reserves that value for failure.
Using `0x01` (user cancel) would make it indistinguishable from a person switching off in the peer's record.

**Rule 2 exists for the same reason as 5.6 `CLOSE`.** When only the `HELLO_ACK` was lost, the peer is already `CONNECTED`
and we end with `PEER_HANDSHAKE_FAILED` after learning. Without notice, the peer waits out the 50-second
idle timeout. The `FAILED` row of the 5.6 table describes **the shutdown procedure after already entering
that state**, while this is the transition into it. Not sending before learning is also the same as in 5.6 `CLOSE`.

---

## 10. Candidates and Endpoints

### 10.1 Candidate Collection and Hygiene

Candidates each peer registers with the control plane:

- **Local candidates**: `active IPv4 interface address : the bound UDP port`. An endpoint, not just an
  address
  - The port is the single actual value Chapter 6 read with `getsockname`, used for every local candidate
    alike. One socket, so one port
  - Exclude loopback, APIPA (`169.254.0.0/16`), our own Wintun adapter, and other tunnel/VPN adapters from
    the interfaces
  - Also read each interface's prefix length (netmask) and keep it locally. The prefix is not sent as a
    candidate
  - The prefix is kept because the source hygiene of Chapter 7 uses that value to compute the directed
    broadcast of our own subnet. Discarding it makes that decision impossible. Sending it to the peer
    would only reveal our internal network layout, and the peer has no use for it
- **Server-reflexive candidates**: the public endpoint obtained via STUN. Recorded per STUN server queried.

**Apply the hygiene rules below to the received candidate list. Not optional.**

| Rule | Basis |
|------|------|
| At most `MAX_CANDIDATES` (8). Discard the excess | An unbounded list means unbounded transmission |
| Reject broadcast, multicast (`224.0.0.0/4`), unspecified (`0.0.0.0`), and loopback (`127.0.0.0/8`) addresses | Reflection and amplification prevention |
| Reject port 0 | |
| Deduplicate | |

As stated in Chapter 2, without these rules the client becomes a reflection tool that sends a packet every 200ms to a third party chosen by the attacker.

**This table applies only to the candidate list.** The source of a received datagram gets the separate source hygiene of Chapter 7,
and the two tables differ on loopback. The reason is written in Chapter 7.

### 10.2 Rendezvous

If the punch starts are misaligned, one side's packets are all discarded at the peer's NAT.

1. Both peers complete `register_candidate`.
2. `get_peers` returns `not_ready` until both have registered.
3. **The moment the second registration completes, the server fixes `punch_delay_ms = 1000` once per
   room and stores it.** Every later `get_peers` response returns the stored value as is. Recomputing
   per response gives the two peers different times.
4. The response carries `elapsed_since_ready_ms` (time elapsed on the server since ready) along with
   `punch_delay_ms`. **No absolute time is used.** There is no guarantee client clocks are
   synchronized.
5. The client polls `get_peers` at 500ms intervals. Up to 60 seconds.
6. On receiving a ready response, start the punch after `max(0, punch_delay_ms - elapsed_since_ready_ms)` milliseconds. If negative, start immediately.

This works even if the peer launches its client 50 seconds late. The earlier side keeps polling, and the moment readiness occurs both compute the delay from the same reference point.

**Bound on the residual skew.** The difference between the two peers' actual punch start times is bounded
by `polling interval (500ms) + the difference in delivery delay of the two responses`.

- The time from the server's readiness decision until each side receives its response is not reflected in
  `elapsed_since_ready_ms`
- Under typical conditions this is under 1 second, and the 10-second punch interval with 200ms
  retransmission absorbs it comfortably
- In an environment that exceeds this bound (e.g. response delays of several seconds) the punch interval
  has to be extended. This document is updated in that case

### 10.3 Punch

- From punch start, send `HELLO` **to all of the peer's candidates** at 200ms intervals
  - Inside a live attempt, the only trigger that stops retransmission is reaching `CONNECTED`
    (Chapter 11 Timers). Do not stop when the state changes to `HANDSHAKING`. Stopping with evidence in
    one direction only removes the means to obtain the remaining evidence
  - When the attempt ends, the retransmission ends too. 9.6 Failure Transitions stops this timer, and
    9.5 Renegotiation restarts it (step 10)
- The punch deadline is **10 seconds from the actual local start time**. Even when the scheduled time has already passed and the punch started immediately, the deadline is 10 seconds from that point.
- If `CONNECTED` is not reached by the deadline, record failure per 9.6 Failure Transitions.

### 10.4 Endpoint Learning

v1 uses **learning** instead of fixed nomination.

```text
peer_endpoint = source endpoint of the most recent packet satisfying all of the conditions below

  (a) passed every validation for its type
      (8.1~8.3 common + 8.4 for DATA, 5.5 matching for PONG, reason validation for CLOSE)
  (b) **advanced** highest in 4.5 Duplicate Suppression and Loss Accounting (reordered accepted packets are excluded)
  (c) the source is in the approved candidate set, or passed path validation (below)
```

**What this section decides is the destination of the `DATA`, `KEEPALIVE`, and `PING` we send first.** Those three
go to the learned `peer_endpoint`, or to the whole candidate set if not yet learned.

**Replies are not decided by this section.** Replies go to the source of the received datagram and are
owned by 9.4 Transition Table. The remaining transmissions are each decided by another section and are
exceptions to this rule.

| Transmission | Destination | Decided by |
|------|--------|-----------|
| `DATA`, `KEEPALIVE`, `PING` | `peer_endpoint`. The whole candidate set before learning | This section |
| `PONG` (reply) | Not decided by this section | 9.4 |
| `HELLO` retransmission (punch) | **The whole candidate set.** Continues until `CONNECTED` is reached. Independent of learning | 10.3 |
| `HELLO` retransmission (after renegotiation) | The whole candidate set | 9.5 step 9 |
| `HELLO_ACK` (reply) | Source of the received datagram | 9.4, 9.5 step 6 |
| Validation `HELLO` (probe) | The address of that tentative path | 10.4 (c) |
| `CLOSE` | The learned `peer_endpoint`. Not sent before learning | 5.6 |

**Why `HELLO` retransmission is not narrowed to `peer_endpoint`.** Learning is only evidence that one path is alive,
and the purpose of the punch is **to open paths that are not yet open.** Narrowing retransmission to one address after learning
removes the alternative when that path dies.

The criterion for learning is not the state. Being in `PUNCHING` alone does not make the target the whole candidate set.
9.5 renegotiation **keeps `peer_endpoint` and erases only the round-trip evidence**, so sessions exist that have a learned value
and yet went back to before `CONNECTED`. The `HELLO` retransmission target in that case is decided separately
by 9.5 step 9. The state name at that time is derived from the flags by 9.5 step 7, so no particular
state is assumed here.

**Without (b)**, an old packet accepted within the 64-slot acceptance window of 4.5 Duplicate
Suppression and Loss Accounting would revert the endpoint to a previous path. Using only advancing
packets for learning prevents the reversion.

**Path validation of (c).** When a packet arrives from an address not in the candidate set, do not
switch immediately. The legitimate case of a port changed by NAT rebinding and an attack with a
forged source cannot be told apart.

```text
1. If the tentative path table is full at MAX_PROBE_PATHS(4), skip only the path registration and drop_probe_full.
   The original packet continues to be processed per the 9.4 transition table. It is not dropped.
   Do not evict the oldest entry.
2. Take one token from the common unverified-destination budget of 9.4 transition table. If none, stop here and
   increment drop_unverified_tx. Do not create a tentative path.
3. Record the new address as a tentative path. Leave peer_endpoint as is.
4. Send one HELLO carrying a probe nonce to that address (5.1). Do not retransmit.
   The token taken above is spent here.
5. When a HELLO_ACK echoing that probe nonce arrives from the same address, switch peer_endpoint then.
   This ACK does not change the handshake state (got_ack, epoch).
6. If it does not arrive within 5 seconds, discard the tentative path and increment path_probe_failed.
7. The table slot is **freed only after the full 5 seconds, regardless of outcome.** Even on a successful switch,
   the slot is not returned immediately. If success freed the slot at once, an attacker who can actually receive at the address
   could answer every probe and keep rotating slots, defeating the cap above.
```

**Even when stopping because the table is full or no token is available, the original packet is not
dropped.** That packet has already passed 8.1 Common Checks through 8.3 Duplicate Suppression and advanced
`sequence`; it is a legitimate packet and continues to be processed as the 9.4 transition table dictates
for that state and type.

- Idle timer refresh and `DATA` injection happen only in the states that table allows. This means the
  procedure stopping does not change that processing; it does not mean everything is performed regardless
  of state
- What is skipped when stopping is only raising the new path as a validation target. The result is that
  the `peer_endpoint` switch is delayed
- What the name `drop_probe_full` counts is also "paths not registered", not "dropped packets"

> **Why.** Dropping the original packet would leave the idle timer unrefreshed, and a live session could
> be judged disconnected.

**Why nothing is evicted when the table is full.** If the oldest were evicted, the side sending forged sources quickly
could push the real new path out of the table. The one filling the table is the attacker, so **protecting what came first is
safer.** The price is that validation of the real path is delayed.

**That price is stated exactly. It is not "it clears after 5 seconds".** If the attacker refills slots as soon as they free up,
that session never validates a new endpoint. That is, sustained source forgery can block that
session's path-switching ability indefinitely, and if the path really had changed, it ends with `TUNNEL_DROPPED` after the
50-second idle timeout. This is a limitation v1 accepts.

> **Why.** Fairness would require a per-source cooldown, but per-source fairness does not hold against a
> peer who can forge sources.

**The reflection cap is not set separately by this section.** The validation `HELLO` is one of the three transmissions to
unverified destinations, and their total is set by the common budget of 9.4 Transition Table. **5 packets per second per session, 10 in a burst.**
The number is not repeated here. Counting per path hides the sum of the three paths.

The constraints this section adds are **4 concurrent tentative paths and 1 validation `HELLO` per path**. Those two
set the table size and whether to retransmit; they do not set the transmit rate. The "block paths that harm third parties" promised in Chapter 2
is kept here by the common budget, not by a complete block.

**Precondition of this procedure and the measured limitation.** This whole procedure assumes that packets
actually arrive from an address outside the candidate set. Measurements found a case where that
assumption does not hold.

- 3 network pairs were measured. Windows↔Windows, Windows↔macOS, Windows↔Linux
- In all three pairs, UDP with **the same source IP and only a different source port** was blocked in
  both directions (6 cases, 0 of 20 shots arrived in each)

**The most likely explanation is port-restricted filtering at the NAT.** The result was the same on macOS
and Linux, where no host firewall filtering appeared to be present. However, this is not asserted. Three
things were not confirmed.

- The macOS side is an inference from defaults and was not queried directly
- On the Linux side only `ufw` being off was confirmed. Without root the full `nft`/`iptables` rule set
  was not seen
- No packet capture was taken on either side

**Therefore v1 does not guarantee that (c) recovers from NAT rebinding.** Precisely: it does not guarantee
recovery for endpoint changes consistent with the tested behavior (only the source port changed). Actual
rebinding was not induced and tested, so it is not claimed that "it fails on every rebinding".

In the non-guaranteed cases the following happens.

- This procedure does not start, and for the same reason the 9.5 renegotiation gets no trigger either
- Both sides reach the idle timeout (50s) and the session ends with `TUNNEL_DROPPED`
- If a retry is made, as 9.6 Failure Transitions defines, it starts from `IDLE` with a new epoch and a new
  nonce and goes through the control plane again

**Who initiates the retry, and when, is not decided by this document.** Whether to retry automatically is undecided.

**A process restart hits the same failure.** Per Chapter 6 the socket binds to port 0, so the restarted
side gets a new local port and therefore a new NAT mapping. On the surviving side it arrives as **same IP, different
port**, which is the blocked case measured above. The surviving side does not go through the control plane again per 9.5 step 8,
so it has no way to learn the peer's new endpoint either. In summary:

| What changed | Does 9.5 renegotiation recover? |
|-----------------|-------------------------|
| New attempt on the same socket (only the epoch changed) | **Yes.** The source is unchanged, so the `HELLO` arrives |
| Socket changed (process restart, rebinding) | **No.** If the `HELLO` from the new source is blocked as measured above, the renegotiation trigger itself never occurs |

Therefore **`session_epoch` and 9.5 Renegotiation are devices for retries on the same socket; they do not guarantee
restart recovery.** Even if the restarted side retries alone, it does not reach the peer while the peer's session
is alive.

**The two sides leave different codes.** The "both sides reach the idle timeout" in the rebinding description above is
the case where both remain in the same session; a restart is not that.

| Side | What it hits | Recorded code |
|----|-----------------|----------------|
| Surviving side | 50-second idle timeout after establishment | `TUNNEL_DROPPED` |
| Restarted side | 10-second punch deadline of the new attempt | By the flag count of 9.6 Failure Transitions. `HOLE_PUNCH_TIMEOUT` if neither is set, `PEER_HANDSHAKE_FAILED` if only one is |

To recover, both sides have to start a new attempt from `IDLE`, and the surviving side reaches that point only after waiting out
the 50 seconds above. After that comes the undecided item
noted above (automatic retry). When 4.3 `session_epoch` says it blocks "delayed packets from a previous attempt after a restart or retry",
it means **filtering delayed packets**, not reconnecting a restarted session.

There is a method where the surviving side re-polls the control plane and receives the peer's new candidates. v1 does not
adopt it. Its precondition, "`peer_id` and virtual IP are preserved on rejoin", was fixed as contract 7 of Chapter 14, so
the reason for not adopting it is not a missing precondition but that **who initiates re-polling, and when**, is undecided,
just like the automatic retry above.

(c) is not removed. There are two reasons.

- Its security role as **defense against forged sources** remains. Whenever a packet arrives from an
  address outside the candidate set, that validation is needed
- **On a NAT that uses address-restricted (restricted cone) filtering, the packet can arrive.** These
  observations are all merely consistent with a port-restricted NAT; that does not make it confirmed

The reasoning and the discarded alternatives are in [`decisions/0002-no-rebinding-recovery.md`](decisions/0002-no-rebinding-recovery.md).

An attacker who forges the source does not receive the `HELLO` of step 2 and so cannot pass step 3.
This is how the "block paths that harm third parties" promised in Chapter 2 is kept under the
learning approach. Without this procedure, anyone who knows the session identifiers could forge the
source and redirect all game traffic to an arbitrary victim.

There are three reasons for not using nomination.

- **Behind the same NAT**: if the two peers each nominate the LAN path and the hairpin public path, they lock onto different endpoints and a deadlock results in which every peer packet is filtered.
- **Asymmetric nomination**: if one side receives on a LAN candidate and the other sends to a public candidate, the same deadlock results.
- **NAT rebinding**: the epoch stays the same even when the mapping changes, so under a fixed approach
  there is no renegotiation trigger and one has to wait for the idle timeout
  - This reason was weakened by measurement. The learning approach too fails to guarantee recovery for
    rebinding consistent with the tested behavior (only the source port changed and caught by the
    observed filtering) (see the measured limitation of (c) above)
  - That is, it does not separate the two approaches. Rebinding itself was not induced and tested, so it
    is also not claimed that this reason is void for every rebinding

The other two reasons remain valid, so the learning approach itself is kept.

The learning approach uses **the path that most recently actually delivered**, so the first two cases
(behind the same NAT, asymmetric nomination) converge automatically. Duplicate suppression (4.5) filters
old packets first, so delayed packets from a previous path cannot revert the endpoint.

**Rebinding is different.** 10.4 (c) path validation starts only when a packet actually arrives from an
address outside the candidate set; if none arrives, there is no convergence.

When the endpoint changes, increment the `endpoint_learned` counter. If this value keeps rising, the path is oscillating; use it for diagnosis.

### 10.5 Security Consequences

Thanks to the path validation of 10.4 (c), an attacker who knows the session identifiers and **forges only the source to redirect the whole `DATA`
flow to a third party** is blocked even in v1. The validation `HELLO` goes to the forged address, so
the attacker cannot answer, and without an answer `peer_endpoint` does not switch.

**Blocking and limiting are distinguished, and so are their owners.** Bulk switching of the `DATA` flow is blocked by the path validation of
10.4 (c). The validation `HELLO` itself still goes out to the forged address, and **its volume is
limited by the common unverified-destination budget of 9.4 Transition Table.** What 10.4 (c) sets is the number of concurrent tentative paths and
the transmissions per path, not a rate cap. The numbers are written only in 9.4 Transition Table.

What is not blocked still remains. If the attacker uses an address that can actually receive packets
(its own address), it passes path validation and can pull the traffic to itself. Reading and
tampering with the content is not blocked either. Under the premise of Chapter 2, v1 does not block
these, and when encryption and peer authentication are introduced, a MAC has to be attached to path
validation.

---

## 11. Timers

**Session timers.** "Session end" means every way of leaving `CONNECTED`. 9.5 Renegotiation (step 10),
9.6 Failure Transitions, and sending or receiving `CLOSE` all belong here.

| Timer | Value | Start | Reset trigger | Cancel |
|--------|-----|------|-------------|------|
| STUN retry | 500ms, 1s, 2s (3 times) | STUN request sent | None | Response received |
| STUN deadline | 5s. **Per server** | First request sent to that server | None | Response from that server |
| `get_peers` polling | 500ms interval | **Successful `register_candidate` response** | Each response | Ready response |
| `get_peers` deadline | 60s | **Successful `register_candidate` response** | None | Ready response |
| Punch delay | Value computed in 10.2 Rendezvous | Ready response | None | Punch starts on expiry |
| `HELLO` retransmission | 200ms interval | Punch start, **9.5 renegotiation (step 10)** | Each send | `CONNECTED` reached, 9.6 failure transition |
| Punch deadline | 10s | **Actual local punch start** | 9.5 renegotiation | `CONNECTED` reached |
| `KEEPALIVE` | 15s interval | `CONNECTED` entry, **after sending once immediately** | Each send | Session end |
| Idle timeout | 50s | `CONNECTED` entry | Packet received that passed every validation for its type (9.4) | Session end |
| `PING` | 5s interval. **Nothing is sent on entry. The first send is 5s after entry** | `CONNECTED` entry | Each send | Session end |
| `pending_pings` cleanup | Every time before insertion | - | - | - |

**Consequence of the STUN deadline being per server.** When one server hits its deadline, the query moves
to the next server on the list (that server's retry schedule and deadline start afresh), and failure is
decided only after the list is exhausted. The selection and replacement rules belong to
[`architecture.md`](architecture.md) 3.5 Startup Inputs, and the failure condition of 9.6 Failure
Transitions points at that section. **The bound on the whole STUN stage is `5s × ceil(list length / 2)`.**
Two servers are queried at once. With the default list of 4, that is 10 seconds.

**The expiry values below are not in the timer table, yet they do expire by time.** They are not one
timer per session but a lifetime attached to each table entry, so they are not in the table above. An
implementation has to handle these too. **This table is an index. The source of each value is the section
in the "Where" column.**

| What | Value | Where |
|------|-----|------|
| Retired list entry | 2 minutes (the cap comes first; best-effort) | 4.3 |
| Attempt nonce retired list entry | 2 minutes | 5.1 |
| Probe nonce and tentative path slot | 5 seconds (held for the full time regardless of outcome) | 5.1, 10.4 (c) |
| `pending_pings` entry | 5 seconds | 5.5 |
| Minimum renegotiation interval | `MIN_RENEG_INTERVAL_MS` (1000ms) | 9.5 |

Deadline comparisons use **strict inequality**. Priority is based on the time dequeued. A receive event
dequeued in the same iteration is processed before that iteration's timer expiries.

The reason it is the dequeue time and not the arrival time is that the receive loop caps the number of
datagrams processed per iteration (`architecture.md` 3.2.3 One Loop Iteration). Draining without a cap
would starve the timers entirely.

In exchange, a datagram that arrived just before a deadline can be pushed to the next iteration and lose
to the timer. While the loop keeps up with the load this delay is under 1ms, two orders of magnitude
below the shortest deadline (200ms). If the loop persistently cannot keep up, the socket buffer overflows
and packets are dropped. That is overload, not a timer ordering problem.

Why the idle timeout is **50s** and not 45s: a keepalive is sent once immediately on entering
`CONNECTED`, so within 45s there are 4 send opportunities at 0s, 15s, 30s, 45s. At 45s the last send
and the timeout race at the same instant. 50s tolerates 3 losses and leaves room to recover on the
fourth.

15s and 50s are initial values. If measurements of mapping lifetime lead to an adjustment, this document is updated.

---

## 12. Size Limits and Fragmentation

```text
1500 (assumed outer path MTU)
 - 20 (outer IPv4 header)
 -  8 (outer UDP header)
 - 20 (tunnel header)
= 1452 bytes = MAX_INNER
```

The virtual adapter MTU default is **1400**. It leaves 52 bytes of headroom below `MAX_INNER`, so it survives the outer path MTU falling to 1448.

**The DF bit is not set on outer UDP packets.** On Windows `IP_DONTFRAGMENT` defaults to off for UDP, so
simply do not turn it on.

> **Why.** With DF set, an intermediate router silently discards when the path MTU is small, so only
> small packets get through and large ones vanish. Inner TCP falls into a retransmission black hole.
> Login works but world loading stalls: that is the classic symptom.

Full PMTU discovery is outside v1 scope. The default is fixed by measurement and the limitations are
recorded in [`experiments.md`](experiments.md). **Even if the decision is to keep the conservative
fixed value as is, the paths that value cannot cover are written in the same place.** Not deciding a
value and deciding one while recording its limits are different things.

---

## 13. STUN Usage Scope

Of RFC 5389, only the following is implemented.

- **Messages**: Binding Request (`0x0001`), Binding Success Response (`0x0101`), Binding Error Response (`0x0111`)
- **Sent attributes**: none. A Binding Request sends the header only
- **Parsed attributes**: `XOR-MAPPED-ADDRESS` (`0x0020`) only. Other attributes are skipped. For `ERROR-CODE` only the value is logged
- **Transaction ID**: 96 bits from the OS CSPRNG
- **Response validation**: magic cookie matches, transaction ID matches, message length matches the header's length field, abort on boundary overrun while walking attributes
- **Address family**: drop if the `XOR-MAPPED-ADDRESS` family is not IPv4 (`0x01`)
- **Retry**: Chapter 11 table

FINGERPRINT, MESSAGE-INTEGRITY, authentication, TURN, and ICE procedures are not implemented.

---

## 14. Control Plane Schema

The control plane's request/response encoding, error schema, and state transitions are outside the
scope of this document and are fixed by [`control_plane.md`](control_plane.md). The contracts this
document requires of the control plane are only the following, and **where each contract is
satisfied is indexed by section 1.3 of that document (Mapping to the protocol.md Section 14
Contracts).**

1. `peer_id` is unique within a room (4.2)
2. `get_peers` returns `not_ready` until both have registered (10.2)
3. `punch_delay_ms` is fixed once per room when the second registration completes and does not change afterwards (10.2)
4. The `get_peers` response includes `elapsed_since_ready_ms`, and that value is computed from a
   **monotonic clock** (10.2 Rendezvous)
   - Computed from the wall clock, the values the two peers receive diverge the moment NTP steps the
     time, and the skew bound of 10.2 Rendezvous breaks
   - The fallback computation for when the server loses its monotonic reference (instance reboot) is
     defined by `control_plane.md` 7.4 Clocks. In that case the skew bound of 10.2 Rendezvous is not
     guaranteed
5. Only candidates that passed the rules of 10.1 Candidate Collection and Hygiene are stored and forwarded
6. Each peer's virtual IP is told to the other (needed for the `virtual_ip` check of 5.1 `HELLO`)
7. **A rejoining peer is given back the same `peer_id` and the same virtual IP.** If new values were
   given, everything would be dropped on the peer side at check 7 of 8.1 (`drop_unknown_peer`) and the
   `virtual_ip` check of 5.1 `HELLO`, and it could not become a renegotiation trigger either. The
   conditions under which a rejoin holds are defined by that document
8. `peer_id` is drawn as **32 bits from a CSPRNG**. Uniqueness alone would let sequential assignment satisfy the contract, but then the "guess" in "knows or guesses" of Chapter 2 becomes trivial

If any of these 8 contracts breaks, the tunnel protocol does not hold.

---

## 15. Implementation Checklist

Everything below has to be in the code in Phase 4. Phase 4 is the stage that implements this checklist.

- [ ] Every constant in Chapter 3
- [ ] Field-by-field header serialization/deserialization + round-trip test
- [ ] `SO_EXCLUSIVEADDRUSE`, no `SO_REUSEADDR`, `SIO_UDP_CONNRESET` off, no `connect()` call
- [ ] `INADDR_ANY` + port 0 bind, actual port confirmed with `getsockname`, `SO_RCVBUF` set and applied value confirmed (Chapter 6)
- [ ] Exclusive socket ownership by a single receive loop
- [ ] Chapter 7 source hygiene (before classification, `drop_bad_source`) and classification (by top two bits, magic at the validation step)
- [ ] The 8 common checks of 8.1 with dedicated counters
- [ ] 8.2 epoch/source rules (especially epoch pinning by a nonce-valid `HELLO_ACK`)
- [ ] The `virtual_ip` check of 8.2 Epoch and Source Checks (`drop_vip_mismatch`). **Before the epoch decision** (5.1, 5.2)
- [ ] 4.5 duplicate suppression algorithm (`highest` + `position` + 64-bit bitmap), before side effects
- [ ] `seq == highest` checked first, `if/else if/else` mutually exclusive branches, `shift == 64` handling, every bit operation in `uint64_t`
- [ ] The `a != b` condition of `is_newer`
- [ ] Per-session-attempt `session_epoch`, CSPRNG and 0 avoidance, 2-minute retired list and the `MAX_RETIRED_EPOCHS` cap. **What goes on the retired list is the peer's epoch** (4.3)
- [ ] CSPRNG nonce, fixed per attempt, 2-minute retired list. Probe nonces in a separate table
- [ ] Every 10.1 candidate hygiene rule, local interface addresses and prefixes kept (input to the Chapter 7 decision)
- [ ] 10.2 relative-time rendezvous
- [ ] 10.4 endpoint learning (advancing packets only, addresses outside the candidate set after path validation), `MAX_PROBE_PATHS` cap and 1 validation `HELLO`
- [ ] The dual flags of 9.2 `CONNECTED` Condition, `sent_ack` only after `sendto` success
- [ ] The whole 9.4 transition table and 9.4.1 terminal-state drop (`drop_terminal_state`)
- [ ] 9.4 common unverified-destination budget. Per-session token bucket (`UNVERIFIED_TX_BUCKET`,
  `UNVERIFIED_TX_REFILL`), applied to all three of `HELLO_ACK`·`PONG`·validation `HELLO`,
  `drop_unverified_tx`. **Not counted per path**
- [ ] The 10 steps of 9.5 renegotiation. **Especially keeping the local epoch/nonce and the step 10 timer restore**
  - Changing the local epoch/nonce means infinite ping-pong
  - Step 10 is restarting `HELLO` retransmission and stopping `KEEPALIVE`, `PING`, and the idle timeout
- [ ] The two 9.5 renegotiation caps (`MIN_RENEG_INTERVAL_MS`, `MAX_RENEGOTIATIONS`)
- [ ] Chapter 11 timer values and lifecycles
- [ ] `CLOSE` send/receive, reason value validation, the `CLOSED` transition on both send and receive sides, and removing only the routing entry (5.6)
- [ ] The three things of 9.6 failure transitions: stopping retransmission and session timers, one `CLOSE` `0x03` to a learned session, and the record
- [ ] `tx_err_send` on `sendto` failure (Chapter 6). `HELLO_ACK` also `tx_err_ack` (9.2)
- [ ] `pending_pings` cap and cleanup
- [ ] Chapter 13 STUN scope
- [ ] 5.4 `DATA` type, 8.4 inner validation (checks 9~15), 8.5 transmit-side validation (checks 1~6, including the length lower bound)

Added in Phase 5: 4.5 loss accounting and `baseline` (accounting that does not count reordering as loss), fuzz defense.
**The 64-slot acceptance window itself is a Phase 4 item above.** The window and the accounting are different things.
