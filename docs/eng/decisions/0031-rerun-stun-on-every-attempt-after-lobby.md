# 0031. Run STUN Again on Every Attempt After the Lobby

- Status: accepted
- Date: 2026-10-23
- Related: [`../control_plane.md`](../control_plane.md) 8.4, [`../concurrency.md`](../concurrency.md) chapter 7, [ADR 0011](0011-lobby-is-room-membership.md) decision 8

## Context

ADR 0011 decision 8 said that rejoining from the lobby runs STUN again and left whether to skip it undecided until it was
measured. The UDP socket is kept in the lobby, so while the mapping lives the previous STUN result could in principle be
reused.

In the 2026-10-22 two-home field test (test 1) the clients reconnected after about 3 min 28 s in the lobby. At both homes
the STUN mapped port was the same in both attempts, and both sides reached `CONNECTED` again. The client ran STUN again
on the second attempt too.

## Decision

The repository owner decided.

**Every attempt after the lobby runs STUN again.** It is not skipped based on how long the process stayed in the lobby.
The previous STUN result is not used as a candidate for the next attempt. This closes the open point of ADR 0011
decision 8.

## Alternatives

**Alternative 1. Reuse the previous STUN result when the lobby stay is shorter than some time.**
Rejected. The measurement is two homes, once, in the 3-minute range. Nothing shows that the mapping survives other stay
lengths or other routers, so no threshold can be set. If the mapping changed and a stale candidate is registered, the
peer's punch does not arrive through that candidate. The connection then depends on other candidates or on endpoint
learning ([`../protocol.md`](../protocol.md) 10.4), and if that also fails the attempt fails. The saving is a few STUN queries.

## Consequences

**Gains.**

- If the mapping changes during the lobby stay, the next attempt registers candidates from the new mapping
- This is the current code. No code changes

**Costs.**

- Every attempt waits for the STUN queries. The wait is the same as on the first attempt
