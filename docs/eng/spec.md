# Spec

**Project:** Direct-First P2P Virtual Network for Multiplayer Games
**Reference document:** [`first_design.md`](first_design.md)
**Design detail:** [`architecture.md`](architecture.md) / **Fixed protocol:** [`protocol.md`](protocol.md)
**Development plan:** [`roadmap.md`](roadmap.md)

> Korean version: [`../kor/spec.md`](../kor/spec.md)

---

## Purpose

Hosting a multiplayer game server normally requires configuring router port forwarding, understanding public and private IP addresses, and modifying firewall rules. For an ordinary user that is itself the barrier to entry.

This project builds a lightweight user-space virtual network so that participants can reach each other's game servers using only a virtual IP address, **with no manual port forwarding**. It discovers public endpoints through STUN, traverses NAT with UDP hole punching, and carries game traffic on a direct peer-to-peer path. AWS is used only for coordination and telemetry and is kept off the game traffic path.

One-sentence definition:

> A direct-first P2P virtual networking system for Windows that uses STUN-assisted NAT traversal and UDP hole punching to let players reach multiplayer servers through virtual IP addresses without manual port forwarding, while using AWS only for coordination and telemetry.

---

## Scope

### In scope

- **C++20 Windows client**: Winsock2-based UDP sockets, endpoint representation, logging
- **Custom STUN client**: Binding Request construction, transaction IDs, response parsing, `XOR-MAPPED-ADDRESS` decoding, timeout handling
- **Python control plane (AWS EC2)**: room creation/joining, peer registration, virtual IP assignment, candidate endpoint exchange, telemetry collection
- **UDP hole punching**: simultaneous bidirectional send, retries, keepalive, failure classification
- **Custom tunnel protocol**: 20-byte wire header, `HELLO`/`HELLO_ACK`/`KEEPALIVE`/`DATA`/`PING`/`PONG`/`CLOSE`, serialization and malformed packet validation
- **Windows virtual network adapter integration**: adapter creation and packet read/inject through Wintun, virtual IP address and route configuration through the Windows IP Helper API
- **Virtual IP routing**: resolving the peer session from the destination virtual IP, encapsulation and decapsulation
- **Telemetry and diagnostics**: connection success rate, establishment time, RTT, packet loss, jitter, session duration, per-stage failure codes
- **Experimentation and analysis**: repeated measurement across multiple network environments, comparison of successful and failed NAT cases, visualization of results
- **Minecraft Java Edition validation**: a real multiplayer session over the virtual network

### Out of scope

- A full replacement for commercial VPN products
- Large-scale public deployment
- Custom cryptographic algorithm development
- Mobile support
- macOS support
- An advanced GUI
- Full ICE implementation
- Guaranteed connectivity through every NAT configuration
- A custom STUN **server** (only the client is implemented; public servers are used)

### Stretch goals

Started only if the direct connection work finishes early. Never pursued at the cost of the core P2P objective.

- Relay fallback for peers that cannot connect directly
- Encryption and authenticated peer sessions
- A Windows installer
- A simple GUI
- Automatic reconnect
- More than two peers in one virtual network
- Validation with a UDP-based game
- Linux interoperability

---

## Requirements

### Functional requirements

| ID | Requirement |
|----|-------------|
| FR-1 | The client can open a UDP socket with Winsock2 and exchange traffic bidirectionally with a given remote endpoint. |
| FR-2 | The client uses its own STUN client to send a Binding Request to a public STUN server and decodes `XOR-MAPPED-ADDRESS` to learn its own public IP and UDP port. |
| FR-3 | When there is no STUN response or the response is malformed, the client does not wait indefinitely; it times out and records `STUN_DISCOVERY_FAILED`. |
| FR-4 | The control plane supports room creation and joining, and assigns each joining peer a unique virtual IP from `10.100.0.0/24`. |
| FR-5 | The control plane accepts each peer's candidate endpoints (local, public) and forwards them to the other peers in the same room. Local candidates are used when both peers are on the same LAN. |
| FR-6 | Clients attempt UDP hole punching toward the exchanged endpoints and establish a direct UDP path without manual port forwarding. Establishment is confirmed by receiving the peer's `HELLO_ACK`, never by a successful local send alone. |
| FR-7 | An established session maintains the NAT mapping with keepalive and measures RTT with `PING`/`PONG`. Disconnection is decided by an idle timeout (no packets from the peer), not by send failure. |
| FR-8 | The tunnel protocol uses the 20-byte wire header fixed by [`protocol.md`](protocol.md) (magic, version, type, payload_length, peer_id, session_epoch, sequence); it serializes field by field and drops any packet that fails the receive validation pipeline. |
| FR-9 | The client creates the virtual adapter with Wintun and configures the assigned virtual IP and the `10.100.0.0/24` route through the Windows IP Helper API. |
| FR-10 | The client routes IP packets read from the virtual adapter by destination virtual IP, encapsulates them in `DATA` packets, and sends them over the direct UDP path. |
| FR-11 | The receiving side decapsulates `DATA` packets and injects the original IP packet into the virtual adapter. Before injection it validates inner IPv4 well-formedness, that the inner source IP equals the sending peer's virtual IP, and that the inner destination IP is our own virtual IP. |
| FR-12 | When a player enters `10.100.0.1:25565` in the Minecraft server address field, they connect to the server on the remote host. |
| FR-13 | Every connection attempt records success/failure and, on failure, the per-stage code (`STUN_DISCOVERY_FAILED`, `CONTROL_PLANE_EXCHANGE_FAILED`, `HOLE_PUNCH_TIMEOUT`, `PEER_HANDSHAKE_FAILED`, `TUNNEL_DROPPED`). |
| FR-14 | The client collects establishment time, RTT, packet loss, jitter, transfer volume, and session duration, and reports them to the control plane. |

