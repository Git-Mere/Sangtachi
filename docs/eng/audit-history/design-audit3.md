# Full Design Audit, Third Round (2026-09-23)

**Target:** the complete text of [`../spec.md`](../spec.md), [`../roadmap.md`](../roadmap.md),
[`../architecture.md`](../architecture.md), [`../protocol.md`](../protocol.md),
[`../control_plane.md`](../control_plane.md), [`../windows-prereq.md`](../windows-prereq.md)
**Point in time:** right before Phase 1 implementation starts, at commit `6b0c85b`
**Reason:** the 54 follow-ups of the second audit (2026-09-21) were all closed in the first and
second batches, and after `control_plane.md` was created there had been no full-text audit that
included that document. We look once more before writing code.

> **This document is an audit record.** It keeps the result and the judgement of that point in
> time. **Current work tracking belongs to [`../../kor/plan.md`](../../kor/plan.md).** The chapter
> and section numbers and the quotations in the body are as written at commit `6b0c85b`, and they
> are not corrected here even if the documents change later. **This audit only investigated.** It
> did not change the documents, and the repository owner decides what to act on after reading this
> report.

---

## 1. Method

Six independent reviews with different lenses ran in parallel against the complete text of the six
documents. The scenario lens of the second round's five lenses was split into a data plane one and
a control plane one. That is because `control_plane.md` is the document getting its first
full-text audit.

| Lens | Question |
|------|------|
| Scenario trace (tunnel) | If one `DATA`, the punch happy path, one failure, one renegotiation, and one shutdown are followed end to end, do the documents answer |
| Scenario trace (control plane) | Are startup -> `create_room`/`join_room` -> registration -> `get_peers` -> punch time, the races (simultaneous join, simultaneous registration, same nonce), server restart, and room lifetime one story |
| Consistency cross-check | Are constants, timers, states, counters, error codes, event keys, requirement IDs, and section number references the same across documents. Extracted exhaustively by script and cross-checked |
| Implementer's view | Under "do not decide in code a value that is not in the documents", can Phase 1-3 be coded now |
| Verification criteria | All roadmap verification items, spec M/T/A, and the six case tables of `control_plane.md`. Does a correct implementation pass, does a defective implementation pass, is the verdict procedure executable |
| Adversarial input + windows-prereq | Holes not written as "intentionally not guaranteed". And the decidability of chapter 15 of `windows-prereq.md` and the safety of its procedures (rules 6 and 7) |

**Constraints given to the lenses.** Read only. A "missing" finding only after grepping the six
documents. Every finding quotes the source text. And **do not raise findings about scheduled
work.** The pre-start items of the roadmap (Phase 3: EIP/DNS, clock premises, credentials, free
tier, case table migration; Phase 4: keepalive measurement procedure, nat-probe re-measurement,
local record format; Phase 5: automatic retry; Phase 6: Wintun publisher, injection failure
representation; Phase 7: MTU measurement; Phase 9: telemetry spec, resource isolation), the
chapter 10 table of `control_plane.md`, and the waiting list of `plan.md` are not defects. They are
raised **only when a deferred decision has no point in time anywhere.**

The report of each lens was not taken on trust; **every blocker and most of the warns were
cross-checked against the HEAD text at integration time.** Findings raised independently by two or
more lenses were merged into one (noted on each finding). The severity used for the totals is the
same as in the second round. A blocker is one where a correct implementation cannot pass
verification, where following the documents makes you believe a wrong result is right, or where
two results are fixed for the same input so the implementer cannot choose.

**Totals: blocker 4, warn 31, info 27 - 62 findings.** Before merging, the lens reports were
blocker 4 / warn 42 / info 40. The lenses overlapped in 11 places in warn and in 13 places in info.

The consistency lens cross-checked about 40 constants, 55 counters, 12 error codes, 13 event keys,
45 requirement IDs, and 234 cross-document section number references, and **no mismatch came out of
the values, names, or section numbers.** One requirement ID (NFR-10) is missing from the tracking
table. There are 0 counters that are used but have no definition. The kinds the second audit fixed
(copied value mismatch, verification counter error, missing startup inputs, missing thread
ownership) did not recur.

**Distribution of this round's defects.** Three of the four blockers are a one-sentence or
one-paragraph fix, and one (B-1) is a design change to the storage contract. The warns cluster in
three places. (1) **The lifecycle of the renegotiation and terminal states** - after the second
round's W-1 filled the transition table, its surroundings (timers, behaviour after
`FAILED`/`CLOSED`, record lines) demand new contracts. (2) **The case tables and prose of
`control_plane.md`** - the kind that comes out of the first full-text audit of a new document.
(3) **The test harness the verification items presume** - no document owns the hooks, entry
points, and timers of the test build.

**No blocker stops Phase 1 from starting.** The placement of the four is in chapter 5.

---

## 2. Blockers (4)

### B-1. If two peers send `register_candidate` at the same time, readiness may never be recorded

(Control plane scenario lens. The integrator re-traced it against the text of 6.3, 4.4, and 4.5)

- Location: `control_plane.md` 6.3 "reads" table, `register_candidate` row / 4.4 "readiness
  verdict" / 4.5 "definition of readiness" / `roadmap.md` Phase 3 verification "readiness exactly
  once per room"
- Source text: 6.3 - "one `Query(pk)` for `ROOM` and all of `PEER#` (expiry and token checks) ->
  `PEER#` update -> the readiness verdict is made on the same `Query` result with our own
  registration folded in".
  4.4 - "even if two peers register at the same time and both see "all registered", the conditional
  write succeeds only once. For the side that fails it means it was already recorded, so it is not
  an error."
  4.5 - "**readiness is defined as the existence of `ROOM.ready_at_wall_ms`.** The number of peers
  or of candidates is not counted again at response time."
- Problem: 6.3 says to make the readiness verdict on the result read **before** the update, with
  only our own registration added. Then in the order (1) A reads -> (2) B reads -> (3) A writes and
  judges (no B candidate, records nothing) -> (4) B writes and judges (no A candidate in the result
  from step 2, records nothing), `ready_at_wall_ms` is recorded **not even once**. Since 4.5 nails
  down that it must not be counted again, `get_peers` stays `ready: false` forever and both sides
  end at the 60 second polling deadline with `CONTROL_PLANE_EXCHANGE_FAILED`. The race 4.4 covers
  is only the case where "both saw all registered". The conditional write
  `attribute_not_exists(ready_at_wall_ms)` stops writing twice but does not stop **writing zero
  times**. roadmap Phase 3's "`room.ready` is one line even if they send at the same time" cannot
  be passed by an implementation built from the documents (zero lines). That violates rule 3.
- Proposal: change 6.3 so that the verdict is made by reading `Query(pk, ConsistentRead)` again
  **after** the `PEER#` update, or bind the readiness record together with a
  `size(candidates) > 0` ConditionCheck on the other `PEER#` in a `TransactWriteItems`. Either way,
  put the row "A reads, B reads, A writes, B writes" into the case table. Before Phase 3 starts.

### B-2. The behaviour when there is only one STUN server is the opposite of itself inside `architecture.md` 3.5

