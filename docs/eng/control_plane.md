# Control Plane

**Project:** Direct-First P2P Virtual Network for Multiplayer Games
**Requirements:** [`spec.md`](spec.md) FR-4, FR-5, NFR-3, NFR-10, C-3 / **Design:** [`architecture.md`](architecture.md) 3.2, 3.3 / **Protocol contract:** [`protocol.md`](protocol.md) section 14 / **Schedule:** [`roadmap.md`](roadmap.md) Phase 3

> Korean version: [`../kor/control_plane.md`](../kor/control_plane.md)

---

## 0. Where This Document Stands

**This is the single source for the control plane.** Operations, request/response encoding, errors, identifiers, room and peer state transitions, DynamoDB table design, server implementation structure, and the client-side call contract are all fixed here. [`architecture.md`](architecture.md) 3.3 and section 6 are summaries that point to this document, and [`protocol.md`](protocol.md) section 14 is a list of **what the tunnel protocol requires from the control plane.** That document owns the requirements; this document owns how they are met.

| What conflicts | Wins |
|-------------|------|
| Control plane wire encoding, error codes, state transitions, table design | This document |
| Tunnel wire format, session state, protocol timers (including the `get_peers` polling interval and deadline) | [`protocol.md`](protocol.md) |
| Client threads and loop structure | [`architecture.md`](architecture.md) 3.2 |
| Requirements and success criteria | [`spec.md`](spec.md) |

If the implementation meets a value that is not in this document, do not decide it in code. Fix this document first.

---

## 1. Scope and Trust Assumptions

### 1.1 What It Does and Does Not Do

| Does | Does not |
|---------|--------------|
| Room creation and joining | Relay game traffic. It is not on the data path (NFR-1) |
| Virtual IP assignment | Collect metrics. The telemetry service does that ([`architecture.md`](architecture.md) 3.4, [ADR 0003](decisions/0003-telemetry-service-split.md)) |
| Store and deliver candidate endpoints | **Open a UDP socket.** The EC2-side UDP probe sender used to measure NAT mapping lifetime is not a function of the control server. It is a separate tool. Its ownership and procedure belong to the [`roadmap.md`](roadmap.md) Phase 4 pre-start items |
| Provide the rendezvous reference point (`punch_delay_ms`, `elapsed_since_ready_ms`) | Judge NAT type or hole punching results. The client does that |
| Return the same identifiers to a peer that rejoins | Start session retries. Who retries and when is undecided, and [`protocol.md`](protocol.md) 10.4 says so |

### 1.2 Trust Assumptions

**The v1 control plane path is plaintext and has no caller authentication.** This is the same kind of explicit choice that [`protocol.md`](protocol.md) section 2 made for the tunnel path. The final report has to state this limit next to the tunnel-side limit.

| Assumption | Content | Consequence |
|------|------|------|
| The server is trusted | The client uses the peer candidates and virtual IP returned by the server without verification. Candidate hygiene ([`protocol.md`](protocol.md) 10.1) is applied separately by the client, but that is a format check, not an authenticity check | A malicious server can insert arbitrary candidates and use the client as a reflector that sends packets to a third party every 200ms. What 10.1 blocks is **amplification** paths such as broadcast and multicast |
| `room_id` is the only secret | Joining a room needs only a `room_id`. The host passes it to the participant over another channel | Any caller who knows the `room_id` can join as the second peer, receive candidates, and become the game peer. This is **room hijacking.** The real participant then receives `room_full` |
| An on-path observer exists | The path is plaintext TCP, so an on-path observer reads `room_id`, `peer_token`, and both sides' local and public endpoints | Room hijacking as above, plus **internal network address exposure.** Local candidates are private IPs and ports |
| `peer_token` is only proof of rejoin | A secret issued at join time. It proves that this is the same peer so that the same `peer_id` and virtual IP are returned | On a plaintext path it leaks the same way `room_id` does. **It is not a defense against an on-path observer.** What it blocks is a third party who knows only the `room_id` **impersonating** an already joined peer and changing that peer's candidates |
| The caller IP is treated as an address that can be answered on that TCP connection | It is the source of a connection that completed the handshake. A NAT or proxy may sit behind it so that many users appear as one address, and conversely one caller may use many addresses | The per-source rate limit in 6.4 stands on this assumption. Against a caller with many addresses the effect shrinks accordingly, and legitimate users who share an address get caught by someone else's exhausted budget (6.4 case table row 12) |

**These are limits that v1 accepts.** Authentication and TLS are [`spec.md`](spec.md) stretch goals. When they are introduced, the "Consequence" column of the table above is the criterion for what is lost. The small size of `room_id` (2.1) is an extension of the same table.

### 1.3 Mapping to the protocol.md Section 14 Contracts

This is an index of where the [`protocol.md`](protocol.md) section 14 contracts are met. That document owns the contract text, and it is not copied here.

| Section 14 contract | Where it is met |
|-----------|---------------|
| 1. `peer_id` unique within a room | 2.2, 6.3 (conditional write of the `PEER#` item) |
| 2. `not_ready` before both sides are registered | 4.5, 5.2 |
| 3. `punch_delay_ms` fixed once per room | 4.4, 6.3 (`attribute_not_exists(ready_at_wall_ms)`) |
| 4. `elapsed_since_ready_ms` included, monotonic clock | 4.5, 7.4 |
| 5. Store and deliver only what passes candidate hygiene | 4.4 hygiene case table |
| 6. Deliver the peer's virtual IP | 4.5 |
| 7. Rejoin returns the same `peer_id` and virtual IP | 4.3 (rejoin form), 5.3 |
| 8. `peer_id` is CSPRNG 32-bit | 2.2 |

---

## 2. Identifiers and Constants

### 2.1 room_id

**6 characters, 32-symbol alphabet, CSPRNG.** The alphabet is the following.

```text
ABCDEFGHJKLMNPQRSTUVWXYZ23456789      (32 symbols. I, O, 0, 1 are removed)
```

- Entropy is 32^6 = 2^30. **About one billion values, a size that can be guessed online.** The length is 6 because a person types the value into a console, and the price for that is the rate limit in 6.4. Without the rate limit, the only thing that bounds the number of requests needed to find an open room is server throughput. Even with the limit, **it slows guessing, it does not stop it.** This belongs in the limits table of 1.2
- `I`, `O`, `0`, `1` are removed because people confuse them when reading and copying. A character outside the alphabet is an error. **Do not reinterpret it as the look-alike character.** If `0` were corrected to `O`, two inputs would point to the same room and shrinking the alphabet would have no meaning
- Input is **case-insensitive.** The server upper-cases the received value and then checks it against the alphabet. Lowercase `a` is `A`. This is not a look-alike substitution; it is a spelling difference of the same character
- At creation, the server catches collisions with the conditional write of the `ROOM` item (6.3) and redraws on collision. The attempt cap is `MAX_ROOM_ID_ATTEMPTS` (4). Beyond that, it is an `internal` error. At a scale of tens of open rooms, a collision in a 30-bit space effectively does not happen; the cap exists to stop a storage error from turning into infinite retries

**Case table.** Normalization and validation have to pass this table. Phase 3 tests run this table as is.

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

**CSPRNG 32-bit, 0 excluded, unique within a room.** The wire header fixes `peer_id` at 4 bytes ([`protocol.md`](protocol.md) 4.2) and the server issues this value. Draw with `secrets.randbits(32)` and redraw if it is 0. Uniqueness within a room is guaranteed by the conditional write of the `PEER#<peer_id>` item (6.3). On collision, redraw; the attempt cap is `MAX_PEER_ID_ATTEMPTS` (4).

