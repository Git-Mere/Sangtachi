# Phase 9 home network vs home network measured again, and analysis of the re-measured batches

## Why

This is the last batch of measuring the Phase 9 batches again with the build that fixed the loss accounting in [ADR 0041](../../decisions/0041-loss-accounting-starts-at-peer-connected-signal.md). The other batches were measured again up to [the 2026-11-19 record](../2026-11-19/03-phase-9-동일-lan-에뮬레이션-핫스팟-다시-재기.md). After that, the owner ran the analysis tool with `--save` in AWS CloudShell and handed over the result.

## Setup

The trial method is the same as in [the 2026-11-18 home network vs home network](../2026-11-18/03-phase-9-가정망-대-가정망-실측과-에뮬레이션-결정.md). Only the differences are listed.

| What | Value |
|------|-----|
| Build | Both are the Windows Release console of commit `6e6f5e4`. The executable in the zip given to the friend has the same SHA-256 as this PC's `Release-p9` executable |
| Roles | The friend's PC (the friend's home network) is the host, this PC (home Wi-Fi) is the player. On 2026-11-18 `mere-galaxy` was the player. This PC was used by the owner's decision |
| This PC's network | Before the trials, `ipconfig` showed a private address on the Wi-Fi adapter. The AS of the public address the trial script logged is Comcast |
| Host | The friend started it once and kept it for all 10 trials. Only the player was started fresh each trial. For the same reason as on 2026-11-18, this batch's setup time is not compared with batches that started the host fresh each trial |
| Trial script | `.prime/p9-field/field_pc_join.py`, ignored by git. It runs the same procedure as `field_join.py` directly on this PC |
| Tailscale | It was on on this PC |
| Records | `field-test/2026-11-23/home-home/`, ignored by git. The friend's standard error log, local record file, and `logs\` copy are under `friend/` there. The batch name is `home-home-1123` |

## Results

The RTT below are preliminary values read from this PC's logs.

| Environment | Result | Path used | This PC's RTT samples |
|------|------|---------|-------------------|
| Home network (this PC) vs the friend's home network | 10/10 `CONNECTED` | Public address in all 10 | n=60, median 25.3ms, interquartile range 19.3~63.1ms |

- Upload failures are 0 on both devices. The friend's `telemetry_queue_dropped` is 0 too
- All `session.loss` lines on both devices are `lost=0`. This PC has 20 lines, the friend 11
- This PC got the same public port from both STUN servers in all 10 trials. The friend's host ran STUN once and both servers gave the same port
- In the friend's local record, all 10 `establish` are `OK` and all 10 `end` are `CLOSED`

## Analysis of the re-measured batches

Run with `--save` in CloudShell. 460 records. Running the same records again with `--load` on this PC gave the same output except line endings (CRLF).

**The batches with the old accounting were removed from the batch file.** This was the owner's decision. They are `same-lan`, `home-hotspot`, `campus-home`, `home-home`, and `emu-dep-map` of 2026-11-17 and 18. Keeping them would mix old losses into the combined real environments. `ctl-punch` and `ctl-stun` were not measured again, so they stay. The file before the change is in `tools/tm-analysis/local/batches-before-1123.txt`, ignored by git.

133 attempts fell in no batch.

| What | Count |
|------|------|
| The five removed batches with the old accounting | 100 |
| `home-hotspot-1119`, removed on 2026-11-19 | 20 |
| Checks outside batches on 2026-11-18. The same 9 as in that day's analysis record | 9 |
| Two rooms before the first campus batch on 2026-11-19. One host and one player each, all `OK`. What they were was not confirmed from the records | 4 |

