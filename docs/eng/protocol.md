# Tunnel Protocol v1

**Project:** Direct-First P2P Virtual Network for Multiplayer Games
**Status:** Fixed before implementation. If the code disagrees with this document, the code is wrong.
**Design background:** [`architecture.md`](architecture.md) / **Requirements:** [`spec.md`](spec.md)
**Audit history:** [`design-audit.md`](design-audit.md)

> Korean version: [`../kor/protocol.md`](../kor/protocol.md)

The purpose is that **the implementer has to invent nothing**. Anywhere a value is left open is a defect in this document.

---

## 1. Scope and Assumptions

| Item | v1 fixed value |
|------|----------------|
| Outer transport | IPv4 UDP only |
| Inner payload | IPv4 packets only. Inner IPv6 is dropped and counted |
| Byte order | All multi-byte integers are **big-endian** |
| Peer count | Two peers per room |
| Encryption | None (section 2) |

IPv6 support is v2 or later. v1 opens sockets with `AF_INET` only and resolves STUN servers as IPv4 only.

---

## 2. Security Model

**v1 provides no confidentiality, no integrity, no peer authentication, and no replay protection.** This is an explicit choice, not an omission.

An on-path or off-path attacker who learns or guesses `peer_id` and `session_epoch` can:

- read and modify game traffic
- inject arbitrary packets
- terminate the session with a forged `CLOSE`
- force renegotiation with a forged `HELLO`
- hijack traffic by abusing endpoint learning (10.5)

The inner packet validation in section 8 is **not an attack defense.** It is hygiene against accidental injection caused by misdelivery, bugs, or wrong routing. The earlier revision of this document called those checks "the only injection defense", which was an overstatement.

The basis for this choice is the non-goals in `spec.md`: custom cryptography and authenticated peer sessions are stretch goals, outside v1. The final report must state this limitation.

However, **paths that cause harm to third parties are blocked even in v1.** The candidate list arrives unverified, and abusing it turns the client into a reflection tool that sends packets to an attacker-chosen third party every 200 ms. The candidate hygiene rules in 10.1 are mandatory, not optional.

---

## 3. Constants

```cpp
constexpr uint32_t TUNNEL_MAGIC   = 0x53414E47;  // "SANG"
constexpr uint8_t  TUNNEL_VERSION = 0x01;
constexpr size_t   HEADER_SIZE    = 20;
constexpr size_t   MAX_INNER      = 1452;
constexpr size_t   MAX_DATAGRAM   = HEADER_SIZE + MAX_INNER;   // 1472
constexpr uint32_t STUN_COOKIE    = 0x2112A442;                // RFC 5389
constexpr size_t   MAX_CANDIDATES = 8;                         // per peer
constexpr size_t   REPLAY_WINDOW  = 64;
constexpr size_t   MAX_PENDING_PINGS = 16;
```

The first byte of `TUNNEL_MAGIC`, `0x53`, has `01` in its top two bits, while a STUN message always has `00`. This property is the basis for the classification in section 7 and must be preserved if the magic value ever changes.

---

## 4. Header

### 4.1 Wire layout

```text
 offset  size  field             type        description
   0      4   magic             uint32 BE   fixed 0x53414E47
   4      1   version           uint8       fixed 0x01
   5      1   type              uint8       PacketType (section 5)
   6      2   payload_length    uint16 BE   bytes following the header
   8      4   peer_id           uint32 BE   sending peer identifier
  12      4   session_epoch     uint32 BE   sender's session attempt identifier
  16      4   sequence          uint32 BE   per-direction monotonic counter
  = 20 bytes
```

Every 4-byte field lands on a multiple-of-4 offset, so no padding appears even under MSVC default alignment. Even so, never `memcpy` the struct: byte order conversion is still required and relying on incidental alignment breaks silently. **Serialize field by field** and use the `HEADER_SIZE` constant for the length.

### 4.2 peer_id

Assigned by the control plane at room join. **Unique within a room.** If both peers had the same `peer_id`, identifying the sender and distinguishing self from peer would be impossible, so the control plane never makes such an assignment and a client that finds its own value in a `get_peers` response aborts with `CONTROL_PLANE_EXCHANGE_FAILED`.

### 4.3 session_epoch

Drawn **per session attempt**, not per process. A new value is drawn every time a new attempt starts from `IDLE`, and it does not change for the duration of that attempt. Zero is never used.

Without this field, delayed packets from a previous attempt or process are accepted into the new session. The receiver pins the peer's epoch from the first valid packet and handles differing epochs per section 9.

Being a 32-bit random value, collisions are possible. Retire each previous attempt's epoch into a **retired list** for 2 minutes and drop every packet carrying a retired epoch. This blocks essentially all delayed packets except the case where the same value is drawn again.

### 4.4 sequence

