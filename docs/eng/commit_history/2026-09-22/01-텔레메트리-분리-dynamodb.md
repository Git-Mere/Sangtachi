# Telemetry Service Split and State Store Move to DynamoDB

Two things were decided before writing the control plane schema document, because the shape of
the schema depends on both. If telemetry collection lives inside the control plane, `report_*`
enters the control plane schema, and the representation of room and peer items differs depending
on whether the store is SQLite or DynamoDB.

## The Wrong Report Before This Commit

**This work started from my citing document content that did not exist.** When the user asked
whether the telemetry split was possible, I answered that `architecture.md` chapter 1 had the
sentence "the control server and the telemetry server are different deployment units. If
telemetry dies, the control plane lives." **There was no such sentence.** The actual document did
the opposite: it drew telemetry collection inside the control plane box (the diagram,
`telemetry.py`, `report_telemetry` in 6.1, "telemetry uses the control plane TCP socket"). I also
wrote the FR/NFR numbers and Phase numbers wrong.

The cause was trusting broken shell output without verification. In the same call every document
came out with the same line count of 586, headings appeared twice, and words not in the `ls`
output were mixed in. There were three signals, and I cited it as is.

It surfaced when the user asked "on which line". Without that question I would have fixed the
documents on a wrong premise. After that, every citation read the file directly with Python and
gave the line number with it.

## Changes

### Telemetry service split

| Document | What |
|------|------|
| `architecture.md` chapter 2 | Split the AWS box in the diagram in two. Coordination server and telemetry service |
| `architecture.md` 3.2.1, 3.2.6, 3.2.7 | The upload target of the client `[telemetry]` thread changed from the control plane to the telemetry service. Stated that it uses its own TCP socket |
| `architecture.md` 3.3 | Removed `telemetry.py`. Added `store.py` |
| `architecture.md` 3.4 | **New.** The telemetry service and the 4 split contracts |
| `architecture.md` 6.1, 6.3 | Removed `report_connection` and `report_telemetry` from the control plane operations and moved them to 6.3 |
| `architecture.md` chapters 9, 10, 11 | Upload target, repo structure (`telemetry-server/`), external dependencies |
| `spec.md` | Added a telemetry service item to the scope. Changed the FR-14 report target. **NFR-10 new** |
| `roadmap.md` | Phase 3 verification: "the control server has no metric collection endpoint". Phase 9: service implementation, 2 pre-start items, 2 split verdicts |
| `protocol.md` 4.5 | Upload target of the loss rate snapshot |

### State store DynamoDB

| Document | What |
|------|------|
| `spec.md` C-3, NFR-5 | "a single EC2 instance and SQLite" → "two processes on a single EC2 instance and Amazon DynamoDB". `boto3` approval recorded in NFR-5 |
| `architecture.md` 3.3 | **5 new store contracts.** `ConsistentRead`, no GSI, conditional-write claim, expiry verdict, stateless (read per request) |
| `architecture.md` chapter 11 | Removed `sqlite3`. Added `boto3` (approved) and DynamoDB |
| `roadmap.md` Phase 3 | **3 pre-start items** (table design, credentials, free tier check) and 1 line on the limits of local testing. 2 SQLite places to DynamoDB. **4 verifications added.** The counts were made by machine |

### New documents

- `decisions/0003-텔레메트리-서비스-분리.md`
- `decisions/0004-상태-저장소-dynamodb.md`

This brings the ADRs to 4.

### `CLAUDE.md`

With the repository owner's permission, **three stale facts were fixed. No rule was touched.**

- "`docs/kor` and `docs/eng` are aligned and `docgate.py` passes" → **already wrong at HEAD.** The
  mirror is behind and it fails on `mirror`/`parity`. Changed to: it is normal if `link` is 0
- ADR count 2 → 4 (two places)
- One line each for the telemetry split and the DynamoDB move in the current status section

And **one rule was added to the "When running reviews" section. This is a new rule, not a stale
fact correction, so separate permission was obtained from the repository owner.** Review input is
cut per lens, repair rounds send only the increment, and countable checks belong to `docgate.py`.
The basis is the review token measurement of this work.

