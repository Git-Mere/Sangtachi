# Full Design Audit (2026-09-14)

**Target:** the complete text of [`../architecture.md`](../architecture.md), [`../spec.md`](../spec.md), [`../roadmap.md`](../roadmap.md)
**Point in time:** before implementation starts, at commit `5807a8b`
**Reason:** three consecutive diff reviews each produced blockers, which raised the question of how much of the design was trustworthy

> Korean version: [`../../kor/audit-history/design-audit.md`](../../kor/audit-history/design-audit.md)

> **This document is an audit record.** It keeps the result of the 2026-09-14 full audit and the
> progress made on it afterwards. **Current work tracking belongs to [`../../kor/plan.md`](../../kor/plan.md).**
> The follow-up table in section 5 is a snapshot of that point in time, so do not read live state
> here.
>
> **The `protocol.md` chapter and section numbers in the body are the numbering of 2026-09-14.**
> The numbers changed later and are not corrected here. A record has to hold the judgement of that
> time as it was. For the current numbers read `../protocol.md` directly. Every reference in the
> live documents was brought to the current numbering.
>
> **The source text of the recurrence-prevention rules in chapter 6 is now `CLAUDE.md` at the
> repository root.** Chapter 6 here is the record of how those rules were obtained. Read
> `CLAUDE.md` if the point is to follow the rules.
>
> `architecture.md`, `protocol.md`, `roadmap.md`, `spec.md`, `plan.md`, `windows-prereq.md` and
> `CLAUDE.md` at the repository root do not link this file. The first six are live documents and
> have to rest on fixed documents, and `CLAUDE.md` took over the source text of the rules this
> file used to hold. `decisions/`, `commit_history/` and `tools/` do link it. They are records
> whose purpose is to point at this audit.

---

## 1. Method

The target was the **complete finished documents**, not a diff. Three independent reviews with different lenses ran in parallel.

| Lens | Question |
|------|----------|
| Mechanism | If this is implemented exactly as written, where does it break at runtime? Where must the implementer invent something? |
| Platform | What Windows/Wintun/firewall/privilege/AWS realities does the document ignore or get wrong? |
| Viability | As a one-semester solo project, do the schedule and success criteria hold up logically? |

Result: **20 blockers, 25 warnings.**

## 2. Root Cause

These were not independent bugs. They come from two structural causes.

**Cause 1. No scenario was ever traced end to end.**
The documents recorded "which components exist" and "how it is verified", but never traced "where one packet goes and what must be true at each point". So every review that actually traced something found a new hole.

**Cause 2. Fixing findings introduced new defects.**
Applying earlier review findings locally meant the fixes themselves created new contradictions or unachievable criteria.

| Added earlier | Problem surfaced now |
|---|---|
| NFR-9 "STUN and tunnel share one socket" | Receive ownership and demultiplexing for that socket were never defined, so two readers steal each other's packets |
| "Supported condition = two STUN servers agree" | Only mapping behavior was considered, not filtering behavior. The condition can hold and hole punching still fail |
| Phase 7 "counters match capture exactly, no tolerance" | Capture placement, offload, and capture loss mean a correct implementation cannot pass |
| Phase 8 "F3 latency within ±30 ms" | Different sampling means a correct tunnel fails momentarily |
| Phase 4 "measure mapping lifetime with external probes" | That external vantage point component exists nowhere in the design |
| A-2 "controlled failure scenario required" | Firewall blocking is not a NAT traversal failure and cannot support conclusions about mapping behavior |

Lesson: **an over-tightened criterion is as bad as a loose one.** A criterion a correct implementation fails is not a criterion.

---

## 3. The 20 Blockers

Status: `fixed` = resolved in this pass, `open` = tracked to a follow-up step.

### A. The wire protocol was unspecified (root blocker)