- **One counter per direction, per session attempt.** It does not distinguish packet types.
- Assigned immediately before sending, incrementing from 0 by 1. Retransmissions get a new number.
- It wraps at 32 bits.

```cpp
// is a strictly newer than b. a == b is false.
bool is_newer(uint32_t a, uint32_t b) {
    return a != b && (uint32_t)(a - b) < 0x80000000u;
}
```

Returning true for `a == b` would classify a duplicate as newer and defeat replay suppression.

### 4.5 Duplicate suppression and loss accounting

The receiver keeps the following state per peer epoch.

```cpp
uint32_t highest;      // highest sequence received so far (32-bit, wrapping)
uint64_t position;     // non-wrapping 64-bit extended position of highest
uint64_t seen_bits;    // bitmap for back=1..64; back=b is bit (b-1)
bool     initialized;
```

Received packets are processed in this order. **This decision precedes every side effect.** Idle timer refresh, state transitions, Wintun injection, and `PONG` matching happen only after this step passes.

```text
if not initialized:
    highest = seq; position = 0; seen_bits = 0; initialized = true
    -> accept (advance). pkt_pos = 0

if seq == highest:
    -> drop, drop_duplicate

if is_newer(seq, highest):                         // advance
    shift = (uint32_t)(seq - highest)              // 1 .. 2^31-1
    if      shift >  64:  seen_bits = 0
    else if shift == 64:  seen_bits = uint64_t{1} << 63
    else:                 seen_bits = (seen_bits << shift) | (uint64_t{1} << (shift-1))
    highest   = seq
    position += shift
    -> accept (advance). pkt_pos = position

otherwise (older):
    back = (uint32_t)(highest - seq)               // 1 .. 2^31
    if back > 64                           -> drop, drop_too_old
    if seen_bits & (uint64_t{1}<<(back-1))  -> drop, drop_duplicate
    seen_bits |= uint64_t{1} << (back-1)
    -> accept (no advance). pkt_pos = position - back
```

Four boundaries are easy to get wrong.

- **Filter `seq == highest` first.** `is_newer` is false for `a == b`, so that case falls into the branch below, producing `back == 0` and the undefined shift `uint64_t{1} << (0-1)`. Handle it as an explicit duplicate first.
- **The three branches must be mutually exclusive.** Written as `if / if / else`, a `shift > 64` clears the bitmap and then also enters the `else`, performing a shift greater than 64. Write `if / else if / else`.
- **Do not clear the bitmap at `shift == 64`.** Clearing leaves the previous `highest` unrecorded at position `back == 64`, so that packet is accepted again even though it is a duplicate. `shift == 64` is the case that sets bit 63. `seen_bits << 64` is undefined behavior in C++, so it needs its own branch.
- **Every bit operation uses `uint64_t`.** `1 << 63` is an `int` shift and is undefined behavior.

**Loss accounting is reset on entry to `CONNECTED`.** During punching, `HELLO` copies sent to several candidates each consume a sequence number while only one path actually delivers, so counting those gaps as loss would corrupt the measurement.

The baseline is taken **immediately after** processing the packet that caused the transition.

```text
baseline = position          // value after processing the transition packet
accepted = 0                 // unique packets accepted beyond baseline

on each acceptance: if that packet's pkt_pos > baseline, accepted += 1

expected  = position - baseline
lost      = expected - accepted
loss rate = lost / expected if expected > 0, else 0
```

The arithmetic uses the non-wrapping 64-bit `position` rather than the 32-bit `sequence`, so a full sequence cycle does not corrupt the result.

`pkt_pos` is the updated `position` for an advancing packet and `position - back` for a reordered one. Using the state variable `position` directly would count reordered packets sent before `baseline`, making `accepted > expected` and underflowing `lost`. Excluding reordered packets at or below `baseline` precisely is what prevents that.

Packets dropped as `drop_too_old` were already counted lost and do not reverse the count. A loss rate snapshot is taken every 10 seconds for telemetry.

---

## 5. Packet Types

| Value | Name | Payload length | Purpose |
|-------|------|----------------|---------|
| `0x01` | `HELLO` | 20 | Hole punching and handshake initiation |
| `0x02` | `HELLO_ACK` | 20 | Proves the peer's `HELLO` was actually received |
| `0x03` | `KEEPALIVE` | 0 | Maintains the NAT mapping |
| `0x04` | `DATA` | 20 to 1452 | Carries an inner IPv4 packet |
| `0x05` | `PING` | 8 | RTT measurement request |
| `0x06` | `PONG` | 8 | RTT measurement reply |
| `0x07` | `CLOSE` | 1 | Graceful shutdown notice |

Any value not listed is dropped with `drop_unknown_type`.

### 5.1 HELLO (20 bytes)

```text
 offset  size  field        description
   0     16   nonce        128-bit random value for this attempt
  16      4   virtual_ip   sender's assigned virtual IP (uint32 BE)
```

