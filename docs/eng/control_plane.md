# Control Plane

**Project:** Direct-First P2P Virtual Network for Multiplayer Games
**Requirements:** [`spec.md`](spec.md) FR-4, FR-5, NFR-3, NFR-10, C-3
**Design:** [`architecture.md`](architecture.md) 3.3 / [`concurrency.md`](concurrency.md) / **Protocol contract:** [`protocol.md`](protocol.md) section 14
**Schedule:** [`roadmap.md`](roadmap.md) Phase 3

> Korean version: [`../kor/control_plane.md`](../kor/control_plane.md)

---

## 0. Where This Document Stands

**This is the single source for the control plane.** What is fixed here is the following.

- Operations, request and response encoding, errors
- Identifiers, room and peer state transitions
- DynamoDB table design, server implementation structure
- The client-side call contract

[`architecture.md`](architecture.md) 3.3 Control Plane and section 6 Control Plane Interface
are summaries that point to this document. [`protocol.md`](protocol.md) section 14 Control Plane
Schema is a list of **what the tunnel protocol requires from the control plane.** That document
owns the requirements; this document owns how they are met.

| What conflicts | Wins |
|-------------|------|
| Control plane wire encoding, error codes, state transitions, table design | This document |
| Tunnel wire format, session state, protocol timers (including the `get_peers` polling interval and deadline) | `protocol.md` |
| Client threads and loop structure | [`concurrency.md`](concurrency.md) |
| Requirements and success criteria | [`spec.md`](spec.md) |

If the implementation meets a value that is not in this document, do not decide it in code. Fix
this document first.

---

## 1. Scope and Trust Assumptions

### 1.1 What It Does and Does Not Do

| Does | Does not |
|---------|--------------|
| Room creation and joining | Relay game traffic. It is not on the data path (NFR-1) |
| Virtual IP assignment | Collect metrics. The telemetry service does that ([`architecture.md`](architecture.md) 3.4, [ADR 0003](decisions/0003-telemetry-service-split.md)) |
| Store and deliver candidate endpoints | **Open a UDP socket.** The EC2-side UDP probe sender used to measure NAT mapping lifetime is not a function of the control server. It is a separate tool. Its ownership and procedure belong to the [`roadmap.md`](roadmap.md) Phase 4 pre-start items |
| Provide the rendezvous reference point (`punch_delay_ms`, `elapsed_since_ready_ms`) | Judge NAT type or hole punching results. The client does that |
| Reclaim a slot on the host's report, and server reclaim of a participant who left without candidates (4.6 `host_report`) | Start session retries. Who retries and when is undecided, and [`protocol.md`](protocol.md) 10.4 Endpoint Learning says so |
| Renew the room lease on the host's signal (4.6, 5.1) | **Judge tunnel liveness.** Even if the lease lapses, an already established tunnel keeps going ([`spec.md`](spec.md) NFR-3) |
| Record pair readiness (4.6, 5.2) | **Recover the identifiers of a departed peer.** There is no rejoin (4.3). Coming back in is a new join |

### 1.2 Trust Assumptions

**The v1 control plane path is plaintext and has no caller authentication.** This is the same kind
of explicit choice that [`protocol.md`](protocol.md) section 2 made for the tunnel path. The final
report has to state this limit next to the tunnel-side limit.

| Assumption | Content | Consequence |
|------|------|------|
| The server is trusted | The client uses the peer candidates and virtual IP returned by the server without verification. Candidate hygiene (`protocol.md` 10.1 Candidate Collection and Hygiene) is applied separately by the client, but that is a format check, not an authenticity check | A malicious server can insert arbitrary candidates and use the client as a **reflector** that sends packets to a third party every 200ms. The upper bound on that reflection is stated by `protocol.md` section 2 Security Model. Read "Inserting a reflection candidate" below with this row |
| `room_id` is the only secret | Joining a room needs only a `room_id`. The host passes it to the participant over another channel | Any caller who knows the `room_id` can join as the second peer, receive candidates, and become the game peer. This is **room hijacking.** The real participant then receives `room_full` |
| An on-path observer exists | The path is plaintext TCP, so an on-path observer reads `room_id`, `peer_token`, and both sides' local and public endpoints | Room hijacking as above, plus **internal network address exposure.** Local candidates are private IPs and ports |
| `peer_token` is an authentication secret used only within one join | It is issued at join time and discarded when that join ends. It is never written outside the process ([`architecture.md`](architecture.md) 3.5 Startup Inputs) | On a plaintext path it leaks the same way `room_id` does. **It is not a defense against an on-path observer.** What it blocks is a third party who knows only the `room_id` impersonating an already joined peer and changing that peer's candidates (4.4), or impersonating the host and deleting someone else's slot (4.6) |
| The caller IP is treated as an address that can be answered on that TCP connection | It is the source of a connection that completed the handshake. A NAT or proxy may sit behind it so that many users appear as one address, and conversely one caller may use many addresses | The per-source budget in 6.4 Rate Limit stands on this assumption. Against a caller with many addresses the effect shrinks accordingly, and legitimate users who share an address get caught by someone else's exhausted budget (6.4 case table row 12) |
| `client_nonce` is also a secret | It is an idempotency key (2.4), but anyone who knows the value can resend the same request and get **the same response.** The response carries a `peer_token` (4.2, 4.3) | An on-path observer already reads the token, so this is not a new capability. But if the client leaves the nonce in a log or on screen, that is the same as leaving the token. **Treat it at the same grade as `room_id`** |

**Inserting a reflection candidate is not something only the server can do.** A peer that joined
the room gets the same result by registering the same address with `register_candidate`. The
server only checks the format and forwards. What `protocol.md` 10.1 Candidate Collection and
Hygiene blocks is amplification paths such as broadcast and multicast.

**These are limits that v1 accepts.** Authentication and TLS are [`spec.md`](spec.md) stretch
goals. When they are introduced, the "Consequence" column of the table above is the criterion for
what is lost. The small size of `room_id` (2.1) is an extension of the same table.

**The availability limits are stated here too.** The table above covers confidentiality and
impersonation; the two paths below are how an unauthenticated caller slows the service down.
v1 accepts both.

| Path | What happens | Why it is accepted |
|------|-------------------|---------------|
| Concurrent connection slot exhaustion | A caller that reopens 32 connections (`MAX_INFLIGHT`) that never send headers every 5 seconds turns the legitimate requests of that window into `unavailable` (3.4, 7.2) | A per-source concurrent connection limit would be needed, and that judgement stands on the "caller IP" assumption of the table above, so it inherits the same limit |
| `create_room` abuse | `create_room` takes only a `client_nonce` and answers with success, so 6.4 Rate Limit does not count it. Repeating it with a changed nonce produces a four-item transactional write per call and those items survive up to a day past expiry (6.5). Exhausting the provisioned write capacity throttles legitimate requests into `internal` | The reason 6.4 Rate Limit gives for not counting success is about `get_peers`, and `create_room` has no polling. A per-source room creation limit could be added, but v1 does not add it. **Observation is done.** The per-source frequency of the `room.created` log (7.5) is the evidence |

**Neither of these hijacks a room or changes a value.** They only slow it down or stop it. That
is why they are stated apart from the trust assumption table above.

### 1.3 Mapping to the protocol.md Section 14 Contracts

This is an index of where the [`protocol.md`](protocol.md) section 14 Control Plane Schema
contracts are met. That document owns the contract text, and it is not copied here.

| Section 14 contract | Where it is met |
|-----------|---------------|
| 1. `peer_id` unique within a room | 2.2 `peer_id`, 6.3 Items (conditional write of the `PEER#` item) |
| 2. `not_ready` before both sides of the pair are registered | 4.5 `get_peers`, 5.2 Ready |
| 3. `punch_delay_ms` fixed once per pair | 4.6 `host_report`, 6.3 Items (conditional write of the `PAIR#` item) |
| 4. `elapsed_since_ready_ms` included, monotonic clock | 4.5 `get_peers`, 4.6 `host_report`, 7.4 Clocks |
| 5. Store and deliver only what passes candidate hygiene | The hygiene case table of 4.4 `register_candidate` |
| 6. Deliver the peer's virtual IP | 4.5 `get_peers`, 4.6 `host_report` |
| 7. `peer_id` is CSPRNG 32-bit | 2.2 `peer_id` |

---

## 2. Identifiers and Constants

### 2.1 room_id

**6 characters, 32-symbol alphabet, CSPRNG.** The alphabet is the following.

```text
ABCDEFGHJKLMNPQRSTUVWXYZ23456789      (32 symbols. I, O, 0, 1 are removed)
```

- Entropy is 32^6 = 2^30. **About one billion values, a size that can be guessed online**
  - The length is 6 because a person types the value into a console, and the price for that is the
    rate limit in 6.4 Rate Limit
  - Without the rate limit, the only thing that bounds the number of requests needed to find an
    open room is server throughput
  - Even with the limit, it slows guessing, it does not stop it. This belongs in the limits table
    of 1.2 Trust Assumptions
- `I`, `O`, `0`, `1` are removed because people confuse them when reading and copying. A character
  outside the alphabet is an error. **Do not reinterpret it as the look-alike character.** If `0`
  were corrected to `O`, two inputs would point to the same room and shrinking the alphabet would
  have no meaning
- Input is **case-insensitive.** The server upper-cases the received value and then checks it
  against the alphabet. Lowercase `a` is `A`. This is not a look-alike substitution; it is a
  spelling difference of the same character
- At creation, the server catches collisions with the conditional write of the `ROOM` item (6.3)
  and redraws on collision. The attempt cap is `MAX_ROOM_ID_ATTEMPTS` (4). Beyond that, it is an
  `internal` error. At a scale of tens of open rooms, a collision in a 30-bit space effectively
  does not happen; the cap exists to stop a storage error from turning into infinite retries

**Case table.** Normalization and validation have to pass this table. Phase 3 tests run this table
as is.

| Input | Normalized | Verdict |
|------|-------------|------|
| `ABCDEF` | `ABCDEF` | Pass |
| `abcdef` | `ABCDEF` | Pass |
| `aBcDeF` | `ABCDEF` | Pass |
| `ABCDE` | - | Reject `bad_request` (5 characters) |
| `ABCDEFG` | - | Reject `bad_request` (7 characters) |
| `ABCDE0` | - | Reject `bad_request` (`0` is outside the alphabet) |
| `ABCDEO` | - | Reject `bad_request` (`O` is outside the alphabet) |
| `ABCDE1` / `ABCDEI` | - | Reject `bad_request` |
| ` ABCDEF` (leading space) | - | Reject `bad_request`. Whitespace is not stripped |
| `ＡＢＣＤＥＦ` (full-width) | - | Reject `bad_request`. ASCII only |
| `""` (empty string) / field missing / not a string | - | Reject `bad_request` |

### 2.2 peer_id

**CSPRNG 32-bit, 0 excluded, unique within a room.** The wire header fixes `peer_id` at 4 bytes
([`protocol.md`](protocol.md) 4.2 `peer_id`) and the server issues this value.

- Draw with `secrets.randbits(32)` and redraw if it is 0
- Uniqueness within a room is guaranteed by the conditional write of the `PEER#<peer_id>` item (6.3)
- On collision, redraw; the attempt cap is `MAX_PEER_ID_ATTEMPTS` (4)

Sequential assignment is not used. The reason is in "Why It Was Decided This Way" below.

### 2.3 peer_token

**128-bit CSPRNG, 32 lowercase hex characters.** `secrets.token_hex(16)`. It is carried in the
join (or room creation) response and then included in every request from that peer.

**The server stores the raw value** (6.3).

- If only a hash were stored, a client that lost the response and came back with the same
  `client_nonce` (2.4, 4.2, 4.3) could not be given the same token
- The price is that a token leaks if the store is read. In v1, where the path is plaintext and
  `room_id` is the only secret, that price is of the same kind as one already being paid (1.2).
  Revisit this line when authentication is introduced

Why the token is needed is in "Why It Was Decided This Way" below.

### 2.4 client_nonce

**Client-generated 128-bit CSPRNG, 32 lowercase hex characters.** Carried in `create_room` and
`join_room` requests. It is an idempotency key. It stops a request that was retried after a lost
response from taking the second peer slot again (4.2, 4.3).

The client uses **one value per join** and does not change it between retries of that join.
When it leaves a room and joins the next one, it draws a new value.

> **Why.** The unit of work that has to be idempotent is one join. Fixing one value per launch
> makes cancellation reason 1 of 6.3 treat a rejoin from the lobby as an "already completed
> request" and return the response of the old room. Changing the value within one join destroys
> idempotency instead.

### 2.5 Virtual IP Pool

