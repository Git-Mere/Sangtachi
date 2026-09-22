# Design Audit 2 Follow-up 1 — Items Unrelated to the Control Plane (2026-09-21)

**Target:** `CLAUDE.md` at the repository root, and under `docs/kor/`: `spec.md`, `roadmap.md`,
`architecture.md`, `protocol.md`, `plan.md`, `audit-history/design-audit2.md` (new),
`commit_history/2026-09-21-design-audit2-followup1.md` (this file, new)
**Only Korean was changed.** The `docs/eng` mirror is made when the push instruction comes. Until
then it is normal for the gate to fail on `mirror` and `parity`.

## Why This Commit Exists

Before Phase 1 implementation started, the five core documents were fully audited
(`audit-history/design-audit2.md`, blocker 6 / warn 24 / info 24). This commit handles **only the
items unrelated to the control plane**. The control plane bundle (B-2, B-3, B-4, B-5, W-6, W-11,
W-12, 4 info items) needs a separate schema document, so it was left as the second round in
`plan.md`. The repository owner instructed that B-2 and B-4 move to the second round.

## Audit Method

Five independent reviews with five lenses ran in parallel: scenario trace, consistency
cross-check, implementer's view, verification criteria, adversarial input. Each lens's findings of
the "missing" kind were cross-checked directly against the HEAD text at integration time.

**What one lens missed, another lens caught.** There were two places three lenses pointed at
independently (the missing 9.4 transitions, control plane thread ownership), and those two were
the biggest holes.

## What Changed

### protocol.md

| Section | Change |
|----|------|
| 2 | Corrected the premise of forged `HELLO` renegotiation to `peer_id` alone. `session_epoch` is not needed. The consequence is also written as "reachable", not "certain" |
| 3 | Four upper-bound constants added: `MAX_PROBE_PATHS`, `MAX_RETIRED_EPOCHS`, `MAX_RENEGOTIATIONS`, `MIN_RENEG_INTERVAL_MS` |
| 4.3 | The epoch is drawn from the OS CSPRNG and redrawn if 0. The retired list bound and **that the 2-minute retention is best-effort, not a guarantee** are stated |
| 4.4 / 4.5 | First packet `sequence = 0` fixed. Stated that `REPLAY_WINDOW` and the bitmap width move together |
| 5.6 | Fixed the target sessions of `CLOSE` (by learned-or-not) and the send condition per reason (by state) |
| 6 | bind contract (`INADDR_ANY` + port 0, `getsockname`), `SO_RCVBUF` setting and confirming the applied value, `SO_SNDBUF` default |
| 7 | **New source hygiene check** (before classification, `drop_bad_source`). Decided by a case table, with the reason loopback handling differs from 10.1 |
| 8.2 | Defined the handling of a valid-nonce `HELLO_ACK` whose epoch differs from the pinned value (discard) |
| 8.5 | Length lower-bound check added. Premise generalized from "a packet read from Wintun" to **"an inner packet regardless of origin"** |
| 9.1 / 9.4 / 9.4.1 | Widened the `PUNCHING` definition to remove the unnamed punch-wait interval. Reception handling for `IDLE`, `FAILED`, `CLOSED` newly added as 9.4.1 |
| 9.5 | Two renegotiation bounds, expanded to 9 steps, `HELLO` send target fixed. **The state is not pinned but derived from the 9.2 flags** |
| 10.1 | Obligation to keep the interface prefix (input to the chapter 7 verdict) |
| 10.4 | Tentative path bound and one verification `HELLO`. **Process restart recovery not guaranteed** and the different failure codes on the two sides stated in a table |
| 10.5 | Separated "block" (bulk redirection) from "limit" (probe reflection) |

### architecture.md

Updated the 5.1 epoch meaning to per session attempt. Unified the chapter 8 failure code basis on
the 9.6 flags and aligned the 5.3 state diagram. Added a **log output contract** to chapter 9 and
stated its distinction from the local record file. Added Wintun injection failure handling and
oversized datagram handling to 3.2.3, the sizes of the two queues to 3.2.6, and the `[console]`
termination rule to 3.2.7.

### roadmap.md / spec.md

Corrected the expected counter of the Phase 4 verification to `drop_retired_epoch`. Lowered the
Phase 2 public IP match criterion to reference and changed the verdict to parsing success. Made a
place for the M-2 verdict in Phase 4. Fixed the RTT baseline as a `DATA` echo. Moved NFR-2 to
Phase 7 with the file transfer test as its verdict. Replaced the requirement lists in the Phase
headers with a reference to the traceability table. Pinned the `send <n>` synthetic packet with an
absolute offset table.

