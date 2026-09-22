# New Control Plane Design Document and Design Audit 2 Follow-up 2 (2026-09-22)

`docs/kor/control_plane.md` was newly written. It is the single source for the control plane.
While writing it, the "design audit follow-up 2: control plane bundle" in `plan.md` (B-2, B-3,
B-4, B-5, W-6, W-11, W-12, info 3, 8, 23, 24) was handled together. **B-2 left the control plane
bundle and the procedure body is not written yet.** See the decisions below.

## What the User Decided

11 questions were asked before the work and the user answered. These answers shaped this document.

| What | Decision |
|------|------|
| File name | `control_plane.md`. The first instruction was `control_panel.md` |
| Source | The new document is the single source for the control plane. `architecture.md` 3.3, 6.1 and `protocol.md` chapter 14 are reduced to summaries and links |
| Transport | HTTP/1.1 subset (`POST` only, `Content-Length` only, `Connection: close`). No web framework |
| Trust | Plaintext, no authentication, `room_id` is the only secret. Written as an accepted limitation |
| `room_id` | **6 characters, random uppercase+digits.** The user decided this directly. Excluding `I O 0 1` (32 characters, 30 bits) was proposed and adopted with no objection |
| `peer_id` | CSPRNG uint32, excluding 0 |
| STUN default list | The four `DEFAULT_STUN_SERVERS` in `tools/nat-probe/natprobe.py` |
| Control server address | **Both an Elastic IP and a DNS name.** DNS points at the EIP. The user decided this directly |
| Rejoin | Re-presenting the `peer_token` issued at join gives the same `peer_id` and virtual IP |
| B-5 | New `[control]` thread. Body in `architecture.md` 3.2 |
| B-2 | Unrelated to the control plane. Only its ownership is sorted out; the procedure stays a before-Phase-4 item |
| Implementation detail | Up to pseudocode at the level of `architecture.md` 3.2. Verdict procedures write the case table first |

## Changes

### New document `control_plane.md`

10 chapters. Scope and trust assumptions, identifiers and constants, transport and encoding, 4
operations, state transitions, storage (DynamoDB tables, items, conditional writes, rate limiting,
TTL), server implementation (modules, processing order, clocks, logs, configuration), client-side
contract (thread, time limits, error classification and retry, startup order), verification, not
decided. There are six case tables (2.1 `room_id`, 3.3 HTTP parsing, 4.4 candidate hygiene, 5.1
expiry, 6.4 rate limiting, 7.4 clocks).

### Living documents

| Document | What |
|------|------|
| `architecture.md` 3.1 | `control_client` row. `[control]` ownership stated |
| `architecture.md` 3.2.1, 3.2.2, 3.2.3, 3.2.5, 3.2.7 | Thread table, wait handles (rank 4), `drain_control` in the loop, state ownership, join order, following the added `[control]` thread |
| `architecture.md` 3.2.8 | **New.** The `[control]` thread. Two queues (8 entries), one outstanding request, DNS once, shutdown. Why it is not put in `[loop]` and why it is not merged with `[telemetry]` (B-5) |
| `architecture.md` 3.3 | Removed the module table and the 5 store contracts. 4 summary lines and a link |
| `architecture.md` 3.5 | **New.** Startup inputs. 5 CLI arguments, STUN default list and server selection, rejoin proof and the room code on standard output (B-4) |
| `architecture.md` 6.1 | Removed the operation table. Four names and a link. No `register_peer` |
| `architecture.md` chapters 7, 10 | Room lifetime link, `control-server/` files in the repo structure |
| `architecture.md` chapter 9 | Log event keys `control.result`, `control.peers` and their field formats. Fixed keys 6 → 8 |
| `protocol.md` chapter 2 | One paragraph: the trust assumptions of the control plane path are `control_plane.md` 1.2 (W-11) |
| `protocol.md` 10.4 | The sentence "chapter 14 contracts have no such item" brought to the current state where contract 7 exists |
| `protocol.md` chapter 14 | Monotonic clock in contract 4 (info 24), contract 7 rejoin (W-6), contract 8 CSPRNG (W-12). 6 → 8 |
| `spec.md` M-3 | Verdict method changed to comparing the `control.peers` event with the peer's `stun.result` (B-3) |
| `roadmap.md` Phase 2 | STUN server source and startup input handling |
| `roadmap.md` Phase 3 | Pre-start items restructured (schema fixed, EIP and DNS, deployment settings), tasks made concrete, **verifications 9 → 21.** The counts were made by machine |
| `roadmap.md` Phase 4 | Ownership of the EC2 sender (separate tool) stated in the B-2 item |
| `windows-prereq.md` sections 6, 10 | Source of port 8000, why both the EIP and DNS are used |
| `plan.md` | Completion table for the second bundle, next steps, 5 document debts |
| `CLAUDE.md` | A `control_plane.md` row in the document role table, the living document list in two places, 3 lines of current status. `control_plane.md` added as item 6 of the reading order. **This is a rule change, so it was added with the repository owner's approval.** The rest is a current status update |

