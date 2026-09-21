# 2026-09-21 Follow-up 4: spec logic errors

Follow-up 4 of the design audit (blockers 18 and 19, plus 3 viability warnings) is resolved. The
audit record moved to `audit-history/`, and the milestone split is gone. **Only Korean was edited.
The English mirror is the next task.**

## Changes

| File | Content |
|------|------|
| `spec.md` | M-2 is judged by comparing two STUN servers on the same UDP socket. M-5 becomes "sending an inner IP packet". M-6 becomes the local record file. C-5 tiers updated and a new section on the cost of removing a priority tier. Week pinning removed from C-4 |
| `roadmap.md` | Traceability table: Phase 4 = M-1 to M-6. Milestone overview section deleted. `DATA` minimum round trip, 8.5, and the local record added to Phase 4. Phase 5 keeps robustness only. 10 stale `protocol.md` references corrected |
| `protocol.md` | Chapter 15 preamble and the loss-accounting contradiction resolved. `DATA`, 8.4, and 8.5 moved to Phase 4. 4 self-references corrected |
| `architecture.md` | Local record file contract in chapter 9. 2 stale references corrected |
| `design-audit.md` | Moved to `audit-history/`. Record declaration added |
| `plan.md` | Follow-up 4 marked done |
| `CLAUDE.md`, `decisions/`, `commit_history/`, `tools/nat-probe/` | Paths only |

## Decisions

**1. The `DATA` minimum round trip moves from Phase 5 to Phase 4.**

What the user first proposed was swapping Phases 4 and 5 wholesale. It was rejected. The Phase 4
deliverables in `roadmap.md` already say "the packets exchanged use the real tunnel header and are
not replaced in Phase 5", so **the premise of the proposal was already true**. What was actually
missing was `DATA` alone. A wholesale swap breaks three things: the ordering principle (riskiest
first), C-5 (hole punching drops out of P0, so the minimum deliverable becomes a LAN tunnel), and
the schedule, which would push the riskiest step to the end.

There is one constraint. `protocol.md` 5.4 defines the `DATA` payload as a **complete inner IPv4
packet**, and 8.4 requires the virtual IP to match. In Phase 4 there is no Wintun, so a
**synthetic IPv4 packet** is built and inserted. The virtual IP uses the value the control plane
assigned in Phase 3.

**2. M-6 is met by a client local record file, not by the control plane DB.**

While investigating blocker 19, **the same defect was found in M-6**. The traceability table gave
M-6 to Phase 5, but the Phase 5 task list has not one line of control plane reporting. That work
is FR-14, and it is Phase 9. Left alone, "drop from P3 first when out of scope" becomes **an
instruction to drop a minimum success criterion**.

Moving it to a local record makes P0 cover M-1 to M-6 completely. The cost is written down. The
record does not follow the machine, and per-environment aggregation is not possible until Phase 9.
**The path and the line format are not decided.** Only the 4 contracts are written in
`architecture.md` chapter 9, and that document decides the rest before Phase 4 starts.

**3. The milestone split is removed.**

A warning about the M1/M2 workload imbalance was still open. Week allocation is set by actual
progress, so pinning it in a document soon disagrees with the facts. **This is not lowering a
criterion to cover the problem: the split itself was deleted, which removes the cause.**

**4. `design-audit.md` moves to `audit-history/`, and live documents drop the link to it.**

If an audit record is used as evidence, something that is not a settled document becomes the
source. All markdown links were removed from `architecture.md`, `protocol.md`, `roadmap.md`,
`spec.md`, `plan.md`, and `windows-prereq.md`. `CLAUDE.md`, `decisions/`, `commit_history/`, and
`tools/` exist to point at that audit, so only their paths were updated.

**The 42 stale `protocol.md` chapter numbers in that file are not fixed.** A record must hold the
judgement of its own time. That decision is written at the head of the file. The 15 references in
live documents were all matched to current numbers.

## Verification

- A sub-agent swept every `protocol.md` chapter and section reference. ok 162, stale 68, unknown 0.
  Only the stale ones in live documents were fixed
- Every relative link in `docs/kor/**/*.md` was checked. The only broken ones are the 5
  `experiments.md` paths, which are a Phase 9 deliverable and the gate's exception
- The 6 main documents have 0 `design-audit` markdown links and 0 plain-text mentions
- `python -m unittest test_docgate`: 105 cases OK
- **`docgate.py` itself was not run.** Only Korean was edited, so `mirror` and `parity` fail. That
  is the expected result. The gate runs right after the English mirror is built. Nothing was
  disabled
- `MAX_INNER = 1452` as the payload limit and `MAX_DATAGRAM = 1472` were confirmed in
  `protocol.md` chapter 3. This checked that the value the sub-agent wrote was not invented

## Cross-model review

Codex. 3 rounds. Round 1 was split into two lenses (internal consistency / scope and honesty).

| Round | Finding | Verdict |
|:--:|------|------|
| 1 | The audit file declares "no other document links to this file as evidence", but `CLAUDE.md`, the ADRs, and the commit records do link to it | **Accepted.** Who links and who does not is now written separately, with the reason |
| 1 | A file declared to be an audit record restates the current completion status and "only follow-up 6 remains", duplicating what `plan.md` manages | **Accepted.** The current-status declaration was removed and it points at `plan.md` |
| 1 | It says the success criteria "do not disappear" when P1 is dropped, but Phase 5 carries the FR-13, NFR-2, and NFR-3 verification | **Accepted.** The requirement verification that does disappear is stated, and the claim now carries that condition |
| 1 | The byte-equality check has no comparison point, so header rewriting or padding can produce a false pass | **Accepted.** Pinned to a byte-for-byte comparison, length included, between the payload just before encapsulation on the sender and the payload after passing 8.4 on the receiver |
| 1 | The boundary size check only says "the 20 to 1452 byte range", so whether it measures just outside the boundary is unclear | **Accepted.** Replaced with a case table of 19/20/576/1452/1453 |
| 1 | The completion claim is duplicated in `plan.md` and in the audit record (nit) | **Accepted.** `plan.md` is named as the source and the audit record side was cut down |
| 2 | The `protocol.md` chapter numbers in the audit record are stale (2 items) | **Rejected.** That document is a record, so it preserves the wording of its time. But rejecting alone brings the same finding back next round, so **the decision was written at the head of the file** |
| 3 | None | `LGTM - no blockers` |

The round 2 rejection is this pass's lesson. **A decision the reviewer cannot see keeps coming back
unless it is written in the document.** Writing the rejection reason only in the commit record does
not help: the next reviewer does not read it.

## What remains

- Follow-up 6 (reverting over-tightened verification criteria) is the only one left
- The path and line format of the local record file. `architecture.md` decides them before Phase 4
  starts
- The English mirror. Create the `audit-history/` directory, mirror every change in this commit,
  then pass `docgate`
