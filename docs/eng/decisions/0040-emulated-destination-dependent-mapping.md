# 0040. Measure an emulated destination-dependent mapping as a controlled condition

- Status: Accepted
- Date: 2026-11-18
- Related: [`../roadmap.md`](../roadmap.md) Phase 9 test environments, [`../architecture.md`](../architecture.md) chapter 8, [`../spec.md`](../spec.md) A-2·M-4, [ADR 0001](0001-no-direct-connection-fallback.md)

## Context

`roadmap.md` Phase 9 asks for success rates compared by observed mapping behavior. The "other available NAT behavior" row of the test environment table is that slot. Every real environment measured so far returned the same public port to both STUN servers. No network changed its port by destination, so one side of the comparison is empty. The repository owner said such a real network is hard to find.

ADR 0001 rejected a NAT emulation testbed (alternative A). That option was to replace real-environment evidence, and doing so would have required widening the "supported NAT environment" definition of M-4.

## Decision

**Create a destination-dependent mapping with a Linux NAT, and use the result only as a controlled condition.**

| Item | Decision |
|------|---------|
| How | A network namespace on `mere-galaxy`, with `iptables` `MASQUERADE --random-fully` on outgoing packets. A public port is picked at random per connection, so the mapping changes by destination. The home router does a second NAT behind it |
| Precondition | Before the trials, check that the client in the namespace gets different ports from the two STUN servers. If they are the same, do not run |
| Roles | The client in the namespace is the player. The other side is a host on a real network |
| Aggregation | The batch kind in the `tools/tm-analysis` batch file is `controlled`. It is not included in real-environment success rates, in the combined real-environment table, or in the M-4 support claim |
| Reporting | `experiments.md` states it is an emulation and reports it apart from the real-environment table |
| Cleanup | The namespace and the rules are removed after the test |

This does not reverse ADR 0001. What that ADR rejected was using emulation as real-environment evidence. This decision uses emulation only as a controlled condition for the comparison.

## Alternatives

| Option | Content | Reason rejected |
|----|------|-----------|
| A | Wait until a real network is found | The owner said one is hard to find. One side of the comparison could stay empty |
| B | Add a separate kind for emulation to the analysis tool | Changes the tool and its tests for one batch. `controlled` is already aggregated apart from real environments |
| C | Count the emulation result as a real environment | Not allowed, for the same reason as ADR 0001 alternative A |

## Consequences

**Gained.** The mapping-behavior comparison gets a destination-dependent case. A failure records its failure code; a success records which path connected.

**Accepted limits.**

- It is not real equipment. The namespace's connection tracking only lets a reply in when it comes from exactly the same address and port. So it also creates the strictest filtering. It is not claimed to match any real device
- It is a double NAT behind the home router
- It is repeated measurement on one device, against one peer, on one day. It is not generalized
