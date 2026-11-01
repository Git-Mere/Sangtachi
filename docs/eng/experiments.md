# Experiments

**This document holds measured results and their limits.** [`roadmap.md`](roadmap.md) owns what is measured when and the pass
conditions. [`field-test/required-test.md`](../../field-test/required-test.md) owns the measurements a human still has to do. The raw logs
are not published.

## 1. Notation

- Networks are written as pseudonyms. Public addresses and ports are not included. Times are written only to the date. The rationale is [ADR 0001](decisions/0001-no-direct-connection-fallback.md)
- Machines are written as `A`, `B`, and `G`. `A` is the repository owner's Windows machine and `G` the owner's Linux machine (`linux-prereq.md`)
- Tunnel RTT is the `session.rtt` line of the client log ([`architecture.md`](architecture.md) chapter 9 Log Output). It is the `PING`/`PONG` round trip
- Loss is the `session.loss` line of the log. It is the value counted by the receiving side
- Medians and quartiles are computed by sorting the samples and interpolating linearly

## 2. Adapter MTU

The measurement that fixed the adapter MTU 1400 of [`protocol.md`](protocol.md) chapter 12.

| Item | Value |
|------|-----|
| Path | One path between two homes' Wi-Fi and home routers |
| Inner maximum size | `ping -f -l 1372` succeeded 4/4. `-l 1373` was refused because of DF. 1372 is the adapter MTU 1400 minus the IPv4 header 20 and the ICMP header 8 |
| Tunnel maximum size | Console `send 1 1452` arrived from B to A as one outer IPv4 packet of 1500 bytes. There were no IP fragments in the whole capture |

**Paths this value does not cover.**

- The maximum size was seen in only one direction (B to A) of one path
- The outer packet is at most 1448 bytes. If the path MTU is smaller than that, it goes as IP fragments because DF is not set. Whether fragments pass middleboxes and NATs was not measured
- Runs over a VPN and enterprise networks were not measured. Mobile tethering carried the chapter 3 session below, but its maximum size was not measured separately

## 3. Minecraft Session

### 3.1 First Session

The record of the first run of part of the [`roadmap.md`](roadmap.md) Phase 8 verification. The build is `field-kit-p7b`, the same tunnel build as the Phase 7 file transfer test.

| Item | Value |
|------|-----|
| Date | 2026-10-28 |
| Networks | A on network P (mobile hotspot), B on network L (school library wired LAN). The configuration of network L is unknown |
| Roles | A was the host and ran the Minecraft server. B was the player |
| NAT | On both sides the local address and the public address differed. On both sides the two STUN servers saw the same public endpoint |
| Establishment | Succeeded. Establishment time A 6484ms, B 7016ms (`setup_ms` of the local record file) |
| Session length | About 3 minutes 51 seconds from entering `CONNECTED` to `CLOSED`. Both sides ended by ending the process (`attempt.end reason=shutdown`) |
| Tunnel RTT | A 46 samples, median 82.4ms, interquartile range 65.7~126.9ms. B 45 samples, median 83.1ms, interquartile range 59.5~99.1ms |
| RTT maximum | Once on both sides at the same time, about 2.3 seconds. The other samples were below 300ms |
| Loss | Received by A 34/17303 (0.20%), received by B 25/18000 (0.14%) |
| Traffic | On the tunnel socket, A sent about 12.1MB and A received about 1.1MB |
| Human observation | Login and world entry succeeded. No delay or stutter was felt during play |

**What this session does not judge.**

- 30 minutes of continuous play (T-5). The session is shorter than 4 minutes
- The difference between the game-displayed latency and the tunnel RTT. The game-displayed value was not recorded
- The propagation time of block placement and chat
- The cause of the RTT of about 2.3 seconds. There is no data to tell which of the two networks it was

### 3.2 Second Session

The record of the part of the `roadmap.md` Phase 8 verification that two machines can do, run in one session. The owner ran it alone with A and G.

