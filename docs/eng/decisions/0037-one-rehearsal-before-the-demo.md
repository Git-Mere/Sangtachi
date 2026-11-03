# 0037. Reduce the pre-demo check to one rehearsal and do not measure the mapping lifetime of the demo networks

- Status: Accepted
- Date: 2026-11-02
- Related: [ADR 0001](0001-no-direct-connection-fallback.md), [ADR 0019](0019-measure-mapping-lifetime-with-a-separate-tool.md), [ADR 0034](0034-decisions-before-phase-8.md), [`../roadmap.md`](../roadmap.md) Phase 8 tasks, [`../experiments.md`](../experiments.md) 3.3

## Context

[ADR 0034](0034-decisions-before-phase-8.md) set two pre-demo checks.

- Rehearse twice with the client on the same network pair as the demo. Once a few days before, once on the day. This replaces the Phase 8 preparation re-measurement and the just-before-demo re-measurement of [ADR 0001](0001-no-direct-connection-fallback.md)
- Measure the mapping lifetime of the demo networks, both the host's mobile hotspot and the player's home Wi-Fi

The demo is on 2026-11-05. On 2026-11-02 the demo player rehearsed from her home Wi-Fi, with the host at school on the owner's mobile hotspot. Creating a room, joining, and connecting to Minecraft all worked (`experiments.md` 3.3).

The purpose of measuring the mapping lifetime is to see whether the keepalive interval fits inside the mapping lifetime of that network ([`../protocol.md`](../protocol.md) chapter 11 Timers). If that verdict is wrong, it shows up as the session dropping on the idle timeout.

## Decision

This is the repository owner's decision.

- **Rehearse only once, a few days before.** The 2026-11-02 rehearsal is that one. There is no rehearsal on the day
- This rehearsal replaces the Phase 8 preparation re-measurement of ADR 0001. **The just-before-demo re-measurement is dropped with nothing replacing it**
- **The mapping lifetime of the demo networks is not measured.** If a session drops in the demo or a rehearsal, `tools/nat-probe` is used then to tell the cause

## Alternatives

**Alternative 1. Keep ADR 0034 as is.** Rejected. Measuring the mapping lifetime needs the demo player to run a separate tool twice. The rehearsal already checked the demo player's side.

**Alternative 2. Drop the rehearsal too.** Rejected. If the direct connection fails there is no fallback (ADR 0001). The only test that shows a failure in advance with time left to respond is the rehearsal a few days before.

## Consequences

- If conditions change just before the demo, it first shows in the demo. The risk ADR 0001 named as "once just before the demo is too late" is reduced only by one rehearsal. This is known and accepted
- Whether the 15-second keepalive interval fits inside the mapping lifetime of the demo networks is not judged. There are three grounds
  - The two homes already measured had a lower bound of 58 seconds and 300 seconds or more
  - 30 minutes of continuous play on the same hotspot did not drop (`experiments.md` 3.2)
  - The rehearsal session of about 10 minutes did not drop (`experiments.md` 3.3)
- Test 3 of `field-test/README.md` stays as a procedure. It is not a planned measurement
