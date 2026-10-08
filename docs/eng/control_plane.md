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
| The caller IP is treated as an address that can be answered on that TCP connection | It is the source of a connection that completed the handshake. A NAT or proxy may sit behind it so that many users appear as one address, and conversely one caller may use many addresses | The per-source budget in 6.4 Rate Limit stands on this assumption. Against a caller with many addresses the effect shrinks accordingly, and legitimate users who share an address get caught by someone else's exhausted budget (6.4 Rate Limit case table `row-12`) |
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
| `create_room` abuse | `create_room` takes only two inputs, `client_nonce` and `name`, and answers with success, so 6.4 Rate Limit does not count it. Repeating it with a changed nonce produces a five-item transactional write per call and those items survive up to a day past expiry (6.5). Exhausting the provisioned write capacity throttles legitimate requests into `internal` | The reason 6.4 Rate Limit gives for not counting success is about `get_peers`, and `create_room` has no polling. A per-source room creation limit could be added, but v1 does not add it. **Observation is done.** The per-source frequency of the `room.created` log (7.5) is the evidence |

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
- Input is **case-insensitive.** The server first checks that the received value is ASCII, then
  upper-cases it and checks it against the alphabet. Lowercase `a` is `A`. This is not a look-alike
  substitution; it is a spelling difference of the same character
  - The ASCII check comes before upper-casing. If the order were reversed, a character such as `ſ`
    (U+017F), which becomes ASCII when upper-cased, would pass
- At creation, the server catches collisions with the conditional write of the `ROOM` item (6.3)
  and redraws on collision. The attempt cap is `MAX_ROOM_ID_ATTEMPTS` (4). Beyond that, it is an
  `internal` error. At a scale of tens of open rooms, a collision in a 30-bit space effectively
  does not happen; the cap exists to stop a storage error from turning into infinite retries

**Case table.** The table that normalization and validation have to pass is `ROOM_ID_CASES` in
[`test_room_id.py`](../../control-server/tests/test_room_id.py). Rows for rules outside the table
are in `ROOM_ID_EXTRA_CASES` in the same file. Leading and trailing whitespace is not stripped, and a
value that is not a string or a missing field is `bad_request`.

### 2.2 peer_id

**CSPRNG 32-bit, 0 excluded, unique within a room.** The wire header fixes `peer_id` at 4 bytes
([`protocol.md`](protocol.md) 4.2 `peer_id`) and the server issues this value.

- Draw with `secrets.randbits(32)` and redraw if it is 0
- Uniqueness within a room is guaranteed by the conditional write of the `PEER#<peer_id>` item (6.3)
- On collision, redraw; the attempt cap is `MAX_PEER_ID_ATTEMPTS` (4). The cap is one per request.
  It is not renewed when `join_room` moves on to the next address in the pool

Sequential assignment is not used. The reason is in "Why It Was Decided This Way" below.

### 2.3 peer_token

**128-bit CSPRNG, 32 lowercase hex characters.** `secrets.token_hex(16)`. It is carried in the
join (or room creation) response and then included in every request from that peer.

**The server stores the raw value** (6.3).
If the value carried in a request is not in this format (it contains uppercase letters or has a
different length), the token is not compared and the result is `bad_request` (step 1 of 7.3
Operation Processing Order).

- If only a hash were stored, a client that lost the response and came back with the same
  `client_nonce` (2.4, 4.2, 4.3) could not be given the same token
- The price is that a token leaks if the store is read. In v1, where the path is plaintext and
  `room_id` is the only secret, that price is of the same kind as one already being paid (1.2).
  Revisit this line when authentication is introduced

Why the token is needed is in "Why It Was Decided This Way" below.

### 2.4 client_nonce

**Client-generated 128-bit CSPRNG, 32 lowercase hex characters.** Carried in `create_room` and
`join_room` requests. It is an idempotency key. It stops a request that was retried after a lost
response from taking the second peer slot again (4.2, 4.3). The server accepts only this format.
If it contains uppercase letters or has a different length, the result is `bad_request`.

The client uses **one value per join** and does not change it between retries of that join.
When it leaves a room and joins the next one, it draws a new value.

If the CSPRNG cannot produce a value, the client does not send the request and ends that attempt
with `CONTROL_PLANE_EXCHANGE_FAILED`. It does not substitute a fixed value or a weak random value.

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
MAX_NAME_LEN              = 16          # Same value as protocol.md section 3. Display name length cap (2.7)
MAX_BODY_BYTES            = 4096        # Request body cap (3.3)
MAX_HEADER_BYTES          = 2048        # Cap on request line plus all headers (3.3). The client also uses it for the response head (3.5)
CLIENT_JSON_MAX_DEPTH     = 32          # Nesting cap when the client parses response JSON (3.5)
SERVER_READ_TIMEOUT_S     = 5           # To receive one whole request (3.4)
CLIENT_CONNECT_TIMEOUT_S  = 3           # Client connect (8.2)
CLIENT_IO_TIMEOUT_S       = 3           # Client send / receive, each (8.2)
MAX_ROOM_ID_ATTEMPTS      = 4           # Redraw cap on room_id collision (2.1)
MAX_PEER_ID_ATTEMPTS      = 4           # Redraw cap on peer_id collision (2.2)
RATE_LIMIT_BUCKET         = 10          # Per-source failure budget (6.4)
RATE_LIMIT_REFILL_PER_MIN = 10          # Refill per minute (6.4)
MAX_RATE_ENTRIES          = 4096        # Rate limit table cap (6.4)
MAX_INFLIGHT              = 32          # Cap on requests in flight (7.2)
LINGER_S                  = 1           # Cap on waiting while discarding remaining input after answering a request not fully read (3.4)
LINGER_MAX_BYTES          = 8192        # Cap on bytes discarded during that wait (3.4)
MAX_REJECTING             = 64          # Cap on connections being rejected with 503 (7.2)
DDB_CONNECT_TIMEOUT_S     = 1           # Connect timeout of the server's DynamoDB calls (7.6)
DDB_READ_TIMEOUT_S        = 1           # Read timeout of the same calls (7.6)
DDB_MAX_ATTEMPTS          = 1           # Number of times the SDK sends. The client does the retries (7.6, 8.3)
```

`MAX_CANDIDATES`, `PUNCH_DELAY_MS`, and `MAX_NAME_LEN` are copies of values owned by
[`protocol.md`](protocol.md). **When that document changes, fix them here to follow.**

> **Why.** These three are the only copies allowed because the server code has to have the values,
> and the server code does not read C++ headers.

### 2.7 name

**A display name shown to people. It is a string of ASCII letters, digits, and underscores, at least
1 and at most `MAX_NAME_LEN` (16) characters long.** The `create_room` and `join_room` requests carry
it (4.2, 4.3). The rationale and the rejected alternatives are in
[ADR 0015](decisions/0015-refer-to-members-by-display-name.md).

```text
ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_      (63 characters)
```

- If it is not a JSON string, `bad_request`
- If even one character is outside the 63 above, `bad_request`. Spaces, hyphens, Hangul, full-width
  characters, and control characters are caught here. **Leading and trailing spaces are not trimmed**
- If the length is less than 1 or exceeds `MAX_NAME_LEN`, `bad_request`. Every character is ASCII, so
  the character count and the byte count are the same
- The three above are checked before the store is read. This is step 1 of 7.3 Operation Processing
  Order
- **Case is stored and returned as registered.** It is not changed
- **Uniqueness within the room is decided ignoring case.** The comparison key is the value converted
  to ASCII lowercase. If the room has `Alice`, `alice` is `name_taken` (4.1). Uniqueness is
  guaranteed by the conditional write of the `NAME#<key>` item (6.3)

**The character set check comes before lowercasing.** The reason is the same as the ASCII check order
of 2.1 `room_id`.

**A name is released when that peer's slot is reclaimed.** `NAME#` is deleted in the same transaction
as `PEER#` (4.6, 6.3). Before that, even the same person joining again with the same name gets
`name_taken`. There is no rejoin (4.3), and coming in again is a new join.