Sequential assignment is not used because the threat premise in [`protocol.md`](protocol.md) section 2 is "knows or guesses the `peer_id`". With sequential assignment the guess becomes trivial and that document's forged `HELLO` attack works from off-path. A 32-bit random value is a **supporting measure** that makes that guess harder. A forged `HELLO` arrives at the client directly over UDP, so the server rate limit (6.4) cannot slow that guessing, and this document does not compute an upper bound on the cost of guessing. **It hides nothing from an on-path observer.** It is plaintext.

### 2.3 peer_token

**128-bit CSPRNG, 32 lowercase hex characters.** `secrets.token_hex(16)`. It is carried in the join (or room creation) response and then included in every request from that peer. **The server stores the raw value** (6.3). If only a hash were stored, a client that lost the response and came back with the same `client_nonce` (2.4, 4.2, 4.3) could not be given the same token. The price is that a token leaks if the store is read, but in v1, where the path is plaintext and `room_id` is the only secret, that price is of the same kind as one already being paid (1.2). Revisit this line when authentication is introduced.

The token is needed for two reasons. (1) To return the same `peer_id` and virtual IP on rejoin, there has to be a basis for deciding "same peer". (2) If `register_candidate` worked with only `room_id` and `peer_id`, a third party who knows the `room_id` could overwrite the other peer's candidates and make that peer send its `HELLO` to an arbitrary address.

### 2.4 client_nonce

**Client-generated 128-bit CSPRNG, 32 lowercase hex characters.** Carried in `create_room` and `join_room` requests. It is an idempotency key. It stops a request that was retried after a lost response from taking the second peer slot again (4.2, 4.3). The client uses **one value per launch** and does not change it between retries. Changing it destroys idempotency.

### 2.5 Virtual IP Pool

| Item | Value |
|------|-----|
| Range | `10.100.0.0/24` ([`spec.md`](spec.md) FR-4) |
| Host (room creator) | `10.100.0.1`. Fixed at room creation |
| Participant pool | From `10.100.0.2` to `10.100.0.(MAX_PEERS)`. `MAX_PEERS` is 2, so **the v1 pool is the single address `10.100.0.2`** |
| Assignment order | Walk the pool from the lowest address and claim the `VIP#<ip>` item with a conditional write (6.3). On failure, move to the next address. When the pool is exhausted, `room_full` |
| Attempt cap | The pool size. One pass over the pool and it ends. It does not loop forever |
| Reclaim | When the room expires, the items are deleted by TTL (6.5). **No reclaim while the room is alive.** If a peer leaves, the slot is still that peer's. Rejoin (4.3) has to return the same address |

Raising `MAX_PEERS` grows the pool by that much and nothing else changes. Three or more participants is a [`spec.md`](spec.md) stretch goal.

### 2.6 Constants

```python
CONTROL_PORT              = 8000        # TCP. windows-prereq.md section 6 references this value
MAX_PEERS                 = 2           # protocol.md section 1. Two peers per room
ROOM_TTL_S                = 3600        # From creation to expiry. No extension (5.1)
STORAGE_GRACE_S           = 86400       # Grace from expiry to DynamoDB TTL deletion (6.5)
PUNCH_DELAY_MS            = 1000        # protocol.md 10.2. Written to the room once at ready
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

`MAX_CANDIDATES` and `PUNCH_DELAY_MS` are copies of values owned by [`protocol.md`](protocol.md). **When that document changes, fix them here to follow.** These two are the only copies allowed because the server code has to have the values, and the server code does not read C++ headers.

---

## 3. Transport and Encoding

### 3.1 Why an HTTP Subset

The transport is an **HTTP/1.1 subset over TCP** and the body is JSON. Three options were weighed.

| Option | Advantage | Disadvantage |
|----|------|------|
| **HTTP/1.1 subset (chosen)** | Can be tested with `curl`. Existing procedures such as server-side `ss`, security groups, and `Test-NetConnection` ([`windows-prereq.md`](windows-prereq.md) section 6) fit as they are | Both sides have to be hand-written. The subset is pinned narrow to reduce that cost |
| Line-delimited JSON over TCP | Simplest implementation | Cannot be tested with standard tools. One more test tool would have to be built |
| Web framework | No parsing to write | Needs [`spec.md`](spec.md) NFR-5 approval, and the client-side cost stays the same |

The subset is all of 3.3 below. **HTTP features not listed there are not supported, and are rejected when received.** That is better than pretending to support them and failing silently.

### 3.2 Address

| Item | Value |
|------|-----|
| Protocol | TCP, IPv4 |
| Server bind | `0.0.0.0:8000`. How to verify is in [`windows-prereq.md`](windows-prereq.md) section 6 |
| Address the client receives | **A DNS name or an IPv4 literal.** Received as a launch input ([`architecture.md`](architecture.md) 3.5). The DNS name is an A record pointing at an Elastic IP. Why an Elastic IP is needed is in [`windows-prereq.md`](windows-prereq.md) section 10 |
| Path prefix | `/v1/`. If the encoding changes incompatibly, open `/v2/`. The same server can serve both prefixes at once |

DNS resolution is done by the client's `[control]` thread ([`architecture.md`](architecture.md) 3.2.8). If resolution returns several results, use the first IPv4 address and ignore the rest. `AAAA` is not used. This is the same scope as the tunnel being IPv4 only ([`protocol.md`](protocol.md) section 1).

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
| Headers | Names are case-insensitive. Headers not in the table above are ignored. If a header appears twice, reject. **A duplicate `Content-Length` is rejected in particular.** Rejecting a message with several `Content-Length` values that differ is a rule of RFC 9112 6.3, and an upstream and downstream intermediary choosing different values is one form of request smuggling. Reject even if the values are equal. There is no reason to distinguish |
| Body | One UTF-8 JSON **object**. Arrays and scalars are rejected. Unknown keys are ignored (forward compatibility) |
| Size | Request line plus headers total `MAX_HEADER_BYTES` (2048), body `MAX_BODY_BYTES` (4096) |
| Encoding | Strings are UTF-8. Integers are JSON numbers with no decimal point. `uint32` fields are 0 or more and 4294967295 or less |
| Compression, chunking, 100-continue, upgrade, authentication headers | None |

**Server parsing case table.** The decision function has to pass this table. For each row, inject one mutation and check that the row `FAIL`s.

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
| `HTTP/1.0` | `400` `bad_request`. Only 1.1 is accepted |
| Normal request with unknown header `X-Foo: bar` | Operation result. Ignored |
| `content-length: 17` (lowercase) | Operation result. Names are case-insensitive |

The body of `400`-class errors is also the error envelope of 4.1. **A parse failure response does not echo the request content.** Otherwise the server becomes a place that reflects arbitrary bytes.

### 3.4 Server-Side Time Limits

From accepting the connection to reading one whole request is `SERVER_READ_TIMEOUT_S` (5 seconds). Beyond that, close without a response and increment `http_read_timeout`. This stops a caller who holds a connection open and trickles bytes from occupying a server connection slot. The same 5 seconds applies to sending the response.

### 3.5 Checks the Client Makes

The client looks at only the following in a response. Everything else is ignored.

1. Does the status line start with `HTTP/1.1 <3-digit number>`? If not, it is a transport error that leads to `CONTROL_PLANE_EXCHANGE_FAILED` (8.3)
2. One `Content-Length` header. If missing or larger than `MAX_BODY_BYTES`, transport error
3. Read the body to that length and parse it as a JSON object. On failure, transport error
4. Split success and error by the `ok` field (4.1). **Not by the HTTP status code.** The status code is for the person holding `curl`; the program looks at the body. An implementation where the two disagree has to be caught in tests, but if the client looked at both, which one to trust when they disagree would differ per implementation

---

## 4. Operations

There are four operations. `create_room`, `join_room`, `register_candidate`, `get_peers`.

**There is no `register_peer`.** The earlier design had that operation for reconnection. Rejoin is now one form of `join_room` (4.3), so a separate operation has nothing to do. There is one fewer operation, and the client does not have to decide "when is it `join_room` and when is it `register_peer`".

### 4.1 Common Envelope

**Success.**

```json
{"ok": true, ...operation-specific fields}
```

**Error.**

```json
{"ok": false, "error": "<error code>", "message": "<one line for humans>"}
```

`error` is one of the strings in the table below and is what the program reads. `message` is for humans and its content is not pinned. **Do not put the request body or tokens in `message`.**

| Error code | HTTP | Meaning | Client action |
|-----------|------|-----|-----------------|
| `bad_request` | 400 | Required field missing, format violation, 2.1 alphabet violation | Do not retry. `CONTROL_PLANE_EXCHANGE_FAILED` |
| `method_not_allowed` | 405 | Not `POST` | Same |
| `unknown_op` | 404 | Path is not an operation from section 4 | Same |
| `length_required` | 411 | No `Content-Length` | Same |
| `too_large` | 413 / 431 | Body or header cap exceeded | Same |
| `room_not_found` | 404 | No room with that `room_id` | Same. **An expired room is not this code either.** See `room_expired` below |
| `room_expired` | 410 | The room exists but its expiry time has passed (5.1) | Same |
| `room_full` | 409 | The participant pool is not empty (2.5) | Same |
| `unauthorized` | 403 | `peer_token` does not belong to that `peer_id` | Same |
| `rate_limited` | 429 | Per-source budget of 6.4 is exhausted | Same. The client does not wait and retry on its own. A person restarts it |
| `internal` | 500 | Storage error, redraw cap reached | **Transient error.** Follow the retry rules of 8.3 |
| `unavailable` | 503 | `MAX_INFLIGHT` exceeded (7.2) | Same. Transient error |

**`bad_request`, `rate_limited`, `internal`, and `unavailable` can occur on any operation.** Parsing, rate limiting, and storage errors happen in the common stages before and after the operation (7.2, 7.3). The per-operation "Errors" lines below list **only what is specific to that operation** and do not repeat these four.

**Why `room_not_found` and `room_expired` are separate.** A person who typed the room code wrong and a person who typed it right but whose host created it an hour ago need different actions. If merged, neither can be told apart. The trade is that **a caller guessing at nonexistent rooms learns that an expired room exists.** An expired room cannot be joined, so revealing it loses nothing.

### 4.2 create_room

Called by the host. Creates the room, registers the host as the first peer, and gives it `10.100.0.1`.

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

**Idempotency.** If the same `client_nonce` comes again, return **the same response for the room already created.** Only `expires_in_s` shrinks. If that room has expired, `room_expired`. No new room is created. The basis for the decision is the `NONCE#` item (6.3).