The `nonce` comes from the **OS CSPRNG** (`BCryptGenRandom`). One per session attempt, reused across retransmissions. Changing it per retransmission makes it impossible to tell which nonce an arriving `HELLO_ACK` answers. Previous attempts' nonces are retired for 2 minutes and never reused.

Consumer of `virtual_ip`: the receiver compares it against the peer's virtual IP as reported by the control plane. On mismatch the packet is dropped with `drop_vip_mismatch`.

**Probe nonces.** The path validation `HELLO` in 10.4 (c) uses a separate **probe nonce**, not the attempt nonce. One is drawn per tentative path and kept in its own table for 5 seconds. The "one per attempt" rule above applies to the attempt nonce only. Mixing the two in one table makes path validation replies indistinguishable from handshake replies.

### 5.2 HELLO_ACK (20 bytes)

```text
 offset  size  field        description
   0     16   echo_nonce   verbatim copy of the nonce from the HELLO being answered
  16      4   virtual_ip   sender's assigned virtual IP (uint32 BE)
```

`virtual_ip` is handled as in 5.1.

### 5.3 KEEPALIVE (0 bytes)

No payload.

### 5.4 DATA (20 to 1452 bytes)

The payload is a **complete inner IPv4 packet**. No Ethernet header, no additional wrapping.

### 5.5 PING / PONG (8 bytes each)

```text
 offset  size  field     description
   0      8   ping_id   uint64 BE
```

`ping_id` starts at 0 for each session attempt and increments by 1. **No timestamp goes on the wire.** The send time lives only in a local table.

```cpp
uint64_t id = next_ping_id++;
pending_pings[id] = steady_clock::now();
```

- `pending_pings` holds at most `MAX_PENDING_PINGS` (16). Entries older than 5 seconds are swept before insertion; if it is still full, no new `PING` is sent.
- On `PONG`, look up by `ping_id`. If absent, drop with `drop_unmatched_pong`. If present, compute the RTT and **remove the entry.** Each `ping_id` is valid exactly once.
- Entries past 5 seconds are removed and their samples discarded.

`PONG` echoes the received `ping_id` verbatim.

### 5.6 CLOSE (1 byte)

```text
 offset  size  field     description
   0      1   reason    0x00 normal shutdown, 0x01 user cancelled, 0x02 configuration error
```

Values outside `0x00` to `0x02` are dropped with `drop_bad_reason`. Sent once best-effort on normal shutdown, never retransmitted, with no reply awaited. The receiver immediately removes the session and its routing entry.

Without `CLOSE`, a normal exit is indistinguishable from a crash and the peer holds a dead session until the 50-second idle timeout. The idle timeout stays in place to cover crashes.

---

## 6. Socket Ownership

**One local UDP socket, owned exclusively by one receive loop.** That loop's thread layout, wait mechanism, and state ownership are in section 3.2 of [`architecture.md`](architecture.md).

- The STUN client never calls `recvfrom` itself. It registers a request and receives the response from the receive loop. Two readers would consume each other's packets.
- The socket is bound once at process start and kept until shutdown. Rebinding changes the NAT mapping.
- Do not use `SO_REUSEADDR`. On Windows another socket can then bind the same endpoint, making delivery nondeterministic. Use `SO_EXCLUSIVEADDRUSE`.
- Do not call `connect()`. A connected UDP socket filters out datagrams from other sources, making it impossible to handle a STUN server and several candidates at once.
- **Turn off `SIO_UDP_CONNRESET`.** Otherwise an ICMP port unreachable from a non-responding candidate surfaces as `WSAECONNRESET` and terminates the receive loop. Sending to non-responding candidates is normal hole punching behavior, so omitting this guarantees a failure in Phase 4.

```cpp
BOOL off = FALSE;
DWORD bytes = 0;
WSAIoctl(sock, SIO_UDP_CONNRESET, &off, sizeof(off), nullptr, 0, &bytes, nullptr, nullptr);
```

---

## 7. Receive Classification

The receive loop classifies each datagram **by the top two bits of the first byte only**. Magic and version are validation items, not classification criteria. Classifying on magic would keep packets with a wrong magic out of the tunnel pipeline entirely, leaving `drop_magic` permanently at zero and hiding malicious tunnel-directed traffic inside `drop_unclassified`.

```text
(buf[0] & 0xC0) == 0x00  and  len >= 20  and  buf[4..8) == STUN_COOKIE
      -> STUN message. Match the transaction ID against pending requests (section 13)
(buf[0] & 0xC0) == 0x40
      -> tunnel candidate. Continue to the section 8 validation pipeline
otherwise
      -> drop, drop_unclassified
```

`buf[a..b)` is a half-open range. `buf[4..8)` is the four bytes at offsets 4, 5, 6, and 7.

---

