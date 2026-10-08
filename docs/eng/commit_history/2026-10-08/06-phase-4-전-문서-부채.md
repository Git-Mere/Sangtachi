# Six Document Debts Before Phase 4 Session Implementation (ADR 0020)

## Why

Six of the document debts in `plan.md` were waiting for decisions. The options and trade-offs were laid out
and the repository owner chose every recommendation.

## What changed

| Decision | Where |
|------|-----------|
| 1. `IDLE` has no session object | `protocol.md` 9.1 table and paragraph, 9.4.1 `IDLE` row. One verification line in `roadmap.md` Phase 4 |
| 2. Terminal-state sessions kept 2 minutes, cap `MAX_PEERS - 1`, deleted on leaving the room | `protocol.md` 5.6, the session count sentence in 8.5, chapter 11 expiry table. `concurrency.md` chapter 4, chapter 7 shutdown and the lobby table. `roadmap.md` Phase 4 verification |
| 3. Local candidates and the own-subnet set live on `[loop]` | `concurrency.md` chapter 5 State Ownership, chapter 8 response interpretation |
| 4. `0.0.0.0/8` and `240.0.0.0/4` in candidate hygiene | `protocol.md` 10.1 hygiene table and paragraph. `control_plane.md` 4.4 rejection policy and band paragraph. `roadmap.md` Phase 4 implementation |
| 5. Reject header lines with whitespace inside the name | `control_plane.md` 3.3 header line syntax, 3.5 client check 3. `roadmap.md` Phase 4 implementation |
| 6. Phase 8 title | `roadmap.md` heading line |

Also `decisions/0020` (new ADR) and `plan.md` (next steps, places to revisit during implementation, document
debts from eleven to five).

## Important decisions

- **3 was smaller than the documents suggested.** The lobby state machine and response interpretation already
  run on `[loop]` (`control/runner.hpp`). Measuring `GetAdaptersAddresses` 20 times gave a median of 2.1ms and a
  maximum of 4.8ms
- **The cap in 2 was added to the decision.** With only 2 minutes, frequent joins and leaves could push the
  session count past the "single digit" of `concurrency.md` chapter 4
- **For 4 and 5 the documents run ahead of the code.** The server redeploy is tied to the Phase 4 display-name
  deploy. The deployed server keeps its current behavior until then
- **For 4, chapter 7 source hygiene was not changed.** The basis is that the two bands mean nothing as
  destinations; answering a packet that came from them causes no amplification

## Verification

| What | Result |
|------|------|
| Interface read time | `GetAdaptersAddresses(AF_INET)` 20 times via ctypes. Median 2.06ms, maximum 4.83ms |
| RFC check | RFC 9110 5.1 `field-name = token` checked in the original text |
| Document gate | `docgate.py` `VERDICT: pass` |

## Cross-model review

Codex. Two lenses (cross-document consistency, English mirror and records) run separately on the staged diff. The
mirror and records lens returned `LGTM - no blockers`.

| Finding | Handling |
|------|------|
| warn. `protocol.md` 8.5 says packets from the old peer after virtual IP reclaim are `drop_unknown_peer` right away, but a kept old session passes check 7 | Accepted. `drop_terminal_state` while kept, `drop_unknown_peer` once deleted |
| warn. `ROSTER` membership is not defined apart from sessions. `IDLE` members have no session and departed members keep one | Accepted. Members are the host itself and the `peers` of the last `host_report` response. Not built from the session list. Bit 0 is 1 only for a `CONNECTED` session |
| warn. 4.3 "register the previous attempt's pinned epoch when starting a new attempt" assumes the value survives session deletion | Accepted. Read from the kept terminal-state session; if deleted, there is no value. Noted that no path for a new attempt with the same peer is defined today |
| warn. The `host_report` rule for a room that ended on the server asks "if sessions remain", which kept terminal-state sessions satisfy | Accepted. "Sessions not in a terminal state". `protocol.md` chapter 11, `control_plane.md` 4.6, `concurrency.md` chapter 7 lobby table |

The second review raised one warn. The new bit-0 rule contradicted the host's own entry (always 1). Accepted. The
session-based rule applies only to player entries, and the host entry is always 1.

The third review (checking that fix) returned `LGTM - no blockers`.
