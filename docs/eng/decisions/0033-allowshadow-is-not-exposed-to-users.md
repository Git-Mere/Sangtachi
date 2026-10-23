# 0033. `-AllowShadow` Is Not Exposed to Users

- Status: accepted
- Date: 2026-10-23
- Related: [`../windows-prereq.md`](../windows-prereq.md) section 7, [`../architecture.md`](../architecture.md) chapter 7 adapter lifetime, [ADR 0026](0026-decisions-before-phase-6.md)

## Context

The subnet overlap check of `windows-prereq.md` section 7 blocks when our `/24` would shadow the range of a shorter
existing route (e.g. `10.0.0.0/8`) (`BLOCK-shadow`). The script has `-AllowShadow` for when a person knows that range
is actually empty. Whether the client exposes this override to users when it calls the check was to be decided in
Phase 6, but Phase 6 ended without deciding it. The client currently does not pass the override.

## Decision

The repository owner decided.

**The client does not expose `-AllowShadow` to users.** It is not a startup argument, a console command or a setting.
The check the client calls always uses the default (block). `-AllowShadow` is used only when a person runs the script
directly.

## Alternatives

**Alternative 1. Expose it as a startup argument.**
Rejected. Machines with a wide range, such as a corporate VPN, could then use the client. The cost is that the user must
judge whether that range is empty, and if wrong, real hosts in that range become unreachable while connected. Section 7
holds "working while cutting off someone else's is not better than not working" as its criterion.

## Consequences

**Gains.**

- The client has no path that shadows someone else's range
- This is the current code. No code changes

**Costs.**

- On a machine with a shorter route covering our range, v1 cannot be used until that interface is brought down. This is
  the same cost as the "Limit" of section 7