(Implementer lens + verification criteria lens. Two lenses raised it independently)

- Location: `architecture.md` 3.5 startup input table `--stun` row and the "server selection"
  paragraph of the same section / `roadmap.md` Phase 2 verification
- Source text 1 (table): "if only one is given the list has one entry, and then the `spec.md` M-2
  verdict (two different servers) cannot be made. In that case the client leaves one `WARN` line
  and proceeds. An undecidable verdict is not turned into a failure, and is not disguised as one"
- Source text 2 ("server selection"): "if the responses do not become two even after the whole list
  is used, it is `STUN_DISCOVERY_FAILED`. **One response alone is also a failure.**"
- Problem: give one `--stun` and let that server respond, and source text 1 says "proceed" while
  source text 2 says `STUN_DISCOVERY_FAILED`. The same section fixes two results for the same
  input. The Phase 2 verification "specify an address that does not respond and after the timeout
  `STUN_DISCOVERY_FAILED`" is naturally run with one `--stun`, and the expectation of its control
  test (one server that does respond) splits. A defective implementation (proceeding with one) and
  a correct one cannot be told apart.
- Proposal: nail it down in one sentence and delete the other one. For example: "if the list has
  one entry, leave a `WARN` at startup, and in the STUN stage the responses do not become two so it
  ends as `STUN_DISCOVERY_FAILED`", or the opposite. Before Phase 2 starts. It is a one-sentence
  fix.

### B-3. The result of one forged `HELLO` is "certain", not "reachable". Chapter 2 understates the attack effect

(Adversarial lens. The integrator re-traced it against 4.3, 8.1, 8.3, 9.5, 9.6, 10.3, and chapter 11)

- Location: `protocol.md` chapter 2 / 9.5 (+ 4.3, 8.1 check 8, the 9.4 `CONNECTED` column, 9.6,
  10.3)
- Source text (chapter 2): "to go all the way it has to reach the deadline in that state, and
  before that, if a new `HELLO` from the real peer arrives, it can be recovered by renegotiation.
  **It is not a fixed result but a reachable one.** The renegotiation limits in 9.5 (count and
  interval) only make this cost finite; they do not stop the attack itself."
  (9.5): "if the 8 are spent with forged `HELLO`s, after that **even a real restart of the peer is
  dropped as `drop_reneg_limit`.**"
- Problem: following the rules of the documents exactly, the recovery path is closed. A and B are
  `CONNECTED` and A has pinned B's epoch E_B; the attacker sends A one
  `HELLO{peer_id=B, epoch!=E_B, virtual_ip=B}`. (1) A accepts it by 9.5, puts E_B on the retired
  list by step 1, and pins the attacker's epoch by step 4. (2) B is `CONNECTED`, so by 10.3 it does
  not retransmit `HELLO`, and all it sends is `DATA`/`KEEPALIVE`/`PING`/`HELLO_ACK` carrying E_B,
  so **A drops it at check 8 of 8.1 as `drop_retired_epoch`.** Since 8.1 comes before 8.2 and 9.3,
  even a `HELLO` carrying E_B cannot become a renegotiation trigger. (3) The "new `HELLO` from the
  real peer" of chapter 2 has to carry a **new epoch**, but B draws a new epoch only when it starts
  a new attempt from `IDLE` by 9.6, and that is after B becomes `TUNNEL_DROPPED` 50 seconds later
  (and only if the undecided automatic retry exists). That is later than A's 10 second punch
  deadline. (4) Result: A is `PEER_HANDSHAKE_FAILED` after 10 seconds, B is `TUNNEL_DROPPED` after
  50 seconds. It is the same if B is still `PUNCHING`/`HANDSHAKING`, because its retransmitted
  `HELLO` carries E_B. That is, **one shot** from an attacker who knows only `peer_id` (and the
  predictable `virtual_ip`) ends both sessions deterministically. 9.5's "if the 8 are spent"
  understates the number of shots needed, and chapter 2's "reachable" understates the certainty.
  Putting in "not a fixed result" while fixing B-6 of the second round came back as an
  understatement this time. If the final report chapter 2 asks for is written as it stands, it
  states more defence than there is.
- Proposal: change the result description of chapter 2 to "it is fixed by one shot. A is
  `PEER_HANDSHAKE_FAILED` after 10 seconds, B is `TUNNEL_DROPPED` after 50 seconds". It is a
  one-paragraph fix. To change the design there is an option of deferring the retirement
  registration of 9.5 step 1 until after a valid `HELLO_ACK` for the new epoch (or a source inside
  the candidate set), but that is a separate review item and this audit does not recommend it.

### B-4. The Phase 4 unverified budget item's "after 10 in a row the 11th is blocked" does not count the probe token, so a correct implementation fails

(Verification criteria lens. The integrator checked it against the 9.4 budget table and items 1-4
of 10.4 (c))

- Location: `roadmap.md` Phase 4 verification "common budget for unverified destinations" /
  `protocol.md` 9.4 budget table, 10.4 (c)
- Source text (roadmap): "we also check that after 10 in a row the 11th is blocked and one gets
  through 200ms later." The same item (d): "`HELLO_ACK`, `PONG`, and the verification `HELLO` fall
  **together** within the limit above".
  (protocol 10.4 (c)): "2. Take one token from the common budget for unverified destinations in
  9.4 ... 4. Send one HELLO carrying the probe nonce to that address ... the token taken earlier is
  spent here"
- Problem: the first `HELLO` from a source outside the candidate set passes 8.1-8.3 and advances
  `highest`, so it hits 10.4 (a)(b)(c), the provisional path registration starts and one token is
  spent on the verification `HELLO`. The `HELLO_ACK` for that same packet spends one more token.
  The first shot costs 2 tokens and later shots 1 token, so capacity 10 is **spent at the 9th and
  the `HELLO_ACK` of the 10th is blocked** (when shooting faster than the refill of 1 per 200ms).
  This sentence contradicts (d) of the same item, which says to count the verification `HELLO`
  together. An implementation built to the rules `FAIL`s on this sentence (rule 3).
- Proposal: change it to "the first `HELLO` from a new source spends 2 tokens, on the `HELLO_ACK`
  and the verification `HELLO`. After 9 in a row the 10th is blocked", or nail the test condition
  down to "a source already registered as a provisional path (within 5 seconds)" so that it becomes
  the 11th. Before Phase 4 starts. It is a one-sentence fix.

---

## 3. Warns (31)

Written by group. The parentheses give the lens that found it.

### A. Lifecycle of the renegotiation and terminal states (protocol chapters 9-11, architecture 3.2.7 and chapter 9)

