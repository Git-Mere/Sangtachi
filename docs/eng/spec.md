# Spec

**Project:** Direct-First P2P Virtual Network for Multiplayer Games
**Reference document:** [`first_design.md`](first_design.md)
**Design detail:** [`architecture.md`](architecture.md) / **Fixed protocol:** [`protocol.md`](protocol.md)
**Development plan:** [`roadmap.md`](roadmap.md)

> Korean version: [`../kor/spec.md`](../kor/spec.md)

---

## Purpose

Hosting a multiplayer game server normally requires router port forwarding, an understanding of public and private IP addresses, and firewall rule changes. For an ordinary user, that alone is the barrier to entry.

This project builds a lightweight virtual network that runs in user space, so that participants reach each other's game servers using only a virtual IP address, **with no manual port forwarding**. It discovers public endpoints with STUN, traverses NAT with UDP hole punching, and carries game traffic on a direct peer-to-peer path. AWS is used only for coordination and telemetry and is not placed on the game traffic path.

One-sentence definition:

> A direct-first P2P virtual networking system for Windows that uses STUN-based NAT traversal and UDP hole punching to let players reach multiplayer servers through a virtual IP without manual port forwarding, and uses AWS only for coordination and telemetry.

---

## Scope

### Included

- **C++20 Windows client**: Winsock2-based UDP socket, endpoint representation, logging
- **Own STUN client**: Binding Request construction, transaction ID, response parsing, `XOR-MAPPED-ADDRESS` decoding, timeout handling
- **Python control plane (AWS EC2)**: room create/join, peer registration, virtual IP assignment, candidate endpoint exchange
- **Python telemetry service (AWS EC2)**: receives and stores connection results and performance metrics. **It is a separate service from the control plane.** The boundary contract is in [`architecture.md`](architecture.md) 3.4
- **UDP hole punching**: simultaneous bidirectional transmission, retries, keepalive, failure reason classification
- **Own tunnel protocol**: 20-byte wire header, `HELLO`/`HELLO_ACK`/`KEEPALIVE`/`DATA`/`PING`/`PONG`/`CLOSE`, serialization and corrupt packet validation
- **Windows virtual network adapter integration**: adapter creation and packet read/inject through Wintun, virtual IP address and route configuration through the Windows IP Helper API
- **Virtual IP routing**: peer session selection by destination virtual IP, encapsulation and decapsulation
- **Telemetry and diagnostics**: connection success rate, establishment time, RTT, packet loss, jitter, session duration, per-stage failure codes
- **Experiments and analysis**: repeated measurements across multiple network environments, comparison of successful and failed NAT cases, result visualization
- **Minecraft Java Edition validation**: a real multiplayer session over the virtual IP

### Excluded

- Full replacement of a commercial VPN product
- Large-scale public deployment
- Development of our own cryptographic algorithms
- Mobile support
- macOS support
- Advanced GUI
- Full ICE implementation
- Guaranteed connectivity across all NAT configurations
- Own STUN **server** implementation (client only; public servers are used)

### Stretch goals

Started only if the direct connection work finishes early. Not pursued at the expense of the core P2P goals.

- Relay fallback for peers whose direct connection fails
- Encrypted and authenticated peer sessions
- Windows installer
- Simple GUI
- Automatic reconnection
- Three or more parties in one virtual network
- Validation with a UDP-based game
- Linux interoperability

---

## Requirements

### Functional requirements

