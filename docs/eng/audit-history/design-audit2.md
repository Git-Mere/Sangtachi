# Full Design Audit, Second Round (2026-09-21)

**Target:** the complete text of [`../spec.md`](../spec.md), [`../roadmap.md`](../roadmap.md),
[`../architecture.md`](../architecture.md), [`../protocol.md`](../protocol.md),
[`../windows-prereq.md`](../windows-prereq.md)
**Point in time:** right before Phase 1 implementation starts, at commit `97de683`
**Reason:** all six follow-ups from the first audit (2026-09-14) were closed, and we decided to
check the trustworthiness of the whole document set once more before writing code

> **This document is an audit record.** It keeps the result and the judgement of that point in
> time. **Current work tracking belongs to [`../../kor/plan.md`](../../kor/plan.md).** The chapter
> and section numbers and the quotations in the body are as written at commit `97de683`, and they
> are not corrected here even if the documents change later.

---

## 1. Method

Against the complete text of the five finished core documents, five independent reviews with
different lenses ran in parallel. Two lenses (verification criteria, adversarial input) were added
to the three lenses of the first audit.

| Lens | Question |
|------|------|
| Scenario trace | If one packet, one failure, one restart, and one control server death are followed end to end, do the documents answer |
| Consistency cross-check | Are constants, timers, state names, and requirement IDs the same across documents. Is there a place where a copied claim differs in strength |
| Implementer's view | Can Phase 1-4 be coded under the rule "do not decide in code a value that is not in the documents" |
| Verification criteria | Does a correct implementation pass each criterion, does a defective implementation pass, is the verdict procedure executable |
| Adversarial input | Is there a hole not written in the documents as "intentionally not guaranteed". Unbounded tables, forged sources, reflection paths |

Each lens reported that it searched the five documents before raising a finding of the "missing"
kind. **That report was not taken on trust**; at integration time I cross-checked every blocker
and the major warns against the HEAD text myself. The certainty of the findings below differs
between those that went through that cross-check and those that depend on a lens report. Findings
raised independently by two or more lenses were merged into one (noted on each finding).

**Totals: blocker 6, warn 24, info 24 — 54 findings.**

The nature differs from the first audit. The 20 blockers of the first round were of the kind
"the wire protocol itself does not exist". This round's blockers are not in the protocol body but
at its **edges**. Two verification procedures, one missing schedule for a deferred decision, one
startup input, one thread ownership, one security statement. The consistency lens cross-checked
constants, offsets, timers, state names, and module names across documents, and **within that
scope no mismatch came out.** The list of checked items is in that lens's report.
`windows-prereq.md` was effectively clean under both the adversarial lens and the decidability lens.

**No blocker stops Phase 1 from starting.** The six are placed as follows. **Two (B-1, B-6) end
with a wording fix now.** Three (B-3, B-4, B-5) are before Phase 2-3 start, and one (B-2) is
before Phase 4 start. Details in chapter 5.

---

## 2. Blockers (6)

### B-1. The roadmap Phase 4 verification expects a counter a correct implementation cannot produce — `drop_stale_epoch`

(consistency lens)

- Location: `roadmap.md` Phase 4 verification / `protocol.md` 8.1 #8, 8.2, 9.5
- Source text: "after a client restart, delayed packets from the previous instance are discarded
  with `drop_stale_epoch`"
- Problem: if the protocol rules are followed as written, `drop_stale_epoch` does not occur in
  this scenario. When the restarted peer's new `HELLO` (different epoch) triggers the 9.5
  renegotiation, step 1 **registers the previous epoch in the retired list**, and 8.1 #8 (retired
  list check, `drop_retired_epoch`) runs **before** the epoch comparison of 8.2/9.3
  (`drop_stale_epoch`). So delayed packets from the previous instance match the pinned epoch and
  are **accepted** before renegotiation, and are discarded with `drop_retired_epoch` after it. For
  `drop_stale_epoch` to appear, the 2-minute retired list would have to expire first, and that is
  not a "delayed packet" test. A correct implementation cannot pass this criterion
  (recurrence-prevention rule 3). The implementer either believes correct code is wrong or lowers
  the criterion.
