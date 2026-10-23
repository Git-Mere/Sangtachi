# 0032. The Byte Count Check Uses a File Transfer

- Status: accepted
- Date: 2026-10-23
- Related: [`../roadmap.md`](../roadmap.md) Phase 7 verification byte count table, [`../concurrency.md`](../concurrency.md) chapter 3, [ADR 0029](0029-decisions-before-phase-7.md)

## Context

`roadmap.md` Phase 7 compares A's send and receive byte counters with a capture. In the 2026-10-22 two-home field test
this comparison (test 6-5) could not be judged, and three things showed up.

- **The console `send` did not reach the requested count.** At 576 bytes it stopped after 110–133 datagrams and at 1452
  bytes after 45, when `sendto` returned `WSAEWOULDBLOCK` and the job ended. The bytes at the stopping point were about
  64KB at every size, so the socket send buffer is taken to have filled. 20 bytes (40KB for 1,000) all went out. So the
  count selection that grows 1,000 / 5,000 / 10,000 / 50,000 cannot run
- **The send counter exceeded the capture by multiples of the `HELLO` size (40 bytes).** All three runs that day (test 1,
  6-1–6-4, 6-5) did this, and receive matched exactly in all three. The 6-1–6-4 run, which contained the 64 MiB transfer,
  matched 49,786 received datagrams down to the byte. Both homes' routers used the same private range, so the peer's
  private candidate was inside the own subnet. This fits the hypothesis that the `HELLO` sent to that address never left
  because ARP failed, and test 1's capture has no packet sent to that address. ARP was not captured, so it is not confirmed
- **`netstat` receive errors rose from 0 to 1824, so the run was discarded by the rule.** That statistic is machine-wide.
  The run's receive counter matched the capture exactly. dumpcap loss was not recorded, so it cannot be concluded that this socket lost nothing

## Decision

The repository owner decided.

**1. The traffic compared is one 64 MiB file transfer.** The peer sends to the measuring side. The console `send` and
the count selection are not used.

**2. Measure on networks where the two PCs' private subnets do not overlap.** If they overlap, put one side on a
network in another range, such as a phone hotspot. The verdict rule is not changed.

**3. `netstat` receive errors are looked at only when the receive-side capture sum exceeds the counter.** If they rose,
discard and measure again; if not, it is a failure. When capture and counter match, they are not looked at.

**4. The capture tool's loss count is taken as an output file.** A person does not copy it by hand.

## Alternatives

**Alternative 1. The console `send` does not stop when the buffer fills and continues on the next loop turn.**
Rejected (for this check). The procedure and size conditions could stay. The cost is changing code, tests and the header
table and rebuilding the kit. This behavior of the console `send` is kept in `plan.md`, separate from this decision.

**Alternative 2. Enlarge the socket send buffer.**
Rejected. 1,000 datagrams of 1452 bytes alone need about 1.5MB. The growing count selection still does not work.

**Alternative 3. Change the send verdict to "capture ≤ counter and byte difference = 40 × datagram difference".**
Rejected. On the same networks a correct implementation would pass. The cost is a more complex rule that also hides
40-byte differences from other causes.

**Alternative 4. Make the send side reference only and judge only receive.**
Rejected. Simplest, but it cannot catch a send counting bug.

**Alternative 5. Confirm the hypothesis first (capture ARP too).**
Rejected. Even after confirming, alternative 3 or the network condition must be chosen. The network condition removes
the difference if the hypothesis is right. Its effect is checked in the rerun.

**Alternative 6. Keep the `netstat` rule, take it right before each run and close other apps.**
Rejected. It is machine-wide, so other apps can get the run discarded again. A socket buffer drop shows up anyway as the
capture exceeding the counter.

## Consequences

**Gains.**

- It can be measured again without changing client code
- Tens of thousands of datagrams are compared

**Costs.**

- The check at fixed sizes 20 / 576 / 1452 bytes is gone. TCP decides the datagram sizes of the transfer
- One side must use a hotspot. If the phone network's NAT is outside the supported conditions, no connection forms on it
- Why the send counter exceeds the capture on networks with overlapping private subnets stays unconfirmed. If the
  difference remains on the hotspot, the network condition alone is not enough, and this decision is reopened to find the cause