## 8. Receive Validation Pipeline

Tunnel candidates are checked in this order. Each step has its own drop counter. Any failure drops the packet immediately.

### 8.1 Common checks

| # | Check | Counter on failure |
|---|-------|--------------------|
| 1 | `len >= HEADER_SIZE` | `drop_short` |
| 2 | `magic == TUNNEL_MAGIC` | `drop_magic` |
| 3 | `version == TUNNEL_VERSION` | `drop_version` |
| 4 | `type` is in the section 5 list | `drop_unknown_type` |
| 5 | `payload_length == len - HEADER_SIZE` | `drop_length` |
| 6 | Per-type length: `HELLO`/`HELLO_ACK` exactly 20, `KEEPALIVE` 0, `PING`/`PONG` 8, `CLOSE` 1, `DATA` 20 to 1452 | `drop_type_length` |
| 7 | `peer_id` equals the peer ID for this room | `drop_unknown_peer` |
| 8 | `session_epoch` is not in the retired list | `drop_retired_epoch` |

### 8.2 Epoch and source rules

`HELLO` and `HELLO_ACK` **prove themselves through the nonce**, so they must pass before an epoch is pinned. Every other type is valid only after pinning.

| Type | Rule |
|------|------|
| `HELLO` | Accepted whether or not an epoch is pinned. No source restriction. Epoch rules in 9.3 apply |
| `HELLO_ACK` (attempt nonce echo) | `echo_nonce` equals **the nonce we sent in this attempt**. Used as handshake evidence (`got_ack`), and pins the epoch if none is pinned |
| `HELLO_ACK` (probe nonce echo) | `echo_nonce` equals an active probe nonce and the source equals the address that probe was sent to. **Validates that tentative path only.** Does not set `got_ack` and does not pin the epoch |
| `HELLO_ACK` (anything else) | drop, `drop_bad_nonce` |
| everything else | If no epoch is pinned, drop with `drop_no_epoch`. If it differs from the pinned value, drop with `drop_stale_epoch` |

Without this rule the statement in 9.4 that an early `HELLO_ACK` is normal contradicts the epoch check, and both sides time out in the perfectly normal situation where the peer started punching first.

### 8.3 Duplicate suppression

Apply the decision from 4.5. Packets dropped as `drop_duplicate` or `drop_too_old` **refresh no idle timer and change no state.**

### 8.4 DATA inner validation

`DATA` must pass all of the following immediately before Wintun injection.

| # | Check | Counter on failure |
|---|-------|--------------------|
| 9 | State is `CONNECTED` | `drop_data_early` |
| 10 | Inner IP version nibble == 4 | `drop_inner_not_ipv4` |
| 11 | Inner IHL between 5 and 15 and `IHL*4 <= payload_length` | `drop_inner_ihl` |
| 12 | Inner IPv4 header checksum valid | `drop_inner_checksum` |
| 13 | Inner `total_length == payload_length` | `drop_inner_length` |
| 14 | Inner source IP == the peer's virtual IP | `drop_inner_src` |
| 15 | Inner destination IP == our own virtual IP | `drop_inner_dst` |

Checks 14 and 15 are hygiene against accidental injection from misdelivery and routing bugs. As stated in section 2, they are **not an attack defense.**

### 8.5 Send-side validation

A packet read from Wintun must pass the following before being encapsulated as `DATA`.

| Check | Counter on failure |
|-------|--------------------|
| State is `CONNECTED` | `tx_drop_not_connected` |
| IP version nibble == 4 | `tx_drop_not_ipv4` |
| Length <= `MAX_INNER` | `tx_drop_oversize` |
| Source IP == our own virtual IP | `tx_drop_bad_src` |
| Destination IP == the peer's virtual IP | `tx_drop_no_route` |

---

## 9. Sessions

### 9.1 States

| State | Meaning |
|-------|---------|
| `IDLE` | Peer candidates not yet received |
| `PUNCHING` | Retransmitting `HELLO`. No round-trip evidence |
| `HANDSHAKING` | Evidence in one direction only |
| `CONNECTED` | Evidence in both directions |
| `FAILED` | Terminated with a failure code |
| `CLOSED` | Terminated normally |

### 9.2 The CONNECTED condition

```text
got_ack   = we received a HELLO_ACK echoing our nonce      (confirms our -> peer path)
sent_ack  = we sent a HELLO_ACK for a valid peer HELLO      (confirms peer -> our path)

CONNECTED  <=>  got_ack && sent_ack
```

`sent_ack` is set only after `sendto` returns success. On failure, increment `tx_err_ack` and leave it unset.

Exactly one flag set is `HANDSHAKING`. **Which one is set first is not determined.** Assuming an order either misreads a one-directional opening as connected, or times both sides out on a normal ordering.

Entering `CONNECTED` resets the loss accounting state (4.5).