- On a normal leave, the host receives `CLOSE` and sends `host_report` right away, so it is released
  within one request. `CLOSE` is sent only once ([`protocol.md`](protocol.md) 5.6), so if it is lost
  it is the same as the next line
- If the process dies, it is released only when the host learns of it by the idle timeout
  ([`protocol.md`](protocol.md) section 11)
- If it left without candidates, it is released only after the grace `JOIN_REGISTER_GRACE_S` of 4.6
  server reclaim passes
- To come in again during that time, use a different name

**The name is for display only.** The server does not look up peers by name. Authentication,
reclaim, and confirmation use `peer_id` and `peer_token`. The same rule on the client side is
"The roster is for display only" in [`protocol.md`](protocol.md) 5.7 ROSTER.

> **Why compare ignoring case.** If `Alice` and `alice` were accepted as different people, a person
> could hardly tell the two apart on the screen. Uniqueness exists to prevent that.

> **Why spaces are not accepted.** The name is one field of the `FAIL` line
> ([`architecture.md`](architecture.md) 3.5 Startup Inputs) and can be the value of a log
> `<name>=<value>`. A space would break both rules.

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

The subset is all of 3.3 HTTP Subset below. **HTTP features not listed there are not supported.**
Whether one is rejected when received or ignored as a header outside the table is decided by the
rules table in 3.3.

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
| Headers | Names are case-insensitive. The four in the format above (`Host`, `Content-Type`, `Content-Length`, `Connection`) and `Transfer-Encoding` are treated as the table headers. Other headers are ignored (ignored even if they appear twice). **A table header is rejected if it appears twice.** For a duplicate `Content-Length`, see below |
| Header line syntax | `name:value`. A line with no colon, and a line with a space or a tab between the name and the colon, are rejected (RFC 9112 5.1). A single space right after the colon is removed as a separator, and any other whitespace is part of the value |
| Body | One UTF-8 JSON **object**. Arrays and scalars are rejected. Unknown keys are ignored (forward compatibility). Duplicate keys within one object are rejected, including in nested objects. `NaN` and `Infinity` are not JSON and are rejected |
| Size | Request line plus headers total `MAX_HEADER_BYTES` (2048), body `MAX_BODY_BYTES` (4096). The former is counted from the first byte of the request line to the CRLF of the empty line that ends the headers |
| Encoding | Strings are UTF-8. Integers are JSON numbers with no decimal point. `uint32` fields are 0 or more and 4294967295 or less |
| Compression, chunking, 100-continue, upgrade, authentication headers | Not supported. `Transfer-Encoding` is rejected, and `Content-Encoding`, `Expect`, `Upgrade`, and `Authorization` are ignored as headers outside the table |
| Byte-exact match | The request line is `POST <path> HTTP/1.1` and the separator is a single space. Two spaces, a tab, the absolute form (`POST http://host/v1/op`), a trailing `/`, and percent encoding (`%5F`) are all rejected. The path is case-sensitive. The grammar is below the table |
| `Content-Length` value | Only `^[0-9]+$` is accepted. `+17`, `1_7`, surrounding whitespace, and full-width digits are rejected. Do not leave this to the language's integer conversion function. Python `int()` accepts the first three |
| Header folding (obs-fold) | Reject a header line that starts with a space or a tab. Do not join it to the previous line |
| `Host`, `Content-Type` | **Not checked.** The client always sends them (the format above) but the server does not look at the values. There is no virtual hosting and no content negotiation. The `<server address>` the client sends is the `--server` name or IPv4 joined with the port by `:` ([`architecture.md`](architecture.md) 3.5 Startup Inputs) |
| Body longer than `Content-Length` | Read only the leading `Content-Length` bytes, respond, then close the connection. The remaining bytes are not read. One request per connection, so those bytes cannot become a second request |

**Request line grammar.** The bytes before the first CRLF have to fully match
`([!-~]+) ([!-~]+) HTTP/1\.1`. The second group (the target) starts with `/`, does not end with `/`,
and does not contain `%`. Otherwise it is `400`. Then, if the method is not exactly `POST` it is
`405`, and if the target is not exactly `/v1/<op>` it is `404`. `<op>` is one of the five operations
of section 4 and is case-sensitive.

**A duplicate `Content-Length` is rejected in particular.** Rejecting a message with several
`Content-Length` values that differ is a rule of RFC 9112 section 6.3, and an upstream and
downstream intermediary choosing different values is one form of request smuggling. Reject even
if the values are equal. There is no reason to distinguish.

**Server parsing case table.** It is `CASES` in [`test_http.py`](../../control-server/tests/test_http.py).
The decision function has to pass this table. Of the rules above, those that the rows of that table
do not cover are checked by `RULE_CASES` in the same file. Whether a parse failure response echoes
the request content back is checked by `ECHO_CASES`, and requests that arrive in several pieces and
the 3.4 Server-Side Time Limits are checked by `STREAM_CASES`.

If `Content-Length` exceeds `MAX_BODY_BYTES`, the server sends `413` without reading the body, then
closes.

**Check order.** Deciding the response to a compound defect needs an order. Read from the top.

1. Byte format of the request line and `HTTP/1.1` -> `400`
2. Method -> `405`
3. Path (the `/v1/` prefix and the operation name, no query string) -> `404`
4. Total header size -> `431`. If the request line does not end within the cap, 1-3 cannot be
   decided, so it is `431` right away
5. Header syntax (folding, a duplicate of a header that is in the table) -> `400`
6. Presence of `Transfer-Encoding` -> `400`
7. Presence of `Content-Length` -> `411`, value format -> `400`, over the cap -> `413`
8. Body read and time limit (3.4) -> close without a response. If the peer closes before the whole
   body arrives, also close without a response. In that case the counter is not incremented
9. Is the body a UTF-8 JSON object -> `400`

The body of `400`-class errors is also the error envelope of 4.1 Common Envelope. **A parse
failure response does not echo the request content.**

> **Why.** Otherwise the server becomes a place that reflects arbitrary bytes.

### 3.4 Server-Side Time Limits

From accepting the connection to reading one whole request is `SERVER_READ_TIMEOUT_S` (5 seconds).
Beyond that, close without a response and increment `http_read_timeout`. The same 5 seconds
applies to sending the response. If sending does not finish within that time, the remaining bytes
are discarded and the connection is aborted. There is no counter.

**A path that answers without reading the whole request does not close right away.** This applies to
`503`, `413`, `431`, and other parse failures. After sending the response, the server first closes
the write side, then reads and discards the remaining input up to `LINGER_S` (1 second) or
`LINGER_MAX_BYTES` (8192), and then closes.

> **Why.** Closing while unread bytes remain in the receive buffer makes the OS send an RST, and the
> client can get a connection reset error before it reads the response that has already arrived.
> RFC 9112 section 9.6 deals with the same problem. Measured on the development machine (Windows).
> Without this handling, sending `503` and `413` 20 times each got a connection reset instead of the
> response all 20 times, and with this handling the response arrived all 20 times.

- This wait after a parse failure counts toward the requests in flight (`MAX_INFLIGHT` of 7.2) and
  holds that slot for up to `LINGER_S` more. The wait after `503` does not hold a slot. That
  connection never got a slot in the first place
- Connections with a normal response, and connections aborted on a send timeout, have no such wait

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

1. Does the status line start with `HTTP/1.1 <3-digit number>`? What follows the number has to be
   a space or the end of the line. If not, it is a transport error that leads to
   `CONTROL_PLANE_EXCHANGE_FAILED` (8.3)
2. Do the status line and headers together end within `MAX_HEADER_BYTES`? If not, transport error
3. Do the header lines follow the header line syntax of 3.3 HTTP Subset? Line endings are CRLF only,
   and folding, whitespace before the colon, and a line with no colon are transport errors. If
   `Transfer-Encoding` is present, it is a transport error regardless of its value
