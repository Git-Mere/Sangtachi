# Experiments

**This document holds measured results and their limits.** [`roadmap.md`](roadmap.md) owns what is measured when and the pass
conditions. [`field-test/required-test.md`](../../field-test/required-test.md) owns the measurements a human still has to do. The raw logs
are not published.

## 1. Notation

- Networks are written as pseudonyms. Public addresses and ports are not included. Times are written only to the date. The rationale is [ADR 0001](decisions/0001-no-direct-connection-fallback.md)
- Machines are written as `A` and `B`. `A` is the repository owner's machine
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
