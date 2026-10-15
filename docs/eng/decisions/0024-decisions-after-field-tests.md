# 0024. Decisions after the field tests

- Status: Accepted
- Date: 2026-10-15
- Related: [ADR 0006](0006-star-topology-no-relay.md), [ADR 0021](0021-per-type-checks-come-after-replay-suppression.md), [ADR 0022](0022-counters-keep-a-table-per-session.md), [ADR 0023](0023-decisions-before-round-3-h-and-i.md), [`../roadmap.md`](../roadmap.md) Phase 4, [`../protocol.md`](../protocol.md) 5.7, [`../architecture.md`](../architecture.md) Chapter 9

## Context

`field-test/` test 1 (two different homes) and test 2 (same router) were run. Three places had to be
decided from the results.

- **Host NAT warning.** ADR 0006 left open "whether to judge the risk of the host NAT before the room is
  created and warn the person", and `roadmap.md` Phase 4 said the nat-probe re-measurement decides it. In the
  re-measurement both homes were endpoint-independent and all four host pairs succeeded three times in a
  row. No bad NAT was measured, so the measurement does not give the answer
- **`drop_roster_stale`.** In the player logs it rose by 9, 4 and 2 in normal sessions. The host sends the
  same roster again with the same `generation` (`protocol.md` 5.7 sending times), and receive check 3
  accepted only a larger value. The counts matched the resend schedule exactly
- **Counter tables printed twice.** On `quit` during `CONNECTED`, that session's table appeared twice. Both
  the terminal-state time and the process-shutdown time in `architecture.md` Chapter 9 cover that session

The repository owner accepted the recommendation for all three.

## Decision

| Place | Choice |
|------|---------|
| Host NAT warning | Not added |
| `ROSTER` receive check 3 | Accept when `generation` is **not less than** the last accepted value. Only a smaller value is `drop_roster_stale` |
| `roster` log line | Emitted only for a roster whose `generation` is the first or a larger one. Narrows "per accepted roster" of ADR 0023 |
| Counter tables printed twice | Left as is. Chapter 9 states that a table can appear twice and that analysis uses the last one |

## Alternatives

**Adding the host NAT warning.** The option was to print one warning line before `register_candidate`
when the two STUN observations differ (a mapping whose port changes per destination). Rejected. The 22
measurements taken in September and this re-measurement never met such a network, and if one is met the player side
already records the failure as `HOLE_PUNCH_TIMEOUT`. What it gains is small next to the cost of newly
fixing and testing the wording, timing and input.

**Silently discarding an equal `generation`.** Discarding without raising a counter. Rejected. Chapter 8
keeps a dedicated counter for every discard, and a discard without a counter would be an exception to that
rule. A roster with the same value also has the same content, so accepting it changes nothing.

**A separate counter for an equal `generation`.** Rejected. A counter that counts normal operation is not
used by any verdict.

**Emitting a `roster` line on every accept.** Rejected. The same line would appear every 15 seconds and
the moment the roster changed would be hard to find in the log.

**Removing the double print.** The options were to skip sessions already emitted, or to add a field marking
when the line was emitted. Rejected. A session kept in a terminal state can still raise
`drop_terminal_state` afterwards, so skipping it loses the increase. A marking field has no user because no
tool sums the log today. Revisit at Phase 9 analysis.

## Consequences

**Gains.**

- `drop_roster_stale` is 0 in a normal session. If it rises, a roster arrived late out of order
- The client gains no warning path

**Costs.**

- If the host NAT is bad, the person learns it from the players' failures after the room is set up. The
  cost of ADR 0006, that the whole room fails, stays
- A `ROSTER` with an equal value is an accepted packet, so it does what an accepted packet does (endpoint
  learning and so on) the same as other types
- A session's counter table can appear twice in the log
- The cost ADR 0021 gave for its alternative 1, "a `ROSTER` duplicated by the network is counted as `drop_roster_stale`", no longer holds. The example in `protocol.md` Chapter 8 was changed to `PONG`