4. One `Content-Length` header. If missing, present twice, not matching `^[0-9]+$`, or larger than
   `MAX_BODY_BYTES`, transport error
5. Read the body to that length and parse it as a JSON object. On failure, transport error. Nesting
   is allowed up to `CLIENT_JSON_MAX_DEPTH`, and duplicate keys are a failure (the body rule of 3.3)
6. Split success and error by the `ok` field (4.1). **Not by the HTTP status code**
   - The status code is for the person holding `curl`; the program looks at the body
   - An implementation where the two disagree has to be caught in tests. But if the client looked
     at both, which one to trust when they disagree would differ per implementation
7. If `ok: true`, does it match the types of that operation's response table (section 4)? A missing
   field or a wrong type is a transport error. Unknown keys are ignored. For `name` in a `peers`
   element, the 2.7 format is checked in addition to the type. A wrong format is a transport error
8. If `ok: false`, is `error` a code in the 4.1 table? A code not in the table is a definite error.
   The transient errors of 8.3 are closed to the two `internal` and `unavailable`

Once `Content-Length` bytes are read, the client stops there. It does not wait for the server to
close the connection.

> **Why narrower than 3.3.** Reading the response head leniently can make the client read
> `Content-Length` differently from the server. Every response the server sends passes all of these
> checks.

> **Why a success with wrong types is a transient error.** It cannot be told apart from a response
> broken on the way. If retrying gives the same answer, the retry cap (8.3) ends it. A code not in
> the table, on the other hand, is an answer the server sent with meaning, so sending again gives
> the same answer.

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
| `name_taken` | 409 | The room has a peer with the same name (2.7, ignoring case) | Same |
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
| `name` | string | Yes | The host's display name (2.7) |

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
room is created. The basis for the decision is the `NONCE#` item (6.3). For a repeated request, `name`
is checked only for format and is not compared with the stored value. The same goes for 4.3
`join_room`.

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
| `name` | string | Yes. 2.7 format. The display name of this participant |

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

- If the `PEER#` that `NONCE#` points to does not exist, what was first issued cannot be returned.
  This cannot be decided, and it is `internal`. Reclaim deletes `PEER#` and `NONCE#` together (4.6),
  so this does not happen on a normal path
- If `NONCE#` exists but the room was deleted first by TTL, it is (absent) in 5.1 Room, so
  `room_not_found`
