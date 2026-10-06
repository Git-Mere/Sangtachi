# 0013. A Host Whose Room Ended on the Server Goes to the Lobby When Its Remaining Sessions End

- Status: accepted
- Date: 2026-10-06
- Related: [ADR 0011](0011-lobby-is-room-membership.md) decision 2, [ADR 0009](0009-room-host-lease.md), [`../control_plane.md`](../control_plane.md) 4.1, 4.6, 5.1, 6.4, 8.3, [`../protocol.md`](../protocol.md) chapter 11, [`../concurrency.md`](../concurrency.md) chapter 7, [`../roadmap.md`](../roadmap.md) Phase 3, 4

## Context

Decision 2 of ADR 0011 decided that a host that has set up a room leaves the room only by `leave`
and `quit`. It also left "whether the host stays in the room when `host_report` gets a definite
error after the room is set up" as an item to settle before Phase 3 starts.

Two rules disagreed at that spot. `control_plane.md` 8.3 ends a definite error right away as
`CONTROL_PLANE_EXCHANGE_FAILED`. 5.1 decides that an already established tunnel goes on as it is even
when the lease is cut. Following the former, a room with tunnels ends as a failure. Following only
the latter, there is a room that is dead on the server while the host stays in it. It is a room
nobody can enter.

## Decision

The repository owner decided the following.

**1. If the response says the room has ended on the server, stop and wait.** These are
`room_expired`, `room_not_found`, `unauthorized` and `bad_request`. The host emits a
`FAIL CONTROL_PLANE_EXCHANGE_FAILED` line once and stops `host_report`. The sessions that already
exist are left as they are.

**2. When all remaining sessions end, it goes to the lobby.** If there is no session, it goes right
away. This trigger is added to "only `leave` and `quit` leave the room" of decision 2 of ADR 0011.

**3. On `rate_limited` it calls again in the next period.** It does not emit `FAIL`. When another
source behind the same public IP has burned the budget, the room is alive. If the lease expires
while this goes on, the first request processed once the budget refills gets `room_expired` or
`room_not_found` and goes to decision 1.

**4. A transient error is retried in the next period, as today.**

ADR 0011 is not edited. This ADR widens decision 2.

## Alternatives

**Alternative 1. On a definite error the host goes to the lobby right away and closes the
sessions.**
Dropped. The tunnel gets cut because of the control plane's state. It goes against `spec.md` NFR-3.

**Alternative 2. Keep calling `host_report` even on a definite error.**
Dropped. An expired room cannot be revived (`control_plane.md` 5.1). The first three errors cut into
the source's rate limit budget (6.4), so they block other users behind the same public IP.

**Alternative 3. Treat `rate_limited` by decision 1 too.**
Dropped. Its strength is matching the general rule of 8.3. The price is that a live room dies by
lease expiry because of someone else's wrong room code. It happens where a public IP is shared,
such as a campus network.

## Consequences

**What we gain.**

- A host does not stay forever in a room that ended on the server
- Players already connected keep playing even after the room ends

**What we pay.**

- `host_report` becomes an exception taken out of the general error rule of 8.3
- One more trigger for going to the lobby is added, making five
