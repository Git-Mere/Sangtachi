# Decisions before Phase 5 and the Chapter 15 implementation conformance list

## Why

Before starting Phase 5, a pass over the documents found six points that would get in the way. The repository owner picked an option
for each. Along with that, `protocol.md` Chapter 15 became a list without a Phase schedule, and the Phase 4 code was compared against it.

## What changed

| What | Where |
|------|------|
| `position` starts at `REPLAY_WINDOW`. The sentence that the acceptance window has no time-based expiry moved over from roadmap. Session totals across renegotiation segments | `protocol.md` 4.5, 9.5 step 3, `replay.cpp`, `replay_test.cpp` |
| No automatic retry | `protocol.md` 9.6, 10.4. Removed the open row from `control_plane.md` Chapter 10 |
| Chapter 15 became the implementation conformance list. Removed the checkboxes and Phase wording, and added 10.3 punch, 5.5 `PING`/`PONG`, 4.5 loss accounting, and fuzz defense as items | `protocol.md` Chapter 15 |
| The Phase 4 implementation list points at Chapter 15, and the items that overlapped Chapter 15 were removed. Only what this Phase decides is left | `roadmap.md` Phase 4 |
| Phase 5 prerequisite, tasks (loss accounting and fuzz defense, ASan build, C6-C9 harness modes), verification (ASan, where the long tests run, `session.loss`, the underflow mutation, the renegotiation total, no retry, the two-home observation of the 1-hour test) | `roadmap.md` Phase 5 |
| The fixed log event `session.loss`, its field formats, and when it is emitted | `architecture.md` Chapter 9 Log Output |
| Comments quoting the Chapter 15 name | `replay.hpp`, `session.cpp`, `natprobe.py` |
| The C6-C9 harness modes are a Phase 5 task | `tools/phase4-capture/README.md` |
| Decisions and rejected alternatives | ADR 0025 |
| Next work becomes Phase 5, two field-measurement waits added, the C6-C9 wait removed, the `pkt_pos` item removed, the own Wintun identifier and test-gap items added | `plan.md` |

## Important decisions

The decisions and the rejected alternatives are ADR 0025. This record holds only the other judgments.

- 10.3 punch and 5.5 `PING`/`PONG` went into Chapter 15 as new items. They are the two items removed from roadmap Phase 4 that Chapter 15
  did not have. They were moved so the obligation does not disappear with the removal
- The sentence "the window has no time-based expiry" in loss accounting moved from roadmap to `protocol.md` 4.5. It is a rule, so the
  specification owns it
- When presenting the options, the C6-C9 harness modes and the fuzz stage 2 injector were called "the same tool". That was not accurate.
  The harness fake peer is Python and sends on the wire; fuzz stage 2 is the receive injection entry point inside the client. roadmap
  lists them separately
- Three subagents split the Chapter 15 code comparison and did it read-only. Some key evidence was opened and checked directly (the
  `pending_pings` cap, the own Wintun identifier)

## Chapter 15 comparison result

The target is `client/` and `tests/` at HEAD `71d6b55`. All 32 items are in the code. The places where tests are missing moved to
`plan.md`. Of the three new items, 10.3 punch is covered by "session: punch start sends HELLO to every candidate and arms both timers" and
5.5 by "session: PING starts 5s after CONNECTED and PONG yields one RTT sample". The `position` start value was added by this change.

