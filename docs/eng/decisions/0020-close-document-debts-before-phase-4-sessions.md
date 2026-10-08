# 0020. Close Six Document Debts Before Phase 4 Session Implementation

- Status: Accepted
- Date: 2026-10-08
- Related: [`../protocol.md`](../protocol.md) 5.6, 8.5, 9.1, 9.4.1, 10.1, chapter 11, [`../concurrency.md`](../concurrency.md) chapters 4, 5, 7, 8, [`../control_plane.md`](../control_plane.md) 3.3, 3.5, 4.4, [`../roadmap.md`](../roadmap.md) Phases 4 and 8, [ADR 0017](0017-local-candidate-criteria.md), [ADR 0018](0018-reject-own-subnet-broadcast-in-received-candidates.md)

## Context

Six of the document debts in `plan.md` were waiting for decisions. The first three had to be decided before
Phase 4 session implementation. The options and trade-offs were shown to the repository owner, who chose
every recommendation.

## Decision

**1. `IDLE` has no session object.** The session is created as `PUNCHING` when peer candidates arrive. It is
not created before that even if the `peer_id` is known first. Owned by `protocol.md` 9.1 States.

**2. A session object in a terminal state is kept for 2 minutes and then deleted.** At most `MAX_PEERS - 1`;
beyond that, the one kept longest is deleted first. Leaving the room deletes them too. Kept sessions do not
count toward the session limit. Owned by `protocol.md` 5.6 `CLOSE`.

**3. `[loop]` builds and uses local candidates and the own-subnet set.** The lobby state machine, STUN, and
hygiene of received candidates already run on `[loop]` (`client/include/sangtachi/control/runner.hpp`).
Reading interfaces took a median of 2.1ms on this machine. Owned by `concurrency.md` chapter 5 State
Ownership.

**4. Add reserved `240.0.0.0/4` and "this network" `0.0.0.0/8` to candidate hygiene.** Both server and client.
Owned by `protocol.md` 10.1.

**5. Reject header lines with a space or tab inside the name.** The name is a token in RFC 9110 5.1. Owned by
`control_plane.md` 3.3.

**6. Rename the Phase 8 title to "Minecraft Validation and GUI Demo".**

The code and server deploy for 4 and 5 are tied to the Phase 4 display-name deploy (`roadmap.md` Phase 4).

## Alternatives

**For 1.** Create an `IDLE` session once the `peer_id` is known. Rejected. Early `HELLO`s could be counted
separately, but 9.4.1 and 8.1 would have to be rewritten and a counter added. That `HELLO` comes again every
200ms. Using the source of a `HELLO` received in `IDLE` as a candidate (ICE peer-reflexive) is outside v1.

**For 2.** Keep them until leaving the room. Rejected. They pile up when a host keeps a room open long. Delete
immediately. Rejected. 5.6, 9.4.1, roadmap verification, and `drop_terminal_state` would have to change or go,
and the place to diagnose post-shutdown receives would disappear.

**For 3.** `[control]` reads and hands over. Rejected. A new request kind appears and the rule that `[control]`
holds no state breaks. The stall is two orders of magnitude below the shortest timer.

**For 4.** Leave as is. Rejected. `HELLO` goes to meaningless destinations. Switch to an allowlist. Rejected. A
wrong list loses valid candidates, and the evidence for one has not been gathered.

**For 5.** Ignore as a header outside the table. Rejected. It would treat the same grammar violation as the
already rejected "space between name and colon" differently.

**For 6.** Leave as is. Rejected. Title and goal disagree.

## Consequences

**Gained.**

- The contradictions between documents that blocked Phase 4 session implementation are gone
- Document debts drop to five

**Paid.**

- For 4 and 5 the documents run ahead of the code. Until server and client are fixed, the deployed server stores
  the two bands and ignores whitespace inside names. `roadmap.md` Phase 4 owns that work
- Each kept terminal-state session adds one expiry
