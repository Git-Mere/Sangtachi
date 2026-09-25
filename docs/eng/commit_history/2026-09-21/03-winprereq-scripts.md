# 2026-09-21 windows-prereq predicates moved into scripts

## Changes

- `tools/winprereq/Test-SubnetOverlap.ps1` **new.** 85 cases
- `tools/winprereq/Test-FirewallPolicy.ps1` **new.** 225 cases
- `windows-prereq.md` sections 2 and 7 drop the decision procedure and keep a call plus a pass condition
- `windows-prereq.md` chapter 0 gains a sentence saying `unknown` is not a pass
- `CLAUDE.md` current status records `tools/winprereq/`

## Why

The diagnosis is in the same-day record `2026-09-21/01-docgate.md`. In short,
`2026-09-21/02-windows-prereq` took 31 rounds and 73 findings, and **32 of them came from two
predicates.** The PowerShell decision procedure lived in the document as prose, so there was no case
table, and the reviewer supplied counterexamples one per round.

This commit moves those two predicates into scripts with case tables. That is recurrence rule 6
("put the procedure in a tool and let the document point at it") and rule 7 ("a function that
decides gets its checks written first").

## The line between document and script

**What to look at belongs to the document; how to count belongs to the script.**

| Kept in the document | Moved into the script |
|------|------|
| The three prefix-length classes and their verdicts | Overlap arithmetic, input rejection, self-adapter exclusion |
| Why `BLOCK-shadow` blocks rather than warns | Route and address queries and their tallies |
| Why `catch-all` is not counted as a conflict | Counting `BlockInboundAlways` and disabled profiles |
| The `NotConfigured` counterexample (a rule 3 case) | Rule scope decisions (adapter, protocol, port) |
| The measured 6 lines a string comparison misses | Port range arithmetic |

The **8 rejection lines** and the **10-line verdict table** were deleted from the document. A case
table in two places gets updated in one. It had already drifted: round 31 narrowed the regex to
`^\d{1,2}$` without updating the printed list, so the document claimed `2001:db8::1/128` was caught
by the "IPv4 prefix length" check while it was really caught by the digit check.

## What writing the case table surfaced

These came from building the table, not from a reviewer. They are defects of the previous revision.

| Defect | Where |
|------|------|
| The target range was never validated, so `10.100.0.5/24` silently produced a wrong verdict | Section 7 |
| `10.100.0/24`, `010.1.1.1` and `10.100.0.256` were not rejected | Section 7 |
| A failed query left an empty list that read as `proceed`. **There was no undecidable state** | Section 7 |
| An empty array unrolled to `$null`, making "0 results" and "query failed" the same value | Section 2 |
| On this machine `Enabled` is `GpoBoolean`, not `[bool]`. An `-is [bool]` guard would fail a healthy machine | Section 2 |
| The `System` token in built-in Windows rules was counted as a dead path | Section 2 |

**There are now three verdict values.** `proceed`/`pass` 0, blocking 1, `unknown` 2. **An
undecidable result is never written as a pass.** When a definite block is present, the block wins.

## Verification

| Item | Result |
|------|------|
| `Test-SubnetOverlap.ps1 -SelfTest` | 85 cases, 0 failures. `SELFTEST: pass` |
| `Test-FirewallPolicy.ps1 -SelfTest` | 225 cases, 0 failures. `SELFTEST: pass` |
| Live run | `VERDICT: proceed` and `VERDICT: pass` on this machine. Exit code 0 |
| Documentation gate | `pairs=26 links=224 findings=0`. `VERDICT: pass` |

**A table where everything passes is not evidence.** The implementation was broken one place at a
time to see whether the table catches it: 13 mutations for the subnet script, 7 for the firewall
script. Removing the alias comparison trips `scope-wrong-adapter`; filtering by `NextHop` trips
`gateway-route-still-blocks`.

**One mutation survived at first.** The port mutation only touched the leading branch, so the set
comparison behind it still ran. That was a fault of the mutation, not of the table. Reverting the
whole port section reproduced the regression and 5 cases caught it. **When a mutation survives,
first check whether the mutation is a real regression.**

## Cross-model review

| Round | Reviewer | Result |
|:--:|------|------|
| 1 | Codex (GPT) | 1 blocker, 2 warns, all in the firewall script |
| 2 | Codex (GPT) | 1 blocker. ICMP type |
| 3 | Codex (GPT) | 2 blockers |
| 4 | Codex (GPT) | 3 blockers, 1 warn |
| 5 | Codex (GPT) | 2 blockers |
| 6 | Codex (GPT) | 1 blocker |
| 7 | Codex (GPT) | 0 blockers, 2 warns |
| 8 | Codex (GPT) | **`LGTM - no blockers`** |

Round 1 findings and actions:

| Severity | Finding | Action |
|------|------|------|
| blocker | Rule scope passed anything that was not `Any`. **A rule bound to the wrong adapter passed** | Applied. Compared against an expected set; verdict values went from four to seven |
| warn | `Domain,Private,Public` spelled out is effectively `Any` but counted as narrowed | Applied. Compared as a sorted set |
| warn | Ports of `1-65535` or a comma list passed as narrowed | Applied. String comparison replaced by range arithmetic |

Two side effects were fixed as well: protocols arriving as numbers (`6`/`17`) failed healthy rules,
and verdict roll-up used string matching, so a rule name containing `=` shifted the result.

Round 2 findings and actions:

| Severity | Finding | Action |
|------|------|------|
| blocker | ICMP scope ignored `IcmpType`. **A rule opening every ICMP type counted as narrowed** | Applied. The type went into rule extraction, expectation parsing and the verdict; values grew to eight |

The expectation notation now takes `ICMPv4:8` and `ICMPv4:3:4`. The defaults match section 2 of the
document: `TCP:25565` and `ICMPv4:8`. Types are compared as values, not strings, and `8` means every
code under type 8.

Three real ICMP rules on this machine were checked as well. The `IcmpType 8` rule passed the type
comparison and stopped at the profile check, `Any` came out `wide`, and `3:4` came out
`other-icmp-type`. **That is evidence the comparison actually runs.**

Round 3 findings and actions:

| Severity | Finding | Action |
|------|------|------|
| blocker | `-InterfaceAlias` support was only printed as a note. **A machine that cannot create adapter-scoped rules still ended with `pass`** | Applied. Promoted to a `scope-param` check; the note was removed |
| blocker | Ownership rested on the name and the Wintun description. **A foreign Wintun adapter carrying our name hid its conflict** | Applied. The marker is now `InterfaceGuid`, and exclusion keys on `InterfaceIndex` |

**A new contract came with the fix** (rule 4). The client records the `InterfaceGuid` used to create
the adapter and passes it as `-OwnInterfaceGuid`. Exclusion happens only when all five conditions
hold; on any mismatch nothing is excluded and the reason is printed. The contract moved into
section 7 of the document.

**What the case table cannot cover is written down too.** The query-to-verdict wiring in live mode
has no place to inject synthetic input, so it cannot be pinned by the table. Instead a temporary
copy imitating an unsupported machine was run live to confirm `VERDICT: fail`. Excluding a live
Wintun adapter could not be run on this machine either, because no adapter exists. Section 7 was
therefore **lowered from measured to partly measured.**

Round 4 findings and actions. All four share one root: **a failed query was being turned into
"none".**

| Severity | Finding | Action |
|------|------|------|
| blocker | An adapter query failure was treated as an empty list, so a failing machine ended in `proceed` | Applied. The failure is passed as a fact and raises `unknown` |
| blocker | `Get-NetConnectionProfile` lacked `-ErrorAction Stop`, so a permission failure became "adapter not found" | Applied. Five queries with the same root were fixed |
| blocker | `Get-NetFirewallRule` had the same problem: a query failure became "0 rules" | Applied |
| warn | The self-test **imitated a query failure with a successful empty list** and pinned that result as correct | Applied. The imitation is gone; live and self-test now call the same wiring function |

**The warn is the heaviest finding of this round.** The case name was right, but the input it built
was not a real failure. **When a case pins the wrong contract, the table becomes a shield.** That is
a trap of the case-table method itself, met here for the first time.

Both scripts reached the same conclusion: **separate the verdict from the query and keep one piece
of wiring.** Only then does the self-test walk the real path instead of an imitation. The subnet
report named the signal:

**"A surviving mutation was itself the sign that an imitation was still there."** Two mutations
survived and forced two more fixes, and both times the self-test was failing to walk the live path.

**A self-inflicted hole was caught by the self-contradiction check too.** Adding the failure as an
ownership *reason* let `bad-guid` win first, so **a bad GUID together with a failed query hid the
failure.** The judgement now keys on the fact, not the reason, pinned by a case.

Round 5 findings and actions:

| Severity | Finding | Action |
|------|------|------|
| blocker | An application-filter query failure became an empty value, so **a rule whose path could not be read passed** | Applied. All 8 queries were swept; the 7 that report `Failed` each gained a failure-path case |
| blocker | The self-test pinned **"one matching expected endpoint is enough"**, so a single rule passed | Applied. An `endpoint-coverage` check now requires all of them |

**One more instance of the round 4 family was still there.** So this round asked for a sweep instead
of a point fix. Of the 8 queries, the 7 that report `Failed` each got a case. Two that were not in
the finding (`Get-NetFirewallProfile`, `Get-Command`) came out of the sweep.

The boundaries of `endpoint-coverage`: **0 rules is `unknown`.** Before Phase 6 that is not a defect
but work not yet done (rule 3). A rule set that misses an expected endpoint is `fail`. A wide rule,
or one bound to the wrong adapter, does not count as covering.

**The case table caught the worker's own mistakes twice.** In a new function `return @()` unrolled
again, making "0 results" equal to "query failed", and the fix then broke the counting. Both showed
up as a failing `-SelfTest`. **It was a repeat, in a new function, of a trap already met in round
1.** Without the table that would have gone to the next review round.

Round 6 findings and actions:

| Severity | Finding | Action |
|------|------|------|
| blocker | Rule selection ignored `Enabled`, so **a disabled allow rule counted as covering.** A machine whose rules are all disabled ended in `pass` | Applied. Only enabled inbound allow rules are selected, and `Direction` and `Action` were fixed with it |

**Measurement confirmed the finding directly.** On this machine
`Core Networking Diagnostics - ICMP Echo Request` is `Enabled=False`, yet through round 6 the script
selected it and reported `wide`. This machine has 240 disabled rules.

**"Disabled" and "not created yet" are separated.** With no rule matching the name the result is
`unknown` (normal before Phase 6). With rules present but all disabled, outbound, or Block, the
result is `fail`, because they exist and do not work. That is why **excluded rules are kept with
their reason.** Previously a single `Where-Object` dropped them without trace.

If any of the three fields cannot be read, the rule is excluded and the result is lowered to
`unknown`. **Nothing is assumed to be enabled.**

Round 7 findings and actions. **No blockers.**

| Severity | Finding | Action |
|------|------|------|
| warn | The profile-state verdict required `Domain,Private,Public` **in order**, so a healthy machine returning another order became `unknown` | Applied. Compared as a sorted set, and the printed sentence is sorted too |
| warn | Existing `/0` and `/1` were classified `catch-all` before the length check, so **a `/1` target missed an equal-length conflict** | Applied. Target length is limited to 2-32 and the verdict order was changed |

**Reachability was checked first.** A `/1` target really was accepted: the validation used `-lt 1`,
so only `/0` was blocked. It was reproduced on the pre-fix revision, so the fix was chosen over
pinning the behaviour.

The order change is **unreachable today**, because target validation blocks `/0` and `/1`. It was
made anyway and the reason is in a comment: on the day validation loosens, that order is the only
defence. **Knowing a path is unreachable is not the same as not knowing.**

**What was not changed is also in a comment.** For a target of `/2` or longer, existing `/0` and
`/1` routes stay exempt; that is the decision of section 7. The next reviewer will ask why a `/1`
target is rejected while a `/1` route is exempt, so the answer is written down in advance (rule 4).

**Mutation testing met its first equivalent mutant.** On the firewall side, adding `-Unique`
survived. The count check runs first, so the list there always has three entries; with a duplicate
the unique names number two or fewer and can never equal the three-name expectation. **That is not a
hole in the table but a mutant with identical behaviour.** No case was forced in. It is the opposite
of round 4, where a surviving mutant meant an imitation was still present.

**The case table also caught the worker's own over-strict criterion.** A profile name differing in
case was first expected to be `unknown`; running it reversed the judgement. Windows name comparison
is case-insensitive, and round 2 had already matched adapter aliases that way. **A rule 3 violation
was nearly created while fixing a rule 3 violation.**

Round 8 produced no findings. **10 blockers and 3 warns were applied in total, with nothing rejected.**
