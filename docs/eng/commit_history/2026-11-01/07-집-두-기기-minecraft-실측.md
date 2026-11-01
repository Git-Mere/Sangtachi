# Ran the Minecraft field test at home with two machines

## Why

The repository owner alone ran the part of the `required-test.md` Phase 8 field tests that two machines can do. The Windows machine (A) was the player
and the Linux machine `mere-galaxy` (G) the host. The owner got a second Minecraft account, so a client could run on G too.

## Results

[`experiments.md`](../../experiments.md) 3.2 Second Session holds the results and limits. A summary:

| Item | Result |
|------|------|
| File transfer (NFR-2) | Passed. 16 MiB, SHA-256 matched |
| 30 minutes of continuous play | Passed |
| Game-displayed latency and tunnel RTT | Passed. Median 72ms against 68.2ms. Median of the paired differences +3.3ms |
| Block placement and chat | Passed. Judged by the owner |
| Wi-Fi cut | Recorded. At 10 seconds the session and the game held. At 60 seconds the game dropped at about 20 seconds and the tunnel at about 50 seconds, and the player went to the lobby |

## What changed

| What | Where |
|------|-------|
| The Minecraft verification kit changes to `field-kit-p8`, and NFR-2 is kept by running the file transfer again. This reverses the work order of ADR 0034 | [ADR 0036](../../decisions/0036-phase-8-minecraft-verification-kit.md) |
| Results and limits of the second session, notation for machine `G` | `experiments.md` chapter 1, 3.2 |
| The kit table changes to `field-kit-p8`, and the four finished rows are removed | `field-test/required-test.md` |
| `mere-galaxy`'s Wi-Fi power saving turns back on when it joins another network too. Where the Linux kit build lives, the second whitelisted account | `plan.md` |
| Tag `field-kit-p8` on `3b92ced`. Local only | git |

## How it was measured

- Networks: G on the owner's mobile hotspot, A on home Wi-Fi. The two machines' public addresses differ and both sides are behind NAT
- Builds: G ran the Linux Release of `3b92ced` (`~/sangtachi-head`), A the GUI distribution built from `9c94d30`. The only difference between the two commits is one Linux platform file
- File transfer: on A, `tools/transfer/Test-Transfer.ps1 -Connect 10.100.0.1 -GenerateMiB 16`. G used a temporary Python receiver with the same wire. 16 MiB instead of 64 MiB was sent to save mobile data. The verdict is a hash match, so the size does not matter
- Game-displayed latency: on A, the Ping chart of `F3+3` was turned on in F3 and 21 screenshots were taken 6 to 13 seconds apart. The avg of each was read. The first 8 screenshots had the chart hidden and were discarded (`21 tx, 507 rx` is packets per second). The screenshots were deleted after the values were transcribed
- Tunnel RTT: each line of G's log was stamped with the time it was received, and the `session.rtt` lines in the screenshot window were cut out. A's log has no times
- Wi-Fi cut: on A a script called `netsh wlan disconnect` and `netsh wlan connect` at the set times and logged tunnel and internet `ping` and new GUI log lines at 0.05-second resolution. G logged `ping -D -i 0.5`. The two clocks were aligned with SNTP (A is about 0.6 seconds fast)

## What happened during the work

- `MERE7410`, given as the new account's name, did not exist at Mojang. The actual Java profile name `Mere_galaxy` came from the server log of a rejected join
- When G moved to the hotspot, Wi-Fi power saving was on again. It was turned off again
- Putting a `pkill -f` or `pgrep -f` pattern on the ssh command line also matched the shell running that command line, so ssh was cut first. This happened twice. After that the commands were sent as script files
- At cleanup, G's copy of standard error (`host.err`, with times stamped on the lines) stopped a few counter lines after `attempt.end reason=shutdown`. The pipe that stamped the times was ended along with the cleanup. The client's own log file runs through `adapter.close`, and the `Sangtachi` device was gone
- During the file transfer A wrote `socket.error op=sendto code=10035` 265 times. It is in the limits of `experiments.md` 3.2

## Verification

| What | Result |
|------|--------|
| Raw data | A's GUI log `20261101T182927Z-22896.log` and record file, G's `~/sangtachi-head/host.err` and record file, `~/mc-server/server.out`, the Wi-Fi cut script log. Not committed |
| Numbers | Medians and quartiles were computed with numpy's linear interpolation |
| `docgate.py`, `--claims` | `VERDICT: pass` |

## Cross-model review

Codex (`gpt-6.1-sol`). The evidence/contract (A) and Korean/English and record-consistency (B) lenses ran in one pass. Two warns were accepted.

| Finding | Disposition |
|------|------|
| [B] ADR 0036 defines both builds as made from one commit, but the measured Windows build is `9c94d30` and the Linux one `3b92ced` | Accepted. The definition is now "the sources that go into both builds equal the tagged commit", and this Windows distribution's difference is written. That the file transfer and Minecraft used the same two executables is written too |
| [A] `roadmap.md` asks for the median and interquartile range of the difference, but only the two distributions and the difference of medians were reported | Accepted. Each reading was paired with the tunnel sample nearest in time, and the difference's median +3.3ms and interquartile range +0.6~+8.1ms were added. The verdict is still the comparison of medians |

After the fixes, the full staged diff went through the same lenses again.