## Decisions

**`register_peer` was removed.** Rejoin became the second form of `join_room`. A separate operation
makes the client judge "when to call it" and gives the server separate state checks for two
operations.

**A `client_nonce` idempotency key was added.** If a `join_room` retry that lost its response takes
the second peer slot again, the real participant gets `room_full`. In a 2-peer room a single retry
ruins the room. The partition key of the `NONCE#` item is the nonce itself. `create_room` has to
look it up at a time when it does not know the `room_id`.

**The definition of ready is the existence of `ready_at`.** The candidate count is not recounted.
If ready flipped back to false the moment a rejoin clears the candidates, it would disagree with a
peer that already started punching. Instead, a response "ready but the peer has 0 candidates" can
exist, and the client does not treat it as ready and keeps polling.

**`elapsed_since_ready_ms` is computed from three stored values (monotonic ns, boot_id, wall
clock).** Because the server holds no state in memory (store 5), storing only the monotonic clock
value leaves no reference after a reboot. The property that `CLOCK_MONOTONIC` does not change
across a process restart is used; a reboot is detected by `boot_id` and falls back to the wall
clock.

**When the rate limit table is full, new sources pass.** With eviction, whoever has many IPs evicts
the real guesser's entry. With pass-through, filling the table gains nothing.

**`room_not_found` and `room_expired` were separated.** Existence leaks, but an expired room cannot
be joined so nothing is lost, and a person mistyping the code and a host having created it an hour
ago call for different actions.

**B-2 is not part of the control plane bundle.** The user asked "the assumption seems to have
changed", so it was looked at again. Neither the control plane nor the telemetry service opens
UDP, so the EC2 probe sender is a `tools/` tool that belongs to neither service. `plan.md` put it
in the control plane bundle under the classification from when the control server was the only
process on EC2. The procedure body is still a before-Phase-4 item and was not written in this
commit.

**The 30-bit `room_id` was accepted as is.** A user decision. Instead the document (1) writes as an
accepted limitation that the size allows online guessing, and (2) adds a per-source failure budget
(10 per minute) as a control plane requirement. It is written as slowing down, not blocking.

## Verification

- `docgate.py`: `link` 0. `mirror` and `parity` fail with the same kind as HEAD and this is the
  normal state (one more: `control_plane.md` has no mirror). The English mirror is made when the
  push instruction comes
- Obligations of what was removed: the 5 store contracts in `architecture.md` 3.3 →
  `control_plane.md` 6.1 (body moved as is). The two places where `roadmap.md` referenced "3.3
  store N" were found by `grep` and fixed. The obligation of `register_peer` in the 6.1 operation
  table went to `join_room` rejoin
- Sentences that relied on a changed definition: "chapter 14 contracts have no such item" in
  `protocol.md` 10.4 was found and fixed. `register_peer`, `room.py`, `peer.py` were `grep`ped
  fully, excluding only `.git`. They remain only in `first_design.md`, which is reference material
- Scenario trace (rule 1): (a) a participant loses the `join_room` response and retries → same
  response via `NONCE#`. (b) host restart → same `10.100.0.1` via `--rejoin`, candidates cleared,
  `ready_at` kept, the peer is not polling → the survivor gets `TUNNEL_DROPPED` after 50 seconds.
  Consistent with the limitation in `protocol.md` 10.4. (c) a ready response with 0 peer candidates
  → keep polling. Tracing these three found that the rule for (c) was missing and added it to 4.5

## Cross-Model Review

Lenses were run on Codex (GPT) one at a time, in order. Not launched in parallel. Input was cut
to the range each lens looks at.

| Round | Lens | Input | Result |
|:------:|------|------|------|
| 1 | Scenario trace | Full `control_plane.md` | blocker 3, warn 1 |
| 1 | Consistency + overstatement (2 bundled) | Full `control_plane.md` | blocker 1, warn 17 (consistency 1, overstatement 16) |
| 1 | Removal obligations + stale statements (2 bundled) | diff of 5 living documents | Removal `LGTM`, stale statements warn 2 |
| 2 | Scenario trace (repair) | Full `control_plane.md` + list of changed sections | blocker 1, warn 1 |
| 3 | Scenario trace (repair, increment) | Chapters 6 and 7 only | warn 1 |
| 3 | Record | diff of `plan.md`, `CLAUDE.md`, this file | blocker 1, warn 1 |
| 4 | Record (repair) | diff of `plan.md`, this file | blocker 1. The tally sentence written as the round 3 repair contradicted the table again. Below |

