# Roadmap

**Project:** Direct-First P2P Virtual Network for Multiplayer Games
**Term:** CSP 400, Fall 2026
**Requirements:** [`spec.md`](spec.md) / **Design:** [`architecture.md`](architecture.md) / **Protocol:** [`protocol.md`](protocol.md)

> Korean version: [`../kor/roadmap.md`](../kor/roadmap.md)

This document is the phase-level plan. What to do first right now is in [`plan.md`](../kor/plan.md).

---

## Requirement Traceability

Maps each ID in [`spec.md`](spec.md) to the phase that satisfies it. No ID may be left untraced.

| Phase | Requirements satisfied |
|-------|------------------------|
| 1 | FR-1, C-2 |
| 2 | FR-2, FR-3, NFR-4, NFR-9 |
| 3 | FR-4, FR-5, C-3 |
| 4 | FR-6, FR-7, FR-8 (up to the minimum `DATA` round trip), FR-13 (partial), NFR-9, M-1 to M-6 |
| 5 | FR-8 (loss and reordering accounting), FR-13, NFR-3 |
| 6 | FR-9, NFR-5, NFR-6, T-1, T-2 |
| 7 | FR-10, FR-11, NFR-2, NFR-6, T-3 |
| 8 | FR-12, T-4, T-5 |
| 9 | FR-14, NFR-7, NFR-10, T-6, A-1 to A-4 |
| All phases | NFR-1, NFR-8, C-1, C-4, C-5 |

**NFR-4 (own implementation) is not judged by a behavior test.** Whether we wrote the STUN,
NAT traversal, tunnel, routing, and session management ourselves is only visible by reading the
code. That item sits in the Phase 2 verification.

---

## Ordering Principle

Phases are ordered by **technical risk**, not by difficulty. The most uncertain assumption (does UDP
hole punching actually work across arbitrary NAT environments?) is validated first. The virtual
adapter and game integration start only after that assumption is proven.

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

