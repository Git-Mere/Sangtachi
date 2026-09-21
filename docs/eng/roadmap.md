# Roadmap

**Project:** Direct-First P2P Virtual Network for Multiplayer Games
**Term:** CSP 400, Fall 2026
**Reference document:** [`first_design.md`](first_design.md)
**Requirements:** [`spec.md`](spec.md) / **Design:** [`architecture.md`](architecture.md) / **Protocol:** [`protocol.md`](protocol.md)

> Korean version: [`../kor/roadmap.md`](../kor/roadmap.md)

This document is the phase-level plan. What to work on first right now is in [`../kor/plan.md`](../kor/plan.md). That document is Korean only.

---

## Requirement Traceability

Maps each ID in [`spec.md`](spec.md) to the phase that satisfies it. No ID may be left untraced.

| Phase | Requirements satisfied |
|-------|------------------------|
| 1 | FR-1, C-2 |
| 2 | FR-2, FR-3, NFR-4, NFR-9 |
| 3 | FR-4, FR-5, C-3 |
| 4 | FR-6, FR-7, FR-8 (up to the minimum `DATA` round trip), FR-13 (partial), NFR-9, M-1 to M-6 |
| 5 | FR-8 (loss and reordering accounting), FR-13, NFR-2, NFR-3 |
| 6 | FR-9, NFR-5, NFR-6, T-1, T-2 |
| 7 | FR-10, FR-11, NFR-6, T-3 |
| 8 | FR-12, T-4, T-5 |
| 9 | FR-14, NFR-7, T-6, A-1 to A-4 |
| All phases | NFR-1, NFR-8, C-1, C-4, C-5 |

---

## Ordering Principle

Phases are ordered by **technical risk**, not by difficulty. The most uncertain assumption (does UDP hole punching actually work across arbitrary NAT environments?) is validated first. The virtual adapter and game integration start only after that assumption is proven.

On scope overrun, work is dropped in reverse order of the priorities below.

```text
P0: UDP + STUN + endpoint exchange + hole punching + tunnel header + minimum DATA round trip + local record (Phases 1-4)
P1: Protocol robustness. Loss and reordering accounting, fuzz defenses, long-duration stability                (Phase 5)
P2: Wintun + virtual IP + Minecraft                                                                            (Phases 6-8)
P3: Telemetry + diagnostics + analysis                                                                         (Phase 9)
P4: Encryption + relay + GUI + extra platforms                                                                 (stretch)
```

---

## Phase 1: Core UDP Networking

**Goal:** Establish the C++ networking fundamentals.
**Priority:** P0
**Requirements:** FR-1, C-2

### Tasks

- Relocate the repository to the target layout (`src/` -> `client/src/`, create `control-server/`)
- Set up the C++20 project with CMake
- Wrap Winsock2 initialization and teardown (`WSAStartup` / `WSACleanup`)
- Implement a UDP socket wrapper (`socket`, `bind`, `sendto`, `recvfrom`, `WSAEventSelect` event handle)
- Implement the event loop skeleton: `WaitForMultipleObjects`, deadline computation, drain pattern ([`architecture.md`](architecture.md) 3.2)
- Implement the endpoint representation type (parsing, comparison, printing)
- Send and receive UDP packets between two known endpoints
- Add basic logging

### Deliverable

```text
Client A <------ UDP ------> Client B
```

### Verification

- Bidirectional message exchange succeeds between two machines on the same LAN (or two processes on one machine)
- Received content matches sent content byte for byte
- On socket error the process does not die and logs the error code
- Several datagrams arriving on one signal are handled in the same iteration (drain pattern works)
- A periodic timer keeps expiring while receives arrive without pause (drain budget works). Verified by generating load above the budget
- `cmake --build` succeeds without warnings

---

## Phase 2: STUN Client

**Goal:** Discover the public UDP endpoint of a client behind NAT.
**Priority:** P0
**Requirements:** FR-2, FR-3

### Tasks

- Study the Binding Request/Response portions of RFC 5389
- Construct the Binding Request packet (header, magic cookie)
- Generate transaction IDs and match them against responses
- Send the request to a public STUN server
- Parse the Binding Response
- Walk the STUN attributes (TLV)
- Decode `XOR-MAPPED-ADDRESS`
- Print the discovered public IP and port
- Handle timeouts and malformed responses

### Deliverable

```text
Local endpoint:  192.168.x.x:xxxxx
Public endpoint: x.x.x.x:xxxxx
```

### Verification