- Proposal: change the expected counter to `drop_retired_epoch`, and state in the verification
  text that packets arriving before renegotiation are accepted by the rules.

### B-2. The Phase 4 keepalive mapping-lifetime measurement procedure contradicts the repo's own measurements

(verification criteria lens + implementer lens)

- Location: `roadmap.md` Phase 4 verification (keepalive interval measurement) / `protocol.md`
  10.4 measurements / `windows-prereq.md` section 2 / `architecture.md` 6.1, `protocol.md`
  chapter 7
- Source text: "use the Phase 3 AWS EC2 control server as an external observation point, send
  periodic UDP probes to that socket's public endpoint, and measure mapping lifetime by whether
  the probes arrive. The client does not answer the probes and only records arrival"
- Problem: the client socket has never sent UDP to EC2 (the control plane is TCP). A probe from
  EC2 is **unsolicited inbound** with a wholly new source IP and port, and two of the repo's own
  measurements say it does not arrive. (1) protocol 10.4: in all 3 measured network pairs, "UDP
  with the same source IP and only a different source port is blocked in both directions" (EC2,
  with a different IP too, is blocked even more). (2) windows-prereq section 2: "unsolicited
  inbound ... `blocked` in all 6 cases". So the probe arrival count is 0 from the start, and
  "the mapping expired" cannot be distinguished from "the filter was closed all along". Trusting
  this procedure as is misjudges the mapping lifetime as near 0 and mis-tunes the keepalive value —
  a case of believing a wrong result is right. Additionally (a) the EC2 capability of sending UDP
  probes is neither in the 6.1 operation table nor in the Phase 3 tasks, (b) there is no probe
  packet format, and (c) under the chapter 7 classification an unknown packet only increments the
  `drop_unclassified` counter, so the instrument itself — recording arrival time per probe — is
  not defined.
- Proposal: right before idling starts, the client sends once from that socket to the EC2 probe
  endpoint to open the path, and the document states that what is measured is not "mapping
  lifetime" but **the filter+mapping lifetime of that path** (this is exactly the value the
  keepalive has to refresh, so it is sufficient for the purpose). Fix the probe format, the
  client's recording method, and which deliverable (Phase 3 or 4) owns the EC2-side sending tool.

### B-3. The start time of the separate control plane schema document is nowhere

(verification criteria lens + implementer lens)

- Location: `protocol.md` chapter 14 / `roadmap.md` Phase 3 / `spec.md` M-3, T-6
- Source text: "the request/response encoding, error schema, and state transitions of
  `register_candidate` and `get_peers` are outside this document's scope and **must be fixed in a
  separate document.**"
- Problem: this is a deferred decision with the same structure as M-6, but of the four
  intentionally deferred decisions (local record format — before Phase 4 start, automatic retry —
  Phase 5, MTU confirmation — Phase 7, and this one) it is **the only one with no start time in
  the roadmap.** Phase 3 implements the Python server and the C++ client together, and neither
  side can be written without the wire encoding (HTTP or not, URLs and methods, error
  representation, `not_ready` representation, port — the 8000 in windows-prereq section 6 cannot be
  told apart as example or decision from that section alone). The encoding of
  `create_room`/`join_room`/`report_telemetry` (FR-14, T-6) is not even in chapter 14's scope.
  By the rules it cannot be decided in code, so Phase 3 start is either blocked or breaks the
  rule. The M-3 verdict cannot be executed without this either, yet unlike M-6 there is no "cannot
  be judged before it is decided" sentence.
- Proposal: add a "fix the control plane schema document before start" item to roadmap Phase 3,
  and attach the same not-judgeable sentence as M-6 to spec M-3.

### B-4. The values and delivery method of the client startup inputs are nowhere

(implementer lens)

- Location: `roadmap.md` Phase 2-3 tasks / `spec.md` M-2 / `architecture.md` (absence confirmed)
- Source text: "send a request to a public STUN server" (roadmap Phase 2) — which server and how
  it is delivered: absent
