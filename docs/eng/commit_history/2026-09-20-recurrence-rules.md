# 2026-09-20 Add Recurrence-Prevention Rules 5 to 7

## Changes

The blocker 20 work earlier the same day drew 93 findings over 19 rounds of cross-model review.
Analysing where they landed produced 3 new rules in `design-audit.md` chapter 6.

- Added rules 5, 6, and 7 with a supporting table to `design-audit.md` chapter 6 (Korean original
  included)
- Added the `check-peer` subcommand and `is_public_unicast()` to `natprobe.py`, collapsing address
  validation into one implementation. It also rejects IANA special-purpose ranges such as 6to4
  relay anycast
- Added `tools/nat-probe/unsolicited-firewall-test.ps1`, turning the procedure from the ADR into a
  real script
- Updated the "When working on documents" section of `CLAUDE.md` from 4 rules to 7, and added the
  self-contradiction check before review
- **Applied rule 6 to this project's own output.** The PowerShell procedure moved out of ADR 0002
  into `tools/nat-probe/README.md` chapter 12, and the ADR only points at it

## Why

56 of the 93 findings landed in 2 places.

| Location | Findings | Cause |
|------|:----:|------|
| `classify_nat` in `natprobe.py` | 31 | A verdict function written as a 20-line convenience |
| The PowerShell procedure in ADR 0002 | 25 | A script for a test that had not been run, placed in a document |

Most of the rest came from copying the same claim into 8 places (4 documents in 2 languages) and
then fixing one or two at a time.

## The new rules

Only the names are listed here. The originals live in
[`../audit-history/design-audit.md`](../audit-history/design-audit.md) chapter 6 (rule 5).

| # | Rule |
|---|------|
| 5 | Never copy the same claim into more than one place (mirrors, indexes, and historical records excepted) |
| 6 | An executable procedure in a document needs code-level review |
| 7 | A function that returns a verdict starts with validation |

The evidence for rule 7 is the most concrete. `is_private` includes loopback and documentation
ranges, and `is_global` is **true for multicast and for 6to4 relay anycast**. Trusting predicates
by their names cost 3 rounds.

## Rule 6 was applied immediately

Adding a rule while leaving the output that breaks it in place kills the rule. The PowerShell
procedure in ADR 0002 was moved.

| Before | After |
|----|-----|
| A 30-line PowerShell block plus a result table inside ADR 0002 | `unsolicited-firewall-test.ps1`, a real script |
| — | `README.md` chapter 12 documents how to run it and how to read it |
| — | ADR 0002 keeps one sentence and 2 links |

**The first attempt was a half-measure.** The script was moved into the README, and review pointed
out that it was still unexecuted PowerShell embedded in Markdown with no way to syntax-check it.
"Put it in the tool" in rule 6 means a `.ps1` file. Moving it properly also surfaced the defects below.

| Defect | Content |
|------|------|
| blocker | Firewall cmdlets fail non-terminating, so the measurement ran even when rule creation failed. **That yields the false conclusion "blocked even with the rule".** Fixed with `$ErrorActionPreference = 'Stop'` and an activation check right after creation |
| Duplicated validation | A second implementation in PowerShell accepted special-purpose ranges such as `192.88.99.0/24`. Collapsed onto `natprobe.py check-peer` |
| Race | Startup unconditionally deleted the group's rules, including those of a concurrent run. Now it reports and aborts instead |
| Overstated conclusion | One transition to `allowed` was treated as proof. NAT state and timing also change between runs. It now alternates control and rule trials over an **even number of pairs (4 by default)**, flipping the order each pair, and looks for a consistent difference |
| blocker (round 2) | IPv6 addresses passed validation. The `natprobe` socket is `AF_INET`, so an unrelated rule would be created and `blocked` misread. Added `check-peer --ipv4` |
| blocker (round 3) | Rule deletion failures were hidden by `-ErrorAction SilentlyContinue` and the next control trial ran anyway. **A leftover allow rule contaminates the control group.** Deletion failure is now terminating, and a surviving rule aborts the whole test |
| Exit code | A failed `natprobe punch` was ignored and the next trial ran. Now `$LASTEXITCODE` is checked |
| Mutex leak | An exception during validation after acquisition never released it. The whole body is now in `try/finally` |
| Address exposure | The peer IP was passed as a Python command-line argument, leaving it in the process list and audit logs. Now piped through `check-peer --stdin`, and the `-Peer` parameter was removed so it only comes from `Read-Host` |
| Missing cleanup guidance | A deletion failure threw immediately, so the leftover check and the manual cleanup instructions never ran. It is now caught, checked, and re-thrown with the rule name and the exact removal command |
| blocker (round 5) | Only the created rule object's `Enabled` was checked. GPO merge results take effect in **`ActiveStore`**. Treating an unapplied rule as active would invert the conclusion. `Enabled`, `Action`, and `Direction` are now verified in `ActiveStore` |
| Value mismatch | Python validated a `strip`ped stdin value while the firewall rule used the original string. It is now trimmed, whitespace and newlines are rejected, and the same value is used for both |
| Ambiguous arguments | `check-peer` accepted a positional address and `--stdin` together and silently preferred stdin. Supplying both is now an error |
| Order effect | The control trial always ran first, so a time-dependent effect such as mapping warm-up could imitate a firewall effect. The order is now flipped each pair and the condition is encoded in the label |
| Partial stdin validation | `readline()` read only the first line, leaving the rest unvalidated. All of stdin is read, and whitespace, multiple lines, and empty input are rejected |
| blocker (round 7) | Firewall queries used `-ErrorAction SilentlyContinue`, so **a query failure read as "no rules".** That means measuring with a stale rule present, or falsely reporting a clean cleanup. `Get-TestRuleSafe` now treats only a genuine no-match as empty |
| Incomplete deny list | `192.0.0.9` (PCP anycast), `192.0.0.10` (NAT64 discovery) and others passed. `192.0.0.0/24` is now rejected as a whole, and **the docstring states this is not a complete IANA check** |
| Untested CLI | Only the function was tested; argument handling, exit codes, and address exposure were not. Added 26 `subprocess` cases |
| blocker (round 9) | Cleanup checked only the default store. **An applied rule can linger briefly in `ActiveStore` and contaminate the next control trial.** It now polls for up to 5 seconds until the rule is gone |
| `-Trials 1` | A single pair leaves no reversed order, removing the order control while the interpretation table still claimed causation. Now **even and at least 2**, defaulting to 4 |
| Assumed peer address | Scoping the rule to the peer IP **assumed** the unsolicited packet's source address. With an address-dependent NAT the rule would never match, and that result would be read as "the firewall is not the cause". **The address was removed entirely and the rule is scoped by port only** |