- The printed public IP matches an external IP lookup service
- Responses with a different transaction ID are ignored
- Pointing at a non-responding address times out, records `STUN_DISCOVERY_FAILED`, and never waits indefinitely
- Truncated responses or a wrong magic cookie are rejected without a crash
- Query at least two public STUN servers and record each server-reflexive address. **Identical results are not required.** If they differ, record it as observed destination-dependent mapping
- Confirm through logs that the socket used for STUN is the same socket that hole punching and the tunnel will later use

---

## Phase 3: AWS Coordination Server

**Goal:** Let peers find each other and exchange candidate information.
**Priority:** P0
**Requirements:** FR-4, FR-5

### Tasks

- Deploy the Python control server to AWS EC2
- Implement room creation
- Implement room joining
- Register peers
- Assign virtual IPs (manage the `10.100.0.0/24` pool)
- Register candidate endpoints (local, public). Local candidates are used when both peers are on the same LAN
- Exchange peer information (`get_peers`)
- Store room/peer state in SQLite
- Implement the control plane client on the C++ side

### Deliverable

Two clients on different networks obtain each other's endpoint through AWS.

### Verification

- The room creator receives `10.100.0.1` and the joiner receives `10.100.0.2`
- Joining the same room twice does not assign a duplicate virtual IP
- The `get_peers` response contains the other peer's virtual IP and public endpoint
- Room state is restored from SQLite after a server restart
- Joining a nonexistent room returns a clear error response

---

## Phase 4: UDP Hole Punching

**Goal:** Create a direct P2P UDP path without manual port forwarding. **This is the riskiest phase of the project.**
**Priority:** P0
**Requirements:** FR-6, FR-7, FR-13, FR-8 (up to the minimum `DATA` round trip), M-5, M-6

### Tasks

- **Re-run the `tools/nat-probe` measurement before starting.** Confirm that the verdict "a direct connection is established" still holds. A changed condition found here still leaves time to revise the design ([ADR 0001](decisions/0001-no-direct-connection-fallback.md))

Follow the section 15 implementation checklist in [`protocol.md`](protocol.md) exactly. This phase is implementation, not design.

- Header serialization/deserialization (field by field, network byte order)
- Exclusive socket ownership by a single receive loop, STUN/tunnel demultiplexing (`protocol.md` sections 6 and 7)
- Required socket options: `SO_EXCLUSIVEADDRUSE`, `SIO_UDP_CONNRESET` off, never call `connect()`
- Receive validation pipeline with dedicated drop counters. The 8.1 common checks 1 to 8 and the 8.4 inner validation checks 9 to 15 are all in this phase (`protocol.md` section 8)
- `session_epoch` generation and pinning
- Candidate gathering (local and server-reflexive) and control plane rendezvous polling (`protocol.md` 10.1, 10.2)
- `HELLO` retransmission every 200 ms to **every** peer candidate, 10 s punch deadline
- Endpoint learning that moves `peer_endpoint` to the source of a packet that passed validation and advanced `sequence`, plus probe nonce path validation for the new address (`protocol.md` 10.4)
- The two-flag `CONNECTED` condition and the full 9.4 transition table
- `KEEPALIVE` at 15 s (one sent immediately on entering `CONNECTED`), disconnect by 50 s idle timeout
- `PING`/`PONG` RTT measurement via `ping_id`
- `CLOSE` send and receive
- Minimum `DATA` type implementation and round trip. The payload is the **complete inner IPv4 packet** defined in [`protocol.md`](protocol.md) 5.4. Wintun is not present yet, so a **synthetic IPv4 packet** is built and carried. The virtual IPs are the values the control plane assigned in Phase 3
- `DATA` send-side validation (`protocol.md` 8.5)
- Write the connection result (success or the FR-13 failure code) and the RTT to a **local record file**. Do not upload it to the control plane (M-6)

> **The protocol is already fixed**
> Implementation cannot start while the wire protocol is unfixed. [`protocol.md`](protocol.md) was therefore written as a fixed specification before implementation starts.
> Phase 4 does **not** design the protocol. It implements `protocol.md`. If a missing value is discovered during implementation, fix `protocol.md` first rather than choosing a value in code.
> What stays in Phase 5 is loss and reordering accounting, fuzz defenses, and long-duration stability.

### Deliverable

```text
PC A <========== Direct UDP ==========> PC B
```

Established without any router port forwarding in supported environments. The packets exchanged use the real tunnel header and are not replaced in Phase 5.

### Verification

