# Design Audit 3 Follow-up — 62 Findings Handled

**Target commit:** this commit
**Basis:** `audit-history/design-audit3.md` (blocker 4 / warn 31 / info 27)

Audit 3 only investigated and came out without fixing the documents. This commit handles those 62
findings. **3 were left unhandled for the owner to judge** and are written in `plan.md`.

## What Was Fixed

The work was split into six bundles and fixed in document order. The protocol body was fixed first,
and the documents that cite it after.

| Bundle | Document | Findings covered |
|:----:|------|-----------|
| 1 | `protocol.md` | B-3, W-1~W-6, the protocol side of W-7 and W-9, info 1~5, 23 |
| 2 | `architecture.md` | B-2, the architecture side of W-9, W-17, W-18, W-19, W-28, info 6~9, 14, 25 (chapter 10), part of 26 |
| 3 | `control_plane.md` | **B-1**, W-8, W-10~W-16, info 11~13, 15~22 |
| 4 | `roadmap.md` | B-4, the roadmap side of W-20~W-27 and W-30, info 24, 26 |
| 5 | `windows-prereq.md` | W-29, W-30, W-31, info 27 |
| 6 | `spec.md` | info 25 |

**1 more.** A consistency finding that was dropped in the audit report merge was fixed together.
The basis for the `CONTROL_PLANE_EXCHANGE_FAILED` verdict in `architecture.md` chapter 8 listed
different conditions from `protocol.md` 9.6 (the `peer_id` collision was missing). Ownership of the
condition list was moved to 9.6 and this table now points at it.

## Decisions on the 4 Blockers

### B-1. An order where readiness is never recorded (`control_plane.md` 4.4, 6.3)

Of two options, **re-read after write** was chosen.

| Option | What | Adopted |
|----|------|:----:|
| A | After the `PEER#` update, read `Query(pk, ConsistentRead)` again and decide | **Yes** |
| B | Put a `size(candidates) > 0` ConditionCheck on the other `PEER#` and bundle it with `TransactWriteItems` | No |

B was dropped because the path splits in two. The condition always fails for the first registrant,
so it must take a cancellation and then go back through a plain update, and that adds one more
"path where a condition failure is normal". A spends one more read and is done. **The correctness
argument was written into the document.** Each side's re-read comes after its own write, so the
side that reads later also sees the other side's write. Therefore at least one side sees both.

The race case table was put in 4.4, and **the last row is a mutation test** (an implementation with
the re-read removed is caught by a count of 0 records). That order cannot be forced from outside, so
chapter 9 "what can only be decided by reading the code" records that the store layer needs a delay
injection point right after the write, and the Phase 3 verification in `roadmap.md` was fixed with
it.

### B-2. When `--stun` has only one entry (`architecture.md` 3.5)

The same section stated both "WARN and proceed" and "fail even with only one response".
**It was unified to fail.** The M-2 verdict and the "supported network conditions" both require two
different servers, so proceeding with one enters punching without being able to tell
destination-dependent mapping apart. The warning is kept but moved to **startup time**. Telling the
user the input is wrong is better than waiting 5 seconds and then seeing a failure.

### B-3. The result of one forged `HELLO` (`protocol.md` chapter 2)

"It is a reachable result, not a settled one", added while fixing B-6 of audit 2, was an
understatement. Check 8 of 8.1 comes before 8.2 and 9.3, so **a `HELLO` from a peer carrying an
epoch that is in the discard list cannot trigger renegotiation.** The peer draws a new epoch only
after the 50-second idle timeout, and our punch deadline is 10 seconds. No recovery path fits inside
that. **The result was written with codes and times.** `PEER_HANDSHAKE_FAILED` at 10 seconds on our
side, `TUNNEL_DROPPED` at 50 seconds on the peer side.

**The design was not changed.** There is an option to defer the discard registration until after a
valid `HELLO_ACK` for the new epoch, but that changes the meaning of renegotiation and is outside
the scope of this commit. Only the description was made to match the facts.

### B-4. The shot count of the unverified budget test (`roadmap.md` Phase 4)