- `NONCE#` does not distinguish operations. If the nonce of `create_room` is given to `join_room`
  for the same room, the stored record (that peer's) is returned. The reverse is the same. For a
  different room it is `bad_request` (cancellation reason 1 of 6.3)

> **Why.** A client that lost the response and retries with a changed nonce fills the room by
> itself. That is why 2.4 `client_nonce` fixed one value per join.

**`room_full` is decided by the pool walk alone.** It is this error only when one pass over the
pool finds no free address. Whether the room is `ready` is not considered (5.1).

> **Why.** Ready is decided per pair, so a room that already has a connected pair can still have
> a free slot. The truth about a slot is the `VIP#` claim and the peer count is derived from it.
> Deciding it in two places makes them disagree right after a reclaim.

**`name_taken` is decided by the `NAME#` claim result alone.** It is not decided by scanning the
room's names with a prior read. If two joins with the same name come at the same time, only one
conditional write succeeds (6.3). A name collision does not move to the next address in the pool.
Changing the address leaves the name colliding just the same.

**Errors.** `room_not_found`, `room_expired`, `room_full`, `name_taken`. The common four (4.1) are not
listed separately.

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
(section 14 contract 5). The client also applies the same rules again to the list it receives, and adds the
own-subnet row that only the client applies.
Regardless of trusting the server, both sides do the format check.
That section owns the rules themselves. The case table that expands those rules **for server input**
is `CANDIDATE_CASES` in [`test_candidates.py`](../../control-server/tests/test_candidates.py). The two
results of the rejection policy below are checked by `POLICY_CASES` in the same file. The processing
order and band paragraphs below are checked by `ORDER_CASES` and `BAND_CASES`, and other rules outside
the table by `CANDIDATE_EXTRA_CASES`.

Three of the case table's results have to be read as rules.

- The candidate list rejects loopback. This differs from receive source hygiene (`protocol.md`
  section 7)
- 9 candidates is `bad_request` for the whole request. The "drop the excess" of 10.1 is a client
  rule for a **received** list; the server enforces the cap on the sender
- A duplicate is not a hygiene rejection, so it is not counted in `rejected`. Therefore
  `accepted + rejected` can be smaller than the number sent

**Do not use a lenient parser of the `inet_aton` kind.** On this repo's Windows Python 3.11
version, `socket.inet_aton` accepted `10.0.5` as `10.0.0.5` and `010.0.0.5` as `8.0.0.5` (octal).

**Rejection policy. There are only two results, and the `result` field of the case table
`CANDIDATE_CASES` states which one each row takes.**

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

**There are three processing steps.** First decide the whole request by the type check, then apply
hygiene rejection, then remove duplicates from what remains. This is the same order as the rules
table of `protocol.md` 10.1 Candidate Collection and Hygiene.

- If the same hygiene-rejected candidate comes twice, `rejected` is 2. Rejection comes before
  deduplication
- On a duplicate, the element that came first is kept. Its `kind` is also kept
- Unknown keys inside a candidate element are ignored. Only `ip`, `port`, and `kind` are stored

**The bands the server decides are exactly the list of 10.1 minus the own-subnet row.** Broadcast is only `255.255.255.255`,
and unspecified is only `0.0.0.0`. Loopback is the whole of `127.0.0.0/8`.

- The server does not receive the peer's prefix length (10.1), so it cannot decide a subnet's
  directed broadcast. `10.0.0.255` is stored
- The rest of `0.0.0.0/8` and `240.0.0.0/4` are not in the 10.1 list, so they are stored

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
response of 4.6 `host_report`. It is not an error if the host calls it. Like `peers` in the 4.6
`host_report` response, it returns every peer except self.

- The order of the `peers` elements is not defined. The client does not rely on the order
- If a player calls it and the room has no `PEER#` for the host, it cannot be decided and is `internal`

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
| `name` | string | The display name that peer registered (2.7). Used for display only |
| `ready` | boolean | True if the `PAIR#` item of this pair exists |
| `punch_delay_ms` | integer | The value written to that pair. Present only when `ready` is true |
| `elapsed_since_ready_ms` | integer 0 or more | From the moment that pair's ready was written to the moment this response is built. Present only when `ready` is true. How it is computed is in 7.4 |
| `candidates` | array | Same element type as 4.4 `register_candidate`. In stored order. Present only when `ready` is true |

**The field set of an element is decided by `ready`.** With no `PAIR#`, it is the four fields
`peer_id`, `virtual_ip`, `name`, `ready: false`. If it exists, `punch_delay_ms`, `elapsed_since_ready_ms`,
and `candidates` are added. The case table is `FIELD_SETS` in
[`test_get_peers.py`](../../control-server/tests/test_get_peers.py).

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
2. For each `peer_id` in `departed`, delete the `PEER#`, that peer's `VIP#` and `NAME#`, the `PAIR#`
   items that peer is part of, and that peer's `NONCE#` (6.3). Then delete the `PEER#`, `VIP#`, and
   `NAME#` of the server reclaim targets too. "Server reclaim" below decides the targets. Server reclaim does not
   decide again on a peer in `departed`. Reclaiming that peer belongs to the host, and `by` in the
   log is `host`
3. For each `peer_id` in `confirm`, if that peer and the caller **both have 1 or more candidates**
   and the `PAIR#` of that pair is absent, create it with a conditional write. The item carries
   `ready_at_*` and `punch_delay_ms = PUNCH_DELAY_MS` (6.3)
4. Renew `ROOM.expires_at_ms` to `now + ROOM_LEASE_S * 1000`
5. Read with `Query(pk, ConsistentRead=true)` and build the response

**Server reclaim.** The targets are the peers of the room read in step 1 that satisfy all of the
following. Deleting one also deletes that peer's `NONCE#`. The key is known from the `client_nonce`
of `PEER#`. The `NAME#` to delete is the item among the room items read in step 1 whose `peer_id`
is that peer. The key is not built from the `name` of `PEER#`. Peers deleted in step 2 are put into `released`.

- Not the host
- Candidates are empty. It is `joined` of 5.3 Peer
- `joined_at_ms + JOIN_REGISTER_GRACE_S * 1000 <= now`

**A peer that cannot be decided is skipped, not deleted.** Each peer is checked in the order below,
and once a step reaches a conclusion the later steps are not checked. On a skip, the
`peer.reclaim_skipped` log line of 7.5 is left.

| Order | What is checked | Result |
|:--:|---------|------|
| 1 | Is `peer_id` an integer | If not, skip. `reason=peer_id_unreadable` |
| 2 | Is it the host | If it is the host, it stays |
| 3 | `candidates` | If the attribute is missing, it is empty (the same as the condition of 6.3). If it is not a list, skip. `reason=candidates_unreadable`. If it is not empty, it stays |
| 4 | Is `joined_at_ms` an integer | If not, skip. `reason=joined_at_unreadable` |
| 5 | Has the grace passed | The third condition above. If it has not passed, it stays |
| 6 | Can the keys to delete be built | If `virtual_ip` and `client_nonce` are not strings, skip. `reason=keys_unreadable`. If they are strings, delete |

> **Why skip.** Turning "cannot decide" into reclaim would let one corrupted item delete a live
> peer. Ending the whole request as `internal` would block even that room's lease renewal.

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

**If the caller's own `peer_id` is in `departed` or `confirm`, that element is skipped.** It is
neither deleted nor written, and it is not put into `released` or `confirmed`. Processing it as is
would make the host delete its own slot, and a confirmation would put two ConditionChecks on the
same item, so DynamoDB would reject the whole request.

**If the re-read of step 5 finds an unreadable `PEER#`, it is `internal`.** This is the type rule of
6.3 Items. The renewal of step 4 has already finished, so the lease lives. While that item remains,
the host gets `internal` every cycle. It is also `internal` when the target of `departed` or
`confirm` is an unreadable `PEER#`, and when the caller's `PEER#` cannot be read. The one exception
is the `client_nonce` of a `departed` target. If only that cannot be read, everything except
`NONCE#` is deleted, per the reclaim row of 6.3 Items.

**The `NAME#` that reclaim deletes is found by `peer_id`.** Among the room items read in step 1,
every item that is a `NAME#` and whose `peer_id` is the target is deleted. If there is none, the
deletion goes ahead without a `NAME#`.

> **Why not find it by the `name` of `PEER#`.** If that value cannot be read, only `PEER#` would be
> deleted while `NAME#` stays, and renewal would keep pushing the `ttl` of the remaining `NAME#`, so
> it would block that name until the room ends.

**Step 2 comes before step 3.** If one request carries the same `peer_id` in both `departed` and
`confirm`, the reclaim wins. Writing an ended session as ready would fill that slot again.

**Step 4 comes before step 5.** The `expires_in_s` of the response has to be the value after
renewal so that the host reads its own lease right away.

**That step 5 comes last is the mutation test of this section.** An implementation that builds the
response from a read taken before the writes of steps 1 to 4 includes the peer it just reclaimed
in `peers`.

**Confirmation race case table.** It is `CONFIRM_RACE` in
[`test_host_report.py`](../../control-server/tests/test_host_report.py). `H` is the host's
`host_report` and `P` is a player's `register_candidate`. Every row needs the store to run. An
implementation that writes step 3 with no condition is caught on the last row
(`unconditional-write-impl`). The precondition of step 3 itself is checked without the store by
`CONFIRM_CORE` in the same file.

**Server reclaim case table.** It is `RECLAIM_CASES` in
[`test_host_report.py`](../../control-server/tests/test_host_report.py). The read rows of the decision
order table above are checked by `RECLAIM_READ` in the same file. Rows that need the store to run are
marked `needs="store"`.

**If a reclaimed peer sends `join_room` again with the same nonce, it is a new join.** `NONCE#` was
deleted together, so it is not the idempotent response of 4.3 `join_room`. Because of the first term
of "Why 90 seconds" above, a normal client's retry finishes before the reclaim.

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
| `room_expired`, `room_not_found`, `unauthorized`, `bad_request`, other definite errors | The room has ended on the server, or sending the same request again gets the same answer | Emit the `FAIL CONTROL_PLANE_EXCHANGE_FAILED` line once and stop `host_report`. Even when a session ends, do not make the immediate call above. Leave existing sessions as they are. When all remaining sessions end, go to the lobby. If there is no session, go right away |
| `rate_limited` | The room may be alive. Another source behind the same public IP may have burned the budget (6.4) | Do not emit `FAIL`; call again on the next cycle |
| `internal`, `unavailable`, transport error | Transient error. A success response with wrong types is also a transport error (3.5) | Call again on the next cycle |

- After the room ends, the tunnels of the remaining sessions keep going ([`spec.md`](spec.md)
  NFR-3). What is lost is only new joins
- The lease can expire while `rate_limited` continues. The budget check comes before the store
  (6.4), so the responses in that window stay `rate_limited`. The first request processed once the
  budget refills gets `room_expired`, or `room_not_found` if TTL has already deleted it, and moves to
  the first row
- Other definite errors are all definite errors other than the four of the first row. They are the
  remaining codes of the 4.1 table, codes not in the 4.1 table (3.5), and the response's `peers`
  containing the caller's own `peer_id` (step 8 of 8.4)
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

**Expiry decision.** `now` is the server wall clock in milliseconds, `expires_at_ms` is the stored
value. The boundary `now == expires_at_ms` is on the expired side. A room with `expires_in_s` of 0
cannot be joined. If `expires_at_ms` is missing or not an integer, it is **`internal`**. Do not turn
"cannot decide" into pass. The creation path always writes this field, so if it is missing the store
is corrupted.

The case table is `EXPIRY` in [`test_room_state.py`](../../control-server/tests/test_room_state.py),
and values that are not integers are checked by `EXPIRY_UNREADABLE`.
Rows that keep the state table above are checked by `STATES` in the same file, and the value of
`expires_in_s` below by `EXPIRES_IN`.

`expires_in_s` is `max(0, ceil((expires_at_ms - now) / 1000))`. The client uses this value only
for display and not in any decision.

- **In a live room it is 1 or more.** Even 1ms left makes the ceiling 1
- 0 means expired. This says the same thing as the boundary of the expiry decision above

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

Items belonging to a room (`ROOM`, `PEER#`, `VIP#`, `NAME#`, `PAIR#`) share the same partition key, so the
whole state of one room is read with a single `Query(pk = room_id, ConsistentRead=true)`. The item
count is at most `1 + MAX_PEERS × 3 + (MAX_PEERS - 1)` (one `ROOM`, a `PEER#`, a `VIP#`, and a
`NAME#` per peer, and one `PAIR#` per host-and-player pair). In v1 that is at most 20.

**The `NONCE#` item has a different partition key.**

> **Why.** The idempotency lookup of `create_room` has to find it by nonce alone, at a point where
> the `room_id` is not yet known.

### 6.3 Items

| `sk` | Attributes | Meaning |
|------|------|-----|
| `ROOM` | `created_at_ms`, `expires_at_ms`, `host_peer_id`, `ttl` | Room. The first `expires_at_ms` is `now + ROOM_LEASE_S * 1000` of `create_room`, and `host_report` pushes it (4.6) |
| `PAIR#<lo>-<hi>` | `ready_at_wall_ms`, `ready_at_mono_ns`, `ready_boot_id`, `punch_delay_ms`, `ttl` | Ready state of a pair. `lo` and `hi` are the two `peer_id`s placed in ascending numeric order and written in decimal. `9` comes before `10`. 7.4 Clocks uses the three values |
| `PEER#<peer_id>` | `peer_id`, `peer_token`, `virtual_ip`, `name`, `candidates` (list), `client_nonce`, `joined_at_ms`, `ttl` | Peer. `peer_id` is written in decimal in `sk`. `peer_token` is the raw value (2.3). `name` is as registered (2.7). `candidates` is not written before the first `register_candidate` |
| `VIP#<virtual_ip>` | `peer_id`, `ttl` | Virtual IP claim marker. `<virtual_ip>` is dotted-decimal |
| `NAME#<name key>` | `peer_id`, `ttl` | Display name claim marker. `<name key>` is the name converted to ASCII lowercase (2.7). Reclaim finds this item by `peer_id` (4.6) |
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
| `create_room` | Transaction: `ROOM` put, `PEER#host` put, `VIP#10.100.0.1` put, `NAME#<host name key>` put, `NONCE#` put | **All five puts carry `attribute_not_exists(pk)`.** A `ROOM` failure is a `room_id` collision → redraw (2.1). A `NONCE#` failure means the same nonce arrived concurrently, so read again and return that result. `PEER#`, `VIP#`, and `NAME#` cannot exist without `ROOM`, so a failure there means the store deleted only part of an earlier room's items. That is `internal` and there is no redraw |
| `join_room` new join | Per pool address, a transaction: `ROOM` **ConditionCheck**, `VIP#<ip>` put, `NAME#<name key>` put, `PEER#<peer_id>` put, `NONCE#` put | `attribute_exists(pk) AND expires_at_ms > :now` on `ROOM`. This blocks the race where the room expires or is deleted between the prior read and the write. `attribute_not_exists(pk)` on `VIP#`, `NAME#`, `PEER#`, and `NONCE#` each. Failure handling is in the cancellation reason table below |
| `register_candidate` | `PEER#` update: `candidates = :list` | `attribute_exists(pk) AND peer_token = :t`. Failure is `unauthorized`. It is the case where that peer was reclaimed after the token check (4.6 server reclaim) |
| `host_report` reclaim | Per target, a transaction: `PEER#<target>` delete, `VIP#<that peer's virtual_ip>` delete, delete the `NAME#` whose `peer_id` is that peer, delete the `PAIR#` items that peer is part of, `NONCE#<that peer's client_nonce>` delete | **`attribute_exists(pk)` on `PEER#` only.** If that condition fails, the target was already gone and is not put into `released` (4.6). The rest are unconditional deletes. A peer that left before confirmation has no `PAIR#` at all, and conditioning on it would cancel the whole reclaim. If `client_nonce` cannot be read, everything except `NONCE#` is deleted |
| `host_report` server reclaim | Per target, a transaction: `PEER#<target>` delete, `VIP#<that peer's virtual_ip>` delete, delete the `NAME#` whose `peer_id` is that peer, `NONCE#<that peer's client_nonce>` delete | `attribute_exists(pk) AND (attribute_not_exists(candidates) OR size(candidates) = :zero) AND joined_at_ms <= :cutoff` on `PEER#`. `:cutoff` is `now - JOIN_REGISTER_GRACE_S * 1000`. If the condition fails, the peer registered in the meantime or was already gone, so it is not put into `released`. A peer that had no candidates cannot have a `PAIR#`, so none is deleted |
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
| 3 | `NAME#` (`join_room`) | `name_taken`. Do not move to the next address (4.3) |
| 4 | `VIP#` | Next address. When the pool is exhausted, `room_full` |
| 5 | `PEER#` | Redraw `peer_id`. The cap is `MAX_PEER_ID_ATTEMPTS` |
| 6 | `ROOM` put (`create_room`) | Redraw `room_id`. The cap is `MAX_ROOM_ID_ATTEMPTS` |
| - | A reason that is not a condition failure (`TransactionConflict`, throttling) | `internal`. The client retries by the rules of 8.3 Error Classification and Retry |

The reason `NONCE#` is placed at row 1 is in "Why It Was Decided This Way" below.

- **`create_room` checks row 6 right after row 1.** If the `room_id` collides with a live room, the
  `ROOM` put fails together with `VIP#10.100.0.1` and `PEER#`, and `NAME#` can fail too, but
  `create_room` has no next address, and the peer whose name collided is a peer of another room. If
  the `ROOM` put succeeded but only `PEER#`, `VIP#`, or `NAME#` failed, it is `internal` per the write
  table above
- **`NAME#` comes before `VIP#`.** If both fail together in the same transaction, the name collided.
  Looking at `VIP#` first would move to the next address, repeat attempts whose name keeps colliding
  as many times as the pool size, and then return `room_full`
- When condition failures and reasons that are not condition failures are mixed, it is row 1 only
  when there is a `NONCE#` condition failure. Otherwise, if there is even one reason that is not a
  condition failure or an unknown reason, it is `internal`

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

**`store.py` normalizes the types of values the store returns before passing them on.** `boto3`
returns numbers as `Decimal`, so integer attributes are converted to `int`. If an attribute of the
item table above is missing or has a different type, it cannot be decided and it is `internal`. There
are three exceptions.

- If `candidates` of `PEER#` is missing, it is an empty list. It is a peer that has not registered
  candidates yet (`joined` of 5.3 Peer). This matches the delete condition of 4.6 server reclaim
  treating `attribute_not_exists(candidates)` as empty
- The decision of 4.6 server reclaim does not end the request and skips only that peer
- In the `departed` reclaim of 4.6 `host_report`, if only the target's `client_nonce` cannot be
  read, everything except `NONCE#` is deleted

If a `sk` not in the item table above appears in the room partition, it cannot be decided and it is
`internal`.

All of them are `ConsistentRead=true` (Storage 1). **There is no form that finds a peer by token
alone.** Every authenticated operation also receives the `peer_id`, so it reads by that key
directly. Finding by token would require reading every peer and comparing.

### 6.4 Rate Limit

**A budget on failure responses per source IP.** What is counted is three: `room_not_found`,
`room_expired`, `unauthorized`. Success and `bad_request` are not counted.

> **Why.** `bad_request` is a format error unrelated to guessing, and counting success would catch
> normal polling.

**`room_full` and `name_taken` are not counted either.** Both go out only to requests that gave a
correct room code. What the budget tries to slow down is room code guessing, and these two are
answers after the guessing has already succeeded. Someone who knows the room code burning storage
write capacity with these two is the same kind of thing as the `create_room` abuse of 1.2 Trust
Assumptions, and v1 accepts it. That `name_taken` reveals that the room has that name is also
accepted. Names are visible to room members anyway.

| Item | Value |
|------|-----|
| Budget | `RATE_LIMIT_BUCKET` (10) tokens per source IP. One is spent each time one of the three errors above goes out |
| Refill | `RATE_LIMIT_REFILL_PER_MIN` (10) per minute. **It is continuous.** One token is added every 6 seconds from the last refill time, capped at `RATE_LIMIT_BUCKET` (10). For the reference time rule, see below |
| When exhausted | **The request is not processed** and `rate_limited` is returned. The store is not read. That way the limit also stops storage cost |
| Table | In memory. Source IP → (tokens, last refill time). Cap `MAX_RATE_ENTRIES` (4096) |
| Entry creation | **Create it only when a token is spent.** A lookup alone does not create one. A source with no entry is the same as a full bucket |
| Entry removal | **Delete the entry when refill brings the tokens to the cap.** A full entry decides the same as no entry, so there is no reason to hold it. Refill is computed when that source comes again, so the entry of a source that does not come again is deleted when the table is full. If a new source is about to spend a token and the table is full, the whole table is scanned once and the entries that reached the cap are deleted |
| When the table is full | **New sources are passed through without limiting.** And the `rate_table_full` counter is incremented. Evicting the oldest entry would let an attacker with many IPs fill the table and evict the real guesser's entry. With pass-through, the attacker gains nothing by filling the table. The price is that new sources have no limit while the table is full, and if this value rises, the cap is revisited |
| When the budget is checked | **After** the request is parsed, before the store is read |

**The reference for refill is the time refill happened.** Neither spending a token nor a
`rate_limited` response changes that time.

- If spending moved the reference, refill for a source that keeps spending would be delayed
  forever
- If `rate_limited` moved it, an attacker could extend the cooldown permanently just by knocking,
  and block a legitimate user of the same source forever. This is the same reason
  [`protocol.md`](protocol.md) 9.5 Renegotiation put the same rule on the renegotiation cooldown
- The reference time advances in steps of 6 seconds. Looking at it 11 seconds after the reference,
  one token is added and the reference moves 6 seconds later. The remaining 5 seconds carry over to
  the next refill

**Tokens do not go below 0.** If two requests in flight pass the budget check with the same last
token and both end in a counted error, the second spend stops at 0.

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

**Case table.** It is `CASES` in [`test_rate_limit.py`](../../control-server/tests/test_rate_limit.py).
The row field `order` is the order number, and the rows run one after another from the same source.
Of the rules above, those that the rows of that table do not cover are checked by `RULE_CASES` in the
same file.

Row 12 of the table (`row-12`) says that a normal `get_peers` from a source that has used up its
budget is also blocked with `rate_limited`. This is because the budget is per source. What that means
is the following. **When two users behind the same NAT share a public IP, one person's
guessing blocks the other person's normal requests.** A campus network is like that.

- A normal client receives a failure response once or twice per launch at most, so a budget of 10
  covers that case. But if the person next door burns the budget, it is blocked. This is a limit
  v1 accepts
- The alternative (not counting requests that carry a token) leaves `unauthorized` guessing open,
  so it is not used

### 6.5 TTL

| Item | `ttl` value |
|------|----------|
| All items of a room (`ROOM`, `PEER#`, `VIP#`, `NAME#`, `PAIR#`, `NONCE#`) | `floor(expires_at_ms / 1000) + STORAGE_GRACE_S`. `expires_at_ms` is the value at the time that item is written |

After a renewal finishes, the items of the room partition share the same `ttl`. One day after the
room expires, DynamoDB deletes them.

**Renewing the lease pushes the `ttl` of every item of the room partition (`ROOM`, `PEER#`, `VIP#`,
`NAME#`, `PAIR#`).** `NONCE#` is not pushed. It is in a different partition, so it does not appear in the
renewal's `Query`, and its job ends when the retries of a request that lost its response end. So the
`NONCE#` of a long-lived room is deleted first, a little over a day after that peer came in. A request
that comes with the same nonce after that is a new join. Renewing only `ROOM` would
delete the `PEER#`, `VIP#`, `NAME#` and `PAIR#` items of a long-lived room first, leaving a live room with
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

The modules are under the `control-server/controlplane/` package.

| Module | Responsibility | External dependency |
|------|------|-----------|
| `server.py` | Accept loop, 3.3 HTTP parsing and response serialization, operation dispatch, time limits, `MAX_INFLIGHT`, rate limit table (6.4) | `asyncio` |
| `ops.py` | The five operations of section 4. Request validation, state decisions (section 5), response assembly | None |
| `ids.py` | Generation of `room_id`, `peer_id`, `peer_token`, `room_id` normalization (2.1), format checks of `client_nonce` and `peer_token` | `secrets` |
| `candidates.py` | 4.4 hygiene. Candidate format checks and the 10.1 rules | `ipaddress` is not used. See below |
| `clock.py` | The three values of 7.4 Clocks | `time`, `/proc/sys/kernel/random/boot_id` |
| `store.py` | All `boto3` calls. The condition expressions of 6.3 Items live **only in this file** | `boto3` |
| `errors.py` | The error codes and HTTP statuses of 4.1 Common Envelope | None |
| `constants.py` | 2.6 Constants | None |
| `log.py` | 7.5 log lines. Takes events and fields as an allowlist | None |

**Candidates are not judged with the `ipaddress` module.**

- On this repo's Python 3.11 version, the observed values are that
  `ip_address('127.0.0.1').is_private`
  and `ip_address('192.0.2.1').is_private` are true, and `ip_address('224.0.0.1').is_global` is
  also true. This can differ by version, so no version is allowed to delegate the decision to those
  predicates
- The decisions of 4.4 hygiene parse the four octets directly and branch on the leading octet and
  range. Parsing is a full match of `[0-9]{1,3}(\.[0-9]{1,3}){3}` with each octet 0~255 and
  **leading zeros rejected**
- Do not write the digit range as `\d`. Python's `\d` also accepts digits outside ASCII. Do not
  anchor the end with `$`. `$` also matches before a trailing newline
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
        try:
            req = await wait_for(read_request(reader), SERVER_READ_TIMEOUT_S)
        except OpError as e:                              # 4xx of 3.3 HTTP Subset
            respond(e.status, e.envelope()); return
        except TimeoutError:
            counters.http_read_timeout += 1; return       # no response (3.4)
        except EOF:
            return                                        # closed before fully received. No response, no counter
        if rate.exhausted(src):                           # 6.4. After parsing, before the store
            counters.rate_limited += 1
            respond(429, rate_limited); return
        try:
            fields = await to_thread(ops.dispatch, req)   # boto3 is blocking. A dedicated pool set as the default executor
        except OpError as e:
            if e.code in {room_not_found, room_expired, unauthorized}:
                rate.spend(src)                           # the three that 6.4 Rate Limit counts
            respond(e.status, e.envelope()); return
        respond(200, {ok: true, **fields})
    except Exception:
        counters.internal_error += 1
        respond(500, internal)                            # do not put the exception text in the body
    finally:
        inflight -= 1; close
```

**The timeout applies only to reading the request.** A `TimeoutError` raised inside an operation is
not a read timeout. Python's `socket.timeout` is also a `TimeoutError`, so applying it to the whole
block would wrongly handle a store-side timeout as a close without a response. In that case the
catch-all handler answers with `internal`.

**The contract of `ops.dispatch`.** On success it returns a dict of the per-operation fields, and the
server adds `ok: true`. On error it raises an exception that carries the error code of 4.1 Common
Envelope. The HTTP status of success is 200.

**Store errors become `internal` by two paths.** A transaction cancellation is decided by `store.py`
per the 6.3 cancellation reason table and turned into an error code. Other `boto3` errors
(throttling, connection failure, timeout) come up as exceptions, the catch-all handler answers, and
`internal_error` is incremented. The response is the same and only the counter differs.

**`boto3` calls are treated as blocking.** They are sent to a dedicated thread pool of
`MAX_INFLIGHT` threads. The server creates that pool and sets it as the event loop's default
executor. To avoid waiting for in-flight calls at shutdown (7.6 shutdown), the process has to hold
that pool itself. Shutdown emits the counters, flushes output, and ends the process without waiting
for the pool.

- Calling them directly on the event loop thread stalls accepting and parsing of other connections
  for the duration of the DynamoDB response delay
- The concurrency cap `MAX_INFLIGHT` (32) stops that thread pool from growing without bound and
  stops waiting requests from piling up and eating memory when the store slows down. Beyond the cap
  it is `unavailable`, and the client retries by the rules of 8.3 Error Classification and Retry

**Connections being rejected have a cap too.** A connection rejected with `503` because there is no
slot does not count toward the requests in flight, but it remains until it sends the response and
finishes the wait of 3.4 Server-Side Time Limits. When there are `MAX_REJECTING` (64) such
connections, a new connection is dropped right away with no response and `unavailable` is
incremented.

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
6. Room expiry (5.1 Room expiry decision)            -> expired: room_expired
7. Token check (only operations that need it)        -> failure: unauthorized
   host_report also checks that the caller is the host (4.6)
8. Operation body (conditional writes)
9. Response assembly
```

**The time is read once at the start of the request.** The expiry, reclaim, and renewal decisions all
use the same `now`. The exceptions are the three `ready_at_*` values of `PAIR#` and the
`elapsed_since_ready_ms` of the response. The monotonic clock and the wall clock have to point at the
same moment, so they are read at the moment of writing or computing (7.4).

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

**Computation path.** The case table is `CLOCK_CASES` in
[`test_clock.py`](../../control-server/tests/test_clock.py), and rules outside the table are
`CLOCK_EXTRA_CASES`.

- Within the same boot, compute with the monotonic clock. This holds even if the process restarts.
  `CLOCK_MONOTONIC` is independent of the process
- If the instance rebooted, fall back to the wall clock and increment `elapsed_wall_fallback`. It can
  be off by the NTP step. **In this case the error bound of `protocol.md` 10.2 Rendezvous is not
  guaranteed.** Section 14 contract 4 delegates that exception to this document. A reboot and a time
  adjustment have to overlap within the few seconds between ready and the polling response, so it is
  rare. It is observed by the counter
- If the wall clock fallback gives a negative value, it is 0. The client waits the full
  `punch_delay_ms`
- If the `boot_id` file is missing, cannot be read, or is empty, a random value drawn at process
  start is used as `boot_id`. Then every process restart drops to the wall clock fallback. Local tests
  that are not on Linux are like that. The deployment target is Linux

The `deploy_only` rows of `CLOCK_CASES` check only up to the pure function's decision of whether it
is the same boot. Whether the monotonic clock continues across a restart is checked on the deployment
instance by the "Clock and logs" group of the [`roadmap.md`](roadmap.md) Phase 3 verification.

**The two premises were confirmed on the deployment instance.** Two things were checked in the 7.6
deployment environment.

- The implementation of `time.get_clock_info('monotonic')` is `clock_gettime(CLOCK_MONOTONIC)` and
  `adjustable=False`
- `boot_id` was read before and after a reboot, and the two values differed

The development machine is Windows, so the same call returns `GetTickCount64()`. Local tests are the
last item of the computation path above. If the deployment image changes, do the same two checks again. If
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
| `peer.reclaim_skipped` | `peer_id`, `reason` | A peer that server reclaim skipped because it could not decide. The values of `reason` are set by the decision order table of 4.6 server reclaim. If `peer_id` cannot be read, it is `-`. The level is `WARN` |
| `room.renewed` | `host_peer_id`, `expires_in_s` | Lease renewal verification (5.1). The line stopping after the host stops signalling is what is watched |
| `vip.claimed` | `virtual_ip`, `peer_id` | Verification of duplicate assignment under concurrent join. Emitted only for a `join_room` claim. The host's `10.100.0.1` is carried by `room.created` |
| `server.started` | `bind`, `port` | Startup confirmation. Emitted once after listening succeeds. The same values as the bind check of [`windows-prereq.md`](windows-prereq.md) section 6 Control plane EC2 |
| `counter` | `name`, `value` | The counters below |

The counters are `http_read_timeout`, `internal_error`, `rate_limited`, `rate_table_full`,
`elapsed_wall_fallback`, `unavailable`. All of them are emitted at shutdown and every 60 seconds.
Zero values are emitted too.

- Only `peer.reclaim_skipped` has level `WARN`; the rest are `INFO`
- `http.request` is one line per answered request. `ms` is the time from accept until the send
  finishes, floored to milliseconds. `op` is carried only when the request was read to the end and
  decided to be one of the five operations; otherwise it is `-`
- `log.py` holds this table as an allowlist of events and fields. A name not in the table does not
  make a line; it is an error

**Not carrying `room_id` in the log is stated again.** The `op` and `status` of `http.request` are
enough for the Phase 3 verification. Diagnostics that need to know which room use `peer_id`. A
`peer_id` has meaning only inside a room, so it alone cannot be used to join.

### 7.6 Configuration and Deployment

Configuration is environment variables. There is no file. There are only four values.

| Variable | Meaning | Default |
|------|-----|--------|
| `SANGTACHI_CP_PORT` | Bind port. A full match of `[1-9][0-9]{0,4}` and 65535 or less. An empty value is an error | `8000` |
| `SANGTACHI_CP_TABLE` | DynamoDB table name | None. **Required** |
| `AWS_REGION` | Region | None. Required. Standard `boto3` variable |
| `SANGTACHI_CP_ENDPOINT` | DynamoDB endpoint URL override. For local tests | None. If absent, the region default |

**There is no variable through which the server receives credentials.** On EC2 it is an IAM role
([ADR 0004](decisions/0004-state-store-dynamodb.md) decision 5).

Local tests are different. DynamoDB local does not check credentials, but `boto3` does not send a
request if it cannot find credentials (`NoCredentialsError`). So the test harness puts values that
are not real keys into the standard `boto3` variables. So that these values do not go to real AWS,
the harness runs the store tests only when the endpoint is loopback. It also stops the user's AWS
profile and config files from being read.

> **Why.** If a variable that takes an access key existed, someone would use it.

Deployment is **one systemd service.** `Restart=always`. There is no restart recovery procedure
(6.1), so restarts are cheap. If a required variable is missing or has a wrong format, the process
ends right away with exit code 2. If startup after the configuration check fails, such as listening
or setting up signal handling, the exit code is 1. Either way, it emits only one fixed-text line and
does not emit variable values or exception tracebacks. The startup failure text carries only one
name. For an OS error it is that error's name (for example `EADDRINUSE`, or `OSError` for an unknown
number), and for any other exception it is the exception class name. This rule applies to failures
before listening succeeds. Exceptions after listening propagate as they are.