### 9.3 Epoch rules

| Situation | Action |
|-----------|--------|
| Valid `HELLO`, or nonce-matching `HELLO_ACK`, received with no epoch pinned | Pin that packet's epoch |
| Packet matching the pinned epoch | Normal processing |
| `HELLO` with an epoch different from the pinned one | **The peer retried.** Renegotiation per 9.5 |
| Any other type with a different epoch | Drop, `drop_stale_epoch` |
| Epoch in the retired list | Drop, `drop_retired_epoch` |

### 9.4 Transition table

| Received \ State | `PUNCHING` | `HANDSHAKING` | `CONNECTED` |
|---|---|---|---|
| `HELLO` (matching or unpinned epoch) | reply `HELLO_ACK`, `sent_ack=true`, learn source | same (on a duplicate, just re-send `HELLO_ACK`) | **re-send `HELLO_ACK`.** The peer's ACK was lost; ignoring it times the peer out |
| `HELLO` (different epoch) | renegotiate per 9.5 | renegotiate per 9.5 | renegotiate per 9.5 |
| `HELLO_ACK` (echoes our nonce) | `got_ack=true`, learn source, pin epoch if unpinned | same | duplicate. Ignore, `dup_ack` |
| `HELLO_ACK` (unknown nonce) | drop, `drop_bad_nonce` | same | same |
| `KEEPALIVE` | refresh idle timer, learn source | same | same |
| `PING` | reply `PONG` | same | same |
| `PONG` | match per 5.5 | same | same |
| `DATA` | drop, `drop_data_early` | drop, `drop_data_early` | inject after 8.4 |
| `CLOSE` | move to `CLOSED` | move to `CLOSED` | move to `CLOSED` |

**Receiving `HELLO_ACK` first while in `PUNCHING` is normal.** It means the peer started punching earlier and our `HELLO` arrived while theirs has not yet.

**Only packets that passed every validation applicable to their type** refresh the idle timer. That means 8.1-8.3 for all types, plus 8.4 for `DATA`, the 5.5 `ping_id` match for `PONG`, and reason validation for `CLOSE`. The section 11 table means the same thing.

### 9.5 Renegotiation

A `HELLO` with an epoch different from the pinned one means the peer started a new attempt. Perform the following atomically.

1. Retire the **peer's** previous epoch for 2 minutes
2. Reset `got_ack` and `sent_ack` to false
3. Reset duplicate suppression state, `pending_pings`, and loss accounting
4. Pin the peer's epoch to the new value
5. **Keep our own `session_epoch` and nonce unchanged**
6. Reply `HELLO_ACK` **directly to the source of that datagram**. Do not change `peer_endpoint`
7. Return to `PUNCHING` and restart the punch deadline as 10 seconds **from this moment**
8. Reuse the existing candidate list without going back to the control plane

Step 5 is the critical one. Changing our epoch in reaction to the peer's retry makes the peer read that as a retry in turn and change theirs, and **the two peers trade epochs forever and never connect.** The local epoch changes only when we start a new attempt from `IDLE`.

Omitting the resets in steps 2 and 3 leaves `got_ack` or `sent_ack` from the previous epoch in place, driving the new session to `CONNECTED` without evidence.

Not changing `peer_endpoint` in step 6 matters. The source of the `HELLO` that triggered renegotiation may not be in the candidate list, and switching there directly would let a single forged retry `HELLO` redirect the whole traffic stream to an arbitrary address, bypassing the 10.4 path validation.

**A single reply and a bulk switch are different things.** Returning a `HELLO_ACK` to the source of the datagram it answers is safe: even at a spoofed address it is one packet with no amplification, and for a genuine NAT rebinding the peer receives it. Switching `peer_endpoint`, by contrast, means every subsequent `DATA` goes there, so it must pass 10.4 (c). If the source is inside the candidate set, 10.4 switches automatically on an advancing packet; if outside, only after path validation.

### 9.6 Failure transitions

| Condition | Recorded code |
|-----------|---------------|
| STUN deadline exceeded | `STUN_DISCOVERY_FAILED` |
| `get_peers` polling deadline exceeded, a control plane error, or a `peer_id` collision | `CONTROL_PLANE_EXCHANGE_FAILED` |
| Neither `got_ack` nor `sent_ack` by the punch deadline | `HOLE_PUNCH_TIMEOUT` |
| Only one of them set by the punch deadline | `PEER_HANDSHAKE_FAILED` |
| Idle timeout after `CONNECTED` | `TUNNEL_DROPPED` |

`FAILED` is terminal. A retry starts a new attempt from `IDLE` with a new epoch and a new nonce.

---

## 10. Candidates and Endpoints

### 10.1 Gathering and hygiene

Candidates each peer registers with the control plane:

- **Local candidates**: active IPv4 interface addresses. Exclude loopback, APIPA (`169.254.0.0/16`), our own Wintun adapter, and other tunnel/VPN adapters.
- **Server-reflexive candidates**: the public endpoint from STUN, recorded per STUN server queried.

**Apply the following hygiene rules to a received candidate list. These are mandatory.**

| Rule | Reason |
|------|--------|
| At most `MAX_CANDIDATES` (8). Discard the rest | An unbounded list means unbounded sending |
| Reject broadcast, multicast (`224.0.0.0/4`), unspecified (`0.0.0.0`), and loopback (`127.0.0.0/8`) addresses | Prevents reflection and amplification |
| Reject port 0 | |
| Deduplicate | |

As stated in section 2, without these rules the client becomes a reflection tool sending packets every 200 ms to an attacker-chosen third party.

### 10.2 Rendezvous

If punch starts are skewed, one side's packets are all discarded by the peer's NAT.

1. Both peers complete `register_candidate`.
2. `get_peers` returns `not_ready` until both have registered.
3. **The moment the second registration completes, the server fixes `punch_delay_ms = 1000` once for the room and stores it.** Every subsequent `get_peers` response returns the stored value unchanged. Recomputing per response would give the two peers different times.
4. The response carries `punch_delay_ms` together with `elapsed_since_ready_ms` (time elapsed on the server since readiness). **No absolute timestamps are used.** Client clocks are not guaranteed to be synchronized.
5. Clients poll `get_peers` every 500 ms, for up to 60 seconds.
6. On a ready response, start punching after `max(0, punch_delay_ms - elapsed_since_ready_ms)` milliseconds, or immediately if that is not positive.

This works even if the peer starts their client 50 seconds late. The earlier peer keeps polling, and both compute the delay from the same reference the moment readiness occurs.

**Bound on the residual skew.** The difference between the two peers' actual punch start times is bounded by `poll interval (500 ms) + the difference in transit delay of the two responses`, because the time from the server declaring readiness to each client receiving its response is not reflected in `elapsed_since_ready_ms`. Under ordinary conditions this is under one second, and the 10-second punch window with 200 ms retransmission absorbs it comfortably. In an environment exceeding this bound (response delays of several seconds), the punch window must be lengthened and this document updated.

### 10.3 Punching

- From the punch start, send `HELLO` to **every one of the peer's candidates** at 200 ms intervals.
- The punch deadline is **10 seconds from the actual local start**. If the scheduled time had already passed and punching started immediately, the deadline is 10 seconds from that moment.
- Failing to reach `CONNECTED` by the deadline records a failure per 9.6.

### 10.4 Endpoint learning

v1 uses **learning** rather than fixed nomination.

```text
peer_endpoint = the source endpoint of the most recent packet satisfying all of:

  (a) it passed every validation applicable to its type
      (8.1-8.3 for all; plus 8.4 for DATA, the 5.5 match for PONG, reason validation for CLOSE)
  (b) it ADVANCED highest in 4.5 (packets accepted as reordered do not qualify)
  (c) its source is in the approved candidate set, or it passed path validation (below)
```

All outbound traffic goes to `peer_endpoint`. Before anything is learned (during punching), traffic goes to every candidate.

**Without (b)**, an older packet accepted inside the reordering window would drag the endpoint back to a previous path. Learning only from advancing packets removes the rollback.

**Path validation for (c).** A packet from an address outside the candidate set does not switch the endpoint immediately, because a legitimate NAT rebinding and a source-spoofed attack are indistinguishable at that moment.

```text
1. Record the new address as a tentative path. peer_endpoint stays as it is.
2. Send a HELLO carrying a probe nonce to that address (5.1).
3. Switch peer_endpoint only when a HELLO_ACK echoing that probe nonce arrives from the same address.
   That ACK does not change handshake state (got_ack) or the pinned epoch.
4. If none arrives within 5 seconds, discard the tentative path and increment path_probe_failed.
```

**The premise of this procedure, and what measurement showed.** Step 1 assumes that a packet from an address outside the candidate set **actually arrives**. Measurements on 2026-09-20 found cases where that assumption fails. On all 3 network pairs measured (Windows to Windows, Windows to macOS, Windows to Linux), UDP with **the same source IP but a different source port** was blocked in both directions (6 cases, 0 of 20 probes arrived in each).

**The leading explanation is port-restricted filtering in the NAT,** because the result was the same on macOS and Linux hosts believed to have no active firewall filtering. **It is not stated as certain.** The macOS case is inferred from defaults and was not queried directly; on Linux only `ufw` was confirmed off, and without root the full `nft` and `iptables` rulesets were not seen. No packet capture was taken on either side.