**Errors.** None specific to the operation. Only the common four (4.1).

### 4.3 join_room

Called by the participant. There are two forms, **distinguished by the field combination.**

| Form | Fields | Meaning |
|------|------|-----|
| New join | `room_id`, `client_nonce` | Claim a virtual IP from the pool and issue a new `peer_id` and `peer_token` |
| Rejoin | `room_id`, `peer_id`, `peer_token` | Prove it is the same peer and get back **the same `peer_id` and virtual IP** ([`protocol.md`](protocol.md) section 14 contract 7) |

Mixing the fields of the two forms (both `client_nonce` and `peer_token`) is `bad_request`. The server does not guess which one was meant.

**Request (new join).**

| Field | Type | Required |
|------|-----|:---:|
| `room_id` | string | Yes. Goes through 2.1 normalization |
| `client_nonce` | 32-character hex | Yes |

**Request (rejoin).**

| Field | Type | Required |
|------|-----|:---:|
| `room_id` | string | Yes |
| `peer_id` | uint32 | Yes |
| `peer_token` | 32-character hex | Yes |

**Response (common to both forms).**

| Field | Type | Meaning |
|------|-----|-----|
| `room_id` | 6-character string | The normalized value. The client uses this value from then on |
| `peer_id` | uint32 | |
| `peer_token` | 32-character hex | On rejoin, **the received value is returned as is.** No new token is issued |
| `virtual_ip` | dotted-decimal string | |
| `expires_in_s` | integer | |

**What rejoin resets.** The candidate list is cleared. A restarted process binds to port 0 again ([`protocol.md`](protocol.md) section 6), so the earlier candidates are all stale. If stale candidates were kept, the peer would receive dead addresses from `get_peers`. The room's `ready_at` is **not cleared.** Once fixed, the `punch_delay_ms` reference point is immutable per room (section 14 contract 3). Even after the rejoined peer registers new candidates, `get_peers` stays ready and `elapsed_since_ready_ms` keeps growing. The client formula `max(0, punch_delay_ms - elapsed)` reaches 0 and it punches immediately. That is the behavior [`protocol.md`](protocol.md) 10.2 fixed.

**Rejoin does not revive the surviving peer.** The measured limit that the restarted side's new `HELLO` does not reach the peer's NAT is in [`protocol.md`](protocol.md) 10.4. What this operation guarantees stops at **keeping the identifiers and the virtual IP.** A procedure for the surviving side to poll `get_peers` again and receive new candidates does not exist in v1. When that procedure is added later, this contract is its premise.

**Idempotency (new join).** If the same `room_id` and `client_nonce` come again, return what was first issued, as is. The `NONCE#` item is the basis. A different `client_nonce` is a new join, and if the pool is not empty, `room_full`. **A client that lost the response and retries with a changed nonce fills the room by itself.** That is why 2.4 fixed one value per launch.

**Errors.** `room_not_found`, `room_expired`, `room_full`, `unauthorized` (token mismatch on rejoin, or that `peer_id` is not in the room). The common four (4.1) are not listed separately.

**Why a `peer_id` that is not in the room is `unauthorized` and not `room_not_found`.** The room exists. What is missing is the peer. A nonexistent `peer_id` and a `peer_id` with a wrong token have to be answered with the same code so that a caller who knows only the `room_id` cannot enumerate the `peer_id`s in the room. Enumeration is unrealistic anyway in a 32-bit space, but there is no reason to help by distinguishing in the response.

### 4.4 register_candidate

Called by a peer after it finishes STUN. It **replaces the whole candidate list.** It is not an append. Call it twice and the second list remains. Replace semantics were chosen because they are idempotent. Sending again after a lost response gives the same result.

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
| `kind` | `"local"` or `"reflexive"` | The two kinds from [`protocol.md`](protocol.md) 10.1. The server only stores and delivers it. It is not used in any decision |

**Hygiene.** The server applies the rules of [`protocol.md`](protocol.md) 10.1 (section 14 contract 5). The client also applies the same rules again to the list it receives. Regardless of trusting the server, both sides do the format check. The case table below expands the 10.1 rules **for server input**, and that section owns the rules themselves.