"After 10 shots in a row the 11th is blocked" is a value that leaves out the probe token. The first
`HELLO` from a **new** source outside the candidate set spends **two tokens**, one for `HELLO_ACK`
and one for the verification `HELLO` of 10.4 (c). The two conditions were written separately. With a
new source, shots go out through the 9th and the 10th is blocked; with a provisional path already
present, shots go out through the 10th and the 11th is blocked. A condition was also attached to the
(c) 30-second test in the same item. If the injector does not answer the probe, the path is
re-registered every 5 seconds and tokens keep leaking, so the longer the test runs the more a correct
implementation fails.

## Where the Design Changed

Only **contract changes** are written here, not wording fixes.

| What | Before | After |
|------|-----|-----|
| Readiness verdict (`control_plane.md` 4.4) | Decided from the read taken before the update | **Decided from a re-read after the update.** Storage reads went up by one per request |
| Renegotiation (`protocol.md` 9.5) | 9 steps | **10 steps.** Restarting `HELLO` retransmission and stopping `KEEPALIVE`, `PING` and idle is step 10 |
| `FAILED` transition (`protocol.md` 9.6) | Record the code only | **Stop timers + `CLOSE` `0x03` to the learned session + record.** The process exits after that (`architecture.md` 3.2.7) |
| `CLOSE` reason (`protocol.md` 5.6) | `0x00`~`0x02` | **`0x03` establishment failure was added.** It is a round 1 review finding. See below |
| `CLOSE` receipt (`protocol.md` 5.6) | Remove the session and the routing entry at once | **Remove routing only and keep the session as `CLOSED`.** `drop_terminal_state` in 9.4.1 needs the session to exist |
| `virtual_ip` check (`protocol.md` 8.2) | Only in 5.1 and not in the pipeline | **Before the epoch verdict of 8.2.** It was also added to the chapter 15 checklist |
| Discard list (`protocol.md` 4.3) | "epochs of earlier attempts" | **The peer's epochs only.** Our own values match no received packet, so they filtered nothing |
| STUN deadline (`protocol.md` chapter 11) | Unit unclear | **Per server.** The upper bound for the whole stage is `5s × ceil(list/2)` |
| Start of `get_peers` polling and deadline (`protocol.md` chapter 11) | Entering `IDLE` | **A successful `register_candidate` response.** At the moment of entering `IDLE` there is not even a `peer_id`, so it could not be called |
| Rate limit refill (`control_plane.md` 6.4) | "10 per minute" | **Continuous, 1 per 6 seconds.** Rules for creating and removing table entries were added too |
| Received `DATA` in a build without an adapter (`architecture.md` 3.2.3) | Undefined | **Test builds use a payload hook, product builds discard and count `drop_no_sink`** |
| Adapter ownership verdict (`windows-prereq.md` section 3) | Name + `InterfaceDescription` | **The recorded `InterfaceGuid`.** Same criterion as section 7. Getting it wrong deletes somebody else's adapter, so a stronger criterion was needed |
| Subnet conflict action (`windows-prereq.md` section 7) | "pick an alternate range or stop" | **Stop.** An alternate range does not hold up, because the control plane has no operation that carries that value. Whether to add one is a before-Phase-6 item |

## Newly Defined

| Name | Owning document | What |
|------|-----------|------|
| `tx_err_send` | `protocol.md` chapter 6 | `sendto` failure for every type. `HELLO_ACK` also counts `tx_err_ack` |
| `drop_no_sink` | `architecture.md` 3.2.3 | `DATA` that passed 8.4 in a product build with no adapter |
| `adapter_rx` | `architecture.md` 3.2.3 | A successful Wintun read |
| `timer.tick` | `architecture.md` chapter 9 | The 200ms timer of test builds. Fields `name`, `elapsed_ms` |
| `rx.raw` | `architecture.md` chapter 9 | Phase 1 raw send and receive. Fields `from`, `len`, `sha256` |
| `--peer`, `raw <byte count>` | `architecture.md` 3.5 | Test input and console command for Phase 1~2 |
| `send <n> [size]` | `roadmap.md` Phase 4 | A size argument was added. The Phase 5 boundary table uses it |

**Fixed log event keys went from 8 to 10.** Verification now reads values in more places, and both
additions are for test builds only.

## Not Handled