| # | Problem | Status |
|---|---------|--------|
| 1 | magic/version/type values, nonce width, payload layouts, and byte offsets all undefined. No code can be written from the document | fixed, [`../protocol.md`](../protocol.md) sections 2 to 4 |
| 2 | No session instance identifier, so a restart lets delayed packets from the previous process into the new session | fixed, `protocol.md` 3.2 (new `session_epoch`, header 16 -> 20 bytes) |
| 3 | Receive ownership of the single socket and STUN/tunnel demultiplexing undefined. The STUN reader and the receive loop consume each other's packets | fixed, `protocol.md` sections 5 and 6 |
| 4 | Four threads mutate session state, timers, and sequence values without synchronization, producing data races | fixed, `architecture.md` 3.2. A single event loop removes the shared state |
| 5 | Telemetry upload shares the timer thread. During a control plane outage a blocking TCP upload stops keepalives, violating NFR-3 | fixed, `architecture.md` 3.2.6. Isolated onto a dedicated thread with a lossy queue |

### B. The hole punching algorithm was not actually designed

| # | Problem | Status |
|---|---------|--------|
| 6 | No punch start synchronization, so if one side starts first the peer's NAT discards everything and the sends never overlap | fixed, `protocol.md` 8.2 (`punch_start_ms` rendezvous) |
| 7 | No candidate nomination, so receiving HELLO on a LAN candidate while sending ACK to a public candidate yields an asymmetric dead session | fixed, `protocol.md` 8.4 |
| 8 | Duplicate and early `HELLO_ACK` handling undefined. An ACK arriving before the peer's HELLO is discarded and both sides time out | fixed, `protocol.md` 9.2 and 9.3 (two-flag condition, full transition table) |

### C. Injection safety and MTU

| # | Problem | Status |
|---|---------|--------|
| 9 | Anything matching the magic can inject arbitrary inner source and destination addresses into Wintun under any `peer_id` | fixed, `protocol.md` section 7 checks 10 to 15 |
| 10 | Fixed MTU with no PMTU response. A smaller path MTU leaves inner TCP in a retransmission blackhole (login works, world loading hangs) | fixed, `protocol.md` section 11 (DF unset, 1400 default, limitation stated) |

### D. Windows platform realities

This entire area was absent from the design.

| # | Problem | Status |
|---|---------|--------|
| 11 | Adapter creation and route configuration need administrator privileges; the document never mentions it | documented, [`../windows-prereq.md`](../windows-prereq.md). Real verification in Phases 3, 6 and 8 |
| 12 | A new adapter is classified Public and the firewall blocks inbound ICMP and TCP 25565. The client's UDP is also blocked if the first-run prompt is dismissed (**corrected by the 5.8 measurement: the reply to a flow we started passes statefully; what is blocked is unsolicited inbound**) | documented, [`../windows-prereq.md`](../windows-prereq.md). Real verification in Phases 3, 6 and 8 |
| 13 | An abnormal termination leaves the adapter, address, and route behind, so the next run creates duplicates | documented, [`../windows-prereq.md`](../windows-prereq.md). Real verification in Phases 3, 6 and 8 |
| 14 | Wintun DLL/driver packaging, architecture, and signing. Installation can fail on the demo PC | documented, [`../windows-prereq.md`](../windows-prereq.md). Real verification in Phases 3, 6 and 8 |
| 15 | If `server-ip` in `server.properties` is set to **a different interface address**, connections to `10.100.0.1:25565` are refused. It must be blank or explicitly `10.100.0.1` | documented, [`../windows-prereq.md`](../windows-prereq.md). Real verification in Phases 3, 6 and 8 |
| 16 | No EC2 security group inbound rule, binding to `127.0.0.1`, public IP changing on restart | documented, [`../windows-prereq.md`](../windows-prereq.md). Real verification in Phases 3, 6 and 8 |
| 17 | `10.100.0.0/24` colliding with a real LAN, Hyper-V, or another VPN sends traffic out the wrong interface | documented, [`../windows-prereq.md`](../windows-prereq.md). Real verification in Phases 3, 6 and 8 |

There is also a usability limitation here. The claim in `spec.md` is "without manual port forwarding", and that claim itself holds. What it costs instead is administrator privileges, a driver install, and firewall rules. **The configuration burden did not disappear; it changed kind.** Whether that is easier than port forwarding has to be argued separately, and it belongs in the final report as an honest limitation.