**Every new contract got a verification item with it.** Source hygiene, discard in terminal
states, renegotiation bound, tentative path bound, `CLOSE` condition, bind, `SO_RCVBUF`, oversized
datagram, retired list bound, epoch random source, console queue, ring size, Wintun injection. A
sentence asking for mutation confirmation was written with them.

## Decisions

| What | Value chosen | Reason |
|------|---------|------|
| Receive buffer | `MAX_DATAGRAM + 1` (1473) | The oversized packet verdict comes from a **length comparison**, not from Winsock's truncation behaviour. It reproduces deterministically in tests |
| When tentative paths are saturated | Reject new paths and **protect the earlier ones** | With eviction, whoever sends forged sources fastest can evict the real path |
| `drop_probe_full` | Skip only path registration and **process the packet normally** | If dropped, the idle timer is not refreshed and a live session is judged as dropped |
| `CLOSE` target | Not the state but **whether `peer_endpoint` was learned** | We can be in `HANDSHAKING` while the peer is in `CONNECTED`. Sending only in `CONNECTED` makes the peer wait 50 seconds |
| Wintun injection counter | **Only** `drop_inject_error` fixed | There was no means to confirm in which call and in what form the failure is reported. Writing an unconfirmed API shape makes the implementation trust it |
| Renegotiation cooldown basis | Time of the **accepted** renegotiation | If a discarded one refreshed the time, an attacker could extend the cooldown forever and block even legitimate retries |

## Not Applied

As `protocol.md` chapter 2 writes, **the structure where one forged `HELLO` can end a live session
stays as it is.** The document was only fixed to state that fact precisely. Blocking it requires
changing the renegotiation design, and that is outside this scope. It is handled as a separate
decision.

## Verification

`python tools/docgate/docgate.py` reports only `mirror` and `parity`. Normal, since only Korean
was changed. There are no broken links and no structural problems inside the Korean documents.

## Cross-Model Review (Codex, parallel per lens)

| Round | Lenses | blocker | warn | nit | LGTM |
|:--:|:--:|:--:|:--:|:--:|:--:|
| 1 | 4 | 2 | 17 | 0 | 0 |
| 2 | 4 | 0 | 19 | 3 | 0 |
| 3 | 4 | 2 | 6 | 0 | 1 |
| 4 | 3 | 2 | 8 | 1 | 0 |

**All findings were applied. One was rejected.** In round 3 a finding said "the roadmap does not
say when the local record file format is decided", but it was already there as a before-Phase-4
item. A case of the reviewer not seeing outside the diff.

The major findings and their handling are as follows.

| Round | Finding | Handling |
|:--:|------|------|
| 1 | Reflection risk: retransmitting every 200ms to the renegotiation source sends dozens of shots to a forged address | Changed to handle it as a tentative path (one send, bound 4). **A defect I created while fixing** |
| 1 | New contracts have no verification (5 items) | Added verification items to Phase 1/4/7/9 |
| 2 | There is no contract that 10.1 collects the netmask, yet chapter 7 presumes it | Added the prefix retention obligation to 10.1. **My repair depended on an input that did not exist** |
| 2 | `SO_SNDTIMEO` 1.5s + `SO_RCVTIMEO` 1.5s = 3s worst case exceeds the 2s join bound | Corrected: what keeps the bound is `_exit`, not the timeouts. **An arithmetic error in the existing document** |
| 2 | 7 overstatements (`[console]` "there is no means", "cannot be made on Windows", M-1 "behind the same NAT", etc.) | All softened. 2 overstatements in the audit record itself were fixed too |
| 3 | `WintunSendPacket` may not report failure, yet that was presumed | Did not assert the API shape; fixed only the behaviour contract. **A defect I created in the round 2 repair** |
| 3 | 9.5 sends `HELLO_ACK` but does not say whether `sent_ack` is raised, so the same packet points at two states | Raised in step 6, and step 7 derives the state |
| 4 | 10.4 still writes "renegotiation returns to `PUNCHING`" | Removed the state name. **I did not follow the ripple of the round 3 repair** |
| 4 | Punch retransmission reads as state-based and could stop in `HANDSHAKING` | Chapter 11 was already `CONNECTED`-based. Only the prose was made explicit |