| What | Why |
|------|-----|
| `2026년 가을` in the `roadmap.md` header | Whether it hits the no-dates rule for living documents, or is course metadata and outside that intent, is rule interpretation |
| A living document linking an ADR | Same reason. Setting the scope of a rule is a rule change |
| Moving `first_design.md` into `decisions/` | It is an uncommitted working tree change, and moving it breaks the front-matter links of three living documents. **It was not put in this commit.** The working tree still holds the deletion of `docs/kor/first_design.md` and the new `docs/kor/decisions/first_design.md` |

## Verification

- `python tools/docgate/docgate.py` — apart from `mirror`, `parity` and the `link` findings created
  by the `first_design.md` move, no new findings appeared. **The English mirror is made when there
  is an instruction to push.**
- The handling check was run by script. It checked as strings whether the fix wording for blocker 4
  + warn 31 + the 1 extra is in the target documents, and whether the fix wording for the 27 info
  items is there. **Counts were not done by hand.**
- Self-contradiction `grep`. The descriptions whose strength was changed while fixing (`IDLE 진입`,
  `어휘는 셋`, `여덟 개`, `수십만`, `참가자가 부른다`, `즉시 세션과 라우팅`,
  `대체 대역을 고르거나`, `확정된 결과가 아니라`) were searched exhaustively across the 6
  documents and confirmed at 0 remaining.
- The new names (`tx_err_send`, `drop_no_sink`, `adapter_rx`, `timer.tick`, `rx.raw`, `probe200`,
  `--peer`) were checked for **being defined in the owning document and used in the citing
  documents**.

## Cross-model Review

3 rounds were run with Codex, changing the lens each time. **Input was cut per lens.** Rounds 1 and
2 got only the documents that lens actually looks at, and only the final round got the whole diff,
`plan.md` and this record.

| Round | Lens | Input | Result |
|:--:|------|------|------|
| 1 | Scenario trace | `protocol.md` + `architecture.md` (58KB) | warn 1 |
| 2 | Races and verification criteria | `control_plane.md` + `roadmap.md` (96KB) | warn 1 |
| 3 | Final (contradictions, record cross-check) | Everything (275KB) | **blocker 1**, warn 4 |
| 4 | Repair check | `plan.md` + this record (incremental lens) | nit 2 |

**Handling per finding. None rejected.**

| # | Finding | Handling |
|:--:|------|------|
| 1-1 | The `CLOSE` sent by 9.6 is `0x01` (user cancel), so in the peer's record a person stopping it and a handshake failure become the same value | **Applied.** Reason `0x03` (establishment failure) was added. The 5.6 wire table, value table and rationale, 9.6, chapter 15 and the Phase 4 verification in `roadmap.md` were fixed together |
| 2-1 | The 60 seconds of rate limit case 14 sits exactly on the bound, so the outcome depends on when it is measured | **Applied.** Changed to 70 seconds and the reason was written. It was also nailed down that the refill reference time **is not updated by consumption or by a `rate_limited` response** |
| 3-1 | **blocker.** The chapter 15 checklist still says `CLOSE 0x01`, which conflicts with the body | **Applied.** Fixed to `0x03`. It is the place the checklist was missed while fixing 1-1 |
| 3-2 | The step count in 3.2.7 was raised to 8 steps, but "already finished at (5)" in the sentence after it is the old number | **Applied.** Fixed to (7) |
| 3-3 | The record says the `first_design.md` move was not handled, but the diff contains the deletion | **Applied.** Removed from staging. That move is not in this commit |
| 3-4 | `plan.md` counts 3 held items but writes "both are rule interpretation" | **Applied.** Changed to three and wrote the nature of the third |
| 3-5 | `plan.md` bundle 2 says it covers info 15, but the actual fix is on the `control_plane.md` side | **Applied.** Moved to bundle 3. The table in this record was fixed with it |

**3-1 is a defect created while fixing 1-1.** The body was fixed and the checklist in the same
document was not. "If you changed a definition, make a list of the sentences that relied on that
definition and read them again one by one" in `CLAUDE.md` is this spot. After the round 1 fix,
`0x01` should have been searched exhaustively.

| 4-1 | `W-26` is listed both in a range and separately in the bundle 4 list of `plan.md` and this record | **Applied.** Only the range notation was kept |

**3-3 is a staging accident.** `git add -u docs/kor` pulled in a file move the owner had in progress
in the working tree. If the final round had not caught it, someone else's change would nearly have
gone into my commit. **Stage with explicit paths.** Do not lump things together with `-u`.