| Environment | Kind | Successes/attempts | Player setup time median | Host setup time median | RTT median (interquartile range) | Jitter median | Loss |
|------|------|-----------|---------------------------|-------------------------|--------------------------|-------------|------|
| Campus network vs home network (`campus-home-1119`) | Real | 19/20, Wilson 95% 76.4~99.1% | 10797.0ms | 6170.0ms | 23.5ms (19.0~33.2), n=225 | 3.1ms | 1/623 (0.16%) |
| Same LAN (`same-lan-1119`) | Real | 10/10, Wilson 95% 72.2~100.0% | 10641.0ms | 6015.5ms | 18.7ms (10.5~40.6), n=120 | 9.6ms | 0/330 (0.00%) |
| Home network vs hotspot (`home-hotspot-1119b`) | Real | 10/10, Wilson 95% 72.2~100.0% | 10110.0ms | 6259.0ms, n=9 | 79.7ms (65.9~106.4), n=113 | 8.6ms | 0/314 (0.00%) |
| Home network vs home network (`home-home-1123`) | Real | 10/10, Wilson 95% 72.2~100.0% | 10945.5ms | 6188.0ms | 25.8ms (18.7~57.7), n=114 | 7.8ms | 0/315 (0.00%) |
| Real environments combined | Real | 49/50, Wilson 95% 89.5~99.6% | 10750.0ms | 6188.0ms | 27.9ms (18.9~65.8), n=572 | 6.6ms | 1/1582 (0.06%) |
| Emulated destination-dependent mapping (`emu-dep-map-1119`) | Controlled | 0/10, Wilson 95% 0.0~27.8%. `HOLE_PUNCH_TIMEOUT` 10 | - | - | - | - | - |
| Unresponsive endpoint (`ctl-punch`) | Controlled | 0/1. `HOLE_PUNCH_TIMEOUT` | - | - | - | - | - |
| Outbound UDP blocked (`ctl-stun`) | Controlled | 0/1. `STUN_DISCOVERY_FAILED` | - | - | - | - | - |

- In [the 2026-11-18 analysis](../2026-11-18/04-phase-9-오늘-세-묶음-분석.md) with the old accounting, the three real environments other than same LAN all had a loss of 8. In the re-measured batches the only loss is the 1 in campus network vs home network
- The one failure in campus network vs home network has the player's `stun_same_endpoint` `false` and the host of the same room `true`. It is the batch's only failure, trial 5 in [the 2026-11-19 record](../2026-11-19/02-phase-9-캠퍼스망과-핫스팟-다시-재기.md)
- In home network vs hotspot, the host setup time n=9, the one "no pair" in the paired mapping table, and the one gap seen as an empty seq match `mere-galaxy` (the host) failing to upload `establish` in trial 4 of [the 2026-11-19 record](../2026-11-19/03-phase-9-동일-lan-에뮬레이션-핫스팟-다시-재기.md)
- The player setup time median of home network vs home network is 10945.5ms. The 2026-11-18 batch with the old accounting was 9206.5ms. Both batches kept the host running. The reason was not examined
- The CloudWatch 1-minute resolution values for 11-19 and 11-23 were not fetched

## What changed

| What | Where |
|------|------|
| Removed the section on Phase 9 batches to measure again | `field-test/required-test.md` |
| Removed the re-measurement row from the next tasks. Changed the note of the `experiments.md` row to the analysis of the re-measured batches | `plan.md` |

## Verification

| What | Result |
|------|------|
| `docgate.py` | `VERDICT: pass` |

## Cross-model review

Codex (`gpt-6.1-sol`). Lenses: raw output and number check (A), Korean/English parity (B), consistency with `plan.md`, `required-test.md`, `roadmap.md`, and the analysis tool README (C), public repo exposure (D). The raw output was handed over as an uncommitted local file.

| Finding | Handling |
|------|------|
| [C] warn. The record says the setup time of a batch that kept the host is not compared, then compared it with the other three environments | Accepted. Removed that comparison and kept only the comparison with the 2026-11-18 batch, which also kept the host |
| [A] warn. No evidence that `field_pc_join.py` runs the same procedure as `field_join.py`, or for the record locations | Accepted. Added the difference between the two scripts, the listing of the record folder, and the git ignore check to the raw material |
| [A] warn. No evidence that this PC was on home Wi-Fi, Comcast, and on the same network as `mere-galaxy` | Accepted. Added `ipconfig` and the AS of the public address to the raw material and wrote only that in the setup table. The same-network claim could not be confirmed and was removed |

After the fixes, the final staged diff was run again with the same lenses. `LGTM - no blockers`.
