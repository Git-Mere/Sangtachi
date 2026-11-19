# 0041. Loss accounting counts from the peer's connected signal

- Status: Accepted
- Date: 2026-11-19
- Related: [`../protocol.md`](../protocol.md) 4.5 Duplicate Suppression and Loss Accounting, [Issue 0009](../issue_history/0009-닿지-않는-후보로-보낸-hello-가-손실로-세졌다.md)

## Context

Loss accounting started when our session entered `CONNECTED`. The aim was to not count as loss the numbers of the `HELLO`s sent to several candidates during the punch phase.

But the punch phase ends at the sender's `CONNECTED`, not the receiver's. In issue 0009 the host entered `CONNECTED` first, and the player kept sending `HELLO` to both candidates after that. One number, sent to the home LAN candidate that the campus network cannot reach, stayed as loss at the host. The loss in the real-environment Phase 9 batches was 0 to 2 per session, and 0 on the same LAN, where the private candidate is also reachable.

## Decision

**Accounting starts when both conditions first hold: our session is `CONNECTED`, and we have received the peer's connected signal.**

| Item | Decision |
|------|----------|
| Connected signal | A `KEEPALIVE`, `DATA`, `PING`, or `ROSTER` that advanced `highest` in 8.3 duplicate suppression after our `got_ack` was set. These four are sent only while the sender is `CONNECTED` |
| Baseline | `position` immediately after processing the packet that set the later condition. Same as the current baseline rule |
| Renegotiation | 9.5 step 3 clears the connected-signal mark together with the loss accounting |
| Wire | Unchanged |

The sender stops `HELLO` retransmission as it enters `CONNECTED` and sends a `KEEPALIVE` right away. So no punch `HELLO` has a number after a connected signal.

Advancing after `got_ack` is required because of 9.5 Renegotiation. The renegotiating side keeps its epoch, nonce, and `sequence` and sends `HELLO` to all candidates again. Packets sent before the renegotiation that arrive late can enter the window of the new attempt as they are. An old `HELLO` can pin the epoch and an old signal can then advance `highest`, so advancing alone does not stop it. A `HELLO_ACK` echoing our new nonce comes only after the renegotiation, and the peer sends signals only with later numbers. So a signal that advances after `got_ack` is from after the renegotiation. If a signal arrives before the `HELLO_ACK`, accounting waits for the next signal.

## Alternatives

| Option | Content | Reason rejected |
|--------|---------|-----------------|
| A | Keep the accounting and report it as a limitation in `experiments.md` | The repository owner chose to fix the code and measure again |
| B | Make punch `HELLO`s not use `sequence` | 4.4's "one counter regardless of type" and 8.3 duplicate suppression also cover `HELLO`. Replay defense would have to be redesigned |
| C | Carry a "count from here" mark in the header or `KEEPALIVE` | Changes the wire format. Existing types already give the same information |
| D | Have the analysis tool subtract 1 or 2 losses per session | Removes the symptom, not the cause. It cannot be told apart from real loss |

## Consequences

**What we gain.** Even when our side is `CONNECTED` first, the peer's punch `HELLO`s do not stay as loss.

**Accepted limits.**

- If the connected signal is lost or arrives reordered, accounting starts late, at the next signal. If the first `KEEPALIVE` is lost, it starts at the `PING` 5 seconds later. Loss in between is not counted, so loss can come out slightly low
- If a 10.4 (c) validation `HELLO` goes to an unreachable address after `CONNECTED`, that number still stays as loss. It needs a packet from an address outside the candidate set
- The loss of the Phase 9 batches measured before this decision is from the old accounting. They are measured again
