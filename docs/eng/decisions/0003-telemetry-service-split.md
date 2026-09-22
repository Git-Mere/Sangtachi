# 0003. Split Telemetry Collection Out of the Control Plane

- Status: accepted
- Date: 2026-09-22
- Related: [`../architecture.md`](../architecture.md) 3.4, [`../spec.md`](../spec.md) NFR-10, [`../roadmap.md`](../roadmap.md) Phase 9

## Context

The original design put telemetry collection inside the control plane. The module table in
`architecture.md` 3.3 had `telemetry.py`, the control plane operation table in 6.1 had
`report_connection` and `report_telemetry`, and the client uploaded metrics over the control
plane TCP socket. One AWS box in the system diagram held rooms, peers, candidate exchange, metric
collection, and storage all together.

We looked at this layout again right before fixing the control plane schema. The two jobs differ
in nature.

| Axis | Control plane | Telemetry |
|----|-----------|-----------|
| Priority | P0 (Phase 1-4) | P3 (Phase 9) |
| Traffic | Small and latency-sensitive | Large and latency-insensitive |
| Consistency | If the peer list and virtual IPs in one room disagree, the connection fails | Loss is tolerated. `architecture.md` 3.2.6 already decided to drop when the queue is full |
| Rate of change | Must stay stable because the client implementation depends on it | Changes often during the Phase 9 experiments |
| If absent | M-1 to M-5 do not hold | M-1 to M-6 hold unchanged. The evidence for M-6 is the local record file |

The last row is the point. `architecture.md` chapter 9 already decided that what M-6 requires is
the local record file, not storage in the control plane. So telemetry upload **is part of no
verdict in the minimum success.** Yet if it lives inside the P0 service, the load and the schema
churn of metric collection ride on the same process as the P0 path.

## Decision

**Telemetry collection becomes a service separate from the control plane.**

1. The control plane handles only room create/join, peer registration, virtual IP assignment,
   and candidate endpoint exchange.
2. `report_connection` and `report_telemetry` **both** move to the telemetry service. We do not
   take the compromise of leaving connection results in the control plane and moving only
   metrics. Connection success/failure and the failure stage are the first row of the metric
   table in `architecture.md` chapter 9, and in the Phase 9 analysis they sit on the same row as
   the other metrics. Splitting them across two places means joining two stores for every
   analysis.
3. There is **no runtime dependency** between the two services. Neither calls the other. The four
   contracts are in `architecture.md` 3.4.
4. The single touch point is one identifier namespace. `room_id` and `peer_id` are issued by the
   control plane, and telemetry records them as opaque values.
5. Deployment starts as **two processes on one EC2 instance, on separate ports**.

## Alternatives

**Alternative 1. Leave it as is (a module inside the control plane).**
Rejected. Metric collection puts load and schema churn on the P0 process. Every schema fix in
Phase 9 would redeploy the P0 process that handles room state, and that is the process NFR-3
requires to keep room state across restarts.

**Alternative 2. Split, but let telemetry ask the control plane whether identifiers are valid.**
Rejected. For data quality this one is better. A `room_id` the control plane never issued can be
filtered at collection time. But then **the control plane takes on the load and failures of
telemetry.** Each metric row adds one lookup, and when that lookup slows down the P0 path slows
down. The reason for the split disappears. Instead, polluted rows are filtered at analysis time
by cross-checking against the control plane's records.

**Alternative 3. Split into two instances from the start.**
Rejected. It does not fit the cost and operating burden of a semester project. **The split is a
logical boundary first; the physical placement can be chosen later.** If decision 3 above is
kept, there are **fewer reasons to change service code** when we later split instances or move to
a container service. Deployment settings, network rules, credentials, and the addresses the client
sees change at that time. **We do not say "no code changes."** That cannot be checked before the
move is tried. What we do now is only keep the two addresses separate in the client configuration.

## Consequences

**What we gain.**

- The metric schema can change throughout Phase 9 without redeploying the control plane
- If the telemetry service **process** stops, room create/join and `get_peers` are not blocked by
  that (NFR-10). **Stopping together because of resource exhaustion is not covered here.** See
  what we pay below
- The control server built in Phase 3 contains no telemetry code at all. The P0 implementation
  shrinks

**What we pay.**

- **Four living documents were changed.** The number of changed places is larger. `spec.md` scope,
  FR-14, NFR-10, C-3; `architecture.md` chapter 2 diagram, 3.2.6, 3.2.7, 3.3, 3.4, 6.1, 6.3,
  chapter 9, chapter 10, chapter 11; `roadmap.md` Phase 3 and Phase 9; `protocol.md` 4.5. The
  split is simple as a concept, but the references were spread wide
- **Collected data cannot be trusted.** This is the price of rejecting alternative 2 above.
  Identifiers the control plane never issued are stored too. The Phase 9 analysis filters them by
  cross-checking
- **Split 4 does not cover resource exhaustion.** No dependency in the design and resource
  isolation are different statements. While both live on one instance, if telemetry exhausts disk
  or CPU the control plane stops with it, and that happens even when every contract in this ADR
  is kept. **There is no resource isolation mechanism yet.** How to partition is listed as a
  Phase 9 pre-start item
- **One more service.** Deployment targets, ports, firewall rules, and log locations each become
  two

**Not decided yet.**

The telemetry service's collection schema, whether it authenticates, upload interval, and
retention period are not decided. This ADR fixes **only the boundary**. The rest belongs to the
Phase 9 pre-start items in `roadmap.md`. Whatever authentication is chosen later, it stays
consistent with this ADR as long as it avoids the shape of alternative 2 above (telemetry asking
the control plane).