| Item | Value |
|------|-----|
| Range | `10.100.0.0/24` ([`spec.md`](spec.md) FR-4) |
| Host (room creator) | `10.100.0.1`. Fixed at room creation |
| Participant pool | From `10.100.0.2` to `10.100.0.(MAX_PEERS)`. `MAX_PEERS` is 5, so the pool is the four addresses `10.100.0.2` through `10.100.0.5` |
| Assignment order | Walk the pool from the lowest address and claim the `VIP#<ip>` item with a conditional write (6.3). On failure, move to the next address. When the pool is exhausted, `room_full` |
| Attempt cap | The pool size. One pass over the pool and it ends. It does not loop forever |
| Reclaim | Two ways. When the host reports that peer's departure with 4.6 `host_report`, the `PEER#` and `VIP#` items are deleted together. A participant that never registered a candidate and whose `JOIN_REGISTER_GRACE_S` has passed is deleted by the server while it processes the same request (4.6). When the room expires, the remaining items are deleted by TTL (6.5) |

**A reclaimed address is used by the next `join_room` new join right away.** No slot is held open
for a return. There is no rejoin (4.3), so there is no path by which the peer that used that
address asks for it again.

**The precondition for reclaim is that the tunnel session of that peer is terminated.** The host
reports only after the session has ended (4.6). Server reclaim does not check this condition
separately. The host cannot confirm a peer with no candidates (step 3 of the 4.6 processing order),
so no tunnel session ever existed for it. Reclaiming the address of a session that is still
alive makes two sessions claim one row of the client routing table
([`protocol.md`](protocol.md) 8.5 Transmit-Side Validation).

Raising `MAX_PEERS` grows the pool by that much and nothing else changes. Six or more
participants is a `spec.md`
stretch goal.

### 2.6 Constants

```python
CONTROL_PORT              = 8000        # TCP. windows-prereq.md section 6 references this value
MAX_PEERS                 = 5           # protocol.md section 1. Five peers per room
ROOM_LEASE_S              = 120         # How far one host_report pushes the expiry time (4.6, 5.1)
HOST_REPORT_OPEN_S        = 5           # host_report interval while a slot is open (4.6)
HOST_REPORT_FULL_S        = 30          # Interval once the room is full (4.6)
JOIN_REGISTER_GRACE_S     = 90          # Until the server reclaims a participant with no candidates (2.5, 4.6)
STORAGE_GRACE_S           = 86400       # Grace from expiry to DynamoDB TTL deletion (6.5)
PUNCH_DELAY_MS            = 1000        # protocol.md 10.2. Written to the pair once when it becomes ready
MAX_CANDIDATES            = 8           # Same value as protocol.md section 3. Per peer
MAX_BODY_BYTES            = 4096        # Request body cap (3.3)
MAX_HEADER_BYTES          = 2048        # Cap on request line plus all headers (3.3)
SERVER_READ_TIMEOUT_S     = 5           # To receive one whole request (3.4)
CLIENT_CONNECT_TIMEOUT_S  = 3           # Client connect (8.2)
CLIENT_IO_TIMEOUT_S       = 3           # Client send / receive, each (8.2)
MAX_ROOM_ID_ATTEMPTS      = 4           # Redraw cap on room_id collision (2.1)
MAX_PEER_ID_ATTEMPTS      = 4           # Redraw cap on peer_id collision (2.2)
RATE_LIMIT_BUCKET         = 10          # Per-source failure budget (6.4)
RATE_LIMIT_REFILL_PER_MIN = 10          # Refill per minute (6.4)
MAX_RATE_ENTRIES          = 4096        # Rate limit table cap (6.4)
MAX_INFLIGHT              = 32          # Cap on requests in flight (7.2)
```

`MAX_CANDIDATES` and `PUNCH_DELAY_MS` are copies of values owned by
[`protocol.md`](protocol.md). **When that document changes, fix them here to follow.**

> **Why.** These two are the only copies allowed because the server code has to have the values,
> and the server code does not read C++ headers.

### Why It Was Decided This Way

- **2.2 `peer_id` does not use sequential assignment.** The threat premise of
  [`protocol.md`](protocol.md) section 2 Security Model is "knows or guesses the `peer_id`".
  With sequential assignment the guess becomes trivial and that document's forged `HELLO` attack
  works from off-path. A 32-bit random value is a supporting measure that makes that guess
  harder. A forged `HELLO` arrives at the client directly over UDP, so the server rate limit
  (6.4) cannot slow that guessing, and this document does not compute an upper bound on the cost
  of guessing. **It hides nothing from an on-path observer.** It is plaintext
- **2.3 `peer_token` is needed for two reasons.** (1) If `register_candidate` worked with only
  `room_id` and `peer_id`, a third party who knows the `room_id` could overwrite the other peer's
  candidates and make that peer send its `HELLO` to an arbitrary address. (2) 4.6 `host_report`
  deletes someone else's item, so there has to be a basis for deciding that the caller is the
  host itself

---

## 3. Transport and Encoding

### 3.1 Why an HTTP Subset

The transport is an **HTTP/1.1 subset over TCP** and the body is JSON. Three options were weighed.

| Option | Advantage | Disadvantage |
|----|------|------|
| **HTTP/1.1 subset (chosen)** | Can be tested with `curl`. Existing procedures such as server-side `ss`, security groups, and `Test-NetConnection` ([`windows-prereq.md`](windows-prereq.md) section 6) fit as they are | Both sides have to be hand-written. The subset is pinned narrow to reduce that cost |
| Line-delimited JSON over TCP | Simplest implementation | Cannot be tested with standard tools. One more test tool would have to be built |
| Web framework | No parsing to write | Needs [`spec.md`](spec.md) NFR-5 approval, and the client-side cost stays the same |

The subset is all of 3.3 HTTP Subset below. **HTTP features not listed there are not supported,
and are rejected when received.**

> **Why.** That is better than pretending to support them and failing silently.

### 3.2 Address

| Item | Value |
|------|-----|
| Protocol | TCP, IPv4 |
| Server bind | `0.0.0.0:8000`. How to verify is in [`windows-prereq.md`](windows-prereq.md) section 6 |
| Address the client receives | **A DNS name or an IPv4 literal.** Received as a launch input ([`architecture.md`](architecture.md) 3.5 Startup Inputs). The DNS name is an A record pointing at an Elastic IP. Why an Elastic IP is needed is in `windows-prereq.md` section 10 |
| Path prefix | `/v1/`. If the encoding changes incompatibly, open `/v2/`. The same server can serve both prefixes at once |

DNS resolution is done by the client's `[control]` thread ([`concurrency.md`](concurrency.md)
chapter 8 The `[control]` Thread). If resolution returns several results, use the first IPv4
address and ignore the rest. `AAAA` is not used. This is the same scope as the tunnel being
IPv4 only ([`protocol.md`](protocol.md) section 1 Scope and Assumptions).

### 3.3 HTTP Subset

**Request.**

```text
POST /v1/<op> HTTP/1.1\r\n
Host: <server address>\r\n
Content-Type: application/json\r\n
Content-Length: <body byte count>\r\n
Connection: close\r\n
\r\n
<JSON body>
```

**Response.**

```text
HTTP/1.1 <status> <reason>\r\n
Content-Type: application/json\r\n
Content-Length: <body byte count>\r\n
Connection: close\r\n
\r\n
<JSON body>
```

| Item | Rule |
|------|------|
| Method | `POST` only. The query (`get_peers`) is also `POST`. Credentials (`peer_token`) go in the body so they do not end up in URLs or logs |
| Path | `/v1/` + an operation name from section 4. No query string |
| Body length | `Content-Length` is required. If `Transfer-Encoding` is present, reject regardless of its value |
| Connection | One connection per request. The server closes after the response. No keep-alive, no pipelining |
| Headers | Names are case-insensitive. Headers not in the table above are ignored (ignored even if they appear twice). **A header that is in the table is rejected if it appears twice.** For a duplicate `Content-Length`, see below |
| Body | One UTF-8 JSON **object**. Arrays and scalars are rejected. Unknown keys are ignored (forward compatibility) |
| Size | Request line plus headers total `MAX_HEADER_BYTES` (2048), body `MAX_BODY_BYTES` (4096) |
| Encoding | Strings are UTF-8. Integers are JSON numbers with no decimal point. `uint32` fields are 0 or more and 4294967295 or less |
| Compression, chunking, 100-continue, upgrade, authentication headers | None |
| Byte-exact match | The request line is `POST <path> HTTP/1.1` and the separator is a single space. Two spaces, a tab, the absolute form (`POST http://host/v1/op`), a trailing `/`, and percent encoding (`%5F`) are all rejected. The path is case-sensitive |
| `Content-Length` value | Only `^[0-9]+$` is accepted. `+17`, `1_7`, surrounding whitespace, and full-width digits are rejected. Do not leave this to the language's integer conversion function. Python `int()` accepts the first three |
| Header folding (obs-fold) | Reject a header line that starts with a space or a tab. Do not join it to the previous line |
| `Host`, `Content-Type` | **Not checked.** The client always sends them (the format above) but the server does not look at the values. There is no virtual hosting and no content negotiation |
| Body longer than `Content-Length` | Read only the leading `Content-Length` bytes, respond, then close the connection. The remaining bytes are not read. One request per connection, so those bytes cannot become a second request |

**A duplicate `Content-Length` is rejected in particular.** Rejecting a message with several
`Content-Length` values that differ is a rule of RFC 9112 section 6.3, and an upstream and
downstream intermediary choosing different values is one form of request smuggling. Reject even
if the values are equal. There is no reason to distinguish.

**Server parsing case table.** The decision function has to pass this table. For each row, inject
one mutation and check that the row `FAIL`s.

| Input | Response |
|------|------|
| Normal `POST /v1/get_peers`, correct `Content-Length`, JSON object | Operation result |
| `GET /v1/get_peers` | `405` `method_not_allowed` |
| `POST /v1/unknown_op` | `404` `unknown_op` |
| `POST /get_peers` (no prefix) | `404` `unknown_op` |
| `POST /v1/get_peers?x=1` | `404` `unknown_op`. Query strings are not supported |
| No `Content-Length` | `411` `length_required` |
| `Content-Length: abc` / negative / twice | `400` `bad_request` |
| `Content-Length: 4097` | `413` `too_large`. Respond without reading the body, then close |
| `Transfer-Encoding: chunked` | `400` `bad_request` |
| Request line + headers exceed 2048 bytes | `431` `too_large` |
| Body shorter than `Content-Length` and nothing more arrives within 5 seconds | Close the connection without a response. Counter `http_read_timeout` |
| Body is not JSON / array / scalar | `400` `bad_request` |
| Body is not UTF-8 | `400` `bad_request` |
| `HTTP/1.0` | `400` `bad_request`. Only `HTTP/1.1` is accepted |
| Normal request with unknown header `X-Foo: bar` | Operation result. Ignored |
| Unknown header `X-Foo` twice | Operation result. Headers outside the table are ignored even when duplicated |
| `content-length: 17` (lowercase) | Operation result. Names are case-insensitive |
| `Content-Length: 4096` | Operation result. It is inside the cap. An implementation that wrote `>=` by mistake fails on this row |
| Request line + headers exactly 2048 bytes | Operation result. It is inside the cap |
| `Content-Length: +17` / `1_7` / ` 17` | `400` `bad_request`. The value is `^[0-9]+$` only |
| Header line starts with a space (obs-fold) | `400` `bad_request` |
| `POST  /v1/get_peers HTTP/1.1` (two spaces) / `POST /v1/get_peers/ HTTP/1.1` / `POST http://h/v1/get_peers HTTP/1.1` | `400` `bad_request`. The request line has to match byte for byte |
| No `Host` | Operation result. It is not checked |
| `Content-Type: text/plain` | Operation result. It is not checked |
| Body longer than `Content-Length` | Operation result. Read only the leading `Content-Length` bytes, then close |
| `GET /v1/unknown_op` | `405` `method_not_allowed`. By the check order below, the method comes before the path |
| `Transfer-Encoding: chunked` with no `Content-Length` | `400` `bad_request`. `Transfer-Encoding` comes before a missing `Content-Length` |

**Check order.** Deciding the response to a compound defect needs an order. Read from the top.

1. Byte format of the request line and `HTTP/1.1` -> `400`
2. Method -> `405`
3. Path (the `/v1/` prefix and the operation name, no query string) -> `404`
4. Total header size -> `431`
5. Header syntax (folding, a duplicate of a header that is in the table) -> `400`
6. Presence of `Transfer-Encoding` -> `400`
7. Presence of `Content-Length` -> `411`, value format -> `400`, over the cap -> `413`
8. Body read and time limit (3.4) -> close without a response
9. Is the body a UTF-8 JSON object -> `400`

The body of `400`-class errors is also the error envelope of 4.1 Common Envelope. **A parse
failure response does not echo the request content.**

> **Why.** Otherwise the server becomes a place that reflects arbitrary bytes.

### 3.4 Server-Side Time Limits

From accepting the connection to reading one whole request is `SERVER_READ_TIMEOUT_S` (5 seconds).
Beyond that, close without a response and increment `http_read_timeout`. The same 5 seconds
applies to sending the response.

**State what it blocks and what it does not.** What this value blocks is the occupancy time of
one connection.

**It does not block exhaustion of the concurrent connection slots.** `MAX_INFLIGHT` (32) is
incremented right after accept, before parsing (7.2), so a caller that reopens 32 connections
that never send headers every 5 seconds turns every legitimate request of that window into
`unavailable`.

