# Phase 4 Round 3 G: Out-of-Candidate Path Validation and the Unverified-Destination Common Budget

## Why

This is the first slice of Phase 4 round 3. Round 2 left out-of-candidate sources unlearned and replies sent without a budget.
This change puts `protocol.md` 10.4 (c) path validation and the 9.4 unverified-destination common budget in that place.

## What changed

| What | Where |
|------|-------|
| 10.4 (c) steps 1-7. Tentative path table (`MAX_PROBE_PATHS`), probe nonce, one validation `HELLO`, 5-second slot, `drop_probe_full`, `path_probe_failed` | `client/src/peer/session.cpp`, `session.hpp` |
| The 8.2 probe nonce echo row. Checked before the attempt nonce rows | Same |
| The 9.4 common budget. Capacity 10, refill 5 per second. `HELLO_ACK`, `PONG`, and validation `HELLO` share one bucket. So does the 9.5 step 6 `HELLO_ACK` | Same |
| A function that draws a probe nonce. The tunnel passes its random source to the session | `peer/epoch`, `tunnel/tunnel.cpp` |
| Counter `path_probe_failed` | `counters.*` |
| Three points the documents did not define (below) | `protocol.md` 8.2, 10.4. English mirror |
| Removed G | `plan.md` |

## Key decisions

**The three document gaps were fixed in the documents first.** What was written is the result of reading the existing rules together.

| Gap | Decision | Basis |
|-----|----------|-------|
| The epoch of a probe nonce echo | If it differs from the pinned value, `drop_stale_epoch`. A validation `HELLO` is sent only after the epoch is pinned, so the unpinned case does not occur | Same reason as the 8.2 attempt nonce echo row. Evidence from another attempt does not change this attempt's `peer_endpoint` |
| Must the echo satisfy (b) to switch | Yes. An echo accepted through reordering does not switch. The address is not remembered separately | The learning definition at the top of 10.4 requires all of (a)(b)(c) |
| What `path_probe_failed` counts | Incremented only at step 6. Paths whose echo arrived, and paths whose session ended first, are not counted | 10.4 step 6 |

**Small points that do not conflict with the document rules were decided in code.** They are written with their basis in the "decided by this code" table in `session.hpp`.

- The triggers for registering a tentative path are a packet that satisfied (a)(b) but not (c), and a `HELLO` that caused renegotiation. `CLOSE` does not register
- An address that is already a tentative path is not registered again. This matches the token count in `roadmap.md` ("from the second shot on, one token")
- The reply spends its token before the registration. This is the order of the 9.4 `HELLO` row
- The bucket refills 5/1000 of a token every 1ms. It starts full
- The expiry of a tentative path slot is part of `next_deadline` but does not emit `timer.tick` (Chapter 11 expiry table)
- Renegotiation does not reset the tentative path table or the budget (not in the 9.5 step 3 list)
- If a probe nonce cannot be drawn, nothing is registered and no token is spent

## Verification

| What | Result |
|------|--------|
| Full test run (`scripts/test.ps1`) | 510 unit tests passed, 0 cli/e2e/lobby check failures, `all tests passed`. Run on DynamoDB local |
| New tests | 14 session, 1 tunnel. They check the `roadmap.md` Phase 4 tentative path cap and common budget (a)-(e) through the send record. Capture checks are round 4 K |
| Existing tests changed | Three. One more validation `HELLO` goes to an out-of-candidate source |
| Mutation (clean copy, fresh modification times) | All 25 killed. The first three did not build because `/W4 /WX` raised a constant condition and an unused parameter to errors. They were rewritten to avoid those warnings and rerun |
| `docgate.py` | `VERDICT: pass` |

## Cross-model review

Codex ran with two separate lenses (contract and correctness; test adequacy and records).

| Finding | Resolution |
|---------|------------|
| warn. If the clock goes backward, the front of the table is not the earliest deadline | Rejected. The timer clock is monotonic (`concurrency.md` Chapter 4). The other session timers rely on the same premise |
| warn. `registered + 5s` overflows at the end of `Millis` | Rejected. It is a 64-bit millisecond monotonic clock. Same arithmetic as `now + interval` in the other timers |
| warn. No test checks that an out-of-candidate `DATA` without a hook registers | Accepted. Test |
| warn. No test for a second echo of the same nonce | Accepted. An advancing echo after a reordered echo switches |
| warn. No test that tentative paths survive renegotiation | Accepted. An echo under the new epoch switches, and the slot lasts 5 seconds from the original time |
| warn. No-eviction is checked only through the send record | Accepted. Checks that the oldest slot's echo is accepted |
| warn. No test for a failed validation `HELLO` send | Accepted. It spends the token, keeps the slot, and is not retransmitted |
| warn. No test that an out-of-candidate `CLOSE` sends no validation `HELLO` | Accepted |
| warn. No test that dropped out-of-candidate packets do not register | Accepted. Duplicate, unmatched `PONG`, 8.4 failure |
| warn. No test that slot expiry does not emit `timer.tick` | Accepted |
| warn. No test of the order when a probe nonce equals the attempt nonce | Rejected. Two 128-bit random values are equal with probability 2^-128. A test would have to rig the random source for that purpose |
| nit. The header comment "the only side effect before 8.3" conflicts with probe expiry | Accepted. Wrote that expiry is an item lifetime, not a side effect of the packet |

The final confirmation review (whole staged diff including records) was `LGTM - no blockers`.
