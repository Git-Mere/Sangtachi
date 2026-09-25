# 2026-09-14 Project Design Documents

## Changes

Wrote three design documents under `docs/`, based on the initial proposal.

- `docs/architecture.md`: design principles, system overview, client/control-plane module
  decomposition, data plane packet path and MTU arithmetic, tunnel header and packet types,
  session state machine, control plane operations and establishment sequence, virtual network
  subnet, failure diagnosis codes, telemetry metrics, target repository layout,
  external dependency scope
- `docs/spec.md`: filled in while keeping the existing template skeleton.
  Functional requirements FR-1 to FR-14, non-functional NFR-1 to NFR-8, constraints C-1 to C-5,
  success criteria split into minimum (M) / target (T) / analysis (A) with verification for each
- `docs/roadmap.md`: goal/tasks/deliverable/verification for Phases 1 through 9, mapping to
  milestones M1 to M3, P0 to P4 priorities, test environments, stretch goals, risk management
- `CSP400_P2P_Virtual_Network_Proposal.md` -> moved and renamed to `docs/first_design.md`

## Decisions

- `plan.md` was left alone. `roadmap.md` is the phase-level plan and `plan.md` is the checklist
  for the current work unit; the two split that role.
- Moved the tunnel header definition from Phase 5 to Phase 4. Hole punching has to distinguish
  `HELLO`/`KEEPALIVE`/`PING`/`PONG`, so the `type` field is already needed in Phase 4.
  Using a temporary format would mean throwing away Phase 4 code. Phase 4 owns the base header,
  Phase 5 owns the `DATA` type and robustness.
- The proposal's per-phase deliverables were diagram-level, which cannot be judged pass/fail.
  Added separate verification items to each phase that can decide success or failure.
- Added MTU arithmetic (maximum inner IP packet 1456 B assuming 1500). The proposal only said
  "handle packet size limits" with no figures. The real value is fixed by measurement in Phase 7.
- Added Wintun approval delay to the risk list. While waiting for approval, Phase 9 measurement
  items that do not need the adapter run first.
- `first_design.md` is reference material, not a fixed specification. Where it conflicts with the
  three documents, the three documents win.

## Verification

- The cross-links between the three documents and the `first_design.md` link all point at real paths
- No remaining references to the old file name `CSP400_P2P_Virtual_Network_Proposal`

## Cross-model Review

- Reviewer: Codex (GPT), technical accuracy lens
- Target: the staged documentation diff (`first_design.md` excluded; it is the user's original
  and not subject to review)
- Result: 2 blockers, 9 warnings. **All applied, none dismissed**

| Severity | Finding | Resolution |
|----------|---------|------------|
| blocker | Sending `HELLO` plus receiving the peer's `HELLO` cannot tell whether the peer received mine, so a "bidirectional confirmation" transition is impossible | Added a `HELLO_ACK` (nonce echo) packet type and changed the state machine transition condition |
| blocker | UDP send does not guarantee delivery, so "consecutive keepalive failures" cannot decide disconnection | Changed to an idle timeout (no packets from peer) basis |
| warn | If STUN and the tunnel use different sockets the discovered mapping is void | Added NFR-9 and stated the single-socket constraint in architecture 3.1 and Phases 2 and 4 |
| warn | Requiring identical results from two STUN servers does not hold under destination-dependent mapping NAT | Changed to recording each server's address separately and treating differences as observations |
| warn | The `TunnelHeader` struct is 20 bytes under MSVC alignment, so the 16-byte claim is wrong | Separated wire format from `sizeof` and specified field-by-field serialization |
| warn | An MTU of 1400 only holds if the outer PMTU is 1500 | Stated the dependency on that assumption and exceptions such as PPPoE |
| warn | Wintun does not configure IP addresses or routes | Split address/route configuration out to the Windows IP Helper API and added it to the dependency table |
| warn | NAT type cannot be determined from Binding results alone | Replaced "by NAT type" with "by observed mapping behavior" everywhere and noted the need for RFC 5780 |
| warn | Counting `sequence` gaps as immediate loss miscounts reordering and 32-bit wraparound | Added wrap-aware comparison and a reordering window, plus two new verification items |
| warn | Stopping only keepalive is invalid because other UDP traffic refreshes the mapping | Changed to an idle window with all sends on that socket stopped, measured with external probes |
| warn | A `ping` to the machine's own virtual IP is short-circuited locally and does not verify Wintun | Changed to peer-host ping plus Wintun ring counter verification |

### Second pass (consistency / verifiability lens)

- Reviewer: Codex (GPT)
- Result: 2 blockers, 15 warnings. 16 applied, 1 dismissed

| Severity | Finding | Resolution |
|----------|---------|------------|
| blocker | Phase 6 precedes tunnel integration, so requiring a peer-host ping is unverifiable at that phase boundary | Replaced with a local test using an unassigned virtual IP ping plus direct inject; peer round trips explicitly moved to Phase 7 |
| blocker | M-4's "supported NAT environment" was undefined, allowing success to be decided after the fact | Added a section defining supported/unsupported conditions and required test topologies ahead of the success criteria |
| warn | FR-1 was not traced to any phase | Added a requirements line to Phase 1 |
| warn | NFR / C / M / T families were not traced to phases | Added a requirement-to-phase traceability table |
| warn | spec C-5's P0/P1 disagreed with the roadmap priorities | Unified the wording and phase boundaries and cross-referenced them |
| warn | `register_peer` was absent from the sequence, leaving its relationship to `join_room` unclear | Specified that room creation/joining performs it internally and it is used only on reconnect |
| warn | architecture said local/public candidates while spec and roadmap said public only | Added local candidates to FR-5 and Phase 3 (same-LAN case) |
| warn | Phase 4 verified no failure code other than `HOLE_PUNCH_TIMEOUT` | Added a verification item that forces three codes to occur |
| warn | NFR-3 (tunnel survives control plane outage) had no verification item | Added a 10-minute control server outage test to Phase 5 |
| warn | The magic/version values and `HELLO` payload layout were undefined | Specified that they are fixed at Phase 4 start and recorded in `protocol.md` |
| warn | RTT "same order of magnitude" cannot be judged pass/fail | Quantified as median of 20 samples within 10 ms absolute or 30% relative |
| warn | The fuzz test had no seed, count, or length, so it was not reproducible | Specified a fixed seed, 100,000 packets, 1 to 2000 bytes |
| warn | The counter "matches" criterion had no counting layer or tolerance | Fixed as UDP payload bytes including the tunnel header, no tolerance |
| warn | Game latency "consistent with" and sync "normal" were subjective | Quantified as ±30 ms and 20 attempts reflected within 2 seconds |
| warn | M-2 does not verify that the port is actually receivable | Noted that this is confirmed in M-4/M-5 |
| warn | A-2 cannot be satisfied if no failure occurs in real environments | Added a controlled failure scenario (UDP blocking, nonexistent endpoint) as a required case |
| warn (dismissed) | The repository state description contradicts the `plan.md` reference | `docs/plan.md` does exist. The reviewer saw only the diff, not the repository. The sentence was still clarified because "repository" read ambiguously where "source tree" was meant |

## Wintun Approval

Approved on 2026-09-14. Reflected as follows.

- `architecture.md` dependency table: "instructor approval required" -> "approved (2026-09-14)"
- `spec.md` NFR-5: the prior approval rule stays, with the Wintun approval recorded
- `roadmap.md` Phase 6 task: the approval request item became approval complete
- `roadmap.md` risk management: the "Wintun approval delay" item marked resolved