**Shutdown.** On `SIGINT` or `SIGTERM`, the server closes the listener, emits the counters, and ends.
It does not wait for in-flight requests. The results of those requests can be missing from the
counters at shutdown, and the client gets a transport error and sends again per 8.3 Error
Classification and Retry. The security group opens inbound TCP 8000. The telemetry service's
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

**One more action, `cloudwatch:GetMetricStatistics`, is added for operational checks.** The server
does not use it. It is used on the instance to read the table's capacity metrics and check
"Capacity" below. The resource is `*`. The server process uses the same role, so the price is that it
can read the account's metrics. It is not a permission that changes metrics or touches data. The
repository owner decided this.

**Capacity.** RCU 25 and WCU 25 are the DynamoDB Always Free limit. This limit is per region and per
payer account, and applies when the table class is Standard. It was confirmed that the account's
Free Tier page shows it as "Always Free monthly allowance".

The load is the worst case under the assumption that each item is 1KB or less. A write uses 1 WCU
per item, and a strongly consistent `Query` uses RCU equal to the total bytes read rounded up in
4KB units (the read/write units section of the AWS DynamoDB Developer Guide). One room has at most
20 items per 6.2.

| Load | Worst case |
|------|--------|
| `host_report` renewal writes of a room below capacity | With three players, 16 items every 5 seconds. 3.2 WCU/s |
| Renewal writes of a full room | 20 items every 30 seconds. 0.7 WCU/s |
| Reads of `host_report` | Two `Query`s per call (4.6). If the room has just become full it is 20KB, so 5 RCU per `Query` and 10 RCU per call. The interval is set by the previous response, so that call can come 5 seconds later, and the periodic calls alone give a worst case of 2 RCU/s. The immediate call when a session ends adds 10 RCU per call |
| One player polling with `get_peers` | One `Query` every 0.5 seconds. With 20 items it is 20KB, so 5 RCU, 10 RCU/s. It runs only until ready |