| Candidate | Verdict | Basis |
|------|------|------|
| `192.168.0.10:51000 local` | Store | Normal local candidate |
| `203.0.113.7:51000 reflexive` | Store | Normal reflexive candidate |
| `255.255.255.255:51000` | Reject | 10.1 broadcast |
| `224.0.0.1:51000` / `239.255.255.250:1900` | Reject | 10.1 multicast `224.0.0.0/4` |
| `0.0.0.0:51000` | Reject | 10.1 unspecified |
| `127.0.0.1:51000` | Reject | 10.1 loopback. **Unlike receive source hygiene (section 7), the candidate list rejects loopback** |
| `10.0.0.5:0` | Reject | 10.1 port 0 |
| `10.0.0.5:65536` / negative / string | Reject | Format |
| `"10.0.0"` / `"10.0.0.5.1"` / `"::1"` / `"10.000.0.5"` | Reject | Not four-octet dotted-decimal IPv4. **Do not use a lenient parser of the `inet_aton` kind.** On this repo's Windows Python 3.11, `socket.inet_aton` accepted `10.0.5` as `10.0.0.5` and `010.0.0.5` as `8.0.0.5` (octal) |
| Same `ip:port` twice | Store one | 10.1 deduplication. It is the same endpoint even if `kind` differs |
| 9 candidates (all normal) | **`bad_request`** | A client sending more than 8 is a format violation. The 10.1 "drop the excess" is a client rule for a **received** list; the server enforces the cap on the sender |
| Empty array | `bad_request` | With no candidates there is nothing to register |
| `kind` is another value | Reject | Format |

**Rejection policy.** If an individual candidate fails hygiene, **drop only that candidate** and store the rest. Hygiene rejection does not fail the whole request. However, **if all are rejected and 0 candidates remain to store, it is `bad_request`.** Storing 0 and returning `ok` would make the peer receive an empty list after ready, end in `HOLE_PUNCH_TIMEOUT`, and disguise that failure as a NAT failure. Format violations (9 candidates, empty array, wrong type) are a request problem, not an individual candidate problem, so the whole request is rejected with `bad_request`.

**Response.**

| Field | Type | Meaning |
|------|-----|-----|
| `accepted` | integer | Number of candidates stored |
| `rejected` | integer | Number of candidates dropped by hygiene. If nonzero, the client leaves one `WARN` line |
| `ready` | boolean | True if this registration made the room ready or it was already ready |

**Ready decision.** After the store completes, if all peers in the room (`MAX_PEERS` of them) have 1 or more candidates and `ROOM.ready_at_wall_ms` is absent, write `ready_at_*` and `punch_delay_ms = PUNCH_DELAY_MS` once with a conditional write (6.3). Even if two peers register at the same time and both see "all registered", the conditional write succeeds only once. The side that fails means it was already written, so it is not an error. This is how section 14 contract 3 is met.

**Errors.** `room_not_found`, `room_expired`, `unauthorized`. The common four (4.1) are not listed separately.

### 4.5 get_peers

The client polls at the interval (500ms) and deadline (60s) of [`protocol.md`](protocol.md) section 11. **That document owns the interval and the deadline.** The numbers are not repeated here.

**Request.**

| Field | Type | Required |
|------|-----|:---:|
| `room_id` | string | Yes |
| `peer_id` | uint32 | Yes |
| `peer_token` | 32-character hex | Yes |

**Response (before ready).**

```json
{"ok": true, "ready": false}
```

**Response (ready).**

| Field | Type | Meaning |
|------|-----|-----|
| `ready` | `true` | |
| `punch_delay_ms` | integer | The value written to the room. Same in every response |
| `elapsed_since_ready_ms` | integer 0 or more | From the moment ready was written to the moment this response is built. How it is computed is in 7.4 |
| `peers` | array | Peers **excluding self.** In v1 the length is 1 |

`peers` element:

| Field | Type | Meaning |
|------|-----|-----|
| `peer_id` | uint32 | |
| `virtual_ip` | dotted-decimal string | Used for the `virtual_ip` check in [`protocol.md`](protocol.md) 5.1 (section 14 contract 6) |
| `candidates` | array | Same element type as 4.4. In stored order |

**The definition of ready is the existence of `ROOM.ready_at_wall_ms`.** Peer count and candidate count are not recounted at response time. If they were recounted, ready would flip back to false the moment a rejoin (4.3) cleared the candidates, and that would diverge from the punch the peer has already started.

**A ready response with 0 peer candidates can exist.** It is the window after a rejoin (4.3) cleared the candidates and before they are registered again. The client does not treat that response as ready and **keeps polling.** If it hits the deadline, it is `CONTROL_PLANE_EXCHANGE_FAILED` by the polling deadline rule of [`protocol.md`](protocol.md) 9.6. Entering the punch with an empty list would wait 10 seconds with nowhere to send, end in `HOLE_PUNCH_TIMEOUT`, and that is a control plane failure disguised as a NAT failure. This is the same judgment as [`architecture.md`](architecture.md) section 8 classifying "no peer candidates" as `CONTROL_PLANE_EXCHANGE_FAILED`.

**Self is excluded by `peer_id`.** Two peers cannot have the same `peer_id` (2.2), so if the response contains a `peer_id` equal to one's own, it is a server defect. In that case the client ends with `CONTROL_PLANE_EXCHANGE_FAILED` ([`protocol.md`](protocol.md) 4.2).

**Errors.** `room_not_found`, `room_expired`, `unauthorized`. The common four (4.1) are not listed separately.

**`not_ready` is not an error.** It is `ok: true` with `ready: false`. Polling is progressing normally, and putting it in the error envelope would give the client an exception path of "error, but continue".

---

## 5. State Transitions

### 5.1 Room

Room state is **derived from stored fields.** There is no separate state field. If there were one, cases would arise where the field and the state disagree.

| State | Definition |
|------|------|
| `waiting` | `ROOM` item exists, `now < expires_at`, no `ready_at_wall_ms` |
| `ready` | `ROOM` item exists, `now < expires_at`, `ready_at_wall_ms` present |
| `expired` | `ROOM` item exists, `now >= expires_at`. It appears in this state until TTL deletion |
| (none) | No `ROOM` item. It never existed, or TTL deleted it |

```text
(none) --create_room--> waiting --second peer's register_candidate succeeds--> ready
                          |                                                  |
                          +---------------- ROOM_TTL_S elapses --------------+--> expired --TTL deletion--> (none)
```

**Expiry is not extended.** `ROOM_TTL_S` (3600 seconds) is fixed from the moment of creation. If activity extended it, nobody could answer "when does the room disappear". One hour is enough to create a room, pass the code to the peer, and have both sides finish STUN and registration, and after the connection is established the control plane is not needed (NFR-3). That is, this value is **the allowance until connection establishment** and is unrelated to session length. If a rejoin becomes necessary after a 30-minute game, the room is expired, `room_expired` comes back, and a new room is created. This is a limit v1 accepts, and this value is revisited when automatic retry is decided ([`protocol.md`](protocol.md) 10.4 open item).

**Allowed states per operation.**

| Operation | `waiting` | `ready` | `expired` | (none) |
|------|-----------|---------|-----------|--------|
| `create_room` | - | - | - | Create. Same nonce returns the existing room (4.2) |
| `join_room` new join | Attempt claim | Attempt claim. The pool will be empty, so `room_full` | `room_expired` | `room_not_found` |
| `join_room` rejoin | Return after token check | Same | `room_expired` | `room_not_found` |
| `register_candidate` | Store. Ready decision | Store. Ready decision is already true | `room_expired` | `room_not_found` |
| `get_peers` | `ready: false` | Ready response | `room_expired` | `room_not_found` |

**Expiry decision case table.** `now` is the server wall clock in milliseconds, `expires_at_ms` is the stored value.

| `now` vs `expires_at_ms` | Verdict |
|--------------------------|------|
| `now < expires_at_ms` | Alive |
| `now == expires_at_ms` | **Expired.** The boundary is on the expired side. A room with `expires_in_s` of 0 cannot be joined |
| `now > expires_at_ms` | Expired |
| `expires_at_ms` field missing | **`internal`.** Do not turn "cannot decide" into pass. The creation path always writes this field, so if it is missing the store is corrupted |