- Two PCs on different home networks communicate bidirectionally with no port forwarding rules
- Packet capture confirms the traffic does not pass through AWS
- Header serialization followed by deserialization reproduces the original (round-trip test)
- The byte layout of captured packets matches the offsets and byte order in `protocol.md` 4.1 exactly, with the first four bytes reading `53 41 4E 47`
- A `HELLO_ACK` arriving first while in `PUNCHING` is accepted normally (`protocol.md` 9.4)
- Re-transmitting `HELLO` while the peer is `CONNECTED` produces another `HELLO_ACK`
- After a client restart, delayed packets from the previous instance are dropped as `drop_stale_epoch`
- Continuously sending to non-responding candidates does not kill the receive loop with `WSAECONNRESET`
- A session where only one side received a `HELLO` does not transition to `CONNECTED` (a one-directional opening is not misreported as success)
- Keepalive interval measurement: create an idle window with **all** UDP sends on that socket stopped (including `PING` and `DATA`), then use **the Phase 3 AWS EC2 control server as an external vantage point** to send periodic UDP probes to that socket's public endpoint, and measure the mapping lifetime from whether the probes arrive. The client does not answer the probes; it only records their arrival. Answering would refresh the mapping with that send and break the idle window. Stopping only keepalive is invalid because game and `PING` traffic also refresh the mapping
- Round-trip a synthetic inner IPv4 packet as `DATA` in both directions and confirm the bytes match (M-5). **The comparison points are pinned.** Compare the payload bytes the sender holds immediately before encapsulation against the payload bytes the receiver extracts after passing the 8.4 validation, **byte for byte including the length**. Do not compare log strings or a reconstructed header. Doing so would pass even if the header was rewritten or padding was added
- `DATA` with a wrong inner source or destination virtual IP is dropped as `drop_inner_src` / `drop_inner_dst`
- The connection result and RTT appear in the local record file, and a failure carries the FR-13 code with it (M-6)
- On hole punching failure, `HOLE_PUNCH_TIMEOUT` is recorded and the process exits cleanly
- Baseline RTT comparison uses a **timestamped UDP echo on the same socket and the same endpoint pair**, not ICMP. ICMP toward a peer behind NAT is commonly blocked or answered by the peer's router, so it does not measure the same path. The median of 20 `PING`/`PONG` samples is within 10 ms absolute or 30% relative of the median of 20 UDP echo samples (allowing for user-space processing overhead). An ICMP comparison may be recorded as supplementary information only
- Forced failure-code test: control plane down -> `CONTROL_PLANE_EXCHANGE_FAILED`, injecting a non-responding endpoint -> `HOLE_PUNCH_TIMEOUT`, blocking the `HELLO_ACK` reply -> `PEER_HANDSHAKE_FAILED`. Each records the exact code and state transition

### Risk

There are NAT environments where this phase fails. Do not hide the failures; record them with the NAT environment information. Define supported and unsupported network conditions explicitly. Relay fallback is not implemented here and stays a stretch goal.

---

## Phase 5: Tunnel Protocol Completion

**Goal:** Add robustness to the tunnel whose round trip was already confirmed in Phase 4. That means loss and reordering accounting, defenses against malformed and fuzzed packets, and long-duration stability.
**Priority:** P1
**Requirements:** FR-8 (loss and reordering accounting), FR-13
**Prerequisite:** the section 15 checklist in [`protocol.md`](protocol.md) is fully complete from Phase 4.

### Tasks

- Loss and reordering accounting from `sequence`, using wrap-aware comparison (32-bit rollover) and a reordering window, so only numbers still missing after the window expires count as lost
- Fuzz defenses. No crash on corrupted or truncated input
- Secure long-duration connection stability

**What Phase 4 already implemented is not built again here.** For the three items below the **test** belongs to this phase and the implementation is in Phase 4. Confusing where each part lives splits the verification criteria across two phases.

| Item | Implementation | What this phase does |
|------|----------------|----------------------|
| 8.1 common checks 1 to 8 and the per-reason drop counters | Phase 4 | Inject wrong magic, unsupported version, length mismatch, and truncated packets, and watch the counters rise under the right reason |
| 8.4 / 8.5 size limits and `payload_length` validation | Phase 4 | Test the 20-byte and `MAX_INNER` boundaries, and the values just outside them |
| 9.4 full session state machine | Phase 4 | Watch the state stay as specified across long-duration windows and failure transitions |

### Deliverable