**That last fix also removed the privacy problem.** With no address to read, the `Read-Host`,
`--stdin`, and transcript exposure paths all disappeared. After closing 5 leak paths one at a
time, **removing the entrance turned out to be the right move, and it was found last.**

`check-peer` is no longer used by the script. It remains as a standalone helper, documented in
README chapter 3, and its 26 CLI tests are kept. The `ok` it printed on success was removed: it
**contradicted the documented contract of answering by exit code only** and made it unusable as a
quiet validator.

**Inverted blockers kept appearing.** Fixing "do not hide a failure" blocked the normal path, and
fixing that missed failures again. It alternated between breaking recurrence-prevention rule 3
(check that a correct implementation passes) and rule 4 (define the contract a fix creates).

| Original | Fix | What the fix broke |
|------|------|---------------------|
| Query failure read as "no rules" | Made it strict | **A genuine "none" was also blocked, so a clean state could not run** |
| Deletion failure hidden | Made it terminating | The cleanup guidance never ran |
| Creation verified in `ActiveStore` | | Deletion and startup checks were not updated |
| Rule scoped to the peer address | Port only | The rule got broader with no warning about persistence |
| Mutex for single execution | | An abandoned mutex (`AbandonedMutexException`) **blocked a legitimate re-run** |
| Application verified once immediately | | Policy propagation delay failed a valid trial |
| Rule scoped by port only | | **Its premise (open to every source) was never verified.** A policy merge narrowing the address range would still pass |

The final blocker was `Get-TestRuleSafe` catching the exception **by type.** A normal "no match"
from `Get-NetFirewallRule` also arrives as `CimJobException`, and only `CimException` was caught,
so **the script aborted even from a clean initial state.** It now decides by category and the
`CmdletizationQuery_NotFound*` error ID instead of by type.

An ADR is about **the decision and its reasoning**. The run procedure for an optional test that
has not even been performed was taking up a third of it.

## Verification

- Heading counts match between eng and kor (design-audit 18, ADR 0002 5 each)
- ADR 0002 contains 0 PowerShell blocks. The procedure exists only in README chapter 12
- Every relative link except `experiments.md` resolves to a real path
- `test_natprobe.py` passes **139/139.** 10 cases added for `is_public_unicast` and the rejected
  IANA special-purpose ranges
- Ran `check-peer` directly: `8.8.8.8` accepted; `192.88.99.1` (6to4 relay anycast), `2002::1`,
  and `64:ff9b::1.2.3.4` rejected; `--ipv4` rejects IPv6
- Confirmed `check-peer` prints no address on either success or failure

## Cross-model review

- Reviewer: Codex (GPT). **14 rounds**
- Result: 6 blockers, 39 warns, 3 nits. **1 rejected, everything else applied**

**The rejected finding.**

> Printing the `DisplayName` and `PolicyStoreSource` of stale rules at startup can expose local
> firewall policy information in a shared terminal log. Restrict details to an optional
> diagnostic mode

**Why it was rejected.** That listing *is* the cleanup instruction. Hiding the rule names leaves
the user unable to tell what to delete, which recreates the defect fixed in round 4: a safety
measure that leaves the user at a dead end. The names are GUIDs this script generated, not user
data, and knowing which store holds them is needed to choose the removal command.

**Other exposure findings in the same round were applied.** The raw exception in the deletion
failure message is not needed for cleanup, so it moved behind `-Verbose`. **This distinguishes
necessary information from unnecessary information; it is not a blanket rejection.**

## Remaining

**`unsolicited-firewall-test.ps1` has never been run.** Of the 6 blockers in this commit,
**3 were "cannot run even in a clean state"** (type-qualified catch, abandoned mutex, empty-array
unrolling with StrictMode), and all were found by static review alone. Running it would have
surfaced them on the first line.

**The next step on Windows is execution, not review.** One run with `-Trials 2` in an
administrator PowerShell will reveal more than another review round.

Review stopped at round 17. Round 18 was started and cancelled, so findings may remain.

Rule 4 was broken for the third time. Again, the description of a cause was softened while the
conclusion resting on it was not. **Rule 4 is not broken for lack of a rule; it is broken by not
tracing the dependency.** More rules will not fix that, so the self-contradiction check before
review was written into `CLAUDE.md` as a procedure.