`expires_in_s` is `max(0, ceil((expires_at_ms - now) / 1000))`. A live room can yield 0 (less than 1 second left). The client uses this value only for display and not in any decision.

### 5.2 Ready

`ready_at_wall_ms` **does not change once written until the room disappears.** It stays even if a rejoin clears candidates, or any peer registers candidates again. The basis is in 4.3 and 4.5.

### 5.3 Peer

| State | Definition |
|------|------|
| `joined` | `PEER#` item exists and `candidates` is empty |
| `registered` | `candidates` has 1 or more |

```text
(none) --create_room / join_room new join--> joined --register_candidate--> registered
                                              ^                              |
                                              +---- join_room rejoin (clears candidates) ----+
```

Peers disappear together with the room. There is no operation that deletes only a peer. `leave_room` is not provided because the room lifetime is short (5.1) and slot reclaim conflicts with the rejoin contract (2.5).

---

## 6. Storage

### 6.1 Storage Contracts

State lives in Amazon DynamoDB. **Why DynamoDB and what was rejected** is owned by [ADR 0004](decisions/0004-state-store-dynamodb.md). There are five contracts.

| Contract | Content |
|------|------|
| Storage 1 | Reads whose values feed a decision set **`ConsistentRead=true` on the table.** Because of Storage 5, **every place where an operation reads room or peer items falls under this.** Not only `get_peers`. Default reads are eventually consistent ([ADR 0004](decisions/0004-state-store-dynamodb.md) decision 1 quotes the AWS documentation), so a candidate registered a moment ago can be missing, and then punching ends in `HOLE_PUNCH_TIMEOUT` and **is disguised as a NAT failure** |
| Storage 2 | Strongly consistent reads work only on the table and LSIs. **No GSI is created.** This design has no query that needs a GSI |
| Storage 3 | Virtual IP assignment keys one item per address and **claims it with a conditional write (`attribute_not_exists`).** No atomic counter is used. A counter is not idempotent and a retry skips an address. On a failed claim, move to the next candidate address, and after one pass over the pool end with `room_full` (2.5). It does not loop forever |
| Storage 4 | Room expiry **is decided by the application** (5.1). TTL is a means of reclaiming storage space, and an item past its expiry time is still visible to queries until it is deleted |
| Storage 5 | The control server **holds no room or peer state in memory.** Every request reads from DynamoDB |

**Because of Storage 5, there is no "restart recovery" procedure.** There is no step at startup that loads the room list, and the first request right after a restart reads exactly as usual. The room state persistence across restart that [`spec.md`](spec.md) NFR-3 requires **holds by this contract, not by a recovery procedure.** A cache would recreate the same problem as eventually consistent reads, so if a cache becomes necessary, redecide it together with Storage 1.

**The exceptions to Storage 5 are the rate limit table (6.4) and the in-flight request count (7.2).** Those two are not room or peer state; they are the server process's self-protection state. They vanish on restart, and what is lost is only a brief moment of protection right after the restart. No other in-memory state is created beyond these two.

### 6.2 Table

**One table.** The name is set by deployment configuration (7.6) and is not hard-coded.

| Item | Value |
|------|-----|
| Partition key | `pk` (string). The value is `room_id` |
| Sort key | `sk` (string). Item kind and identifier |
| TTL attribute | `ttl` (number, epoch seconds). 6.5 |
| Capacity mode | [ADR 0004](decisions/0004-state-store-dynamodb.md) decision 4. Provisioned. Values are in deployment configuration |
| Indexes | None (Storage 2) |

Items belonging to a room (`ROOM`, `PEER#`, `VIP#`) share the same partition key, so the whole state of one room is read with a single `Query(pk = room_id, ConsistentRead=true)`. The item count is at most `1 + MAX_PEERS × 2` (one `ROOM`, and a `PEER#` and a `VIP#` per peer). In v1 that is at most 5. **The `NONCE#` item has a different partition key.** The idempotency lookup of `create_room` has to find it by nonce alone, at a point where the `room_id` is not yet known.

### 6.3 Items

| `sk` | Attributes | Meaning |
|------|------|-----|
| `ROOM` | `created_at_ms`, `expires_at_ms`, `host_peer_id`, `ttl` | Room. The ready attributes below are absent at first |
| `ROOM` (after ready) | The above plus `ready_at_wall_ms`, `ready_at_mono_ns`, `ready_boot_id`, `punch_delay_ms` | 7.4 uses the three clock values |
| `PEER#<peer_id>` | `peer_id`, `peer_token`, `virtual_ip`, `candidates` (list), `client_nonce`, `joined_at_ms`, `ttl` | Peer. `peer_id` is written in decimal in `sk`. `peer_token` is the raw value (2.3) |
| `VIP#<virtual_ip>` | `peer_id`, `ttl` | Virtual IP claim marker. `<virtual_ip>` is dotted-decimal |
| (pk = `NONCE#<client_nonce>`, sk = `NONCE`) | `room_id`, `peer_id`, `ttl` | Idempotency key → room and peer mapping. **The pk is the nonce, not the room** |

Token comparison uses a constant-time comparison (`hmac.compare_digest`). A comparison inside a condition expression (`peer_token = :t`) is done by DynamoDB, so constant time is not claimed for it. Therefore operations that need the token **first read `PEER#` and do the constant-time comparison in the application**, and the `peer_token = :t` in the condition expression serves to confirm that the value did not change between the read and the write.

**All writes are conditional, and any place that writes several items together uses `TransactWriteItems`.** If any one condition fails, all of them are cancelled.

| Operation | Write | Condition |
|------|------|------|
| `create_room` | Transaction: `ROOM` put, `PEER#host` put, `VIP#10.100.0.1` put, `NONCE#` put | `attribute_not_exists(pk)` on `ROOM` and on `NONCE#` each. A `ROOM` failure is a `room_id` collision → redraw (2.1). A `NONCE#` failure means the same nonce arrived concurrently, so read again and return that result |
| `join_room` new join | Per pool address, a transaction: `ROOM` **ConditionCheck**, `VIP#<ip>` put, `PEER#<peer_id>` put, `NONCE#` put | `attribute_exists(pk) AND expires_at_ms > :now` on `ROOM`. This blocks the race where the room expires or is deleted between the prior read and the write. `attribute_not_exists(pk)` on `VIP#`, `PEER#`, and `NONCE#` each. Failure handling is in the cancellation reason table below |
| `join_room` rejoin | `PEER#` update: `candidates = []` | `attribute_exists(pk) AND peer_token = :t`. Failure is `unauthorized` |
| `register_candidate` | `PEER#` update: `candidates = :list` | `attribute_exists(pk) AND peer_token = :t` |
| Ready write | `ROOM` update: `ready_at_wall_ms`, `ready_at_mono_ns`, `ready_boot_id`, `punch_delay_ms` | `attribute_not_exists(ready_at_wall_ms)`. Failure is not an error (4.4) |

**When a transaction is cancelled, read the cancellation reason per item and decide by the priority below.** The `CancellationReasons` of `TransactionCanceledException` come one per item, **in the same position** as the items put into the transaction. So items are identified by position, and the decision order is not by position but by the table below. Several items can fail at once, so which one is looked at first determines the response.