**So v1 does not guarantee recovery from NAT rebinding through (c).** More precisely, it is not guaranteed **for endpoint changes matching the behaviour that was tested, where only the source port changes.** An actual rebinding was never induced and tested, so this does not claim that every rebinding fails. Where it is not guaranteed, step 1 never starts, and renegotiation in 9.5 loses its trigger for the same reason. Both sides then reach the idle timeout (50s) and the session ends with `TUNNEL_DROPPED`. As 9.6 states, **if a retry happens** it starts from `IDLE` with a new epoch and nonce and goes through the control plane again. **This document does not define who starts that retry, or when.** Whether it is automatic is undecided and must be settled in Phases 3 to 5.

(c) is not removed, for two reasons. First, its **defence against forged source addresses** still applies: whenever a packet does arrive from outside the candidate set, that validation is needed. Second, **on a NAT that uses address-restricted (restricted cone) filtering the packet can arrive.** Every observation here was merely **consistent with** a port-restricted NAT; it was not confirmed. The reasoning and the discarded alternatives are in [`decisions/0002-no-rebinding-recovery.md`](decisions/0002-no-rebinding-recovery.md).

An attacker spoofing the source never receives the `HELLO` in step 2 and so cannot complete step 3. This is how the promise in section 2 to block third-party harm is preserved under endpoint learning. Without this procedure, anyone who knows the session identifiers could redirect the entire game traffic stream at an arbitrary victim.

Nomination is not used for three reasons.

- **Behind the same NAT**: if the two peers nominate the LAN path and the hairpinned public path respectively, they pin different endpoints and each filters out the other's packets, deadlocking.
- **Asymmetric nomination**: one side receiving on a LAN candidate while the other sends to a public candidate produces the same deadlock.
- **NAT rebinding**: a mapping can change without the epoch changing, so a fixed scheme has no trigger to renegotiate and must wait for the idle timeout. **Measurement on 2026-09-20 invalidated this reason.** Learning **does not guarantee recovery for a rebinding that produces the tested endpoint change, where only the source port differs and the observed filtering applies** (see the measured limit under (c) above). An actual rebinding was never induced and tested. The other two reasons still hold, so the learning approach itself stays.

Learning uses **whichever path most recently actually delivered**, so all three cases converge automatically. Duplicate suppression (4.5) filters older packets first, so a delayed packet from a previous path cannot drag the endpoint backwards.

Increment `endpoint_learned` whenever the endpoint changes. A continuously rising value means the path is oscillating and is useful for diagnosis.

### 10.5 Security consequence

Thanks to the path validation in 10.4 (c), an attacker who knows the session identifiers cannot redirect traffic to a third party **by spoofing the source alone**, even in v1: the validation `HELLO` goes to the spoofed address and the attacker cannot answer it.

What remains unprotected: an attacker using an address they can actually receive on (their own) passes path validation and pulls the traffic to themselves, and nothing prevents reading or modifying the content. Per section 2, v1 does not address this; when encryption and peer authentication are introduced, path validation must carry a MAC.

---

## 11. Timers

| Timer | Value | Starts at | Reset by | Cancelled by |
|-------|-------|-----------|----------|--------------|
| STUN retry | 500 ms, 1 s, 2 s (3 attempts) | STUN request sent | none | response received |
| STUN deadline | 5 s | first request sent | none | response received |
| `get_peers` poll | every 500 ms | entering `IDLE` | each response | ready response |
| `get_peers` deadline | 60 s | entering `IDLE` | none | ready response |
| Punch delay | computed per 10.2 | ready response | none | expiry starts punching |
| `HELLO` retransmit | every 200 ms | punch start | each send | reaching `CONNECTED` |
| Punch deadline | 10 s | **actual local punch start** | 9.5 renegotiation | reaching `CONNECTED` |
| `KEEPALIVE` | every 15 s | **one sent immediately on** entering `CONNECTED` | each send | session end |
| Idle timeout | 50 s | entering `CONNECTED` | a packet passing all validation for its type (9.4) | session end |
| `PING` | every 5 s | entering `CONNECTED` | each send | session end |
| `pending_pings` sweep | before every insertion | - | - | - |

Deadline comparisons are **strict**. Priority is by **dequeue time**: a receive event dequeued in a given iteration is processed before that iteration's timer expiry.

Dequeue time rather than arrival time, because the receive loop caps how many datagrams it handles per iteration ([`architecture.md`](architecture.md) 3.2.3). Draining without a cap means the timers never run at all. The cost is that a datagram arriving just before a deadline can slip to the next iteration and lose to the timer. While the loop keeps up with the load, that delay is under 1 ms, two orders of magnitude below the shortest deadline (200 ms). If the loop falls persistently behind, the socket buffer overflows and packets are dropped. That is overload, not a timer ordering problem.

The idle timeout is **50 s** rather than 45 s because a keepalive is sent immediately on entering `CONNECTED`, giving four send opportunities within 45 s (at 0, 15, 30, and 45 s). At 45 s the last send would race the timeout. 50 s tolerates three losses and leaves room to recover on the fourth.