| ID | Requirement |
|----|----------|
| FR-1 | The client can open a UDP socket with Winsock2 and send to and receive from a specified remote endpoint in both directions. |
| FR-2 | The client sends a Binding Request to a public STUN server with its own STUN client implementation and decodes `XOR-MAPPED-ADDRESS` to learn its public IP and UDP port. |
| FR-3 | If the STUN response is missing or malformed, the client does not wait forever; after a timeout it records `STUN_DISCOVERY_FAILED`. |
| FR-4 | The control plane supports room creation and joining, and assigns each joining peer a unique virtual IP in the `10.100.0.0/24` range. |
| FR-5 | The control plane accepts each peer's candidate endpoints (local, public) and delivers them to the other peers in the same room. The local candidate is used when two peers are on the same LAN. |
| FR-6 | Clients attempt UDP hole punching toward the exchanged endpoints and establish a direct UDP path without manual port forwarding. Connection establishment is confirmed by **evidence in both directions**. Both must hold: a `HELLO_ACK` echoing the nonce we sent was received (self -> peer path), and a `HELLO_ACK` was sent in reply to a valid `HELLO` from the peer (peer -> self path). Evidence from one side alone, or our own successful transmission alone, is not a verdict. The decision formula is set by [`protocol.md`](protocol.md) 9.2. |
| FR-7 | An established session keeps the NAT mapping alive with keepalives and measures RTT with `PING`/`PONG`. Disconnection is judged by an idle timeout (no packets received from the peer), not by transmission failure. |
| FR-8 | The tunnel protocol uses the 20-byte wire header fixed by [`protocol.md`](protocol.md) (magic, version, type, payload_length, peer_id, session_epoch, sequence), serializes it field by field, and discards packets that fail the receive validation pipeline. |
| FR-9 | The client creates a virtual adapter with Wintun and configures the assigned virtual IP and the `10.100.0.0/24` route with the Windows IP Helper API. |
| FR-10 | The client routes IP packets read from the virtual adapter by destination virtual IP, encapsulates them in `DATA` packets, and sends them over the direct UDP path. |
| FR-11 | The receiver decapsulates `DATA` packets and injects the original IP packet into the virtual adapter. Before injection it validates inner IPv4 validity, that the inner source IP matches the sending peer's virtual IP, and that the inner destination IP is its own virtual IP. |
| FR-12 | When a player enters `10.100.0.1:25565` in the Minecraft server address field, the client connects to the server on the remote host. |
| FR-13 | Every connection attempt records success or failure and, on failure, a per-stage code (`STUN_DISCOVERY_FAILED`, `CONTROL_PLANE_EXCHANGE_FAILED`, `HOLE_PUNCH_TIMEOUT`, `PEER_HANDSHAKE_FAILED`, `TUNNEL_DROPPED`). |
| FR-14 | The client collects connection establishment time, RTT, packet loss, jitter, transfer volume, and session duration and reports them to the **telemetry service**. It does not report them to the control plane. |

### Non-functional requirements

| ID | Requirement |
|----|----------|
| NFR-1 | Game traffic does not pass through AWS on the normal path. AWS is for coordination and telemetry only. |
| NFR-2 | The tunnel layer contains no Minecraft-specific logic. Replacing Minecraft with another IP-based application does not change the tunnel code. |
| NFR-3 | A control plane failure does not tear down an already established P2P tunnel. Telemetry transmission failures are ignored. **Failed records are not resent.** The next period sends that period's new metrics. The basis for verdicts is the local record file, not the upload ([`architecture.md`](architecture.md) chapter 9). |
| NFR-4 | STUN, NAT traversal, hole punching, the tunnel protocol, routing, and session management are implemented directly. Wintun provides virtual interface access only. |
| NFR-5 | Non-basic third-party dependencies receive instructor approval **before** integration, and the scope each library provides is documented. Wintun and `boto3` are approved. |
| NFR-6 | The virtual adapter MTU is set with the tunnel header overhead in mind, and no IP fragmentation occurs for normal game traffic. |
| NFR-7 | Failures are not hidden. NAT environments that cannot be traversed are explicitly marked as out of support scope and recorded. |
| NFR-8 | Source control and weekly development records are maintained. Where AI tools contributed to code or documents, the contribution is verified and attributed according to course policy. |
| NFR-9 | STUN, hole punching, and tunnel traffic all use the same local UDP socket. A different socket gets a different NAT mapping, which invalidates the discovered endpoint. |
| NFR-10 | The telemetry service is a separate deployment unit from the control plane, and there is **no call dependency** between the two services. If the telemetry service process stops, the control plane's operations and the data plane are not blocked by it. **Resource isolation is not part of this requirement.** While the two services share one instance, shared resource exhaustion remains a risk, and it does until an isolation mechanism is chosen. The boundary contract is in [`architecture.md`](architecture.md) 3.4. |

### Constraints

