# Phase 9 campus network vs home network and home network vs hotspot, measured again with the fixed loss accounting

## Why

[ADR 0041](../../decisions/0041-loss-accounting-starts-at-peer-connected-signal.md) fixed the loss accounting. `field-test/required-test.md` says to measure the Phase 9 batches again with this build. The owner asked for campus network vs home network and home network vs hotspot from school.

## Setup

The trial method is the same as in the [2026-11-17 first field measurement](../2026-11-17/05-phase-9-상대-없는-검증과-첫-실측.md). Same launch arguments, same trial script. Only the differences are listed.

| What | Value |
|------|-----|
| Build | Release console of commit `6e6f5e4` on both. This PC rebuilt the `Release-p9` build tree. `mere-galaxy` replaced its source with `git archive` and rebuilt |
| `mere-galaxy`'s network | Home Wi-Fi, Comcast, in all three batches. Wi-Fi power saving was off (issue 0001) |
| This PC's network, campus vs home | School wired LAN. It was also joined to the hotspot Wi-Fi, and the default route was the wired LAN (route metric 25 vs 50). The public address was the same as in the 2026-11-18 batch |
| This PC's network, home vs hotspot | The owner's phone hotspot, T-Mobile. The wired LAN was unplugged, so the hotspot was the only default route |
| Roles | `mere-galaxy` is the host and this PC is the player. In home vs hotspot, the hotspot side is the reverse of 2026-11-17. Then `mere-galaxy` was on the hotspot |
| Batch names | `batches.txt` lists two `campus-home-1119` batches and `home-hotspot-1119`. The names are separate so they do not mix with the batches of the old accounting |
| Records | `campus-home/`, `campus-home2/`, `home-hotspot/` under `field-test/2026-11-19/`, ignored by git |

## Results

The RTT below are preliminary values read from this PC's logs. The numbers in `experiments.md` use the values the analysis tool produces from the table.

| Batch | Result | Path used | This PC's RTT samples |
|------|------|---------|-------------------|
| Campus vs home, first | 9/10 `CONNECTED`, 1 `HOLE_PUNCH_TIMEOUT` | Public address in all 9 connected trials | n=54, median 22.1ms, IQR 19.0-27.2ms |
| Campus vs home, second | 10/10 `CONNECTED` | Public address in all 10 | n=58, median 23.7ms, IQR 18.8-34.2ms |
| Home (`mere-galaxy`) vs hotspot (this PC) | 10/10 `CONNECTED` | Public address in all 10 | n=55, median 200.7ms, IQR 147.0-362.8ms |

- Upload failures are 0 in all three batches on both devices. The `telemetry_upload_failed` counter of both devices is 0 in every trial
- `mere-galaxy` got the same public port from both STUN servers in every trial. So did this PC, except in one trial

### Trial 5 of the first campus vs home batch

- This PC got different public ports from the two STUN servers. Cloudflare returned `32885` and Google `32884`. The school NAT may have given a different mapping per destination, or it may have changed the mapping between the two requests. These records cannot tell which
- Both devices went from `PUNCHING` to `FAILED`. Neither device has a `session.endpoint`. Neither confirmed any of the peer's candidate addresses. `rx_datagrams` is 3 on both devices
- The other 19 trials on the same network and the 10 trials of the 2026-11-18 batch got the same port from both servers
- This is a failure on a real network. It is handled separately from the destination-dependent mapping emulation ([ADR 0040](../../decisions/0040-emulated-destination-dependent-mapping.md))

### RTT of home vs hotspot

It is higher and wider than the 2026-11-17 batch (median 111.0ms, IQR 95.4-132.9ms). The place, the device on the hotspot, and the time of day differ from that batch. The owner said they were watching video on the phone during the batch. These records cannot tell which one is the cause.

## Not verified

- Running the analysis tool. The owner runs it with `--save` in CloudShell
- The three remaining batches. Same LAN, home vs home, and the destination-dependent mapping emulation

## What changed

| What | Where |
|------|------|
| Removed campus vs home and home vs hotspot from the table of batches to measure again | `field-test/required-test.md` |

## Verification

| What | Result |
|------|------|
| `docgate.py` | `VERDICT: pass` |

## Cross-model review

Codex (`gpt-6.1-sol`). Lenses: raw output and number check (A), Korean and English parity (B), consistency with `plan.md` and `required-test.md` (C), public repo exposure (D). The raw output was passed as an uncommitted local file.

| Finding | Disposition |
|------|------|
| [A] No evidence for `mere-galaxy`'s final state and receive counter in trial 5. That no datagram came from the peer was not observed | Fixed. Added `mere-galaxy`'s per-trial records to the raw file. Narrowed the sentence to: neither device has a `session.endpoint` |
| [A] The only evidence that `mere-galaxy` got the same port in every trial is trial 5 | Fixed. Added the STUN results of all 30 trials to the raw file |
| [A] Different ports from two servers do not alone prove destination-dependent mapping. The mapping may have changed between the two requests | Fixed. Both explanations are written, with a note that the records cannot tell which |
| [A] No evidence for the build commit and settings, or for the route metrics of the campus batches | Fixed. Added both devices' build settings, the source check, and the route table to the raw file |
| [A] The owner's statement about watching video is not in the raw file | Fixed. Copied the owner's words into the raw file verbatim |

After the fixes, the final staged diff was run again with the same lenses. `LGTM - no blockers`.