### E. Project viability

| # | Problem | Status |
|---|---------|--------|
| 18 | The C-5 priority order does not protect the minimum deliverable | fixed, [`../spec.md`](../spec.md) C-5 and the "Cost of dropping a priority" section. A table records, tier by tier, whether it can be dropped and which criteria disappear with it. The minimum `DATA` round trip and the local record moved to P0, so **minimum success M-1 to M-6 holds with P0 alone** |
| 19 | The traceability table assigns M-5 to Phase 4, but `DATA` does not exist until Phase 5 | fixed. The minimum `DATA` round trip moved to Phase 4. M-6 had the same defect (no control plane record appeared in the Phase 5 work), so it became a local record and moved to Phase 4 |
| 20 | If both available home networks show destination-dependent mapping, minimum success is impossible and there is no alternative. Relay is a stretch goal after Phase 9 and cannot rescue it | **resolved (2026-09-20).** 22 measurements, all 7 networks `endpoint-independent`, 0 symmetric. No fallback added, M-1 and M-4 kept. **A residual risk remains** — it returns if conditions change, and there is no fallback. Re-measurement gates are set at the start of Phase 4, during Phase 8 preparation, and right before the demo. [ADR 0001](../decisions/0001-no-direct-connection-fallback.md) |

**Number 20 was the most dangerous.** The others can be fixed, but this one would have left no time to recover if it had surfaced mid-semester. The whole project was hostage to an external condition outside the student's control.

**It was resolved by measurement on 2026-09-20.** The hostage structure itself has not gone away, though. The measurements are observations at one point in time and can be invalidated by a router replacement or an ISP configuration change. There is still no fallback for the fallback, so a re-measurement right before the demo is added to the Phase 8 preparation steps.

---

## 4. The 25 Warnings

