# 0019. Measure Mapping Lifetime with a Separate Tool

- Status: Accepted
- Date: 2026-10-08
- Related: [`../roadmap.md`](../roadmap.md) Phase 4, [`../protocol.md`](../protocol.md) Chapters 7 and 11, [`../control_plane.md`](../control_plane.md) 1.1, [`../windows-prereq.md`](../windows-prereq.md) Section 2, [ADR 0002](0002-no-rebinding-recovery.md)

## Context

The `KEEPALIVE` 15 seconds and idle timeout 50 seconds in `protocol.md` chapter 11 are initial values to be
adjusted by measuring the mapping lifetime. The original plan was to leave the client idle and then have EC2
send a UDP probe to the client's public endpoint. The client has never sent UDP to EC2, so that probe is
unsolicited inbound, and the measurements in `windows-prereq.md` Section 2 and `protocol.md` 10.4 say such
inbound is all blocked. Zero arrivals cannot be split into "expired" and "filter closed". `roadmap.md`
Phase 4 asked for this to be redecided before starting.

## Decision

The repository owner chose the separate tool; the rest was decided within it.

**1. Measure with a separate tool in `tools/nat-lifetime`, not with the client.** The client side and the EC2
side are one file using only the standard library.

**2. The client sends to the responder first, and the responder answers once from that IP:port after the requested time.**
This is not unsolicited inbound.

**3. Before idling, requests and ACKs are exchanged at 1-second intervals for 6 seconds.** This makes a
bidirectional flow like a live tunnel.

**4. Each delay value gets its own socket, measured in parallel.** Each socket gets one late reply.

**5. The shortest delay is the control.** If it does not arrive, the verdict is undeterminable.

**6. The responder runs on EC2 only while measuring, and the security group is opened briefly to the measuring machine's `/32` only.**

**7. The `KEEPALIVE` interval keeps `interval × 3 < smallest measured lifetime`.** It tolerates two consecutive
losses. In the first measurement (one home Wi-Fi, `[58, 60)` seconds), 15 seconds gives 45 seconds and keeps it.
So 15 seconds and 50 seconds stay. The rule is owned by `protocol.md` chapter 11.

The procedure, wire format, and verdict table are owned by
[`tools/nat-lifetime/README.md`](../../../tools/nat-lifetime/README.md).

## Alternatives

**Alternative 1. Measure with the Sangtachi client's socket.**
Rejected. It would measure the real product path. The cost is that the tunnel is a Phase 4 deliverable so it
cannot run before starting, and a probe format would have to be newly defined to pass the `protocol.md`
chapter 7 classification. What is measured is the NAT, not our code.

**Alternative 2. Send once from one socket and receive replies at several times.**
Rejected. It finishes in one go. The cost is overestimating the lifetime on NATs where inbound packets refresh
the timer (allowed by RFC 4787 REQ-6).

**Alternative 3. No warm-up.**
Rejected. It is simpler. The cost is underestimating on NATs that give a longer timeout to flows that have seen
a reply. Linux conntrack defaults are 30 and 120 seconds.

**Alternative 4. Increase the delay step by step on one socket.**
Rejected. It keeps using one mapping. The cost is time equal to the sum of the delays.

**Alternative 5. Keep the interval below half the lifetime.**
Rejected. It is simple. The cost is tolerating only one consecutive loss. On a network whose lifetime is near
60 seconds, two consecutive losses break the path.

## Consequences

**Gained.**

- It can be measured before Phase 4 implementation
- The control shows "the path opens" in every run. Zero arrivals are not misjudged as zero lifetime

**Paid.**

- It is not the client's socket. Even on the same NAT, the host firewall state of our process is not measured
- The other end is a single EC2. On a NAT with per-destination timers, the peer-to-peer path may differ
- A person opens and closes the security group for each measurement
