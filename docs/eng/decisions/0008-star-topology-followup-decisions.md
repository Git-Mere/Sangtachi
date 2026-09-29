# 0008. Nine Follow-Up Decisions for the Star Topology and the GUI

- Status: accepted
- Date: 2026-09-28
- Related: [ADR 0006](0006-star-topology-no-relay.md), [ADR 0007](0007-gui-core-scope-qt.md), [`../protocol.md`](../protocol.md) chapter 5, 8.5, 10.2, chapter 14, [`../control_plane.md`](../control_plane.md) 2.4, 2.5, 4.3, 4.4, 4.5, [`../concurrency.md`](../concurrency.md) chapter 7, [`../architecture.md`](../architecture.md) 3.5, chapter 8, chapter 9, [`../spec.md`](../spec.md) FR-13, FR-15

## Context

[ADR 0006](0006-star-topology-no-relay.md) decided the topology alone and left seven items as
"What This ADR Does Not Decide". [ADR 0007](0007-gui-core-scope-qt.md) decided the scope, the tier
and the toolkit of the GUI alone and left four. The two lists overlap. What carries a member's
connection state is in both as the same question.

The items that were left are tied to one another, so they could not be decided separately. Whether
the virtual IP slot is reclaimed hangs on whether rejoin is kept, and whether rejoin is kept hangs
on what the process does when the host disappears. So the repository owner decided nine at once.

**This document is the record of that judgement.** The source of values and rules is the living
documents. The sentences that actually enforce the decisions below are owned by `protocol.md`,
`control_plane.md`, `concurrency.md`, `architecture.md` and `spec.md`.

## Decision

### 1. When the host disappears the player leaves the room and the process goes to the lobby

The tunnel-side result was already decided by decision 4 of ADR 0006. When the host's session ends,
every tunnel of that room ends. What this decision adds is what comes after. The player leaves the
room of the control plane as well, and the process does not die but goes back to the state of
picking a room.

It reverses the sentence below in `concurrency.md` chapter 7 Shutdown.

> **Once a terminal state is reached the process ends too.** When a session becomes `FAILED` or `CLOSED` (...)
> in the minimum scope there is one session, so once that session ends the process has nothing to do.

It uses the generalization decision 6 of ADR 0006 asked for, as it is. The rule is written with the
session list. When the list is empty it goes to the lobby. A player is the case where the list has
1 entry.

### 2. Ready and `punch_delay_ms` move to a pair basis

Today they are on a room basis. `control_plane.md` 4.5 `get_peers` writes that "the definition of
ready is the existence of `ROOM.ready_at_wall_ms`", and contract 3 of `protocol.md` chapter 14
writes that "`punch_delay_ms` is fixed once per room when the second registration completes".

Move them to a pair basis. Once every candidate of one host-and-player pair is registered, that
pair is ready. It does not look at the state of another pair.

**A late join is allowed.** It is an item ADR 0006 left open and it is the result of this decision.
A player who starts late punches as soon as its own pair is ready. Sessions that are already
attached take no effect from it.

### 3. The transmit check picks a session by destination and then looks at the state

Decision 3 of ADR 0006 pointed at the problem and did not decide the order. Check 1 of the current
`protocol.md` 8.5 Transmit-Side Validation is "state is `CONNECTED`", but when there are several
sessions, which session that state belongs to is not fixed until a session is picked by
destination.

The new order has six steps.

| # | Check | Failure counter |
|---|------|-------------|
| 1 | Length >= 20 | `tx_drop_short` |
| 2 | IP version nibble == 4 | `tx_drop_not_ipv4` |
| 3 | Length <= `MAX_INNER` | `tx_drop_oversize` |
| 4 | Source IP == own virtual IP | `tx_drop_bad_src` |
| 5 | Pick a session by destination IP. If there is none, fail | `tx_drop_no_route` |
| 6 | The picked session is `CONNECTED` | `tx_drop_not_connected` |

The reason the length check is first is the same as in the current table. The source and the
destination sit at offsets 12 to 20, so reading them before the 20-byte lower bound is checked
reads outside the buffer.

