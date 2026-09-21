# 2026-09-20 First run of the firewall test script

## Changes

`unsolicited-firewall-test.ps1` was run on Windows for the first time. **It did not even parse.**

- Added a UTF-8 BOM to `tools/nat-probe/unsolicited-firewall-test.ps1`
- Added 5 regression cases to `tools/nat-probe/test_natprobe.py` (146 -> **151 on Windows**,
  149 on Linux). The last two are Windows-only: one finds `powershell`, the other calls the real
  parser.
  The `145` written in `NEXT-ON-WINDOWS.md` was stale; the real starting count was 146
- `tools/nat-probe/README.md` section 12: updated the run status, stated that every trial needs
  two manual inputs
- Updated `tools/nat-probe/NEXT-ON-WINDOWS.md`

## Blocker - the script could not run

Windows PowerShell 5.1 reads a `.ps1` without a BOM **in the ANSI code page.** On Korean Windows
that is CP949. Every Korean comment and string stored as UTF-8 is mangled, and the mangled bytes
split quotes and braces, producing 10 parse errors.

```
Unexpected token '??젣媛' in expression or statement.
Missing closing '}' in statement block or type definition.
The string is missing the terminator: ".
```

The parser rejects the whole file before the first line runs. **The `-Trials` validation, the
administrator check and the mutex never execute.** PowerShell 7 reads UTF-8 without a BOM by
default, so it does not have this problem. Testing only with `pwsh` would have hidden it. The
target of this repo is PowerShell 5.1, the default shell of Windows 10/11.

With the 3 BOM bytes the file parses and Korean prints correctly. PowerShell 7 also reads the BOM.

**The prediction was right.** The previous commit left this sentence.

> Of the 6 blockers in this commit, 3 were "cannot run in a normal state", and all were caught by
> static review alone. A single run would have surfaced them on the first line.

Seventeen rounds of static review missed it. The reviewers and I only read the contents of the
file. Nobody asked **what reads the file, and in which encoding.** One run caught it on the first
line.

## Verification

| Step | Method | Result |
|------|--------|--------|
| Parser | `[Parser]::ParseFile` (PS 5.1) | 10 errors before the fix -> 0 after, 1299 tokens |
| Non-admin run | `-Label smoke -Trials 2` | Mutex and path checks pass, stops at the administrator check as intended. Korean messages render correctly |
| `Get-TestRuleSafe` | Function extracted and called under StrictMode | Empty result 0 (no abort), existing rule 1, query against a missing store aborts |
| Admin run (fake peer) | Ran with `192.0.2.1:47000` as the peer | One control trial completed. In the rule trial the rule was created and all 4 `ActiveStore` checks passed (Enabled/Action/Direction, port filter, profile, address scope) |
| Cleanup on the error path | After that run aborted on the `natprobe` exit code | `finally` removed the rule. Both stores were then queried directly: 0 leftover rules, 0 allow rules on UDP 47000 |
| Admin run (structure) | Replaced `natprobe` with a stub that succeeds immediately, `-Trials 2` | All 4 trials completed, order reversal confirmed (pair 1 control->rule, pair 2 rule->control), labels match the rule, `no leftover rules` printed, exit code 0 |
| Regression cases | Removed the BOM and ran `test_natprobe.py` | **2 of 151 failed** (the BOM check and the parser check) -> 151/151 after restoring. The cases really decide |

The stub run checks structure without measuring. It takes 40 seconds to confirm that the script
runs to the end before a real measurement. The one result JSON produced with the fake peer was
deleted.

## Not fixed

**Every trial asks for the peer endpoint and an Enter by hand.** With the default 8 runs that is
16 inputs. Passing `--peer` to `natprobe` would remove it, but **the script does not pass it.**
The previous commit spent five rounds removing the peer address from the command line, from
`Read-Host` and from transcripts. `--peer` would put that address straight back into the process
list and the PowerShell history. The inconvenience is what that decision costs. It is written in
README 12.1 so it is known before a measurement starts.

## Cross-model review

- Reviewer: Codex (GPT). Two lenses (correctness/security/robustness, and claim checking),
  **8 rounds.** The last round ran against the final diff, this table included, and both lenses
  were clean
- Result: 0 blockers, 10 warns, 2 nits. **2 dismissed, the rest applied**

| Round | Finding | Action |
|:--:|------|--------|
| 1 | The regression only checks BOM presence and UTF-8 decoding, so it passes even if PowerShell 5.1 still cannot parse the script | Applied. Added a case that calls the real parser |
| 1 | `145` in `NEXT-ON-WINDOWS.md` disagrees with `146` in the commit record | Applied. Stated that 145 was stale |
| 1 | "148/149 실패" reads as 148 failures | Applied. Changed to "2 of 151 failed" |
| 1 | The input count reads as a contradiction (nit) | Applied. Stated that `-Trials` counts pairs and trials are twice that |
| 2 | The parser case is skipped silently when `powershell` is not found | Applied. Not finding it is itself a failing case |
| 3 | The default in `NEXT-ON-WINDOWS.md` disagrees with README and the commit record | **Dismissed** |
| 4 | The dismissal above contradicts itself | Applied. Rewrote it to separate pairs from trials as two units |
| 6 | "the last two call the real parser" is stronger than the diff; one case only looks for `powershell` (3 files) | Applied. Wrote out what each of the two does |
| 6 | An unbalanced bold marker (nit) | Applied |
| 7 | The `../../docs/...` link in `NEXT-ON-WINDOWS.md` resolves under `tools/docs` | **Dismissed** |

**The first finding is rule 7 again.** Checking for a BOM diagnoses one cause; the decision is
"does it parse". A diagnosis was put in the decision's place. It now calls the real parser, and
removing the BOM makes that case fail.

**Why round 7 was dismissed.** The link is right. The file sits in `tools/nat-probe/`, so `../../`
is the repo root and `../../docs/kor/...` is `docs/kor/...`. All 13 relative links in the four
documents this change touches were resolved and every target exists. The reviewer itself hedged
with "if verified".

**Why round 3 was dismissed.** There is no disagreement. **There are two units and both are right.**
The `-Trials` option counts **pairs** and its default is 4. The prose "8 runs" counts **trials**.
Four pairs are 8 trials, so both describe the same setting. The reviewer read the two as one unit
and saw 4 against 8. The mix is easy to trip over, which is why `NEXT-ON-WINDOWS.md` already says
that `-Trials` counts pairs and trials are twice that.

## What remains

The 2 measurements are unchanged. **Both need a peer.**

| # | What | State |
|---|------|-------|
| 1 | The real firewall test (8 runs on each side) | Waiting. ADR 0002 stays **provisional** |
| 2 | Windows x NAT<->NAT x a different ISP, 3 times | Waiting |

The script is confirmed to run. What remains is arranging a peer.