**Round 5 could not run because of the usage limit.** The marker was not stamped and the commit
gate was not bypassed. The remaining procedure is in `plan.md`.

## What Was Learned This Time

**Repairs created new blockers, by three mechanisms.** Obtained by classifying the 6 blockers.

1. **While fixing, I rewrote the same rule in a second place, and later fixed only one.** Round 4
   blocker 1 is that. What I grepped was the **old wording**; **the text I had newly written** was
   not fully checked.
2. **To fill a gap I asserted an external fact I could not confirm.** The Wintun API. The pressure
   to answer a finding led to an unfounded assertion.
3. **I changed a definition and did not reread the other sentences that relied on it.**

**On the other hand, areas where the same lens ran again became clean.** The numeric lens gave 4
findings in round 2 and `LGTM` in round 3. Half the reason rounds keep producing findings is
**that the lens changed every time**, and the findings from the two lenses first added in round 4
were original gaps unrelated to my repairs.

**Lens diversity is confirmed by measurement.** Of 11 findings in round 4, only 1 place was
pointed at by two lenses, and the 2 blockers were found by one lens each. Whether that is **the
effect of splitting the calls** has not been measured yet. The measurement design is in
`plan.md`.

---

## Cross-Model Review Rounds 5-9 (2026-09-22)

Cut off by the usage limit, then resumed. Each round ran **individual calls** and **a call with
the lenses bundled in one prompt** side by side on the same diff, to measure the bundling effect.

| Round | Individual lenses | Individual findings | Bundled findings | Handling |
|:--:|:--:|:--:|:--:|------|
| 5 | 3 | 11 (blocker 3) | 7 (blocker 1) | 13 applied, 4 rejected |
| 6 | 2 | 6 (blocker 1) | 4 | 6 applied |
| 7 | 2 | 11 (blocker 3) | 6 (blocker 2) | 12 applied, 2 rejected |
| 8 | 2 | 5 | 3 | 8 applied |
| 9 | 2 | 5 (blocker 1) | 4 | 8 applied |

### All rejected findings (7 across 15 rounds)

| Round | Finding | Reason for rejection |
|:--:|------|-----------|
| 3 | The roadmap does not say when the local record file format is decided | Already in the `roadmap.md` Phase 4 tasks as a "before start ... fix" item. The reviewer did not see outside the diff |
| 5 | The local record file path and format are undecided, so M-6 cannot be implemented (individual) | **Intentionally deferred item.** Set in three documents as a before-Phase-4 decision |
| 5 | Same finding (bundled call) | Same as above |
| 5 | No behaviour when `MAX_CANDIDATES` is exceeded | 10.1 has "the excess is dropped". The reviewer looked only at the chapter 3 constant list |
| 5 | No behaviour when `MAX_PENDING_PINGS` is saturated | 5.5 has "when full, no new `PING` is sent". Same reason |
| 7 | Local record file undecided (blocker) | Same intentional deferral as above |
| 7 | Control plane schema undecided (blocker) | **Intentionally deferred item.** Before Phase 3 start, and the second bundle of this commit |

Rejecting is not discarding a finding. The first two are "the reviewer missed what is outside the
diff", and the rest are open items this commit intentionally left, so the same finding came up
again every round. So from round 8 **the prompt said "do not report these two again"**, and they
did not come up after that.

### The three big fixes in this stretch

**A common budget was put on transmissions to unverified destinations.** At first only `HELLO_ACK`
had a rate limit, but the adversarial lens pointed out that **one `HELLO` from a source outside
the candidates triggers both a `HELLO_ACK` and a path verification `HELLO`**, so the sum of
per-path limits underestimates the actual transmission volume. Including `PONG`, they were bundled
into one token bucket per session. In doing so, values scattered over three places were gathered
into 9.4 alone.

**Decided how many lines the local record file has per attempt.** `PING` starts after `CONNECTED`,
so there is no RTT sample at establishment time. So the RTT on the establishment line is usually
empty, and only attempts that reached `CONNECTED` get one more line at termination. **Whether it
was reached can be read from the line count.**

**Prevented the infinite loop of the test build echo responder.** If both sides enable the option,
an echo is echoed again. Synthetic packet offset 40 is used as a direction marker and responses are
not answered.

### Lens bundling measurement result