**The routing table is a fixed array of 6 slots.** The index is the last octet (0 to 5) of the
destination virtual IP and the value is a session. A lookup has two steps. First it sees whether
the destination is inside the virtual range, then it reads the slot. Outside the range, broadcast
and multicast are caught at the first step.

- One's own slot is left empty. A packet sent to one's own virtual IP has no route
- A player's table fills the host slot alone. The other player slots being empty is the place that
  enforces decision 2 of ADR 0006
- The owner of the table is the `[loop]` thread. It changes only when a session comes into being or
  disappears

> **Why.** The virtual IP pool is fixed from `10.100.0.1` to `10.100.0.5` (decision 5 of ADR 0006),
> so no hash map is needed. A fixed array has no allocation and a constant lookup time.

`tx_drop_no_route` is a global counter. It is the case where no session was picked, so there is no
session to attribute it to.

### 4. The host sends the room roster and the connection states over the tunnel

It is the answer to the same question of ADR 0006 and 0007. We make one new tunnel message type,
and the host sends the room roster and each member's connection state to each player.

The basis is the source of truth. Exactly as ADR 0006 writes it, the host holds every session
directly, so it knows who is attached to begin with. What the control plane knows is `joined` and
`registered` alone and neither of the two is whether the peer is connected (`control_plane.md` 5.3
Peer).

**We do not extend the control plane.** We do not make a separate service server either. Accounts,
a friend list, a room list and presence are outside the scope of this project.

This decision makes the third behavior of `spec.md` FR-15 and the last condition of T-7 judgeable.

### 5. The virtual IP slot of a peer that left is reclaimed

It reverses the rule below in `control_plane.md` 2.5 Virtual IP Pool.

> **While the room is alive it is not reclaimed.** A slot stays with that peer even after the peer leaves.
> It is because a rejoin (4.3) has to give back the same address.

The basis of that rule was rejoin, and decision 6 removes rejoin. The basis disappears, so the rule
changes with it. A new join can enter a reclaimed slot.

### 6. Rejoin is removed

Of the two forms of `join_room` in `control_plane.md` 4.3, the rejoin one is removed. There are
four targets.

| Target | Where |
|------|------|
| The rejoin form of `join_room` and the descriptions attached to it | `control_plane.md` 4.3 |
| Contract 7 (the same `peer_id` and virtual IP for a rejoining peer) | `protocol.md` chapter 14 |
| The `--rejoin` argument and the `REJOIN` standard output line | `architecture.md` 3.5 Startup Inputs, `client/src/args.cpp` |
| The "ready but 0 peer candidates" window after a rejoin cleared the candidates | `control_plane.md` 4.5 |

**`peer_token` stays.** `get_peers` and `register_candidate` authenticate with it. What disappears
is the proof that survives across a restart.

**If the process dies, that peer loses its slot.** Coming back in is a new join and the virtual IP
changes. A player connects to the host's `10.100.0.1` alone, so the effect on the game side is
small. If the host restarts, the room disappears and the room code is handed out again. It is the
same direction as decision 1.

### 7. The trigger of the reclaim is the host

The host tells the control plane that a session was cut. That call deletes that peer's item and its
virtual IP claim. One new operation is added.

The basis is the same as decision 4. What knows about the cut is the host, and the control plane
does not store a fact it does not know itself.

### 8. The console build goes back to the lobby too

Decision 1 is not split by build. The console build too does not end when it has no session; it
goes back to the state of creating a room or entering a room code.

**The process lifetime and the session lifetime are separated.** The blast radius of this decision
is the largest. It is the place `concurrency.md` chapter 7 Shutdown announced with "when automatic
retry is decided, this line is looked at again", and here that line is looked at again.

### 9. A failure shows the code and human words together

It emits the failure code of `spec.md` FR-13 as it is, and next to it a sentence a person reads.
It does not emit only one of the two.

> **Why.** With the code alone the user does not know what to do, and with the sentence alone what
> the user reports cannot be matched against the classification of `architecture.md` chapter 8
> Failure Diagnosis.

## What This ADR Does Not Decide

The place where they are decided is the living documents and the timing is owned by
[`../roadmap.md`](../roadmap.md).