| Item | Value |
|------|-----|
| Calls | 7, about 180k tokens |
| Limit-exceeded failures | 2. 44k with 0 outputs |
| Floor per call | 22k. What the two failed calls spent without doing any work |
| Full diff | 72KB sent whole in all 7 calls |
| Count-error findings | 2 of 5 in F1. Recounted the same thing with three lines of Python |

Automating the count checks in `docgate.py` or a separate script is not part of this commit.
**No new code goes in while review is blocked.** Put on the `plan.md` document debt.

## Decisions

**`report_connection` also moved to telemetry.** The compromise of moving only metrics and leaving
connection results in the control plane was considered and rejected. Connection success/failure
and the failure stage are the first row of the metric table in `architecture.md` chapter 9, and in
the Phase 9 analysis they sit on the same row as the other metrics. Splitting them means joining
two stores for every analysis.

**Telemetry does not ask the control plane whether identifiers are valid.** For data quality,
asking is better, but then the control plane takes on telemetry's load and failures and the reason
for the split disappears. Polluted rows are filtered by cross-checking at analysis time. The price
is written in ADR 0003.

**`get_peers` uses a strongly consistent read.** The cost is double, but if an eventually
consistent read drops the peer's candidate, punching ends in `HOLE_PUNCH_TIMEOUT` and **a store
defect disguises itself as a NAT failure.** In a project that measures success rate per NAT
behaviour, this pollution invalidates the whole result.

**Room expiry verdicts are not delegated to TTL.** What the AWS documentation guarantees is
"within a few days", and items stay visible to reads after the expiry time until deleted.

**Capacity mode starts as provisioned.** The documentation's general recommendation is on-demand,
but the pricing page states that the free tier capacity applies only to provisioned.

## Verification

**All DynamoDB facts were collected by a sub-agent from official AWS documentation.** 72 English
quotations were machine-verified against the source text, and the 5 that could not be confirmed
were separated into a list. Three of them (TTL delay upper bound, whether the free tier is
permanent, TTL behaviour in DynamoDB local) were left as they are in the "Not verified" section of
ADR 0004. **Missing evidence was not filled in.**

`docgate.py` is `fail`. All 14 findings are `mirror` and `parity`, and `link` is 0. **HEAD
already fails with 11 of the same kind** (confirmed by checking out HEAD separately with
`git worktree`). This state is normal by the convention that the English mirror is made when the
push instruction comes. But **the `CLAUDE.md` statement "`docs/kor` and `docs/eng` are aligned and
`docgate.py` passes" was already stale at HEAD.** Put on `plan.md` as the mirror-behind document
debt.

## Cross-Model Review

Three lenses were run **separately** on Codex (GPT). 8 findings came out and **all were applied.
0 rejected.**

| Lens | What it looked at | Result |
|------|---------------|------|
| L1 accuracy and internal consistency | Cited chapter/section numbers, requirement numbers, conflicts among the new contracts | warn 2 |
| L2 unfounded assertions | Was something written as guaranteed that the official documentation does not guarantee | warn 6 |
| L3 stale statements | After changing a definition, did sentences relying on the old definition remain | `LGTM` |

**The six L2 findings all had one root.** The effects of the split and of DynamoDB were written
stronger than the evidence.

