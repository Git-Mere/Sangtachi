# Roadmap

**Project:** Direct-First P2P Virtual Network for Multiplayer Games
**Term:** CSP 400, Fall 2026
**Reference document:** [`first_design.md`](first_design.md)
**Requirements:** [`spec.md`](spec.md) / **Design:** [`architecture.md`](architecture.md) / **Protocol:** [`protocol.md`](protocol.md)

> Korean version: [`../kor/roadmap.md`](../kor/roadmap.md)

This document is the phase-level plan. The detailed checklist for the work unit currently in progress is kept in [`plan.md`](plan.md).

---

## Requirement Traceability

Maps each ID in [`spec.md`](spec.md) to the phase that satisfies it. No ID may be left untraced.

| Phase | Requirements satisfied |
|-------|------------------------|
| 1 | FR-1, C-2 |
| 2 | FR-2, FR-3, NFR-4, NFR-9 |
| 3 | FR-4, FR-5, C-3 |
| 4 | FR-6, FR-7, FR-8 (base header), FR-13 (partial), NFR-9, M-1 to M-5 |
| 5 | FR-8, FR-13, NFR-2, NFR-3, M-6 |
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
P0: UDP + STUN + endpoint exchange + hole punching + base tunnel header  (Phases 1-4)
P1: DATA transport + protocol robustness + session state machine        (Phase 5)
P2: Wintun + virtual IP + Minecraft                                     (Phases 6-8)
P3: Telemetry + diagnostics + analysis                                  (Phase 9)
P4: Encryption + relay + GUI + extra platforms                          (stretch)
```

---

## Milestone Overview

| Milestone | Timing | Phases | Core proof |
|-----------|--------|--------|------------|
| M1 Networking proof of concept | ~week 5 | Phases 1-4 | Two PCs on different networks talk directly over UDP without port forwarding |
| M2 Working virtual network | ~week 10 | Phases 5-8 | A player connects to Minecraft at `10.100.0.1:25565` |
| M3 Final system and analysis | ~week 15 | Phase 9 + cleanup | Measured data across multiple environments plus reliability and performance analysis |

---

## Phase 1: Core UDP Networking

**Goal:** Establish the C++ networking fundamentals.
**Priority:** P0
**Requirements:** FR-1, C-2

### Tasks

- Relocate the repository to the target layout (`src/` -> `client/src/`, create `control-server/`)
- Set up the C++20 project with CMake
- Wrap Winsock2 initialization and teardown (`WSAStartup` / `WSACleanup`)
- Implement a UDP socket wrapper (`socket`, `bind`, `sendto`, `recvfrom`, `WSAPoll`)
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
**Requirements:** FR-6, FR-7, FR-13, FR-8 (base header)

### Tasks

Follow the section 12 implementation checklist in [`protocol.md`](protocol.md) exactly. This phase is implementation, not design.

- Header serialization/deserialization (field by field, network byte order)
- Exclusive socket ownership by a single receive loop, STUN/tunnel demultiplexing (`protocol.md` sections 5 and 6)
- Required socket options: `SO_EXCLUSIVEADDRUSE`, `SIO_UDP_CONNRESET` off, never call `connect()`
- Receive validation pipeline steps 1 to 9 with dedicated drop counters (`protocol.md` section 7)
- `session_epoch` generation and pinning
- Candidate gathering (local and server-reflexive) and control plane rendezvous polling (`protocol.md` 8.2)
- `HELLO` retransmission every 200 ms to **every** peer candidate, 10 s punch deadline
- `HELLO_ACK`-based nomination rule (`protocol.md` 8.4)
- The two-flag `CONNECTED` condition and the full 9.3 transition table
- `KEEPALIVE` at 15 s (one sent immediately on entering `CONNECTED`), disconnect by 50 s idle timeout
- `PING`/`PONG` RTT measurement via `ping_id`
- `CLOSE` send and receive
- Record success or the failure reason

> **The protocol is already fixed**
> The 2026-09-14 full audit ([`design-audit.md`](design-audit.md)) found the unspecified wire protocol to be a blocker at the level of "nothing can be implemented". [`protocol.md`](protocol.md) was therefore written as a fixed specification before implementation starts.
> Phase 4 does **not** design the protocol. It implements `protocol.md`. If a missing value is discovered during implementation, fix `protocol.md` first rather than choosing a value in code.
> What stays in Phase 5 is the `DATA` type, inner packet validation (section 7, checks 10 to 15), loss accounting, and fuzz defenses.

### Deliverable

```text
PC A <========== Direct UDP ==========> PC B
```

Established without any router port forwarding in supported environments. The packets exchanged use the real tunnel header and are not replaced in Phase 5.

### Verification

- Two PCs on different home networks communicate bidirectionally with no port forwarding rules
- Packet capture confirms the traffic does not pass through AWS
- Header serialization followed by deserialization reproduces the original (round-trip test)
- The byte layout of captured packets matches the offsets and byte order in `protocol.md` 3.1 exactly, with the first four bytes reading `53 41 4E 47`
- A `HELLO_ACK` arriving first while in `PUNCHING` is accepted normally (`protocol.md` 9.3)
- Re-transmitting `HELLO` while the peer is `CONNECTED` produces another `HELLO_ACK`
- After a client restart, delayed packets from the previous instance are dropped as `drop_stale_epoch`
- Continuously sending to non-responding candidates does not kill the receive loop with `WSAECONNRESET`
- A session where only one side received a `HELLO` does not transition to `CONNECTED` (a one-directional opening is not misreported as success)
- Keepalive interval measurement: create an idle window with **all** UDP sends on that socket stopped (including `PING` and `DATA`), and measure mapping lifetime with periodic probes from an external vantage point. Stopping only keepalive is invalid because game and `PING` traffic also refresh the mapping
- On hole punching failure, `HOLE_PUNCH_TIMEOUT` is recorded and the process exits cleanly
- Baseline RTT comparison uses a **timestamped UDP echo on the same socket and the same endpoint pair**, not ICMP. ICMP toward a peer behind NAT is commonly blocked or answered by the peer's router, so it does not measure the same path. The median of 20 `PING`/`PONG` samples is within 10 ms absolute or 30% relative of the median of 20 UDP echo samples (allowing for user-space processing overhead). An ICMP comparison may be recorded as supplementary information only
- Forced failure-code test: control plane down -> `CONTROL_PLANE_EXCHANGE_FAILED`, injecting a non-responding endpoint -> `HOLE_PUNCH_TIMEOUT`, blocking the `HELLO_ACK` reply -> `PEER_HANDSHAKE_FAILED`. Each records the exact code and state transition

### Risk

There are NAT environments where this phase fails. Do not hide the failures; record them with the NAT environment information. Define supported and unsupported network conditions explicitly. Relay fallback is not implemented here and stays a stretch goal.

---

## Phase 5: Tunnel Protocol Completion

**Goal:** Build game traffic transport and robustness on top of the base header from Phase 4.
**Priority:** P1
**Requirements:** FR-8, FR-13
**Prerequisite:** the section 12 checklist in [`protocol.md`](protocol.md) is fully complete from Phase 4.

### Tasks

- Add the `DATA` packet type (carries arbitrary payloads)
- Define the payload size limit and validate `payload_length`
- Defend against malformed packets (wrong magic, unsupported version, length mismatch, truncated packets)
- Loss and reordering accounting from `sequence`, using wrap-aware comparison (32-bit rollover) and a reordering window, so only numbers still missing after the window expires count as lost
- Add per-reason drop counters
- Clean up the session state machine (`IDLE` / `PUNCHING` / `HANDSHAKING` / `CONNECTED` / `FAILED`)
- Secure long-duration connection stability
- Fold the `DATA` sections into [`protocol.md`](protocol.md) (an addition, not a structural change)

### Deliverable

An application-level tunnel that reliably carries arbitrary payloads between two peers. In Phase 7 the original IP packet takes the place of this `DATA` payload.

### Verification

- Round-tripping an arbitrary-size payload as `DATA` matches byte for byte
- A payload above the maximum allowed size is rejected at the send stage
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

- Wintun dependency **approved (2026-09-14)**. Keep documenting the scope it provides
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
- Send and receive byte counters match packet capture exactly. The counting basis is fixed as **UDP payload bytes including the tunnel header**, with no tolerance for discrepancy

---

## Phase 8: Minecraft Validation

**Goal:** Demonstrate a real multiplayer session.
**Priority:** P2
**Requirements:** FR-12

### Tasks

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
- In-game displayed latency (F3 screen) is within ±30 ms of the tunnel RTT measured at the same moment
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

### Deliverable

A repeatable experiment set and data suitable for the final report and presentation.

### Verification

- At least 10 repeated measurements per environment
- Every failure record carries a per-stage code and NAT environment information
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
| ~~Wintun approval delay~~ | **Resolved (approved 2026-09-14).** Any additional non-basic dependency goes through the same prior approval |