- That caller does not spend the budget of 6.4 Rate Limit either, because `http_read_timeout` is
  not one of the three that section counts
- A legitimate client retries by the rules of 8.3 Error Classification and Retry and then ends in
  `CONTROL_PLANE_EXCHANGE_FAILED`
- Blocking it would need a per-source concurrent connection limit, and that inherits the source
  assumption of 6.4 Rate Limit (1.2) as it is

**This is a limit that v1 accepts.** It belongs in the limits table of 1.2 Trust Assumptions.

### 3.5 Checks the Client Makes

The client looks at only the following in a response. Everything else is ignored.

1. Does the status line start with `HTTP/1.1 <3-digit number>`? If not, it is a transport error
   that leads to `CONTROL_PLANE_EXCHANGE_FAILED` (8.3)
2. One `Content-Length` header. If missing or larger than `MAX_BODY_BYTES`, transport error
3. Read the body to that length and parse it as a JSON object. On failure, transport error
4. Split success and error by the `ok` field (4.1). **Not by the HTTP status code**
   - The status code is for the person holding `curl`; the program looks at the body
   - An implementation where the two disagree has to be caught in tests. But if the client looked
     at both, which one to trust when they disagree would differ per implementation

---

## 4. Operations

There are five operations. `create_room`, `join_room`, `register_candidate`, `get_peers`,
`host_report`.

**The first four are called by anyone and `host_report` is called only by the host.** 4.6 fixes
that judgement.

**There is no `register_peer`.** Joining ends with `create_room` and `join_room`, so a separate
operation has nothing to do. The client does not have to decide "when is it `join_room` and when
is it `register_peer`".

**There is no `leave_room` either.** No operation is called by the one who leaves. What knows
whether the slot may be reclaimed is the host, one end of that tunnel session (2.5 Reclaim), and
if the leaver calls it, a state appears where the slot is free before the host has closed the
session. A participant that left without candidates never had a session, so the server reclaims it
(4.6 server reclaim).

### 4.1 Common Envelope

**Success.**

```json
{"ok": true, ...operation-specific fields}
```

**Error.**

```json
{"ok": false, "error": "<error code>", "message": "<one line for humans>"}
```

`error` is one of the strings in the table below and is what the program reads. `message` is for
humans and its content is not pinned. **Do not put the request body or tokens in `message`.**

| Error code | HTTP | Meaning | Client action |
|-----------|------|-----|-----------------|
| `bad_request` | 400 | Required field missing, format violation, 2.1 alphabet violation | Do not retry. `CONTROL_PLANE_EXCHANGE_FAILED` |
| `method_not_allowed` | 405 | Not `POST` | Same |
| `unknown_op` | 404 | Path is not an operation from section 4 | Same |
| `length_required` | 411 | No `Content-Length` | Same |
| `too_large` | 413 / 431 | Body or header cap exceeded | Same |
| `room_not_found` | 404 | No room with that `room_id` | Same. **An expired room is not this code either.** See `room_expired` below |
| `room_expired` | 410 | The room exists but its expiry time has passed (5.1) | Same |
| `room_full` | 409 | Every address in the participant pool is already claimed (2.5) | Same |
| `unauthorized` | 403 | `peer_token` does not belong to that `peer_id` | Same |
| `rate_limited` | 429 | The per-source budget of 6.4 Rate Limit is exhausted | Same. The client does not wait and retry on its own. A person restarts it. Only `host_report` is the exception (4.6) |
| `internal` | 500 | Storage error, redraw cap reached | **Transient error.** Follow the rules of 8.3 Error Classification and Retry |
| `unavailable` | 503 | `MAX_INFLIGHT` exceeded (7.2) | Same. Transient error |

**`bad_request`, `rate_limited`, `internal`, and `unavailable` can occur on any operation.**
Parsing, rate limiting, and storage errors happen in the common stages before and after the
operation (7.2, 7.3). The per-operation "Errors" lines below list only what is specific to that
operation and do not repeat these four.

Why `room_not_found` and `room_expired` are separate is in "Why It Was Decided This Way" below.

### 4.2 create_room

Called by the host. Creates the room, registers the host as the first peer, and gives it
`10.100.0.1`.

**Request.**

| Field | Type | Required | Meaning |
|------|-----|:---:|-----|
| `client_nonce` | 32-character hex string | Yes | Idempotency key (2.4) |

**Response.**

| Field | Type | Meaning |
|------|-----|-----|
| `room_id` | 6-character string | 2.1 |
| `peer_id` | uint32 | 2.2 |
| `peer_token` | 32-character hex | 2.3 |
| `virtual_ip` | dotted-decimal string | `10.100.0.1` |
| `expires_in_s` | integer | Seconds left until expiry, from the moment of the response. No absolute time is given. The client clock is not trusted |

**Idempotency.** If the same `client_nonce` comes again, return **the same response for the room
already created.** Only `expires_in_s` shrinks. If that room has expired, `room_expired`. No new
room is created. The basis for the decision is the `NONCE#` item (6.3).

**Errors.** None specific to the operation. Only the common four (4.1).

### 4.3 join_room

**Called by the participant.** It claims a virtual IP from the pool and issues a new `peer_id`
and `peer_token`. There is one form.

**There is no rejoin.** If a process dies, that peer's slot is reclaimed on the host's report
(2.5 Reclaim), and coming back in is a new join. It receives a new `peer_id`, and the virtual IP is
assigned again in the 2.5 assignment order. If the old address is free at that time, the same value
can come out.

> **Why.** Holding a slot open for a return means the address cannot be reclaimed while the room
> is alive. A late participant using that slot and a departed peer getting it back cannot both
> hold.

**Request.**

| Field | Type | Required |
|------|-----|:---:|
| `room_id` | string | Yes. Goes through 2.1 normalization |
| `client_nonce` | 32-character hex | Yes |

**Response.**

| Field | Type | Meaning |
|------|-----|-----|
| `room_id` | 6-character string | The normalized value. The client uses this value from then on |
| `peer_id` | uint32 | |
| `peer_token` | 32-character hex | The authentication secret for this join (2.3) |
| `virtual_ip` | dotted-decimal string | |
| `expires_in_s` | integer | The lease remaining (5.1). Used for display only |

**Idempotency.** If the same `room_id` and `client_nonce` come again, return what was first
issued, as is. The `NONCE#` item is the basis. A different `client_nonce` is a new join, and if
every address in the pool is already claimed, `room_full`.

> **Why.** A client that lost the response and retries with a changed nonce fills the room by
> itself. That is why 2.4 `client_nonce` fixed one value per join.

**`room_full` is decided by the pool walk alone.** It is this error only when one pass over the
pool finds no free address. Whether the room is `ready` is not considered (5.1).

> **Why.** Ready is decided per pair, so a room that already has a connected pair can still have
> a free slot. The truth about a slot is the `VIP#` claim and the peer count is derived from it.
> Deciding it in two places makes them disagree right after a reclaim.

**Errors.** `room_not_found`, `room_expired`, `room_full`. The common four (4.1) are not listed
separately.

### 4.4 register_candidate

Called by a peer after it finishes STUN. It **replaces the whole candidate list.** It is not an
append. Call it twice and the second list remains.

> **Why.** Replace semantics were chosen because they are idempotent. Sending again after a lost
> response gives the same result.

**Request.**

| Field | Type | Required |
|------|-----|:---:|
| `room_id` | string | Yes |
| `peer_id` | uint32 | Yes |
| `peer_token` | 32-character hex | Yes |
| `candidates` | array | Yes. Length 1 or more, `MAX_CANDIDATES` (8) or less |

Candidate element:

| Field | Type | Meaning |
|------|-----|-----|
| `ip` | dotted-decimal IPv4 string | |
| `port` | integer 1~65535 | |
| `kind` | `"local"` or `"reflexive"` | The two kinds from [`protocol.md`](protocol.md) 10.1 Candidate Collection and Hygiene. The server only stores and delivers it. It is not used in any decision |

**Hygiene.** The server applies the rules of `protocol.md` 10.1 Candidate Collection and Hygiene
(section 14 contract 5). The client also applies the same rules again to the list it receives.
Regardless of trusting the server, both sides do the format check. The case table below expands
those rules **for server input**, and that section owns the rules themselves.

| Candidate | Result | Basis |
|------|------|------|
| `192.168.0.10:51000 local` | Store. `accepted`+1 | Normal local candidate |
| `203.0.113.7:51000 reflexive` | Store. `accepted`+1 | Normal reflexive candidate |
| `255.255.255.255:51000` | Drop that candidate only. `rejected`+1 | 10.1 broadcast |
| `224.0.0.1:51000` / `239.255.255.250:1900` | Drop that candidate only. `rejected`+1 | 10.1 multicast `224.0.0.0/4` |
| `0.0.0.0:51000` | Drop that candidate only. `rejected`+1 | 10.1 unspecified |
| `127.0.0.1:51000` | Drop that candidate only. `rejected`+1 | 10.1 loopback. **Unlike receive source hygiene (section 7), the candidate list rejects loopback** |
| `10.0.0.5:1` / `10.0.0.5:65535` | Store. `accepted`+1 | Both ends of the port range are normal. An implementation that wrote the cap as `>=` fails on this row |
| `10.0.0.5:0` | Drop that candidate only. `rejected`+1 | 10.1 port 0. The hygiene rules fix that value, so it is not a format violation |
| `10.0.0.5:65536` / `-1` / `"51000"` (string) | **`bad_request` for the whole request** | `port` is not a JSON integer in 1~65535. It is a type violation |
| `"10.0.0"` / `"10.0.0.5.1"` / `"::1"` / `"10.000.0.5"` | **`bad_request` for the whole request** | `ip` is not four-octet dotted-decimal IPv4. It is a type violation. For the parser, see below |
| `kind` is not `"local"`/`"reflexive"`, or one of `ip`, `port`, `kind` is missing | **`bad_request` for the whole request** | It is a type violation |
| Same `ip:port` twice | Store one. `accepted`+1, **`rejected` unchanged** | 10.1 deduplication. It is the same endpoint even if `kind` differs. A duplicate is not a hygiene rejection, so it is not counted. Therefore `accepted + rejected` can be smaller than the number sent |
| 9 candidates (all normal) | **`bad_request` for the whole request** | A client sending more than 8 is a format violation. The "drop the excess" of 10.1 Candidate Collection and Hygiene is a client rule for a **received** list; the server enforces the cap on the sender |
| 8 candidates (all normal) | Store. `accepted`=8 | It is inside the cap |
| Empty array | **`bad_request` for the whole request** | With no candidates there is nothing to register |
| A list with only hygiene failures (for example one loopback) | **`bad_request` for the whole request** | 0 candidates remain to store. See the rejection policy below |

**Do not use a lenient parser of the `inet_aton` kind.** On this repo's Windows Python 3.11
version, `socket.inet_aton` accepted `10.0.5` as `10.0.0.5` and `010.0.0.5` as `8.0.0.5` (octal).

**Rejection policy. There are only two results, and the "Result" column above states which one
each row takes.**

| What | Result |
|------|------|
| **Hygiene rejection** — the address or port value hits a rule of 10.1 Candidate Collection and Hygiene (broadcast, multicast, unspecified, loopback, port 0) | Drop that candidate only and store the rest. Increment `rejected`; the response is `ok` |
| **Type violation** — `ip`, `port`, or `kind` is out of type or range or missing, or the list has 9 entries or is empty | **The whole request is `bad_request`.** It is a request problem, not an individual candidate problem |

**The dividing line is "does the value hit a rule" versus "is it not the right type".**
`10.0.0.5:0` is the integer `0` by type and the rules reject that value, so it is a hygiene
rejection. `10.0.0.5:65536` is out of range, so it is a type violation. Without this distinction,
both `{"ok": true, "rejected": 1}` and `400 bad_request` read as correct for the same input and
no test can be written.

**If everything is rejected by hygiene and 0 candidates remain to store, it is `bad_request`.**
Storing 0 and returning `ok` would make the peer receive an empty list after ready, end in
`HOLE_PUNCH_TIMEOUT`, and disguise that failure as a NAT failure.

**The client-side result differs.** On hygiene rejection, `rejected` is nonzero, so the client
leaves one `WARN` line and continues. `bad_request` is a final error, so it ends in
`CONTROL_PLANE_EXCHANGE_FAILED` with no retry (8.3).

**Response.**

| Field | Type | Meaning |
|------|-----|-----|
| `accepted` | integer | Number of candidates stored |
| `rejected` | integer | Number of candidates dropped by hygiene. If nonzero, the client leaves one `WARN` line |

**This operation does not write ready.** It stores the candidates and ends. The ready state of a
pair is written when the host confirms that pair with 4.6 `host_report` (5.2).

> **Why.** One end of a pair is always the host, and if the peer starts punching while the host
> does not know that pair exists, no packet leaves the host side and no hole opens
> ([`protocol.md`](protocol.md) 10.3 Punch). Putting the reference time at the moment the host
> knows makes both peers receive the same value.

**The client uses the success response of this operation as the polling start point**
(`protocol.md` section 11 Timers). Right after registration it is not ready yet, so `get_peers`
returning `ready: false` is normal.