| # | Finding | Applied |
|---|------|------|
| L1-1 | The `roadmap.md` verification "with only the control server down" does not hold if read as an instance outage, because both are on the same EC2 | Stated in both lines that it is a **process-level stop**, and added a sentence that it is not a test that takes the instance down |
| L1-2 | The premise in ADR 0004 context 2, "if two processes open one file the split breaks", is not forced. Separate files are possible, and that is alternative 2 of the same ADR | Made conditional. Wrote that it is not forced and the point is that **neither side solves context item 1 (tied to the instance disk)** |
| L2-1 | `architecture.md` split 4 "is not affected" is strong enough to conflict with the resource sharing limit in the paragraph right below | Narrowed the scope of the contract to **inter-service calls**, and stated that shared resource exhaustion is not covered by this contract |
| L2-2 | `spec.md` NFR-10 asserts no impact while resource isolation is undecided | Limited to **call dependency** and wrote that resource isolation is not part of the requirement |
| L2-3 | ADR 0003 alternative 3 guarantees "no code change needed" without verification | Lowered to "fewer reasons to change" and wrote that deployment, network, credentials, and addresses change. **Cannot be checked before the move is tried** |
| L2-4 | "works unchanged" in the ADR 0003 consequences overstates the effect by leaving out resource limits | Limited to a process stop and stated that resource exhaustion is excluded |
| L2-5 | "what TTL guarantees is within a few days" in ADR 0004 is a guarantee expression that conflicts with the same ADR's unverified item | Changed to "only describes it and **sets no upper bound**" |
| L2-6 | "keep the cost at 0" in ADR 0004 makes free operation read as fact while the free tier's permanence is unverified | Changed to "running inside the free limit" and wrote that **we do not say the cost becomes 0** |

### Round 2 — final check right before commit

Two more lenses were run including `plan.md` and this record file. **8 findings, 0 rejected.**

| Lens | Result |
|------|------|
| F1 repair verification + record cross-check (2 lenses bundled) | warn 4, nit 1 |
| F2 scenario end-to-end trace | **blocker 1**, warn 2 |

**The F2 blocker is this round's harvest.** Following "the control server restarts" to the end,
as the lens name says, **no document answered what is read to restore room state.** All three
lenses in round 1 passed over it. Looking at components and contracts one at a time, the gap is
not visible.

| # | Finding | Applied |
|---|------|------|
| F2-1 | What the restart restoration reads is deferred to after the table design, so the scenario cannot be traced (blocker) | **Store 5 new.** The control server holds no state in memory and reads on every request. **So there is no "restoration procedure".** NFR-3 holds by this contract, not by restoration |
| F2-2 | Unclear whether the restart restoration read is subject to store 1's strong consistency | Widened store 1 from the `get_peers` example to **"every place that reads room and peer items"**. With store 5 that is all of them |
| F2-3 | No answer to where a telemetry upload failure itself is recorded | Added a `telemetry_upload_failed` counter to 6.3 and wrote that it is a **different counter** from `telemetry_queue_dropped`. Counting both under one name makes remote failure indistinguishable from client overload |
| F1-1 | `register_peer` was missing from the `plan.md` schema list | Added |
| F1-2 | "5 pre-start items" in this record differs from reality | **Recounted by machine.** 3 pre-start items and 1 line on local testing |
| F1-3 | ADR 0003's "six places in documents" does not match the list that follows (four documents) | Wrote the file count and the place count separately |
| F1-4 | **A pre-repair premise remained** in ADR 0004 alternative 1. Context 2 was made conditional, but alternative 1 still asserts "it becomes a structure where two open one file" | Limited the title of alternative 1 to "share one file" |
| F1-5 | `report_connection` is not a periodic upload, yet it was bundled with `report_telemetry` as "moves on to the next period" (nit) | Wrote separately that the two have different periods. A connection report happens once per attempt, so there is no next period to move on to |

**F1-4 is exactly the mechanism `CLAUDE.md` warned about.** "In repair rounds, fully check the
text I newly wrote as well. If a definition changed, list the sentences that relied on it and
reread them one by one." I fixed context 2 and did not reread alternative 1. Caught in the same
place again.

### Total

**4 rounds, 10 calls (8 succeeded, 2 limit failures), 23 findings, 0 rejected.** The last final
round is `LGTM`.

**The last two paragraphs of this section were written after receiving that `LGTM`.** A sentence
that records a review result cannot be seen by that review. It is unavoidable as long as records
are kept, so it is written openly. The review marker stamped with the sha256 of the staged diff is
also stamped with this paragraph included.

### Round 3 — limit exceeded and rerun

