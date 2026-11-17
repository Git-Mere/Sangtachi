# Phase 9 verification without a partner and the first two field environments

## Why

The Phase 9 verifications in `roadmap.md` that need no other person were run first. The owner approved running them. As the owner decided, the field measurement covered same LAN and home network against mobile hotspot with `mere-galaxy` and this PC.

## Setup

| What | Value |
|------|-----|
| Build | Both are Release console builds of commit `5f9f710`. `SANGTACHI_TEST_BUILD` off (`build_kind` is `release`). This PC's build has a static CRT and no administrator manifest |
| Start | `--no-adapter --server <Elastic IP> --telemetry <Elastic IP>`. Connection setup and session values need no virtual adapter, and this runs without administrator rights |
| Roles | `mere-galaxy` is host and this PC is player, in every trial |
| Trial | Start both fresh, hold 30 seconds once connected, end with `quit`, then rest 60 seconds (longer than the 50 seconds between trials in the `roadmap.md` statistical design) |
| Records | Per-trial stdout, logs, and summaries are in `field-test/2026-11-17/`, ignored by git. The batches' UTC ranges are in `tools/tm-analysis/local/batches.txt` |

## Results: verification without a partner

| Verification | Result |
|------|------|
| Port 8001 (the two checks of `windows-prereq.md` section 6) | `ss -ltnp4` inside the instance shows it listening on port 8001 on all IPv4 addresses. `Test-NetConnection` on this PC gives `TcpTestSucceeded : True` |
| Only the telemetry service stopped (separation 4) | `verify.py` room creation, four joins, candidate registration, confirmation, and release gave `VERDICT: pass` |
| Only the control server stopped (separation 2) | Three test records (live room, absent room, no room) were all accepted with `ok` |
| Both services stopped together for about 613 seconds (21:58:25-22:08:38 UTC) (NFR-3) | The same-LAN tunnel did not break. On both sides the only state change after `CONNECTED` is the close by `quit` at the end, and there is no `TUNNEL_DROPPED`. RTT samples were 122 on the PC and 123 on `mere-galaxy`, matching the about 126 expected from a 5-second cycle over the about 630 connected seconds. Log lines carry no time, so the gaps between samples were not seen directly. The host's 121 control plane call failures and 10 upload failures per side are from the stop |
| Controlled failure (1) outbound UDP blocked | On `mere-galaxy`, that user's UDP was blocked except DNS (53) and it joined as player. `FAIL STUN_DISCOVERY_FAILED`. 0 upload failures. The rule was removed, with a safety that removes it after 240 seconds |
| Controlled failure (2) unresponsive endpoint | A fake host was made through the control plane API with one RFC 5737 documentation address as its candidate. When this PC joined, `FAIL HOLE_PUNCH_TIMEOUT`. 0 upload failures |
| Full queue | Covered by the client unit test `uploader: a full queue drops the new record and counts it`. Not re-checked in a real run |

## Results: field measurement

The RTT below is preliminary values from this PC's logs. The numbers in `experiments.md` use the values the analysis tool produces from the table.

| Environment | Result | Path used | RTT samples on this PC |
|------|------|---------|-------------------|
| Same LAN (two home Wi-Fi devices, same public IP) | 10/10 `CONNECTED` | Private address in all 10 | n=59, median 12.5ms, interquartile range 10.2-17.5ms |
| Home network (this PC) vs mobile hotspot (`mere-galaxy`, T-Mobile) | 10/10 `CONNECTED` | Public address in all 10 | n=58, median 111.0ms, interquartile range 95.4-132.9ms |

- Upload failures were 0 in both environments on both devices
- On joining the hotspot, Wi-Fi power saving on `mere-galaxy` was on again. It was turned off before the runs (issue 0001)

## Not verified

- Running the analysis tool and the table's `room_known` values. This PC has no AWS credentials, and the instance role has no `Query` on the telemetry table
- The three remaining field environments and the reproduction trial

## What changed

| What | Where |
|------|------|
| The next-work table. The three remaining environments, running the analysis tool, the reproduction trial | `plan.md` |
| Where the field builds and the trial script are | `plan.md` test device section |

## Verification

| What | Result |
|------|------|
| `docgate.py` | `VERDICT: pass` |

## Cross-model review

Codex (`gpt-6.1-sol`). Lenses: numbers against the raw output (A), Korean-English agreement (B), agreement with `plan.md` (C), public-repo exposure (D). The raw output was given as a local file that is not committed. After four rounds of fixes, the fifth gave `LGTM - no blockers`.

| Finding | Handling |
|------|------|
| [D] Two address literals | Applied. The documentation address is now "an RFC 5737 documentation address", and the bind check is "port 8001 on all IPv4 addresses" |
| [A] RTT sample counts alone do not show the 5-second cycle never broke | Applied. Now says log lines carry no time, and states only that the counts match the expectation and that the only state change is the `quit` at the end |
| [A] No evidence for 0 upload failures in the controlled failures and for the firewall safety | Applied. Added the counts and the `iptables` output to the raw data |
| [A] The stop lasted 613 seconds, not 609 | Applied. 609 was from a time taken after the stop command. Now uses the stop and restart times |
| [A] No `CLOSED` transition on the PC | The raw data had been read before `quit`. Added the lines read afterwards; the record is unchanged |
| [A] No evidence for the build settings or for power saving coming back on | Applied. Added the build scripts and the `iw` output of that time to the raw data |
| [A] nit. Mentions setup times not in the table | Applied. Removed the mention |