- **Goal:** Establish the C++ networking fundamentals.
- **Priority:** P0
- **Requirements:** The [Requirement Traceability](#requirement-traceability) table is the source. Not repeated here

### Tasks

- Relocate the repository to the target layout (`src/` -> `client/src/`, create `control-server/`)
- Set up the C++20 project with CMake. The warning level and the test executable are fixed here
- Add the test executable and register it with CTest ([ADR 0005](decisions/0005-test-framework-catch2.md))
- Wrap Winsock2 initialization and teardown (`WSAStartup` / `WSACleanup`)
- Implement a UDP socket wrapper (`socket`, `bind`, `sendto`, `recvfrom`, `WSAEventSelect` event handle)
- Implement the event loop skeleton. `WaitForMultipleObjects`, timer deadline computation, drain pattern ([`architecture.md`](architecture.md) 3.2)
- Implement the endpoint representation type (parsing, comparison, printing)
- Send and receive UDP packets between two known endpoints
- Add basic logging

### Deliverable

```text
Client A <------ UDP ------> Client B
```

### Verification

- Bidirectional message exchange succeeds between two machines on the same LAN (or two processes on one machine)
  - The input and the judging means are the `--peer` argument and the `raw <byte count>` console
    command in [`architecture.md`](architecture.md) 3.5 Startup Inputs
  - When run on two machines, **each side sends once first**

  > **Why.** If only one side sends, that first datagram is unsolicited inbound to the other side and
  > is blocked exactly as measured in [`windows-prereq.md`](windows-prereq.md) 2.
- Received content matches sent content byte for byte
  - **The judgment is a comparison of `len` and `sha256` on the two `rx.raw` log lines**
    (`architecture.md` 9)
  - Do not compare bytes by eye
- On a socket error the process does not die and the error code is logged
- Console command queue cap (`architecture.md` 3.2.6 Telemetry Isolation). **Observe with the
  consumer stopped**
  - If `[loop]` drains the queue, pushing 17 lines never overflows. Use either a queue-unit test with
    consumption blocked or a test that deliberately holds `[loop]` and injects 17 lines
  - Under that condition the first 16 lines remain in order and the 17th is dropped and
    `console_queue_dropped` increments
  - An implementation that evicts the oldest entry is caught here

  > **Why.** Without pinning the condition, a correct implementation also fails.
- Confirm the bind contract ([`protocol.md`](protocol.md) 6). After binding to `INADDR_ANY` and port
  0, `getsockname` returns a **non-zero port** and that value is logged. Launching the same
  executable twice on one machine starts both (with a fixed port the second would fail)
- `SO_RCVBUF` contract (`protocol.md` 6). Three pass conditions
  - The requested value is `262144`. Read it from `rcvbuf_requested` in the log
  - The set call actually exists
  - The applied value is at least the one-round budget of 94208 bytes. Read it from `rcvbuf_applied`
  - Logging both values is the `socket.bind` contract in `architecture.md` 9

  > **Why.** Checking only the applied value lets an implementation that requested 128KB pass while
  > violating chapter 6.
- Multiple datagrams that arrived on a single signal are processed in the same round (drain pattern works)
- An oversized datagram does not break batching. **Run the two paths separately**
  (`architecture.md` 3.2.3 One Loop Iteration)
  - (a) Exactly 1473 bytes is caught by the length comparison (`n > MAX_DATAGRAM`)
  - (b) 1474 bytes or more is caught by `WSAEMSGSIZE`
  - Insert each between several normal datagrams. Check that the oversized one increments
    `drop_oversize_datagram` and that the normal datagrams that follow in the same round keep being
    processed

  > **Why.** Testing only (a) lets an implementation that ends the round on `WSAEMSGSIZE` pass.
- Periodic timers keep expiring while receive traffic never stops (drain budget works).
  **Pin "keep" to a value**
  - Criterion. With receive load above the budget applied continuously for 10 seconds, `elapsed_ms`
    of the test build's 200ms periodic timer (`timer.tick name=probe200`) does not exceed 400
    (`architecture.md` 9)
  - Read the interval from that field, not from a timestamp on the log line. The log format has no
    timestamp
  - Make the load actually exceed the budget. In the test build, add a per-datagram delay in
    `classify` to make the overrun deterministic, and record that delay value
  - Confirm that a mutant with the drain budget code removed `FAIL`s this criterion

  > **Why.** Without a numeric tolerance, a timer that fires in an incidental gap in the load passes,
  > so an implementation without a budget is not filtered. If the loopback generator cannot outrun
  > processing, the queue is empty every round and an implementation without a budget also passes.
  > If the mutant passes, this case does not yet protect anything.
- `cmake --build` succeeds with **zero warnings at MSVC `/W4`**. Without the level written down,
  the same sentence passes at the default warning level and "no warnings" means something
  different per compiler setting
  - Fix the level and `/WX` (warnings as errors) in the CMake configuration. That is what keeps a
    person from counting this item by eye
  - Apply it only to the targets of this repository. Applying it to an outside library ties the
    build to that version of the library

---

## Phase 2: STUN Client

- **Goal:** Discover the public UDP endpoint of a client behind NAT.
- **Priority:** P0
- **Requirements:** The [Requirement Traceability](#requirement-traceability) table is the source. Not repeated here

### Tasks

- Study the Binding Request/Response parts of RFC 5389
- Build the Binding Request packet (header, magic cookie)
- Generate transaction IDs and match responses
- Send requests to public STUN servers. **Which servers, how the two are chosen, and how they are
  switched on failure are decided by [`architecture.md`](architecture.md) 3.5 Startup Inputs.** Do
  not hard-code server addresses
- Handle startup inputs. The argument list and format are in `architecture.md` 3.5. This Phase needs
  `--stun`; `--server`, `--room`, and `--rejoin` are accepted and stored only
- Parse the Binding Response
- Iterate STUN attributes (TLV)
- Decode `XOR-MAPPED-ADDRESS`
- Print the discovered public IP and port
- Handle timeouts and malformed responses

### Deliverable

```text
INFO socket.bind local=0.0.0.0:51000 rcvbuf_requested=262144 rcvbuf_applied=262144
INFO stun.result server=stun.l.google.com:19302 mapped=x.x.x.x:51000
INFO stun.result server=stun1.l.google.com:19302 mapped=x.x.x.x:51000
```

The bound endpoint is `0.0.0.0:<port>` ([`protocol.md`](protocol.md) 6). An interface address
(`192.168.x.x`) is a **local candidate** of 10.1 Candidate Collection and Hygiene, and collecting it
is Phase 4.

### Verification

- The verdict for this Phase is **successful parsing of responses from two STUN servers.** Decoding
  `XOR-MAPPED-ADDRESS` from each response to obtain a public endpoint is a pass
  - Agreement with an external IP-check service is recorded for reference only. That service is a TCP
    path and may differ from the UDP send path. [`spec.md`](spec.md) M-2 keeps that observation as
    reference for the same reason
  - Which servers to query and how observations are recorded are set by the items below
- Responses with a different transaction ID are ignored
- Pointing at a non-responding address yields `STUN_DISCOVERY_FAILED` in the **log** of
  [`architecture.md`](architecture.md) 9 after the timeout, with no infinite wait. The M-6 local
  record file is a Phase 4 deliverable and does not exist in this Phase
- Truncated responses or a wrong magic cookie are rejected without a crash
- Query at least two public STUN servers and record each server-reflexive address. **Identical results are not required.** If servers differ, record the observation as destination-dependent mapping
- NFR-4 verdict. **Read the code** to confirm that STUN message construction, parsing, and retry are
  our own. An implementation that calls an external STUN library is caught here. A behavior test does
  not tell the two apart
- Confirm via log that the socket used for STUN is the same socket later used for hole punching and
  the tunnel. **This is reference only.** It is self-reported, so an implementation that opened a
  new socket can print the same line. The on-the-wire verdict belongs to Phase 4 verification, where
  capture is possible

---

## Phase 3: AWS Coordination Server

- **Goal:** Peers find each other and exchange candidate information.
- **Priority:** P0
- **Requirements:** The [Requirement Traceability](#requirement-traceability) table is the source. Not repeated here

### Tasks

- **The control plane schema is fixed by [`control_plane.md`](control_plane.md).** Operations,
  encoding, errors, identifiers, state transitions, and the DynamoDB table design (partition key,
  sort key, items, conditional writes, TTL) are there
  - This Phase implements that document. It does not design
  - If a value is missing from the document, fix the document first instead of deciding it in code
  - The decision to switch the store from SQLite and its cost are in
    [ADR 0004](decisions/0004-state-store-dynamodb.md)
- **Before starting, obtain an Elastic IP and a DNS name.** The client receives the DNS name and that
  name points to the Elastic IP (`control_plane.md` 3.2 Address,
  [`windows-prereq.md`](windows-prereq.md) 10). Actual values are not written in documents
- **Before starting, set the table name, region, and capacity values as deployment configuration.**
  Not hard-coded (`control_plane.md` 7.6)
- **Before starting, confirm two clock assumptions on the deployment instance.** `control_plane.md`
  7.4 states both as unverified assumptions. If they do not hold, fix that section
  - Whether the implementation of `time.get_clock_info('monotonic')` is
    `clock_gettime(CLOCK_MONOTONIC)`
  - Whether `/proc/sys/kernel/random/boot_id` changes across a reboot
- **Before starting, fix the EC2 credential method.** Access keys are not written in source or documents. The document recommends an IAM role (ADR 0004)
- **Before starting, check the free tier coverage directly in the account.** Documents alone did not confirm whether 25 WCU / 25 RCU / 25 GB is permanent (ADR 0004 "Not confirmed")
- Local tests run on DynamoDB local. **The consistency path is not judged there, though.** Local reads usually look up to date, so a missing `ConsistentRead` does not show (ADR 0004)
- **At start, move the case tables of `control_plane.md` (2.1, 3.3, 4.4, 5.1, 6.4, 7.4) into
  `control-server/tests/` and change the document to point at those files.** A table kept in two
  places gets fixed in one and drifts. Attach a mutation test to each moved table
- Deploy the Python control server to AWS EC2. systemd service, security group TCP 8000. **Run the
  deployment procedure once for real, then put it in a tool and have the document point at it**
  (`control_plane.md` 7.6)
- Implement room creation (`create_room`, idempotency nonce)
- Implement room join (`join_room`, both the new-join and rejoin forms)
- Virtual IP allocation (`VIP#` conditional-write claim)
- Candidate endpoint registration (`register_candidate`, server-side sanitation, ready recorded once). Local candidates are used when both peers are on the same LAN
- Peer information exchange (`get_peers`, monotonic-clock `elapsed_since_ready_ms`)
- Per-source rate limiting and `MAX_INFLIGHT`
- Store room/peer state in DynamoDB
- Implement the C++ control plane client. The `[control]` thread and two queues
  ([`architecture.md`](architecture.md) 3.2.8 The `[control]` Thread), a minimal HTTP/1.1 client,
  error classification and retry (`control_plane.md` 8)
- Complete startup input handling. `--server`, `--room`, and `--rejoin` are actually used
  (`architecture.md` 3.5)

### Deliverable

Two clients on different networks obtain each other's endpoints through AWS.

### Verification

**Basic behavior.**

- The room creator receives `10.100.0.1` and the joiner `10.100.0.2`
- Joining the same room twice does not allocate a duplicate virtual IP
- The `get_peers` response includes the peer's virtual IP and public endpoint
- Room state survives a server restart. **Without any restore procedure,** the first request right
  after restart reads directly from DynamoDB ([`control_plane.md`](control_plane.md) 6.1 Storage
  Contracts, store 5)
- Joining a non-existent room yields a clear error response
- **The control server has no metrics collection endpoint.** Telemetry is a separate service and
  belongs to Phase 9 ([`architecture.md`](architecture.md) 3.4 Telemetry Service)

**Store contract and concurrency.**

- **Every place that reads room/peer items uses `ConsistentRead=true`** (`control_plane.md` 6.1
  store 1). Not only `get_peers`. Read the code directly. This cannot be judged by a behavioral test

  > **Why.** Eventually consistent reads usually return the latest value too, so the test passes even
  > when it is missing.
- Two peers joining **simultaneously** do not get overlapping virtual IPs. This is a different test from the sequential-join item above (store 3)
- Joining a room whose expiry time has passed yields `room_expired`. A non-existent room yields
  `room_not_found`. **They are different codes.** Do not wait for TTL deletion (store 4,
  `control_plane.md` 4.1 Common Envelope). Also check that the `now == expires_at_ms` boundary is on
  the expired side
- Rejoin (`control_plane.md` 4.3 join_room). Calling `join_room` again with the same `peer_id` and
  `peer_token` returns **the same `peer_id` and the same virtual IP**
  - That the candidates were cleared is seen in the peer's `get_peers` response. In that response the
    peer's `candidates` is an empty array and `ready` is `true`. The rejoin response itself carries no
    candidate field
  - A wrong token yields `unauthorized`
  - A non-existent `peer_id` is also `unauthorized`, not `room_not_found`
- Idempotency (`control_plane.md` 4.2 create_room, 4.3)
  - Calling `join_room` twice with the same `client_nonce` returns the same response the second time
    and **creates only one `VIP#` item**
  - A different nonce yields `room_full`
  - `create_room` with the same nonce is also the same room
- Ready is recorded **exactly once per room** (`control_plane.md` 4.4 register_candidate, 5.2 Ready)
  - Even when two peers send `register_candidate` at the same time, the server log has one
    `room.ready` line
  - Re-registering candidates by either peer afterwards does not move the reference point of
    `punch_delay_ms` and `elapsed_since_ready_ms`
  - Run every row of the race case table (4.4). The order cannot be forced from outside, so build each
    order with a delay injection point right after the write in the store layer
  - Watch especially for the case of 0 lines, not just more than one. An implementation that judges
    from a read taken before the `PEER#` update is caught on that row, and that is the last row of the
    table
- Even after a rejoin clears candidates, `get_peers` keeps `ready: true`. Ready is defined as the
  existence of `ready_at`. An implementation that recounts candidates is caught here
  (`control_plane.md` 4.5 get_peers)

**All rows of the case tables pass.** The `control_plane.md` sections below say **where each table
came from.** Once the task above has moved the tables into `control-server/tests/`, those files are
what gets run.

- All rows of the HTTP parsing case table (`control_plane.md` 3.3 HTTP Subset) pass. In particular,
  duplicate `Content-Length`, `Transfer-Encoding`, a 4097-byte body, and `HTTP/1.0` are rejected.
  For each row, a mutant with that rule removed `FAIL`s on that row
- All rows of the `room_id` normalization case table (`control_plane.md` 2.1 room_id) pass. An implementation that "corrects" `0` to `O` is caught
- All rows of the candidate sanitation case table (`control_plane.md` 4.4) pass. If all candidates
  are rejected the result is `bad_request`, not `ok`. Read the code to confirm that `ipaddress`
  predicates are not used for the verdict
- All rows of the rate-limit case table (`control_plane.md` 6.4 Rate Limit) pass. The 11th
  failing request is `rate_limited` and **no store call is made** (count calls into the store
  layer). Successes and `bad_request` are not counted
- The server log contains no `room_id` and no `peer_token` (`control_plane.md` 7.5 Logs and Counters)
  - Walk the normal path and every error path once, then `grep` the whole log for both values.
    **Must be 0 hits**
  - This is a necessary condition, not a sufficient one. That no path emits them is confirmed by
    reading the code

**Clock and logs.**

- `elapsed_since_ready_ms` comes from a monotonic clock. **Run this on the same Linux as the
  deployment target** (`control_plane.md` 7.4 Clocks)
  - Restart the server process after ready and call `get_peers`; the value continues and the
    `elapsed_wall_fallback` counter is 0
  - Moving the server wall clock forward by 1 minute does not make the value jump by 1 minute
  - Do not run this item on a local Windows machine. There a correct implementation also fails

  > **Why.** With no `boot_id`, falling back to the wall clock on every process restart is the
  > behavior set by the `control_plane.md` 7.4 case table, and a counter of 1 is the rule there.

**Client-side contracts.**

- The client's `[loop]` does not stall while the control server is down
  - Give a connection-refused or non-responding address as `--server` and check that the test build's
    `timer.tick name=probe200` keeps appearing (`architecture.md` 9)
  - **The verdict is the same value as in Phase 1.** `elapsed_ms` does not exceed 400 even while a
    control request is outstanding
  - Watch console command responses too, but do not judge on them alone. At human typing speed even a
    stall of tens of seconds looks like "it responded"
  - An implementation with `connect` on `[loop]` is caught here (`architecture.md` 3.2.8)
- Control plane retry on the client (`control_plane.md` 8.3 Error Classification and Retry)
  - On `rate_limited`, `room_full`, or `unauthorized` the client reports
    `CONTROL_PLANE_EXCHANGE_FAILED` immediately without retry
  - `internal`, `unavailable`, and connect timeout are attempted **3 times in total including the
    first attempt, with the same `client_nonce`**
  - The judging axis differs per operation. For `create_room`, check that only one room is created
    (`ROOM` item count 1)
  - For a new `join_room`, check that the second response has the same `peer_id` and virtual IP as the
    first. The `VIP#` item count does not decide it
  - Connect timeout and `unavailable` happen before the store is reached, so nonce sameness leaves no
    trace in the store. Put an injection point in the store layer that fails right after the commit
    and just before the response, and run the `internal` retry through it

  > **Why.** The `VIP#` item count does not decide it because the pool holds only one address, so even
  > a mutant that changed the nonce ends the second attempt with `room_full` and leaves one item.

---

## Phase 4: UDP Hole Punching

- **Goal:** Establish a direct P2P UDP path without manual port forwarding. **This is the riskiest phase of the project.**
- **Priority:** P0
- **Requirements:** The [Requirement Traceability](#requirement-traceability) table is the source. Not repeated here

### Tasks

**To be decided before starting.** Four items. Implementation of those places does not begin before
they are decided

- **Before starting, redesign the keepalive mapping-lifetime measurement procedure**

  - The form where EC2 sends a UDP probe to the client's public endpoint cannot be used. The client
    has never sent UDP to EC2 (the control plane is TCP), so that probe is unsolicited inbound
  - The part that creates the idle interval stays as is. Stop **all** UDP transmission on that socket,
    including `PING` and `DATA`. A test that turns off only keepalive is invalid because game and
    `PING` traffic refresh the mapping
  - Three things must be redefined. The transmission that opens the path just before idle, the probe
    format and the EC2-side sending tool, and how the client records arrival time
  - Also write down that the quantity measured is not "mapping lifetime" but "filter+mapping lifetime
    of that path"
  - The EC2-side sender is not a control server feature. Neither the control plane nor the telemetry
    service opens UDP ([`control_plane.md`](control_plane.md) 1.1 What It Does and Does Not Do), so
    this sender is a separate tool under `tools/` and is started on EC2 only when measuring. It is of
    the same kind as `tools/nat-probe`
  - The probe format is a matter for the chapter 7 classification in
    [`protocol.md`](protocol.md), so it is decided by fixing that document. The 15 seconds and 50
    seconds in chapter 11 are initial values until then

  > **Why.** The measurement in [`windows-prereq.md`](windows-prereq.md) 2 (all 6 unsolicited inbound
  > cases blocked) and the measurement in `protocol.md` 10.4 Endpoint Learning (all 3 pairs blocked
  > even when only the source port differs) predict zero arrivals. Then "the mapping expired" cannot
  > be distinguished from "the filter was closed from the start", so the mapping lifetime is misjudged
  > as 0.

- **Before starting, fix the criteria for a local candidate**

  - `protocol.md` 10.1 Candidate Collection and Hygiene says to exclude loopback, APIPA, our own
    Wintun, and other tunnel/VPN adapters from the "active IPv4 interfaces", but there is no judging
    input for the last two and for "active"
  - Decide whether it is the adapter type (`IfType`), the name string, or the presence of a gateway,
    and whether only `OperStatus` is read or address ownership too, then put a case table in that
    section
  - This is a judging function, so the case table comes first

  > **Why.** Including one wrongly sends `HELLO` to an unreachable address every 200ms; excluding one
  > wrongly removes the same-LAN candidate, and then the same-LAN verification above ends up relying
  > on hairpinning.
- **Before starting, re-measure with `tools/nat-probe`.** Confirm that the verdict "a direct
  connection is established" still holds. If conditions have changed, this is the point where there
  is still time to revert the design ([ADR 0001](decisions/0001-no-direct-connection-fallback.md))
- **Before starting, fix the path and line format of the local record file.**
  [`architecture.md`](architecture.md) 9 has only 4 contracts and no format. **M-6 cannot be judged
  until it is fixed.** Do not decide it in code; fix that document first

**Implementation.** Follow the `protocol.md` 15 implementation checklist as written. This Phase is
implementation, not design.

- Header serialization/deserialization (field by field, network byte order)
- Exclusive socket ownership by a single receive loop, STUN/tunnel demultiplexing (`protocol.md` 6-7)
- Required socket options: `SO_EXCLUSIVEADDRUSE`, `SIO_UDP_CONNRESET` off, no `connect()` call
- Receive validation pipeline and dedicated drop counters (`protocol.md` 7-8)
  - Chapter 7 source sanitation (before classification), 8.1 common checks 1-8, 8.4 inner validation
    9-15, and 8.5 sender-side checks 1-6 are all in this Phase
  - **In this Phase the input to 8.5 Transmit-Side Validation is a synthetic inner packet.** Wintun
    reads become the input from Phase 6, and the checks themselves are the same
- `session_epoch` generation and pinning
- Candidate gathering (local/server-reflexive) and control plane rendezvous polling (`protocol.md`
  10.1, 10.2). **This wires the control plane client built in Phase 3 into the session state
  machine.** The operation calls themselves are a Phase 3 deliverable; here their results become the
  candidate set and the punch time
- `HELLO` retransmission every 200ms to **all** of the peer's candidates, punch deadline 10 seconds
- Endpoint learning that updates `peer_endpoint` to the source of a packet that passed validation and advanced `sequence`, and probe-nonce path verification for a new address (`protocol.md` 10.4)
- Dual-flag `CONNECTED` condition, the full 9.4 transition table, 9.4.1 terminal-state discard
- The four caps in chapter 3 and the two renegotiation caps in 9.5 (`protocol.md` 3, 4.3, 9.5, 10.4)
- `KEEPALIVE` every 15 seconds (one immediately on entering `CONNECTED`), disconnect verdict based on the 50-second idle timeout
- `PING`/`PONG` RTT measurement with `ping_id`
- `CLOSE` send and receive
- Minimal `DATA` type implementation and round trip. The payload is the **complete inner IPv4 packet**
  defined in `protocol.md` 5.4 `DATA`. Without Wintun, build and insert a synthetic IPv4 packet.
  Virtual IPs are the values distributed by the control plane in Phase 3
- `DATA` sender-side validation (`protocol.md` 8.5)
- Write the connection result (success or an FR-13 failure code) and RTT to the **local record file.** Not uploaded to the control plane (M-6)

> **The protocol is already fixed.** Implementation cannot start without a fixed wire protocol. That
> is why `protocol.md` was written as the final version before implementation began. Phase 4 does not
> design the protocol. It only implements `protocol.md`. If a value missing from the document is found
> during implementation, fix `protocol.md` first instead of deciding it in code.
> What is left for Phase 5 is loss and reordering accounting, fuzz defenses, and long-duration stability.

### Deliverable

```text
PC A <========== Direct UDP ==========> PC B
```

Established without router port forwarding in supported environments. The packets exchanged use the official tunnel header and are not replaced in Phase 5.

### Test Harness

Contracts of the tools the verification procedures use. These are equipment, not pass criteria.

- **Pin the console commands used by the verification procedures.** The minimum vocabulary accepted by
  the `[console]` scaffolding of Phases 1-5 ([`architecture.md`](architecture.md) 3.2 Concurrency
  Model) is four commands
  - `quit` signals the shutdown event and starts the normal shutdown procedure (`architecture.md`
    3.2.3 One Loop Iteration)
  - `counters` dumps all counters to the log immediately (`architecture.md` 9). Verification items
    that read counters take their snapshot with this command
  - `raw <byte count>` is the raw send of Phases 1-2, and its contract is in `architecture.md` 3.5
    Startup Inputs
  - `send <n> [size]` sends `n` synthetic inner IPv4 packets as `DATA`

  The contract of `send <n> [size]`.

  - `n` is an integer from 1 to 1000 inclusive
  - `size` defaults to the 100 bytes below; if given, it is an integer from 20 to `MAX_INNER` (1452)
    inclusive and builds a packet of that size (the Phase 5 boundary table uses it)
  - Out of range or not an integer sends nothing and leaves one `WARN` line
  - If the session is not `CONNECTED`, nothing is sent and one `WARN` line is left. 8.5 check 1 blocks
    it anyway, but filtering at the command stage keeps the test's subject clear
  - Transmission is not crammed into one round; it is split the same way as the `MAX_DRAIN` budget
  - If `sendto` fails midway, stop there and record the count sent
  - **Pin the bytes that go out.** The table below is that layout

**Absolute offsets** of the 100-byte inner packet. Written as half-open ranges

| Offset | Size | Value |
|--------|:----:|-----|
| `0` | 1 | `0x45` (version 4, IHL 5) |
| `1` | 1 | `0x00` (DSCP/ECN) |
| `2..4` | 2 | `total_length` = 100 (BE) |
| `4..6` | 2 | id = 0 |
| `6..8` | 2 | flags + fragment offset = 0 |
| `8` | 1 | TTL = 64 |
| `9` | 1 | protocol = 17 (UDP) |
| `10..12` | 2 | IPv4 header checksum. Computed and filled in. `protocol.md` 8.4 check 12 inspects it |
| `12..16` | 4 | source = own virtual IP |
| `16..20` | 4 | destination = peer's virtual IP |
| `20..22` | 2 | UDP source port = 25565 |
| `22..24` | 2 | UDP destination port = 25565 |
| `24..26` | 2 | UDP `length` = 80 (header 8 + payload 72) |
| `26..28` | 2 | UDP checksum = 0 (allowed in IPv4. 8.4 DATA Inner Validation does not inspect it) |
| `28..32` | 4 | packet number counting from 0 (BE) |
| `32..40` | 8 | send time. Monotonic clock milliseconds (BE). Used by the RTT baseline |
| `40` | 1 | **Direction marker.** `0xA5` is a request, `0x5A` is an echo reply |
| `41..100` | 59 | `0xA5` repeated |

> **Why.** The byte values must be fixed so that the M-5 byte-for-byte comparison below and the RTT
> baseline echo use the same input.

Nothing other than the checksum and the send time may be derived. **Therefore, for a single
transmission, the sender-side bytes and the receiver-side bytes are exactly the same.** That is the
M-5 comparison below.

Do not compare across different runs. The 8 send-time bytes and the checksum that depends on them
differ per run, so making a cross-run byte comparison the pass condition fails a correct
implementation.

The round-trip test and the failure-code test below are started and ended with these two commands.

> **Why.** Without the vocabulary, who initiates the round trip differs from test to test.

### Verification

**Direct path establishment.**

- Two PCs on different home networks communicate bidirectionally with no port forwarding rules
- **Two PCs on the same LAN also reach `CONNECTED`.** The "Supported Network Conditions" of
  [`spec.md`](spec.md) set two required topologies and this is one of them
  - Confirm from the log that the learned `peer_endpoint` is the peer's local candidate
    ([`protocol.md`](protocol.md) 10.1 Candidate Collection and Hygiene, 10.4 Endpoint Learning)
  - If it converges on the public candidate behind the same NAT, that relies on hairpinning, so record
    that case as an observation

  > **Why.** The same-LAN test of Phase 1 is raw send and receive before the tunnel, so it cannot stand
  > in for this item, and the "same-LAN baseline" of Phase 9 is P3, so the minimum success verdict
  > cannot rest there.
- Packet capture confirms traffic does not pass through AWS
- At step 5 of the startup procedure ([`control_plane.md`](control_plane.md) 8.4 From Launch to Punch), query two different STUN servers **from the same socket** and observe the same public
  IP:Port
  - This is the verdict for `spec.md` M-2 and "Supported Network Conditions". Phase 2 does not require
    identical results, so this verdict is made here for the first time
  - If observations diverge, it is destination-dependent mapping and an unsupported condition; record
    it as unsupported with the observed values rather than as a failure
- In the same capture, confirm that **the source port of the STUN query and the source port of
  tunnel transmission are the same** (NFR-9). Phase 2 checked this only via log, and a log is
  self-reported, so an implementation that actually opened a new socket passes it

**Wire format.**

- Deserializing a serialized header yields the original (round-trip test)
- The byte layout of captured packets matches the offsets and byte order of `protocol.md` 4.1 (Wire
  Layout) exactly. The first 4 bytes are `53 41 4E 47`

**State machine.**

- In `PUNCHING`, a `HELLO_ACK` arriving first is accepted normally (`protocol.md` 9.4)
- In `CONNECTED`, when the peer retransmits `HELLO`, `HELLO_ACK` is sent again
- Delayed packet handling after renegotiation. **Do not run this as a restart test across NAT**
  - Run it where arrival of the new `HELLO` is guaranteed: two processes on the same machine or direct
    injection into the receive path
  - In that setup, a delayed packet from the previous epoch that arrives after the new `HELLO`
    triggered `protocol.md` 9.5 Renegotiation is discarded as `drop_retired_epoch`
  - A delayed packet that arrives before renegotiation matches the pinned epoch, so accepting it is
    the rule. Expecting a drop in that interval fails an implementation built to the rule

  > **Why.** The restarted peer's new `HELLO` may be blocked as measured in `protocol.md` 10.4
  > Endpoint Learning and v1 does not guarantee that recovery, so on the wire the renegotiation never
  > happens and a correct implementation fails. It is discarded because step 1 of renegotiation puts
  > the previous epoch in the retired list, and 8.1 common check 8 (retired list) runs before the
  > epoch comparison of 8.2 Epoch and Source Checks and 9.3 Epoch Rules.

**Receive hygiene and caps.**

- Continuously sending to a non-responding candidate does not kill the receive loop via `WSAECONNRESET`
- Check source sanitation (`protocol.md` 7) at two layers
  - (a) Run that section's case table as-is against the **verdict function.** Multicast, limited
    broadcast, the directed broadcast of one's own subnet, unspecified, and port 0 are rejected, and
    loopback and ordinary unicast pass
  - (b) Feed a rejected source into the **receive pipeline**; it reaches neither STUN classification
    nor tunnel classification, ends at `drop_bad_source`, and not a single response goes out
  - (b) is run by injecting the datagram directly into the receive path instead of sending it on the
    wire. An ordinary Windows UDP socket cannot produce a datagram with a forged source
  - **That requires a test entry point in the receive path.** Expose in the test build one function
    that takes `(buffer, length, source endpoint)` and runs from classification onward. That function
    must take the same code as the path after `recvfrom`
  - Confirm that a mutant with one rule line removed and a mutant changed to reject loopback each
    `FAIL`

  > **Why.** Testing only (a) lets an implementation that builds the function correctly but calls it
  > after classification pass. If the entry point is written separately for tests, the test does not
  > verify the real path.
- Terminal-state reception (9.4.1). After exchanging `CLOSE` and reaching `CLOSED`, a `KEEPALIVE`
  from the same peer only increments `drop_terminal_state` and no response goes out. Confirm silence
  with a capture
- Retired list cap (`protocol.md` 4.3 `session_epoch`). **Test it as a unit**
  - Register 17 entries directly in the retired list data structure and check that the list stays at no
    more than 16 and the oldest is evicted
  - Check the pipeline separately. In a session with the peer epoch pinned, injecting a non-`HELLO`
    packet carrying the evicted epoch is caught as `drop_stale_epoch` (second line of defense)
  - Injecting it while the epoch is unpinned gives `drop_no_epoch`, which means something else was
    observed
  - An implementation that grows without bound and one that rejects new entries are each caught

  > **Why.** 17 cannot be produced through the defined interfaces. Renegotiation is capped at 8 per
  > attempt (9.5) and Phase 4 has no means to initiate a retry on our side.
- `session_epoch` randomness source (4.3). Make the source injectable and check (a) that a 0 result
  is redrawn and (b) that the draw path is the OS CSPRNG. Value distribution cannot expose a
  `rand()` implementation, so **checking the call path in code is part of the verdict**
- Timers after renegotiation (`protocol.md` 9.5 Renegotiation step 10). Inject a `HELLO` with a
  different epoch into a `CONNECTED` session to trigger renegotiation, then check three things
  - `HELLO` retransmission goes out again at 200ms intervals (capture)
  - `KEEPALIVE` and `PING` do not go out during that interval
  - **The idle timeout is halted.** Trigger the renegotiation when little idle time is left. If the
    peer's transmission is cut just before renegotiation to raise idle to 45 seconds and then the
    injection is made, an implementation that does not halt the timer ends with `TUNNEL_DROPPED`
    before the punch deadline (10 seconds) and is caught. A correct implementation makes no disconnect
    verdict in that interval
- Renegotiation caps (`protocol.md` 9.5)
  - Sending `HELLO`s with different epochs back to back within 1 second from the same-machine injector
    renegotiates only the first; the rest increment `drop_reneg_rate`
  - Exceeding `MAX_RENEGOTIATIONS` with intervals over 1 second makes the rest increment
    `drop_reneg_limit`
  - **Also check that the normal path is not caught here.** A `HELLO` retransmission after one
    renegotiation carries the same epoch and must be handled as an ordinary `HELLO`
- Provisional path cap (`protocol.md` 10.4)
  - Sending packets that pass validation and advance `sequence` from five different source ports
    outside the candidate set makes only the first four provisional paths, and the fifth increments
    `drop_probe_full`
  - **The fifth packet itself is processed normally.** The idle timer is refreshed and, if it was
    `DATA`, it is injected. An implementation that drops that packet is caught by this item
  - The verification `HELLO` goes out exactly once per path (confirm by capture). The oldest path is
    not evicted
- Common budget for unverified destinations (`protocol.md` 9.4 Transition Table). **Get the token count
  right first**
  - One `HELLO` from a new source outside the candidate set spends two tokens. One `HELLO_ACK`, and one
    verification `HELLO` that 10.4 (c) sends while promoting that source to a provisional path
  - From the second shot of the same source on, the provisional path already exists and only one token
    is spent. That slot fills 5 seconds regardless of the outcome

  Check five things.

  - (a) Sending `HELLO` at 20 per second for 10 seconds from a source outside the candidate set, the
    number of outgoing transmissions does not exceed an initial 10 + a 5 per second refill, and the
    excess increments `drop_unverified_tx`
  - (b) The same test from a source inside the candidate set has no limit and everything is answered
  - (c) **Normal retransmission is not blocked.** Sending at 200ms intervals for 30 seconds from an
    outside source, not one response is missing. The injector answers the verification `HELLO` with
    `HELLO_ACK`
  - (d) **Check that there is one budget.** Sending mixed `HELLO` and `PING` from an outside source,
    `HELLO_ACK`, `PONG`, and verification `HELLO` together stay within the limit above. An
    implementation that counts per path is caught here
  - (e) **Count the boundary.** From a source that already has a provisional path (registered within 5
    seconds), sending in a row goes out through the 10th and the 11th is blocked. Starting straight
    from a new source, the first shot spends two, so it goes out through the 9th and the 10th is
    blocked. Write down which of the two conditions is run and expect the matching number. Also confirm
    that one passes 200ms later. A fixed-window implementation is caught by (c)

  > **Why.** Leaving the token count out fails an implementation built to the rule on (e). If the
  > injector in (c) does not answer, the provisional path expires every 5 seconds and is registered
  > again, so one more token goes out every 5 seconds, and the longer the test runs the more surely
  > even a correct implementation hits the budget. If it answers, that path becomes verified and is no
  > longer an unverified destination.

**Shutdown and timers.**

- `CLOSE` send condition (`protocol.md` 5.6 `CLOSE`, 9.6 Failure Transitions)
  - Closing a session that has learned `peer_endpoint` sends `CLOSE` once to that address; closing
    before learning sends **none at all** (confirm by capture). An implementation that sprays all
    candidates is caught by this item
  - Check the reason value in the capture too. Closing a `CONNECTED` session with `quit` gives `0x00`,
    `quit` during establishment gives `0x01`, and failing at the punch deadline gives `0x03`. An
    implementation that sends one value for all three is caught here
- No transition to `CONNECTED` when only one side has received `HELLO` (a one-directional opening is not misjudged as success)
- Keepalive period: in the capture, `KEEPALIVE` goes out once immediately on entering `CONNECTED`
  and then every 15 seconds. **What is measured here is our send period, not the NAT mapping
  lifetime.** Mapping lifetime measurement is the before-start item in the Tasks section above

**`DATA` round trip.**

- Round-trip a synthetic inner IPv4 packet as `DATA` in both directions and the bytes match (M-5).
  **Pin the comparison point**
  - Compare, byte for byte including length, the payload byte string the sender held just before
    encapsulation with the payload byte string the payload hook received after passing 8.4 validation
  - That hook is where a `DATA` that passed 8.4 goes in a build without an adapter, and its contract is
    in `architecture.md` 3.2.3 One Loop Iteration. The echo responder attaches at the same place
  - Do not compare log strings or a reconstructed header
  - In the product build, also confirm that such a `DATA` is dropped as `drop_no_sink`. The hook must
    not remain in the product build

  > **Why.** Comparing log strings or a reconstructed header passes even when the header is rewritten
  > or padding is added.
- `DATA` with a wrong inner source/destination virtual IP is discarded as `drop_inner_src` / `drop_inner_dst`

**Records and failure codes.**

- The line count of the local record file matches whether `CONNECTED` was reached (M-6,
  `architecture.md` 9)
  - **An attempt that failed to establish has one establishment line; an attempt that reached
    `CONNECTED` has two lines, establishment and termination.** On failure the FR-13 code is written
  - RTT is normally empty when there is no sample. The establishment line is usually empty because
    `PING` has not run yet, and the termination line carries the last sample. An empty value is not
    judged as failure
- On hole punching failure, `HOLE_PUNCH_TIMEOUT` is recorded and the process exits normally.
  **Reaching `FAILED` is itself the shutdown trigger** (`architecture.md` 3.2.7 Shutdown)
  - Also confirm by capture that `HELLO` retransmission stops at that transition. In an implementation
    that does not stop, a finished session keeps firing at every candidate (`protocol.md` 9.6)
- The RTT baseline comparison uses **a round trip on the same socket and the same endpoint pair,** not
  ICMP
  - Do not create a new wire type for the baseline. Carry the send time in the synthetic inner IPv4/UDP
    packet used by the M-5 item above as `DATA`
  - The echo responder swaps the inner source and destination, changes the direction marker at offset
    40 to `0x5A`, recomputes the IPv4 header checksum, and sends it back
  - Do not respond to a packet that is `0x5A`
  - The echo responder goes into the test build only. Not in the product build. Gate it with a CMake
    option, and a build with it enabled leaves one `WARN` line at startup so the log alone tells them
    apart
  - Pass condition. The median of 20 `PING`/`PONG` samples is within an absolute difference of 10ms or
    a relative difference of 30% of the median of 20 `DATA` echoes (allowing for user-space processing
    overhead)
  - An ICMP comparison may be recorded for reference only

  > **Why.** ICMP toward a peer behind NAT is often blocked or answered by the peer's router, so it
  > does not measure the same path. Without the `0x5A` rule, when both sides enable the option the
  > reply is echoed again and packets bounce between the two peers forever. If the checksum is left as
  > is, the returned packet is caught by `protocol.md` 8.4 check 12 as `drop_inner_checksum` and this
  > test cannot run. A feature that sends received `DATA` back is a reflection path by itself and must
  > not remain in the demo build. Since `protocol.md` 5 has no echo type and the chapter 7
  > classification discards datagrams outside the protocol, assuming a separate echo packet means an
  > implementation built to the rule cannot run this test.
- Forced failure-code tests: control plane down -> `CONTROL_PLANE_EXCHANGE_FAILED`, inject a
  non-responding endpoint -> `HOLE_PUNCH_TIMEOUT`, block the `HELLO_ACK` response ->
  `PEER_HANDSHAKE_FAILED`. Each records the exact code and state transition

### Risk

NAT environments exist where this Phase fails. Failures are not hidden; they are recorded with NAT
environment information. Supported and unsupported network conditions are defined explicitly. Relay
fallback is not implemented at this point and remains a stretch goal.

---
## Phase 5: Tunnel Protocol Completion

- **Goal:** Add robustness to the tunnel whose round trip was confirmed in Phase 4. Loss and reordering accounting, defenses against corrupted packets and fuzzing, and long-duration stability.
- **Priority:** P1
- **Requirements:** The [Requirement Traceability](#requirement-traceability) table is the source. Not repeated here
- **Prerequisite:** The [`protocol.md`](protocol.md) 15 checklist is fully completed in Phase 4.

### Tasks

- `sequence`-based loss and reordering **accounting**. Implement the `position` / `baseline` /
  `accepted` computation of [`protocol.md`](protocol.md) 4.5 Duplicate Suppression and Loss Accounting
  to produce loss and loss rate
  - The acceptance window itself belongs to Phase 4
  - That window is a 64-slot position-based bitmap with no time-based expiry, so there is no "window
    expiry" trigger
  - Loss is not finalized by expiry; it comes out as `expected - accepted`. A packet dropped as
    `drop_too_old` was already finalized as lost and is not reverted
- Fuzz defense. No crash on corrupted and truncated input
- Long-duration connection stability
- **Fix whether to retry automatically.** `protocol.md` 9.6 Failure Transitions does not say who
  initiates a retry and when after `TUNNEL_DROPPED`. Decide, then fix that document

**What Phase 4 already implemented is not built again here.** For the three below, the **tests**
belong to this Phase and the implementation is in Phase 4. If where things live is unclear, the
verification criteria split across two Phases.

| Item | Implementation | What this Phase does |
|------|------|----------------------|
| 8.1 common checks 1-8 and per-reason drop counters | Phase 4 | Inject a bad magic, unsupported version, length mismatch, and truncated packets and check that the counter for the right reason increments |
| 8.4 / 8.5 size limits and `payload_length` validation | Phase 4 | Test the 20-byte and `MAX_INNER` boundaries, and just outside each boundary |
| 9.4 full session state machine | Phase 4 | Check that state stays as specified through long-duration intervals and failure transitions |

### Deliverable

An application-level tunnel exists between two peers that carries inner IPv4 packets reliably under
loss, reordering, and corruption. In Phase 7 the IP packets actually captured by Wintun go into this
`DATA` payload slot.

### Verification

- Run the whole boundary-size case table. **Write ranges as values, not words.** Without values, just outside the boundary is not tested

| Payload size | Expected |
|--------------:|------|
| 19 | `drop_type_length` on receive. The `DATA` minimum is 20 |
| 20 | **One-way arrival and comparison.** Not a round trip. See the note below |
| 576 | Round trip succeeds. Midpoint |
| 1452 | Round trip succeeds. `MAX_INNER` boundary |
| 1453 | `tx_drop_oversize` on send. **The receive side is not tested with this table.** The datagram is 20+1453 = 1473 bytes, so the `n > MAX_DATAGRAM` length comparison discards it as `drop_oversize_datagram` and it never reaches the validation pipeline (`architecture.md` 3.2.3. The `WSAEMSGSIZE` path starts at 1474 bytes) |

  The match verdict for round-trip success cases uses the same comparison point as Phase 4 (the
  payload hook).

  **How the size is given, and the note on 20 bytes.** The Phase 4 console command is used as
  `send <n> [size]`. Omitting `size` gives the 100-byte layout pinned in Phase 4; giving it builds an
  inner IPv4/UDP packet of that size.

  - 20 bytes is an IPv4 header only, so neither the UDP header nor the direction marker (offset 40)
    fits. The echo responder cannot read the direction marker, so not sending it back is the rule, and
    therefore this size is sent one way and the bytes are compared at the receiver's hook
  - 576 and 1452 keep the 100-byte layout and only grow the fill bytes after offset 41
  - `total_length`, UDP `length`, and the IPv4 checksum are recomputed for the size
- Packets with a bad magic, unsupported version, or `payload_length` mismatch are all discarded and the counter for that reason increments
- Fuzz test. **Split it into two stages**
  - **Stage 1 (classification and 8.1).** Injecting 100,000 random packets of 1-2000 bytes generated
    from a fixed seed into the socket causes no crash and no memory error
  - **Stage 2 (after 8.2 Epoch and Source Checks).** Attach a valid header through magic, version, the
    peer `peer_id`, and the pinned epoch, fill the rest at random, and feed 100,000 such packets into
    the Phase 4 receive injection entry point. Put `type` and `payload_length` in the mutation set too
    so length mismatches and boundaries are exercised
    - Whether it got there is read from the counters. If only `drop_magic` increments and no counter
      after it moves at all, that test did not walk the pipeline
  - Both stages log the seed, count, and length range so they are reproducible

  > **Why.** Running only stage 1 barely exercises the validation pipeline. Three quarters of random
  > first bytes end at the chapter 7 classification of [`protocol.md`](protocol.md), and what is left
  > is caught almost entirely by the 4-byte magic, so no input reaches past 8.2 Epoch and Source Checks
  > (the nonce table, the duplicate suppression bitmap, 8.4 inner validation, `CLOSE` reason,
  > renegotiation).
- Control plane outage test: after reaching `CONNECTED`, taking the control server down keeps `DATA` transfer and session state for 10 minutes (NFR-3)
- Deliberately dropping packets yields a loss rate computed from `sequence` gaps
- Injecting packets out of order is not counted as loss (the 64-slot acceptance window of
  `protocol.md` 4.5 Duplicate Suppression and Loss Accounting accepts reordered packets and they count
  toward `accepted`)
- The loss rate does not jump when `sequence` wraps past the 32-bit boundary. **Run it through the
  receive injection entry point**
  - Inject a packet sequence starting at `sequence = 0xFFFFFFF0` and check that `position` continues
    without wrapping (`protocol.md` 4.5)

  > **Why.** Counting from 0 as the rule says would need 2^32 packets to reach the boundary, so it
  > cannot be run on the wire.
- Idle timeout test: cutting the peer's transmission entirely records `TUNNEL_DROPPED` with the correct state transition (completes the FR-13 failure code set)
- Session state stays `CONNECTED` during a long (1 hour or more) connection

---

## Phase 6: Windows Virtual Network Adapter

- **Goal:** Capture and inject IP packets through a virtual interface.
- **Priority:** P2
- **Requirements:** The [Requirement Traceability](#requirement-traceability) table is the source. Not repeated here

### Tasks

- **Before starting, fix where the expected publisher string and published hash of the Wintun
  distribution are recorded.** [`windows-prereq.md`](windows-prereq.md) 4 says to compare the
  distribution's `Subject` and hash against expected values, but does not say where those expected
  values are kept. Decide the location, then fix that document

  > **Why.** Without a place for them, verdicts 2 and 3 of that section cannot run and the check
  > collapses to looking at `Status` only.
- **Before starting, decide whether to add an alternate range.** `windows-prereq.md` 7 currently only
  aborts on the conflict condition
  - Changing the range means both sides must use the same value, which means the control plane has to
    tell them, but the operations in [`control_plane.md`](control_plane.md) 4 have no such field and
    2.5 Virtual IP Pool fixes the range as a constant
  - If it is added, both documents are fixed together
  - If it is not added, aborting is the final v1 behavior and that limit stays in section 7
- **Before starting, confirm how Wintun injection failure is reported.** Check in the header used and
  its documentation which call fails and in what form, and record in
  [`architecture.md`](architecture.md) 3.2.3 One Loop Iteration whether "ring full" gets its own
  counter
  - The behavioral contract (discard, `drop_inject_error`, no retry, non-blocking loop) is already
    fixed, so the only thing this item blocks is counter granularity
- Wintun dependency **approved.** Documentation of what it provides is kept as is
- Create and open the virtual adapter
- Assign the virtual IP address
- Read packets from the adapter
- Inject packets into the adapter
- Set up the required Windows routes and clean them up on exit
- Verify traffic with basic IP tools, without a game

### Deliverable

```text
Windows IP stack
      |  packets destined for 10.100.0.0/24
      v
Wintun Adapter  --- read ---> client (counter increments)
Wintun Adapter  <-- inject -- client (hand-built reply)
```

On a single host, packets can be read from the virtual adapter and injected back. Communication between two virtual IPs requires tunnel integration and is a Phase 7 deliverable.

### Verification

- `ipconfig` shows the virtual adapter and the assigned virtual IP
- `route print` shows the `10.100.0.0/24` route pointing at the adapter
- On the same machine, `ping` to an **unassigned** virtual IP (for example `10.100.0.9`) increments
  the adapter read counter `adapter_rx`. Hand-building an ICMP reply to that packet and injecting it
  makes `ping` receive a reply (verifies the read/inject path in both directions)
  - The ICMP reply generator goes into the test build only. It sends back what it received, so it is
    of the same kind as the Phase 4 echo responder and sits under the same CMake option
- `ping` to one's own virtual IP is short-circuited by the Windows local stack and does not pass through the adapter, so it is not used for verification
- A real virtual IP round trip between two peers comes after tunnel integration and is verified in Phase 7
- No adapter or route remains after the client exits
- After an abnormal exit, rerunning recreates the adapter normally

### Risk

Windows virtual networking can be more complex than expected. This work is kept separate from the
Phase 4 NAT traversal work. Wintun is used only for virtual interface access; the routing decision
logic is owned by the project.

---

## Phase 7: Virtual Adapter and P2P Tunnel Integration

- **Goal:** Forward virtual network IP packets between peers.
- **Priority:** P2
- **Requirements:** The [Requirement Traceability](#requirement-traceability) table is the source. Not repeated here

### Tasks

- Read outgoing IP packets from Wintun
- Determine the target peer from the destination virtual IP
- Encapsulate as `DATA` packets
- Transmit over the direct UDP connection
- Decapsulate received packets
- Inject the original IP packet into Wintun
- Handle packet size limits and fix the MTU
- Add counters and diagnostics

### Deliverable

Two Windows machines communicate using virtual IPs only.

### Verification

- `ping` round trip to the peer's virtual IP succeeds
- File transfer between virtual IPs (SMB or a temporary TCP server) succeeds with matching content.
  **This item is the verdict for NFR-2**
  - It shows that non-Minecraft traffic passes through the tunnel unchanged
  - One more pass condition is attached. Run this test and the Phase 8 Minecraft test with the same
    tunnel build. If the tunnel code had to be changed to switch applications, NFR-2 does not hold
- Maximum-size packets are delivered without fragmentation (MTU value fixed by measurement)
- Packets sent to a virtual IP with no routing target are discarded and the counter increments
- Injection failure handling ([`architecture.md`](architecture.md) 3.2.3 One Loop Iteration). When the
  send ring is made full, that packet is discarded as `drop_inject_error` and **the loop does not wait
  there**
  - The verdict does not look at the counter alone. Also check that the keepalive send interval is
    maintained during the interval where injection fails
  - An implementation that retries until space frees up is caught because that interval collapses
- Cross-check send/receive byte counters against a packet capture. **The pass criteria belong to the
  table below.**

| Item | Definition |
|------|------|
| Counting basis | **UDP payload bytes including the tunnel header.** Ethernet, IP, and UDP headers are not counted |
| Measurement point | The client counts the bytes returned by successful `sendto`/`recvfrom`. The capture filters only that socket's 5-tuple on the same machine |
| Sample condition | **Generated traffic,** not game traffic. Only the interval that starts idle, sends a fixed count, and stops is inspected. Sizes are 20 / 576 / 1452 bytes from the Phase 5 boundary table |
| Count | **Still empty. Decide it at test time and write it in this cell.** Try 1,000 / 5,000 / 10,000 / 50,000 in order and pick the largest value for which capture loss is reported as 0. The ladder is only a search grid; the chosen value is what the verdict uses. It depends on the machine and capture setup and cannot be invented in advance |
| Valid interval | Judge **only in an interval where the capture tool reports 0 loss.** Non-zero is not a failure; discard and measure again |
| Pass condition | Byte sums match in the valid interval. If the capture sum is **greater** than the counter sum, it is a failure. Bytes were sent uncounted, so it is a counting bug |
| Packet count | **Recorded for reference only.** With UDP segmentation offload on, the capture sees a different number of datagrams. The byte sum is still the same |

  **What this test does not guarantee.** Accuracy in an interval where the capture experiences loss is
  not judged. This test says nothing about counting accuracy under high load

---

## Phase 8: Minecraft Validation

- **Goal:** Demonstrate a real multiplayer session.
- **Priority:** P2
- **Requirements:** The [Requirement Traceability](#requirement-traceability) table is the source. Not repeated here

### Tasks

- **At the start of Phase 8, re-measure with `tools/nat-probe`.** Before setting up the demo
  environment, confirm that the direct-connection verdict still holds
  ([ADR 0001](decisions/0001-no-direct-connection-fallback.md))
- **Re-measure once more just before the demo.** A same-day check. The direct-connection verdict is
  an observation at a point in time and can be invalidated by a router replacement or an ISP
  configuration change
- Run a Minecraft Java server on the host
- The player connects to the host's virtual IP
- Confirm login and world entry
- Extended play
- Measure latency and disconnection behavior
- Record packet and connection metrics

### Deliverable

```text
Minecraft Client -> 10.100.0.1:25565 -> Project Virtual Network -> Minecraft Server
```

### Verification

- The player connects by entering only `10.100.0.1:25565`, with no public IP/port entry
- Succeeds with no port forwarding rule on the host's router
- No forced disconnect during 30 or more minutes of continuous play
- Record the game-displayed latency and the tunnel RTT over the same interval and **report the
  difference between them.** The pass criteria belong to the table below

| Item | Definition |
|------|------|
| What is measured | Game-displayed latency is the value the Minecraft client shows; tunnel RTT is the `PING`/`PONG` measurement. **They do not measure the same thing.** The game value is an application-layer round trip and sits on top of the tunnel |
| Measurement point | The game value is read from the screen; the tunnel value is read from the client log |
| Sample condition | At least 20 each over the same interval. Log the start and end of the interval. **This is not a claim of statistical sufficiency; it is the minimum condition for a record.** The game-displayed latency updates about once a second, so collecting 20 takes roughly 1 minute |
| Statistics | Median and interquartile range. Mean and standard deviation are not used |
| What is judged | **Measurement consistency. Not quality.** No matter how large the game latency is, this criterion alone does not fail it. Perceived quality is judged by the propagation test below |
| Pass condition | The median of the game-displayed latency is **at or above** the tunnel RTT median. If it is smaller, it still passes as long as the difference is within the tolerance. The tolerance is the larger of the tunnel RTT interquartile range and one unit of the game display's resolution. On a quiet LAN the interquartile range approaches 0, and if screen rounding alone could flip the result there, a correct implementation would fail. Smaller by more than the tolerance is a failure and means one of the two is being measured wrong. Do not invent the tolerance in milliseconds. Use only the spread from the same measurement and the tool's display resolution |
| Report | Put the median and interquartile range of the difference in [`experiments.md`](experiments.md) |

  **What this criterion does not guarantee.** No absolute latency ceiling is guaranteed. Perceived
  quality is judged only by the propagation test below
- Attempt 20 block placements and 20 chat messages; all propagate to the other client within 2 seconds

---

## Phase 9: Monitoring and Experimental Evaluation

- **Goal:** Go beyond a working demo and produce measurable CSP analysis results.
- **Priority:** P3
- **Requirements:** The [Requirement Traceability](#requirement-traceability) table is the source. Not repeated here

### Tasks

- **Before starting, fix the telemetry service spec.** Decide the collection schema, whether to
  authenticate, upload period, retention period, and DynamoDB table design. **Do not implement
  before deciding.** The separation contract itself is already held by
  [`architecture.md`](architecture.md) 3.4 Telemetry Service
- **Before starting, fix the resource isolation method within one instance.** The two services run
  on the same EC2 instance, so if telemetry exhausts disk or CPU the control plane stalls with it.
  Having no dependency by design is not the same as having isolated resources (`architecture.md` 3.4
  Telemetry Service)
- Implement and deploy the telemetry service (a separate process and port from the control server)
- Record connection establishment time
- Record RTT
- Record packet loss
- Estimate jitter
- Record tunnel throughput
- Record session duration
- Move telemetry upload to a dedicated thread and a lossy queue (`architecture.md` 3.2.6 Telemetry Isolation)
- Test across multiple network environments
- Compare successful and failed NAT traversal cases (by observed mapping behavior, not NAT type)
- Visualize experimental results
- Document limitations
- Write [`experiments.md`](experiments.md)

### Test Environments

| Environment | Purpose |
|------|------|
| Same LAN | Baseline measurement |
| Home network to home network | Primary use scenario |
| Home network to mobile hotspot | Difficult NAT conditions such as CGNAT |
| Campus network to home network | Institutional firewall conditions |
| Other available NAT behaviors | Comparison by mapping behavior |

### Statistical Design

**The table below is the statistical design of Phase 9.** It fixes, before measuring, what counts as
one trial, whether trials are independent, and how uncertainty is written.

> **Why.** Without that, nobody can later say what the resulting numbers mean.

| Item | Definition |
|------|------|
| One trial | From client start to reaching `CONNECTED` or recording a failure code. Session duration and quality measurement are a separate interval after `CONNECTED` |
| Between trials | Shut the client down and restart with a new `session_epoch`. Rest longer than the idle timeout (50s) so the previous NAT mapping is no longer alive |
| Independence | **10 consecutive runs on the same network on the same day are not called independent trials.** Router state and the ISP path are shared. Record them as repeated measurements and count only environment changes as separate conditions |
| Representative value | Median and interquartile range. **Mean and standard deviation are not used.** RTT and establishment time are skewed, so the mean does not represent them |
| Success rate | **Always write the denominator n** alongside the ratio. When attaching an interval, use the Wilson score interval. The normal approximation produces a wrong interval when n is small and the ratio is near 1 |
| Sample count | 10 or more per environment. **Not a claim of statistical sufficiency.** It is the minimum that is feasible on the networks reachable in one semester. Write that limitation in [`experiments.md`](experiments.md) and do not overload the numbers with meaning |

**What this design does not guarantee.** This design is not population estimation. It only records
what was observed on the few networks we could reach, and it does not generalize to other NAT
environments.

### Deliverable

A repeatable experiment set and data for the final report and presentation.

### Verification

- Rerun the control plane outage test: with telemetry enabled, taking **both the control server and
  the telemetry service** down keeps the tunnel for 10 minutes (NFR-3). The Phase 5 test is rerun in
  the telemetry-enabled configuration.

  > **Why.** Both are taken down because a failure of either must not break the tunnel. Taking down
  > only one leaves the other alive, and then it is not clear what held the tunnel up
- With **only the telemetry service process** stopped, room creation, room join, and `get_peers`
  respond normally (NFR-10). This is the verdict for separation 4
  ([`architecture.md`](architecture.md) 3.4 Telemetry Service). It is not a test that takes the
  instance down. Both are on the same EC2, so taking the instance down kills both and judges nothing
- With **only the control server process** stopped, metrics uploads are received normally (NFR-10).
  This is the verdict for separation 2. An implementation where telemetry asks the control plane to
  validate identifiers fails here
- With the queue full, metric recording on `[loop]` does not block and `telemetry_queue_dropped`
  increments. **Also check the size and the drop direction**
  - The ring size is owned by `architecture.md` 3.2.6 Telemetry Isolation. The value at this point is
    256 entries
  - When full it drops the new record. An implementation that drops the oldest must be filtered here

  > **Why.** Dropping the oldest makes the metrics look better, but the producer is moving the
  > consumer's tail.
- At least 10 repeated measurements collected per environment. Follow the trial definition and spacing of the statistical design above
- Every failure has its stage code and NAT environment information recorded. Controlled failures and real-environment failures are **tallied separately** (A-2)
- **Run both controlled failure scenarios for real** ([`spec.md`](spec.md) A-2)
  - (1) Blocking outbound UDP in the host firewall and starting the client records
    `STUN_DISCOVERY_FAILED`. Revert the rule after the test
  - (2) Injecting a non-responding endpoint records `HOLE_PUNCH_TIMEOUT`. This reruns the same Phase 4
    test once more in this configuration
  - The two are evidence of the failure handling path, not evidence of a failure rate. Write them that
    way in the tally

  > **Why (1) is a different path from Phase 2.** The "non-responding address" test of Phase 2 has a
  > wrong address, while this one has our own transmission blocked. Whether the deadline and the
  > recording still run on a blocked path is only observed here.
- Success rate is tallied by observed mapping behavior. No statement asserts a NAT type from RFC 5389 Binding results alone
- One reproduction run passes. The pass condition is set by `spec.md` A-4
- Graphs and a limitations write-up are included in [`experiments.md`](experiments.md)

---

## Stretch Goals

Start only if Phases 1-9 finish with time to spare. Do not proceed at the expense of the core P2P goal.

| Item | Note |
|------|------|
| Relay fallback | For peers that cannot connect directly. Rescues environments classified as unsupported in Phase 4 |
| Encryption and peer authentication | No in-house cryptographic algorithms |
| Windows installer | |
| Simple GUI | |
| Automatic reconnection | |
| Virtual network with 3 or more parties | Requires extending the router structure |
| UDP-based game validation | Further proof that the tunnel is game-agnostic |
| Linux interoperability | |

---

## Risk Management

| Risk | Response |
|------|------|
| Hole punching does not work on all networks | Validate early in Phase 4 before building upper layers. Record NAT-related failures rather than hiding them. Reserve relay fallback as a follow-up solution. State supported/unsupported conditions explicitly |
| Windows virtual networking is more complex than expected | Separate virtual adapter work from NAT traversal work. Verify direct P2P first with ordinary application data. Use Wintun only for interface access. The project owns the routing logic |
| Scope overrun | Fix the P0-P4 priorities. Remove from the lowest priority upward and keep the core P2P goal until the end |
| Third-party library constraints | Request approval before using non-basic dependencies such as Wintun. Implement core algorithms independently of those dependencies. Document exactly what each library provides |