An application-level tunnel that carries inner IPv4 packets between two peers reliably under loss, reordering, and corruption. In Phase 7 the IP packet actually captured by Wintun takes the place of this `DATA` payload.

### Verification

- Run the whole boundary size case table. **The range is written as values, not as words.** Without values, the points just outside the boundary are not tested

| Payload size | Expected |
|-------------:|----------|
| 19 | `drop_type_length` on receive. The `DATA` minimum is 20 |
| 20 | Round trip succeeds. The smallest packet, an IPv4 header with no options |
| 576 | Round trip succeeds. Middle value |
| 1452 | Round trip succeeds. The `MAX_INNER` boundary |
| 1453 | `tx_drop_oversize` on send. **The receive side is not tested by this table.** The datagram exceeds `MAX_DATAGRAM` (1472), so `recvfrom` fails with `WSAEMSGSIZE` and the validation pipeline is never reached |

  The match decision for the round-trip cases uses the same comparison points as Phase 4
- Packets with wrong magic, unsupported version, or mismatched `payload_length` are all dropped and increment the matching reason counter
- Fuzz test: injecting 100,000 random packets of 1 to 2000 bytes generated from a fixed seed causes no crash or memory error. Log the seed, count, and length range so the run is reproducible
- Control plane outage test: after reaching `CONNECTED`, taking the control server down leaves `DATA` transport and session state intact for 10 minutes (NFR-3)
- Deliberately dropping packets produces a loss rate computed from `sequence` gaps
- Injecting packets out of order does not count them as lost (reordering window works)
- Loss rate does not spike when `sequence` crosses the 32-bit boundary
- Idle timeout test: silencing the peer entirely records `TUNNEL_DROPPED` with the correct state transition, completing the FR-13 failure code set
- Session state stays `CONNECTED` across a long-duration run (1 hour or more)

---

## Phase 6: Windows Virtual Network Adapter

**Goal:** Capture and inject IP packets through a virtual interface.
**Priority:** P2
**Requirements:** FR-9

### Tasks

- Wintun dependency **approved**. Keep documenting the scope it provides
- Create and open the virtual adapter
- Assign the virtual IP address
- Read packets from the adapter
- Inject packets into the adapter
- Configure the required Windows routes and clean them up on shutdown
- Validate traffic with basic IP tools, without a game

### Deliverable

```text
Windows IP stack
      |  packet to 10.100.0.0/24
      v
Wintun Adapter  --- read ---> client (counter increments)
Wintun Adapter  <-- inject -- client (hand-crafted reply)
```

A single host can read packets out of the virtual adapter and inject packets back into it. Peer-to-peer communication between two virtual IPs requires tunnel integration and is the Phase 7 deliverable.

### Verification

- `ipconfig` shows the virtual adapter and its assigned virtual IP
- `route print` shows the `10.100.0.0/24` route pointing at the adapter
- Sending a `ping` from the same machine to an **unassigned** virtual IP (for example `10.100.0.9`) increments the adapter read counter. Hand-crafting the ICMP reply and injecting it makes the `ping` receive a response (verifies both read and inject paths)
- A `ping` to the machine's own virtual IP is short-circuited by the Windows local stack and never reaches the adapter, so it is not used for verification
- Actual virtual IP round trips between two peers require tunnel integration and are verified in Phase 7
- No adapter or route remains after the client exits
- The adapter is recreated correctly after an abnormal termination and restart

### Risk

Windows virtual networking may be more complex than expected. This work is kept separate from the Phase 4 NAT traversal work. Wintun is used only for virtual interface access; the project owns the routing decision logic itself.

---

## Phase 7: Integrate the Virtual Adapter with the P2P Tunnel

**Goal:** Carry virtual network IP packets between peers.
**Priority:** P2
**Requirements:** FR-10, FR-11, NFR-6

### Tasks

- Read outgoing IP packets from Wintun
- Determine the target peer from the destination virtual IP
- Encapsulate in a `DATA` packet
- Send over the direct UDP connection
- Decapsulate received packets
- Inject the original IP packet into Wintun
- Handle packet size limits and fix the MTU
- Add counters and diagnostics

### Deliverable

The two Windows machines communicate using virtual IP addresses alone.

### Verification

- `ping` round trip to the peer's virtual IP succeeds
- File transfer between virtual IPs (SMB or a temporary TCP server) succeeds with matching content
- Maximum-size packets are delivered without fragmentation (MTU value fixed by measurement)
- Packets sent to a virtual IP with no routing target are dropped and increment a counter
- Cross-check the send and receive byte counters against packet capture. **"Exact match, no tolerance" could not be passed even by a correct implementation, so it was replaced by the table below.**

