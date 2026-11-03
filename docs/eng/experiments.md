# Experiments

**This document holds measured results and their limits.** [`roadmap.md`](roadmap.md) owns what is measured when and the pass
conditions. [`field-test/required-test.md`](../../field-test/required-test.md) owns the measurements a human still has to do. The raw logs
are not published.

## 1. Notation

- Networks are written as pseudonyms. Public addresses and ports are not included. Times are written only to the date. The rationale is [ADR 0001](decisions/0001-no-direct-connection-fallback.md)
- Machines are written as `A`, `B`, `C`, and `G`. `A` is the repository owner's Windows machine, `G` the owner's Linux machine (`linux-prereq.md`), and `C` the demo player's Windows machine
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

### 3.3 Demo Rehearsal

The pre-demo rehearsal of the `roadmap.md` Phase 8 tasks ([ADR 0037](decisions/0037-one-rehearsal-before-the-demo.md)). It is the same network pair as the demo.

| Item | Value |
|------|-----|
| Date | 2026-11-02 |
| Build | `field-kit-p8`. The same two executables as 3.2. C used the same GUI distribution as A. The kit did not change, so the file transfer was not run again ([ADR 0036](decisions/0036-phase-8-minecraft-verification-kit.md)) |
| Networks | G on network P (the owner's mobile hotspot at school), C on network R (the demo player's home Wi-Fi). A was on network S (school wired LAN) and on the Wi-Fi of network P at the same time |
| Roles | G was the host and ran the Minecraft server. C and A were players |
| NAT | For both G and C the local and public addresses differed and the port changed too. On both sides the two STUN servers saw the same public endpoint |
| Setup | Succeeded. Setup time of the G–C session: G 6159ms, C 9204ms (`setup_ms` of the local record file) |
| Session length | About 10 minutes 14 seconds from entering `CONNECTED` for the G–C session. It ended `CLOSED` when C closed the window |
| Tunnel RTT | G 118 samples, median 199.0ms, interquartile range 192.3–209.3ms, max 319.0ms. C 122 samples, median 198.9ms, interquartile range 191.2–207.1ms, max 330.7ms |
| Loss | Received by G 0/29106, received by C 7/27069 (0.03%) |
| Human observation | C and A logged in to the Minecraft server and entered the world. C stayed about 5 minutes |

**Result.** The rehearsal passed. Creating a room, joining, and connecting to Minecraft all worked.

**Limits of this session.**

- The network state on the demo day is not judged. If conditions change in between, it first shows in the demo (ADR 0037)
- It is not 30 minutes of continuous play. C played about 5 minutes
- While A was on two networks at once, G switched A's endpoint between two addresses 27 times in about the first minute (`session.endpoint`). After that it stayed on one, and the loss on A's session was 9/33717. The cause was not examined

## 4. Member List and Disconnection

T-7 of the `roadmap.md` Phase 8 verification. Same day, machines, and networks as 3.3.

| Item | Value |
|------|-----|
| Date | 2026-11-02 |
| Roles | G is the host (Linux console), C the player watching the member list (GUI), and A the player that is disconnected (GUI) |
| Deadline | The time the host learns of the disconnection plus one roster period (`roadmap.md` Phase 8 verification) |
| Times | The time the host learned is the receive time attached to G's log lines. C's log lines carry no times. The time C received the roster was bounded to a 5-second window by counting the 5-second `PING` timer lines after `CONNECTED` in C's log. For the same setup, the record-file times of C and G differed by 0.02 seconds |

| How A is disconnected | Host | C's member list | Verdict |
|-----------------------|------|-----------------|---------|
| Close A's window | Receives `CLOSE`, `CLOSED` | Within 1.9 seconds after the host learned, A changed to disconnected (connection bit 0). The line was removed in the next roster. A person saw it as "almost immediately" | Pass |
| End A with `taskkill /F` | `session.failed code=TUNNEL_DROPPED` on the idle timeout | Within 5 seconds after the host learned, A changed to disconnected. A person saw about 47 seconds after the kill | Pass |
| End the host | Normal shutdown | `CLOSED` 0.12 seconds later, `attempt.end reason=closed`. The window returned to the lobby | Pass |

**The five actions of FR-15.** Each action was done with the GUI. They were not done as one continuous flow. The host of this measurement was a console started from the command line, so "end to end with the GUI only" of `spec.md` T-7 is not judged yet.

| Action | Where it was done |
|--------|-------------------|
| Create a room and show the room code | The same day A's GUI created a room, and G and another player joined with that code |
| Join | The GUIs of C and A |
| Member list and connection status | C's GUI. The table above |
| Show the failure code | In 3.2 A's GUI showed `FAIL TUNNEL_DROPPED` |
| Exit by closing the window | The first row of the table above |

**Limits of this measurement.**

- The first attempt, ended from the "Processes" tab of Task Manager, sent a close signal to the window and took the normal shutdown path. It is not counted
- The 47 seconds of the forced kill were counted by a person from the kill. The kill time is not in any log