One room fits within 25 for writes. For reads, during the few seconds when four players poll at the
same time, polling alone is a worst case of 40 RCU/s, and the `host_report` reads on top of that go
over 25. The excess can be absorbed by burst capacity, where DynamoDB keeps up to 300 seconds of
unused capacity. It is not guaranteed, though. The same guide says that reserve can be used for
background work without notice, so throttling can happen. Then the client gets `internal` (the 6.3
cancellation reason table) and retries per 8.3 or waits for the next poll. If several rooms are
joining at the same moment it can go over.

**The table above is the worst case, and the measured values are much smaller.** With
`tools/cp-deploy/verify.py --soak 180`, a state was kept for 3 minutes in which four players in one
room poll every 0.5 seconds and the host calls `host_report` every 5 seconds. The maximum of the
CloudWatch 1-minute `Sum` divided by 60 was 7.1 RCU/s for reads and 2.6 WCU/s for writes. That means
one poll is about 1 RCU, which matches the whole set of items of one room fitting within 4KB. In the
same window the throttle metrics had no data points. `ThrottledRequests` was read per operation (the
`Operation` dimension), and `ReadThrottleEvents` and `WriteThrottleEvents` per table.

**The measured values are from before the `NAME#` item existed.** They have not been measured again
since. A `NAME#` item holds only the key, `peer_id`, and `ttl`, so the whole set of items of one room
is expected to still fit within 4KB, but this has not been confirmed.

