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
| `network/udp_socket` | Winsock2 init/teardown, UDP socket creation, non-blocking send/receive, `WSAPoll`-based waiting | Winsock2 |
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

The client is a single process. The initial version assumes the following thread layout.

```text
[main]        initialization, state machine, shutdown
[net_rx]      UDP socket receive loop -> tunnel dispatch
[tun_rx]      Wintun read loop -> routing -> UDP send
[timer]       keepalive, PING/PONG, retries, telemetry flush
```

In Phases 1 through 5 there is no `[tun_rx]`; `[main]` feeds console input instead.

**All UDP sends and receives share one local socket.** If the STUN Binding Request, `HELLO`, `HELLO_ACK`, `KEEPALIVE`, `PING`/`PONG`, and `DATA` use different sockets, the NAT maps a different public port for each, so the endpoint discovered through STUN does not apply to the tunnel. This constraint must hold from Phase 2 onward.

### 3.2 Control Plane (Python)

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
 - 16 (TunnelHeader)
= 1456 bytes (maximum inner IP packet)
```

Set the virtual adapter MTU down to around 1400 to avoid fragmentation.

Note that the calculation above depends on the **assumption that the outer path MTU is 1500**. Paths over PPPoE lines or through another tunnel are smaller, and in that case the outer UDP packet fragments. The exact value is fixed by measurement in Phase 7; if a conservative fixed value is used, state that limitation in [`experiments.md`](experiments.md).

---

## 5. Tunnel Protocol

### 5.1 Header

```cpp
struct TunnelHeader
{
    uint32_t magic;           // protocol identifier, rejects misdelivered packets
    uint8_t  version;         // protocol version
    uint8_t  type;            // PacketType
    uint32_t peer_id;         // sending peer identifier
    uint32_t sequence;        // for loss/reordering measurement
    uint16_t payload_length;  // payload bytes that follow
};                            // wire format 16 bytes, network byte order
```

The wire format is 16 bytes, but `sizeof(TunnelHeader)` is not 16. Under MSVC default alignment, 2 bytes of padding are inserted after `type`, making it 20. Do not `memcpy` the struct as-is and do not use `sizeof` as the header length. **Serialize field by field** and fix the header length as a constant.

The actual values of `magic` and `version`, and the field widths and layout of the `HELLO` / `HELLO_ACK` payloads, are fixed at the start of Phase 4 and recorded in [`protocol.md`](protocol.md), which is therefore created in Phase 4 and extended in Phase 5. Without them the packet capture verification in Phase 4 has no basis.

`payload_length` is used for receiver-side validation. If it disagrees with the actual byte count received, the packet is dropped and a counter is incremented.

### 5.2 Packet Types

| Type | Direction | Payload | Purpose |
|------|-----------|---------|---------|
| `HELLO` | both | peer ID, virtual IP, session nonce | Hole punching attempt and handshake initiation |
| `HELLO_ACK` | both | echo of the peer's `HELLO` nonce | Proves the peer actually received my packet |
| `KEEPALIVE` | both | none | Maintains the NAT mapping (interval: 15-25 s) |
| `DATA` | both | original IP packet | Carries game traffic |
| `PING` | both | send timestamp | RTT measurement |
| `PONG` | both | echo of the original timestamp | RTT measurement reply |

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

- `HANDSHAKING -> CONNECTED`: requires a `HELLO_ACK` carrying the nonce from my own `HELLO`. The fact that I sent a `HELLO` and received the peer's `HELLO` says nothing about whether the peer received mine. Hole punching can open in one direction only.
- `CONNECTED -> FAILED`: decided not by keepalive send failure, but by receiving no valid packet from the peer for a set interval (roughly 3x the keepalive period).

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
| Keepalive local send errors | timer thread | stability analysis. This counts local `sendto` errors only; it is not evidence of delivery failure, which is why disconnection is decided by idle timeout instead |
| Idle timeout events | receive path | stability analysis |
| Direct connection uptime | duration in `CONNECTED` | stability analysis |
| Tunnel throughput | send/receive byte counters | performance analysis |
| Session duration | full game session | Minecraft stability |

The client buffers locally first and uploads to the control plane periodically. Upload failures are ignored and retried on the next cycle so that a control plane outage never affects the data plane.

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
|   |   +-- plan.md              checklist for the current work unit
|   |   +-- first_design.md      initial proposal (reference only)
|   |   +-- protocol.md          tunnel protocol detail (started in Phase 4, completed in Phase 5)
|   |   +-- experiments.md       experiment design and results (written in Phase 9)
|   |   +-- decisions/           design decision records
|   |   +-- commit_history/      per-work-unit change records
|   +-- kor/                 Korean documents, same structure
+-- README.md
```

Both language trees hold the same files. When a document changes, update both sides in the same commit.

### Difference from the current state

The source tree currently contains only `src/main.cpp` and `CMakeLists.txt` at the root (`docs/` is already populated). At the start of Phase 1 these move under `client/` and `control-server/` is created. This relocation is handled as the first task of Phase 1.

---

## 11. External Dependencies

| Dependency | Scope provided | Approval status |
|------------|----------------|-----------------|
| Wintun | Windows virtual network interface access only. Adapter creation, packet read/inject | **Approved (2026-09-14)** |
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