**4 of the 7 blockers came from the scenario lens.** The 1 from the consistency lens (conflict
between the transaction item order and the cancellation reason verdict order) arose from a fix the
scenario lens produced, and the 2 from the record lens were both the tally sentence in this section
contradicting the table. The tally was counted by hand and wrong twice; this sentence was written
after adding the `blocker N` in the table above by script and confirming 7. The overstatement,
removal, and stale statement lenses produced no blockers. Every design defect was found by the
scenario lens. As `CLAUDE.md` says.

### Applied

| Round | Finding | Applied |
|:------:|------|------|
| 1 | The same `peer_token` cannot be returned on a nonce retry, because only the hash is stored | Store the original. The price is written in 2.3 |
| 1 | The verdict order when there are several transaction cancellation reasons is undecided → a retry can get `room_full` | 6.3 cancellation reason table. `NONCE#` first |
| 1 | The wall clock fallback on reboot contradicts protocol 14 contract 4 (monotonic) | Exception delegation sentence in 14 contract 4. Stated in 7.4 that the error bound is not guaranteed |
| 1 | `rate_limited` is only in some operations' error lists | Pinned four common errors in 4.1 and removed them from the per-operation lists |
| 1 | Transaction item order (`NONCE#` last) and cancellation reason verdict order (`NONCE#` first) conflict | Changed so items are identified by position and the verdict order is set by the table |
| 1 | 8.3 presumes `client_nonce` for rejoin retries too | Split new join and rejoin into two lines |
| 1 | 16 overstatements. External system behaviour and security claims have no evidence | Handled in three ways. Below |
| 1 | `shutdown()` speaks of joining `[control]` in Phase 1-2 too | Added a sentence that it exists only from Phase 3 |
| 1 | 3.5 makes `--server` required in all of Phase 1-5 | Required from Phase 3. Before that, only stored |
| 2 | The new join transaction does not condition on room survival, so an item can be created in a room that expired after the pre-read | Put a `ROOM` ConditionCheck in the transaction and added a row to the cancellation reason table |
| 2 | The rejoin and candidate registration updates have the same race | **Decided to allow it and wrote why.** Also wrote why only new join is conditioned |
| 3 | The blocker tally below this table ("all 4 from scenario") contradicts the table (consistency blocker 1) | Changed to 4 of 5 |
| 3 | `plan.md` keeps a detailed per-item handling table for the finished second bundle, departing from the "finished work gets only a pointer" principle | Removed the table, leaving one link line and one line on the B-2 ownership change |
| 4 | The tally sentence written as the round 3 repair left out the record lens blockers and contradicted the table again | Added the table's `blocker N` by script and changed it to 7 |
| 3 | The consequence of the allowed race was overstated. "carried in no response" was wrong; the current request gets `ok` | Narrowed to "the current request can succeed and later reads get `room_expired`". Also wrote why there is no recheck right before the response |

**Handling of the 16 overstatements.** No fabricated citation was added.

| Handling | Count | Where |
|------|:--:|------|
| Checked directly on this machine and wrote the value | 2 | `inet_aton('10.0.5')`, `'010.0.0.5'`. Three `ipaddress` predicate values (Python 3.11) |
| Pointed at the decision number in ADR 0004, which already cites AWS documentation | 3 | Consistency (decision 1), TTL (decision 3), DynamoDB local (what we pay) |
| Lowered to an assumption and set the check time before Phase 3 start in `roadmap.md` | 2 | `CLOCK_MONOTONIC`, `boot_id` |
| Weakened the claim | 8 | Caller IP, "hundreds per second", 32-bit random, SYN retries, `getaddrinfo`, `SO_*TIMEO` (see 3.2.7), Elastic IP (see windows-prereq section 10), `boto3` (as an implementation assumption) |
| Cited a rule that is known | 1 | Rejecting duplicate `Content-Length` with differing values. RFC 9112 6.3 |

### Rejected

None. The 8 overstatement findings handled as "weaken" instead of "cite" are accepted findings. The point of the finding was that the document claimed more strength than the evidence, and lowering the strength when no evidence can be given is the other remedy for that finding.
