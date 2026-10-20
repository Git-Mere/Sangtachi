# Phase 7 two-machine field procedure and kit `field-kit-p7`

## Why

The remaining Phase 7 checks need two machines. The repository owner asked whether the procedure was in `field-test`, and it was not. The owner
decided to run them on the same day as the remaining Phase 4~5 measurements (tests 1 and 5) with one kit. This commit changes only the procedure
and the kit. The owner will say when the measurements happen.

## What changed

| What | Where |
|------|------|
| Kit moved to `field-kit-p7`. Table of files in the kit. `--no-adapter` for tests 1, 2 and 5. Test 6 runs in an elevated cmd, with the command to remove the rules afterwards, and cannot run on a PC whose network overlaps | `field-test/README.md` Setup |
| Test 6. Checking other block rules, startup and capture, virtual IP `ping`, file transfer, maximum-size packets and MTU, byte counters against the capture, files to collect | `field-test/README.md` |
| Kit table, next step 4, what to do with the results 4 | `field-test/required-test.md` |
| Measurement boundary (process lifetime, the peer exits first) and sample condition of the capture comparison table | `roadmap.md` Phase 7 |
| Remaining work reduced to the two-machine verification | `plan.md` |

## Important decisions

- **The measurement boundary is the whole lifetime of the measuring client process.** `roadmap.md` left this cell empty with "decide it when
  writing the comparison procedure". Log lines carry no time and keepalive and `PING` never stop, so cutting in the middle leaves no way to align
  the counters with the capture. The counters start at 0 and the full dump comes out once more at exit, so the lifetime is an interval that needs
  no cutting. Shown to the repository owner as a proposal and approved
- **The peer exits first.** A review caught this as a `blocker`. If the measuring side exits first, the peer's keepalives arrive after its last
  receive and are left only in the capture
- **Tests 1, 2 and 5 use `--no-adapter`.** It is the same path as up to Phase 5, so those tests' verdicts (`drop_no_sink` on the receiving
  side and so on) stay. No administrator rights are needed
- **6-5 runs in a different process from 6-1~6-4.** Mixing in the 64 MiB transfer makes the capture large and can cause loss

## Verification

- `python tools/docgate/docgate.py` passed
- The tshark sum commands of 6-5 were run on A's capture from test 1 on 2026-10-14 (`t1.pcapng`, local port 53014). They gave
  `capture tx=3847 rx=3244` and 115 sent datagrams. That build had no byte counters, so nothing was compared
- The 6-0 block rule query was run on this machine with Windows PowerShell and pwsh. Both printed lines, and the block rule found targets `Unity.exe`, so it is outside the pass condition
- **Not run.** The rest of test 6. It needs two machines

## Cross-model review

Codex was asked to read the diff as an executable procedure. Three findings. All were applied.

| # | Finding | Disposition |
|---|------|------|
| 1 | `blocker`. If A exits first, B's keepalives arrive after A's last receive and stay in the capture. Zero loss and zero receive errors do not filter this, so a correct implementation fails | Applied. B exits first and A exits 5 seconds later. Also written into the `roadmap.md` measurement boundary (mirror included) |
| 2 | `warn`. The block rule check misses blocks targeting `powershell.exe` and ranges containing 25565 | Applied. The pass condition is split into three |
| 3 | `warn`. The count ladder stops at 5,000. `roadmap.md` goes to 50,000 | Applied. 1,000, 5,000, 10,000, 50,000 in turn, choosing the step before the first loss |

The diff with the repairs and the records was shown to Codex once more. Two findings, both `warn`. Both were applied.

| # | Finding | Disposition |
|---|------|------|
| 4 | The 6-0 command prints the multi-valued `LocalPort` with `-f`, giving `System.String[]`. Ranges and lists are hidden and the block rule check can pass wrongly | Applied. Protocol, port and program are joined with commas |
| 5 | Rerunning the ladder overwrites `a-udp-*.txt` and `a-dumpcap.txt`, losing the valid-interval evidence of the chosen step | Applied. The evidence files get the count suffix too, and the collection table was fixed |