| Area | Item | Status |
|------|------|--------|
| Protocol | Whether the `sequence` space is global, per type, per direction, or per session was undefined | fixed, `protocol.md` 3.3 |
| Protocol | `PING`/`PONG` timestamp representation, units, matching, and replay protection undefined | fixed, `protocol.md` 4.5 (`ping_id` approach) |
| Protocol | No graceful close packet, so shutdown is indistinguishable from a crash | fixed, `protocol.md` 4.6 (`CLOSE`) |
| Protocol | Punch/handshake/keepalive/idle deadlines given as "roughly" or ranges, preventing interoperability | fixed, `protocol.md` section 10 (all fixed values) |
| Protocol | Candidate pair scheduling, multi-interface local address selection, and retry cadence undefined | fixed, `protocol.md` 8.1 and 8.3 |
| Protocol | Source validation and NAT rebinding policy undefined | fixed, `protocol.md` 8.5 |
| Protocol | Handling of a restarted peer's HELLO in `CONNECTED` undefined, and `FAILED` had no exit | fixed, `protocol.md` 9.3 and 9.4 |
| Protocol | Inner address family undeclared, so IPv6 packets get parsed at IPv4 offsets | fixed, `protocol.md` section 1 and check 11 |
| Protocol | Behavior for oversized inner packets undefined | fixed, `protocol.md` 11.4 |
| Platform | Without handling `SIO_UDP_CONNRESET`, `WSAECONNRESET` kills the receive loop | fixed, `protocol.md` section 5 |
| Platform | Calling `connect()` filters out datagrams from other candidates | fixed, `protocol.md` section 5 |
| Platform | `SO_REUSEADDR` makes delivery nondeterministic | fixed, `protocol.md` section 5 |
| Platform | Local candidates advertising Wintun/Hyper-V/VPN addresses | fixed, `protocol.md` 8.1 |
| Platform | Wintun is an L3 ring API, not TAP or `ReadFile` | fixed, `architecture.md` 3.2.3. DLL packaging is [`../windows-prereq.md`](../windows-prereq.md) section 4 |
| Platform | Without waiting on the read event and releasing packets, the loop spins or exits | fixed, `architecture.md` 3.2.3 |
| Platform | The on-link `/24` route already exists, so creating it again errors | documented, [`../windows-prereq.md`](../windows-prereq.md). Real verification in Phases 3, 6 and 8 |
| Platform | Starting tests while the address is still tentative causes intermittent failures | documented, [`../windows-prereq.md`](../windows-prereq.md). Real verification in Phases 3, 6 and 8 |
| Platform | EC2 public IP changes on restart | documented, [`../windows-prereq.md`](../windows-prereq.md). Real verification in Phases 3, 6 and 8 |
| Platform | Minecraft LAN discovery uses multicast and cannot work over a unicast tunnel | documented, [`../windows-prereq.md`](../windows-prereq.md). Real verification in Phases 3, 6 and 8 |
| Platform | A Java update invalidates a path-scoped firewall exception | documented, [`../windows-prereq.md`](../windows-prereq.md). Real verification in Phases 3, 6 and 8 |
| Platform | SmartScreen/Defender may warn on, block, or quarantine the unsigned client, depending on reputation, MOTW and Defender policy | documented, [`../windows-prereq.md`](../windows-prereq.md). Real verification in Phases 3, 6 and 8 |
| Viability | M1 (Phases 1-4) and M2 (Phases 5-8) workloads are unbalanced at five weeks each | fixed. The milestone split itself was removed. Progress decides the week allocation, so it is not fixed in the documents |
| Viability | The external UDP vantage point required by Phase 4 keepalive verification does not exist in the design | fixed, [`../roadmap.md`](../roadmap.md) Phase 4. It states that the AWS EC2 control server from Phase 3 is used as the vantage point |
| Viability | The HTTPS IP lookup in M-2 verification may use a different egress path than the UDP socket | fixed, [`../spec.md`](../spec.md) M-2. The check became a query to two different STUN servers from the same UDP socket. The HTTPS lookup moved down to a reference record |
| Viability | Phase 7 exact counter equality, Phase 8 ±30 ms, and the A-2 controlled failure scenario are over-tightened | open, follow-up 6 |
| Viability | Phase 9 statistical design (trial duration, independence, uncertainty reporting) undefined | open, follow-up 6 |

---

## 5. Follow-up Plan

| # | Work | Resolves | Status |
|---|------|----------|--------|
| 1 | Write the fixed `protocol.md` | blockers 1,2,3,6,7,8,9,10 plus 13 warnings | **done (2026-09-14)** |
| 2 | Settle the concurrency model. A single event loop | blockers 4,5 plus 2 warnings | **done (2026-09-15)** |
| 3 | Add a Windows prerequisites section: privileges, firewall, adapter cleanup, subnet collision check, Wintun packaging | blockers 11-17 plus 6 warnings | **done (2026-09-21)**. [`../windows-prereq.md`](../windows-prereq.md). **The completion condition is documentation, not real verification** |
| 4 | Fix the spec logic errors: C-5 priorities, M-5/M-6 traceability, milestone handling | blockers 18,19 plus 3 warnings | **done (2026-09-21)** |
| 5 | Contingency for direct connection being impossible | blocker 20 | **done (2026-09-20)**. Resolved by measurement. Options A, B, and C all rejected |
| 6 | Loosen the over-tightened verification criteria | 2 warnings | open |

Step 1 covered 40% of the blockers and half the warnings, and step 2 covered the two remaining concurrency blockers. **Step 5 was resolved by measurement on 2026-09-20.** It was handled first because it was the most dangerous item.

**The 13 items under step 3 are marked `documented`, not `fixed`.** That means the check command and the pass rule are written down, not that they were confirmed on a demo PC. Real verification sits in Phase 3 (EC2), Phase 6 (Wintun) and Phase 8 (demo preparation).

**Step 3 was completed on 2026-09-21.** [`../windows-prereq.md`](../windows-prereq.md) covers the 13 items one section each, and 8 of those sections were written from output actually produced on Windows 11 (4 measured, 4 partly measured). **The week imbalance warning under step 4 was not covered by lowering the bar.** The milestone split itself was removed, which erased the cause. **What remains is read from [`../../kor/plan.md`](../../kor/plan.md), not from this table.**