- Problem: the client needs five or more inputs at startup (STUN server address list, control
  server address, room create/join choice, `room_id`, own identifier), and neither the values nor
  the delivery method (CLI arguments or a configuration file) is in any of the five documents.
  Under the rule "do not decide in code a value that is not in the documents", Phase 2 (STUN
  server list) and Phase 3 (control server address, room join input) are blocked the moment they
  start. Phase 1 can get by with temporary test arguments.
- Proposal: fix the startup input list and delivery method as one section in architecture, and
  decide the source of the default STUN server list. Before Phase 2 start.

### B-5. No thread owns the control plane TCP calls and DNS resolution — `[loop]` blocking is scheduled at the document level

(implementer lens + scenario lens)

- Location: `architecture.md` 3.1, 3.2.1, 3.2.2, 3.2.6 / `protocol.md` chapter 11
- Source text: the thread table (3.2.1) and the wait handle table (3.2.2) have no control plane
  TCP. "[telemetry] | consumes the metric queue, uploads to the control plane" — only the
  telemetry upload is isolated.
- Problem: no thread is designated to perform `create_room`/`join_room`/`register_candidate`/
  `get_peers` polling/`report_connection`. protocol chapter 11 defines the `get_peers` poll
  (500ms) as a timer, and timers belong to `[loop]`, so the natural implementation is **to put
  blocking TCP calls inside `[loop]`**. `report_connection` goes out right after entering
  `CONNECTED`; if the control plane is dead at that moment (the situation the Phase 4 verification
  creates on purpose), the TCP connect spends tens of seconds in `[loop]` on SYN retries, and
  during that time `HELLO` retransmission (200ms), keepalive (15s), and the response to the
  shutdown event all stop. This is the very reason 3.2.6 rejected the 4-thread design ("a TCP
  upload to an unresponsive control plane blocks the timer thread for tens of seconds") coming
  back through another door, and it violates NFR-3 ("a control plane failure does not affect
  existing tunnels") at the design level. STUN server resolution (`getaddrinfo`) is in the same
  place.
- Proposal: fix the owning thread of control plane I/O (or a non-blocking/timeout contract) in
  architecture 3.2. At minimum, move `report_connection` and polling out of `[loop]`. Before
  Phase 3 start.

### B-6. The forged `HELLO` attack premise in protocol chapter 2 is overstated — it works without `session_epoch`

(adversarial lens)

- Location: `protocol.md` chapter 2 / cross-checked with 8.1, 5.1, 9.3, 9.5
- Source text: "an on-path or off-path attacker who knows or guesses `peer_id` and
  `session_epoch` can do the following. ... induce session renegotiation with a forged `HELLO`"
- Problem: the trigger for renegotiation (9.5) is "a `HELLO` with an epoch **different** from the
  pinned one". The checks a forged `HELLO` has to pass are only the 8.1 `peer_id` match (#7) and
  absence from the retired list (#8), and the 5.1 `virtual_ip` match; the epoch **only has to
  differ** from the pinned value (the attacker can use any random number). `virtual_ip` is
  predictable as `10.100.0.1/2`. So the premise of this capability is `peer_id` alone. The
  consequence is also large: 9.5 step 1 puts the real peer's live epoch on the retired list for 2
  minutes, so every subsequent packet from the peer (including `HELLO_ACK`) is dropped with
  `drop_retired_epoch`; we return to `PUNCHING`, cannot re-raise the flags, and end in failure
  (9.6) after 10 seconds, and the peer, whose incoming traffic from us has stopped, dies with
  `TUNNEL_DROPPED` after 50 seconds idle (automatic retry undecided). One forged `HELLO` ends both
  sessions. Chapter 2 requires that "the final report must state this limitation", but with the
  current sentence the report would state the attack premise stronger than it is — a place where a
  wrong security claim is believed to be right.
- Proposal: separate "inducing renegotiation with a forged `HELLO`" from the capability list in
  chapter 2 and state precisely that it is "possible with `peer_id` (and the predictable virtual
  IP) alone". A one-sentence fix.

---

## 3. Warns (24)

Written by group. Parentheses give the lens that found it.

### A. State machine and renegotiation (protocol chapters 9-10)

**W-1. The 9.4 transition table does not cover `IDLE`, `FAILED`, `CLOSED`, or the punch-wait interval.**
(consistency + implementer + scenario. Three lenses raised it independently)
9.1 defines six states, but the transition table's columns are only `PUNCHING`/`HANDSHAKING`/
`CONNECTED`. The socket is bound at startup and kept until shutdown (chapter 6), so packets
arrive in the other states too. In particular the interval (ready ~ punch start) is an unnamed
interval where the peer's `HELLO` actually arrives on the normal path because of rendezvous error
(10.2, up to ~1 second). Whether to answer or drop here, and what to do with a `HELLO` (the peer's
retry) arriving after `FAILED`/`CLOSED`, the documents do not decide. The statement in
`architecture.md` 5.3 that "the transitions for every state x every received packet are in the
9.4 transition table" is a claim wider than the table's actual scope. The probe nonce echo
`HELLO_ACK` row is also not in the table (it is only in 8.2 and 10.4). — Add columns for the three
states (or one line "all other states discard + counter") and a punch-wait interval rule to 9.4,
and bring the 5.3 claim down to the table's scope.

**W-2. The basis for the failure code verdict differs between `architecture.md` chapter 8 and `protocol.md` 9.6.** (consistency)
When only `got_ack` holds (a timeout on the "`HELLO_ACK` first in PUNCHING" path that 9.4 states
is normal), the architecture chapter 8 basis ("peer `HELLO` not received") gives
`HOLE_PUNCH_TIMEOUT`, and the protocol 9.6 basis ("only one holds") gives
`PEER_HANDSHAKE_FAILED`. architecture 5.3 and chapter 8 do not agree with each other either. The
classification axis of the Phase 9 failure tally splits. — Rewrite the chapter 8 basis in terms of
the 9.6 flags.

**W-3. The meaning of `session_epoch` remains stale in `architecture.md` 5.1.** (consistency)
The "per running instance" that protocol explicitly denied is still in architecture as it was
("this running instance" vs protocol 4.3 "drawn **per session attempt**. Not per process"), and
"packets of another epoch are discarded from then on" contradicts the 9.3 rule that a `HELLO`
with another epoch = renegotiation. A case of recurrence-prevention rule 5 (copy, then update only
one side). — Bring 5.1 to the protocol 4.3/9.3 meaning, or reduce it to a link.

**W-4. Reconnection after a peer restart does not work if the documents are followed, and no
document states that limitation.** (scenario)
If B restarts: new socket → new NAT mapping → new public port. The surviving A does not receive
candidates again (9.5 item 8), and B's new `HELLO` arrives at A as "same IP, different port",
which is exactly the blocked case 10.4 (c) measured (all 3 pairs, 0 of 20 shots arrived each).
Renegotiation gets no trigger, A holds the dead session until 50 seconds idle, and B gets
`HOLE_PUNCH_TIMEOUT` after 10 seconds. Yet the wording of 4.3 and 9.5 ("delayed packets from a
previous attempt after a restart or retry...") reads as if restart were handled. 10.4 (c) wrote
this limitation only for NAT rebinding, and there is no statement that the more common event, a
process restart, hits the same failure. — State in 9.5 or 10.4 that "only a retry on the same
socket is a recovery target, and recovery after a restart (new socket) is not guaranteed in v1".
This is also the place to widen the scope of the decision in
[ADR 0002](../decisions/0002-no-rebinding-recovery.md) to restarts.

**W-5. The `HELLO` send target in `PUNCHING` after renegotiation differs between 10.3 and 10.4.** (scenario)
10.4 uses "punching = before learning" as an equation ("if not yet learned (punching), to all
candidates"), but the 9.5 renegotiation keeps `peer_endpoint` (item 6) while returning to
`PUNCHING` (item 7), creating a state where that equation is broken. Here there are two answers to
whether `HELLO` retransmission goes to all candidates (10.3) or only to the learned one (10.4). —
Fix the target in one sentence.

**W-6. Whether `peer_id` and the virtual IP are kept on rejoin is undecided.** (scenario)
When a restarted peer attaches to the room again, if a new `peer_id` is issued everything dies at
the peer's 8.1 #7 (`drop_unknown_peer`), and if a new virtual IP is issued at `drop_vip_mismatch`,
and neither becomes a trigger for renegotiation. This item is not among the six control plane
contracts in protocol chapter 14, and the roadmap Phase 3 verification ("joining the same room
twice does not assign a duplicate virtual IP") does not cover rejoin by the same peer. — Add a
seventh contract to chapter 14 (rejoin returns the same values, or is defined as an error).

### B. Unbounded tables and forged sources (protocol chapters 5, 8, 10)

**W-7. The tentative path (probe) table has no upper bound, and the path verification probe is a third-party reflection path.**
(implementer + adversarial)
`MAX_CANDIDATES` (8), `MAX_PENDING_PINGS` (16), and `MAX_DRAIN` (64) have upper bounds, but only
the probe table has none (5.1 "one drawn per tentative path, kept for 5 seconds"). An attacker who
knows the session identifiers (the premise chapter 2 already assumes) can spray forward packets
with forged sources, the table grows without limit, and a verification `HELLO` goes out per
tentative path to the forged address — an arbitrary third party. There is almost no
amplification, but it is reflection, and it stays outside chapter 2's promise that "paths that
harm others are blocked even in v1". — Add a `MAX_PROBE_PATHS` constant with its overflow policy
and a probe send rate limit to chapter 3, and write the remaining reflection in chapter 2 as an
accepted limitation.

**W-8. The epoch retired list has no count limit, and there is no limit on renegotiation count or rate.** (adversarial)
Each forged `HELLO` causes one renegotiation (B-6) and adds one retired list entry, and 8.1 #8
looks up that list for every received packet. Because 9.5 step 7 resets the punch deadline to 10
seconds on every renegotiation, sending forged `HELLO`s continuously keeps the session in
`PUNCHING` forever. There is no overall establishment deadline. — Add a retired list count limit
and a renegotiation rate limit as chapter 3 constants.

**W-9. The 10.1 hygiene check is not applied to reply destinations.** (adversarial)
The address hygiene of 10.1 (rejecting broadcast, multicast, unspecified, loopback, port 0)
applies only to the **candidate list**. But the `HELLO_ACK` reply to a `HELLO` goes "directly to
the source of that datagram" (9.4, 9.5 item 6), and the source can be forged, so a `HELLO` with
its source forged to a broadcast/multicast address steers the client's transmission there. The
9.5 argument "one response and no amplification" implicitly assumes a unicast source. — Add "the
source passes the 10.1 hygiene rules" to the 8.1 common checks, or widen the scope of 10.1 to
"every destination this protocol sends to".

**W-10. The handling of a `HELLO_ACK` that echoes a valid nonce but carries an epoch different from the pinned value is undefined.**
(adversarial)
8.2 decides only up to "if the epoch is not pinned, pin it with this packet", and does not decide
whether to accept or drop a valid-nonce `HELLO_ACK` carrying an epoch different from an already
pinned value. It actually happens when the peer crashes and restarts during punching. Either way
converges, but the document does not decide, so the implementer invents — which hits the chapter
1 declaration "if there is a place where a value is not decided, that is a defect of this
document". — Add one row to the 8.2 table.

### C. Control plane (protocol chapter 14, architecture chapter 6)

**W-11. The control plane's trust assumptions are in no document.** (adversarial)
The chapter 2 threat model covers only packet attackers and not the control plane path. The fact
that API callers are not authenticated, the assumption that the server is trusted, and the
consequences (room hijack — any caller who knows `room_id` joins as the second peer, receives
candidates, and becomes the game opponent; candidate forgery; IP exposure; a malicious server
breaking the contract) are not written. If this is accepted for v1, it has to be written as
accepted. The limitation statement in the final report that chapter 2 requires would also miss
this path. — Add one paragraph of trust assumptions to chapter 14 or chapter 2.

**W-12. The entropy and allocation method of `peer_id` (and `room_id`) are absent.** (adversarial)
The chapter 14 contract requires only uniqueness, so sequential allocation (1, 2, ...) also
satisfies it. Then the "guesses" in chapter 2's "knows or guesses" becomes trivial, and the B-6
attack works off-path. — Add one line "allocated as CSPRNG 32 bits" (or an intentional acceptance
of sequential allocation) to the contract.

### D. Verification criteria (roadmap, spec)

**W-13. The Phase 2 verification "matches the result of an external IP lookup service" conflicts with the downgrade of spec M-2.**
(verification criteria)
M-2 lowered the same observation to "**recorded for reference only**, because it is a TCP path
and can differ from the UDP send path", but Phase 2 uses it as a pass criterion. In an environment
where the public IP differs between TCP and UDP (some CGNAT), a correct STUN implementation fails
the verification (rule 3). It is also a violation of rule 5, align all copies when lowering the
strength. — Lower the Phase 2 item to "record whether it matches, for reference. The verdict is
successful parsing of the responses from two STUN servers".

**W-14. No Phase verification item executes the M-2 verdict procedure (same socket, two STUN servers, identical observation).** (verification criteria)
Phase 2 says "does not require identical results" (correct at that stage), and the verification
list of Phase 4, where M-1 to M-6 are assigned, has no STUN item. There is no place in the roadmap
where the M-2 verdict runs. — Add one line to the Phase 4 verification.

**W-15. The "timestamped UDP echo" of the Phase 4 RTT baseline presumes a packet that is not in the protocol.**
(verification criteria + implementer)
protocol chapter 5 has no echo type (`PING`/`PONG` by design carry no timestamp on the wire), and
the chapter 7 classification discards out-of-protocol datagrams as `drop_unclassified`. The
receiver, if it follows the documents, drops the traffic this verification requires. — Fix the echo
method (for example, carry a synthetic inner IPv4/UDP in `DATA` and have the test build echo it).

**W-16. NFR-2 is mapped to Phase 5, but the Phase 5 verification has no such test.** (verification criteria)
NFR-3 exists as the "control plane outage test", but there is no item that judges NFR-2 (the
tunnel's independence from the game). The nearest one, the Phase 7 file transfer test, is not
linked to NFR-2 in the traceability table. — Move NFR-2 to Phase 7 and name the file transfer as
its verdict, or add a test to Phase 5.

**W-17. The Phase 1 criterion "the periodic timer keeps firing" has no verdict axis as a value.** (verification criteria)
There is no allowed delay for "keeps", so if there are gaps in the load an implementation without
a budget also passes. This is the same place as a failure the repo already went through ("we
looked at the count when we should have looked at the interval"). — Write a value in the form
"during N seconds of sustained load, consecutive firing intervals of a timer with period T are at
most k times T", and confirm it with a budget-removal mutation.

**W-18. The "related requirements" in the roadmap Phase headers differ from the requirement traceability table.** (consistency)
The same mapping is in two places with different content (rule 5). Throughout Phase 2-9 the
headers miss IDs, and Phase 4 even differs in strength (traceability table "FR-13 (partial)" vs
the header's unconditional "FR-13" — FR-13 completing in Phase 5 disagrees with the spec C-5
table). If a per-Phase audit takes the header as its scope, that Phase's NFR verification is
missed. — Replace the ID lists in the headers with a link to the traceability table, or make them
identical.

### E. Gaps in the implementation contract (architecture, protocol chapter 6)

**W-19. There is no UDP socket bind rule (address, port).** (implementer)
Whether `INADDR_ANY`, whether port 0 or fixed, is absent. It is a real decision that affects
local candidate registration (10.1) and NAT mapping behaviour on re-run, and the Phase 1 socket
wrapper needs it from the first line. — Add a one-line contract to protocol chapter 6.

**W-20. There is no logging contract.** (implementer)
There is no destination, format, or level scheme, yet the Phase 2-3 verifications use "record"
of `STUN_DISCOVERY_FAILED` etc. and "confirm in the log" as verdict means. Whether that "record"
is the M-6 local record file or a general log is also ambiguous. — Fix a minimum contract in
architecture and state which "record" the Phase 2-3 items belong to.

**W-21. The random source of `session_epoch` is unspecified.** (implementer)
The nonce and transaction ID are explicitly "OS CSPRNG" (5.1), but only the epoch is "a 32-bit
random number". With a weak random source the probability of the same epoch after a restart
grows, weakening the premise of the retired list design. There is also no way to avoid 0. — One
line in 4.3: "OS CSPRNG, redraw if 0".

**W-22. There is no way to terminate the `[console]` thread.** (implementer)
The 3.2.7 shutdown procedure goes only as far as joining `[telemetry]`. `[console]` is a thread
blocked on standard input, so a join is practically impossible, and an implementation written as
"join all threads" hangs in shutdown until the user presses Enter. — One line: "do not join (leave
it to process exit)".

**W-23. The "time" in the local record contract excludes `TUNNEL_DROPPED`.** (implementer)
With architecture chapter 9's "time | when session establishment ends", there is no place to write
`TUNNEL_DROPPED`, a failure **after** establishment (the roadmap Phase 5 verification requires it
to be recorded). Whether a second line is written for the same attempt is not in the four
contracts. — When fixing the format before Phase 4 start, also decide the rule "a drop after
establishment is a separate line".

**W-24. There is no handling or counter for Wintun injection (send) failure.** (scenario)
3.2.3 decided receive-path errors per code, but failure on the injection path
(`WintunAllocateSendPacket`/`WintunSendPacket`) is in none of the five documents. A full send ring
happens normally under high load. — Fix in one line: discard on injection failure + a dedicated
counter.

---

## 4. Info (24)

One line each. Location in parentheses.

1. The virtual adapter MTU statement differs in strength — "default 1400" (protocol 12) vs "about 1400" (architecture 4). protocol is the source, so align architecture
2. The spec FR-6 wording reads as checking a single flag (`HELLO_ACK` received) — reflect the double-flag condition of protocol 9.2 in one phrase
3. Control plane port 8000 is only in windows-prereq section 6 and has no source — fix it in the schema document (B-3) and mark it as an example
4. 9.4 speaks of "refreshing the idle timer" in states before `CONNECTED`, but that timer starts on entering `CONNECTED` (chapter 11) — make clear it is a no-op
5. The word "reordering window" is used for both a Phase 4 object (the 4.5 acceptance window) and a Phase 5 object (loss tally) (protocol chapter 15, roadmap Phase 5) — the roadmap's "window expiry" also reads as a time-based expiry that is not in 4.5. Separate the words
6. The 8.5 sender-side validation has no minimum length (>= 20) check — add a lower-bound row before accessing offsets 12-20 (rule 7)
7. Whether `shutdown()` sends `CLOSE` for a `FAILED` session too is undecided (architecture 3.2.7 "per session") — pin down the target states
8. The room disappearance time in "reclaimed when the room disappears" (architecture chapter 7) is undecided — put room lifetime on the chapter 14 list of separate-document scope
9. There is no verdict method for M-1 "process starts normally" and "different networks" — make "different public IPs confirmed in the M-2 log" the verdict
10. There is no verdict procedure for A-4 / Phase 9 "a third party can reproduce from the documents alone" — make "one person other than the author performs one reproduction run" the pass condition
11. The Phase 2 verification of NFR-9 "confirm in the log" is self-reporting, so a defective implementation also passes — judge by comparing the source port in a packet capture (one line in the Phase 4 capture)
12. The record location and source of the expected publisher string and public hash in windows-prereq section 4 are absent — fix at Phase 6 start
13. The warning level (/W4, /WX) of Phase 1 "`cmake --build` succeeds without warnings" is undecided — pin it in the CMake configuration and write it in the item (two lenses overlapped)
14. There are no size values for the telemetry SPSC ring and the console command queue (architecture 3.2.6) — write them as constants
15. There is no console command vocabulary for Phase 1-5 — the initiator of the Phase 4 `DATA` round trip depends on it. Write a minimum command set in the verification procedure
16. The sequence start value "assigned from 0, incrementing by 1" (protocol 4.4) is ambiguous about whether the first packet is 0 or 1 — pin "the first packet is 0"
17. The destination of the `HELLO_ACK` for a `HELLO` received again in `CONNECTED` is undecided (protocol 9.4) — add the phrase "to the source of that datagram"
18. The "source learning" notation in 9.4 hides that it is conditional on 10.4 — mark "(10.4 conditions apply)"
19. There is no send condition for `CLOSE` reason 0x02 (configuration error) (protocol 5.6) — write the condition or remove it
20. There is no socket buffer size (SO_RCVBUF/SO_SNDBUF) contract (protocol chapter 6) — if defaults are used, say so. The Windows default receive buffer is smaller than one `MAX_DRAIN` round (~94KB)
21. The `REPLAY_WINDOW` constant is only declared and the 4.5 pseudocode uses the literal 64 — connect or remove
22. `drain_udp` does `return EMPTY` on every error other than `WSAEWOULDBLOCK` (architecture 3.2.3) — an oversized datagram (`WSAEMSGSIZE`) ends the round early and breaks batching. Count, then continue
23. Telemetry and connection report reception is unauthenticated, so the Phase 9 dataset can be polluted — one line in the limitations of the Phase 9 statistics design
24. The server clock type (monotonic) of `elapsed_since_ready_ms` is not in chapter 14 contract 4 — computed from the wall clock, an NTP step breaks the 10.2 error bound

---

## 5. Follow-up Placement

This table is the judgement at this point in time. **Live tracking belongs to [`../../kor/plan.md`](../../kor/plan.md).**

| When | What |
|------|------|
| Before Phase 1 start (wording fixes) | Cross-document mismatches and overstatements such as B-1 (verification counter), B-6 (one sentence in chapter 2), W-2, W-3, W-13, W-18. There is no code yet, so now is cheapest |
| Before Phase 1 start (fix values) | W-17 (timer verdict value), W-19 (bind rule), W-20 (logging contract), info 13 — things the Phase 1 verification depends on directly |
| Before Phase 2 start | B-4 (startup inputs) |
| Before Phase 3 start | B-3 (control plane schema + M-3 wording), B-5 (control plane I/O thread ownership), W-6, W-11, W-12 (chapter 14 contract reinforcement) |
| Before Phase 4 start | B-2 (keepalive measurement procedure), W-14, W-15, W-23 — same place as fixing the local record format |
| protocol state machine reinforcement (before implementing the Phase 4 transition table) | W-1, W-5, W-7, W-8, W-9, W-10, W-21, info 16-19 |
| Before each of Phase 5-9 starts | W-4 (state the restart limitation — same place as the automatic retry decision), W-16, W-24, the remaining info |

Of the six blockers, **none blocks Phase 1 code.** But B-1 and B-6 can be fixed in wording
immediately and only cost more if left, so handling them before start is right.

## 6. What This Audit Taught

- **The repairs from the first audit are holding.** The full cross-check of constants, offsets,
  timers, and names matched throughout, and the verdict conditions of windows-prereq passed the
  decidability lens. The kind of defect the last audit fixed did not recur.
- **The remaining defects cluster at the edges.** Verification procedures presume tools or packets
  that are not in the protocol (B-2, W-15), a deferred decision lost its schedule (B-3), or I/O
  outside the thread table lost its owner (B-5). This reconfirms rule 4: when the body is fixed,
  its surroundings (verification procedure, schedule, ownership) demand new contracts.
- **The repo's own measurements were the best reviewer.** B-1, B-2, and W-4 are all
  contradictions with "a fact another part of the documents already measured". When writing a new
  verification procedure, cross-check it against the existing measurement list first.
- **When three lenses independently point at the same place, that is the biggest hole.** The 9.4
  transition table (W-1) and control plane thread ownership (B-5) were such places.