**W-1. After a renegotiation happens in `CONNECTED`, there is no lifecycle for `HELLO`
retransmission, `KEEPALIVE`, `PING`, and the idle timer.** (Tunnel scenario)
Chapter 11 sets the cancel of `HELLO` retransmission to "reaching `CONNECTED`" and the cancel of
the `KEEPALIVE`, `PING`, and idle timers to "session end". Step 9 of 9.5 fixes only the **target**
of retransmission and has no sentence about **starting** it again, and in the "start" column of
chapter 11, 9.5 appears only in the punch deadline row. Implemented to the letter, (a) after a
renegotiation not one of our `HELLO`s goes out, so both sides are `PEER_HANDSHAKE_FAILED` 10
seconds later, (b) a session that dropped down to `HANDSHAKING` keeps sending `PING`, so RTT
samples are produced before `CONNECTED` (contradicting 9.4 "`PING` is sent from `CONNECTED`" and
architecture chapter 9 "no RTT sample is produced before `CONNECTED`"), and (c) the idle timer
keeps running but 9.4 does not refresh it in `HANDSHAKING`, so if the idle time left just before
the renegotiation is under 10 seconds, `TUNNEL_DROPPED` fires before the punch deadline. As a side
result, a session that has been through `CONNECTED` can end with `HOLE_PUNCH_TIMEOUT` or
`PEER_HANDSHAKE_FAILED` of 9.6, but the code set of the end line in architecture chapter 9 is only
the two "`CLOSED` or `TUNNEL_DROPPED`", so there is no code to write on that line. - Put the stop
of the three timers and the restart of retransmission into the 9.5 steps, and reflect 9.5 in the
start and cancel columns of the chapter 11 table. Widen the code set of the chapter 9 end line to
all of 9.6. Before the Phase 4 transition table is implemented.

**W-2. The behaviour of `HELLO` retransmission and of the process after reaching `FAILED` is
nowhere.** (Tunnel scenario + verification criteria)
Read to the letter, 10.3 "the only trigger that stops retransmission is reaching `CONNECTED`" and
the cancel column of chapter 11 mean a `FAILED` session sends `HELLO` to the whole candidate set
every 200ms until the process ends. Whether the process then exits by itself (as roadmap Phase 4
"on hole punching failure ... the process exits normally" expects) or waits for `quit` (9.4.1 says
packets keep arriving after a terminal state and that is normal) is neither in the shutdown
triggers of 3.2.7 nor in 9.6. It is about **ending**, separately from the automatic retry (Phase 5,
undecided). - Put "punch deadline" into the cancel column of the `HELLO` retransmission row of
chapter 11, and fix the process behaviour after reaching a terminal state in one line in 3.2.7.

**W-3. The entry into `CLOSED` and the retention rule conflict in two places.** (Tunnel scenario +
verification criteria)
(a) Whether our session transitions to `CLOSED` when we send `CLOSE` is missing. Entry into
`CLOSED` is only in the **receive** row of 9.4, while the end line code `CLOSED` of architecture
chapter 9 and the counter dump point "reaching a terminal state" presume that transition. (b) 5.6
"the receiving side removes the session and the routing entry immediately" conflicts with 9.4.1
"`CLOSED` | discard everything regardless of type, `drop_terminal_state`". An implementation that
removed the session has no `CLOSED` session, so it cannot pass the roadmap Phase 4 item "receiving
in a terminal state" (`KEEPALIVE` after `CLOSED` is `drop_terminal_state`), and only an
implementation that retains it passes. Both are inside protocol.md, so the precedence rule does not
settle it. - Nail down in one line each "a session that sent `CLOSE` transitions to `CLOSED` (even
if the send failed)" and "remove the routing entry and retain the session as `CLOSED`".