15 s and 50 s are initial values. If adjusted from the measured mapping lifetime in Phase 4, update this document.

---

## 12. Size Limits and Fragmentation

```text
1500 (assumed outer path MTU)
 - 20 (outer IPv4 header)
 -  8 (outer UDP header)
 - 20 (tunnel header)
= 1452 bytes = MAX_INNER
```

The virtual adapter MTU defaults to **1400**, leaving 52 bytes of headroom below `MAX_INNER` so an outer path MTU as low as 1448 still works.

**Do not set the DF bit on outer UDP packets.** On Windows `IP_DONTFRAGMENT` is off by default for UDP, so simply do not enable it. With DF set, an intermediate router silently discards packets when the path MTU is smaller, so small packets get through while large ones vanish and **the inner TCP stack enters a retransmission blackhole.** The classic symptom is that login succeeds but world loading hangs.

Full path MTU discovery is out of scope for v1. Phase 7 measures and fixes the default, and the limitation is recorded in [`experiments.md`](experiments.md).

---

## 13. STUN Usage Scope

Only the following parts of RFC 5389 are implemented.

- **Messages**: Binding Request (`0x0001`), Binding Success Response (`0x0101`), Binding Error Response (`0x0111`)
- **Attributes sent**: none. The Binding Request is header only
- **Attributes parsed**: `XOR-MAPPED-ADDRESS` (`0x0020`) only. All others are skipped. `ERROR-CODE` is logged by value
- **Transaction ID**: 96 bits from the OS CSPRNG
- **Response validation**: magic cookie matches, transaction ID matches, message length agrees with the header length field, attribute walking stops on a bounds overrun
- **Address family**: drop if the `XOR-MAPPED-ADDRESS` family is not IPv4 (`0x01`)
- **Retries**: per the section 11 table

FINGERPRINT, MESSAGE-INTEGRITY, authentication, TURN, and ICE procedures are not implemented.

---

## 14. Control Plane Schema

The request/response encodings, error schema, and state transitions for `register_candidate` and `get_peers` are outside this document and **must be fixed in a separate document.** The only contracts this document requires of the control plane are:

1. `peer_id` is unique within a room (4.2)
2. `get_peers` returns `not_ready` until both peers have registered (10.2)
3. `punch_delay_ms` is fixed once per room when the second registration completes and never changes afterwards (10.2)
4. `get_peers` responses include `elapsed_since_ready_ms` (10.2)
5. Only candidates that pass the 10.1 hygiene rules are stored and forwarded
6. Each peer's virtual IP is reported to the other (needed for the `virtual_ip` validation in 5.1)

If any of these six contracts breaks, the tunnel protocol does not hold.

---

## 15. Implementation Checklist

All of the following must exist in code before Phase 4 begins.

- [ ] Every constant in section 3
- [ ] Field-by-field header serialization/deserialization plus a round-trip test
- [ ] `SO_EXCLUSIVEADDRUSE`, no `SO_REUSEADDR`, `SIO_UDP_CONNRESET` off, `connect()` never called
- [ ] Exclusive socket ownership by a single receive loop
- [ ] Section 7 classification (top two bits; magic checked in validation)
- [ ] The eight common checks in 8.1 with dedicated counters
- [ ] The 8.2 epoch/source rules (especially epoch pinning from a nonce-valid `HELLO_ACK`)
- [ ] The 4.5 duplicate suppression algorithm (`highest`, `position`, 64-bit bitmap), ahead of all side effects
- [ ] The leading `seq == highest` check, mutually exclusive `if/else if/else`, the `shift == 64` case, and `uint64_t` for every bit operation
- [ ] Loss accounting on the 64-bit `position` with a `baseline`
- [ ] The `a != b` condition in `is_newer`
- [ ] Per-attempt `session_epoch` with a 2-minute retired list
- [ ] CSPRNG nonce, fixed per attempt, with a 2-minute retired list. Probe nonces in a separate table
- [ ] All of the 10.1 candidate hygiene rules
- [ ] The 10.2 relative-time rendezvous
- [ ] The 10.4 endpoint learning (advancing packets only; path validation for addresses outside the candidate set)
- [ ] The 9.2 two-flag `CONNECTED`, with `sent_ack` only after a successful `sendto`
- [ ] The full 9.4 transition table
- [ ] The eight steps of 9.5 renegotiation, **especially keeping the local epoch/nonce** (changing them causes an infinite ping-pong)
- [ ] The section 11 timer values and lifecycles
- [ ] `CLOSE` send and receive with reason validation
- [ ] The `pending_pings` cap and sweep
- [ ] The section 13 STUN scope

Phase 5 adds: the `DATA` type, 8.4 inner validation, 8.5 send-side validation, 4.5 loss accounting, and fuzz defenses.