| Item | Definition |
|------|------------|
| Counting basis | **UDP payload bytes including the tunnel header.** Ethernet, IP, and UDP headers are not counted |
| Measurement point | The client counts the bytes that `sendto`/`recvfrom` returned on success. The capture runs on the same machine and filters only that socket's 5-tuple |
| Sample condition | Use **generated traffic**, not game traffic. Start from idle, send a fixed count, stop, and look only at that window. The sizes are 20 / 576 / 1452 bytes from the Phase 5 boundary table |
| Count | **Not fixed here.** Try 1,000 / 5,000 / 10,000 / 50,000 in order and take **the largest value for which the capture reports zero loss.** Write that value into this table. The ladder is only a search grid; the chosen value is what the verdict uses. It depends on the machine and the capture setup, so it cannot be made up in advance |
| Valid window | Decide **only on windows where the loss count reported by the capture tool is 0.** A non-zero count is not a failure; **discard it and measure again** |
| Pass condition | The byte sums match in a valid window. If the capture sum is **larger** than the counter sum it is a failure. Bytes were sent without being counted, which is a counting bug |
| Packet count | **Recorded for reference only.** With UDP segmentation offload on, the number of datagrams the capture sees differs. The byte sum is still the same |

  **No longer claimed.** Accuracy in windows where the capture loses packets is not judged. This test says nothing about counting accuracy under high load

---

## Phase 8: Minecraft Validation

**Goal:** Demonstrate a real multiplayer session.
**Priority:** P2
**Requirements:** FR-12

### Tasks

- **Re-run the `tools/nat-probe` measurement when Phase 8 starts.** Confirm the direct connection verdict still holds before setting up the demo environment ([ADR 0001](decisions/0001-no-direct-connection-fallback.md))
- **Re-run it once more right before the demo.** This is the same-day check. The direct connection verdict rests on observations at one point in time and can be invalidated by a router replacement or an ISP configuration change
- Run the Minecraft Java server on the host
- Have the player connect to the host's virtual IP
- Confirm login and world entry
- Play for an extended period
- Measure latency and disconnect behavior
- Record packet and connection metrics

### Deliverable

```text
Minecraft Client -> 10.100.0.1:25565 -> Project Virtual Network -> Minecraft Server
```

### Verification

- The player connects by entering only `10.100.0.1:25565`, with no public IP or port
- Succeeds with no port forwarding rule on the host's router
- No forced disconnect during 30 minutes or more of continuous play
- Record the in-game displayed latency and the tunnel RTT over the same window and **report the difference between them.** **"Within ±30 ms" could not be decided because it had no measurement method and no measurement point.** It was replaced by the table below

| Item | Definition |
|------|------------|
| What is measured | The in-game displayed latency is the value the Minecraft client shows, and the tunnel RTT is the `PING`/`PONG` measurement. **The two do not measure the same thing.** The game value is an application-layer round trip and sits on top of the tunnel |
| Measurement point | The game value is read from the screen; the tunnel value is read from the client log |
| Sample condition | At least 20 of each over the same window. Log the start and end of the window. **This is not a claim of statistical sufficiency; it is the minimum condition for a record.** The in-game latency refreshes about once per second, so collecting 20 takes roughly a minute |
| Statistics | Median and interquartile range. Mean and standard deviation are not used |
| What is decided | **Measurement consistency, not quality.** However large the game latency is, this criterion alone does not fail it. Perceived quality is judged by the reflection test below |
| Pass condition | The median in-game latency is **at or above** the median tunnel RTT. If it is smaller, it still passes **as long as the difference stays inside the allowance.** The allowance is **the larger of the tunnel RTT interquartile range and one unit of the game display resolution**. On a quiet LAN the interquartile range approaches 0, and a correct implementation would fail there on screen rounding alone. Smaller than that by more than the allowance is a failure and means one of the two is being measured wrongly. **The allowance is not made up in milliseconds. Only the spread from the same measurement and the display resolution of the tool are used** |
| Reporting | Put the median and interquartile range of the difference in [`experiments.md`](experiments.md) |

  **No longer claimed.** No absolute latency bound is claimed. Perceived quality is judged only by the reflection test below
- 20 block placements and 20 chat messages all reflect on the peer client within 2 seconds

---

## Phase 9: Monitoring and Experimental Evaluation

