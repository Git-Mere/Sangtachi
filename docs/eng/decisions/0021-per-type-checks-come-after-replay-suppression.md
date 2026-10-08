# 0021. Per-type checks come after replay suppression

- Status: Accepted
- Date: 2026-10-08
- Related: [`../protocol.md`](../protocol.md) 4.5, 5.5, 5.6, 5.7, section 8, [`../roadmap.md`](../roadmap.md) Phase 4

## Context

The receive validation pipeline in `protocol.md` section 8 orders 8.1 common checks, 8.2 epoch and source checks, and 8.3 replay
suppression. Among the per-type checks, 8.4 `DATA` inner validation and 5.5 `PONG` matching were already placed after 8.3 (4.5 puts
`PONG` matching after replay suppression). The 5.6 `CLOSE` reason check could be read as after 8.3 only from the valid-packet list in
9.4, and 5.6 itself gave no order. The five 5.7 `ROSTER` receive checks only said "after the 8.1 common checks", so whether they
come before or after 8.2 and 8.3 was missing. Building the Phase 4 wire codec and the
acceptance window exposed this gap. At the same spot, "is a packet dropped after passing 8.3 still recorded in the window" was not
written as a sentence either.

## Decision

The repository owner chose.

**1. The order is 8.1, 8.2, 8.3, then the per-type checks.** The per-type checks are 8.4 `DATA` inner validation, 5.5 `PONG`
matching, the 5.6 `CLOSE` reason, and the 5.7 `ROSTER` receive checks. Side effects happen only after every check passes. The one exception is 9.5 Renegotiation, which 4.5 already states.

**2. A packet that passed 8.3 is still recorded in the acceptance window even if a later per-type check drops it.** The 4.5
pseudocode updates state at the 8.3 step, so this was already the rule, and it is now written as a sentence.

**3. `ROSTER` goes into the valid-packet list of 9.4 and the validation list of 10.4 (a).** Endpoint learning also uses only packets
that passed the `ROSTER` receive validation.

## Alternatives

**Alternative 1. Put the per-type checks right after 8.1, before 8.2.**
Rejected. A malformed packet would be dropped before it reaches the epoch decision and the acceptance window. The cost is threefold.

- Only `DATA` and `PONG` would sit elsewhere, so there would be two ordering rules
- A `ROSTER` duplicated by the network has the same `generation` and would be counted as `drop_roster_stale`. That cannot be told
  apart from "the host sent a stale roster"
- A `ROSTER` or `CLOSE` whose epoch differs from the pinned one would split into per-type counters depending on its content, so the
  8.2 counters would not count that cause in full. An epoch on the retired list is caught first by check 8 of 8.1
  (`drop_retired_epoch`), so this difference does not apply to it

The apparent gain, "a malformed packet does not use up a number", makes no real difference. A correct packet with the same number
does not arrive again in normal operation, and a sender that can pass 8.2 can send well-formed packets from the start (section 2).

## Consequences

**Gains.**

- There is one ordering rule. Every type goes common, epoch, replay, per-type
- One counter points to one cause. Duplication is `drop_duplicate`, an epoch mismatch is 8.1 `drop_retired_epoch` or an 8.2
  counter, and a content error is the per-type counter

**Costs.**

- A malformed `ROSTER` or `CLOSE` also uses up one number at 8.3
