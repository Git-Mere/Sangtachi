# 0009. Keep a Room Alive by a Host Lease

- Status: accepted
- Date: 2026-09-28
- Related: [ADR 0008](0008-star-topology-followup-decisions.md) decision 7, [ADR 0006](0006-star-topology-no-relay.md) decision 4, [`../control_plane.md`](../control_plane.md) 2.5, 2.6, 5.1, 6.5, [`../spec.md`](../spec.md) NFR-3, [`../concurrency.md`](../concurrency.md) chapter 8

## Context

A room's lifetime is fixed from the moment of creation. The 2.6 constants of `control_plane.md`
write `ROOM_TTL_S = 3600` and 5.1 Room writes that the value is not extended.

> **Expiry is not extended.** `ROOM_TTL_S` (3600 seconds) is fixed from the moment of creation.
> After the connection is established the control plane is not needed (NFR-3), so this value is
> **the period for which the room code is valid** and is unrelated to session length.

Two things pushed that rule out.

**First, a friend cannot come in later.** What the repository owner asked for is to make a room and
have someone else enter that room three hours later. Under the current rule the room code dies once
an hour has passed. 5.1 had already written that limit down. It is "if a rejoin becomes necessary
after a 30-minute game, the room is expired, `room_expired` comes back, and a new room is created.
This is a limit v1 accepts".

**Second, a room stays for an hour even after the host dies.** Decision 7 of ADR 0008 made the
reclaim of a virtual IP slot the host's notification, and the party that tells is the host itself.
If the host dies with no room to tell, the control plane does not know it. The room and the slot
stay until expiry, and whoever comes in with that code fails with nobody to attach to.

**That failure disguises itself as a NAT failure.** If the host died after registering its
candidates, the pair is ready, and the new participant waits until the punch deadline and gets
`HOLE_PUNCH_TIMEOUT` ([`../protocol.md`](../protocol.md) 9.6 Failure Transitions). The real cause is
the absence of the other side, while the record and the user guidance say a NAT failure. ADR 0007
raised the GUI into the core scope, so that guidance is seen by the user directly.

## Decision

**Turn a room's lifetime into a lease held by the host.** Six items are part of this decision.

**1. The host renews the room lease periodically.** While it keeps the session the host signals the
control plane periodically, and that signal pushes the room's expiry time back.

**2. Remove the absolute upper bound on a room's lifetime.** The room lives while the host signals.
A participant who arrives three hours later comes in with the same room code.

**3. The lease ending does not cut the tunnel.** What lease expiry blocks is new joins and control
plane operations. An already established tunnel goes on as it is. Even if the control plane dies
for a while and the host cannot send its signal, the game continues.

> **Why.** `spec.md` NFR-3 requires that "a control plane failure does not cut an already
> established P2P tunnel". Making the lease a survival condition of the tunnel breaks that
> requirement.

**4. Merge the host's leave notification and the lease renewal into one operation.** The
notification decision 7 of ADR 0008 asked for and the renewal of this decision are both the host
speaking to the control plane periodically. Splitting them into two operations puts two sets of the
same authentication and the same rate limit budget. The name, the fields, and the errors are owned
by chapter 4 of `control_plane.md`.

**5. When the host leaves the room the lease is cut.** If the host goes back to the lobby or closes
the program, that room is closed. Decision 4 of ADR 0006 put in no host migration, so a room without
a host is of no use to anyone.

**6. The `[control]` thread lives on while the session is kept.** The premise that the thread set by
chapter 8 of `concurrency.md` has nothing to do after the connection is established changes on the
host side. The player side does not change.

### What This ADR Does Not Decide

| Item | Deciding document |
|------|-------------------|
| The actual values of the renewal period and the lease length. The proposal is a 30-second period and a 120-second lease | `control_plane.md` 2.6 constants |
| The name, path, fields, errors, idempotency, and rate limit budget of the merged operation | `control_plane.md` chapter 4 |
| What is shown to the host when a renewal fails | `architecture.md` chapter 8 |
| How the changed meaning of `expires_in_s` is written into the response description | `control_plane.md` 4.2, 4.3 |
| Whether the renewal writes fit inside the DynamoDB free tier | `roadmap.md` Phase 3 pre-start item |

## Alternatives

**Alternative 1. Leave it alone.**
Dropped. Its strength was that no operation is added. The price is taking both items of the context
above as they are. The room code lives for an hour only, the room of a dead host stays until expiry,
and that failure is recorded as a NAT failure.

**Alternative 2. Put a TTL on every peer.**
Dropped. Alternative 4 of [ADR 0008](0008-star-topology-followup-decisions.md) already dropped this
one. Everyone would have to knock on the control plane even after the connection is made. Decision 1
of this ADR is a reduced version of that alternative and the knocking side is the host alone. In a
5-person room the load is one fifth.

**Alternative 3. A separate device that detects the host's death.**
Dropped. It needs yet another means of detection and that means becomes the same periodic
communication as alternative 2.

**Alternative 4. Extend the room lifetime only.**
Dropped. This is the plan of raising `ROOM_TTL_S` to three hours or a day. It solves the first
problem only and makes the second worse. The room of a dead host stays around that much longer.

## Consequences

**What we gain.**

- The room code lives together with the host. A friend comes in three hours later too
- The room of a dead host disappears within the lease length. A participant who arrives after that
  gets the answer that there is no room **at once**, instead of waiting until the punch deadline and
  getting a NAT failure
- 5.1 Room can answer "when does the room disappear". It is within the lease time after the host
  stops signalling
- The leave notification rides on the same operation. The host has one place where it speaks to the
  control plane

**What we pay.**

- **The host keeps speaking to the control plane after the connection too.** The `[control]` thread
  of `concurrency.md` chapter 8 lives through the whole session on the host side
- Control plane writes happen periodically per room. Until now writes happened at join and at
  registration only
- The control plane use of the host and of a player becomes asymmetric. Decision 6 of ADR 0006, that
  the rules are written as one set, is about the data plane so nothing is broken, but the control
  plane side descriptions split by role
- Zombie rooms do not disappear entirely. They stay for as long as the lease length

**What we could not check.**

- The free tier impact of the renewal writes. How many writes one room that stays up all day makes
  is calculated once the values are decided
- How the host handles a renewal failure when the control plane is down for a long time. Decision 3
  set only that the tunnel is kept, and the retry policy follows 8.3 Error Classes and Retry of
  `control_plane.md`
