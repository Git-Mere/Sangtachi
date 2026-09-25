# 2026-09-14 Bilingual Documentation Split

## Changes

Split `docs/` into `docs/kor/` and `docs/eng/`, with both trees holding the same file set.

```text
docs/
+-- kor/                      Korean (existing documents moved here)
|   +-- architecture.md
|   +-- spec.md
|   +-- roadmap.md
|   +-- plan.md
|   +-- first_design.md       translated from the English original
|   +-- decisions/README.md
|   +-- commit_history/*.md
+-- eng/                      English (newly translated)
    +-- (same layout)
```

- `first_design.md` was originally written in English, so the original stays in `eng/` and a new Korean translation was written into `kor/`.
- The other documents were originally written in Korean, so they stay in `kor/` and translations were written into `eng/`.
- Every document carries a link to its counterpart in the other language at the top.
- The repository layout section of the root `README.md` was updated to `docs/kor/` and `docs/eng/`.

## Decisions

- File names are kept in English on both sides. The path already indicates the language;
  translating file names too would make cross-links and commit history comparison awkward.
- No index file was placed at the `docs/` root. The requested structure is that `eng/` and
  `kor/` mirror each other, and a root file would break that symmetry.
- `protocol.md` and `experiments.md` do not exist on either side yet. They are Phase 5 and
  Phase 9 deliverables and only the links are in place. Both sides get written together.

## Fixed Along the Way

Pre-existing inconsistencies found while moving the documents.

- The module table in `architecture.md` 3.1 still said Wintun was "approval required". Only the
  dependency table in section 11 had been updated to approved; the module table was missed.
- `spec.md` had NFR-9 wedged between NFR-1 and NFR-2. Moved into numeric order.
- Absolute-style references such as `docs/experiments.md` and `docs/protocol.md` were changed to
  same-folder relative links. The language split would have broken those paths.
- The prerequisite line for Phase 5 in `roadmap.md` omitted `HELLO_ACK`, which is a packet type
  implemented in Phase 4. Added.
- The priority block in `roadmap.md` was misaligned.

## Verification

- File listings of `eng/` and `kor/` match
- Heading counts match per document: architecture 23, spec 15, roadmap 45, first_design 56
- Section numbers (1 to 23) in `first_design.md` match on both sides
- Phase headings (1 to 9) in `roadmap.md` match on both sides
- Full relative markdown link check: every link resolves except two deliberate forward
  references, `protocol.md` and `experiments.md`, which are absent symmetrically on both sides
  and are written in Phases 4/5 and 9 respectively

## Maintenance Cost

There are now two copies of every document, which introduces drift risk. When a document
changes, both sides must change in the same commit. This rule is stated in the repository
layout section of `architecture.md` section 10.

## Cross-model Review

- Reviewer: Codex (GPT), translation accuracy and internal contradiction lens
- Result: 2 blockers, 7 warnings, 1 nit. All applied, identically in both languages

| Severity | Finding | Resolution |
|----------|---------|------------|
| blocker | Phase 4 used ICMP `ping` as the RTT baseline, but ICMP toward a peer behind NAT is commonly blocked or answered by the peer's router, so it does not measure the same path | Changed the baseline to a timestamped UDP echo on the same socket and endpoint pair. ICMP demoted to supplementary information |
| blocker | The Phase 6 deliverable diagram showed communication between two virtual IPs, while the verification in the same section stated that this belongs to Phase 7 | Replaced the deliverable with a single-host read/inject diagram and stated that peer communication is the Phase 7 deliverable |
| warn | `protocol.md` was said to be recorded at the start of Phase 4, while the repository layout and roadmap said it is written in Phase 5 | Unified as started in Phase 4, completed in Phase 5 |
| warn | Every failure was required to record a public endpoint, but `STUN_DISCOVERY_FAILED` means no endpoint was ever discovered | Relaxed to recording whichever fields are available |
| warn | The "keepalive failure count" metric contradicts receive-based disconnect detection. UDP keepalive has no acknowledgment, so delivery failure cannot be counted | Narrowed the definition to local `sendto` errors and added idle timeout events as a separate metric |
| warn | The traceability table assigned FR-13 to Phase 5, but Phase 5 never mentioned FR-13 nor verified `TUNNEL_DROPPED` | Added FR-13 to the Phase 5 requirements and introduced an idle timeout test |
| warn | The supported-condition wording "both peers observe the same public IP:Port" reads as the two peers sharing one endpoint | Clarified as "each peer observes the same public IP:Port for itself across two STUN servers" |
| warn | CGNAT was classified as unsupported by definition, though some CGNAT deployments use endpoint-independent mapping and do permit hole punching | Classified by observed behavior rather than carrier topology; CGNAT demoted to "potentially difficult topology" |
| warn | "0 broken links" contradicted the absence of `protocol.md` / `experiments.md` | Reworded to report the two deliberate forward references explicitly |
| nit | The English "All UDP send and receive shares" was ungrammatical | Corrected to "All UDP sends and receives share" |