| Axis | Individual calls | Bundled call |
|------|----------|----------|
| Findings | 38 | 24 |
| blocker | **8** | 3 |
| Input tokens | 187k | **85k** |
| Findings per token | 0.20/1k | **0.28/1k** |
| Places only the bundle found | — | **21** |

**Bundling is 1.39x more efficient per token, but individual calls found 2.7x more blockers.**
And the two found different things. The bundle alone found 21 places. So **neither replaces the
other; the union is used.**

**In round 5, which had three lenses bundled, one lens dropped out entirely.** Not zero findings —
that lens was not handled at all. The four rounds with two lenses handled both every time.
Because each finding had to carry its lens name, the omission could be seen.

## `CLAUDE.md` Rule Additions — Prior Approval by the Repository Owner

**The repository owner gave prior approval in this session.** The instruction was "measure and put
them in all at once. Do not propose, put them in directly, and tell me when reporting the first
round of work". They were not added without approval. This is recorded here so the next session
can verify it from this record.

Six were added. The first four are in the "When running reviews" section, the last two in the
"When working on documents" section.

1. Exclude the record folders from review input (measured: 20% saving per call)
2. Run both individual and bundled calls and use the union (measured: table above)
3. Do not put three or more lenses in a bundled call, and have each finding carry its lens name
   (measured: lens omission)
4. Judge whether a review is running only by the output file, and do not report "running" when
   nobody is present
5. In repair rounds, fully check **the text I newly wrote** as well (the mechanism of 3 of 6
   blockers)
6. Do not assert unverifiable facts to fill a gap (the Wintun API case)

Number 4 was added because the same mistake was made twice in this session. I reported that a
review was launched when it was not. Both times the user's question caught it. When a guard was
built and tested, **the check at launch time did not catch the usage-limit failure.** Right after
launch, `running` is true and the error comes a few seconds later. So the verdict was moved to the
output file.

## Rounds 10-15 (Right Before Commit)

| Round | Calls | Findings | What came out |
|:--:|:--:|:--:|------|
| 10 | 3 | 2 | The bundled call gave `LGTM` for the first time. Individual calls found 2 more |
| 11 | 2 | 5 | All references to step numbers in 10.4 (c) |
| 12 | 2 | 5 | Numeric errors in this record file and in `plan.md` itself |
| 13 | 1 | 3 | Tally arithmetic, target list, console queue verification condition |
| 14 | 1 | 1 | The rounds 10-12 subtotal was wrong again |
| 15 | 1 | 1 | The tally script was counting files that were not review outputs |

Round 11 was about **step number references in the 10.4 (c) procedure**. Token reservation was
moved earlier and the steps were renumbered, but three places in the body pointed at the old
numbers and a non-existent "step 0" remained. **All number dependence was removed and replaced by
condition names.** This is the third place in this commit where the same kind came up.

From round 12 the defects came **from record documents, not living documents**. `CLAUDE.md` was
missing from the target list, the rejection count was written as four when it was actually seven,
and the round count in `plan.md` did not match the record. Round 13 caught the tally arithmetic
and the console queue verification condition. In that verification, if `[loop]` drains the queue
the 17th is not dropped, so **a correct implementation could fail.** It was fixed with the
consumer stopped as the condition.

**Round 14 caught that the subtotal I wrote by hand was wrong; round 15 caught that the tally
script made to fix it was counting files that were not review outputs (`tmp/r5_plan.txt`, an
experiment design memo).** Counting by hand was wrong, and counting by script got the input scope
wrong. The tally below is recounted with a narrowed file name rule.

| Range | Calls | Findings | blocker |
|------|:--:|:--:|:--:|
| Rounds 1-4 | 15 | 60 | 6 |
| Rounds 5-9 | 16 | 62 | 11 |
| Rounds 10-15 | 10 | 17 | 1 |
| **Total** | **41** | **139** | **18** |

7 were rejected, and the reasons for all are in the table above.

## One More Lesson

**Record documents are review targets too, and the numbers I count are wrong.** Of the 10 findings
in rounds 12-15, 6 were numeric and list errors in this file and `plan.md`. The first eleven rounds
put only living documents in the input, so nobody looked at this file. **Once, right before
commit, put `plan.md` and the commit record in the input too.** It does not have to be every
round. And **tallies are written by counting the outputs, but also check what is being counted.**
Adding the table by eye was wrong three times, and after counting by script the input scope was
wrong once more. The reviewer caught it every time.