**Goal:** Produce measurable CSP analysis, not just a working demo.
**Priority:** P3
**Requirements:** FR-14, A-1 to A-4

### Tasks

- Record connection establishment time
- Record RTT
- Record packet loss
- Estimate jitter
- Record tunnel traffic volume
- Record session duration
- Separate telemetry upload onto a dedicated thread with a lossy queue ([`architecture.md`](architecture.md) 3.2.6)
- Test multiple network environments
- Compare successful and failed NAT traversal cases (by observed mapping behavior, not by NAT type)
- Visualize experimental results
- Document limitations
- Write [`experiments.md`](experiments.md)

### Test Environments

| Environment | Purpose |
|-------------|---------|
| Same LAN | Baseline measurement |
| Home to home | Primary usage scenario |
| Home to mobile hotspot | Difficult NAT conditions such as CGNAT |
| Campus to home | Institutional firewall conditions |
| Other available NAT behaviors | Comparison across mapping behaviors |

### Statistical Design

**Fix first what counts as one trial, whether trials are independent, and how uncertainty is stated.**
Without that, nobody can say what the resulting numbers mean.

| Item | Definition |
|------|------------|
| One trial | From client start until `CONNECTED` is reached or a failure code is recorded. Session duration and quality measurements are a separate window after `CONNECTED` |
| Between trials | Take the client down and start it again with a new `session_epoch`. Wait longer than the idle timeout (50s) so the previous NAT mapping is no longer alive |
| Independence | **Ten runs done back to back on the same network on the same day are not called independent trials.** The router state and the ISP path are shared. Record them as **repeated measurements**, and count only a changed environment as a separate condition |
| Representative value | Median and interquartile range. **Mean and standard deviation are not used.** RTT and establishment time are skewed, so the mean does not represent them |
| Success rate | Always write the **denominator n** next to the ratio. When an interval is attached, use the Wilson score interval. The normal approximation gives a wrong interval when n is small and the ratio is near 1 |
| Sample count | At least 10 per environment. **This is not a claim of statistical sufficiency.** It is the minimum that is feasible on the networks reachable within one semester. State that limit in [`experiments.md`](experiments.md) and do not read more into the numbers than they carry |

**No longer claimed.** This design is not population estimation. It only records what was observed on the
few networks we could reach, and it does not generalize to other NAT environments.

### Deliverable

A repeatable experiment set and data suitable for the final report and presentation.

### Verification

- Control plane outage test re-run: with telemetry enabled, taking the control server down keeps the tunnel up for 10 minutes (NFR-3). The Phase 5 test is repeated in the telemetry-enabled configuration
- Recording a metric on `[loop]` does not block when the queue is full, and the loss counter increments
- At least 10 repeated measurements per environment. Keep the trial definition and the interval from the statistical design above
- Every failure record carries a per-stage code and NAT environment information. Controlled failures and real-environment failures are **aggregated separately** (A-2)
- Success rate is aggregated by observed mapping behavior. No statement asserts a NAT type from RFC 5389 Binding results alone
- A third party can reproduce the same experiments from the documentation alone
- Charts and a statement of limitations appear in [`experiments.md`](experiments.md)

---

## Stretch Goals

Started only if Phases 1-9 finish with room to spare. Never pursued at the cost of the core P2P objective.

| Item | Note |
|------|------|
| Relay fallback | For peers that cannot connect directly. Rescues environments classified unsupported in Phase 4 |
| Encryption and peer authentication | No custom cryptographic algorithm is developed |
| Windows installer | |
| Simple GUI | |
| Automatic reconnect | |
| Virtual network with three or more peers | Requires extending the router structure |
| Validation with a UDP-based game | Further proof that the tunnel is game-agnostic |
| Linux interoperability | |

---

## Risk Management

| Risk | Mitigation |
|------|------------|
| Hole punching does not work on every network | Validate early in Phase 4 before stacking higher layers. Record NAT-related failures instead of hiding them. Hold relay fallback as a later remedy. State supported and unsupported conditions explicitly |
| Windows virtual networking is more complex than expected | Keep virtual adapter work separate from NAT traversal work. Validate direct P2P with ordinary application data first. Use Wintun only for interface access. The project owns the routing logic |
| Scope grows too large | Fix the P0-P4 priorities. Drop from the lowest priority upward and keep the core P2P objective to the end |
| Third-party library restrictions | Request approval before using any non-basic dependency such as Wintun. Keep core algorithms independent of those dependencies. Document precisely what each library provides |