| Item | Deciding document |
|------|-------------|
| The wire layout, the send timing, the size cap and the receive validation of the tunnel message of decision 4 | `protocol.md` chapter 5 and chapter 8 |
| The operation name, the authentication method, the contention rules and the handling of the room when the host leaves, for decision 7 | `control_plane.md` chapter 4 |
| The input the lobby of decision 8 takes. The relation between the console vocabulary and the CLI arguments | `architecture.md` 3.5 Startup Inputs |
| The scope of `client_nonce`. Today's rule is one value per launch (`control_plane.md` 2.4) and a lobby return joins several times in one launch | `control_plane.md` 2.4 |
| Whether the socket and the STUN result are reused when going back to the lobby | `protocol.md` chapter 6, 10.1, chapter 13 |
| The procedure for setting the adapter address again when the room changes | `concurrency.md` chapter 3, Phase 6 |
| The list of sentences of decision 9 and where those sentences are put | `architecture.md` chapter 8 |
| The GUI thread model and the GUI startup input path | The open items ADR 0007 left. Phase 8 |

**Decision 6 cuts the contracts of `protocol.md` chapter 14 from eight to seven.** Whether they are
renumbered is decided by that document.

## Alternatives

**Alternative 1. The control plane carries the member states.**
Dropped. The peers would have to keep reporting the connection state, and then the control plane
holds a stale copy of data plane state. Polling keeps running even after the connection stands.
What it gains is not touching the tunnel protocol.

**Alternative 2. Do not make a member state at all.**
Dropped. The third behavior of `spec.md` FR-15 and the last condition of T-7 cannot be judged. It
goes against ADR 0007 raising the minimum GUI into the core scope.

**Alternative 3. Keep rejoin.**
Dropped. Keeping it makes the virtual IP slot impossible to reclaim (`control_plane.md` 2.5). It is
an alternative that cannot stand together with decision 5. What it gains is coming back to the same
slot even after the process dies, and that value is small in a structure where the room ends once
the host disappears (decision 4 of ADR 0006).

**Alternative 4. Do the reclaim with a TTL on the peer item.**
Dropped. Its strength was that no operation is added. The price is that each peer has to knock on
the control plane periodically even after the connection stands, and that the slot frees up as late
as the expiry time.

**Alternative 5. The console build ends.**
Dropped. It is the repository owner's decision. Its strength was leaving `concurrency.md` chapter 7
Shutdown nearly as it is. The price is that the lifetime rules of the console and the GUI split in
two, and it goes against decision 6 of ADR 0006 asking for one set of rules.

## Consequences

**What we gain.**

- Six of the seven open items of ADR 0006 and two of the four open items of ADR 0007 close. What is
  left is the GUI thread model and the GUI startup input path, and both are Phase 8
- There are places where the documents shrink. Removing rejoin removes the two forms of
  `join_room`, one `unauthorized` reason, the candidate deletion description and the 0-candidate
  window of `get_peers` together
- A late join stands with no separate device. It is the result of pair-basis ready
- One set of rules is left. Shutdown, the transmit check order and the lobby return do not split
  into a host one and a player one, or a console one and a GUI one

**What we pay.**

- **The process lifetime and the session lifetime are separated.** The startup input, the scope of
  `client_nonce`, the reuse of the socket and the STUN result, setting the adapter address again
  and the meaning of the exit code all hang on that separation. Half of "What This ADR Does Not
  Decide" above came out of here
- One message type is added to the tunnel protocol, and receive validation and tests grow by that
  much
- One operation is added to the control plane. That operation has to check that the caller is the
  host
- A peer whose process died cannot come back on the same virtual IP
- The routing table is tied to the range and the pool size. If the range changes (the alternate
  range item of Phase 6), the index rule of the table is looked at with it

**What we could not check.**

- Whether at most 4 `CLOSE` messages and the adapter cleanup fit inside the 3-second grace of
  `CTRL_CLOSE_EVENT` when the host shuts down. It is an item ADR 0006 left and it is not a thing to
  decide but a thing to measure
- What a lobby return does to the NAT mapping. Keeping the same socket looks like it keeps the
  mapping, but it was not measured
- The contention behavior of pair-basis ready when 5 people join at the same time. The current case
  table is on a 2-party basis (`control_plane.md` 4.4)