**W-4. The justification sentence of 5.6 does not hold for the `FAILED` transition and for the
path from a source outside the candidate set.** (Tunnel scenario)
If only B->A `HELLO_ACK` is blocked, A has only `sent_ack` set and is `HANDSHAKING` with
`peer_endpoint` learned, and B is `CONNECTED`. Ten seconds later A goes to
`FAILED(PEER_HANDSHAKE_FAILED)`, but 9.6 does not send `CLOSE` on that transition and the `FAILED`
row of the 5.6 table blocks it afterwards too. So B waits 50 seconds. The reason 5.6 gives for
sending `CLOSE` to a `HANDSHAKING` session after learning ("the peer waits the 50 second idle
timeout") applies exactly to this situation, but the result is the opposite. Also, if both `HELLO`
and `HELLO_ACK` come from a source outside the candidate set, learning is blocked at 10.4 (c), so a
session that is `CONNECTED` but has not learned comes into being, and "it has been learned" in 5.6
is false in that case. Since the document states that `FAILED` does not send, the definition
exists. The strength of the justification sentence differs in two places. - Either decide that the
punch deadline -> `FAILED` transition sends `CLOSE 0x01` to a learned session, or write the reason
for not sending, and narrow "it has been learned" to "if the source was inside the candidate set".

**W-5. The `virtual_ip` check of 5.1 (`drop_vip_mismatch`) is not in the chapter 8 pipeline nor in
the chapter 15 checklist.** (Tunnel scenario)
There is no `virtual_ip` comparison anywhere in the 8.1 table, the 8.2 table, 8.3, or 8.4, and none
in chapter 15 either. Whether the "valid `HELLO`" of 9.3 includes this check, and whether the check
happens **before** the epoch is pinned, is not fixed. Chapter 2 writes that the only gate against a
forged `HELLO` is "`virtual_ip` match (5.1)", so if this check is missing or comes after pinning,
it contradicts chapter 2. Chapter 15 is the list of "everything that must be in the code at Phase
4", and the Phase 4-5 roadmap verification has no `drop_vip_mismatch` test. - Put "if `virtual_ip`
does not match, discard, `drop_vip_mismatch` (before the epoch is pinned)" into the
`HELLO`/`HELLO_ACK` row of 8.2 and add it to chapter 15.

**W-6. "Our own previous attempt epoch", which 4.3 puts on the retired list, can never match any
received packet.** (Integrator. Checked against the text of 4.3 and 8.1)
4.3: "entries arise only from our own previous attempt (one per retry) and the peer's previous
epoch (one per renegotiation in 9.5)". Check 8 of 8.1 compares the `session_epoch` **of the
received packet** (the sender's, that is the peer's value) against the retired list. Our own epoch
is not carried in any packet the peer sends, so that entry filters nothing. The delayed packet to
be filtered when we retry from `IDLE` carries **the peer epoch we pinned in the previous attempt**,
and 4.3 does not say to register that. What actually stops that case is `drop_no_epoch` of 8.2
(a non-`HELLO` while no epoch is pinned) and `drop_bad_nonce` (a `HELLO_ACK` echoing an old nonce),
not the retired list. 4.3's "this stops ... most delayed packets" does not hold in the direction of
our own retry, and the size justification of `MAX_RETIRED_EPOCHS` (1 for retry + 1 for
renegotiation) is off by that much as well. The behaviour converges, so it is a warn. - Change the
registration target of 4.3 to "on retry, the peer epoch pinned in the previous attempt", and write
that the defence line in the direction of our retry is 8.2.

### B. The base point of the `get_peers` deadline and of the STUN deadline (protocol chapter 11, architecture 3.5, control_plane 5.1 and 8.4)

**W-7. The start point "entering `IDLE`" of the `get_peers` polling and deadline points at a moment
that cannot be executed on the first attempt.** (Control plane scenario + implementer + tunnel
scenario. Three lenses raised it independently)
Chapter 11: "`get_peers` polling | 500ms interval | entering `IDLE`", "`get_peers` deadline | 60s |
entering `IDLE`". Since 9.1 has no state before `IDLE`, "entering `IDLE`" on the first attempt is
the process start. At that moment there is no `room_id`, `peer_id`, or `peer_token`, so `get_peers`
cannot be called, and 8.4 of `control_plane.md` puts the polling at step 7 (after registration).
If the 60 seconds are counted from the start of the attempt, the `create_room`/`join_room` retries
(8.2, up to 9 seconds x the maximum count + 1 second intervals), STUN, and the
`register_candidate` retries eat into the deadline, so in the extreme the deadline passes before
the first poll, and 10.2's "it works even if the peer turns the client on 50 seconds later" does
not hold. If counted from the first poll, the chapter 11 table is wrong. - Change the start column
of the two chapter 11 rows to "the first `get_peers` send" (or the successful `register_candidate`
response). Before Phase 3 starts.

**W-8. The justification sentence for the 3600 second room lifetime conflicts with the 60 second
`get_peers` deadline.** (Consistency)
`control_plane.md` 5.1: "one hour is a value sufficient to create the room, pass the code to the
peer, and have both sides finish STUN and registration". But per 8.4 the host creates the room,
goes through STUN, and enters polling right away, and if the peer does not finish joining, STUN,
and registration within 60 seconds it ends with `CONTROL_PLANE_EXCHANGE_FAILED`. The real window
for a human to pass the code is not 3600 seconds but the host's 60 second polling. Building a demo
script on this sentence makes a host failure within one minute read as a control plane fault. -
Change the justification in 5.1 to "the slack for code delivery and registration is set by the
`get_peers` deadline in protocol chapter 11", and write the limit that the 60 second deadline does
not cover a human's delivery time.

**W-9. The result of exceeding the STUN deadline differs between `protocol.md` 9.6 and
`architecture.md` 3.5.** (Consistency + implementer + tunnel scenario)
9.6: "STUN deadline exceeded | `STUN_DISCOVERY_FAILED`". 3.5: "if one server hits the STUN deadline
(chapter 11), switch to the next server in the list and query again. If the responses do not become
two even after the whole list is used, it is `STUN_DISCOVERY_FAILED`." protocol has deadline
exceeded = failure transition, architecture has deadline exceeded = server switch with failure only
after the list is exhausted. Whether the 5 second deadline of chapter 11 is per server or for the
whole stage, whether the retry schedule and the deadline restart on the switched server, and the
worst case total time (4 in the list, 2 at a time) are nowhere. Applying the rule that protocol
wins for timers and transitions, the server switch of 3.5 does not hold. It is a decision needed
for the Phase 2 (P0) implementation. - Write the unit of the deadline and whether it restarts on a
switch into chapter 11, and make the 9.6 row the same sentence as the condition in 3.5.

### C. The case tables and prose of `control_plane.md`

**W-10. Whether "reject" in the 4.4 candidate hygiene case table is an individual discard
(`rejected`) or a whole-request `bad_request` is not fixed.** (Control plane scenario + implementer
+ verification criteria. Three lenses)
The table writes broadcast (individual discard) and `10.0.0.5:65536`, `"10.0.0"`, and a different
`kind` value (form) with the same word "reject", while the rejection policy classifies "wrong type"
as a whole-request `bad_request`. Neither the table nor the policy says whether those rows are
hygiene or form, so both `{"ok":true,"rejected":1}` and `400 bad_request` read as conforming. The
client result splits too (`WARN` then proceed vs `CONTROL_PLANE_EXCHANGE_FAILED`). The expected
value for the Phase 3 verification "all rows pass" cannot be written and no mutation test can be
attached. Whether the row "the same `ip:port` twice -> only one stored" counts into `rejected` is
also missing (if it does, a normal list gets a `WARN` attached). - Rewrite the verdict column per
row as one of the three "store / discard that candidate only (`rejected`+1) / whole-request
`bad_request`", and write the `rejected` value of the duplicate row.

**W-11. A restarted host needs `room_id` to rejoin, but `--room` is `player` only.** (Control plane
scenario + consistency)
`architecture.md` 3.5: "`--room <6 chars>` | `player` only", "`--rejoin <peer_id>:<peer_token>` ...
can also be used with the `host` role. A restarted host also rejoins". The rejoin request in 4.3 of
`control_plane.md` requires `room_id`. `host --rejoin ...` has no argument to take `room_id`, so
the parser rejects it or it gets a `bad_request`. The first sentence of 4.3, "a participant calls
it", also conflicts with a host rejoin. The Phase 3 work names 3.5 as its source. - Either change
the `--room` row to "`player`, and a `host` given `--rejoin`" or put `room_id` into `--rejoin` and
match the `REJOIN` output line. Change the first sentence of 4.3 to "a new join is by the
participant, a rejoin by either peer".

**W-12. The refill unit of the 6.4 rate limit and the creation and removal rules for table entries
are missing.** (Verification criteria + adversarial)
"refill | `RATE_LIMIT_REFILL_PER_MIN`(10) per minute" satisfies both continuous refill of 1 every 6
seconds and a batch refill of 10 every 60 seconds, and row 13 of the case table ("any request 6
seconds later | 1 token refilled") holds only under the first reading. The reference time of row 13
and whether a `rate_limited` response updates the refill time are also missing. And when a table
entry is created (on the `exhausted` lookup or on `spend`) and when it disappears is missing.
Without a removal rule the table grows monotonically, so after 4096 sources the limit is off for
new sources until a restart. The phrase "while the table is full" presumes the table empties again,
but there is no means for that. - Put "refill is continuous (1 per 6 seconds, capped at 10), a
`rate_limited` response does not update the time" and "an entry is created on `spend` and removed
when its tokens reach capacity" into 6.4, and add two rows to the case table.

**W-13. `MAX_INFLIGHT`(32) x the 5 second time limit lets 32 slow connections block the control
plane continuously, and this availability limit is not in the acceptance table.** (Adversarial)
3.4: "it stops a caller that holds a connection open and trickles bytes from occupying the server's
connection slots." 7.2 raises `inflight` right after accept, before parsing. A caller that reopens
32 connections that send no header every 5 seconds (one IP, 6.4 connections per second) returns
`503` to every normal request in between, and since `http_read_timeout` does not spend the 6.4
budget it is not caught by `rate_limited` either. A normal client's `join_room` is
`CONTROL_PLANE_EXCHANGE_FAILED` within 3 seconds per 8.3. 3.4's "it stops" only cuts the occupancy
time of one connection and does not stop seat exhaustion (an overstatement of the second round B-6
kind). It is neither in the 1.2 acceptance limit table nor in the chapter 10 undecided table. -
Change 3.4 to "it limits the occupancy of one connection to 5 seconds. It does not stop seat
exhaustion" and put it into 1.2 as an acceptance limit, or fix a per-source concurrent connection
cap in 6.4.

**W-14. `create_room` is caught by no budget at all.** (Adversarial)
What 6.4 counts is the three `room_not_found`, `room_expired`, and `unauthorized`, and successes
are not counted. `create_room` takes one input, `client_nonce`, and returns a success response, so
an unauthenticated caller that changes the nonce and calls makes every call a four-item transaction
write that stays for 25 hours. Exhausting the provisioned write capacity throttles normal calls ->
`internal`. "Counting successes catches normal polling" is the reason for `get_peers`, and
`create_room` has no polling. This path is not in the 1.2 table. - Either put a separate per-source
budget on `create_room` successes or write it into the 1.2 table as an acceptance limit.

**W-15. "Hundreds of thousands of years to sweep the 30 bit space" is a wrong calculation. It is
about 204 years.** (Adversarial)
6.4: "at 10 per minute from one IP it is hundreds of thousands of years to sweep the 30 bit space".
2^30 / (10/min) = 107,374,182 minutes ~ 204 years. Three orders of magnitude too large. Also the
attacker's goal is not a specific room but **any live room**, so the expected number of attempts is
2^30 / (number of live rooms). At the "a few tens" scale 2.1 mentions (50), that is about 4 years
with one IP, and within two weeks with 100 IPs. Expired rooms also reveal their existence through
`room_expired` (4.1), so every room in the 25 hour window is a target. Believing the numbers in the
document reports the effect of the rate limit as far larger than it is. - Change it to "about 204
years for the whole space with one IP, 1/N of that with N live rooms, divided again by the number
of IPs".

**W-16. Chapter 2's "paths that harm others are blocked even in v1" does not cover unicast
reflection on the candidate path, and its strength differs from `control_plane.md` 1.2.**
(Adversarial)
10.1 filters only amplification addresses (broadcast, multicast, unspecified, loopback, port 0), so
an arbitrary **unicast** third party address passes as a candidate. The side that can insert that
candidate is not only the malicious server 1.2 writes about but **the peer that joined the room**
(`register_candidate` needs only its own token and the server checks only the form). The peer's
client sends to those 8 addresses every 200ms for 10 seconds, up to 400 shots. protocol chapter 2
writes "blocked" (assertion) and control_plane 1.2 writes "can be used" (acceptance) for the same
path. The same correction 10.4 made for the probe path ("it is not complete blocking; it is held by
the common budget") does not exist for the candidate path. - Change chapter 2 to "amplification
paths are blocked and unicast reflection is capped by 8 candidates x 10 seconds of punching x
200ms" and put "the peer can do the same thing" into the 1.2 table.

### D. Verification criteria (roadmap, spec)

**W-17. The remote endpoint input and the means to start sending that the Phase 1 verification
requires are in no document.** (Implementer)
Phase 1 work "send and receive UDP packets between two known endpoints", verification "bidirectional
message send and receive succeeds ... matching byte for byte". `architecture.md` 3.5 says "Phase 1-5
use CLI arguments only" while fixing only five arguments (`host|player`, `--server`, `--room`,
`--rejoin`, `--stun`), with no peer endpoint argument. The console word `send <n>` is `DATA` and
presumes `CONNECTED`, so it cannot be used in Phase 1. Where and in what shape a received datagram
is emitted so that "matching byte for byte" can be judged is also missing. Since 3.5 claims to be
the source of the startup inputs, the implementer ends up inventing an argument or command that is
not in the documents. - Either add scaffolding arguments for Phase 1-2 to the 3.5 table and one
line each for a Phase 1 raw send command and receive output format in the Phase 4 console word
section, or narrow the Phase 1 verification to "two processes on the same machine + a test build
entry point". Before Phase 1 starts.

**W-18. The location, event key, and measurement method of the "200ms test periodic timer" and the
"periodic timer log" are missing.** (Implementer + verification criteria)
Phase 1: "the **successive expiry interval of the 200ms test periodic timer does not exceed
400ms.**" Phase 3: "we watch whether **console command responses and periodic timer logs** keep
coming". Whether that timer is in the product build or only in the test build, how it is turned on,
and what the "successive expiry interval" is measured with are missing. The chapter 9 log contract
of architecture has no time field, so 400ms cannot be judged from the logs, and the 8 fixed event
keys have no periodic timer event, so what Phase 3 should grep cannot be decided. That contradicts
the rule chapter 9 set for itself, "events the verification looks for get fixed keys". The client
at the Phase 3 point has no timer that emits periodic logs, so a correct implementation can read as
"they do not come", and a defective implementation that put `connect` in `[loop]` can also pass
because there is no allowed delay value. - Add a periodic timer event key (for example
`timer.tick`, fields `name` and `elapsed_ms`) to the chapter 9 table, write in architecture that it
is a fixed device of the test build, and attach the same 400ms value as Phase 1 to the Phase 3
item. Before Phase 1 starts.

**W-19. In Phase 4-5 (no adapter) there is no destination for a received `DATA` that passed 8.4.**
(Tunnel scenario + verification criteria. The verification lens raised it as a blocker and the
integrator lowered it to warn, because the echo responder paragraph implies that the test build has
a hook)
protocol 8.4: "`DATA` must pass all of the below right before Wintun injection." The
`session == NULL` branch of architecture 3.2.3 covers only **reads** (`drain_wintun`). The only
destination the documents fix for a received `DATA` is Wintun injection, and Phase 4-5 have no
adapter. (1) Where the "payload byte string taken out after passing the 8.4 verification on the
receiving side" of M-5 is observed, (2) at which point the echo responder receives the packet, and
(3) whether a received `DATA` in the product build at Phase 4-5 is discarded and which counter goes
up, are missing. The source hygiene item nailed the receive **entry point** down in a sentence, but
its symmetric receive **exit** was not nailed down. - Fix in one line in 3.2.3: "if there is no
adapter session, a `DATA` that passed 8.4 goes to the payload hook of the test build, and in the
product build it is discarded and `<counter>` goes up". Before Phase 4 starts.

**W-20. The Phase 3 monotonic clock item does not fix the execution environment, and on Windows
locally a correct implementation fails.** (Verification criteria)
roadmap: "restart the server process and call `get_peers`, and the value continues and the
`elapsed_wall_fallback` counter is 0." `control_plane.md` 7.4 case table: "there is no `boot_id`
file (not Linux) | use a random number drawn at process start as `boot_id` | it falls back to the
wall clock on every process restart. That is what happens in local tests." Exactly as 7.4 writes
itself, running on Windows locally (DynamoDB local) makes the counter 1, so a correct implementation
`FAIL`s, and lowering the criterion lets real defects pass too. - Attach "run it on a Linux
deployment instance. On Windows locally, `elapsed_wall_fallback` being 1 is the specified
behaviour" to the item.

**W-21. The verdict axis "number of `VIP#` entries" of the Phase 3 retry item does not catch the
nonce-changing mutation of `join_room`, and there is no `internal` injection point.**
(Verification criteria)
Since the pool is the single `10.100.0.2`, a mutation that retries with a changed nonce ends with
`room_full` on the second request, so `VIP#` is 1 anyway. The axis holds only for `create_room`,
where two rooms are created. A connect timeout and `unavailable` are before the store is reached,
so `VIP#` is 0. To see nonce identity through the store, an injection that returns `internal` after
the first request commits is needed, and that point does not exist. - Split the verdict into
"`create_room`: `ROOM` count 1; `join_room`: the second response has the same `peer_id` and
`virtual_ip` as the first" and write a "fail after commit, right before the response" injection
point into the store layer.

**W-22. The "round trip succeeds" of the Phase 5 boundary size table cannot be run with the tool
Phase 4 nailed down (`send <n>` fixed at 100 bytes, direction mark at offset 40).** (Verification
criteria)
A 20 byte packet has no offsets 28-40, so the echo responder cannot read the direction mark, and a
responder built to the rule "do not answer a packet whose mark is `0x5A`" does not send it back.
For 576 and 1452 bytes the positions and fill rules for the packet number, time, and direction mark
are missing too. The reason Phase 4 nailed the words down ("who starts the round trip differs from
test to test") recurs in Phase 5. - Either fix a size argument and a variable size layout such as
`send <n> [size]`, or lower the 20 byte case to "one way arrival + comparison".

**W-23. The mandatory topology "same LAN" of spec M-4 is not in the Phase 4 verification.**
(Verification criteria)
spec: "mandatory test topologies | (1) the same LAN, (2) two different home networks. M-4 must hold
in these two to be accepted as minimum success". The Phase 4 verification runs only "two PCs on
different home networks". Reaching `CONNECTED` on the same LAN (local candidate path, convergence
behind the same NAT - the reason 10.4 chose its learning method) is not the UDP send and receive of
Phase 1, and Phase 9 "same LAN | baseline measurement" is P3. Dropping P3 (C-5) removes half of the
minimum success verdict. - Add "reaching `CONNECTED` on two PCs on the same LAN too. The learned
`peer_endpoint` is a local candidate" to the Phase 4 verification.

**W-24. spec A-2's "run both controlled failures (firewall UDP block -> `STUN_DISCOVERY_FAILED`)"
is in no Phase verification of the roadmap.** (Verification criteria)
The STUN failure of Phase 2 is "an address that does not respond", not a firewall block (the paths
differ). The Phase 9 item only says "count it separately" and does not say to run it. The strength
spec requires (running two scenarios) is nowhere in the roadmap as a verdict item. - Add an item
for running the two scenarios to the Phase 9 verification.

**W-25. The Phase 5 fuzz test puts random bytes into the socket, so it barely walks the chapter 8
pipeline.** (Verification criteria)
Three quarters of random first bytes end at the chapter 7 classification, and the rest pass the 4
byte magic only with probability 2^-32. Everything after 8.2 (the nonce table, the duplicate
suppression bitmap, the 8.4 IHL and checksum, the `CLOSE` reason, renegotiation) is executed
essentially 0 times. An implementation with truncation or overflow defects in those places passes
too. - Add a second stage that injects "a random payload with a valid header (magic, version, the
peer's `peer_id`, the pinned epoch) + type and length mutations" at the receive injection entry
point.

**W-26. NFR-10 is not in the roadmap requirement tracking table.** (Consistency + verification
criteria)
The tracking table: "no ID may be left untracked." It is the only omission among the 45 spec IDs.
The Phase 9 verification quotes NFR-10 twice, so the assignment is fixed and only the table is
missing. NFR-4 (direct implementation) is tracked in Phase 2, but no Phase verification judges it
(there is no code reading item). - Put NFR-10 into row 9 of the table and write "judged by code
review" for NFR-4.

### E. Gaps in the implementation contract (protocol 10.1, architecture 3.5)

**W-27. There is no verdict criterion for "other tunnel/VPN adapters" and for "active" in the
exclusion of local candidates.** (Implementer)
10.1: "exclude loopback, APIPA (`169.254.0.0/16`), our own Wintun adapter, and other tunnel/VPN
adapters from the interfaces." Loopback, APIPA, and our own Wintun are judged mechanically, but for
"other tunnel/VPN" there is no definition of whether it is `IfType`, a name string, or the presence
of a gateway, and for "active" whether it is `OperStatus` or holding an address. Getting it wrong
one way sends `HELLO` to an unreachable address every 200ms, and the other way removes the same LAN
candidate. It falls under rule 7 but has no case table and is not in a roadmap pre-start item. -
Either write the verdict inputs (`IfType` and `OperStatus` of `GetAdaptersAddresses`) and a case
table into 10.1, or hang the point in time on it as a Phase 4 pre-start item.

**W-28. Who does DNS resolution of the STUN server names, when, and what happens on failure is
missing.** (Implementer)
All four of the default list in 3.5 are names, so Phase 2 necessarily calls `getaddrinfo`. 3.2.8
moves DNS out of `[loop]` but covers only the single control server, and Phase 2 has no `[control]`
thread (3.2.1 "Phase 1-2 are three threads"). Whether it is a synchronous resolution before the
loop starts, whether the first IPv4 is taken when there are several results, and whether a server
that fails to resolve is treated as "a server that does not respond" and skipped or is a startup
failure, are missing. - Write "STUN names are resolved once before `[loop]` starts, and an entry
that fails is removed from the list with a `WARN`" and the same "first IPv4" rule as control_plane
3.2 into 3.5. Before Phase 2 starts.

### F. windows-prereq

**W-29. Chapters 3 and 7 fix the "our adapter" criterion differently. The chapter 3 criterion makes
you delete somebody else's adapter.** (windows-prereq lens)
Chapter 3: "whether we created it is checked together by whether `InterfaceDescription` is Wintun."
Chapter 7: "**the name and the driver description are not proof of ownership.** Anyone else can use
both the same way. ... The ownership mark is `InterfaceGuid` alone." The result of chapter 3 is
**deletion** (getting it wrong deletes another installation using the same name or another
Wintun-family adapter) and the result of chapter 7 is exclusion from a computation, yet the more
destructive chapter 3 uses the weaker criterion. Chapter 7 writes "the same rule as chapter 3",
hiding the mismatch. - Change the ownership verdict of chapter 3 to a match against the recorded
`InterfaceGuid`, as in chapter 7, and write that when there is no GUID record (first run, lost
state) it does not delete but creates under a different name or stops. Before Phase 6 starts.

**W-30. The "alternative range mechanism" and the "value the control plane tells you" that chapter
7 speaks of are not in the control plane document.** (windows-prereq lens)
Chapter 7: "if the blocking condition holds, pick an alternative range or stop.", "there is an
alternative range mechanism, so the cost is small too.", "if the range changes both sides have to
use the same value, so it must be a value the control plane tells you." Nothing in the four
operations and response fields of `control_plane.md` negotiates or announces a range, and 2.5 fixes
the range as a constant. The roadmap has no decision point either. Chapter 7 made its second and
third lines "blocking, not a warning" on the premise of a mechanism that does not exist. The only
behaviour actually possible is "stop". It is a case of a deferred decision with no point in time
anywhere (the second round B-3 kind). - Change "when blocked" in chapter 7 to "v1 stops.
Alternative range negotiation is undecided and its point in time is before roadmap Phase 6 starts"
and hang that item on the roadmap.

**W-31. Chapter 9's "poll until it becomes `Preferred`" has no termination condition. On
`Duplicate`/`Invalid` it waits forever.** (windows-prereq lens)
Chapter 9: "go to the next step after the `AddressState` of our address becomes `Preferred`. **Do
not wait by time.** ... poll the state." If DAD detects a conflict it becomes `Duplicate` and never
turns into `Preferred`. If the adapter disappears the query returns an empty result or an error.
Implemented as the document says, the client hangs in that case, leaving the leftovers of chapter 3
behind, and a human has to kill it. It violates rule 6 (cleanup on abort, range of result
interpretation). - Add "`Duplicate` and `Invalid` stop as a failure and run the chapter 3 cleanup.
Put a polling cap and take the same path when it is exceeded" to the verdict criteria.

---

## 4. Info (27)

One line each. The location is in parentheses, the lens as an initial (tunnel T / control C /
consistency K / implementer I / verification V / adversarial A / prereq P / integrator U).

1. Two rows of the 9.4 transition table, `DATA` and `CLOSE`, are cut off outside the table by a blank line, so the rendered table is missing those two rows (protocol 9.4 source lines 558-561) - delete the blank line (T, U)
2. Whether the first `PING` send is "immediately on entry" or "after 5s" is undecided (protocol chapter 11. Only the `KEEPALIVE` row states one immediate send) (T)
3. The chapter 11 timer table is missing five time values from the body (retired list 2 minutes, nonce retirement 2 minutes, probe 5 seconds, minimum renegotiation interval 1 second, `pending_pings` 5 seconds), and architecture 3.2.4 "about 6 per session" does not match either (T)
4. The wording of 4.5 "before any side effect" and 8.3 "epoch handling comes before duplicate suppression" conflicts. The result is the same (protocol 4.5, 8.3, chapter 15) (T)
5. 8.2 "`HELLO` proves itself with the nonce" is an overstatement. What proves it is the echo in `HELLO_ACK`, and chapter 2 is accurate (T)
6. There is no counter name for `sendto` failures of types other than `HELLO_ACK` (there is only `tx_err_ack`). The architecture chapter 9 metric "keepalive local send error" has no name either, and the 3.2.3 `recvfrom` "other errors" counter has no name (T, I)
7. The path by which `quit` causes `shutdown()` is not in architecture. `quit` appears only in the roadmap (architecture 3.2.3, 3.2.7) (T)
8. Steps (1)-(6) of the 3.2.7 shutdown order have no step for the record file end line and the full counter dump, and the thread that writes the record file is not in the 3.2.5 ownership list. Chapter 9 points at 3.2.7 (T)
9. The split sending of `send <n>` is not reflected in the `busy` computation of 3.2.3, so the remainder stretches out in units of the timer interval (roadmap Phase 4, architecture 3.2.3) (T)
10. Whether the duplicate `HELLO_ACK` of `CONNECTED` ("ignore, `dup_ack`") also means blocking the idle timer refresh and learning is undecided (protocol 9.4) (T)
11. `expires_in_s` uses `ceil`, so 0 cannot come out for a live room. "0 can come out (under 1 second left)" is the explanation for `floor` (control_plane 5.1) (C, U)
12. The `room_full` condition is written the opposite way in "the pool is not empty" (4.1, 4.3) and "the pool is empty" (5.1) (C)
13. The 5.1 transition figure's "the second peer's register_candidate succeeds" should be "the second registration" (if the participant registers first, the host's registration is the trigger), and the `create_room` same-nonce row of the allowed state table has no `room_expired` in the `expired` column (control_plane 5.1, 4.2) (C)
14. The 3.2.1 `[loop]` row "it shares only the telemetry queue and the shutdown/cleanup events" leaves out the console queue and the control request and response queues (architecture 3.2.1) (C)
15. DNS resolution failure = "startup failure" has neither a chapter 8 failure code nor a log event. Nowhere is the boundary with "control plane request failure" written (control_plane 8.2, 8.4) (C)
16. The owning thread of the 8.3 retry and the place of the 1 second interval timer are missing. If it is `[loop]`, 3.2.4 has no such timer; if it is `[control]`, it conflicts with 3.2.8 "the only state" (C)
17. The result of a request that reuses the same nonce for a different room differs between the pre-read path (`bad_request`) and the transaction cancel path (the response of the first room) (control_plane 6.3) (C)
18. Although "all writes are conditional", no condition is written for the `PEER#host` and `VIP#10.100.0.1` puts of `create_room` (control_plane 6.3) (C)
19. The 7.2 pseudocode does not raise the `rate_limited` and `unavailable` counters. They are only in the 7.5 list (control_plane 7.2, 7.5) (C, K, U)
20. The `room.ready` log event has no field that distinguishes rooms, so with two or more rooms "exactly once per room" cannot be judged. `room.created` and `vip.claimed` carry `peer_id` (control_plane 7.5) (K)
21. `client_nonce` is a secret with the same power as `peer_token` (anyone who knows it gets the token in the same response), yet the 1.2 table does not count it as a secret; there is also no verdict for a re-request with the same nonce after the `NONCE#` TTL deletion creating a new room (differing from 4.2 "it does not create a new room"), nor for the cross use of calling `create_room` with a `join_room` nonce (control_plane 1.2, 2.4, 4.2, 6.5) (A)
22. The 3.3 server parsing case table does not fix inputs outside the subset (header folding, `Content-Length` value forms `+17`, ` 17 `, full width, duplicates of headers outside the table, missing `Host`, `Content-Type` mismatch, request line variants, a body longer than `Content-Length`), the check order for combined defects, or the rows that pass just inside the boundary (4096, 2048, port 1, 65535) (control_plane 3.3, 4.4) (A, I, V)
23. protocol chapter 3 "there are four tables with caps" leaves out the nonce retirement list of 5.1 (no cap needed because the input is local only) and the "per epoch" duplicate suppression state of 4.5 (reading 8.3 it is one state initialised and reused) (A)
24. The justification for row 1453 of the roadmap Phase 5 boundary table, "`recvfrom` fails with `WSAEMSGSIZE`", is wrong. 20+1453 = 1473 bytes is the length comparison path (`n > MAX_DATAGRAM`) fixed by architecture 3.2.3 and the Phase 1 item. The conclusion "it does not reach" stands (K, V)
25. The spec "cost of removing a priority" table leaves out NFR-5 and NFR-6 lost at P2 and NFR-7 lost at P3. The P1 row also writes the NFR-3 verification, so the criterion differs (spec C-5) (K)
26. Places where the means of execution for a Phase verification item is missing - the entry point for registering 17 retired list entries (Phase 4. Through the defined interface the cap is 8 renegotiations), the entry point for the `sequence` 32 bit boundary (Phase 5), the load generation means for checking the drain budget mutation (Phase 1), the justification for the 30 seconds of the Phase 4 budget (c) (probe re-registration bottoms out at about 50 seconds), the observation point for the rejoin "the candidate list is empty" (the `join_room` response carries no candidates), "query two STUN servers right before punching" being worded differently from the 8.4 order (step 5), the Phase 2 deliverable "Local endpoint 192.168.x.x" being a different thing from the `local` of `socket.bind` (`0.0.0.0:port`), the Phase 6 adapter read counter name and the build scope of the ICMP responder, the unrecorded exception between `[console]` raising `console_queue_dropped` and 3.2.5 "all counters are owned by `[loop]`", the Phase 1 two machine test not pointing at the inbound block of windows-prereq chapter 2, Phase 3 and Phase 4 both writing candidate collection and polling as their own work, whether the retry "up to 3 times" is the total number of attempts or of retries, and the argument parsing error behaviour and `peer_id` notation (V, I)
27. windows-prereq - chapter 2's "all 6 are `blocked`" has no justification for attributing it to the firewall (tool, path, firewall on/off comparison) while protocol 10.4 attributes the same kind of observation to NAT; the chapter 5 progress verdict query uses `-ErrorAction SilentlyContinue`, which chapter 0 limited to reference use only; chapter 10's "both an Elastic IP and DNS" has no check method or pass condition for the DNS side, and control_plane 3.2 also allows an IPv4 literal; the chapter 4 PE header reading has no `PE\0\0` signature check, no offset range check, and no `try/finally`; architecture chapter 10 "the two language trees have the same files" conflicts with the Korean-only rule for `plan.md`; the roadmap header "autumn 2026" may be outside the intent of the date rule, so whether to keep it should be decided (P, K)

---

## 5. Follow-up Placement (proposed)

This table is the judgement of this point in time. **The repository owner decides what to act on
and in what order.** Live tracking belongs to [`../../kor/plan.md`](../../kor/plan.md).

| When | What |
|------|------|
| Before Phase 1 starts (wording fixes) | B-2, B-3, B-4, W-15 and other one-sentence or one-paragraph fixes. There is no code yet, so now is cheapest. info 1, 11, 12, 19, 24 are the same kind |
| Before Phase 1 starts (fixing values and contracts) | W-17 (Phase 1 peer endpoint input and raw send), W-18 (test timer and event key). The Phase 1 verification depends on them directly |
| Before Phase 2 starts | W-9 (STUN deadline unit and switching), W-28 (STUN name resolution) |
| Before Phase 3 starts | **B-1 (readiness lost update)**, W-7 (`get_peers` deadline base point), W-8, W-10 (4.4 case table), W-11 (host rejoin argument), W-12 (6.4 refill and table entries), W-20, W-21, W-26. It is the same place as the scheduled work of moving the case tables of `control_plane.md` into `tests/` |
| Before the Phase 4 transition table is implemented | **W-1 (timers after renegotiation)**, W-2 (behaviour after `FAILED`), W-3 (`CLOSED` entry and retention), W-4, W-5 (position of the `virtual_ip` check), W-6 (retired list target), W-19 (`DATA` exit in a build with no adapter), W-23 (same LAN verification), W-27 (local candidate verdict), info 2-10 |
| Before each of Phase 5-9 starts | W-22, W-24, W-25, W-13, W-14, W-16 (the acceptance limit prose only has to be there before the final report), W-29, W-30, W-31 (Phase 6), the rest of the info items |

Among the four blockers, **none blocks Phase 1 code.** B-2, B-3, and B-4 are wording fixes that can
be done immediately and carry only the cost of being left alone, so doing them before the start is
right. B-1 is a design change to the storage contract, so keeping it as a pre-Phase 3 item is right.

## 6. Things Judged to Be Scheduled Work and Not Raised

These are what the lenses suspected but were dropped because a point in time is already hung on
them in a roadmap pre-start item, chapter 10 of `control_plane.md`, or the waiting list of
`plan.md`. They are kept so that the next session does not suspect the same places again.

| Suspicion | Where the point in time is |
|------|----------------------|
| Local record file path and line format, empty RTT notation, how to join two lines. M-6 undecidable | roadmap before Phase 4 starts |
| keepalive mapping lifetime measurement procedure, probe format, EC2 send tool | roadmap before Phase 4 starts |
| Automatic retry after `TUNNEL_DROPPED`, re-polling by the surviving side | roadmap Phase 5 / protocol 10.4 / control_plane chapter 10 |
| The API representation of a Wintun injection failure and counter granularity, where the publisher string and hash are kept | roadmap before Phase 6 starts |
| Fixing the MTU by measurement | roadmap Phase 7 |
| Telemetry service schema, authentication, period, port, resource isolation, table and IAM separation | roadmap before Phase 9 starts / control_plane chapter 10 |
| Table name, region, capacity, the real EIP and DNS values, IAM credentials, free tier, clock premises (`clock_gettime`, `boot_id`) confirmation | roadmap before Phase 3 starts |
| The six case tables of `control_plane.md` existing only in the document | migration to `tests/` when roadmap Phase 3 starts |
| Whether 3600 seconds for room expiry is appropriate | control_plane chapter 10 (after Phase 8) |
| TLS, caller authentication | spec stretch / control_plane 1.2 |
| Firewall inbound measurement, physical verification of windows-prereq, the absence of `experiments.md` | plan.md waiting list and documentation debt |
| `MAX_CANDIDATES` and `PUNCH_DELAY_MS` existing in both protocol and control_plane | control_plane 2.6 states that it is a copy and why (rule 5 exception) |
| Live documents linking to ADRs (protocol 10.4, several places in control_plane, windows-prereq chapters 1 and 2) | It is a question of interpreting the CLAUDE.md rule and is a pattern across the repository, so it was left outside the scope of this audit. The owner decides |

## 7. What This Audit Taught

- **The repairs of the second audit are holding.** In the exhaustive cross-check of constants,
  counters, error codes, event keys, and 234 section number references, mismatches are 0. The
  transition table the second round filled (W-1), the startup inputs (B-4), the thread ownership
  (B-5), and the control plane schema (B-3) are all in HEAD.
- **The next defect is next to the place that was fixed.** It is a re-confirmation of rule 4. Once
  the transition table was filled, the timer lifecycle after it and the behaviour after a terminal
  state were left empty (W-1 to W-4), and once the renegotiation cap went in, the attack
  description in chapter 2 was weakened to "reachable" and became an understatement (B-3). The
  sentence that fixed B-6 of the second round is B-3 of the third. **When lowering the strength of
  a sentence to take up a finding, the lowered sentence has to be traced again by scenario.**
- **A new document loses the verdict column of its case tables in the first full-text audit.** The
  "reject" of the 4.4 table of `control_plane.md` was not a response (W-10), and row 13 of the 6.4
  table presumed a refill unit that is not in the body (W-12). A case table can be moved into a
  test only when its expectation column is **the response itself**.
- **No document owns the test harness the verification items presume.** The test timer (W-18), the
  receive exit hook (W-19), the Phase 1 raw send (W-17), and the retired list and `sequence` entry
  points (info 26) are all mentioned only inside the roadmap verification sections. That is the
  place where architecture should have one "test build" section.
- **A sentence with a calculation in it gets the calculation redone.** "Hundreds of thousands of
  years" (W-15) was wrong by three orders of magnitude, and "the 11th" (B-4) was off by one. Both
  are the kind a script, not a reviewer, can catch.
- **Where three lenses point independently is the biggest hole.** The same as the second round.
  This time it was the `get_peers` deadline base point (W-7) and the 4.4 case table (W-10).
