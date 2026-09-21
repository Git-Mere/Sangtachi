# 0001. Do Not Add a Fallback for Direct Connection Failure

- Status: accepted
- Date: 2026-09-20
- Related: [`../audit-history/design-audit.md`](../audit-history/design-audit.md) blocker 20 and follow-up 5, [`../commit_history/2026-09-20-blocker20-resolution.md`](../commit_history/2026-09-20-blocker20-resolution.md)

## Context

The full design audit named blocker 20 the most dangerous item.

> If both available home networks show symmetric behaviour, the minimum success is impossible and
> there is no alternative. A relay is a post-Phase-9 stretch goal and cannot rescue it.

This is not a defect we can fix. ISPs and routers decide it, and if it surfaces mid-semester there
is no time to undo it. Design audit follow-up 5 stated that the decision cannot be made without
measurement, and required measuring **both** mapping behaviour and filtering behaviour.

A diagnostic tool ([`../../../tools/nat-probe/`](../../../tools/nat-probe/)) was built and used
from 2026-09-17 to 2026-09-20.

| Item | Value |
|------|-----|
| Measurements | 22 |
| Distinct networks | 7 (3 T-Mobile, 3 Comcast, 1 KT) |
| Mapping verdict | **`endpoint-independent` in all 22** |
| Punch result | **`success` in all 22** |
| Symmetric (destination-dependent) NAT | **0** |
| Network pairs attempted | 6. 0 failures |

Required test topology (2), "2 different home networks", succeeded **3 times in a row** on
2026-09-18. No condition was changed between rounds, and the end times agreed within 1 second each
round.

## Decision

**No fallback is added. M-1 and M-4 in `spec.md` stay exactly as written.**

- The relay stays a post-Phase-9 stretch goal. It is not promoted to P1
- The required test topology contract is unchanged
- The direct-first claim stands

**Deciding not to revise is also a decision.** Revising the criteria would state a weaker claim
than what was actually achieved.

**Instead, re-measurement gates are set at three points:** the start of Phase 4, Phase 8
preparation, and right before the demo. Once, right before the demo, is too late: there would be
no time left to react. The reasoning is in the "Consequences" section. This
decision rests on observations at one point in time, and a router replacement or an ISP
configuration change can invalidate it.

## Alternatives

| Option | Content | Reason for rejection |
|----|------|-----------|
| A | Build a NAT emulation testbed and reproduce the desired mapping behaviour | Unnecessary now that real-world evidence exists. Emulation is less convincing analytically, and it would require widening the definition of "supported NAT environment" in M-4 to include emulation |
| B | Promote a minimal relay to P1 as a fallback when direct fails | Direct succeeded on all 6 pairs. Making a fallback mandatory weakens the direct-first claim and adds a phase. It spends implementation effort with no observed failure to justify it |
| C | Add a third path such as a mobile hotspot to the required test topology | Required topology (2) already passed 3 times in a row. There is no reason to put uncontrollable conditions (carrier, time, signal) into a mandatory contract. The hotspot stays best-effort |

All three options carried the same cost: **the success criteria would have to be revised along
with them.** The measurements satisfied the criteria, so that cost no longer needs paying.

## Consequences

**What this gains.**

- The Phase 1-9 structure is untouched. No new phase
- The success criteria in `spec.md` and the phase structure in `roadmap.md` are unchanged. Only
  three re-measurement tasks are added to `roadmap.md` (start of Phase 4, Phase 8 preparation,
  and right before the demo)
- No implementation effort is spent on a relay

**Risks accepted.**

- **The evidence is split across measurements.** No single measurement satisfied "NAT-NAT +
  different ISP + Windows-Windows" at the same time. The combination is covered and all 6 pairs
  attempted succeeded, but it was not proven within one measurement
- **The different-ISP pair is not 2 home networks.** In the T-Mobile to Comcast pair one side is a
  mobile hotspot, which is a best-effort topology under `spec.md` line 123
- **The sample is concentrated in the western and midwestern US plus one site in Korea.** Three
  Comcast networks are close to half the total. The possibility of meeting a symmetric NAT at
  another ISP is not excluded
- **There is no fallback for the fallback.** If conditions change on demo day and direct fails,
  nothing remains. This is accepted knowingly

**Required follow-up.**

1. **Put a re-measurement gate at three points.** Once, right before the demo, is too late: if
   conditions have changed, there is no time left to react.

   | Point | Reason |
   |------|------|
   | Start of Phase 4 | Before the hole punching implementation begins. A change found here still leaves time to revise the design |
   | Phase 8 preparation | When the demo environment is being set up |
   | Right before the demo | A same-day check |
2. Keep the raw measurements and records local via `.gitignore`. They contain public IPs and the
   repository is public. **Do not attach them as-is to the report either.** A submission system
   and shared links create the same exposure. **Replace public addresses with irreversible
   pseudonyms (`network A`, `network B`) and keep timestamps only to the date.** Masking the last
   IPv4 octet is not enough and does not apply to IPv6. Otherwise obtain explicit consent from the
   people involved. State in the report which was done
3. Carry the four "risks accepted" items into the report verbatim. Do not hide the limits of the
   evidence
