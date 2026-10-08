# 0018. Reject Own-Subnet Broadcast in Received Candidates

- Status: Accepted
- Date: 2026-10-08
- Related: [`../protocol.md`](../protocol.md) Chapters 2 and 7 and 10.1, [`../control_plane.md`](../control_plane.md) 4.4, [ADR 0017](0017-local-candidate-criteria.md)

## Context

The received candidate hygiene of `protocol.md` 10.1 judged broadcast as `255.255.255.255` only, on the
grounds that the peer's prefix is unknown so subnet directed broadcast is not judged. So if the peer or a
malicious server puts our LAN's directed broadcast (e.g. `192.168.0.255`) in as a candidate, we send
`HELLO` to that address every 200ms during the punch. Those packets reach every device on our LAN.

[ADR 0017](0017-local-candidate-criteria.md) gave the client an own-subnet set. We now know our own
subnet's netmask.

## Decision

The repository owner decided the following.

**1. The client rejects, as hygiene, received candidates whose address is a broadcast blocked by the own-subnet set.**
It is judged with the set at the time of receipt. It belongs to the hygiene rejection step of the
processing order.

**2. The server does not apply this row.** The server has no such set. The server's bands are unchanged.

**3. A directed broadcast of the peer's subnet is still not judged.**

## Alternatives

**Alternative 1. Leave it as document debt.**
Rejected. It leaves an amplification path open while the input to judge it already exists.

**Alternative 2. Reject every address whose last octet is 255.**
Rejected. It rejects normal addresses on networks whose netmask is not `/24`, and misses the broadcast of
our own network when it is not `/24`.

**Alternative 3. Have the client send its prefix so the server can judge too.**
Rejected. It conflicts with why `protocol.md` 10.1 does not send the prefix (disclosing the internal
network layout), and the side that must block is the sending client itself.

## Consequences

**Gained.**

- The amplification path toward our LAN is closed. "Amplification is blocked" in the Chapter 2 Security
  Model table now holds for our own subnet on the candidate list side too

**Paid.**

- Client and server hygiene bands differ by one row. The client can drop a candidate the server stored
- A broadcast of an interface that changed between collections is not blocked until the next collection
  (10.1 residual risk)
- The Phase 3 client code (`sanitize_received_candidates`) does not have this row yet. It goes in together
  with Phase 4 local candidate collection