---

## 6. Preventing Recurrence

Rules taken from this audit.

1. **Trace one scenario end to end before declaring a document finished.** One packet, one failure, one restart. A component list and a verification checklist do not reveal the holes.
2. **Diff review only catches local defects.** Run a separate full-document audit at every milestone.
3. **Check that a correct implementation passes a criterion before adding it.** A criterion nothing can pass gets loosened later, and real defects pass along with it.
4. **When applying a finding, define the new contract that fix creates.** Adding the constraint "they share one socket" required settling that socket's ownership and demultiplexing at the same time.
5. **Never copy the same claim into more than one place.** Write it once and link to it. A copied claim has to be tracked down everywhere each time its strength changes, and one or two places always get missed. If it is already spread out, `grep` for every occurrence and fix them in one pass. **There are 3 exceptions.** (1) The `docs/kor` and `docs/eng` mirror pair (Korean is the original). (2) A **summary index**: at most a one-line name per item, never the body, and it must state where the original lives. (3) **Historical records** such as `commit_history/` and measurement records. These state facts about a moment, so they carry the evidence and figures as they were. **They are not updated when the source changes.** If they only linked, a later reader would see the current document instead of the judgement made at the time, which defeats their purpose.
6. **An executable procedure in a document needs code-level review.** Input validation, cleanup on interruption, privilege scope, and the limits of what the result proves. If that is not worth doing, put the procedure in a tool and have the document point at it. **Do not keep a script for a test that has not been run yet in a document.**
7. **A function that returns a verdict starts with validation.** The default is "indeterminate when unsure"; it asserts only when certain. Do not trust a standard library predicate by its name (`is_private` includes loopback and documentation ranges, and `is_global` is true for multicast). An allow list is safer than a deny list.

**Rules 5 to 7 come from the blocker 20 work on 2026-09-20.** Nineteen rounds of cross-model
review produced 93 findings, and 56 of them landed in two places.

| Location | Findings | Cause |
|------|:----:|------|
| `classify_nat` in `natprobe.py` | 31 | A verdict function written as a 20-line convenience. It asserted on invalid input (rule 7) |
| The PowerShell procedure in ADR 0002 | 25 | A script for a test that had not been run, placed in a document. After round 13 it produced 9 of the next 10 findings (rule 6) |

Most of the rest came from copying the same claim into 8 places (4 documents in 2 languages) and
then fixing one or two at a time (rule 5). **Rule 4 was broken again too:** the description of the
cause was softened to "likely" while the conclusion that depended on it was left absolute.


---

## 7. Second Review of protocol.md (same day)

The first draft of `protocol.md`, written as follow-up 1, was re-reviewed under two lenses.

| Lens | Result |
|------|--------|
| Implementability ("implement it in your head and find where you must still guess") | 20 blockers |
| Adversarial (an attacker and real network conditions) | 10 blockers |

**The first draft did not deserve to be called a fixed specification.** The main defects and their fixes:

| Severity | Defect | Fix |
|----------|--------|-----|
| blocker | `is_newer(a,b)` returned true for `a == b`, classifying duplicates as newer | added the `a != b` condition (4.4) |
| blocker | "an early `HELLO_ACK` is normal" contradicted the epoch check, timing both sides out in a normal situation | 8.2 lets `HELLO`/`HELLO_ACK` pass before epoch pinning on the strength of the nonce |
| blocker | The text said `HELLO` was exempt from the source check while the check table had no exemption | split per-type rules into the 8.2 table |
| blocker | Classifying on magic left `drop_magic`/`drop_version` permanently zero and hid malicious traffic in `drop_unclassified` | section 7 now classifies on the top two bits; magic moved into validation |
| blocker | No duplicate suppression algorithm, so a replayed `DATA` was injected twice and a replayed `KEEPALIVE` kept a dead session alive | 4.5 specifies `highest` plus a 64-bit bitmap, decided ahead of every side effect |
| blocker | Nomination deadlocks behind the same NAT or on asymmetric paths, and offers no NAT rebinding recovery trigger | replaced with endpoint learning (10.4) |
| blocker | Unclear whether `punch_start_ms` was per response or per room, and absolute timestamps were vulnerable to clock skew | 10.2 fixes it once per room and switches to relative time (`elapsed_since_ready_ms`) |
| blocker | On an epoch change, `got_ack`/`sent_ack` persisted and drove the new session to `CONNECTED` without evidence | the eight reset steps of 9.5 |
| blocker | Epoch lifecycle contradiction (3.2 "per process" versus 9.4 "new on retry") | unified as per session attempt, with a 2-minute retired list |
| blocker | `peer_id` allocation rules and uniqueness undefined | 4.2 |
| blocker | An unbounded, unverified candidate list turns the client into a reflection tool firing at a third party every 200 ms | 10.1 hygiene rules made mandatory |
| blocker | No stated security model, and inner validation was overstated as "the only injection defense" | new section 2 enumerating everything not provided, overstatement withdrawn |
| warn | A 45 s keepalive timeout races the last send | changed to 50 s with the rationale stated (section 11) |
| warn | IPv4 header checksum never validated | 8.4 check 12 |
| warn | Sequence numbers consumed punching multiple candidates were miscounted as loss | loss accounting resets on entering `CONNECTED` |
| warn | No send-side validation rules | new section 8.5 |
| warn | The `virtual_ip` field had no consumer | validation rule added in 5.1 |
| warn | STUN procedures and control plane schema undefined | section 13 fixes the STUN scope; section 14 states the six contracts the control plane must honor |

### What this confirms

Thirty blockers against the first draft is not bad news in itself, **because they surfaced before implementation.** The same defects found during Phase 4 would have cost days of diagnosis, and the missing duplicate suppression and the nomination deadlock in particular would have appeared as "it sometimes fails to connect", which is barely reproducible.

It does also mean the root cause named in section 2 is still active. Writing that first draft, I again did not trace a scenario to the end. I built a state machine table but never substituted concrete situations such as "two peers behind the same NAT" or "a previous-epoch packet arriving right after a restart". Rule 1 in section 6 has to be genuinely applied to every document.

### Third and fourth reviews

The second revision was reviewed twice more. It is converging.

| Round | Blockers | Character |
|-------|----------|-----------|
| First draft | 30 | gaps in the design itself |
| Third | 5 | mostly side effects of the endpoint learning introduced in the second revision |
| Fourth | 5 | pseudocode branch precision and interactions among the preceding fixes |

Third-round blockers:

| Defect | Fix |
|--------|-----|
| On renegotiation both sides drew a new local epoch, producing an **infinite epoch ping-pong** in which the two peers never connect | 9.5 keeps the local epoch/nonce; it changes only when we start a new attempt from `IDLE` |
| Clearing the bitmap at `shift == 64` let the previous `highest` be accepted again despite being a duplicate | clear only when `shift > 64`; `shift == 64` sets bit 63 |
| An older packet accepted inside the reordering window dragged the endpoint back to a previous path | 10.4 (b): only packets that advance `highest` are used for learning |
| Spoofing the source could redirect the entire game traffic stream at an arbitrary victim, contradicting the section 2 promise to block third-party harm | 10.4 (c): addresses outside the candidate set switch only after nonce path validation |
| Cumulative subtraction on 32-bit sequences breaks after a full cycle | introduced the non-wrapping 64-bit `position` and a `baseline` |

Fourth-round blockers:

| Defect | Fix |
|--------|-----|
| `seq == highest` fell into the "older" branch with `back == 0`, giving the undefined `uint64_t{1} << (0-1)` | explicit duplicate check placed first |
| Written as `if / if / else`, a `shift > 64` cleared the bitmap and then also entered the `else`, shifting by more than 64 | mutually exclusive `if / else if / else` |
| Reordered packets had no computed position, so using the state variable `position` underflowed `accepted > expected` | defined a per-packet `pkt_pos` (`position` when advancing, `position - back` when reordered) |
| Renegotiation learned an off-candidate source directly, bypassing the 10.4 path validation | reply `HELLO_ACK` to the datagram source without changing `peer_endpoint`; a single reply and a bulk switch are different things |
| The path validation nonce conflicted with the "one per attempt" rule | probe nonces moved to a separate table (5.1, 8.2) |