| Item (Chapter 15 order) | Verdict | Evidence summary |
|------|------|-----------|
| 1 Chapter 3 constants | Present | `protocol_constants.hpp`. No test pins each value; behavior tests read them |
| 2 Header serialization and round trip | Present | `packet.cpp`. "packet: header round-trips through serialize and deserialize" |
| 3 The four socket options | Present | `udp_socket.cpp`. `SO_EXCLUSIVEADDRUSE`, no `SO_REUSEADDR`, and no `connect()` have no test (absence of a call) |
| 4 bind, `getsockname`, `SO_RCVBUF` | Present | "udp: open binds to any address on an ephemeral port" and others |
| 5 Socket ownership by the single receive loop | Present | Enforced by structure: no copy, a move. `recv_from` is called in one place |
| 6 Source hygiene and classification | Present | `loop.cpp`. "classify: the source hygiene table decides before the class" and others |
| 7 The 8 common checks of 8.1 | Present | "packet: check_common case table" and others |
| 8 8.2 epoch/source rules | Present | "session: HELLO_ACK checks virtual_ip, then nonce, then epoch" and others |
| 9 `virtual_ip` check before the epoch decision | Present | "session: a HELLO with the wrong virtual_ip is dropped before it can pin" |
| 10 4.5 duplicate suppression, before side effects | Present | `replay.cpp`, "replay: matches a set-based reference model" |
| 11 The four 4.5 boundaries | Present | A replay test per boundary |
| 12 `a != b` in `is_newer` | Present | "replay: is_newer is false for equal values" |
| 13 `session_epoch` and the retired list | Present | "epoch: retired list holds at most MAX_RETIRED_EPOCHS and evicts the oldest" and others. No test that a new attempt uses a different epoch |
| 14 Attempt nonce and probe nonce | Present | "nonce: a retired nonce is not reused" and others |
| 15 10.1 candidate decision and the three case tables | Present | Every row of the three tables. The product path still passes an empty own Wintun identifier (the adapter is Phase 6) |
| 16 10.2 rendezvous | Present | "lobby: a ready player reply opens one session with the delay of 10.2" |
| 17 10.4 endpoint learning | Present | "session: four probe paths at most, the fifth packet is still processed" and others |
| 18 9.2 dual flags | Present | "session: sent_ack is not set when sending HELLO_ACK fails" and others |
| 19 9.4 transition table and 9.4.1 | Present | Tests are spread per row. No per-cell case table and no test of receiving in `FAILED` |
| 20 9.4 unverified-destination budget | Present | "session: the unverified budget counts tokens as the roadmap does" |
| 21 9.5 ten renegotiation steps | Present | "session: renegotiation from CONNECTED follows the ten steps". The loss accounting reset of step 3 comes with the accounting in Phase 5 |
| 22 The two 9.5 renegotiation caps | Present | "session: the ninth renegotiation is drop_reneg_limit" and others |
| 23 Chapter 11 timers | Present | Session, STUN, and lobby timer tests |
| 24 `CLOSE` | Present | Clearing the routing slot on the receiving side takes the same path as the sending side and has no direct test |
| 25 The three 9.6 failure-transition steps | Present | "session: idle timeout fails CONNECTED as TUNNEL_DROPPED and sends CLOSE 0x03" and others |
| 26 `tx_err_send`, `tx_err_ack` | Present | "session: a failed send of any type counts tx_err_send in the session table" and others |
| 27 `pending_pings` cap and cleanup | Present | No test reaches the cap branch. The PING interval and the entry lifetime are both 5 seconds, so the current configuration cannot reach it |
| 28 Chapter 13 STUN scope | Present | "stun: parse_binding_response case table" and others |
| 29 `DATA`, 8.4, 8.5 transmit side | Present | "tunnel: inner IPv4 case table", "tunnel: 8.5 checks run in order and count in the right table" and others |
| 30 8.5 routing table | Present | "tunnel: the 8.5 routing case table". Row 4 is covered only by the same input as row 2 |
| 31 `ROSTER` receive checks and the four counters | Present | "packet: decode_roster case table" and others |
| 32 The three `ROSTER` send moments | Present | "roster: a change is resent twice 5s apart, then every 15s" and others |

## Verification

| What | Result |
|------|------|
| `pwsh -NoProfile -File scripts/test.ps1 -Filter replay` | 25/25 passed |
| Mutation: put `position` and the first `pkt_pos` back to 0 | 14 replay tests failed. Caught. Reverted |
| `pwsh -NoProfile -File scripts/test.ps1` (with DynamoDB local) | `all tests passed` |
| `docgate.py`, `--claims` | `VERDICT: pass`. The strong-claim wording of added lines was scanned separately |

## Cross-model review

Codex. Three lenses were run separately, and the repaired places were run again under the same lens.

| Lens | Finding | Handling |
|------|------|------|
| Correctness and contract | warn. The accounting values outside a segment (before the first `CONNECTED`, after a renegotiation until the next `CONNECTED`) were undefined | Accepted. 4.5 now says the session totals start at 0, renegotiation step 3 adds only when inside a segment, and the current values outside a segment are 0 (Korean and English) |
| Correctness and contract | warn. `lost / expected` divides by zero when `expected` is 0, contradicting the zero-denominator rule of 4.5 | Accepted. `architecture.md` Chapter 9 says it is 0 when `expected` is 0 (Korean and English) |
| Correctness and contract (round 2) | warn. Whether the last segment's values survive the end of the session depends on how it is read | Accepted. It now says the last segment's values stay when the session ends (Korean and English). Round 3 `LGTM - no blockers` |
| Obligations and stale references | warn. The `control_plane.md` 1.1 table still said "who retries and when is undecided" (Korean and English) | Accepted. It now points at 9.6. The first search looked only for the phrase "automatic retry" and missed it. A new search for "undecided" together with "retry" finds nothing left in living documents |
| Obligations and stale references | warn. `plan.md` says only field measurements are left in Phase 4, but C6-C9 are left too | Accepted |
| Korean/English mirror | `LGTM - no blockers` | - |