| Item | Value |
|------|-----|
| Date | 2026-11-01 |
| Build | `field-kit-p8` ([ADR 0036](decisions/0036-phase-8-minecraft-verification-kit.md)). G ran the Linux Release console and A the GUI distribution. A's distribution was built from the commit just before; the only difference between them is one Linux platform file |
| Networks | G on network P (mobile hotspot), A on network H (home Wi-Fi) |
| Roles | G was the host and ran the Minecraft server and a client. A was the player |
| NAT | On both sides the local address and the public address differed. On both sides the two STUN servers saw the same public endpoint |
| Establishment | Succeeded. Establishment time G 6266ms, A 7172ms (`setup_ms` of the local record file) |
| Session length | About 41 minutes 17 seconds from entering `CONNECTED`. Both sides ended with `TUNNEL_DROPPED` from the 60-second Wi-Fi cut below |
| Tunnel RTT | G 480 samples, median 83.1ms, interquartile range 66.8~144.0ms, maximum about 3.5 seconds. A 470 samples, median 80.1ms, interquartile range 64.3~136.2ms, maximum about 1.2 seconds |
| Loss | Received by G 2927/155959 (1.88%), received by A 222/124680 (0.18%). Includes what was lost during the two Wi-Fi cuts |

**Results.**

| Verification | Result |
|------|------|
| File transfer (NFR-2) | Passed. Before Minecraft, with the same build, 16 MiB went from A to G and both SHA-256 values matched. The receiver was a Python receiver with the same wire as `Test-Transfer.ps1` |
| 30 minutes of continuous play (T-5) | Passed. For about 34 minutes after both accounts joined, the server log had no disconnect and both sessions were `CONNECTED`. Then the Wi-Fi cut test followed |
| Game-displayed latency and tunnel RTT | Passed. Game-displayed latency 21 readings, median 72ms, interquartile range 71~74ms. Tunnel RTT over the same window of about 2 minutes 46 seconds, 33 samples, median 68.2ms, interquartile range 64.3~78.7ms. The verdict compares the two medians. The difference (game − tunnel) has median +3.3ms and interquartile range +0.6~+8.1ms. Each reading was paired with the single tunnel sample nearest in time (at most 2.1 seconds apart) |
| Block placement and chat | Passed. The owner saw them appear on both screens without noticeable delay |

**Wi-Fi cut behavior.** Recorded, not judged. A's Wi-Fi connection was dropped with `netsh wlan disconnect` and reconnected after the set time.
The connection was dropped; the radio was not turned off. Times were aligned by comparing both clocks against the same NTP server.

| Cut | Behavior |
|-----------|------|
| About 10 seconds | `ping` from G to A stopped for about 10.8 seconds. On A the tunnel came back about 0.7 seconds after reconnecting. Both sessions stayed `CONNECTED`. Minecraft paused briefly and then continued |
| About 60 seconds | About 20 seconds in, the Minecraft server dropped A's player with `Timed out` and A's screen showed a disconnect. About 50 seconds after the cut both sides wrote `session.failed code=TUNNEL_DROPPED`, matching the 50-second idle timeout ([`protocol.md`](protocol.md) chapter 11 Timers). A's GUI went to the lobby and showed `FAIL TUNNEL_DROPPED galaxyhost The connection to the other player was lost. They may have left, or the network is unstable.`. G stayed in the room. It does not rejoin after Wi-Fi returns; the player has to join again |

**Limits of this session.**

- The game-displayed latency was read as the avg of the min/avg/max that F3's Ping chart shows. It is an average over the chart window, not an instantaneous value, and the windows of neighboring readings overlap, so they are not independent
- The tunnel RTT window was cut from G's log. A's log lines carry no times. It is the round trip of the same session
- During the file transfer A wrote `socket.error op=sendto code=10035` (`WSAEWOULDBLOCK`) 265 times. The send buffer was full and those datagrams were dropped (`tx_err_send` 266), and TCP resent them. There were none during Minecraft play
- One session and one network pair. It is not the demo network pair (a mobile hotspot at school and the demo player's home)
