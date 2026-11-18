# Phase 9 field measurement: home network vs home network and an emulated destination-dependent mapping

## Why

This is the home network vs home network row of the `roadmap.md` Phase 9 test environment table. The other side is the home network of the owner's friend. On the same day the owner said a real network for "other available NAT behaviors" is hard to find, so the decision to emulate a destination-dependent mapping ([ADR 0040](../../decisions/0040-emulated-destination-dependent-mapping.md)) was made and that measurement was also run.

## Setup

The trial method is the same as in the [2026-11-17 first field measurement](../2026-11-17/05-phase-9-상대-없는-검증과-첫-실측.md). Only the differences are listed.

| What | Value |
|------|-----|
| Roles | The friend's PC is the host, `mere-galaxy` (home Wi-Fi, Comcast) is the player |
| Host | The friend started the same field build (Windows Release console of commit `5f9f710`, `--no-adapter`) once and kept it for all 10 trials. Only the player was started fresh for each trial. The rest between trials is the same 60 seconds |
| What the friend got | A zip with the executable, `host.cmd` that starts the host, and instructions. Outside the repo |
| Trial script | `.prime/p9-field/field_join.py`, ignored by git. It starts only the player with an existing room code |
| `mere-galaxy` | Wi-Fi power saving was turned off before the start (issue 0001) |
| Records | `field-test/2026-11-18/home-home/`, ignored by git. The batch's UTC range is in `tools/tm-analysis/local/batches.txt` |

**Keeping the host running changes the condition for setup time.** Before the batch, `mere-galaxy` was started once as a host and this PC joined it twice, 60 seconds apart. Both reached `CONNECTED`, and the time to the result seen by the trial script was 11.2 and 6.6 seconds. This batch's setup time is not compared with other batches, where the host was started fresh for each trial. Success and RTT are used as they are.

## Results

The RTT below is preliminary values from `mere-galaxy`'s logs. The numbers in `experiments.md` use the values the analysis tool produces from the table.

| Environment | Result | Path used | RTT samples on `mere-galaxy` |
|------|------|---------|---------------------------|
| Home network (`mere-galaxy`, Comcast) vs the friend's home network | 10/10 `CONNECTED` | Public address in all 10 | n=60, median 20.8ms, interquartile range 17.7-29.0ms |

- Upload failures were 0 on both devices. The friend's standard error log, local record file, and `logs\` copy were received and placed in `field-test/2026-11-18/home-home/friend/`, ignored by git. On the friend's side `telemetry_upload_failed` and `telemetry_queue_dropped` are 0, with 10 `CONNECTED` and 10 end lines
- Both devices got the same public port from both STUN servers. The friend's host ran STUN only once
- The host setup time in the friend's local record is n=10, median 6187.5ms (5953-6250). It is close to the host-side median (about 6.2 seconds) of the two 2026-11-17 batches, where the host was started fresh for each trial. The host-side starting point is when the player first appears in `peers`, so it seems less affected by keeping the host running, but this was not verified

## Emulation measurement

As in ADR 0040, a network namespace and `MASQUERADE --random-fully` were set up on `mere-galaxy`. Outside the namespace the two STUN servers saw the same port; inside, they differed every time. This PC (school wired LAN) is the host and the client in the namespace is the player. Both were started fresh for each trial. The trial script is `.prime/p9-field/field_sym.py`, ignored by git. The records are in `field-test/2026-11-18/emu-dep-map/`, and the batch kind is `controlled`.

| Environment | Result | The player's two STUN ports |
|------|------|-------------------------|
| Emulated destination-dependent mapping (`mere-galaxy`) vs campus network (this PC) | 10/10 `HOLE_PUNCH_TIMEOUT` | Different in all 10 |

- Both sides went from `PUNCHING` to `FAILED`. In all 10 trials the per-peer receive counters (`rx_bytes`, `rx_datagrams`) on both sides were 0. No packet from the other side arrived in either direction. Process-wide, each side received 3 datagrams (132 bytes), which by size are STUN responses. The player-side counters were saved as `galaxy-rx-counters.txt`
- The player registered both STUN ports as candidates. `--random-fully` picks a new port per destination, so the mapping used during the punch was most likely a port different from both. That port was not observed directly
- Upload failures were 0 on both devices
- As in ADR 0040, this result is not included in the real-environment success rate

Before this measurement, two trials ran with the same setup and were stopped by the owner's decision. Both were `HOLE_PUNCH_TIMEOUT`. They are not in the batch file, so the analysis tool lists them as attempts in no batch. The records are in `field-test/2026-11-18/emu-dep-map-aborted/`.

After the test the namespace and the rules were removed, and the STUN ports outside the namespace were the same again.

## Not verified

- Running the analysis tool. The owner runs it with `--save` in CloudShell

## What changed

| What | Where |
|------|------|
| The emulation decision | [ADR 0040](../../decisions/0040-emulated-destination-dependent-mapping.md) |
| ADR 0040 linked from "other available NAT behaviors" in the test environment table | `roadmap.md` |
| Emulation added to `controlled` in the batch file | `tools/tm-analysis/README.md` |
| Home network vs home network removed from the remaining field work. Seven batches, the emulation result | `plan.md` |

## Verification

| What | Result |
|------|------|
| `docgate.py` | `VERDICT: pass` |

## Cross-model review

Codex (`gpt-6.1-sol`). Lenses: numbers against the raw output (A), Korean-English agreement (B), agreement with ADR 0001, `roadmap.md`, `spec.md`, the analysis tool README and `plan.md` (C), public repo exposure (D). The raw output was passed as uncommitted local files.

| Finding | Handling |
|------|------|
| [A] warn. The player-side receive counters were not in the local raw files, so "each side" and the two-way inference went beyond the evidence | Applied. The player-side counters for all 10 trials were saved, and the sentence now states that the per-peer receive counters on both sides were 0 |

After the fix, the final staged diff was run again with the same lenses. `LGTM - no blockers`.