### Current status

The fourth round of fixes is applied. No fifth review was run, so **no claim is made that nothing remains.** One more pass runs the next time this document is touched.

The convergence trend (30 -> 5 -> 5) and the change in the character of the defects (design gaps -> side effects -> pseudocode precision) suggest the structural problems are resolved. That **all five fourth-round findings were side effects of preceding fixes** does show that rule 4 in section 6, defining the new contract a fix creates, is still not being followed closely enough. Four came from the third-round fixes; the `seq == highest` branch came from introducing the duplicate suppression algorithm in the second round. The first draft had no such algorithm at all.

---

## 8. Follow-up 2: Concurrency Model Review (2026-09-15)

Section 3.2 of `architecture.md` was written and put through six rounds. Rounds 1 to 4 each used a different lens; rounds 5 and 6 were convergence checks.

| Round | Lens | Blockers | Warnings |
|-------|------|----------|----------|
| 1 | Windows API correctness | 2 | 2 (plus 1 nit) |
| 2 | Internal consistency and completeness of the design | 3 | 2 |
| 3 | Side effects of the fixes, pseudocode precision | 2 | 4 |
| 4 | What the third-round fixes broke | 1 | 2 (plus 1 nit) |
| 5 | Convergence check (focused on the newly changed points) | 1 | 1 |
| 6 | Convergence check (final) | 0 | 0 |

**The first draft took 2 blockers.** Not calling `WSAEnumNetworkEvents`, which makes the loop spin burning CPU after the first datagram; and the console handler returning immediately on window close, which skips the `CLOSE` send and adapter cleanup entirely. Both would have surfaced in Phase 1.

The three blockers in round 2 were heavier.

| Defect | Fix |
|--------|-----|
| The drain loops had no cap, so whenever arrival outpaces processing the timers never run at all. Keepalives stop | A 64-per-source-per-iteration budget. Hitting the budget sets `busy` and re-enters immediately with a zero timeout |
| `drain_wintun` was called unconditionally although Phases 1-5 have no adapter. The console event sat in the wait set with nothing servicing it, spinning the loop | Added a NULL session guard, added `drain_console`, made the console event auto-reset |
| A mutex-protected queue paired with the claim that `[loop]` never blocks. If the consumer is descheduled holding the lock, that is false, and the roadmap criterion becomes unpassable | Replaced with a lock-free SPSC ring. When full, the new record is dropped |

The third is an instance of rule 3 in section 6. **I wrote another criterion that a correct implementation cannot pass.**

Rounds 3 and 4 were entirely side effects of my own fixes: calling `now()` twice so an underflow turns the timeout into 49.7 days; the `ERROR_INVALID_DATA` branch with no return, letting a `NULL` pointer through; pushing the shutdown sentinel into a lossy queue where a full moment discards it; a 3-second socket timeout outlasting the 2-second join bound and rendering it meaningless.

The round-5 blocker was a different kind. Round 4 changed the timer ordering rule in protocol.md section 11 to dequeue time, but the surviving "arrival time" wording in `architecture.md` 3.2.4 was not changed with it. **The two documents stated different rules.** Rule 4 in section 6, missed again.

Round 6 returned `LGTM - no blockers`. Unlike protocol.md, this one was carried to a confirmed convergence.

### What this round taught

protocol.md stopped at round 4 and made no claim that nothing remained. This time the rounds continued until one came back clean, and **round 5 did in fact surface one more cross-document contradiction.** Stopping at round 4 would have left it in place.

Changing the lens across rounds 1 to 4 is what made it work. Six runs of the same prompt would have kept re-confirming what round 1 already caught.
