# 0025. Decisions Before Phase 5

- Status: Accepted
- Date: 2026-10-15
- Related: [`../roadmap.md`](../roadmap.md) Phase 4 and 5, [`../protocol.md`](../protocol.md) 4.5, 9.6, 10.4, Chapter 15, [`../architecture.md`](../architecture.md) Chapter 9

## Context

Before starting Phase 5, a pass over the documents found six points that would get in the way. The repository owner picked one
option for each.

- `pkt_pos = position - back` in `protocol.md` 4.5 wraps around in 64 bits for a number older than the first accepted packet
- There is no place that emits the loss values. 4.5 says they are uploaded to the telemetry service, but that service is Phase 9
- Whether to retry automatically after `TUNNEL_DROPPED` is undecided (9.6, 10.4)
- The build has no means to judge "no memory error" in the fuzz test
- The criterion for closing Phase 4 was written as checkboxes, and nobody checked them. Writing progress state into a specification
  also conflicts with the rule that makes `plan.md` the single source of current state
- Where the two long-duration tests run is not decided

## Decision

| Point | Choice |
|------|---------|
| `pkt_pos` underflow | `position` starts at `REPLAY_WINDOW`, not 0. `back` is at most that, so the subtraction cannot wrap |
| Loss output | A fixed log event `session.loss` (`peer_id`, `expected`, `accepted`, `lost`), emitted at the same moments as the session counter tables |
| When renegotiation resets the accounting | Add that segment's values to the session totals, then reset. `session.loss` covers the whole session |
| Automatic retry | None. A new attempt starts when the player joins again |
| Memory error verdict | Run both fuzz stages as unit tests in a test build with ASan enabled (MSVC `/fsanitize=address`) |
| Where the long tests run | The 10-minute control plane outage and the 1-hour hold run as a separate script outside `scripts/test.ps1`. Both are judged with two processes on the same machine, and the 1-hour test is run once more between two different homes and kept as an observation |
| `protocol.md` Chapter 15 | Rename it the implementation conformance list and drop the checkboxes and Phase wording. `roadmap.md` sets the timing by pointing at Chapter 15, the result of comparing against the code goes to `commit_history/`, and the remaining items go to `plan.md` |
| Phase 4 captures C6-C9 | Add modes that create those scenes to the fake peer of the control server harness in Phase 5 |

## Alternatives

**Underflow.** A "before baseline" flag in the reorder branch was rejected. It changes the shape of the decision result and adds a
branch. Changing the accounting condition to `back < position - baseline` was rejected too. It is mathematically the same, but it
means removing the `pkt_pos` description of 4.5 and the acceptance window code with it. Moving the start value changes one constant.

**Loss output.** Putting `rx_expected` and `rx_accepted` into the session counter table was rejected. It needs no new key, but
reading the loss rate takes one more computation step. Emitting one more line every 10 seconds was rejected because it adds 360 lines
to a 1-hour session. Putting it in the local record file was rejected because it changes the format (`v=1`) of the M-6 verdict input.
The 10-second snapshot is the job of the Phase 9 upload.

**Renegotiation segments.** Counting only the last `CONNECTED` segment was rejected. The loss before the renegotiation would disappear
and the session would look better than it was.

**Automatic retry.** Having the player rejoin automatically with the same room code was rejected. There is no rejoin, so it gets a new
`peer_id` and a new virtual IP; to the game that is a different peer, so the connection on top of it breaks anyway. Continuing the old
session under the same `peer_id` was rejected too. It needs a new rejoin rule in the control plane and goes beyond P1 scope.

**Memory errors.** Judging with the current Debug build and the CRT debug heap was rejected. Standard library assertions only see
access through containers and iterators. Lowering the wording to "no crash and no assertion" was rejected because it lowers the bar.

**Long tests.** Judging only between two real homes was rejected. It needs a second machine and a person, and is hard to judge
automatically. Using only the same machine was rejected because it cannot see keepalive interacting with the NAT mapping lifetime.

**Chapter 15.** Checking the boxes from the code comparison was rejected. Progress state would enter a specification. Moving Chapter
15 to `roadmap.md` was rejected too. The list is what code implementing the protocol has to have, and its grounds are sections of
`protocol.md`.

**C6-C9.** Leaving them in Phase 4 with the field measurements was rejected. They are scenes one machine can create without a
second machine, so there is no reason to wait.

## Consequences

**Gains.**

- Loss wrapping negative becomes structurally impossible
- The loss values and output moments that Phase 5 verification reads are defined
- One open item of 9.6 and 10.4 is closed
- Chapter 15 stays a specification without a schedule, so it does not need fixing when Phases change

**Costs.**

- The absolute value of `position` shifts by 64. The expected values of the acceptance window tests that read it directly changed
- The fixed log events become eighteen
- One more test build configuration. The ASan build is slow and cannot be combined with `/RTC`
- If the connection drops during a demonstration, a person has to join again
