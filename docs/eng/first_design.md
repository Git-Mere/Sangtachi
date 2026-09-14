# CSP 400 Project Proposal

> Korean version: [`../kor/first_design.md`](../kor/first_design.md)
>
> This is the initial proposal, not a fixed specification. Where it conflicts with
> [`spec.md`](spec.md), [`architecture.md`](architecture.md), or [`roadmap.md`](roadmap.md),
> those documents take precedence.

## Project Title
**Direct-First P2P Virtual Network for Multiplayer Games**

## Student
Sam Jeon

## Course
CSP 400: Computer Science Project — Fall 2026

---

## 1. Project Overview

This project will develop a **Windows-first peer-to-peer virtual networking system** that allows users to join multiplayer game servers without manually configuring router port forwarding.

The system will create a private virtual network between participating computers. Each client will receive a virtual IP address, and game traffic sent to that virtual address will be captured, encapsulated, transmitted through a direct peer-to-peer tunnel when possible, and injected into the remote machine's virtual network interface.

The initial real-world validation target will be **Minecraft Java Edition**. A successful project should allow a player to connect to a Minecraft server running on another user's computer through a virtual IP address without requiring manual router configuration.

The project will prioritize **direct peer-to-peer connectivity**. AWS will be used primarily as a **control plane** for peer coordination, connection metadata, and telemetry rather than as the normal path for game traffic.

---

## 2. Problem Statement

Hosting multiplayer games from a home network often requires users to configure router port forwarding, understand public and private IP addresses, modify firewall rules, or use a commercial virtual LAN service. These requirements create a significant usability barrier for ordinary users.

The project investigates whether a lightweight user-space networking system can automatically establish connectivity between players by combining:

- Virtual network interfaces
- STUN-based public endpoint discovery
- NAT traversal
- UDP hole punching
- Peer-to-peer tunneling
- Cloud-based peer coordination
- Network quality measurement and diagnostics

The main technical challenge is to provide a virtual LAN-like experience while keeping the actual game traffic on a direct peer-to-peer path whenever possible.

---

## 3. Project Goals

### Primary Goals

1. **Playable**  
   The system must support sustained real multiplayer gameplay rather than only proving that two clients can exchange packets.

2. **Direct-First**  
   Game traffic should travel directly between peers whenever NAT conditions permit.

3. **No Manual Port Forwarding**  
   A normal user should not need to configure router port forwarding to host or join a supported game session.

4. **Virtual LAN Experience**  
   Users should communicate using virtual IP addresses rather than manually using discovered public endpoints.

5. **Observable**  
   The system should record connection establishment results and relevant network performance data.

6. **Diagnosable Failure**  
   A failed connection should be categorized by stage, such as STUN failure, candidate exchange failure, hole-punching failure, or tunnel failure.

7. **Game-Agnostic Networking Layer**  
   Minecraft Java Edition will be used as the first validation target, but the tunnel itself should not contain Minecraft-specific networking logic.

---

## 4. Non-Goals for the Initial Version

The first version will intentionally avoid several features to keep the project achievable within one semester.

- Full replacement for commercial VPN products
- Large-scale public deployment
- Custom cryptographic algorithm development
- Mobile support
- macOS support
- Advanced graphical user interface
- Full ICE implementation in the first milestone
- Guaranteed connectivity through every NAT configuration

Relay fallback may be added later as a stretch goal if direct connection work is completed early enough.

---

## 5. Target Platform

The project will initially assume a **Windows environment**.

### Initial Target

- Windows 10 / Windows 11
- x64
- C++20 client
- Windows Sockets API
- Virtual network adapter through Wintun, subject to instructor approval

The project may later add Linux support if time allows, but Windows will be the primary implementation and demonstration environment.

---

## 6. Proposed Technology Stack

### Client / Data Plane

**Language:** C++20

Planned responsibilities:

- UDP socket management
- STUN client implementation
- Public endpoint discovery
- Peer session management
- NAT hole punching
- Tunnel packet protocol
- Keepalive and connection state
- Virtual network adapter interaction
- Virtual IP routing
- Packet encapsulation and decapsulation
- Telemetry collection
- Diagnostics

Where practical, the networking logic will use native Windows APIs instead of high-level networking frameworks.

### Windows Networking APIs

Possible APIs include:

- Winsock2
- `socket()`
- `bind()`
- `sendto()`
- `recvfrom()`
- `select()` or `WSAPoll()`

### Virtual Network Adapter

**Planned dependency:** Wintun

Wintun would only provide access to a Windows virtual network interface. The project would still implement its own:

- Peer discovery
- STUN client logic
- NAT traversal
- Hole punching
- Tunnel protocol
- Routing logic
- Session management
- Monitoring
- Diagnostics

Because CSP 400 requires prior approval for non-basic third-party libraries, Wintun will be treated as an instructor-approved infrastructure dependency rather than as the central technical contribution.

### Control Plane

**Language:** Python

Initial responsibilities:

- Room creation
- Room joining
- Peer registration
- Virtual IP assignment
- Endpoint/candidate exchange
- Connection metadata
- Telemetry ingestion

The first version should remain intentionally simple.

Possible initial stack:

- Python standard library
- `socket`
- `asyncio`
- `json`
- `sqlite3`

A higher-level web framework may be considered later if needed and approved.

### Cloud

**Initial deployment target:** AWS EC2

The first architecture will likely use:

- One EC2 instance
- Python control server
- SQLite database or lightweight persistent storage

The initial cloud component should remain simple because the central technical work of the project is peer-to-peer networking rather than backend web development.

---

## 7. High-Level Architecture

```text
                         AWS Control Plane
                    +-----------------------+
                    | Python Coordination   |
                    |                       |
                    | Rooms                 |
                    | Peer Registry         |
                    | Candidate Exchange    |
                    | Telemetry             |
                    +-----------+-----------+
                                |
                      coordination only
                                |
              +-----------------+-----------------+
              |                                   |
              |                                   |
      +-------v--------+                  +-------v--------+
      | Windows Client |                  | Windows Client |
      |      A         |                  |      B         |
      |                |                  |                |
      | Wintun Adapter |                  | Wintun Adapter |
      | Routing        |                  | Routing        |
      | UDP Tunnel     |<================>| UDP Tunnel     |
      | Hole Punching  |    Direct P2P    | Hole Punching  |
      | STUN Client    |                  | STUN Client    |
      | Telemetry      |                  | Telemetry      |
      +-------+--------+                  +-------+--------+
              |                                   |
        Minecraft Host                      Minecraft Client
        10.100.0.1                           10.100.0.2
```

The AWS control plane will coordinate peers, but the preferred data path will be:

```text
Game
  -> Virtual Adapter
  -> C++ Client
  -> UDP Tunnel
  -> Internet
  -> Remote C++ Client
  -> Remote Virtual Adapter
  -> Game
```

---

## 8. STUN Scope

The project will initially implement a **STUN client**, not a complete STUN server.

The client will:

1. Construct a STUN Binding Request.
2. Send the request to an existing STUN server over UDP.
3. Receive the Binding Response.
4. Parse the STUN response.
5. Decode `XOR-MAPPED-ADDRESS`.
6. Determine the client's public IP address and public UDP port.

Example:

```text
Local endpoint:
192.168.1.10:50000

NAT mapping discovered through STUN:
73.12.34.56:42001
```

The discovered public endpoint will then be registered with the AWS control plane so other peers can attempt direct connectivity.

Implementing a custom STUN server is not part of the initial project scope, although it may be considered later if it contributes useful experimental value.

---

## 9. NAT Traversal and Peer Connection

After public endpoint discovery, each peer will send its connection information to the AWS control plane.

Example:

```text
Alice
Virtual IP: 10.100.0.1
Public endpoint: 73.x.x.x:42001

Bob
Virtual IP: 10.100.0.2
Public endpoint: 24.x.x.x:51032
```

The control plane will exchange these endpoints between peers.

The clients will then attempt UDP hole punching:

```text
Alice                              Bob
  |                                 |
  | ------ UDP packet ------------> |
  | <----- UDP packet ------------- |
  |                                 |
  +========= Direct P2P ============+
```

The project will begin with a minimal connection-establishment procedure rather than immediately implementing the full ICE specification.

A later version may compare the custom procedure with ICE concepts such as host candidates, server-reflexive candidates, connectivity checks, and relay fallback.

---

## 10. Virtual Network Design

Each participant will receive a virtual IP address within the project network.

Example:

```text
Host:   10.100.0.1
Player: 10.100.0.2
```

A player should be able to enter:

```text
10.100.0.1:25565
```

into Minecraft and connect to the remote host.

The virtual adapter will capture the game's IP packets. The client will then encapsulate those packets inside the project's UDP tunnel protocol.

Conceptually:

```text
Minecraft TCP packet
        |
        v
Virtual Network Adapter
        |
        v
Original IP packet
        |
        v
Project Tunnel Header + IP Packet
        |
        v
UDP
        |
        v
Internet
```

At the remote peer, the process is reversed and the original IP packet is injected into the virtual adapter.

Minecraft Java Edition uses TCP for gameplay, but the tunnel itself can use UDP because the original TCP packets are being transported inside the tunnel.