**The deployment procedure is owned by [`tools/cp-deploy/`](../../tools/cp-deploy/README.md).** The
unit file and deployment commands are not copied here. That tool uploads only committed code, writes
the system user and the systemd unit, and confirms startup with the `server.started` line and the
response of one operation.

**Time limits and retries of DynamoDB calls.** These are the values the server gives `boto3`.

| Item | Value |
|------|-----|
| connect timeout | `DDB_CONNECT_TIMEOUT_S` (1 second) |
| read timeout | `DDB_READ_TIMEOUT_S` (1 second) |
| Number of times the SDK sends | `DDB_MAX_ATTEMPTS` (1). The SDK does not send again |

> **Why.** Retrying is the client's job (8.3 Error Classification and Retry). The client of 8.2 Time
> Limits puts 3 seconds on one socket receive. If those 3 seconds pass before the server starts
> sending the response, the client gives up, and the abandoned request only holds a server slot
> (`MAX_INFLIGHT`). If the SDK sent again, the time of one call would grow several times and easily
> exceed those 3 seconds. 1 second is the value measured on the deployed table plus a margin. The
> server-side time of one whole operation, by the `ms` of `http.request`, was a median of 8~118ms,
> and the longest was 331ms (`host_report`).

**These values are not a cap on one whole operation.** One operation calls the store several times,
so if each call takes 1 second, the whole takes that multiple. There is no cap on the whole
operation.