The final round after round 2 **failed on both calls because of the usage limit. 44k tokens with
0 outputs.** The marker was not stamped and the commit was postponed. There was a stale marker in
`.git/cross-review-ok`, but its sha256 differed from the current diff and the gate blocked it.
**A stale marker could match by chance, so from now on it is compared before stamping.**

After the limit lifted, it was rerun **applying the rules newly put in `CLAUDE.md` as they are.**
Input was cut per lens and the calls were launched sequentially. 7 findings, 1 of them a blocker.

| # | Finding | Applied |
|---|------|------|
| R3c-1 | **`spec.md` NFR-3 says "telemetry send failures are ignored and retried", but 6.3 says "no retry"** (blocker) | Fixed NFR-3. **The failed record is not resent**, and the next period sends that period's new metrics. Now consistent with the 3.2.6 ring buffer dropping when full |
| R3c-2 | Store 3 prevents duplicate IPs but says nothing about what to do when the claim fails | Wrote: move to the next candidate, but with an **upper bound on attempts**, and end with a clear error when the bound is reached |
| R3a-1 | "the process holding room state" in ADR 0003 disagrees with store 5 (nothing held in memory) | Changed to "the P0 process that handles room state" |
| R3a-2 | "not tied back together at the store" in ADR 0004 hides the fact that both services use the same DynamoDB | Narrowed to **only the file lock relation disappears**, and wrote that separating tables, IAM permissions, and failure domains is not decided |
| R3b-1 | `plan.md` wrote the `CLAUDE.md` rule narrowed to `docgate.py` only | Aligned to "`docgate.py` or a script" |
| R3b-2 | This record also narrowed it in the same place | Aligned too |
| R3b-3 | "5 calls" in the total uses a different basis from the 7 in the body | Wrote success/failure separately |

**All three blockers came from the scenario trace lens.** The three round 1 lenses (accuracy,
evidence, stale statements) and the round 2 repair and record lenses caught none of them. **Looking
at components and contracts one at a time, the gap is not visible.** This is what
recurrence-prevention rule 1 in `CLAUDE.md` says.

### Measured review input savings

The effect of the rule newly put in `CLAUDE.md` (per-lens scope) was measured in the same work.

| Call | Diff sent | Tokens | Tokens/KB |
|------|-----------|------|---------|
| R3a repair (scope 53%) | 40,616 B | 16,379 | 413 |
| R3b record (scope 29%) | 22,151 B | 19,287 | 892 |
| R3c scenario (full, final round) | 76,092 B | 24,289 | 327 |

**Comparing the same lens is cleanest.** The repair lens on the full diff took 22,172 tokens, and
scoped 16,379 tokens. **-26%.** The round total went from 72,478 for 3 calls in round 1 to 59,955
for 3 calls in round 3, **-17%**. Same number of calls, and it went down.

**But "input is almost everything" was not true.** R3b sent only 29% of the diff but used 79% of
the full-diff tokens. Fitting two points, tokens per call are roughly **fixed 7.3k + 228 per diff KB
+ lens workload**. R3b's 7k excess is what that lens spent exploring to cross-check the
`CLAUDE.md` rules.

**Limits of this measurement.** The three calls had different lenses, so it is not an experiment
that isolates diff size alone. The linear fit has two points and is not precise. **Nothing other
than the same-lens comparison (-26%) is used as a metric.**

### Round 4 — final

After repairing the 7 findings, two lenses (repair verification, scenario re-trace) were bundled
and run once more on the full diff. **`LGTM`.** 27,450 tokens. The final round sent the full diff
as the rule says.

The review cost of the whole work is **10 calls, about 232k tokens**. Of that, **44k is the 2 calls
that left nothing because of the limit.**

**The L3 `LGTM` was checked separately by machine.** One lens's `LGTM` does not mean clean, so the
living documents and all of `decisions/` were swept again for `sqlite` and expressions like "the
control plane collects telemetry". The only hits were **the context and alternatives sections of
the ADRs, and describing the old state is their job, so that is normal.** The same goes for
"the decision that moved the store from SQLite" in `roadmap.md`.
