# 2026-09-21 Windows preconditions document (follow-up 3)

## Changes

Follow-up 3 is done. Blockers 11-17 and 6 platform warnings, 13 items in all, each get their own
section.

- **New** `docs/{kor,eng}/windows-prereq.md`
- `plan.md` item 3 `todo` -> `done`, with a verification result table
- `design-audit.md` blockers 11-17 and the 6 warnings `open` -> **`documented`**, chapter 5 table updated
- `decisions/0002` **corrected.** Its cost statement about administrator rights was wrong
- `CLAUDE.md` document role table, heading-count check list, current state

## Why a separate document

It did not go into `architecture.md`. That document covers system structure, module breakdown,
the concurrency model and the data plane path. Adding check commands and expected output for 13
items would double its size and blur its role. It is registered in the `CLAUDE.md` role table as
the single source for runtime preconditions.

## Which sections are measured and which are not

**Every section states its verification status.** There are three grades: 5 measured, 3 partly
measured and 5 unverified.

| Status | Sections |
|--------|----------|
| Measured | 1 admin rights, 7 subnet conflict, 8 on-link route, 9 tentative, 12 Java path |
| Partly measured | 2 firewall and profile (default policy and existing adapters only), 3 leftovers (only somebody else's stale adapter), 13 SmartScreen (only the policy value) |
| Unverified | 4 Wintun (not adopted), 5 Minecraft bind (Phase 8), 6 EC2 (Phase 3), 10 Elastic IP (Phase 3), 11 LAN discovery (Phase 8) |

**Output from one machine is a fact about that machine.** The document says so, and states that
what transfers is the pass rule, not the value.

## The commands were reviewed like code

That is recurrence-prevention rule 6. Running them first produced two findings.

**1. Deciding with `Get-NetFirewallProfile` fails a correct machine.**

```
DefaultInboundAction : NotConfigured
```

`NotConfigured` does not mean "not set", it means **"use the built-in default"**, and that default
is Block. A check written as `-eq 'Block'` **reports a stock Windows as failing.** On the same
machine `netsh advfirewall` correctly says `BlockInbound`. The decision is made with `netsh`, and
the cmdlet appears only as a counterexample. **This is the exact situation recurrence-prevention
rule 3 describes.**

**2. My own check command hid its verdict.**

Printing the Java path check through `Format-Table -AutoSize` **drops the `Exists` column
entirely** because the path is long. It really did disappear on this machine. It was changed to
formatted-string output, and the document records why a table is wrong here.

## Correcting ADR 0002

[ADR 0002](../../decisions/0002-no-rebinding-recovery.md) stated the cost of option a (the client
registers a firewall rule) like this.

> It would also require administrator rights, increasing the burden of blocker 11
> No administrator rights requirement appears

**Both are wrong.** Blocker 11 itself reads "adapter creation and route configuration need
administrator privileges; the document never mentions it". Creating a Wintun adapter already
requires them. The marginal cost of option a is not "rights appear" but **"make one more rule with
rights you already hold"**.

**The cost of option a drops accordingly.** The decision itself is unchanged: the evidence that
the NAT is the likely cause still stands, and the condition that would reverse it is still the
firewall test. Only a cost item that disagreed with the facts was fixed.

The contradiction only became visible while writing follow-up 3, with section 1 and the ADR side
by side. **Each document made sense on its own.** It is the kind of defect that
recurrence-prevention rule 4 aims at.

## Verification

| Item | Result |
|------|--------|
| kor/eng heading counts | `windows-prereq` 19:19. The other 6 documents also match |
| Relative links | Every link in 60 documents was resolved. Only `experiments.md` is missing, which is allowed as a Phase 9 deliverable |
| Commands in the document | **Every command with an available target was run, and section 4 had no target, so it was shape-checked against `kernel32.dll`.** Queries for a missing adapter or route return 0 without error, and `Unblock-File` and the `Zone.Identifier` query both work. **The signature and architecture commands in section 4 ran against `kernel32.dll` because there is no `wintun.dll` yet** (`Status: Valid`, `0x8664`). The command shape is confirmed; the target file is not |
| `#` inside code blocks | The heading check counts `^#`, so code comments were moved into prose |

## Cross-model review

- Reviewer: Codex (GPT). Three lenses (correctness/security/robustness, claim checking, and
  **Windows facts and commands**). **Findings were applied across 31 rounds**, and the result was
  reviewed once more, with no findings from any lens, before committing
- Result: 0 blockers, 93 warns, 6 nits. **4 dismissed, the rest applied**

| Round | Finding | Action |
|:--:|------|--------|
| 1 | 8 sections are measured, not 7 (6 places across 3 documents) | Applied |
| 1 | "everything runnable was run", but the section 4 commands have no target file | Applied. Recorded that they ran against `kernel32.dll` |
| 1 | **Subnet conflict decided on `NextHop`.** It misses a VPN route sending `10.0.0.0/8` to a gateway | Applied |
| 1 | SmartScreen does not block every unsigned file; it is reputation based | Applied |
| 1 | A USB stick or a local build does not guarantee no MOTW | Applied. The `Zone.Identifier` check is now required |
| 1 | Turning protection off without checking provenance | Applied. A `Get-FileHash` match is now a pass condition |
| 2 | **Unverified items were marked `fixed`.** The audit alone makes the risk look closed | Applied. All 13 rows now say `documented` |
| 2 | Section 13's pass rule is about the executable, but only the policy value was read | Applied. **Added the `partly measured` grade** |
| 3 | **The corrected conflict rule fails a full-tunnel VPN machine.** `0.0.0.0/1` is not the default route | Applied |
| 4 | The next version's string match is wrong too: it blocks `10.100.0.0/23` and misses `10.100.0.5/32` | Applied. **Parse the prefix length and compute overlap both ways** |
| 4 | The warning check only scans `10.*`, so it never sees `0.0.0.0/1` | Applied. The overlap test runs over all IPv4 routes |
| 4 | The commit record says the rows became `fixed`, but they became `documented` | Applied |
| 4 | "machine verification is done" in `CLAUDE.md` cites nothing | Applied. The commit record path was added |
| 4 | "only the first run is blocked" is stronger than SmartScreen behaviour | Applied. Narrowed to the same file on the same PC |
| 4 | No `Zone.Identifier` output does not by itself mean no MOTW (FAT32 and so on) | Applied. Added `-LiteralPath` and the file-system caveat |
| 5 | **The conflict check does not produce a verdict.** It prints a list and leaves the lengths to the eye | Applied. The command emits a `Verdict` column and a single `VERDICT: proceed` line |
| 6 | The self-test table says `catch-all` where the documented function returns `normal` | Applied. Replaced with the output of running the function as written |
| 6 | Section 3 says `measured`, but what was seen is **somebody else's** leftover adapter | Applied. Changed to `partly measured` |
| 6 | "our build output falls into that case" is an expectation, not an observation | Applied. Stated as an expectation and deferred to Phase 8 |
| 6 | "tunnel UDP passes because we send first" is too broad | Applied. Narrowed to **the reply to a flow we started**, and recorded that all 6 unsolicited cases were `blocked` |
| 6 | "`/0` and `/1` exist on every machine" is wrong for `/1` (nit) | Applied |
| 6 | "machine verification is done" in `CLAUDE.md` has no evidence inside this diff | **Dismissed** |
| 7 | **Letting a length 2-23 overlap proceed does not close blocker 17** | Applied. It is now `BLOCK-shadow`; blocking is the default and only an explicit override proceeds |
| 7 | Section 2 says `measured`, but a new virtual adapter being classified Public was not observed | Applied. Changed to `partly measured` |
| 7 | "SmartScreen quarantines" in `design-audit.md` is stronger than the document | Applied. Changed to "may warn, block or quarantine" |
| 8 | **`netsh` labels are translated with the display language.** Filtering lines by English words catches nothing on a Korean Windows | Applied. Narrowed with the `firewallpolicy` subcommand and counted the untranslated value `BlockInbound` |
| 8 | The address test `IPAddress -like '10.100.0.*'` is silently wrong for an alternative range that is not a `/24` | Applied. Each address becomes a `/32` and goes through the same overlap function |
| 8 | Querying a missing route is not "no output"; it records a non-terminating error | Applied. The verdict is the count, and the fact is written down |
| 8 | `ss -ltnp` is Linux-only, but the section reads as EC2 in general | Applied. States that the control plane runs on Linux EC2 |
| 8 | The review result in the commit record has no evidence inside this diff | **Dismissed** |
| 9 | The route existence check uses `-ErrorAction SilentlyContinue`, swallowing a permission failure as "absent" | Applied. Only "not found" returns 0, the same shape as `Get-TestRuleSafe` |
| 9 | **`Select-String 'BlockInbound'` also passes `BlockInboundAlways`**, a mode that ignores every inbound allow rule | Applied. Count the whole value and check `BlockInboundAlways` is 0 |
| 10 | The Elastic IP cost note is out of date: a public IPv4 is billed even while attached | Applied, with a line telling the reader to check current pricing |
| 10 | The Minecraft multicast endpoint was stated as fact without verification | Applied. Narrowed to Java Edition, marked unverified, and stated that **the conclusion does not depend on the address** |
| 11 | The three leftover probes in section 3 swallow errors too; fix them rather than referring to the fix | Applied. One shared wrapper, and the gate/informational split is stated once in section 0 |
| 12 | The Elastic IP check passes on any EIP in the account | Applied. Filtered by instance id, requiring exactly one |
| 12 | An equal-length `/24` does not "win or tie"; the metric decides | Applied. Split `> 24` from `= 24`; both still block |
| 13 | Even with a `BlockInbound` policy, **a profile whose firewall is off** breaks the premise | Applied. The `Enabled` check is part of the verdict |
| 13 | `Set-NetConnectionProfile` can fail on an unidentified network because of policy | Applied. A `-Profile Any` rule comes first, and the category change now verifies its result |
| 14 | The policy check is still a substring match | Applied. Anchored with `\s...\s*$` and confirmed by a 5-case self-test |
| 14 | **The conflict check counts our own adapter's leftover address, blocking us because of ourselves** | Applied. `$ownAlias` is excluded and the section 3 cleanup must run first |
| 14 | "`/0` exists on every machine" is false without a default route (nit) | Applied |
| 14 | "a file extracted from an archive inherits the MOTW" is too absolute | Applied. It depends on the extractor and version; the check command decides |
| 15 | The Authenticode check was written as "the signature must be valid or the driver will not load"; it checks DLL provenance | Applied. Driver load is decided by the adapter creation result and `setupapi.dev.log` |
| 16 | Simplified syntax such as `Where-Object Enabled -eq $false` is fragile | Applied. Every `Where-Object` in the document now uses a script block |
| 16 | **The Elastic IP check again produces no verdict** | Applied. `--query 'length(Addresses)'` prints the count and `1` is the pass condition |
| 17 | The bind pass rule fails `[::]:8000`, which serves IPv4 on a dual-stack socket | Applied. All three forms pass, with an IPv4-only query alongside |
| 17 | "stop and start changes the public IPv4" is false with an Elastic IP attached | Applied. Narrowed to an **auto-assigned** address |
| 18 | Accepting `[::]:8000` leans on the `bindv6only` default, which can be changed | Applied. The IPv4-only query is the verdict and the external connection is the confirmation |
| 18 | The record claims every `Where-Object` became a script block, but one was left (nit) | Applied |
| 19 | `netstat -ano \| findstr 25565` also matches remote ports and does not separate listening sockets | Applied. Replaced with `Get-NetTCPConnection -State Listen` plus a `LocalAddress` rule |
| 19 | `Test-Path $_.Program` marks a healthy rule dead when the path uses `%ProgramFiles%` | Applied. Expand with `ExpandEnvironmentVariables` first; measured `False` unexpanded and `True` expanded |
| 20 | A `LocalAddress` of `::` does not prove IPv4 is accepted | Applied. Marked inconclusive, with `Test-NetConnection` as the verdict |
| 21 | Excluding our adapter by name alone lets a same-named foreign adapter hide a real conflict | Applied. Exclude only when exactly one adapter with that name has a Wintun description |
| 21 | The address check counts a normal address such as `10.0.0.49` as a block | **Dismissed** |
| 22 | **Both examples given for why string comparison fails were wrong.** `10.100.0.5/32` does match the string, and `10.100.0.0/23` blocks correctly under the current policy | Applied. Replaced with real misses (`10.0.0.0/8`, `0.0.0.0/1`) and a measured table |
| 23 | With `$ownAlias = $null`, `-ne $null` drops rows without an alias, making "exclude nothing" false | Applied. Guarded with `-not $ownAlias -or ...` |
| 23 | `ConvertTo-UInt32Ip` reads the first 4 bytes of an IPv6 address as IPv4 | Applied. Throws unless `InterNetwork`; the IPv6 rejection is measured |
| 23 | The leading `\s` in the policy regex is too strict if the spacing changes | Applied. Relaxed to `(^\|\s)` and the count of 3 was re-confirmed |
| 24 | **Recommending `-Profile Any` alone opens the rule on the physical network too** | Applied. The example is now scoped with `-InterfaceAlias` and an exact protocol and port |
| 24 | `Status: Valid` only means "somebody trusted signed it", so another signed DLL passes | Applied. Three conditions now: publisher `Subject` and the published release hash as well. Shape checked with `kernel32.dll` |
| 24 | `grep 8000` also matches unrelated ports such as `18000` (nit) | Applied. Uses `ss 'sport = :8000'` |
| 25 | The SmartScreen policy query only reads the Group Policy override; "the default applies" is too strong | Applied. The valid check is launching the binary |
| 26 | `Test-Overlap` accepts `/33`, a missing length and malformed text, producing a wrong mask | Applied. Format and range are validated, with 6 rejections in the self-test |
| 27 | **The IPv6 case in that self-test never reaches the IPv6 guard**, because the format check catches it first | Applied. Added `2001:db8::1/24` and recorded which test rejected each input |
| 28 | `[int]''` is `0`, so `10.100.0.0/` reads as `/0` and passes | Applied. The digit test comes first, and the self-test grew to 8 rejections |
| 29 | "every section has a check command" is false for section 11, which is a manual procedure | Applied. Reworded to "how to check and what passes", with the exception stated |
| 29 | Even with `-AddressFamily IPv4`, a `::/0` entry raises instead of producing a verdict | Applied. Only IPv4-shaped prefixes are passed in |
| 30 | "every command with a target was run" hides section 4 | Applied. The first sentence now says section 4 was only shape-checked |
| 30 | Section 1 asserts all three operations need admin rights, but only elevation was measured | Applied. Marked as a Windows API premise, with confirmation deferred to Phase 6 |
| 30 | The machine-verification claim in `CLAUDE.md` has no evidence (third time) | Applied. The commit hash `c48d9dd` is now cited |
| 31 | `/999999999999` passes the digit test and then throws inside the `[int]` cast | Applied. `^\d{1,2}$` bounds the digit count |
| 31 | `New-NetFirewallRule -InterfaceAlias` may not exist on stock Windows | **Dismissed** |

**Rule 3 was broken three times in the same place.** The subnet conflict rule first used `NextHop`,
which missed covering routes; the fix then **failed a correct machine** on a full-tunnel VPN; the
version after that used string matching, which **missed overlaps whose text differs**, such as
`10.0.0.0/8`, and could not tell prefix lengths apart. **Only the fourth version** parses the
length, computes overlap both ways, and separates block, note and normal. A pass rule has to be checked both
ways: **a correct machine passes** and **a broken machine is caught.** Checking one direction
breaks the other. **It took four versions before the decision function was tried against 10
synthetic inputs first**, which is what rule 7 says to do from the start.

**Why round 31 was dismissed.** It does exist. On this machine (Windows 11 24H2),
`(Get-Command New-NetFirewallRule).Parameters.ContainsKey('InterfaceAlias')` returns `True`, and
`InterfaceType` and `LocalAddress` are present as well. The reviewer hedged with "need verify",
and verification shows the parameter is real.

**Why round 21 was dismissed.** It is factually wrong. `Test-Overlap` compares under the
**shorter** of the two masks, so `10.0.0.49/32` is cut to `/24`, giving `10.0.0.0`, which differs
from the target `10.100.0.0`. All 7 addresses on this machine were run through it and every one
returned `overlap=False`, with `BLOCK addresses` at `0`. The reviewer appears to have read the
`/32` length without applying the overlap test.

**Why round 8 was dismissed.** The review output is not committed. It is a temporary file, and
its content is this table. **`CLAUDE.md` asks for the review result to be recorded, and this table
is that record.** Committing the artefact as well would put the per-finding outcome in two places,
which breaks rule 5. The reviewer sees only the diff and cannot confirm the source of a record,
the same limit as the round 6 dismissal.

**Why round 6 was dismissed.** The evidence sits outside this diff, which is where it belongs. The
machine verification of the firewall test script happened in the previous commit `c48d9dd`, and
the procedure and results are in
[`2026-09-20/05-firewall-script-first-run.md`](../2026-09-20/05-firewall-script-first-run.md). The path was
already added to `CLAUDE.md` after the round 4 finding. **A single commit does not have to carry
the evidence for the whole repository again.** The reviewer sees only the diff, which is the limit
here.

**A substring check nearly read a stricter setting as normal.** `BlockInboundAlways` contains
`BlockInbound`. In that mode every inbound allow rule is ignored, so the step in section 2 and
option a of ADR 0002 do nothing. **Match a verdict string against the whole value.**

**A check command hid its verdict three times.** After fixing `Format-Table` cutting the verdict column
in the Java path check, the same thing happened in the subnet conflict check and then again in
the Elastic IP check: print a list, leave the verdict to a person. **Writing the pass rule in
prose does not make the command produce it.** The review caught all three; I caught none of them
first.

**The round 2 finding is the heaviest.** There was a habit of marking an item resolved once a
document existed. `design-audit.md` said `fixed` for sections that `windows-prereq.md` itself
marked `unverified`. **Each made sense until the two were put side by side.**

## What remains

| # | Work | State |
|---|------|-------|
| 4 | Fix the spec logic errors | Open. Document work |
| 6 | Walk back the over-strict criteria | Open. Document work |
| Measurement | The real firewall test | Open. It needs a peer and will be done when the EC2 instance is set up |

**The 5 unverified sections of `windows-prereq.md` get checked against real things in Phases 3, 6
and 8.** End-to-end verification on a clean PC belongs to the Phase 8 demo preparation and is not
a completion condition for this item.
