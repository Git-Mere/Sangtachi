# 2026-09-20 Blocker 20 Resolved, and 2 ADRs

## Changes

22 measurements resolved blocker 20 from `design-audit.md`, and a design problem found along the
way was applied to `protocol.md`. **These are the first 2 ADRs in `decisions/`.**

- Added `decisions/0001-no-direct-connection-fallback.md` (with the Korean original)
- Added `decisions/0002-no-rebinding-recovery.md` (with the Korean original)
- Added the premise and the measured limit of path validation to `protocol.md` 10.4, with a
  caveat on one of the three reasons for choosing learning
- Rewrote `plan.md` 5.7-5.9. Follow-up 5 moves from `todo` to **done**
- Updated blocker 20 and the chapter 5 tracking table in `design-audit.md`
- Added "re-measure right before the demo" to the Phase 8 tasks in `roadmap.md`
- Added `tools/nat-probe/results/**` and `records/**` to `.gitignore`
- Added `classify_nat()` to `natprobe.py`, so it decides for itself whether it is behind a NAT
- Self-checks went from 55 to 105

## Measurement results

| Item | Value |
|------|-----|
| Measurements | 22 |
| Distinct networks | **7** (3 T-Mobile, 3 Comcast, 1 KT) |
| Mapping verdict | **`endpoint-independent` in all 22** |
| Punch result | **`success` in all 22** |
| Symmetric NAT | **0** |
| Network pairs attempted | 6. 0 failures |

Required test topology (2), "2 different home networks", succeeded 3 times in a row on
2026-09-18.

## Decision 1: no fallback is added

Options A (NAT emulation), B (promoting the relay to P1), and C (making the hotspot mandatory)
from `plan.md` chapter 5 were **all rejected**, and M-1 and M-4 stay exactly as written. All three
carried the same cost, that the success criteria would have to be revised with them, and the
measurements satisfied the criteria, so that cost no longer needs paying.

**Deciding not to revise is also a decision, so the reasoning was recorded.** Revising the
criteria would state a weaker claim than what was actually achieved.

Four accepted risks are written into the ADR. Two of them matter most.

- **No single measurement satisfied all three conditions (NAT-NAT + different ISP +
  Windows-Windows) at once.** The combination is covered, but it was not proven within one
  measurement
- **There is no fallback for the fallback.** If conditions change on demo day, nothing remains.
  That is why a re-measurement right before the demo was added to the Phase 8 preparation

## Decision 2: rebinding recovery is not guaranteed

**This was not an expected finding.** It surfaced while investigating why no Windows firewall
prompt appeared in the 09-20 measurement even though all inbound traffic arrived.

The default inbound action is `Block` and there is no `python.exe` rule, yet the punch succeeded.
**Stateful UDP** explains it: because we sent first, the replies counted as solicited traffic.
Other explanations were not excluded. That left packets **from a source port never sent to**
untested.

An `--unsolicited` test was built and run on 3 pairs. **All 6 cases were `blocked`.**

| Host OS | Host firewall | Result |
|-----------|---------------|------|
| Windows 11 | Inbound default `Block`, no `python.exe` rule | `blocked` |
| macOS | Application Firewall off by default | `blocked` |
| Linux | `/etc/ufw/ufw.conf` says `ENABLED=no` (checked directly) | `blocked` |

**It was blocked even on hosts believed to have no active firewall filtering, so the most likely
cause is port-restricted filtering in the NAT.** The macOS case is inferred and only `ufw` was checked on Linux, so it is
not stated as certain. The client cannot fix it either way, so the decision is the same.

That breaks the premise of path validation in `protocol.md` 10.4 (c). The procedure begins with
"when a packet arrives from an address not in the candidate set", and that packet never arrives.
Of the three reasons 10.4 chose learning over nomination, **NAT rebinding recovery is invalidated
by measurement.**

**(c) was not deleted.** It is still needed for hairpin behind the same NAT and for switching
inside the candidate set, and it keeps its security role against forged source addresses. It may
work normally on a NAT that uses address-restricted filtering. **This says "not guaranteed", not
"impossible".**

**The client will not register firewall rules. This is provisional.** The NAT is the likely
cause, so it would have no effect. As a result no administrator rights requirement appears, and
the "without manual port forwarding" claim in `spec.md` stands. **Without this finding, a
requirement that does not exist would have been designed in.**

However, **the firewall is not excluded as the cause for directions where our Windows host is the
receiver.** Only the directions with a macOS or Linux peer narrow to the NAT. A revisit condition
is written into ADR 0002: adding one narrowly scoped temporary inbound allow rule and re-running
`--unsolicited` settles it.

## The raw measurements are not committed

`tools/nat-probe/results/` and `records/` were added to `.gitignore`. This repository is public,
and the JSON and records carry the public IPs of two households with minute-level timestamps.
Publishing a friend's home IP cannot be undone and was not agreed to.