| Order | Failed condition | Action |
|:----:|-------------|------|
| 1 | `NONCE#` | **Whatever the other reasons are,** a request with the same nonce was established first. Read `NONCE#` again, `Query` the room by its `room_id` and `peer_id`, and return the same response. If that room has expired, `room_expired` (4.2). Do not go to `room_full` or to a redraw |
| 2 | `ROOM` ConditionCheck (`join_room`) | Read the room again. If absent, `room_not_found`; if present, `room_expired`. Do not move to the next address |
| 3 | `VIP#` | Next address. When the pool is exhausted, `room_full` |
| 4 | `PEER#` | Redraw `peer_id`. The cap is `MAX_PEER_ID_ATTEMPTS` |
| 5 | `ROOM` put (`create_room`) | Redraw `room_id`. The cap is `MAX_ROOM_ID_ATTEMPTS` |
| - | A reason that is not a condition failure (`TransactionConflict`, throttling) | `internal`. The client retries per 8.3 |

Row 1 comes first because of the retry scenario. When a participant who lost the response comes back with the same nonce and the first request already claimed `VIP#`, then `VIP#` and `NONCE#` fail **together in the same transaction.** If `VIP#` were looked at first, the server would move to the next address and return `room_full`, and that client would believe it failed to enter a room it is already in.

**Single-item updates (rejoin, `register_candidate`, the ready write) do not put room liveness in the condition.** Expiry is decided first in the order of 7.3 and then the write happens, so the only thing that slips through is the race where the room expires within the tens of milliseconds between the read and the write. **That race is accepted.** Its result is stated precisely. **The current request can receive `ok`.** For `register_candidate`, `accepted` and `ready` also come back with normal values. The updated item sits inside an expired room, and every **subsequent** operation that reads that room ends in `room_expired` at step 6 of 7.3, so the only response that carries the updated value is that one request. The client receives `room_expired` on its next request (usually the `get_peers` poll) and ends with `CONTROL_PLANE_EXCHANGE_FAILED`. Expiry is not rechecked right before the response. Even if it were, the same race exists between that check and the response. Only new join puts it in the condition, because that race would **create new `VIP#` and `NONCE#` items inside an expired room** and their `ttl` has to be derived from the room's value, and the room may already be gone.

**A failed condition still consumes write capacity** ([ADR 0004](decisions/0004-state-store-dynamodb.md) decision 2). So new join reads the room **before** walking the pool to confirm expiry and existence first, and the pool size is itself the attempt cap.

**Reads.**

| Operation | Reads |
|------|------|
| `create_room` | `NONCE#` GetItem (idempotency). If present, `Query` by that `room_id` and assemble the same response |
| `join_room` new join | `NONCE#` GetItem → if present, the stored `room_id` has to equal the request's (otherwise `bad_request`; the nonce was reused for a different room) → if absent, `ROOM` GetItem (expiry and existence) → transaction |
| `join_room` rejoin | `ROOM` GetItem → `PEER#` GetItem (token check) → update |
| `register_candidate` | One `Query(pk)` for `ROOM` and all `PEER#` (expiry and token check) → `PEER#` update → the ready decision is made on the same `Query` result with its own registration applied |
| `get_peers` | One `Query(pk)`. `ROOM` and all `PEER#` come back. The token check is also done on that result |

All of them are `ConsistentRead=true` (Storage 1). **Rejoin reads `PEER#` with GetItem because the request supplies the `peer_id`.** That is why there is no form that finds a peer by token alone. Finding by token would require reading every peer and comparing.

### 6.4 Rate Limit

**A budget on failure responses per source IP.** What is counted is three: `room_not_found`, `room_expired`, `unauthorized`. Success and `bad_request` are not counted. `bad_request` is a format error unrelated to guessing, and counting success would catch normal polling.

| Item | Value |
|------|-----|
| Budget | `RATE_LIMIT_BUCKET` (10) tokens per source IP. One is spent each time one of the three errors above goes out |
| Refill | `RATE_LIMIT_REFILL_PER_MIN` (10) per minute |
| When exhausted | **The request is not processed** and `rate_limited` is returned. The store is not read. That way the limit also stops storage cost |
| Table | In memory. Source IP → (tokens, last refill time). Cap `MAX_RATE_ENTRIES` (4096) |
| When the table is full | **New sources are passed through without limiting.** And the `rate_table_full` counter is incremented. Evicting the oldest entry would let an attacker with many IPs fill the table and evict the real guesser's entry. With pass-through, the attacker gains nothing by filling the table. The price is that new sources have no limit while the table is full, and if this value rises, the cap is revisited |
| When the budget is checked | **After** the request is parsed, **before** the store is read |

**It slows, it does not stop.** At 10 per minute from one IP, sweeping the 30-bit space takes hundreds of thousands of years, but with many IPs it divides by that many. The last line of 1.2 is this limit.

**Case table.**

| Order | Input (same source) | Response | Tokens left |
|:----:|--------------------|------|:---------:|
| 1~10 | `join_room` on a nonexistent room, 10 times | `room_not_found` 10 times | 0 |
| 11 | `join_room` on a nonexistent room | **`rate_limited`.** No store read | 0 |
| 12 | Normal `get_peers` on an existing room | **`rate_limited`.** The budget is per source, so a normal request is blocked too | 0 |
| 13 | Any request 6 seconds later | 1 token refilled. Processed | 0 (one spent) or 1 (if success) |
| - | `join_room` on a nonexistent room from another source | `room_not_found`. The table is per source | 9 for that source |
| - | Normal `get_peers` 100 times (success) | All processed | 10. Success is not counted |
| - | `bad_request` 100 times | All `bad_request` | 10. Not counted |

Row 12 means the following. **When two users behind the same NAT share a public IP, one person's guessing blocks the other person's normal requests.** A campus network is like that. A normal client receives a failure response once or twice per launch at most, so a budget of 10 covers that case, but if the person next door burns the budget, it is blocked. This is a limit v1 accepts. The alternative (not counting requests that carry a token) leaves `unauthorized` guessing open, so it is not used.

### 6.5 TTL

| Item | `ttl` value |
|------|----------|
| All items of a room (`ROOM`, `PEER#`, `VIP#`, `NONCE#`) | `floor(expires_at_ms / 1000) + STORAGE_GRACE_S` |

Items of the same room share the same `ttl`. One day after the room expires, DynamoDB deletes them. **The one-day grace exists because expiry is decided by the application per Storage 4.** If items were deleted right after expiry, the distinction between `room_expired` and `room_not_found` (4.1) would waver depending on deletion timing. One day later it no longer matters which one is answered. That the AWS documentation states the deletion delay only as "within a few days" and sets no upper bound is in [ADR 0004](decisions/0004-state-store-dynamodb.md) decision 3 and item 1 of "what was not confirmed".

---

## 7. Server Implementation

### 7.1 Modules

| Module | Responsibility | External dependency |
|------|------|-----------|
| `server.py` | Accept loop, 3.3 HTTP parsing and response serialization, operation dispatch, time limits, `MAX_INFLIGHT`, rate limit table (6.4) | `asyncio` |
| `ops.py` | The four operations of section 4. Request validation, state decisions (section 5), response assembly | None |
| `ids.py` | Generation and normalization of `room_id`, `peer_id`, `peer_token`. The 2.1 alphabet and case table attach here as tests | `secrets` |
| `candidates.py` | 4.4 hygiene. Candidate format checks and the 10.1 rules | `ipaddress` is not used. See below |
| `clock.py` | The three clock values of 7.4 | `time`, `/proc/sys/kernel/random/boot_id` |
| `store.py` | All `boto3` calls. The condition expressions of 6.3 live **only in this file** | `boto3` |