**Errors.** `room_not_found`, `room_expired`, `unauthorized`. The common four (4.1) are not listed
separately.

### 4.5 get_peers

**Called by players.** The host does not call it. The host receives the same information in the
response of 4.6 `host_report`.

The client polls at the interval (500ms) and deadline (60s) of [`protocol.md`](protocol.md)
section 11 Timers. **That document owns the interval and the deadline.** The numbers are not
repeated here.

**Request.**

| Field | Type | Required |
|------|-----|:---:|
| `room_id` | string | Yes |
| `peer_id` | uint32 | Yes |
| `peer_token` | 32-character hex | Yes |

**Response.**

| Field | Type | Meaning |
|------|-----|-----|
| `ready` | boolean | True if `peers` holds at least one element that is ready |
| `peers` | array | Peers **excluding self.** When a player calls it the length is 1. The element is the host |

`peers` element:

| Field | Type | Meaning |
|------|-----|-----|
| `peer_id` | uint32 | |
| `virtual_ip` | dotted-decimal string | Used for the `virtual_ip` check in `protocol.md` 5.1 `HELLO` (section 14 contract 6) |
| `ready` | boolean | True if the `PAIR#` item of this pair exists |
| `punch_delay_ms` | integer | The value written to that pair. Present only when `ready` is true |
| `elapsed_since_ready_ms` | integer 0 or more | From the moment that pair's ready was written to the moment this response is built. Present only when `ready` is true. How it is computed is in 7.4 |
| `candidates` | array | Same element type as 4.4 `register_candidate`. In stored order. Present only when `ready` is true |

**The field set of an element is decided by `ready`.** The case table spells it out.

| State of that pair | Fields in the element |
|--------------|------------------|
| No `PAIR#` | `peer_id`, `virtual_ip`, `ready: false` |
| `PAIR#` present | The above plus `punch_delay_ms`, `elapsed_since_ready_ms`, `candidates` |

> **Why.** Handing over candidates before ready would let the client start punching without the
> host's confirmation. That confirmation in 4.6 is what aligns the punch times (5.2), so before
> it there is no address to shoot at.

**The ready state of a pair is the existence of that `PAIR#` item.** Peer count and candidate
count are not recounted at response time.

> **Why.** If they were recounted, ready would flip back to false the moment one peer registered
> its candidates again, and that would diverge from the punch the peer has already started.

**There is no ready response with 0 peer candidates.** What writes the `PAIR#` is the host, and
the host writes it only after confirming the candidates of both sides (4.6). If the client
receives an empty list anyway, it does not treat the response as ready and keeps polling. If it
hits the deadline, it is `CONTROL_PLANE_EXCHANGE_FAILED` by the polling deadline rule of
`protocol.md` 9.6 Failure Transitions.

> **Why check anyway.** Entering the punch with an empty list would wait 10 seconds with nowhere
> to send and end in `HOLE_PUNCH_TIMEOUT`, and that is a control plane failure disguised as a NAT
> failure. This is the same judgment as [`architecture.md`](architecture.md) section 8 Failure
> Diagnosis classifying "no peer candidates" as `CONTROL_PLANE_EXCHANGE_FAILED`.

**Self is excluded by `peer_id`.** Two peers cannot have the same `peer_id` (2.2), so if the
response contains a `peer_id` equal to one's own, it is a server defect. In that case the client
ends with `CONTROL_PLANE_EXCHANGE_FAILED` (`protocol.md` 4.2 `peer_id`).

**Errors.** `room_not_found`, `room_expired`, `unauthorized`. The common four (4.1) are not listed
separately.

**`not_ready` is not an error.** It is `ok: true` with `ready: false`.

> **Why.** Polling is progressing normally, and putting it in the error envelope would give the
> client an exception path of "error, but continue".

### 4.6 host_report

**Called only by the host.** One request does three things. It renews the room lease, reclaims
the slots of ended sessions, and writes the ready state of pairs that are not confirmed yet. The
response is the current room state the host has to know.

> **Why one request.** All three are things the host says to the control plane periodically.
> Splitting them would mean three copies of the same authentication and the same rate limit
> budget, and the cases where the three requests arrive out of order would have to be handled
> separately.

**Interval.** While a slot is open, call it every `HOST_REPORT_OPEN_S` (5 seconds); once the room
is full, every `HOST_REPORT_FULL_S` (30 seconds). The decision uses the `peers` length of the
previous response.

**When a session ends, call it once right away without waiting for the next cycle.** The `departed`
of that call carries the ended peer. If a request is outstanding, call it right after that response
arrives. The one-at-a-time rule ([`concurrency.md`](concurrency.md) chapter 8 The `[control]`
Thread) stays as is. The periodic timer counts again from this call.

**The host keeps the `departed` it has not yet reported.** It puts each peer whose session ended into
that set and carries the whole set in every `host_report`. On a success response it removes what
that request carried from the set. It removes them even if they are not in `released`, since that
means the peer was already gone. On failure it leaves the set as is and carries it again in the next
call.

- The set does not exceed `MAX_PEERS - 1`. An unreported peer still holds a slot on the server, so
  their number cannot exceed the pool size
- Reclaim is idempotent, so carrying the same peer twice is safe

> **Why.** When one person leaves a full room, the slot frees only after the host reports it.
> Following the cycle alone, that gap is up to 30 seconds, and a join arriving in that window gets
> `room_full`. Calling right away shrinks the gap to one request. When a player process dies, the
> host too learns of it only after the idle timeout ([`protocol.md`](protocol.md) section 11), so
> this rule does not shrink that case.

**Both intervals have to be smaller than `ROOM_LEASE_S`.** Otherwise a healthy host's room dies of
lease expiry. The current values are 5 and 30 against 120, which **tolerates four consecutive
failures.** Keep that margin in view when changing the values.

**Request.**

| Field | Type | Required | Meaning |
|------|-----|:---:|-----|
| `room_id` | string | Yes | |
| `peer_id` | uint32 | Yes | The caller. It has to be the host's `peer_id` |
| `peer_token` | 32-character hex | Yes | |
| `departed` | array | No | The `peer_id`s whose sessions have ended. Absent means an empty array or omitted. Length at most `MAX_PEERS - 1` |
| `confirm` | array | No | The `peer_id`s of the peers to write as ready. Same constraint |

**Caller decision.** It is processed only when `peer_id` equals `ROOM.host_peer_id` and
`peer_token` is that peer's token. If either does not match, it is `unauthorized`.

> **Why only the host.** Whether a slot may be reclaimed is decided by whether that tunnel session
> has ended (2.5 Reclaim), and what knows that is the host, one end of the session. If the leaver
> calls it, a state appears where the slot is free before the host has closed the session.

**Response.**

| Field | Type | Meaning |
|------|-----|-----|
| `expires_in_s` | integer | The lease remaining after renewal. Normally `ROOM_LEASE_S` |
| `released` | array | The `peer_id`s actually deleted. Ones that were already gone are not included |
| `confirmed` | array | The `peer_id`s of the peers whose `PAIR#` this call newly wrote |
| `peers` | array | Same element type as 4.5 `get_peers`. All current peers excluding self |

**The processing order is pinned.** One request runs the steps below in order.

1. Caller decision. If it does not match, it ends with `unauthorized`
2. For each `peer_id` in `departed`, delete the `PEER#`, that peer's `VIP#`, and the `PAIR#`
   items that peer is part of (6.3). Then delete the `PEER#` and `VIP#` of the server reclaim
   targets too. "Server reclaim" below decides the targets
3. For each `peer_id` in `confirm`, if that peer and the caller **both have 1 or more candidates**
   and the `PAIR#` of that pair is absent, create it with a conditional write. The item carries
   `ready_at_*` and `punch_delay_ms = PUNCH_DELAY_MS` (6.3)
4. Renew `ROOM.expires_at_ms` to `now + ROOM_LEASE_S * 1000`
5. Read with `Query(pk, ConsistentRead=true)` and build the response

**Server reclaim.** The targets are the peers of the room read in step 1 that satisfy all of the
following. Deleting one also deletes that peer's `NONCE#`. The key is known from the `client_nonce`
of `PEER#`. Peers deleted in step 2 are put into `released`.

- Not the host
- Candidates are empty. It is `joined` of 5.3 Peer
- `joined_at_ms + JOIN_REGISTER_GRACE_S * 1000 <= now`

**The race is blocked by a storage condition.** The peer can register candidates after the read and
before the delete. So the delete condition includes "candidates are still empty" (6.3). If the
condition fails, that peer is not deleted. The next `register_candidate` of a deleted peer ends in
`unauthorized` at the token check of the 7.3 operation processing order. If the reclaim happens
after the token check passes and before the write, the conditional update of 6.3 fails, and that is
`unauthorized` too. Either way, the client gets `CONTROL_PLANE_EXCHANGE_FAILED` per 8.3 Error
Classification and Retry.

> **Why the server deletes it.** A participant that left without candidates never had a session
> with the host, so the host has no trigger to report it. Each time someone joins the same room
> again from the lobby, such a slot piles up and the pool fills. Whether candidates are empty is a
> fact the server knows directly from the store, and unlike whether a tunnel is alive, there is no
> need to ask the host. The basis is
> [ADR 0012](decisions/0012-server-reclaims-candidateless-peers.md).

> **Why 90 seconds.** The reference point `joined_at_ms` is the time the server wrote `PEER#`. The
> upper bound from there until the player's `register_candidate` succeeds is the sum of three.
>
> - Losing the `join_room` response and getting it again with the same nonce. 3 times including the
>   first attempt, at most 9 seconds each plus two 1-second intervals, is 29 seconds (8.2, 8.3)
> - The STUN stage. With the default list it is 10 seconds (`protocol.md` section 11 Timers)
> - `register_candidate` retry. 29 seconds as above
>
> A margin is put on the sum of 68 seconds. This sum is computed from the documented upper bounds
> and was not measured. There are two cases that exceed it. A long list given with `--stun` raises
> the STUN bound. And 9 seconds per attempt is the single-request bound of 8.2, and as that table
> states, the send and receive limits are per operation, so in practice it can be longer. Either
> way it ends on the `unauthorized` path above.

**Step 2 comes before step 3.** If one request carries the same `peer_id` in both `departed` and
`confirm`, the reclaim wins. Writing an ended session as ready would fill that slot again.

**Step 4 comes before step 5.** The `expires_in_s` of the response has to be the value after
renewal so that the host reads its own lease right away.

**That step 5 comes last is the mutation test of this section.** An implementation that builds the
response from a read taken before the writes of steps 1 to 4 includes the peer it just reclaimed
in `peers`.

**Case table.** `H` is the host's `host_report` and `P` is a player's `register_candidate`.

| Order | What the step 5 read sees | `PAIR#` write | What the host does next |
|------|-------------------|--------------|------------------------|
| `P` → `H(confirm=[P])` | P has candidates | Once | Punch |
| `H(confirm=[P])` → `P` | P has no candidates | **Zero. The condition of step 3 does not hold** | `confirm` again on the next cycle |
| `H` called with no `confirm` | P has candidates | Zero | Read `peers` of the response and `confirm` on the next cycle |
| `H` twice with the same `P` | — | Once. The second time the `PAIR#` already exists | Nothing |
| `H(departed=[P], confirm=[P])` | No P | Zero | Nothing |
| An implementation that writes step 3 with no condition | — | **Writes even for a pair with no candidates. This is the defect the table catches** | — |

**Server reclaim case table.** `J` is `joined_at_ms` and `G` is `JOIN_REGISTER_GRACE_S * 1000`.

| Peer | Time | Result |
|------|------|------|
| No candidates | `now < J + G` | Stays |
| No candidates | `now == J + G` | **Deleted.** The boundary is on the reclaim side. Put into `released` |
| No candidates | `now > J + G` | Deleted. That address becomes eligible for assignment again. Which join gets it follows the 2.5 assignment order |
| Has candidates | `now > J + G` | Stays. A peer that may have a tunnel is reclaimed only by the host |
| No candidates, registers after the read and before the delete | `now > J + G` | **Stays.** The delete condition fails. An implementation that deletes with no condition is caught on this row |
| The host itself, no candidates | `now > J + G` | Stays. Deleting it would make the caller itself disappear |
| `join_room` retry with the nonce of a reclaimed peer | - | A new join. There is no `NONCE#`, so it is not the idempotent response of 4.3. Because of the first term of "Why 90 seconds" above, a normal client's retry finishes before the reclaim |
| `joined_at_ms` field missing | - | **Stays.** Do not turn "cannot decide" into reclaim. Leave `peer.reclaim_skipped` of the 7.5 log. Ending the whole request as `internal` would block even that room's lease renewal |

**Idempotency.** It does not use a `client_nonce`. Sending the same request twice leaves the
second one's `released` and `confirmed` empty and the stored state the same. The renewal uses the
`now` of that moment, so only the lease is pushed further. Server reclaim also decides with the
`now` of that moment, so if a peer's grace passes between the two requests, the second one deletes
it. What idempotency guarantees is that the same `departed` and `confirm` are not applied twice.