### Non-functional requirements

| ID | Requirement |
|----|-------------|
| NFR-1 | Game traffic does not pass through AWS on the normal path. AWS is for coordination and telemetry only. |
| NFR-2 | The tunnel layer contains no Minecraft-specific logic. Replacing Minecraft with another IP-based application requires no change to the tunnel code. |
| NFR-3 | A control plane outage does not tear down already established P2P tunnels. Telemetry upload failures are ignored and retried. |
| NFR-4 | STUN, NAT traversal, hole punching, the tunnel protocol, routing, and session management are implemented directly. Wintun provides virtual interface access only. |
| NFR-5 | Non-basic third-party dependencies require instructor approval **before** integration, and the scope each library provides is documented. Wintun is approved. |
| NFR-6 | The virtual adapter MTU accounts for tunnel header overhead, and normal game traffic does not trigger IP fragmentation. |
| NFR-7 | Failures are not hidden. NAT environments that could not be traversed are explicitly declared out of the supported range and recorded. |
| NFR-8 | Source control and weekly development records are maintained. Where AI tools contribute to code or documentation, the output is verified and cited per course policy. |
| NFR-9 | STUN, hole punching, and tunnel traffic all use the same local UDP socket. Different sockets mean different NAT mappings, which invalidates the discovered endpoint. |

### Constraints

| ID | Constraint |
|----|------------|
| C-1 | The client target is Windows 10 / 11 x64. Both development and demonstration happen on Windows. |
| C-2 | The client language is C++20, built with CMake. Native Windows APIs are used instead of high-level networking frameworks. |
| C-3 | The control server language is Python, initially standard library only. The deployment target is a single AWS EC2 instance with SQLite. |
| C-4 | The work must be completable within one semester. **Phases are not pinned to specific weeks.** Progress sets the pace, so a fixed week allocation in the documents would soon disagree with the facts. |
| C-5 | On scope overrun, drop from the lowest priority upward. P0: UDP + STUN + endpoint exchange + hole punching + tunnel header + minimum `DATA` round trip + local record (Phases 1-4) / P1: Protocol robustness. Loss and reordering accounting, fuzz defenses, long-duration stability (Phase 5) / P2: Wintun + virtual IP + Minecraft (Phases 6-8) / P3: telemetry + diagnostics + analysis (Phase 9) / P4: encryption + relay + GUI + extra platforms (stretch). The tier contents must match the priority block in [`roadmap.md`](roadmap.md). They need not be identical character for character. One is a sentence and the other is a code block. **What may be dropped, and what disappears when it is dropped, is written in "Cost of dropping a priority" below** |

#### Cost of dropping a priority

This records **which success criteria disappear** together with a dropped C-5 tier. Without
this, "drop from the lowest upward" becomes an instruction that breaks the minimum deliverable.

| Tier dropped | What disappears | May it be dropped |
|--------------|-----------------|-------------------|
| P4 (stretch) | Nothing. It was never counted in | Yes |
| P3 (Phase 9) | A-1 - A-4, T-6, FR-14 | Yes. But **the analysis success the course requires disappears.** Minimum success remains |
| P2 (Phases 6-8) | T-1 - T-5, FR-9 - FR-12 | Yes. Minimum success remains |
| P1 (Phase 5) | The loss and reorder accounting of FR-8, the completion of the FR-13 failure code set (`TUNNEL_DROPPED`), and the verification of NFR-2 and NFR-3. **No M/T/A success criterion disappears** | Yes. It is dropped in the knowledge that requirement verification is reduced |
| P0 (Phases 1-4) | All of M-1 - M-6 | **No.** Minimum success disappears |

**Minimum success M-1 - M-6 holds with P0 alone.** That is the definition of P0. It is why the
minimum `DATA` round trip (M-5) and the local record (M-6) sit in P0.