**Candidates are not judged with the `ipaddress` module.** On this repo's Python 3.11, the observed values are that `ip_address('127.0.0.1').is_private` and `ip_address('192.0.2.1').is_private` are true, and `ip_address('224.0.0.1').is_global` is also true. This can differ by version, so no version is allowed to delegate the decision to those predicates. The decisions of the 4.4 case table parse the four octets directly and branch on the leading octet and range. Parsing is `^\d{1,3}(\.\d{1,3}){3}$` with each octet 0~255 and **leading zeros rejected.** Using `ipaddress.IPv4Address` for format parsing only is allowed. The point is that its predicates are not used for the decision.

`asyncio` and `json` are standard library. `boto3` has [`spec.md`](spec.md) NFR-5 approval. No web framework is used (3.1).

### 7.2 One Pass of Request Handling

```text
on_connection(reader, writer):
    src = peer IP
    if inflight >= MAX_INFLIGHT:
        respond(503, unavailable); close; return          # does not touch the store
    inflight += 1
    try:
        req = await wait_for(read_request(reader), SERVER_READ_TIMEOUT_S)
                                                          # 3.3 case table. Failures end here as 4xx
        if rate.exhausted(src):                           # 6.4. After parsing, before the store
            respond(429, rate_limited); return
        result = await to_thread(ops.dispatch, req)       # boto3 is blocking. Send it to a thread
        if result.error in {room_not_found, room_expired, unauthorized}:
            rate.spend(src)                               # the three that 6.4 counts
        respond(result)
    except TimeoutError:
        counters.http_read_timeout += 1; close            # no response (3.4)
    except Exception:
        counters.internal_error += 1
        respond(500, internal)                            # do not put the exception text in the body
    finally:
        inflight -= 1; close
```

**`boto3` calls are treated as blocking.** Calling them directly on the event loop thread stalls accepting and parsing of other connections for the duration of the DynamoDB response delay. They are sent through `asyncio.to_thread`. The concurrency cap `MAX_INFLIGHT` (32) stops that thread pool from growing without bound and stops waiting requests from piling up and eating memory when the store slows down. Beyond the cap it is `unavailable`, and the client retries per 8.3.

**One process, one event loop, one thread pool.** No multiprocessing. With several processes, the rate limit table (6.4) would exist separately per process and the budget would multiply by the process count.

### 7.3 Operation Processing Order

This is the common order inside `ops.dispatch`. Every operation keeps this order. Changing the order changes the responses in the case tables.

```text
1. Field presence and type check (section 4 tables)  -> failure: bad_request
2. room_id normalization (2.1)                       -> failure: bad_request
3. Per-operation field combination check (4.3 forms) -> failure: bad_request
4. Store read (6.3 reads table, ConsistentRead)
5. Room existence                                    -> absent: room_not_found
6. Room expiry (5.1 case table)                      -> expired: room_expired
7. Token check (only operations that need it)        -> failure: unauthorized
8. Operation body (conditional writes, ready decision)
9. Response assembly
```

**Format checks come before the store read.** That way `bad_request` spends no storage cost, and a guessing attack does not reach the store even though the rate limit (6.4) does not count format errors. **Expiry comes before the token.** A request with a wrong token on an expired room is `room_expired`. An expired room cannot be used anyway, so revealing whether the token matched loses nothing, and pinning the order is what makes the case tables deterministic.

### 7.4 Clocks

`elapsed_since_ready_ms` is **computed with a monotonic clock** ([`protocol.md`](protocol.md) section 14 contract 4). Computed with the wall clock, the moment NTP steps the time the values the two peers receive diverge and the error bound of 10.2 breaks. But because of Storage 5 the server does not hold the ready time in memory, and monotonic clock values have a different base per boot. So three values are stored together.

| Stored value | Source | Use |
|--------|------|------|
| `ready_at_mono_ns` | `time.monotonic_ns()`. **The premise is that in the deployment environment (Linux) this is the system-wide clock (`CLOCK_MONOTONIC`), so its base does not change across process restarts.** See below | Elapsed computation within the same boot |
| `ready_boot_id` | Content of `/proc/sys/kernel/random/boot_id`. **The premise is that this value changes on every boot** | Deciding whether the value above has the same base |
| `ready_at_wall_ms` | `time.time()` in milliseconds | Fallback when the boot changed, and the ready-existence decision of 5.1 |

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
| After an instance reboot | Wall clock fallback | Can be off by the NTP step. **In this case the error bound of [`protocol.md`](protocol.md) 10.2 is not guaranteed.** Section 14 contract 4 delegates that exception to this document. A reboot and a time adjustment have to overlap within the few seconds between ready and the polling response, so it is rare. It is observed by the counter |
| Negative in the wall clock fallback | `max(0, ...)` | 0. The client waits the full `punch_delay_ms` |
| No `boot_id` file (not Linux) | A random value drawn at process start is used as `boot_id` | Every process restart drops to the wall clock fallback. That is what happens in local tests. **The deployment target is Linux** |

**The two premises were not confirmed in this repo.** This machine is Windows and `time.get_clock_info('monotonic')` returns `GetTickCount64()`. Confirming on the deployment instance, with the same call, that the implementation is `clock_gettime(CLOCK_MONOTONIC)`, and confirming that `boot_id` differs before and after a reboot, are [`roadmap.md`](roadmap.md) Phase 3 pre-start items. Until confirmed, this section is **a design, not verified behavior.** If either premise turns out wrong, it drops to the wall clock fallback, so what is lost is accuracy, not correctness.

### 7.5 Logs and Counters

One event per line on standard error. The shape is the same as the client log contract ([`architecture.md`](architecture.md) section 9): `<level> <event key> <name>=<value> ...`. The keys the Phase 3 verification looks for are the following.

| Event key | Required fields | Which verification uses it |
|----------|-----------|--------------------|
| `http.request` | `op`, `status`, `error` (`-` on success), `src`, `ms` | Verification of every operation. **Does not carry `peer_token`, the body, or `room_id`.** If a room code lands in the log, reading the log is room hijacking |
| `room.created` | `peer_id`, `virtual_ip` | Virtual IP assignment verification. `room_id` is not carried |
| `room.ready` | `punch_delay_ms` | Verification of the single ready write. It has to appear **exactly once** per room |
| `vip.claimed` | `virtual_ip`, `peer_id` | Verification of duplicate assignment under concurrent join |
| `counter` | `name`, `value` | The counters below |

The counters are `http_read_timeout`, `internal_error`, `rate_limited`, `rate_table_full`, `elapsed_wall_fallback`, `unavailable`. All of them are emitted at shutdown and every 60 seconds. Zero values are emitted too.

**Not carrying `room_id` in the log is stated again.** The `op` and `status` of `http.request` are enough for the Phase 3 verification. Diagnostics that need to know which room use `peer_id`. A `peer_id` has meaning only inside a room, so it alone cannot be used to join.

### 7.6 Configuration and Deployment

Configuration is environment variables. There is no file. There are only four values.

| Variable | Meaning | Default |
|------|-----|--------|
| `SANGTACHI_CP_PORT` | Bind port | `8000` |
| `SANGTACHI_CP_TABLE` | DynamoDB table name | None. **Required** |
| `AWS_REGION` | Region | None. Required. Standard `boto3` variable |
| `SANGTACHI_CP_ENDPOINT` | DynamoDB endpoint URL override. For local tests | None. If absent, the region default |

**There is no credentials variable.** On EC2 it is an IAM role ([ADR 0004](decisions/0004-state-store-dynamodb.md) decision 5), and local tests use DynamoDB local, which needs no credentials. If a variable that takes an access key existed, someone would use it.

