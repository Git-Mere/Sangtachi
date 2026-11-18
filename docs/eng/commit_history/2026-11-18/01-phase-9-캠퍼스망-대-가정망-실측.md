# Phase 9 campus network vs home network field measurement

## Why

This is the campus network vs home network row of the `roadmap.md` Phase 9 test environment table. The owner asked for the run from school. This PC was on the school wired LAN and `mere-galaxy` was on home Wi-Fi.

## Setup

The trial method is the same as in the [2026-11-17 first field measurement](../2026-11-17/05-phase-9-상대-없는-검증과-첫-실측.md). Same build (Release console of commit `5f9f710`), same launch arguments, same trial script. Only the differences are listed.

| What | Value |
|------|-----|
| This PC's network | School wired LAN. It was also joined to the phone hotspot Wi-Fi. The default route was the wired LAN (route metric 25 vs 50). The public address both STUN servers returned was the wired LAN's |
| `mere-galaxy`'s network | Home Wi-Fi, Comcast. Wi-Fi power saving was off at the start and at the end (issue 0001). It was turned off before the start |
| Records | `field-test/2026-11-18/campus-home/`, ignored by git. The batch's UTC range is in `tools/tm-analysis/local/batches.txt` |

Right before the batch, the same two devices connected once. The host was the `~/sangtachi-head` build, and this PC joined with the field build and `--no-adapter`. It reached `CONNECTED` in 7.6 seconds. This attempt is outside the batch range, so the analysis tool may list it as an "attempt in no batch". The first trial started 4 seconds after it ended. The first trial used a new socket and got a different public port.

## Results

The RTT below is preliminary values from this PC's logs. The numbers in `experiments.md` use the values the analysis tool produces from the table.

| Environment | Result | Path used | RTT samples on this PC |
|------|------|---------|-------------------|
| Campus network (this PC, school wired LAN) vs home network (`mere-galaxy`, Comcast) | 10/10 `CONNECTED` | Public address in all 10 | n=57, median 21.2ms, interquartile range 18.0-28.6ms |

- Upload failures were 0 on both devices. The `telemetry_upload_failed` and `telemetry_queue_dropped` counters on both devices were 0 in every trial
- In every trial, both devices got the same public port from both STUN servers. Like the networks so far, the school NAT did not change its mapping by destination

## Trial script fix

Upload failures on `mere-galaxy` were counted with `grep -c telemetry.upload_failed`. The `.` matches any character, so it counted the counter lines (`telemetry_upload_failed value=0`) and gave 3 per trial. All counter values were 0. The script now reads the counter values. The counter values on `mere-galaxy` for the two 2026-11-17 batches were checked again. All were 0, so the "0 upload failures" in that record holds.

## Not verified

- Running the analysis tool. The owner runs it with `--save` in CloudShell

## What changed

| What | Where |
|------|------|
| Campus network vs home network removed from the remaining field work. Five batches, the mapping behavior so far | `plan.md` |

## Verification

| What | Result |
|------|------|
| `docgate.py` | `VERDICT: pass` |

## Cross-model review

Codex (`gpt-6.1-sol`). Lenses: numbers against the raw output (A), Korean-English agreement (B), agreement with `plan.md` (C), public repo exposure (D). The raw output was passed as uncommitted local files.

| Finding | Handling |
|------|------|
| [C] warn. "Both devices in every batch" in `plan.md` includes the controlled failure batches. One had failed STUN discovery and one had no host record | Applied. Narrowed to the three real-environment batches |

After the fix, the final staged diff was run again with the same lenses. `LGTM - no blockers`.