---

## 11. Tunnel Protocol

A small custom packet header will be defined for communication between project clients.

Possible fields:

```cpp
struct TunnelHeader
{
    uint32_t magic;
    uint8_t  version;
    uint8_t  type;
    uint32_t peer_id;
    uint32_t sequence;
    uint16_t payload_length;
};
```

Possible packet types:

- `HELLO`
- `KEEPALIVE`
- `DATA`
- `PING`
- `PONG`

This protocol can support both tunneled game packets and connection-quality measurements.

---

## 12. Control Plane Responsibilities

The control plane will be separated from the actual game data path.

Initial operations may include:

```text
create_room
join_room
register_peer
register_candidate
get_peers
report_connection
report_telemetry
```

A typical connection flow will be:

```text
Host starts client
    -> connect to AWS
    -> create room
    -> receive virtual IP
    -> run STUN
    -> register public endpoint

Player starts client
    -> connect to AWS
    -> join room
    -> receive virtual IP
    -> run STUN
    -> register public endpoint

AWS
    -> exchange peer endpoints

Clients
    -> perform UDP hole punching
    -> establish direct tunnel
    -> begin virtual network traffic
```

---

## 13. Telemetry and Analysis

The project will collect enough data to evaluate both connection reliability and network quality.

Planned metrics include:

- Connection establishment success/failure
- Connection establishment time
- Round-trip time (RTT)
- Packet loss
- Jitter
- Keepalive failures
- Direct-connection duration
- Tunnel throughput
- Minecraft session stability

Possible test environments include:

- Same local network
- Home network to another home network
- Home network to mobile hotspot
- Campus network to home network
- Different NAT behaviors when available

Each failed connection should also record the stage at which it failed.

Example categories:

```text
STUN_DISCOVERY_FAILED
CONTROL_PLANE_EXCHANGE_FAILED
HOLE_PUNCH_TIMEOUT
PEER_HANDSHAKE_FAILED
TUNNEL_DROPPED
```

---

## 14. Success Criteria

### Minimum Successful Project

The minimum acceptable version should demonstrate all of the following:

1. Two Windows machines run the project client.
2. Both machines discover usable public endpoints through the custom STUN client.
3. Both peers exchange connection information through the AWS control plane.
4. The peers establish a direct UDP connection without manual port forwarding in at least supported NAT environments.
5. The system transfers arbitrary packets or application data over the direct tunnel.
6. Connection status and basic performance data are recorded.

### Target Successful Project

The target version should additionally demonstrate:

1. A virtual adapter on both Windows machines.
2. Virtual IP assignment.
3. Routing of IP packets through the P2P tunnel.
4. A real Minecraft Java Edition multiplayer session over the virtual network.
5. Sustained gameplay rather than only initial connection success.
6. Measured latency, packet loss, connection time, and session stability.

### Stretch Goals

- Relay fallback for peers that cannot establish a direct connection
- Encryption and authenticated peer sessions
- Windows installer
- Simple GUI
- Automatic reconnect
- More than two peers in one virtual network
- UDP-based game validation
- Linux interoperability

---

## 15. Development Plan

The development plan is intentionally ordered by technical risk. The earliest work focuses on proving that direct peer-to-peer connectivity is possible before investing significant time in the virtual adapter or user interface.

### Phase 1 — Core UDP Networking

**Goal:** Establish basic networking primitives in C++.

Tasks:

- Create C++20 project structure with CMake
- Implement Winsock initialization and cleanup
- Build UDP socket wrapper
- Implement endpoint representation
- Send and receive UDP packets between two known endpoints
- Add basic logging

Deliverable:

```text
Client A <------ UDP ------> Client B
```

---

### Phase 2 — STUN Client

**Goal:** Discover the public UDP endpoint of a client behind NAT.

Tasks:

- Study the required portions of STUN
- Construct Binding Request packets
- Generate transaction IDs
- Send request to public STUN server
- Parse Binding Response
- Parse STUN attributes
- Decode `XOR-MAPPED-ADDRESS`
- Display discovered public IP and port
- Add timeout and invalid-response handling

Deliverable:

```text
Local endpoint: 192.168.x.x:xxxxx
Public endpoint: x.x.x.x:xxxxx
```

---

### Phase 3 — AWS Coordination Server

**Goal:** Allow peers to find one another and exchange candidate information.

Tasks:

- Deploy basic Python server to AWS EC2
- Implement room creation
- Implement room joining
- Register peers
- Register virtual IPs
- Register public endpoints
- Exchange peer information
- Store lightweight room/peer state

Deliverable:

Two clients in different networks can obtain each other's endpoint through the AWS control plane.

