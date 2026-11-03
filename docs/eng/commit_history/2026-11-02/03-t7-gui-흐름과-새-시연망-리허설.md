# GUI-only flow of T-7 and the rehearsal on the new demo networks

## Why

`spec.md` T-7 requires end to end with the GUI only. The host of the earlier three-machine measurement was a console. The owner ran it with a friend
on two GUI machines. The same day, a rehearsal was also done on the new demo networks of [ADR 0038](../../decisions/0038-put-the-demo-host-on-home-wi-fi.md).

## Results

[`experiments.md`](../../experiments.md) 3.4 and 4.2 own the results and limits. Summary:

| Item | Result |
|------|--------|
| GUI-only flow of T-7 | Pass. A created a room with the GUI and read the code, F joined with that code, both windows showed the other as Connected, F was removed from A's list when F closed the window, and F joined with a nonexistent room code and a failure code was shown |
| Rehearsal on the new demo networks | Pass. All three pairs between host G on home Wi-Fi and A (mobile hotspot), C, and F connected directly, and the three players entered Minecraft |
| C's first start | The subnet-overlap check passed the 30-second cap and it ended. Normal when started again. [Issue 0007](../../issue_history/0007-첫-기동의-서브넷-검사가-상한을-넘겼다.md) |

## What changed

| What | Where |
|------|-------|
| 3.4 rehearsal on the new demo networks. Split chapter 4 into 4.1 three machines and 4.2 GUI-only flow. Notation for machine `F` | `experiments.md` chapter 1, 3.4, chapter 4 |
| Timeout of the subnet check on the first start | Issue 0007 |
| Removed the remaining field tests | `field-test/required-test.md` |
| The demo and closing Phase 8 as the next items | `plan.md` |

## How it was measured

- T-7: A was on the mobile hotspot and F on the friend's home Wi-Fi. Both machines started the `field-kit-p8` GUI distribution by double-clicking.
  The evidence is the GUI logs of A and F, F's failure screenshot, and A's room screenshot. That each window showed the other as Connected is what
  the owner saw and heard from F
- Rehearsal: G was started over `ssh` on the owner's home Wi-Fi. G's standard error had the receive time attached to each line. The evidence is G's
  log and record file, the Minecraft server log, the GUI logs of A, C, and F, and A's screenshot showing four members
- The raw files are kept in `field-test/2026-11-02/` and not committed

## What happened during the work

- With the same two machines, A's Minecraft opened a single-player world to LAN and F came in at A's virtual IP and that port. It works without a separately started server
- In the rehearsal A joined with the same name as the host and received `name_taken` twice. That follows FR-16. A joined with another name
- The friend's Minecraft account was added to G's whitelist. After the rehearsal, G's host and Minecraft server were left running

## Verification

| What | Result |
|------|--------|
| Numbers | Medians and quartiles were computed with numpy's linear interpolation |
| `docgate.py`, `--claims` | `VERDICT: pass` |

## Cross-model review

Codex (`gpt-6.1-sol`). The evidence and contract lens (A) and the mirror and record consistency lens (B) were run together. One warn came from the diff with T-7 only, and two from the diff with the rehearsal added. All were accepted.

| Finding | Handling |
|---------|----------|
| [A] F receiving the roster is not evidence that A's name appeared in F's window | Applied. The owner checked with F and it is recorded as F's observation |
| [B] "The GUI first started that day" in Issue 0007 conflicts with the same-day starts in Issue 0006 | Applied. Changed to the first GUI started for the rehearsal, with the previous GUI having ended about 4 hours 46 minutes earlier. The hypothesis was adjusted to match |
| [B] `plan.md` says the rehearsal host was stopped and also that it was left running | Applied. Changed to describe what the scripts are for |

After the fixes, the whole staged diff was run again with the same lenses.