**Errors.** `room_not_found`, `room_expired`, `unauthorized`. The common four (4.1) are not listed
separately.

**An expired room cannot be revived.** If `expires_at_ms` has already passed, it is
`room_expired` and no renewal happens. A room whose lease has lapsed is a room that has ended.

**When the host receives an error, it handles it in three groups.** This differs from the general
rule of 8.3 Error Classification and Retry. A `host_report` failure is not a failure of an attempt
but a matter of a room that already has tunnels.

| Received | Meaning | What the host does |
|---------|-----|------------------|
| `room_expired`, `room_not_found`, `unauthorized`, `bad_request` | The room has ended on the server. Sending the same request again gets the same answer | Emit the `FAIL CONTROL_PLANE_EXCHANGE_FAILED` line once and stop `host_report`. Even when a session ends, do not make the immediate call above. Leave existing sessions as they are. When all remaining sessions end, go to the lobby. If there is no session, go right away |
| `rate_limited` | The room may be alive. Another source behind the same public IP may have burned the budget (6.4) | Do not emit `FAIL`; call again on the next cycle |
| `internal`, `unavailable`, transport error | Transient error | Call again on the next cycle |

- After the room ends, the tunnels of the remaining sessions keep going ([`spec.md`](spec.md)
  NFR-3). What is lost is only new joins
- The lease can expire while `rate_limited` continues. The budget check comes before the store
  (6.4), so the responses in that window stay `rate_limited`. The first request processed once the
  budget refills gets `room_expired`, or `room_not_found` if TTL has already deleted it, and moves to
  the first row
- The list of triggers for going to the lobby is owned by `concurrency.md` chapter 7 Lobby

> **Why stop on the first row.** An expired room cannot be revived, and the first three errors of
> that row deduct from the source's rate-limit budget (6.4). Calling on gains nothing and blocks
> other users behind the same public IP.

> **Why call again on `rate_limited`.** Stopping because of a budget burned by someone else's
> guessing would kill a live room by lease expiry. The budget refills one token every 6 seconds.

### Why It Was Decided This Way

- **4.1 The two codes are separate.** A person who typed the room code wrong and a person who
  typed it right but whose host stopped signalling need different actions. If merged, neither
  can be told apart. The trade is that **a caller guessing at nonexistent rooms learns that an
  expired room exists.** An expired room cannot be joined, so revealing it loses nothing
- **4.6 Calling with a `peer_id` that is missing is `unauthorized`.** The room exists. What is
  missing is the peer. A nonexistent `peer_id` and a `peer_id` with a wrong token have to be
  answered with the same code so that a caller who knows only the `room_id` cannot enumerate the
  `peer_id`s in the room. Enumeration is unrealistic anyway in a 32-bit space, but there is no
  reason to help by distinguishing in the response
- **4.6 The confirmation is read after the writes.** If the response is not built from a re-read
  taken after the reclaim, the confirmation, and the renewal, a peer that was just deleted stays
  in `peers` and a pair that was just confirmed goes out as `ready: false`. The host reads that
  response and `confirm`s the same peer again on the next cycle, and that call does nothing
  because the conditional write fails. From the outside it looks normal, so the defect never
  shows
- **4.6 The condition of a confirmation is candidates on both sides.** Even if the host
  `confirm`s a peer with no candidates, nothing is written. Writing it would make 4.5 `get_peers`
  hand out ready together with an empty candidate list, and the peer would wait to the punch
  deadline with nowhere to send and end in `HOLE_PUNCH_TIMEOUT`. **That is a control plane
  failure disguised as a NAT failure**

---

## 5. State Transitions

### 5.1 Room

Room state is **derived from stored fields.** There is no separate state field.

> **Why.** With a separate state field, cases arise where the field and the state disagree.

| State | Definition |
|------|------|
| `open` | `ROOM` item exists and `now < expires_at` |
| `expired` | `ROOM` item exists and `now >= expires_at`. It appears in this state until TTL deletion |
| (none) | No `ROOM` item. It never existed, or TTL deleted it |

```text
(none) --create_room--> open --lease expires--> expired --TTL deletion--> (none)
                          ^                   |
                          +-- host_report ----+   (renewal works only before expiry)
```

**Ready is not a room state.** It is decided per pair (5.2), so a ready pair and a pair that is
not ready sit in the same room.

**A room lives on the host's lease.** One `host_report` pushes the expiry time to
`now + ROOM_LEASE_S` (4.6). There is no absolute cap. While the host signals, the room code is
valid, and a participant who arrives hours later still gets in with that code.

> **Why allow extension.** Because "when does the room disappear" can still be answered. The
> answer is **within `ROOM_LEASE_S` after the host stops signalling.** A fixed lifetime makes
> that answer simpler, but then a room whose host is dead stays alive for the remaining time and
> a live room dies because its time is up. The basis is
> [ADR 0009](decisions/0009-room-host-lease.md).

**The lease is not a liveness condition of the tunnel.** Even if the control plane dies and the
host fails to renew, an already established tunnel keeps going ([`spec.md`](spec.md) NFR-3). What
a room with a lapsed lease loses is **new joins and control plane operations.**

**State what the lease covers and what it does not.**

- The allowance for a player to receive the host's confirmation is not this value but the
  `get_peers` deadline of [`protocol.md`](protocol.md) section 11 Timers. The player has to see the
  host's confirmation within that deadline, counted from its own successful `register_candidate`.
  Beyond it, the run ends in `CONTROL_PLANE_EXCHANGE_FAILED` even though the room is still alive
  (9.6)
- The host has no such deadline. It does not call `get_peers` (8.4), and after setting up the room
  it stays in the room even with no players ([`concurrency.md`](concurrency.md) chapter 7 Lobby)
- A peer whose process died loses its slot. Coming back in is a new join and the virtual IP is
  assigned anew at that time (4.3). The room is alive, so the same room code is used again
- If the host dies, the room expires within `ROOM_LEASE_S`. A participant who joins in that window
  has no host to confirm the pair, never reaches ready, and ends at the polling deadline

**Reflect in the demo script that the two values have different roles.** A player that did not
receive the host's confirmation failing within a minute is not a control plane outage.

**Allowed states per operation.**

| Operation | `open` | `expired` | (none) |
|------|--------|-----------|--------|
| `create_room` | Same nonce returns the existing room (4.2) | Same nonce gives `room_expired` (4.2) | Create |
| `join_room` | Attempt claim. No free address gives `room_full` | `room_expired` | `room_not_found` |
| `register_candidate` | Store | `room_expired` | `room_not_found` |
| `get_peers` | Respond with the ready state of each pair | `room_expired` | `room_not_found` |
| `host_report` | Reclaim, confirm, renew | `room_expired`. No renewal | `room_not_found` |

**Expiry decision case table.** `now` is the server wall clock in milliseconds, `expires_at_ms` is
the stored value.

| `now` vs `expires_at_ms` | Verdict |
|--------------------------|------|
| `now < expires_at_ms` | Alive |
| `now == expires_at_ms` | **Expired.** The boundary is on the expired side. A room with `expires_in_s` of 0 cannot be joined |
| `now > expires_at_ms` | Expired |
| `expires_at_ms` field missing | **`internal`.** Do not turn "cannot decide" into pass. The creation path always writes this field, so if it is missing the store is corrupted |

`expires_in_s` is `max(0, ceil((expires_at_ms - now) / 1000))`. The client uses this value only
for display and not in any decision.

- **In a live room it is 1 or more.** Even 1ms left makes the ceiling 1
- 0 means expired. This says the same thing as the boundary row above

### 5.2 Ready

**The unit of ready is the pair.** The host and one player form a pair, and the pair is ready if
its `PAIR#` item exists (6.3).

**Only the host writes it.** The host names the pair with the `confirm` of `host_report`, and the
server creates it with a conditional write after confirming the candidates of both sides (4.6).

> **Why the host writes it.** One end of a pair is always the host (ADR 0006). If the peer starts
> punching while the host does not know, no packet leaves the host side and no hole opens. Putting
> the reference time at the moment the host knows makes both peers receive the same value.

The `ready_at_wall_ms` of a `PAIR#` **does not change once written until that pair disappears.**
It stays even if any peer registers candidates again. The basis is in 4.5 `get_peers` and 4.6
`host_report`.

**A pair disappears only when that peer is reclaimed** (2.5 Reclaim, 4.6). At that point the
`PEER#`, `VIP#`, and `PAIR#` items are deleted together.

### 5.3 Peer

| State | Definition |
|------|------|
| `joined` | `PEER#` item exists and `candidates` is empty |
| `registered` | `candidates` has 1 or more |

```text
(none) --create_room / join_room--> joined --register_candidate--> registered
   ^  ^                                  |                             |
   |  +-- server reclaim (grace passed) -+                             |
   +------------- the departed of host_report (slot reclaim) ----------+
```

**Two things delete a peer.** The host's report, and the server reclaim (4.6) of a participant
whose `JOIN_REGISTER_GRACE_S` has passed with no candidates. Otherwise peers disappear together with
the room.

> **Why.** The reason `leave_room` is not provided is at the head of section 4. The one who leaves
> does not know whether its session is closed on the host side too.

---

## 6. Storage

### 6.1 Storage Contracts

State lives in Amazon DynamoDB. **Why DynamoDB and what was rejected** is owned by
[ADR 0004](decisions/0004-state-store-dynamodb.md). There are five contracts.

| Contract | Content |
|------|------|
| Storage 1 | Reads whose values feed a decision set **`ConsistentRead=true` on the table.** Because of Storage 5, every place where an operation reads room or peer items falls under this, not only `get_peers`. Default reads are eventually consistent (ADR 0004 decision 1 quotes the AWS documentation), so a candidate registered a moment ago can be missing, and then punching ends in `HOLE_PUNCH_TIMEOUT` and is disguised as a NAT failure |
| Storage 2 | Strongly consistent reads work only on the table and LSIs. **No GSI is created.** This design has no query that needs a GSI |
| Storage 3 | Virtual IP assignment keys one item per address and **claims it with a conditional write (`attribute_not_exists`).** No atomic counter is used. A counter is not idempotent and a retry skips an address. On a failed claim, move to the next candidate address, and after one pass over the pool end with `room_full` (2.5). It does not loop forever |
| Storage 4 | Room expiry **is decided by the application** (5.1). TTL is a means of reclaiming storage space, and an item past its expiry time is still visible to queries until it is deleted |
| Storage 5 | The control server **holds no room or peer state in memory.** Every request reads from DynamoDB |

**Because of Storage 5, there is no "restart recovery" procedure.** There is no step at startup
that loads the room list, and the first request right after a restart reads exactly as usual.

- The room state persistence across restart that [`spec.md`](spec.md) NFR-3 requires holds by this
  contract, not by a recovery procedure
- A cache would recreate the same problem as eventually consistent reads. If a cache becomes
  necessary, redecide it together with Storage 1

**The exceptions to Storage 5 are the rate limit table (6.4) and the in-flight request count
(7.2).** Those two are not room or peer state; they are the server process's self-protection
state. They vanish on restart, and what is lost is only a brief moment of protection right after
the restart. No other in-memory state is created beyond these two.

### 6.2 Table

**One table.** The name is set by deployment configuration (7.6) and is not hard-coded.

| Item | Value |
|------|-----|
| Partition key | `pk` (string). The value is `room_id` |
| Sort key | `sk` (string). Item kind and identifier |
| TTL attribute | `ttl` (number, epoch seconds). 6.5 |
| Capacity mode | [ADR 0004](decisions/0004-state-store-dynamodb.md) decision 4. Provisioned. Values are in deployment configuration |
| Indexes | None (Storage 2) |

Items belonging to a room (`ROOM`, `PEER#`, `VIP#`, `PAIR#`) share the same partition key, so the
whole state of one room is read with a single `Query(pk = room_id, ConsistentRead=true)`. The item
count is at most `1 + MAX_PEERS × 2 + (MAX_PEERS - 1)` (one `ROOM`, a `PEER#` and a `VIP#` per
peer, and one `PAIR#` per host-and-player pair). In v1 that is at most 15.

**The `NONCE#` item has a different partition key.**

> **Why.** The idempotency lookup of `create_room` has to find it by nonce alone, at a point where
> the `room_id` is not yet known.

### 6.3 Items

| `sk` | Attributes | Meaning |
|------|------|-----|
| `ROOM` | `created_at_ms`, `expires_at_ms`, `host_peer_id`, `ttl` | Room. `expires_at_ms` is pushed by `host_report` (4.6) |
| `PAIR#<lo>-<hi>` | `ready_at_wall_ms`, `ready_at_mono_ns`, `ready_boot_id`, `punch_delay_ms`, `ttl` | Ready state of a pair. `lo` and `hi` are the two `peer_id`s written in decimal and joined in ascending order. 7.4 Clocks uses the three values |
| `PEER#<peer_id>` | `peer_id`, `peer_token`, `virtual_ip`, `candidates` (list), `client_nonce`, `joined_at_ms`, `ttl` | Peer. `peer_id` is written in decimal in `sk`. `peer_token` is the raw value (2.3) |
| `VIP#<virtual_ip>` | `peer_id`, `ttl` | Virtual IP claim marker. `<virtual_ip>` is dotted-decimal |
| (pk = `NONCE#<client_nonce>`, sk = `NONCE`) | `room_id`, `peer_id`, `ttl` | Idempotency key → room and peer mapping. **The pk is the nonce, not the room** |

