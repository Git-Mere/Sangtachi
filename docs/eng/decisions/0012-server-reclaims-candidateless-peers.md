# 0012. The Server Reclaims the Slot of a Participant Who Left Without Candidates

- Status: accepted
- Date: 2026-10-06
- Related: [ADR 0006](0006-star-topology-no-relay.md), [ADR 0008](0008-star-topology-followup-decisions.md) decision 5, 7, alternative 4, [ADR 0011](0011-lobby-is-room-membership.md), [`../control_plane.md`](../control_plane.md) 2.5, 2.6, 4.6, 5.3, 6.3, 6.5, 7.5, 8.4, [`../protocol.md`](../protocol.md) chapter 11, [`../roadmap.md`](../roadmap.md) Phase 3, 4

## Context

Decision 7 of ADR 0008 made the host the trigger of slot reclaim. The host reports a peer whose
session ended in `departed` of `host_report`.

**There are peers with no such trigger.** It is a player that did `join_room` but failed or did
`leave` before `register_candidate`. The host checks only counterparts that have candidates
(`control_plane.md` 4.6), so no session forms with this peer, and with no session to end there is
nothing to report. That slot stays until the room ends.

ADR 0011 made this hole bigger. Since a player can rejoin the same room from the lobby, each time a
player whose STUN failed comes back in, one more slot piles up. The pool is four, so after four
times that room is `room_full`. The cross-model review of the ADR 0011 work pointed it out.

## Decision

The repository owner decided the following. The first question was "moving room management to the
server", and after a plan that narrows the scope was reviewed separately, the narrow one was picked.

**1. The server reclaims a participant that has never registered a candidate.** The target is a
peer that is not the host, has empty candidates, and is past `JOIN_REGISTER_GRACE_S` (90 seconds)
from `joined_at_ms`. That peer's `NONCE#` is deleted with it.

**2. The reclaim is done while processing `host_report`.** That request already reads the whole
room, so no read is added. No separate scheduler or in-memory state is kept (`control_plane.md` 6.1
storage 5).

**3. The delete is conditional.** The condition includes "the candidates are still empty", so a
peer that registered after the read and before the delete is not deleted. A deleted peer that
registers late gets `unauthorized`.

**4. A peer that has registered candidates is reclaimed by the host only, as today.** Decision 7 of
ADR 0008 stays as it is within that scope.

**5. When it cannot be judged, nothing is deleted.** A peer with no `joined_at_ms` is left, and only
a log entry is written.

**6. When a session ends the host sends `host_report` right away without waiting for the next
period.** The repository owner pointed out the problem that, in a full room, a join that arrives
during the gap (at most 30 seconds) between one player leaving and the host reporting it gets
`room_full`. The gap shrinks to one request.

**7. Make the per-item `ttl` update of the lease renewal conditional and take it out of the
transaction.** It is a defect that came out in the review of this ADR. The unconditional update
revived a `PEER#` or `VIP#` that the same request had just reclaimed as an item holding only `ttl`.
The defect was in the host's `departed` reclaim as well.

ADR 0008 is not edited. This ADR narrows "the trigger of the reclaim is the host" of decision 7.

## Alternatives

**Alternative 1. The host reports after a grace period.**
Dropped. The principle "only the host reclaims" stays, but a fact the server knows directly from
the store is left to the judgement of the host client. A defect in the host implementation becomes
a slot leak as it is.

**Alternative 2. Move all reclaims to the server.**
Dropped. The server does not know whether a tunnel is alive. To know, connected peers would have to
signal the server periodically even during the game. It is the path alternative 4 of ADR 0008
dropped for the same reason. Then a control plane failure would reclaim the slot of a live peer,
the next participant would get the same virtual IP, and two sessions would claim one entry of the
host's routing table. The property of `spec.md` NFR-3, that the tunnel goes on even if the control
plane dies, is lost.

**Alternative 3. Reclaim only when `join_room` cannot find a free slot.**
Dropped. It writes less, but a reclaim path gets mixed into a join transaction that already has
many conditions.

**Alternative 4. Postpone it.**
Dropped. Rejoining from the lobby comes in with Phase 3, so it shows up right then.

**Alternative 5. Grow the virtual IP pool up to `10.100.0.10` instead of decision 6.**
The repository owner proposed it and it was dropped after review. Today the pool size is the room
capacity itself, so `room_full` is the only device that keeps the 5-person limit. Without counting
the capacity separately, one room grows up to 10 people, and the 5-person star structure of ADR
0006, the 6-entry routing table, and the measurements on a 4-pair basis are reopened. If the
capacity is counted separately as "the number of live participants", a peer that left but has not
been reported yet is still counted as one entry and the same `room_full` occurs. That is because
the server does not know that peer left.

## Consequences

**What we gain.**

- The slot of a participant who left without candidates frees up after the grace. Rejoining from
  the lobby does not pile up slots
- All the inputs of the judgement are in the store. It does not lean on the behavior of the host
  client

**What we pay.**

- There become two parties that delete a peer. A `by` field is added to `peer.released` in the log
- One write path is added to `host_report` processing, and every departure sends one more
  `host_report`
- The gap when a player process dies does not shrink. The host also learns of it only after the
  idle timeout passes
- A player that registers later than the grace is refused. That can happen when a long `--stun`
  list is given

**What we could not check.**

- Whether 90 seconds is enough for the actual join, STUN, and registration time. The value was set
  from the sum of the upper bounds in the documents (68 seconds) and was not measured
