# Phase 9 analysis of today's three batches

## Why

Today's three batches (campus network vs home network, home network vs home network, and the emulated destination-dependent mapping) were run through the analysis tool. The owner ran `tools/tm-analysis` with `--save` in AWS CloudShell and passed on the result. This PC and the instance role cannot read the telemetry table.

## What was done

| What | Result |
|------|------|
| Analysis tool | 196 records, seven batches. Running the same records again on this PC with `--load` gave the same output apart from line endings (CRLF) |
| 9 attempts in no batch | All are checks done outside the batches. The first join on the school wired LAN 1 (player only), the check that joined one host twice 4, the two stopped emulation trials 4 |
| CloudWatch 1-minute values | Ten metrics over a range covering today's three batches, saved in `tools/tm-analysis/local/cw-2026-11-18-1min.json`, ignored by git. The `Attempts` total of 35 is the three batches in that range (30) plus the player attempts outside batches (5) |

The SSH used to fetch the CloudWatch values got a different host key on the school wired LAN. That is [issue 0008](../../issue_history/0008-학교-유선-lan-에서-ssh-호스트-키가-달랐다.md). The values were fetched through `mere-galaxy`.

## Analysis results

| Environment | Kind | Success/attempts | Player setup time median | Host setup time median | RTT median (interquartile range) | Jitter median | Loss |
|------|------|-----------|---------------------------|-------------------------|--------------------------|-------------|------|
| Campus network vs home network | Real | 10/10, Wilson 95% 72.2-100.0% | 10859.0ms | 6200.5ms | 21.2ms (17.8-28.7), n=115 | 2.3ms | 8/355 (2.25%) |
| Home network vs home network | Real | 10/10, Wilson 95% 72.2-100.0% | 9206.5ms | 6187.5ms | 20.8ms (17.3-26.6), n=120 | 2.6ms | 8/363 (2.20%) |
| Real environments combined (four batches) | Real | 40/40, Wilson 95% 91.2-100.0% | 10476.5ms | 6198.0ms | 21.9ms (15.6-80.8), n=471 | 2.7ms | 24/1427 (1.68%) |
| Emulated destination-dependent mapping | Controlled | 0/10, Wilson 95% 0.0-27.8% | - | - | - | - | - |

- In the paired mapping table, for all four real environments both the player and the host in the same room have `stun_same_endpoint` `true`. For the emulation the player is `false`, the host `true`, and it is 0/10
- The player setup time of home network vs home network (9206.5ms) is shorter than the other three real environments (10250.5-10859.0ms). Only that batch kept the host running. The host setup time is about 6.2 seconds in all four
- Loss is 0 for the same LAN and exactly 8 for each of the other three real environments. Why batches with similar expected counts got the same number was not examined
- The analysis tool's `controlled` section is titled "통제된 실패 (실패 처리 경로의 증거. 실패율이 아니다)" (controlled failures, evidence of the failure handling path, not a failure rate). The emulation is also in that section. When it goes into `experiments.md`, the emulation is reported apart from the controlled failures (ADR 0040)

## What changed

| What | Where |
|------|------|
| The analysis tool output covers seven batches. Where the 1-minute values are | `plan.md` next work |
| Tailscale on the school wired LAN worked today. SSH to the instance goes through `mere-galaxy` | `plan.md` test device section |
| The SSH host key on the school wired LAN | [Issue 0008](../../issue_history/0008-학교-유선-lan-에서-ssh-호스트-키가-달랐다.md) |

## Verification

| What | Result |
|------|------|
| `docgate.py` | `VERDICT: pass` |

## Cross-model review

Codex (`gpt-6.1-sol`). Lenses: numbers against the raw output (A), Korean-English agreement (B), agreement with `plan.md` and the issue README (C), public repo exposure (D). The raw output was passed as uncommitted local files.

| Finding | Handling |
|------|------|
| [D] blocker. Minute-level times of the CloudWatch fetch range | Applied. Written as "a range covering today's three batches" |
| [D] blocker. A minute-level time in the `plan.md` analysis row | Applied. Only the date is kept |
| [C] warn. A different key alone does not confirm that something other than the instance answered | Applied. The confirmed part is limited to the key differing by path, and who answered moved to the inferred part |
| [C] warn. The hypothesis in the issue's investigation table still said "a different server answers" | Applied. Changed to "the key received depends on the path" |

After the fix, the final staged diff was run again with the same lenses. `LGTM - no blockers`.