Deployment is **one systemd service.** `Restart=always`. There is no restart recovery procedure (6.1), so restarts are cheap. The security group opens inbound TCP 8000. The telemetry service's port is outside this document's scope and is a [`roadmap.md`](roadmap.md) Phase 9 pre-start item. **The unit file and deployment commands are not kept here.** This follows the rule that procedures not yet executed do not go into documents; after the real deployment in Phase 3, the procedure goes into a tool and the document points to it.

**Limits of local testing.** In DynamoDB local, reads usually look like the latest value, so a missing `ConsistentRead` does not show, and `TransactionConflictException` does not occur either. Both are stated with AWS documentation quotes in the "what it costs" section of [ADR 0004](decisions/0004-state-store-dynamodb.md). Storage 1 is judged by reading the code, and the transaction conflict path is confirmed on a real table.

---

## 8. Client-Side Contract

### 8.1 Owning Thread

Control plane TCP calls and DNS resolution are done by the **`[control]` thread.** Thread composition, the queue with `[loop]`, and shutdown order are owned by [`architecture.md`](architecture.md) 3.2.8. Here only which rules of this document that thread keeps is stated.

The reason it is not put in `[loop]` is one line. If the control server does not respond, a TCP `connect` with no connection time limit blocks for the length of the OS default retry, and during that time `HELLO` retransmission and keepalive stop. That is a [`spec.md`](spec.md) NFR-3 violation.

### 8.2 Time Limits

| Stage | Value | If exceeded |
|------|-----|--------|
| DNS resolution (`getaddrinfo`) | OS default. **No separate limit is set.** This design does not use a means of putting a time limit on a synchronous call | `[control]` blocks for that long. `[loop]` is unaffected |
| `connect` | `CLIENT_CONNECT_TIMEOUT_S` (3). Non-blocking `connect` + `select` | Transport error |
| Send, receive, each | `CLIENT_IO_TIMEOUT_S` (3). `SO_SNDTIMEO` / `SO_RCVTIMEO`. The same options as the telemetry socket in [`architecture.md`](architecture.md) 3.2.7 and the same limit (they are per call, so the sum can be longer) | Transport error |
| Cap on one request | The sum of the three above. **At most 9 seconds, excluding DNS** | - |

**DNS resolution is done once at launch.** `[control]` keeps the resulting IPv4 address and uses it for every request after that. Resolving on every poll would let a DNS outage stop polling. The premise that the address does not change during a launch comes from the Elastic IP, and its basis and price are owned by [`windows-prereq.md`](windows-prereq.md) section 10. If resolution fails, launch fails. No retry.

**A new connection is opened per request** (3.3). No reuse.

### 8.3 Error Classification and Retry

The result `[control]` returns to `[loop]` is one of three.

| Result | What | Example |
|------|------|-----|
| Success | An `ok: true` response | Operation result |
| Definite error | `ok: false` and `error` is not a transient error | `room_not_found`, `room_full`, `unauthorized`, `bad_request`, `rate_limited` |
| Transient error | Transport error (3.5), `internal`, `unavailable` | connect timeout, 500, 503 |

**Retry applies only to transient errors, and the rule differs per operation.**

| Operation | On transient error |
|------|--------------|
| `create_room`, `join_room` new join | **With the same `client_nonce`,** up to 3 times at 1-second intervals. Beyond that, `CONTROL_PLANE_EXCHANGE_FAILED` |
| `join_room` rejoin | With the same `room_id`, `peer_id`, `peer_token`, up to 3 times at 1-second intervals. The server-side action is idempotent (clearing candidates), so no nonce is needed |
| `register_candidate` | Up to 3 times at 1-second intervals. Replace semantics (4.4) make retry safe |
| `get_peers` | **No separate retry rule.** The next poll is the retry. The polling interval and deadline are in [`protocol.md`](protocol.md) section 11 |

A definite error is not retried and is immediately `CONTROL_PLANE_EXCHANGE_FAILED`. **`rate_limited` is a definite error too.** If the client waited and retried on its own, several normal clients behind the same NAT would back off at the same time, rush back at the same time, and get caught again. A person restarts it.

The list of conditions that end in `CONTROL_PLANE_EXCHANGE_FAILED` is owned by [`protocol.md`](protocol.md) 9.6. This section expands what "control plane error" among those conditions means.

### 8.4 From Launch to Punch

This is the order in which the client calls the operations of this document. The timer values are all from [`protocol.md`](protocol.md) section 11; only the order is fixed here.

```text
1. Read the launch input (architecture.md 3.5). If the role is host, create_room; if player, join_room
2. [control] resolves DNS once. On failure, launch fails
3. UDP socket bind, getsockname (protocol.md section 6)
4. create_room or join_room. Print the room_id from the response to the console (the host passes it to the peer)
5. STUN (protocol.md section 13). Query both servers
6. register_candidate. Local candidates + reflexive candidates. If more than 8, the client trims first
7. Poll get_peers. Until ready: true and the peer has 1 or more candidates, up to the deadline (4.5)
8. Apply 10.1 hygiene again to the received candidates. If own peer_id appears, CONTROL_PLANE_EXCHANGE_FAILED
9. Punch after max(0, punch_delay_ms - elapsed_since_ready_ms) (protocol.md 10.2)
```

**Step 4 comes before step 5.** STUN has a 5-second deadline and the room code has to be passed by a person. Creating the room first, printing the code, and running STUN in the meantime overlaps the time the person waits. The protocol still holds if the order is swapped, but the person waits longer.

**Rejoin differs only in the `join_room` form of step 4.** Where the `peer_id` and `peer_token` needed for rejoin are kept is decided by the launch input section ([`architecture.md`](architecture.md) 3.5).

---

## 9. Verification

The Phase 3 verification items are owned by [`roadmap.md`](roadmap.md). This document owns **the case tables those items run.** The case tables are in 2.1, 3.3, 4.4, 5.1, 6.4, and 7.4, and at Phase 3 start they move to `control-server/tests/` so the tests read the tables directly. After the move, this document points to those files instead of the tables. There will not be one set of tables in the document and another in the tests.

**Every case table gets a mutation test.** Check which row of the table an implementation with one rule line deleted `FAIL`s on. If a mutation drops no row, that rule does not protect anything yet.

**What can be judged only by reading code.**

| What | Why a behavioral test cannot do it |
|------|----------------------------|
| Storage 1 `ConsistentRead=true` | Eventually consistent reads also usually return the latest value. Local even more so |
| Token comparison is constant time and done first in the application (6.3) | Timing differences are hard to reproduce in a test |
| No `ipaddress` predicates in decisions (7.1) | A case table row can pass by accident |
| `room_id` and `peer_token` are absent from logs (7.5) | Scanning all the logs is not a proof that "it appears on no path" |

---

## 10. Undecided

| What | Where and when |
|------|-------------|
| Table name, capacity values, region | Deployment configuration. Together with the [`roadmap.md`](roadmap.md) Phase 3 pre-start item (free tier check) |
| Actual values of the Elastic IP and DNS name | At Phase 3 deployment. Addresses are not hard-coded in documents |
| Telemetry service port, schema, authentication | Outside this document's scope. [`roadmap.md`](roadmap.md) Phase 9 |
| Whether the two services share one table, whether IAM is split | [ADR 0004](decisions/0004-state-store-dynamodb.md) deferred it to Phase 9 |
| Automatic retry and re-polling by the surviving side | [`protocol.md`](protocol.md) 10.4 open item. The rejoin contract of this document (4.3) is its premise |
| TLS, caller authentication | Stretch. 1.2 |
| Whether the room expiry value of 3600 seconds is right | After the Phase 8 demo. Judged by whether a rejoin was needed after 30 minutes of play |