Token comparison uses a constant-time comparison (`hmac.compare_digest`). A comparison inside a
condition expression (`peer_token = :t`) is done by DynamoDB, so constant time is not claimed for
it. Therefore an operation that needs the token has two steps.

- **First read `PEER#` and do the constant-time comparison in the application**
- The `peer_token = :t` in the condition expression serves to confirm that the value did not
  change between the read and the write

**All writes are conditional, and any place that writes several items together uses
`TransactWriteItems`.** If any one condition fails, all of them are cancelled. The only exception is
the `host_report` renewal (table below).

| Operation | Write | Condition |
|------|------|------|
| `create_room` | Transaction: `ROOM` put, `PEER#host` put, `VIP#10.100.0.1` put, `NONCE#` put | **All four puts carry `attribute_not_exists(pk)`.** A `ROOM` failure is a `room_id` collision → redraw (2.1). A `NONCE#` failure means the same nonce arrived concurrently, so read again and return that result. `PEER#` and `VIP#` cannot exist without `ROOM`, so a failure there means the store deleted only part of an earlier room's items. That is `internal` and there is no redraw |
| `join_room` new join | Per pool address, a transaction: `ROOM` **ConditionCheck**, `VIP#<ip>` put, `PEER#<peer_id>` put, `NONCE#` put | `attribute_exists(pk) AND expires_at_ms > :now` on `ROOM`. This blocks the race where the room expires or is deleted between the prior read and the write. `attribute_not_exists(pk)` on `VIP#`, `PEER#`, and `NONCE#` each. Failure handling is in the cancellation reason table below |
| `register_candidate` | `PEER#` update: `candidates = :list` | `attribute_exists(pk) AND peer_token = :t`. Failure is `unauthorized`. It is the case where that peer was reclaimed after the token check (4.6 server reclaim) |
| `host_report` reclaim | Per target, a transaction: `PEER#<target>` delete, `VIP#<that peer's virtual_ip>` delete, delete the `PAIR#` items that peer is part of | **`attribute_exists(pk)` on `PEER#` only.** If that condition fails, the target was already gone and is not put into `released` (4.6). The other two are unconditional deletes. A peer that left before confirmation has no `PAIR#` at all, and conditioning on it would cancel the whole reclaim |
| `host_report` server reclaim | Per target, a transaction: `PEER#<target>` delete, `VIP#<that peer's virtual_ip>` delete, `NONCE#<that peer's client_nonce>` delete | `attribute_exists(pk) AND (attribute_not_exists(candidates) OR size(candidates) = :zero) AND joined_at_ms <= :cutoff` on `PEER#`. `:cutoff` is `now - JOIN_REGISTER_GRACE_S * 1000`. If the condition fails, the peer registered in the meantime or was already gone, so it is not put into `released`. A peer that had no candidates cannot have a `PAIR#`, so none is deleted |
| `host_report` confirmation | Per pair, a transaction: `PEER#<host>` **ConditionCheck**, `PEER#<other>` **ConditionCheck**, `PAIR#<lo>-<hi>` put | Both condition checks are `attribute_exists(pk) AND size(candidates) > :zero`. The put is `attribute_not_exists(pk)`. **The both-sides-have-candidates rule is a storage condition.** The other peer can be reclaimed or have its candidates cleared between the read and the write. If only the put condition fails, the pair is already confirmed and that is not an error (4.6) |
| `host_report` renewal | First write the single `ROOM` update (`expires_at_ms = :new`, `ttl = :new_ttl`). If it succeeds, write a separate `ttl = :new_ttl` update for each other item of that room. Not bundled into a transaction | `attribute_exists(pk) AND expires_at_ms > :now` on `ROOM`. Failure is `room_expired`, and the rest is not written. **Each other item carries `attribute_exists(pk)`.** If the condition fails, the item was deleted in the meantime, so it is ignored. The item list is the first `Query` result of this request minus the items step 2 deleted |

**When a transaction is cancelled, read the cancellation reason per item and decide by the
priority below.**

The `CancellationReasons` of `TransactionCanceledException` come one per item, **in the same
position** as the items put into the transaction. So items are identified by position, and the
decision order is not by position but by the table below.

> **Why.** Several items can fail at once, so which one is looked at first determines the
> response.

| Order | Failed condition | Action |
|:----:|-------------|------|
| 1 | `NONCE#` | **Whatever the other reasons are,** a request with the same nonce was established first. Read `NONCE#` again, `Query` the room by its `room_id` and `peer_id`, and return the same response. If that room has expired, `room_expired` (4.2). Do not go to `room_full` or to a redraw. If the re-read `room_id` differs from the request's room, it is `bad_request`. The nonce was reused for a different room, and the answer has to match what the prior-read path gives for the same input |
| 2 | `ROOM` ConditionCheck (`join_room`) | Read the room again. If absent, `room_not_found`; if present, `room_expired`. Do not move to the next address |
| 3 | `VIP#` | Next address. When the pool is exhausted, `room_full` |
| 4 | `PEER#` | Redraw `peer_id`. The cap is `MAX_PEER_ID_ATTEMPTS` |
| 5 | `ROOM` put (`create_room`) | Redraw `room_id`. The cap is `MAX_ROOM_ID_ATTEMPTS` |
| - | A reason that is not a condition failure (`TransactionConflict`, throttling) | `internal`. The client retries by the rules of 8.3 Error Classification and Retry |

The reason `NONCE#` is placed at row 1 is in "Why It Was Decided This Way" below.

**Single-item updates (`register_candidate`, the confirmation of `host_report`) do not put room
liveness in the condition.** The renewal of `host_report` is the exception. The purpose of that
write is the room's expiry time, so room liveness is in its condition (the table above). Expiry is
decided first in the order of 7.3 Operation Processing Order and then the write happens, so the
only thing that slips through is the race where the room expires within the tens of milliseconds
between the read and the write. That race is accepted, and its result is stated precisely.

- The current request can receive `ok`. For `register_candidate`, `accepted` also comes back with
  a normal value
- The updated item sits inside an expired room, and every **subsequent** operation that reads that
  room ends in `room_expired` at the expiry check of step 6 of 7.3 Operation Processing Order. So
  the only response that carries the updated value is that one request
- The client receives `room_expired` on its next request (usually the `get_peers` poll) and ends
  with `CONTROL_PLANE_EXCHANGE_FAILED`
- Expiry is not rechecked right before the response. Even if it were, the same race exists between
  that check and the response

> **Why.** Only new join puts it in the condition, because that race would create new `VIP#` and
> `NONCE#` items inside an expired room and their `ttl` has to be derived from the room's value,
> and the room may already be gone.

**A failed condition still consumes write capacity** (ADR 0004 decision 2). So new join reads the
room before walking the pool to confirm expiry and existence first, and the pool size is itself
the attempt cap.

**Reads.**

| Operation | Reads |
|------|------|
| `create_room` | `NONCE#` GetItem (idempotency). If present, `Query` by that `room_id` and assemble the same response |
| `join_room` new join | `NONCE#` GetItem → if present, the stored `room_id` has to equal the request's (otherwise `bad_request`; the nonce was reused for a different room) → if absent, `ROOM` GetItem (expiry and existence) → transaction |
| `register_candidate` | `Query(pk)` for `ROOM` and all `PEER#` (expiry and token check) → `PEER#` update |
| `get_peers` | One `Query(pk)`. `ROOM` and all `PEER#` and `PAIR#` come back. The token check is also done on that result |
| `host_report` | `Query(pk)` for the expiry and caller decision → reclaim, confirmation, and renewal writes → **re-read with `Query(pk)`** to assemble the response (4.6). Why there are two reads is in that section |

All of them are `ConsistentRead=true` (Storage 1). **There is no form that finds a peer by token
alone.** Every authenticated operation also receives the `peer_id`, so it reads by that key
directly. Finding by token would require reading every peer and comparing.

### 6.4 Rate Limit

**A budget on failure responses per source IP.** What is counted is three: `room_not_found`,
`room_expired`, `unauthorized`. Success and `bad_request` are not counted.

> **Why.** `bad_request` is a format error unrelated to guessing, and counting success would catch
> normal polling.

| Item | Value |
|------|-----|
| Budget | `RATE_LIMIT_BUCKET` (10) tokens per source IP. One is spent each time one of the three errors above goes out |
| Refill | `RATE_LIMIT_REFILL_PER_MIN` (10) per minute. **It is continuous.** One token is added every 6 seconds from the last refill time, capped at `RATE_LIMIT_BUCKET` (10). For the reference time rule, see below |
| When exhausted | **The request is not processed** and `rate_limited` is returned. The store is not read. That way the limit also stops storage cost |
| Table | In memory. Source IP → (tokens, last refill time). Cap `MAX_RATE_ENTRIES` (4096) |
| Entry creation | **Create it only when a token is spent.** A lookup alone does not create one. A source with no entry is the same as a full bucket |
| Entry removal | **Delete the entry when refill brings the tokens to the cap.** A full entry decides the same as no entry, so there is no reason to hold it. Without a removal rule the table grows monotonically, and after 4096 entries are filled a new source is not limited until a restart |
| When the table is full | **New sources are passed through without limiting.** And the `rate_table_full` counter is incremented. Evicting the oldest entry would let an attacker with many IPs fill the table and evict the real guesser's entry. With pass-through, the attacker gains nothing by filling the table. The price is that new sources have no limit while the table is full, and if this value rises, the cap is revisited |
| When the budget is checked | **After** the request is parsed, before the store is read |

**The reference for refill is the time refill happened.** Neither spending a token nor a
`rate_limited` response changes that time.

- If spending moved the reference, refill for a source that keeps spending would be delayed
  forever
- If `rate_limited` moved it, an attacker could extend the cooldown permanently just by knocking,
  and block a legitimate user of the same source forever. This is the same reason
  [`protocol.md`](protocol.md) 9.5 Renegotiation put the same rule on the renegotiation cooldown

**10 tokens are not filled all at once every 60 seconds.** A batch refill stretches the empty
window to a full minute and blocks the one-off failure response of a legitimate client for a long
time.

**It slows, it does not stop. State how much slower.**

| What | Value |
|------|-----|
| The whole 30-bit space (2^30) from one IP | At 10 per minute, about **204 years**. `2^30 / 10 / 60 / 24 / 365` |
| Until one live room is hit | With `N` rooms open at that moment, the expected number of attempts is `2^30 / N`. At the "tens of open rooms" scale assumed by 2.1 `room_id` (`N` = 50), about **4 years** from one IP |
| With several IPs | It divides by that many. With 100 IPs it is 1/100 of the values above |

An expired room also reveals its existence through `room_expired` (4.1), so the target is not
only live rooms. **These numbers are the size of the limit, not the size of the protection.** The
last line of 1.2 Trust Assumptions is that limit.

**Case table.**

| Order | Input (same source) | Response | Tokens left |
|:----:|--------------------|------|:---------:|
| 1~10 | `join_room` on a nonexistent room, 10 times | `room_not_found` 10 times | 0 |
| 11 | `join_room` on a nonexistent room | **`rate_limited`.** No store read | 0 |
| 12 | Normal `get_peers` on an existing room | **`rate_limited`.** The budget is per source, so a normal request is blocked too | 0 |
| 13 | 6 seconds after the response of row 11, `join_room` on a nonexistent room | 1 token refilled and then spent. `room_not_found` | 0 |
| 14 | 70 seconds after the **refill time** of row 13 (no request in between) | Tokens reach the cap of 10 and **the entry is deleted.** The first request after that is handled as a source with no entry | - |
| - | `join_room` on a nonexistent room from another source | `room_not_found`. The table is per source | 9 for that source |
| - | Normal `get_peers` 100 times (success) | All processed | 10. Success is not counted |
| - | `bad_request` 100 times | All `bad_request` | 10. Not counted |

**Why row 14 is 70 seconds and not 60.** Reaching the cap of 10 takes exactly 60 seconds, so
writing 60 seconds as the expected value would split an implementation that stopped at 9 from one
that filled 10, depending on clock resolution and the moment of measurement. The boundary is not
used in a decision. To look at the boundary itself, put "it is not deleted just before 60 seconds"
in a separate row.

Row 12 means the following. **When two users behind the same NAT share a public IP, one person's
guessing blocks the other person's normal requests.** A campus network is like that.

- A normal client receives a failure response once or twice per launch at most, so a budget of 10
  covers that case. But if the person next door burns the budget, it is blocked. This is a limit
  v1 accepts
- The alternative (not counting requests that carry a token) leaves `unauthorized` guessing open,
  so it is not used

### 6.5 TTL

| Item | `ttl` value |
|------|----------|
| All items of a room (`ROOM`, `PEER#`, `VIP#`, `PAIR#`, `NONCE#`) | `floor(expires_at_ms / 1000) + STORAGE_GRACE_S` |

