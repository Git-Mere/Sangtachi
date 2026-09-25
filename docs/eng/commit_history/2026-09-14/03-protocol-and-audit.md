# 2026-09-14 Full Design Audit and protocol.md

> This record was not written at the time of commit `9808640`; it was added retroactively on
> 2026-09-15, reconstructed from that commit's diff and [`../../audit-history/design-audit.md`](../../audit-history/design-audit.md).

## Changes

- Added `design-audit.md`: audit method, root causes, 20 blockers, 25 warnings, 6 follow-up
  steps, 4 recurrence-prevention rules, and the records of protocol.md review rounds 2 to 4
- Added `protocol.md` (638 lines). It settles constants, offsets, integer widths, byte order,
  timer values, the full transition table, the receive validation pipeline, the duplicate
  suppression algorithm, candidate hygiene rules, and the security model, to minimize the
  points where implementation would otherwise decide ad hoc
- `architecture.md`: header 16 to 20 bytes, MTU 1456 to 1452, added the `CLOSE` packet type,
  added socket ownership and demultiplexing rules, reduced section 5 to a summary that defers
  to `protocol.md`
- `roadmap.md`: changed Phase 4 from designing the protocol to implementing it, replaced the
  Phase 5 prerequisite with the `protocol.md` checklist
- `spec.md`: FR-8 moved to the 20-byte header, FR-11 gained three inner-packet validation checks

## Decisions

**The audit targeted whole documents, not diffs.** Three lenses (mechanism, platform,
achievability) were run independently in parallel. The diff reviews used until then had been
catching only local defects.

**Two root causes were recorded.**

1. No scenario had ever been traced end to end. Component lists and verification items do not
   reveal the gaps.
2. Applying review findings locally in earlier rounds created new contradictions and
   unachievable criteria. **An over-tightened criterion is as bad as a loose one.**

**The header grew from 16 to 20 bytes** with the addition of `session_epoch`. Without it, delayed
packets from a previous instance are accepted into a new session after a restart. Every 4-byte
field was placed at a multiple-of-4 offset so MSVC default alignment adds no padding, but
field-by-field serialization is still mandated. Relying on alignment happening to work out breaks
silently.

**Phase 4 no longer designs the protocol.** An unsettled wire protocol turned out to be a blocker
at the "cannot be implemented" level, so `protocol.md` was written as a settled document before
implementation begins. If a missing value is found during implementation, `protocol.md` is fixed
first rather than the value being decided ad hoc in code.

**Candidate nomination was replaced with endpoint learning.** ICE-style nomination deadlocks when
two peers behind the same NAT nominate the LAN path and the hairpinned public path respectively,
filtering out each other's packets entirely. WireGuard-style learning handles both that case and
NAT rebinding recovery.

## Verification

- eng/kor heading counts match (protocol 43, design-audit 16, architecture 23, spec 15,
  roadmap 45, first_design 56)
- Every relative link except `experiments.md` resolves. That one is the only unresolved link and
  is identical on both sides (a Phase 9 deliverable)

## Cross-Model Review

- Reviewer: Codex (GPT)
- Full audit: three lenses in parallel. 20 blockers, 25 warnings. These 45 were not "applied or
  dismissed" but sorted into items fixed immediately and follow-up steps 1 through 6
- `protocol.md`: the first draft was reviewed under two lenses (30 blockers), then the revision
  was reviewed twice more (5, then 5). **All 40 were applied and none dismissed.** Section 7 of
  `design-audit.md` counts the drafting itself as the first pass and so calls these 2, 3, and 4

| protocol.md review | Blockers | Character |
|--------------------|----------|-----------|
| First draft (2 lenses) | 30 | Gaps in the design itself |
| Revision, pass 1 (section 7's round 3) | 5 | Side effects of the endpoint learning just added |
| Revision, pass 2 (section 7's round 4) | 5 | Pseudocode branch precision, interactions among the preceding fixes |

The applied-fix details below are reconstructed from the section 7 record and the commit diff.

The defects that actually mattered:

| Defect | Fix |
|--------|-----|
| On renegotiation both peers drew a new local epoch, producing an **infinite epoch ping-pong**. The two peers never connect | Keep the local epoch and nonce; change them only when starting a new attempt from `IDLE` (9.5) |
| **There was no duplicate suppression algorithm.** A retransmitted `DATA` is injected twice and a retransmitted `KEEPALIVE` revives a dead session | `highest` plus a 64-bit bitmap, evaluated before any side effect (4.5) |
| Candidate nomination deadlocks behind the same NAT | Replaced with endpoint learning (10.4) |
| `is_newer(a,b)` returned true for `a == b`, classifying duplicates as new packets | Added the `a != b` condition (4.4) |
| A spoofed source could redirect all game traffic at an arbitrary victim, contradicting the third-party-harm promise in section 2 | Addresses outside the candidate set switch only after passing nonce path validation (10.4 c) |
| Classifying on magic made `drop_magic` and `drop_version` permanently 0 and hid malicious traffic in `drop_unclassified` | Classify on the top two bits; magic moved to the validation stage (section 7) |
| The bitmap cleared itself at `shift == 64`, so the previous `highest` was re-accepted despite being a duplicate | Clear only when `shift > 64`; `shift == 64` sets bit 63 |
| `seq == highest` fell into the "older" branch, giving `uint64_t{1} << (0-1)`, undefined behavior | Put an explicit duplicate check first |

**All eight defects above were documented before implementation began.** Had the missing
duplicate suppression and the nomination deadlock surfaced in Phase 4, they would have appeared
as "it sometimes fails to connect" and been hard even to reproduce.

### What this commit left open

No further review round was run, and **no claim was made that nothing remained.** It was also
recorded that all five findings in the last pass were side effects of the preceding fixes.
Section 11 of `protocol.md` did in fact have to be revisited during follow-up 2 (2026-09-15).

## Remaining Follow-ups

Steps 2 through 6 are tracked in `design-audit.md` section 5. Blocker 20 (no alternative if a
direct connection proves impossible) is the most dangerous.