| ID | Constraint |
|----|------|
| C-1 | The client target is Windows 10 / 11 x64. Both development and demonstration are done on Windows. |
| C-2 | The client language is C++20, built with CMake. Native Windows APIs are used instead of a high-level networking framework. |
| C-3 | The control server and telemetry service are written in Python. The deployment target is **two processes on a single AWS EC2 instance** (separate ports) and **Amazon DynamoDB**. Beyond the standard library, the AWS SDK (`boto3`) is used. `boto3` was approved under NFR-5. |
| C-4 | Must be completable within one semester. **Phases are not pinned to specific weeks.** Progress speed decides, so fixing a week allocation in the document soon diverges from fact. |
| C-5 | If scope is exceeded, remove from the lowest priority first. P0: UDP + STUN + endpoint exchange + hole punching + tunnel header + minimal `DATA` round trip + local record (Phase 1-4) / P1: protocol robustness. Loss and reordering aggregation, fuzz defense, long-run stability (Phase 5) / P2: Wintun + virtual IP + Minecraft (Phase 6-8) / P3: telemetry + diagnostics + analysis (Phase 9) / P4: encryption + relay + GUI + additional platforms (stretch). The priority block in [`roadmap.md`](roadmap.md) **must have the same tier contents.** It need not match word for word. One is prose and the other is a code block. **What can be dropped, and what disappears when it is, is written in "Cost of priority removal" below** |

#### Cost of priority removal

This records **which success criteria disappear** when a C-5 tier is dropped. Without this,
"remove from the lowest first" becomes an instruction that breaks the minimum deliverable.

| Tier dropped | What disappears | Can it be dropped |
|-------------|-------------|----------------|
| P4 (stretch) | Nothing. It was never in the calculation | Yes |
| P3 (Phase 9) | A-1 ~ A-4, T-6, FR-14 | Yes. But **the analysis success the course requires disappears.** Minimum success remains |
| P2 (Phase 6-8) | T-1 ~ T-5, FR-9 ~ FR-12, verification of NFR-2 (Phase 7 file transfer test) | Yes. Minimum success remains |
| P1 (Phase 5) | FR-8 loss and reordering aggregation, completion of the FR-13 failure code set (`TUNNEL_DROPPED`), verification of NFR-3. **No M/T/A success criterion disappears** | Yes. Drop it knowing that requirement verification shrinks |
| P0 (Phase 1-4) | All of M-1 ~ M-6 | **No.** Minimum success disappears |

**Minimum success M-1 ~ M-6 holds with P0 alone.** That is the definition of P0. It is why the
minimal `DATA` round trip (M-5) and the local record (M-6) are placed in P0.

---

## Success criteria

#### Supported network conditions (defined in advance)

The "supported NAT environment" of M-4 is not decided after the fact. The following is fixed **before** the success verdict.

| Item | Definition |
|------|------|
| Supported condition | Each peer, when querying two different STUN servers, observes the same public IP:Port for itself. That is, the mapping does not change with the destination (endpoint-independent mapping) |
| Required test topologies | (1) same LAN, (2) two different home networks. M-4 must hold in both for minimum success to be recognized |
| Best-effort topologies | Mobile hotspot, campus network. Failure does not affect the minimum success verdict, but the failure is recorded |
| Unsupported condition | A mapping whose public port changes per destination (symmetric behavior). The classification criterion is observed behavior, not carrier configuration. CGNAT is by definition classified not as unsupported but as a topology that may be difficult. Some CGNATs use endpoint-independent mapping and hole punching works. Direct connection failure is treated as normal behavior and only recorded |

### Minimum success (Minimum)

All must be met.