Items of the same room share the same `ttl`. One day after the room expires, DynamoDB deletes them.

**Renewing the lease pushes the `ttl` of every item of that room.** Renewing only `ROOM` would
delete the `PEER#`, `VIP#` and `PAIR#` items of a long-lived room first, leaving a live room with
no peers. The write is done by the renewal writes of 4.6 `host_report` (6.3).

- **An item created between two renewals takes its `ttl` from the `expires_at_ms` of that moment.**
  The next renewal pushes it along. The report interval is at most 30 seconds and
  `STORAGE_GRACE_S` is one day, so nothing is deleted in between

- **The one-day grace exists because expiry is decided by the application per Storage 4.** If items
  were deleted right after expiry, the distinction between `room_expired` and `room_not_found`
  (4.1) would waver depending on deletion timing. One day later it no longer matters which one is
  answered
- That the AWS documentation states the deletion delay only as "within a few days" and sets no
  upper bound is in [ADR 0004](decisions/0004-state-store-dynamodb.md) decision 3 and item 1 of
  "what was not confirmed"

### Why It Was Decided This Way

- **6.3 The `host_report` renewal is not bundled into a transaction.** An unconditional update
  creates a missing item, so it would revive a `PEER#` or `VIP#` deleted by the reclaim of the same
  request or by another overlapping request as an item holding only `ttl`. A revived `VIP#` blocks
  that address until the room ends. Putting conditions on them inside a transaction would cancel
  the whole lease renewal because of one item another request deleted. Writing them separately can
  leave the `ttl` of some items pushed one time fewer, but the next cycle pushes it again, and
  `STORAGE_GRACE_S` is one day, so they are not deleted in between
- **6.3 `NONCE#` is placed at cancellation reason row 1.** It is because of the retry scenario.
  When a participant who lost the response comes back with the same nonce and the first request
  already claimed `VIP#`, then `VIP#` and `NONCE#` fail together in the same transaction. If
  `VIP#` were looked at first, the server would move to the next address and return `room_full`,
  and that client would believe it failed to enter a room it is already in

---

## 7. Server Implementation

### 7.1 Modules

| Module | Responsibility | External dependency |
|------|------|-----------|
| `server.py` | Accept loop, 3.3 HTTP parsing and response serialization, operation dispatch, time limits, `MAX_INFLIGHT`, rate limit table (6.4) | `asyncio` |
| `ops.py` | The four operations of section 4. Request validation, state decisions (section 5), response assembly | None |
| `ids.py` | Generation and normalization of `room_id`, `peer_id`, `peer_token`. The 2.1 alphabet and case table attach here as tests | `secrets` |
| `candidates.py` | 4.4 hygiene. Candidate format checks and the 10.1 rules | `ipaddress` is not used. See below |
| `clock.py` | The three values of 7.4 Clocks | `time`, `/proc/sys/kernel/random/boot_id` |
| `store.py` | All `boto3` calls. The condition expressions of 6.3 Items live **only in this file** | `boto3` |

**Candidates are not judged with the `ipaddress` module.**

- On this repo's Python 3.11 version, the observed values are that
  `ip_address('127.0.0.1').is_private`
  and `ip_address('192.0.2.1').is_private` are true, and `ip_address('224.0.0.1').is_global` is
  also true. This can differ by version, so no version is allowed to delegate the decision to those
  predicates
- The decisions of the 4.4 case table parse the four octets directly and branch on the leading
  octet and range. Parsing is `^\d{1,3}(\.\d{1,3}){3}$` with each octet 0~255 and **leading zeros
  rejected**
- Using `ipaddress.IPv4Address` for format parsing only is allowed. The point is that its
  predicates are not used for the decision

`asyncio` and `json` are standard library. `boto3` has [`spec.md`](spec.md) NFR-5 approval. No web
framework is used (3.1).

### 7.2 One Pass of Request Handling

```text
on_connection(reader, writer):
    src = peer IP
    if inflight >= MAX_INFLIGHT:
        counters.unavailable += 1
        respond(503, unavailable); close; return          # does not touch the store
    inflight += 1
    try:
        req = await wait_for(read_request(reader), SERVER_READ_TIMEOUT_S)
                                                          # 3.3 case table. Failures end here as 4xx
        if rate.exhausted(src):                           # 6.4. After parsing, before the store
            counters.rate_limited += 1
            respond(429, rate_limited); return
        result = await to_thread(ops.dispatch, req)       # boto3 is blocking. Send it to a thread
        if result.error in {room_not_found, room_expired, unauthorized}:
            rate.spend(src)                               # the three that 6.4 Rate Limit counts
        respond(result)
    except TimeoutError:
        counters.http_read_timeout += 1; close            # no response (3.4)
    except Exception:
        counters.internal_error += 1
        respond(500, internal)                            # do not put the exception text in the body
    finally:
        inflight -= 1; close
```

**`boto3` calls are treated as blocking.** They are sent through `asyncio.to_thread`.

- Calling them directly on the event loop thread stalls accepting and parsing of other connections
  for the duration of the DynamoDB response delay
- The concurrency cap `MAX_INFLIGHT` (32) stops that thread pool from growing without bound and
  stops waiting requests from piling up and eating memory when the store slows down. Beyond the cap
  it is `unavailable`, and the client retries by the rules of 8.3 Error Classification and Retry

**One process, one event loop, one thread pool.** No multiprocessing.

> **Why.** With several processes, the rate limit table (6.4) would exist separately per process
> and the budget would multiply by the process count.

### 7.3 Operation Processing Order

This is the common order inside `ops.dispatch`. Every operation keeps this order. Changing the
order changes the responses in the case tables.

```text
1. Field presence and type check (section 4 tables)  -> failure: bad_request
2. room_id normalization (2.1)                       -> failure: bad_request
3. Per-operation field combination check (section 4 per-operation tables) -> failure: bad_request
4. Store read (6.3 reads table, ConsistentRead)
5. Room existence                                    -> absent: room_not_found
6. Room expiry (5.1 case table)                      -> expired: room_expired
7. Token check (only operations that need it)        -> failure: unauthorized
   host_report also checks that the caller is the host (4.6)
8. Operation body (conditional writes)
9. Response assembly
```

**Format checks come before the store read.** That way `bad_request` spends no storage cost, and
a guessing attack does not reach the store even though the rate limit (6.4) does not count format
errors.

**Expiry comes before the token.** A request with a wrong token on an expired room is
`room_expired`.

> **Why.** An expired room cannot be used anyway, so revealing whether the token matched loses
> nothing, and pinning the order is what makes the case tables deterministic.

### 7.4 Clocks

`elapsed_since_ready_ms` is **computed with a monotonic clock** ([`protocol.md`](protocol.md)
section 14 contract 4). So three values are stored together.

- Computed with the wall clock, the moment NTP steps the time the values the two peers receive
  diverge and the error bound of `protocol.md` 10.2 Rendezvous breaks
- Because of Storage 5 the server does not hold the ready time in memory, and monotonic clock
  values have a different base per boot

| Stored value | Source | Use |
|--------|------|------|
| `ready_at_mono_ns` | `time.monotonic_ns()`. **The premise is that in the deployment environment (Linux) this is the system-wide clock (`CLOCK_MONOTONIC`), so its base does not change across process restarts.** See below | Elapsed computation within the same boot |
| `ready_boot_id` | Content of `/proc/sys/kernel/random/boot_id`. **The premise is that this value changes on every boot** | Deciding whether the value above has the same base |
| `ready_at_wall_ms` | `time.time()` in milliseconds | Fallback when the boot changed, and the ready-existence decision of 5.1 Room |

```text
elapsed_since_ready_ms(room):
    if room.ready_boot_id == current_boot_id:
        d = (monotonic_ns() - room.ready_at_mono_ns) // 1_000_000
        source = "mono"
    else:
        d = wall_ms() - room.ready_at_wall_ms                # the instance rebooted
        source = "wall"
        counters.elapsed_wall_fallback += 1
    return max(0, d)
```

**Case table.**

| Situation | Computation | Result |
|------|------|------|
| Same boot, no process restart | Monotonic | Exact |
| Same boot, process restart | Monotonic. `CLOCK_MONOTONIC` is independent of the process | Exact |
| After an instance reboot | Wall clock fallback | Can be off by the NTP step. **In this case the error bound of `protocol.md` 10.2 Rendezvous is not guaranteed.** Section 14 contract 4 delegates that exception to this document. A reboot and a time adjustment have to overlap within the few seconds between ready and the polling response, so it is rare. It is observed by the counter |
| Negative in the wall clock fallback | `max(0, ...)` | 0. The client waits the full `punch_delay_ms` |
| No `boot_id` file (not Linux) | A random value drawn at process start is used as `boot_id` | Every process restart drops to the wall clock fallback. That is what happens in local tests. **The deployment target is Linux** |

**The two premises were confirmed on the deployment instance.** Two things were checked in the 7.6
deployment environment.

- The implementation of `time.get_clock_info('monotonic')` is `clock_gettime(CLOCK_MONOTONIC)` and
  `adjustable=False`
- `boot_id` was read before and after a reboot, and the two values differed

The development machine is Windows, so the same call returns `GetTickCount64()`. Local tests are the
last row of the case table above. If the deployment image changes, do the same two checks again. If
either premise turns out wrong, it drops to the wall clock fallback, so what is lost is accuracy,
not correctness.

### 7.5 Logs and Counters

One event per line on standard error. The shape is the same as the client log contract
([`architecture.md`](architecture.md) section 9 Telemetry and Records):
`<level> <event key> <name>=<value> ...`. The keys the Phase 3 verification looks for are the
following.

| Event key | Required fields | Which verification uses it |
|----------|-----------|--------------------|
| `http.request` | `op`, `status`, `error` (`-` on success), `src`, `ms` | Verification of every operation. **Does not carry `peer_token`, the body, or `room_id`.** If a room code lands in the log, reading the log is room hijacking |
| `room.created` | `peer_id`, `virtual_ip` | Virtual IP assignment verification. `room_id` is not carried |
| `pair.ready` | `host_peer_id`, `peer_id` (the other end of the pair), `punch_delay_ms` | Verification of the single ready write. It has to appear **exactly once** per pair. Splitting lines in a log with more than one room needs the two `peer_id`s |
| `peer.released` | `peer_id`, `virtual_ip`, `by` (`host` or `server`) | Slot reclaim verification (4.6). Reassignment is seen when the reclaimed address appears in `vip.claimed` again. `by` separates the host's report from server reclaim |
| `peer.reclaim_skipped` | `peer_id`, `reason` | A peer that server reclaim skipped because it could not decide (4.6 server reclaim case table). The level is `WARN` |
| `room.renewed` | `host_peer_id`, `expires_in_s` | Lease renewal verification (5.1). The line stopping after the host stops signalling is what is watched |
| `vip.claimed` | `virtual_ip`, `peer_id` | Verification of duplicate assignment under concurrent join |
| `counter` | `name`, `value` | The counters below |

The counters are `http_read_timeout`, `internal_error`, `rate_limited`, `rate_table_full`,
`elapsed_wall_fallback`, `unavailable`. All of them are emitted at shutdown and every 60 seconds.
Zero values are emitted too.

**Not carrying `room_id` in the log is stated again.** The `op` and `status` of `http.request` are
enough for the Phase 3 verification. Diagnostics that need to know which room use `peer_id`. A
`peer_id` has meaning only inside a room, so it alone cannot be used to join.

### 7.6 Configuration and Deployment

Configuration is environment variables. There is no file. There are only four values.

| Variable | Meaning | Default |
|------|-----|--------|
| `SANGTACHI_CP_PORT` | Bind port | `8000` |
| `SANGTACHI_CP_TABLE` | DynamoDB table name | None. **Required** |
| `AWS_REGION` | Region | None. Required. Standard `boto3` variable |
| `SANGTACHI_CP_ENDPOINT` | DynamoDB endpoint URL override. For local tests | None. If absent, the region default |

**There is no credentials variable.** On EC2 it is an IAM role
([ADR 0004](decisions/0004-state-store-dynamodb.md) decision 5), and local tests use DynamoDB
local, which needs no credentials.

> **Why.** If a variable that takes an access key existed, someone would use it.

Deployment is **one systemd service.** `Restart=always`. There is no restart recovery procedure
(6.1), so restarts are cheap. The security group opens inbound TCP 8000. The telemetry service's
port is outside this document's scope and is a [`roadmap.md`](roadmap.md) Phase 9 pre-start item.

**Deployment environment.** The values below were confirmed on the real instance and table. Actual
values such as the region, table name, public address, and account details are not kept in the
public repo. They are kept in `deploy/aws.local.md` inside the repo, and `.gitignore` blocks it.