---

### Phase 4 — UDP Hole Punching

**Goal:** Establish a direct peer-to-peer UDP path without manual port forwarding.

Tasks:

- Design peer handshake
- Simultaneously send connection packets to remote endpoint
- Add retries and timeout logic
- Confirm bidirectional traffic
- Add keepalive packets
- Measure RTT with `PING` / `PONG`
- Record success or failure reason

Deliverable:

```text
PC A <========== Direct UDP ==========> PC B
```

with no manually configured router port forwarding in supported environments.

---

### Phase 5 — Tunnel Protocol

**Goal:** Create a stable transport layer for project packets.

Tasks:

- Define tunnel packet header
- Add protocol version
- Add peer/session identifier
- Add packet type
- Add sequence number
- Implement serialization/deserialization
- Add malformed-packet validation
- Implement `DATA`, `KEEPALIVE`, `PING`, and `PONG`

Deliverable:

A stable application-level tunnel exists between two peers.

---

### Phase 6 — Windows Virtual Network Adapter

**Goal:** Capture and inject IP packets through a virtual interface.

Tasks:

- Obtain approval for Wintun dependency
- Create/open virtual adapter
- Assign virtual IP address
- Read packets from the adapter
- Inject packets into the adapter
- Configure required Windows routes
- Validate traffic using simple IP tools before testing a game

Deliverable:

```text
10.100.0.1 <------ virtual network ------> 10.100.0.2
```

---

### Phase 7 — Integrate Virtual Adapter with P2P Tunnel

**Goal:** Transport virtual-network IP packets between peers.

Tasks:

- Read outgoing IP packet from Wintun
- Determine destination peer from virtual IP
- Encapsulate packet in tunnel `DATA` packet
- Send to peer over direct UDP connection
- Decapsulate received packet
- Inject original IP packet into Wintun
- Handle packet size limits
- Add counters and diagnostics

Deliverable:

The two Windows machines can communicate using only their virtual IP addresses.

---

### Phase 8 — Minecraft Validation

**Goal:** Demonstrate a real multiplayer session.

Tasks:

- Run Minecraft Java server on host
- Connect player to host virtual IP
- Verify login and world join
- Play for an extended period
- Measure latency and disconnect behavior
- Record packet and connection metrics

Primary demo:

```text
Minecraft Client
      |
      | Connect to 10.100.0.1:25565
      v
Project Virtual Network
      v
Minecraft Server
```

Success means that multiplayer gameplay is sustained without manual port forwarding.

---

### Phase 9 — Monitoring and Experimental Evaluation

**Goal:** Produce measurable CSP analysis rather than only a working demo.

Tasks:

- Record connection establishment time
- Record RTT
- Record packet loss
- Estimate jitter
- Record tunnel traffic volume
- Record session duration
- Test multiple network environments
- Compare successful and failed NAT traversal cases
- Visualize experimental results
- Document limitations

Deliverable:

A repeatable experiment set and data suitable for the final written analysis and presentation.

---

## 16. Tentative Milestone Plan

The course syllabus places project milestones approximately at Weeks 5, 10, and 15. The implementation plan is therefore divided around those checkpoints.

### Milestone 1 — Networking Proof of Concept

Target scope:

- C++ UDP networking
- Custom STUN client
- AWS peer coordination prototype
- Endpoint exchange
- UDP hole punching
- Basic direct P2P messaging
- Initial RTT measurement

Ideal demonstration:

```text
Two Windows computers
Different networks
No manual port forwarding
Direct UDP connection established
Messages successfully exchanged
```

This milestone should prove the highest-risk networking assumption before the virtual network layer is added.

### Milestone 2 — Functional Virtual Network

Target scope:

- Tunnel protocol
- Wintun integration
- Virtual IP assignment
- Packet capture/injection
- Direct P2P packet forwarding
- Virtual-IP communication
- Minecraft integration attempt

Ideal demonstration:

```text
Player -> 10.100.0.1:25565 -> Direct P2P Tunnel -> Minecraft Host
```

The preferred Milestone 2 result is a successful Minecraft session.

### Milestone 3 — Final System and Analysis

Target scope:

- Stable Minecraft gameplay
- Connection diagnostics
- Telemetry collection
- Multiple network-environment tests
- Performance analysis
- Reliability analysis
- Project polish
- Final report and presentation

If time permits, this phase may also add a relay fallback, encryption, GUI improvements, or additional peer support.

---

## 17. Risk Management

### Risk 1 — NAT Hole Punching Does Not Work in All Networks

Mitigation:

- Test early before building higher layers
- Record NAT-related failures instead of hiding them
- Treat relay fallback as a possible later solution
- Define supported and unsupported network conditions clearly

### Risk 2 — Windows Virtual Networking Is More Complex Than Expected

Mitigation:

- Keep virtual adapter work separate from NAT traversal work
- First validate direct P2P using ordinary application data
- Use Wintun only for virtual-interface access
- Keep routing logic under project control

### Risk 3 — Project Scope Becomes Too Large

Mitigation:

Priority order:

```text
P0: UDP + STUN + endpoint exchange + hole punching
P1: Direct peer session + tunnel protocol
P2: Wintun + virtual IP + Minecraft
P3: Telemetry + diagnostics + analysis
P4: Encryption + relay + GUI + extra platform support
```

Lower-priority features will be removed before compromising the core P2P networking objective.

### Risk 4 — Third-Party Library Restrictions

Mitigation:

- Request approval before using Wintun or any other non-basic dependency
- Keep the project's core networking algorithms independent of those dependencies
- Document exactly what each approved library provides

---

## 18. Proposed Repository Structure

```text
csp-p2p-network/
|
+-- client/
|   +-- include/
|   |
|   +-- src/
|   |   +-- main.cpp
|   |   |
|   |   +-- network/
|   |   |   +-- udp_socket.cpp
|   |   |   +-- endpoint.cpp
|   |   |   +-- stun_client.cpp
|   |   |
|   |   +-- peer/
|   |   |   +-- peer.cpp
|   |   |   +-- hole_punch.cpp
|   |   |   +-- session.cpp
|   |   |
|   |   +-- tunnel/
|   |   |   +-- packet.cpp
|   |   |   +-- tunnel.cpp
|   |   |   +-- router.cpp
|   |   |
|   |   +-- adapter/
|   |   |   +-- wintun_adapter.cpp
|   |   |
|   |   +-- telemetry/
|   |       +-- telemetry.cpp
|   |
|   +-- CMakeLists.txt
|
+-- control-server/
|   +-- server.py
|   +-- room.py
|   +-- peer.py
|   +-- telemetry.py
|
+-- tests/
|
+-- docs/
|   +-- architecture.md
|   +-- protocol.md
|   +-- experiments.md
|
+-- README.md
```

---

## 19. Expected Technical Contributions

The main technical contribution of the project is not simply the use of a VPN or cloud library. The project is expected to directly implement and study several networking mechanisms:

- STUN Binding Request/Response handling
- Public endpoint discovery
- NAT traversal
- UDP hole punching
- Peer handshake design
- Direct peer session management
- Custom tunnel packet format
- Virtual-IP routing
- Packet encapsulation and decapsulation
- Connection diagnostics
- Network performance measurement

These components provide enough technical depth for comparison, experimentation, and performance analysis rather than only application-level integration.

---

## 20. Expected Final Demonstration

The ideal final demo will involve two Windows computers on different networks.

### Host

```text
1. Start project client
2. Create virtual network
3. Receive virtual IP 10.100.0.1
4. Start Minecraft Java server
```

### Player

```text
1. Start project client
2. Join host network
3. Receive virtual IP 10.100.0.2
4. Open Minecraft
5. Connect to 10.100.0.1:25565
```

### Behind the Scenes

```text
STUN discovery
      -> AWS endpoint exchange
      -> UDP hole punching
      -> Direct P2P tunnel
      -> Wintun packet transport
      -> Minecraft gameplay
```

The router on the host side should not require manual port forwarding.

---

## 21. One-Sentence Project Definition

> **A direct-first peer-to-peer virtual networking system for Windows that uses STUN-assisted NAT traversal and UDP hole punching to let players access multiplayer game servers through virtual IP addresses without manual port forwarding, while using AWS only for coordination and telemetry.**

---

## 22. Course Alignment

This proposal is intentionally structured around the CSP 400 emphasis on implementing computing solutions, comparing and analyzing them, iterating toward technical requirements, and presenting measurable outcomes. The development plan also follows the course's approximate Week 5, Week 10, and Week 15 milestone structure.

The project will maintain source control and weekly development records. Any use of non-basic third-party libraries, particularly Wintun, will be requested for instructor approval before integration. If AI tools assist with code or documentation, generated material will be tested, understood, validated, and cited in accordance with the course policy.

---

## 23. Immediate Next Steps

The first implementation sprint should focus only on the highest-risk networking path:

```text
C++ UDP socket
    -> STUN Binding Request
    -> public endpoint discovery
    -> AWS endpoint exchange
    -> UDP hole punching
    -> direct bidirectional peer communication
```

Only after this path works reliably should development move to Wintun and Minecraft traffic tunneling.
