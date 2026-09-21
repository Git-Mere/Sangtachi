# 2026-09-21 Follow-up 6: reverting over-tightened verification criteria

4 verification criteria that could not be judged, or that a correct implementation could not pass,
were fixed. `plan.md` was cut down to a handover document for the next session. **All 6 design
audit follow-ups are done. Only Korean was edited.**

## The 4 criteria that were fixed

**1. Phase 7 byte counters.** It read "exactly equal to the packet capture, no tolerance". Capture
loss and UDP segmentation offload mean a correct implementation cannot pass either. Instead of
inventing a tolerance, a **judgeable window** was defined. Judge only within a window where the
capture tool reports 0 loss; if it is not 0, that is not a failure, it is discarded and measured
again. Within that window the byte sums must match. If the capture sum exceeds the counter, that is
a failure: bytes were sent without being counted, so it is a counting bug. Packet counts are
recorded for reference only. Offload changes the number of datagrams, but the byte sum stays the
same.

**2. Phase 8 game latency.** It read "within ±30 ms of the tunnel RTT". There was no measurement
method and no measurement point, and it **assumed the two measure the same thing**. The game value
is an application-layer round trip, and it sits on top of the tunnel. The absolute number was
dropped and replaced by a directional condition. The median of the latency the game displays must
be greater than or equal to the median tunnel RTT.

**What this criterion judges was given a name. It is instrumentation consistency, not quality.** No
matter how large the latency gets, this criterion alone does not fail it. The quality judgement
belongs to the reflection test (2 seconds).

**3. Phase 9 statistical design.** It was undefined. The trial definition, the interval between
trials, independence, the representative value, the success-rate interval, and the sample count
were pinned. Two points matter. 10 runs executed back to back on the same network on the same day
are **not called independent trials**. They are recorded as repeated measurements. When an interval
is attached to a success rate, the **Wilson score interval** is used. The normal approximation
produces a wrong interval when n is small and the proportion is near 1.

**4. A-2 controlled failure.** An artificial failure was pinned as a required case without writing
what it is evidence of. The scenario stays, but the boundary was pinned. A controlled failure is
**evidence that the failure-handling path works**, not evidence about the failure rate in a real
environment. If there are 0 real failures, 0 is what gets written.

All 4 also write down **what is no longer guaranteed**. Lowering a criterion without writing the
scope is the same as removing it.

## `plan.md` cleanup

Cut from 299 lines to 60. The history of finished work was removed, leaving only what remains, what
is waiting, the documentation debt, and what comes next. The `plan.md` descriptions in `CLAUDE.md`,
`architecture.md`, and `roadmap.md` were matched to it.

## Verification

- All 4 criteria write a pass condition and a fail condition as sentences. Which one applies can be
  judged by reading
- Every newly added value carries evidence, or, where no evidence could be given, the value was
  **not decided and the decision point was written down**
- `docgate.py` itself was not run. Only Korean was edited, so `mirror` and `parity` fail

## Cross-model review

Codex. 4 rounds.

| Round | Finding | Verdict |
|:--:|------|------|
| 1 | The Phase 7 "fixed count" is undefined, so it is not a reproducible procedure | **Accepted.** No number was invented; it is fixed and written down on the first run. The sizes reuse the values in the Phase 5 boundary table |
| 1 | Phase 8 "60 seconds / 20 runs" has no evidence | **Accepted.** Added the derivation that the display updates once per second, so 20 runs take about a minute, and stated that this is not a claim of statistical sufficiency |
| 1 | The Phase 8 pass condition is weak, so an implementation with excessive latency also passes | **Accepted.** Named as an instrumentation-consistency judgement, with the quality judgement tied to the reflection test |
| 1 | Phase 9 "at least 10 runs" has no basis for the choice | **Accepted.** Written as the executable minimum, with an explicit statement that it is not a sufficiency claim |
| 2 | Comparing medians with no tolerance fails a correct implementation on jitter and rounding alone | **Accepted.** An inversion within the interquartile range of the tunnel RTT is allowed |
| 3 | On a quiet LAN the interquartile range becomes 0, so display rounding alone fails it | **Accepted.** The tolerance is the larger of the interquartile range and 1 unit of display resolution |
| 4 | None | `LGTM - no blockers` |

## What this pass taught

**Fixing one `±30ms` took three wrong attempts.**

1. The sample-condition constants were given no evidence
2. Removing the tolerance entirely failed a correct implementation
3. Deriving the tolerance from the measurement made it 0 on a quiet network

**"No tolerance" and "an arbitrary tolerance" are two symptoms of the same illness.** Both pick a
number without measuring. The fix is neither removing the tolerance nor choosing a different
number; it is **deriving the tolerance from the measurement**. But the case where that measurement
becomes 0 has to be looked at with it.

And **rule 3 is not a question you ask once**. Fix it so a correct implementation passes, and a bad
implementation passes too; tighten that, and a correct implementation fails again at the boundary.
The evidence is that this work, which was about fixing over-tightened criteria, created 4 new
defects of the same kind.