| Item | Value | How it was confirmed |
|------|-----|----------------|
| OS | Ubuntu 26.04 LTS, x86_64 | `/etc/os-release`, `uname -m` on the instance |
| Instance type | t3.micro | Console |
| Python | 3.14 (system). The server runs with `boto3` installed in a venv | `python3 --version` |
| systemd | Present | `systemctl --version` |
| Credentials | IAM role. There is no credentials file on the instance | `aws sts get-caller-identity` gives `assumed-role`; no `~/.aws/credentials` |
| Security group inbound | TCP 8000 allowed from anywhere | Console |
| Public address | Elastic IP. The client is given an IPv4 literal. No DNS name is used | Console. [`windows-prereq.md`](windows-prereq.md) section 10 |
| Table | The two keys of 6.2 (`pk`, `sk`), TTL attribute `ttl`, no index | Console |
| Capacity | provisioned, RCU 25, WCU 25 | Console. "Capacity" below |

**The IAM role needs six actions.** The resource is narrowed to that one table.

| Action | Where it is used |
|------|-----------|
| `dynamodb:GetItem` | Prior read of `NONCE#` and `ROOM` (6.3 reads) |
| `dynamodb:Query` | Reading the whole room (6.3 reads) |
| `dynamodb:PutItem`, `dynamodb:UpdateItem`, `dynamodb:DeleteItem` | Single writes and the same actions inside transactions |
| `dynamodb:ConditionCheckItem` | ConditionCheck inside transactions (the `join_room` and `host_report` confirmation of 6.3) |

> **Why list `ConditionCheckItem` separately.** Put, Update, and Delete inside a transaction are
> allowed by the permission of the single action of the same name, but ConditionCheck needs this
> permission separately (the transactions IAM section of the AWS DynamoDB Developer Guide). Without
> it, joins and pair confirmations end in a permission error.

**Capacity.** RCU 25 and WCU 25 are the DynamoDB Always Free limit. This limit is per region and per
payer account, and applies when the table class is Standard. It was confirmed that the account's
Free Tier page shows it as "Always Free monthly allowance".

The load is the worst case under the assumption that each item is 1KB or less. A write uses 1 WCU
per item, and a strongly consistent `Query` uses RCU equal to the total bytes read rounded up in
4KB units (the read/write units section of the AWS DynamoDB Developer Guide). One room has at most
15 items per 6.2.

| Load | Worst case |
|------|--------|
| `host_report` renewal writes of a room below capacity | With three players, 12 items every 5 seconds. 2.4 WCU/s |
| Renewal writes of a full room | 15 items every 30 seconds. 0.5 WCU/s |
| Reads of `host_report` | Two `Query`s per call (4.6). If the room has just become full it is 15KB, so 4 RCU per `Query` and 8 RCU per call. The interval is set by the previous response, so that call can come 5 seconds later, and the periodic calls alone give a worst case of 1.6 RCU/s. The immediate call when a session ends adds 8 RCU per call |
| One player polling with `get_peers` | One `Query` every 0.5 seconds. With 15 items it is 15KB, so 4 RCU, 8 RCU/s. It runs only until ready |

One room fits within 25 for writes. For reads, during the few seconds when four players poll at the
same time, polling alone is a worst case of 32 RCU/s, and the `host_report` reads on top of that go
over 25. The excess can be absorbed by burst capacity, where DynamoDB keeps up to 300 seconds of
unused capacity. It is not guaranteed, though. The same guide says that reserve can be used for
background work without notice, so throttling can happen. Then the client gets `internal` (the 6.3
cancellation reason table) and retries per 8.3 or waits for the next poll. Real items are expected
to be much smaller than 1KB so that a whole room fits within 4KB, but this was not measured. If
several rooms are joining at the same moment it can go over. When the actual consumed capacity is
checked is owned by `roadmap.md`.

**The unit file and deployment commands are not kept here.** This follows the rule that procedures
not yet executed do not go into documents. After the real deployment in Phase 3, the procedure goes
into a tool and the document points to it.

**Limits of local testing.** In DynamoDB local, reads usually look like the latest value, so a
missing `ConsistentRead` does not show, and `TransactionConflictException` does not occur either.

- Both are stated with AWS documentation quotes in the "what it costs" section of ADR 0004
- Storage 1 is judged by reading the code, and the transaction conflict path is confirmed on a real
  table

---

## 8. Client-Side Contract

### 8.1 Owning Thread

Control plane TCP calls and DNS resolution are done by the **`[control]` thread.** Thread
composition, the queue with `[loop]`, and shutdown order are owned by
[`concurrency.md`](concurrency.md) chapter 8 The `[control]` Thread. Here only which rules of this
document that thread keeps is stated.

> **Why.** The reason it is not put in `[loop]` is one line. If the control server does not
> respond, a TCP `connect` with no connection time limit blocks for the length of the OS default
> retry, and during that time `HELLO` retransmission and keepalive stop. That is a
> [`spec.md`](spec.md) NFR-3 violation.

### 8.2 Time Limits

| Stage | Value | If exceeded |
|------|-----|--------|
| DNS resolution (`getaddrinfo`) | OS default. **No separate limit is set.** This design does not use a means of putting a time limit on a synchronous call | `[control]` blocks for that long. `[loop]` is unaffected |
| `connect` | `CLIENT_CONNECT_TIMEOUT_S` (3). Non-blocking `connect` + `select` | Transport error |
| Send, receive, each | `CLIENT_IO_TIMEOUT_S` (3). `SO_SNDTIMEO` / `SO_RCVTIMEO`. The same options as the telemetry socket in [`concurrency.md`](concurrency.md) chapter 7 Shutdown and the same limit (they are per call, so the sum can be longer) | Transport error |
| Cap on one request | The sum of the three above. **At most 9 seconds, excluding DNS** | - |

**DNS resolution is done once at launch.** `[control]` keeps the resulting IPv4 address and uses
it for every request after that. Resolving on every poll would let a DNS outage stop polling. The
premise that the address does not change during a launch comes from the Elastic IP, and its basis
and price are owned by [`windows-prereq.md`](windows-prereq.md) section 10.

**If resolution fails, launch fails. No retry.** This is not an [`spec.md`](spec.md) FR-13 failure
code. The connection attempt never started, so it ends with one `ERROR` line and a nonzero exit
code, and because there is no session no line appears in the local record file either
(`architecture.md` section 9 Telemetry and Records).

**A server that does not respond after the name resolved is different.** That is a request failure
and it is `CONTROL_PLANE_EXCHANGE_FAILED` by the rules of 8.3 Error Classification and Retry. The
boundary is "was an address to send the request to obtained".

**A new connection is opened per request** (3.3). No reuse.

### 8.3 Error Classification and Retry

The result `[control]` returns to `[loop]` is one of three.

| Result | What | Example |
|------|------|-----|
| Success | An `ok: true` response | Operation result |
| Definite error | `ok: false` and `error` is not a transient error | `room_not_found`, `room_full`, `unauthorized`, `bad_request`, `rate_limited` |
| Transient error | Transport error (3.5), `internal`, `unavailable` | connect timeout, 500, 503 |

**The one that decides on a retry is `[loop]`.** `[control]` only sends one request and puts the
result in the response queue ([`concurrency.md`](concurrency.md) chapter 8 The `[control]` Thread).
The retry count and the 1-second interval timer are held by `[loop]` next to the session state,
and on a retry it puts the same request back into the request queue. That is how `[control]` keeps
that section's rule of "the only state is the resolved server address".

**Retry applies only to transient errors, and the rule differs per operation.**

| Operation | On transient error |
|------|--------------|
| `create_room`, `join_room` new join | **With the same `client_nonce`,** at 1-second intervals, **3 times in total including the first attempt**. Beyond that, `CONTROL_PLANE_EXCHANGE_FAILED` |
| `register_candidate` | At 1-second intervals, 3 times in total including the first attempt. Replace semantics (4.4) make retry safe |
| `get_peers` | **No separate retry rule.** The next poll is the retry. The polling interval and deadline are in [`protocol.md`](protocol.md) section 11 Timers |
| `host_report` | **No separate retry rule.** The next cycle is the retry. It is idempotent (4.6), so the unreported `departed` is carried again and sent (4.6). Even if transient errors continue and the lease lapses, the tunnel is kept (5.1). Definite errors also follow the error table of 4.6, not the general rule below |

A definite error is not retried and is immediately `CONTROL_PLANE_EXCHANGE_FAILED`.
**`rate_limited` is a definite error too.** A person restarts it. Only `host_report` is the
exception and follows the error table of 4.6.

> **Why.** If the client waited and retried on its own, several normal clients behind the same NAT
> would back off at the same time, rush back at the same time, and get caught again.

The list of conditions that end in `CONTROL_PLANE_EXCHANGE_FAILED` is owned by `protocol.md` 9.6
Failure Transitions. This section expands what "control plane error" among those conditions
means.

### 8.4 From Launch to Punch

This is the order in which the client calls the operations of this document. The timer values are
all from [`protocol.md`](protocol.md) section 11 Timers; only the order is fixed here.

```text
1. Take whether to create or join a room. It is the CLI role or a lobby command (architecture.md 3.5).
   Host: create_room; player: join_room
2. [control] resolves DNS once. On failure, launch fails
3. UDP socket bind, getsockname (protocol.md section 6)
4. create_room or join_room. Print the room_id from the response to the console (the host passes it to the peer)
5. STUN (protocol.md section 13). Query both servers
6. register_candidate. Local candidates + reflexive candidates. If more than 8, the client trims first
7. Player: poll get_peers. Until that pair has ready: true and the peer has 1 or more candidates (4.5).
   **The polling interval and deadline are counted from the moment the success response of step 6
   arrives** (protocol.md section 11)
   Host: call host_report periodically. When the response peers shows a new peer that has
   candidates, put that peer_id into the confirm of the next call (4.6). It does not call get_peers
8. Apply 10.1 hygiene again to the received candidates. If own peer_id appears, CONTROL_PLANE_EXCHANGE_FAILED
9. Punch after max(0, punch_delay_ms - elapsed_since_ready_ms) (protocol.md 10.2)
10. When a session ends, the host sends one host_report right away and puts that peer_id into departed (4.6)
```

**Steps 2 and 3 happen once per process.** They are done at startup even with no role argument.
When a new attempt starts from the lobby, it takes the command of step 1 and goes again from step 4.
The STUN of step 5 also runs again. Whether to skip it is left undecided by
[`concurrency.md`](concurrency.md) chapter 7 Lobby.

**Step 4 comes before step 5.** The protocol still holds if the order is swapped, but the person
waits longer.

> **Why.** STUN has a 5-second deadline and the room code has to be passed by a person. Creating
> the room first, printing the code, and running STUN in the meantime overlaps the time the person
> waits.

**Step 7 is the only place that splits by role.** The host and the player receive the same
information through different operations. The data path does not split
([ADR 0006](decisions/0006-star-topology-no-relay.md) decision 6).

**The host starts `host_report` right after step 4.** The room stays alive only if the lease is
renewed (5.1). The cycle runs even when there is no pair to confirm.

---

## 9. Verification

The Phase 3 verification items are owned by [`roadmap.md`](roadmap.md). This document owns **the
case tables those items run.**

- The case tables sit in 2.1 `room_id`, 3.3 HTTP Subset, 4.4 `register_candidate` (candidate
  hygiene), 4.5 `get_peers` (the field set of an element), 4.6 `host_report` (the reclaim and
  confirmation race), 5.1 Room, 6.4 Rate Limit, and 7.4 Clocks
- At Phase 3 start they move to `control-server/tests/` so the tests read the tables directly.
  After the move, this document points to those files instead of the tables. There will not be one
  set of tables in the document and another in the tests

**Every case table gets a mutation test.** Check which row of the table an implementation with one
rule line deleted `FAIL`s on. If a mutation drops no row, that rule does not protect anything yet.

**What can be judged only by reading code.**

| What | Why a behavioral test cannot do it |
|------|----------------------------|
| Storage 1 `ConsistentRead=true` | Eventually consistent reads also usually return the latest value. Local even more so |
| Token comparison is constant time and done first in the application (6.3) | Timing differences are hard to reproduce in a test |
| No `ipaddress` predicates in decisions (7.1) | A case table row can pass by accident |
| `room_id` and `peer_token` are absent from logs (7.5) | Scanning all the logs is not a proof that "it appears on no path" |
| Whether the response of `host_report` is read **after** the writes (4.6) | The read and write order of two requests cannot be forced from outside. Sending them at the same time usually ends up sequential and the defect does not show. Running the last row of the 4.6 case table needs an **injection point in the store layer that adds a delay right after the write**, and whether that injection point exists is also judged by reading code |

---

## 10. Undecided

| What | Where and when |
|------|-------------|
| Telemetry service port, schema, authentication | Outside this document's scope. [`roadmap.md`](roadmap.md) Phase 9 |
| Whether the two services share one table, whether IAM is split | [ADR 0004](decisions/0004-state-store-dynamodb.md) deferred it to Phase 9 |
| Automatic retry and re-polling by the surviving side | [`protocol.md`](protocol.md) 10.4 open item. There is no rejoin (4.3), so that procedure has to stand on a new join |
| TLS, caller authentication | Stretch. 1.2 |
| Whether `ROOM_LEASE_S` (120) and the two signalling intervals are right | After the Phase 8 demo. Judged by how long a room takes to disappear after the host dies, and by how long a late participant waits |
