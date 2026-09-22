# 2026-09-21 Tool READMEs

READMEs were added for `tools/docgate` and `tools/winprereq`, and
`tools/nat-probe/README.md` was shortened. They sit outside the document tree, so the mirror
convention and the document gate do not cover them. They are kept in Korean only.

## Changes

| File | Lines |
|------|-------|
| `tools/docgate/README.md` | 38 (new) |
| `tools/winprereq/README.md` | 46 (new) |
| `tools/nat-probe/README.md` | 515 -> 144 |

The stale `plan.md` chapter references in the header comment of `natprobe.py` and in `.gitignore`
were fixed as well.

## Decisions

**A tool is its own source.** What left the nat-probe README is the full JSON schema, the long
run procedure and the record-writing procedure. Those live in the comments of `natprobe.py`, in
`--help`, and in `RECORD-TEMPLATE.md`. The README points there.

**What must survive shortening is any sentence that stops a wrong measurement from being believed.**
That is the criterion, not the line count.

## Where this went wrong

**Cutting to 90 lines dropped four kinds of safety and validity warning.** The cross-model review
found one more of them in each round.

| Round | What was dropped | If left out |
|:--:|------------------|-------------|
| 1 | Allow the Windows firewall prompt | A `failure` measured with the prompt refused is read as NAT behaviour |
| 1 | The separate-home-networks requirement | Two PCs behind one router become evidence for blocker 20 |
| 1 | The `--peer` exposure warning | The peer public address lands in shell history and process listings |
| 1 | The persistent firewall rule warning | After an abnormal exit an inbound port stays open |
| 4 | `--peer` skips the Enter synchronisation | Following the README produces an invalid run with mismatched start times |
| 5 | Remove path pollution | A run measured over an active VPN is recorded as a home-network measurement |
| 5 | Record `socket.behind_nat` | **A run where one side is not behind a NAT is reported as NAT to NAT** |

The last one actually happened once. In one measurement one side's local address was also its
public address, and it was noticed only afterwards. The shortened validity check, "do the two
public IPs differ", **passes that case**.

**The cause was reading "be concise" as a line-count target.** The list of what must survive
should have come first. Deleting first and letting the review restore things cost five extra
rounds.

The line count grew 90 -> 123 -> 135 -> 144. That is still a little over a quarter of the
original. **When brevity and a safety warning collide, safety wins.** A bad measurement does not
produce a wrong result; it produces a wrong result that is believed.

## Verification

- `python tools/docgate/docgate.py`: `VERDICT: pass`
- `test_docgate` 109 OK, `test_natprobe` 151/151 passed
- Every flag, default, exit code, verdict string and case count in the new READMEs was checked
  against the actual scripts
- Dead references to `plan.md` were swept with no extension filter. There are none left

## Cross-model review

Codex. 6 rounds. 13 accepted, 1 dismissed, final `LGTM`.

| Round | Representative finding | Verdict |
|:--:|------------------------|---------|
| 1 | 4 safety and validity warnings, missing usage lines, and the full-path self-contradiction in the docgate README (8 findings) | **Accepted** |
| 2 | The README did not say that `results/` and `records/` are covered by `.gitignore` | **Accepted** |
| 3 | "only `unknown` needs a re-run" misses the other invalid measurements | **Accepted** |
| 4 | There is no warning that `--peer` skips the Enter synchronisation | **Accepted** |
| 4 | A warning that both peers must run the same `natprobe.py` version is missing | **Dismissed.** No such warning exists in the 515-line original or anywhere in the tool. Adding it would put an unverified claim in the document |
| 5 | Removing path pollution and checking `socket.behind_nat` are missing | **Accepted** |
| 6 | None | `LGTM` |

**The dismissed finding was invented by the reviewer.** It is plausible and technically sensible,
and it has no basis. Adding a warning that does not exist is the same class of defect as removing
one that does.
