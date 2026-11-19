# 0009. A HELLO sent to an unreachable candidate was counted as loss

- Status: Resolved
- Found: 2026-11-18
- Related: [`../protocol.md`](../protocol.md) 4.5 Duplicate suppression and loss accounting, [`../commit_history/2026-11-18/04-phase-9-오늘-세-묶음-분석.md`](../commit_history/2026-11-18/04-phase-9-오늘-세-묶음-분석.md)

## Symptom

The Phase 9 analysis tool's loss was 0/350 for the same LAN and exactly 8 for each of the other three real environments (home network vs hotspot, campus network vs home network, home network vs home network). Per session, the loss was only 0, 1, or 2. The expected count in one direction of a 30-second session is 15 to 21.

## Investigation

| Suspected | Observed | Verdict |
|-----------|-------|------|
| Random loss on the network | Almost every session with loss lost exactly 1, and all 20 sessions of the same LAN batch had 0 | Hard to read it that way |
| Accounting counts the numbers of packets sent to unreachable candidates | The campus network vs home network setup (`mere-galaxy` host, this PC player) was run again with captures on both sides. On the second attempt the host had `expected=18 accepted=17 lost=1`. Table below | Yes |

The tunnel packets the player sent in that session:

| `sequence` | Type | Destination | Did the host receive it |
|-----------|------|--------|-----------------|
| 0 | `HELLO` | Host public candidate | No. It is the first punch packet |
| 1 | `HELLO` | Host private candidate (home LAN) | No. That address is unreachable from the school network |
| 2 | `HELLO_ACK` | Public candidate | Yes |
| 3 | `HELLO` | Public candidate | Yes |
| 4 | `HELLO` | Private candidate | No |
| 5-21 | `HELLO_ACK`, `KEEPALIVE`, `PING`, `PONG`, `CLOSE` | Public candidate | All yes |

The host's `expected=18` is the value when accounting starts at 3 and ends at 21. The only number missing in between is 4.

## Cause

**Confirmed.** In the reproduced session, the loss of 1 is the `HELLO` (number 4) the player sent to the host's private candidate. `sequence` also spends numbers on the `HELLO` sent to each candidate. The host started accounting on receiving number 3, while the player was still punching and sent one more `HELLO` to both candidates. Number 4, sent to the private candidate, could never arrive. This is the same contamination that made `protocol.md` 4.5 start accounting at `CONNECTED`, happening once more just after that boundary.

**Inferred.** The other losses in the batches are taken to have the same cause. Loss was 0 to 2 per session, and 0 on the same LAN, where the private candidate is also reachable. The batch sessions were not captured.

## Handling

Loss accounting was changed to count from the peer's connected signal ([ADR 0041](../decisions/0041-loss-accounting-starts-at-peer-connected-signal.md), [`../protocol.md`](../protocol.md) 4.5). The record is [`../commit_history/2026-11-19/01-손실-집계를-상대의-연결-신호부터.md`](../commit_history/2026-11-19/01-손실-집계를-상대의-연결-신호부터.md). The captures are in `field-test/2026-11-18/lossdbg/`, ignored by git.

## Result

The order of the reproduced session (we are `CONNECTED` first, then the peer sends `HELLO` to an unreachable candidate) was turned into a unit test. With the fixed accounting that number does not stay as loss, and reverting to the old accounting makes the test fail. The real-environment batches are measured again. That schedule belongs to [`plan.md`](../../kor/plan.md) (Korean only).
