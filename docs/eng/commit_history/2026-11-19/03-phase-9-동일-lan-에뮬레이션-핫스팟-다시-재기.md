# Phase 9 same LAN, destination-dependent mapping emulation, and home network vs hotspot, measured again

## Why

Following [the earlier record of the same day](02-phase-9-캠퍼스망과-핫스팟-다시-재기.md), the Phase 9 batches were measured again with the build that fixed the loss accounting ([ADR 0041](../../decisions/0041-loss-accounting-starts-at-peer-connected-signal.md)). The owner asked for same LAN and the emulation from home. The home vs hotspot batch of the earlier record was slow, and the owner asked to measure it again.

## Setup

The trial method is the same as in the [2026-11-17 first field measurement](../2026-11-17/05-phase-9-상대-없는-검증과-첫-실측.md). The build is the same Release console of commit `6e6f5e4` as in the earlier record. Only the differences are listed.

| Batch | This PC | `mere-galaxy` | Roles |
|------|-------|---------------|------|
| Same LAN | Home Wi-Fi | Home Wi-Fi. Both devices had the same public address | `mere-galaxy` is the host |
| Emulation | The owner's phone hotspot, T-Mobile. The hotspot was the only default route | A network namespace on home Wi-Fi ([ADR 0040](../../decisions/0040-emulated-destination-dependent-mapping.md)) | This PC is the host, the client in the namespace is the player |
| Home vs hotspot | Home Wi-Fi | The owner's phone hotspot, T-Mobile | `mere-galaxy` is the host |

- `mere-galaxy`'s Wi-Fi power saving was off in all three batches (issue 0001). It was turned off again after joining the hotspot
- `batches.txt` lists the batches as `same-lan-1119`, `emu-dep-map-1119` (kind `controlled`), and `home-hotspot-1119b`
- Records are `same-lan/`, `emu-dep-map/`, `home-hotspot-b/` under `field-test/2026-11-19/`, ignored by git

**The emulation's peer network differs from 2026-11-18.** Then this PC was on the school wired LAN. This time it was at home, so it joined the hotspot. On home Wi-Fi, this PC and `mere-galaxy` are behind the same router. The player could then reach this PC's private address candidate without going through the router, which changes the condition being measured.

**The home vs hotspot arrangement is the same as 2026-11-17.** This PC is on the home network and `mere-galaxy` on the hotspot. The batch of the earlier record was the reverse.

## Results

The RTT below are preliminary values read from this PC's logs. The numbers in `experiments.md` use the values the analysis tool produces from the table.

| Batch | Result | Path used | This PC's RTT samples |
|------|------|---------|-------------------|
| Same LAN | 10/10 `CONNECTED` | Private address in all 10 | n=60, median 15.4ms, IQR 9.7-34.0ms |
| Home (this PC) vs hotspot (`mere-galaxy`) | 10/10 `CONNECTED` | Public address in all 10 | n=56, median 92.4ms, IQR 64.2-114.8ms |
| Emulated destination-dependent mapping (`mere-galaxy`) vs hotspot (this PC) | 10/10 `HOLE_PUNCH_TIMEOUT` | None | None |

- In same LAN and home vs hotspot, both devices got the same public port from both STUN servers in every trial
- There is one upload failure. In trial 4 of home vs hotspot, `mere-galaxy` failed to upload the `establish` record right after `CONNECTED` (`reason=connect_timeout`). That trial's `establish` record is likely missing from the table. All other trials and this PC's side are 0

### Emulation

- Before the start, the two STUN servers returned the same port both times outside the namespace and different ports all three times inside
- In all 10 trials the player's two STUN ports differed
- Both sides went from `PUNCHING` to `FAILED`. In all 10 trials the per-peer `rx_datagrams` on both sides is 0 and there is no `session.endpoint`. Process-wide, both sides received 3
- Upload failures are 0 on both devices
- The result is the same as 2026-11-18. Per ADR 0040, it is not counted in the real-environment success rate
- After the test, the namespace and rules were removed, and the STUN ports outside the namespace were confirmed to match again

### The earlier record's home vs hotspot batch is excluded from the analysis

The earlier record's `home-hotspot-1119` had a median RTT of 200.7ms. The owner said they were watching video on the phone providing the hotspot during that batch. The remeasured batch is 92.4ms, close to the 2026-11-17 batch (111.0ms). Because the hotspot link was shared with traffic outside the test, that batch was removed from `batches.txt` by the owner's decision. Its record folder is kept. It was not confirmed that the video was the cause. The two batches also differ in place and in which device was on the hotspot.

## Not verified

- Running the analysis tool. The owner runs it with `--save` in CloudShell
- The remaining batch, home vs home

## What changed

| What | Where |
|------|------|
| Reduced the table of batches to measure again to home vs home only | `field-test/required-test.md` |

## Verification

| What | Result |
|------|------|
| `docgate.py` | `VERDICT: pass` |

## Cross-model review

Codex (`gpt-6.1-sol`). Lenses: raw output and number check (A), Korean and English parity (B), consistency with `plan.md` and `required-test.md` (C), public repo exposure (D). The raw output was passed as an uncommitted local file. The first run returned `LGTM - no blockers`. After filling this section, the final staged diff was run again with the same lenses. `LGTM - no blockers`.