Attachments are not the raw files. A submission system and shared links create the same exposure. **Replace public addresses with irreversible pseudonyms (`network A`, `network B`) and keep timestamps only to the date.** Masking the last IPv4 octet is not enough and does not apply to IPv6. Otherwise obtain explicit consent from the people involved. State in the report which was done.

**Cost:** the raw data does not follow the repository to another machine. The verdicts and the
reasoning stay in `plan.md` 5.7-5.9 and in the 2 ADRs, so the conclusions are not lost.

**`.gitignore` alone is not enough.** Ignore rules do not protect copies that are already tracked
or already in history. This was checked.

```
git log --all --diff-filter=A --name-only --format='' | grep -E 'results/|records/|\.json$'
  -> tools/nat-probe/records/.gitkeep
     tools/nat-probe/results/.gitkeep
```

**No measurement file has ever entered git history.** The only tracked files are those 2
`.gitkeep` placeholders.


## Verification

- `test_natprobe.py` passes 105/105. 50 cases added for `classify_nat` (private local address,
  local equals public, same IP with a different port, 464XLAT `192.0.0.0/29`, CGNAT
  `100.64.0.0/10`, no STUN response, observed values differing per server, unparseable address,
  `0.0.0.0`, loopback, malformed observed value, only one observed value malformed, observed port
  out of range, local port 0 and non-numeric, and differing IPv6 spellings)
- Every figure in the 4 records was checked against the raw JSON. No mismatch
- Heading counts match between eng and kor (protocol 43, plan 10, design-audit 18, roadmap 45,
  spec 15, architecture 31)
- Both ADRs have 5 headings in each language
- Every relative link except `experiments.md` resolves to a real path

## Cross-model review

- Reviewer: Codex (GPT). **19 rounds,** run until the result came back clean
- Result: 1 blocker, 52 warns, 4 nits. **All applied, 1 rejected** (a false positive caused by
  splitting the review scope)

| Round | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 | 16 | 17 | 18 | 19 |
|--------|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|
| Findings | 3 | 6 | 4 | 5 | 3 | 3 | 5 | 4 | 2 | 4 | 2 | 3 | 1 | 2 | 2 | 1 | 3 | 2 | 1 |
| Code defects | 0 | 1 | 2 | 2 | 1 | 2 | 2 | 1 | 0 | 2 | 2 | 1 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |

**The findings that mattered.**

| Finding | Content |
|------|------|
| blocker (round 2) | The cause was softened to "likely", but the rejection of option a, which depends on that cause, was left absolute. If peer host filtering were the cause, the client on that host could register a rule and fix it, so "cannot be fixed" was wrong. Option a became **deferred**, with an explicit revisit condition |
| `run_punch` defect | Every kind that was not `PING` fell through to the `PONG` branch. Adding 3 kinds broke that premise while the comment stayed. A peer finishing its punch first corrupts the metrics |
| Verdict logic defect | `peer_ran` was derived from received probes only, so the case where **both sides are blocked** (exactly the `blocked` we were looking for) came out as `unknown`. One real measurement was misjudged because of this |
| `is_private` / `is_global` | Standard library predicates were trusted wrongly 3 times. `is_private` includes loopback plus documentation and benchmarking ranges, and `is_global` is **true for multicast**. Both were eventually replaced with an allow list and `_is_usable_public()` |
| A gap in the spec | The text claimed automatic recovery after the idle timeout, but `protocol.md` 9.6 only says a retry starts from `IDLE`; **it never says who starts it or when.** Writing the ADR exposed a hole in the documents |
| Overstated conclusion | It said that still being blocked with a temporary allow rule means the firewall is not the cause. **An explicit block rule takes precedence over an allow rule,** so only the default inbound policy is excluded |

**Two recurring patterns.**

1. **Softening a claim in one place requires softening everything that leaned on it, and one or
   two places were missed every time.** The same claim was spread across `protocol.md`,
   `plan.md`, the ADRs, and the commit history in both languages: 8 places. It only improved
   after switching to a `grep` sweep. This broke recurrence-prevention rule 4 in `design-audit.md`
   chapter 6 again
2. **The PowerShell procedure added in round 13 accounted for 9 of the following 10 findings.**
   Putting an executable procedure into a document means that procedure gets reviewed like code:
   input validation, guaranteed cleanup, privilege scope, and result interpretation all became
   findings

## Remaining

| Item | Content |
|------|------|
| Follow-up 3 | The Windows prerequisites section. **No firewall rule registration item is added** (decision 2) |
| Follow-up 4 | The spec logic errors (blockers 18 and 19) |
| Follow-up 6 | Loosening the over-tightened verification criteria |
| Phase 4 | Measure mapping lifetime during the keepalive work, and whether the 15s interval prevents rebinding |
| `experiments.md` | A Phase 9 artifact. Still the only broken link |
| `31e4240` | The CMake smoke commit still has no commit history entry |

**The code is still at Phase 1 not started.** All of this work is the diagnostic tool and the
documents.