---

## Success Criteria

#### Supported network conditions (defined in advance)

The "supported NAT environment" in M-4 is not decided after the fact. The following is fixed **before** any success determination.

| Item | Definition |
|------|------------|
| Supported condition | Each peer observes the same public IP:Port for itself across queries to two different STUN servers, i.e. the mapping does not change with the destination (endpoint-independent mapping) |
| Required test topologies | (1) same LAN, (2) two different home networks. M-4 must hold in both for minimum success to be granted |
| Best-effort topologies | Mobile hotspot, campus network. Failure here does not affect the minimum success determination, but the failure is still recorded |
| Unsupported conditions | Mappings where the public port changes per destination (symmetric behavior). Classification is by observed behavior, not by carrier topology: CGNAT is listed as a potentially difficult topology, not as unsupported by definition, since some CGNAT deployments use endpoint-independent mapping and do permit hole punching. A failed direct connection is treated as expected behavior and only recorded |

### Minimum success

All of the following must hold.

| # | Criterion | Verification |
|---|-----------|--------------|
| M-1 | The client runs on two Windows machines on different networks. | Run the client on both machines and confirm the process starts normally |
| M-2 | Both machines discover a usable public endpoint through the custom STUN client. | Each machine logs its local and public endpoint. The public endpoint is confirmed by **querying two different STUN servers over the same UDP socket** and observing the same IP:Port (the same determination as the supported condition above). An HTTPS-based IP lookup service runs over a TCP path, which may differ from the UDP send path, so it is **recorded for reference only.** Whether the discovered IP:Port is actually receivable is confirmed in M-4/M-5 |
| M-3 | The two peers exchange connection information through the AWS control plane. | The `get_peers` response contains the other peer's virtual IP and public endpoint |
| M-4 | A direct UDP connection is established in a supported NAT environment without manual port forwarding. | With no router port forwarding rule in place, the session reaches `CONNECTED`. Packet capture confirms the traffic does not go through AWS |
| M-5 | Inner IP packets are transferred over the direct tunnel. | A `DATA` round trip succeeds in both directions and the received bytes match the sent bytes. The payload is a **complete inner IPv4 packet** as defined in [`protocol.md`](protocol.md) 5.4. Phase 4 judges this with synthesized packets and no Wintun; carrying real application traffic is confirmed in T-3 |
| M-6 | Connection status and basic performance data are recorded. | The client writes the connection result (success, or the FR-13 failure code) and the RTT to a **local record file**. The path and the line format of that file are fixed in [`architecture.md`](architecture.md) chapter 9. **Until they are fixed, this criterion cannot be decided.** Control plane upload and per-environment aggregation are FR-14 and belong to Phase 9 |

### Target success

In addition to minimum success, all of the following must hold.

| # | Criterion | Verification |
|---|-----------|--------------|
| T-1 | A virtual adapter comes up on both Windows machines. | `ipconfig` shows the virtual adapter and its assigned virtual IP |
| T-2 | Virtual IPs are assigned. | Host is `10.100.0.1`, player is `10.100.0.2` |
| T-3 | IP packets are routed through the P2P tunnel. | `ping` to the peer's virtual IP succeeds and tunnel counters increase |
| T-4 | A real Minecraft Java Edition multiplayer session runs over the virtual network. | The player connects to `10.100.0.1:25565`, logs in, and enters the world |
| T-5 | Gameplay is sustained rather than stopping at initial connection success. | No forced disconnect during 30 minutes or more of continuous play |
| T-6 | Latency, packet loss, connection time, and session stability are measured. | A dataset of RTT, loss rate, establishment time, and duration for the whole session |

### Analysis success (CSP requirement)

| # | Criterion | Verification |
|---|-----------|--------------|
| A-1 | Repeated experiments are run across multiple network environments. | Measurements for same LAN, home to home, home to mobile hotspot, and campus to home |
| A-2 | Successful and failed NAT traversal cases are compared. | Every failure records a per-stage code and NAT environment information, aggregated by observed mapping behavior. NAT type is never asserted. **Both controlled failure scenarios** are run. Blocking UDP at the firewall must record `STUN_DISCOVERY_FAILED`. Injecting an unresponsive endpoint must record `HOLE_PUNCH_TIMEOUT`. **What it is evidence of is written down separately.** A controlled failure is evidence that the failure handling path works. It is not evidence about the failure rate or the failure distribution in real environments. The failure rate comes only from the repeated measurements of A-1. **If there are zero real failures, zero is what is written.** A controlled failure does not stand in for it |
| A-3 | Experimental results are visualized and limitations are documented. | [`experiments.md`](experiments.md) contains charts and a statement of limitations |
| A-4 | A repeatable experimental procedure exists. | A third party can reproduce the same experiments from the documentation alone |