| # | Criterion | Verification method |
|---|------|-----------|
| M-1 | The client runs on two Windows machines on different networks. | There are two verdicts. **Normal startup** means that each machine's log shows **the actual local endpoint that was bound** and the process does not exit afterwards. That value is the contract in [`protocol.md`](protocol.md) chapter 6 (bind to `INADDR_ANY` and port 0, read the actual port with `getsockname`). **Different networks** means that the public IPs in the M-2 logs differ between the two machines. If the public IP is the same, this criterion does not recognize them as different networks. Different networks behind a shared CGNAT can appear with the same public IP, so we do not assert "behind the same NAT". The startup verdict does not use the STUN result because M-1 would then become a subset of M-2. Then "it started but STUN does not work" would be visible in no criterion |
| M-2 | Both machines discover a usable public endpoint with their own STUN client. | Each machine's log prints the local endpoint and the public endpoint. The public endpoint is confirmed by **querying two different STUN servers from the same UDP socket** and observing the same IP:Port (the same verdict as the supported condition above). HTTPS-based IP check services are a TCP path that may differ from the UDP transmission path, so they are **recorded for reference only.** That the discovered IP:Port is actually reachable is confirmed in M-4/M-5 |
| M-3 | The two peers exchange connection information through the AWS control plane. | Both machines receive, in the `get_peers` ready response, the peer's virtual IP and **the public endpoint printed in the peer machine's M-2 log**. The response format is set by [`control_plane.md`](control_plane.md) 4.5, and it is read from `virtual_ip` and `candidates` of the `control.peers` event ([`architecture.md`](architecture.md) chapter 9) in the client log. `candidates` must contain the peer's `stun.result` `mapped` value. **The verdict is made from the logs of both machines separately.** Only one side receiving it is not an exchange |
| M-4 | A direct UDP connection is established in a supported NAT environment without manual port forwarding. | The session reaches `CONNECTED` with no router port forwarding rule. Packet capture confirms the path does not go through AWS |
| M-5 | Inner IP packets are transferred over the direct tunnel. | A `DATA` round trip succeeds in both directions and received bytes match sent bytes. The payload is a **complete inner IPv4 packet** as defined by [`protocol.md`](protocol.md) 5.4. In Phase 4 the verdict uses synthesized packets without Wintun; carrying real application traffic is confirmed in T-3 |
| M-6 | Connection state and basic performance data are recorded. | The client writes the connection result (success or an FR-13 failure code) and RTT to a **local record file**. The contract is in [`architecture.md`](architecture.md) chapter 9. Two things are subject to the verdict. **An attempt that fails to establish is one line; an attempt that reaches `CONNECTED` is two lines, an establishment line and a termination line**, and **an empty RTT is normal when there is no sample** (`PING` starts after `CONNECTED`, so the establishment line usually has no sample). An empty RTT is not judged a failure. The path and line format of that file are not decided yet, and **this criterion cannot be judged until they are.** Control plane upload and per-environment aggregation are FR-14 and Phase 9 |

### Target success (Target)

All must be met in addition to minimum success.

| # | Criterion | Verification method |
|---|------|-----------|
| T-1 | A virtual adapter comes up on both Windows machines. | `ipconfig` shows the virtual adapter and the assigned virtual IP |
| T-2 | Virtual IPs are assigned. | Host `10.100.0.1`, player `10.100.0.2` confirmed |
| T-3 | IP packets are routed through the P2P tunnel. | `ping` to the peer's virtual IP succeeds, tunnel counters increase |
| T-4 | A real Minecraft Java Edition multiplayer session is established over the virtual network. | The player connects to `10.100.0.1:25565`, logs in, and enters the world |
| T-5 | Sustained gameplay is maintained beyond the initial connection. | No forced termination during 30 or more minutes of continuous play |
| T-6 | Latency, packet loss, connection time, and session stability are measured. | A dataset of RTT, loss rate, establishment time, and duration is obtained for whole sessions |

### Analysis success (CSP requirement)

| # | Criterion | Verification method |
|---|------|-----------|
| A-1 | Repeated experiments are performed across multiple network environments. | Measurement results for each of: same LAN, home network to home network, home network to mobile hotspot, campus network to home network |
| A-2 | Successful and failed NAT traversal cases are compared. | Each failure keeps its per-stage code and NAT environment information, aggregated by observed mapping behavior. NAT type is not asserted. **Both controlled failure scenarios** are run. Blocking UDP with the firewall must record `STUN_DISCOVERY_FAILED`, and injecting an unresponsive endpoint must record `HOLE_PUNCH_TIMEOUT`. **Write down what each is evidence of, and keep them apart.** A controlled failure is evidence that the failure handling path works, not evidence about the failure rate or failure distribution in real environments. The failure rate comes only from the repeated measurements of A-1. **If there are zero real failures, write zero.** Do not substitute controlled failures |
| A-3 | Experiment results are visualized and limitations are documented. | [`experiments.md`](experiments.md) contains graphs and a statement of limitations |
| A-4 | A repeatable experiment procedure exists. | The pass condition is that **one person who is not the author** **performs one reproduction to completion** from the document alone. The reproducer proceeds without asking for explanation outside the document and records where they got stuck. **Do not declare "reproducible" without a trial.** The author finds missing premises in their own document only with difficulty by reading alone |