**Limits of local testing.** In DynamoDB local, reads usually look like the latest value, so a
missing `ConsistentRead` does not show, and `TransactionConflictException` does not occur either.

- Both are stated with AWS documentation quotes in the "what it costs" section of ADR 0004
- Storage 1 is judged by reading the code, and the transaction conflict path is confirmed on a real
  table

**Transaction conflicts happen on the real table.** When four players in one room send `join_room`
at the same time, some get `internal`. Over five runs it was 7 of 20. Every new join transaction does
a ConditionCheck on the same `ROOM` item, so overlapping transactions are cancelled with
`TransactionConflict`. This was confirmed by reading the cancellation reasons directly. Per the
cancellation reason table of 6.3 Items it is `internal`, and when the client retry of 8.3 Error
Classification and Retry (same nonce, 1-second interval, 3 times) was simulated, all 40 of ten runs
joined. The test tool is `tools/cp-deploy/verify.py`.

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
| Send, receive, each | `CLIENT_IO_TIMEOUT_S` (3). Send uses `SO_SNDTIMEO`, so it is per call. If it goes out in pieces, the sum can be longer. Receive puts a 3-second deadline on the whole stage and waits with `select` for the remaining time. `SO_RCVTIMEO` is also set | Transport error |
| Cap on one request | The sum of the three above. **At most 9 seconds, excluding DNS.** If the send goes out in pieces, it can be longer by that much | - |

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
| Definite error | `ok: false` and `error` is not a transient error | `room_not_found`, `room_full`, `name_taken`, `unauthorized`, `bad_request`, `rate_limited` |
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
6. register_candidate. Local candidates + reflexive candidates. If more than 8, the client trims first.
   The list order and trimming rule are the registered list of protocol.md 10.1
7. Player: poll get_peers. Until that pair has ready: true and the peer has 1 or more candidates (4.5).
   **The polling interval and deadline are counted from the moment the success response of step 6
   arrives** (protocol.md section 11)
   Host: call host_report periodically. When the response peers shows a peer with ready: false,
   put that peer_id into the confirm of the next call. The server checks both sides' candidates (4.6 processing order step 3).
   It does not call get_peers
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

**If no peer candidate remains after the hygiene of step 8, that pair is treated as not ready.** It is
the same as the empty candidate list of 4.5. The rules and order of hygiene are owned by `protocol.md`
10.1 Candidate Collection and Hygiene.

**Step 7 is the only place that splits by role.** The host and the player receive the same
information through different operations. The data path does not split
([ADR 0006](decisions/0006-star-topology-no-relay.md) decision 6).

**The host starts `host_report` right after step 4.** The room stays alive only if the lease is
renewed (5.1). The cycle runs even when there is no pair to confirm.

---

## 9. Verification

The Phase 3 verification items are owned by [`roadmap.md`](roadmap.md). **The case tables those items
run are in `control-server/tests/`.** This document owns the rules, and each section points to the
file of its own table. There is not one set of tables in the document and another in the tests.

- The case tables sit in 2.1 `room_id`, 3.3 HTTP Subset, 4.4 `register_candidate` (candidate
  hygiene), 4.5 `get_peers` (the field set of an element), 4.6 `host_report` (the reclaim and
  confirmation race), 5.1 Room, 6.4 Rate Limit, and 7.4 Clocks
- Rows that need the store to run are marked `needs="store"` and are skipped until then
- How to run the tests is in [`control-server/README.md`](../../control-server/README.md)

**Every case table gets a mutation test.** Check which row of the table an implementation with one
rule line deleted `FAIL`s on. If a mutation drops no row, that rule does not protect anything yet. The mutation list is one file
per table under `control-server/tests/mutants/`, and the runner `tests/mutate.py` decides.

**What can be judged only by reading code.**

| What | Why a behavioral test cannot do it |
|------|----------------------------|
| Storage 1 `ConsistentRead=true` | Eventually consistent reads also usually return the latest value. Local even more so |
| Token comparison is constant time and done first in the application (6.3) | Timing differences are hard to reproduce in a test |
| No `ipaddress` predicates in decisions (7.1) | A case table row can pass by accident |
| The name check compares directly against the 63-character set and uses no Unicode predicates (2.7) | `str.isalnum()` and the regex `\w` accept Unicode letters, and `$` also matches before a trailing newline. A character not in the case table can slip through that gap |
| `room_id` and `peer_token` are absent from logs (7.5) | Scanning all the logs is not a proof that "it appears on no path" |
| Whether the response of `host_report` is read **after** the writes (4.6) | The read and write order of two requests cannot be forced from outside. Sending them at the same time usually ends up sequential and the defect does not show. Running the 4.6 confirmation race case table needs an **injection point in the store layer that adds a delay right after the write**, and whether that injection point exists is also judged by reading code |

---

## 10. Undecided

| What | Where and when |
|------|-------------|
| Telemetry service port, schema, authentication | Outside this document's scope. [`roadmap.md`](roadmap.md) Phase 9 |
| Whether the two services share one table, whether IAM is split | [ADR 0004](decisions/0004-state-store-dynamodb.md) deferred it to Phase 9 |
| Automatic retry and re-polling by the surviving side | [`protocol.md`](protocol.md) 10.4 open item. There is no rejoin (4.3), so that procedure has to stand on a new join |
| TLS, caller authentication | Stretch. 1.2 |
| Whether `ROOM_LEASE_S` (120) and the two signalling intervals are right | After the Phase 8 demo. Judged by how long a room takes to disappear after the host dies, and by how long a late participant waits |
