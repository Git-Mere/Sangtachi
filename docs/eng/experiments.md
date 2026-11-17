# Experiments

**This document holds measured results and their limits.** [`roadmap.md`](roadmap.md) owns what is measured when and the pass
conditions. [`field-test/required-test.md`](../../field-test/required-test.md) owns the measurements a human still has to do. The raw logs
are not published.

## 1. Notation

- Networks are written as pseudonyms. Public addresses and ports are not included. Times are written only to the date. The rationale is [ADR 0001](decisions/0001-no-direct-connection-fallback.md)
- Machines are written as `A`, `B`, `C`, `F`, and `G`. `A` is the repository owner's Windows machine, `G` the owner's Linux machine (`linux-prereq.md`), `C` the demo player's Windows machine, and `F` the Windows machine of the owner's friend
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
- One session and one network pair. It is not the network pair between the demo player and the host. These two networks are used, with roles swapped, between the host and the owner in the demo of [ADR 0038](decisions/0038-put-the-demo-host-on-home-wi-fi.md)

### 3.3 Demo Rehearsal

The pre-demo rehearsal of the `roadmap.md` Phase 8 tasks ([ADR 0037](decisions/0037-one-rehearsal-before-the-demo.md)). It is the demo network pair set at that time. The demo networks changed afterwards ([ADR 0038](decisions/0038-put-the-demo-host-on-home-wi-fi.md)).

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

### 3.4 Rehearsal on the New Demo Networks

The rehearsal on the demo networks of [ADR 0038](decisions/0038-put-the-demo-host-on-home-wi-fi.md).

| Item | Value |
|------|-----|
| Date | 2026-11-02 |
| Build | `field-kit-p8`. The same executables as 3.3 |
| Networks | G on network H (the owner's home Wi-Fi), A on network P (mobile hotspot), C on network R, F on network W |
| Roles | G was the host and ran the Minecraft server. A, C, and F were players that joined with the GUI |
| Setup | Direct connection on all three pairs. Setup time at the host: F 5978ms, A 6654ms, C 6082ms (`setup_ms` of the local record file) |
| Session length | About 8 to 11 minutes. All three players ended with leave or by closing the window, and the host received `CLOSED` |
| Member list | All four members appeared as Connected in A's window. The rosters of C and F also carried all four members with connection bit 1 |
| Human observation | The three players logged in to the Minecraft server and entered the world. The server log has no player dropped by a timeout |

| Player | Tunnel RTT, G side | Tunnel RTT, player side | Loss, received by G | Loss, received by the player |
|--------|--------------------|-------------------------|---------------------|------------------------------|
| F | 129 samples, median 17.9ms, interquartile range 14.8–24.1ms, max 194.1ms | 129 samples, median 18.1ms, interquartile range 14.4–24.2ms, max 246.8ms | 0/42042 | 41/43426 (0.09%) |
| A | 117 samples, median 117.9ms, interquartile range 93.4–209.6ms, max 866.7ms | 119 samples, median 131.3ms, interquartile range 101.4–231.1ms, max 894.8ms | 35/42686 (0.08%) | 650/37610 (1.73%) |
| C | 98 samples, median 181.2ms, interquartile range 172.4–212.1ms, max 321.5ms | 98 samples, median 185.4ms, interquartile range 171.3–235.8ms, max 320.2ms | 1/18604 | 1/17637 |

**Result.** The rehearsal passed. Creating a room, joining, and connecting to Minecraft all worked.

**Limits of this session.**

- The network state on the demo day is not judged (ADR 0037)
- A's mobile hotspot at school had the widest RTT spread and the most loss. The signal on the demo day may differ
- Just before C entered Minecraft, G wrote `socket.error op=sendto code=11` (`EAGAIN`) 40 times. The send buffer was full and those datagrams were dropped. It is the same kind as `10035` in 3.2
- C's first start ended with a timeout of the subnet-overlap check ([Issue 0007](issue_history/0007-첫-기동의-서브넷-검사가-상한을-넘겼다.md)). C joined with the process started again

## 4. Member List and Disconnection

T-7 of the `roadmap.md` Phase 8 verification.

### 4.1 Three Machines

Same day, machines, and networks as 3.3. The host was a console started from the command line, so "with the GUI only" of `spec.md` T-7 is judged by 4.2.

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

**Limits of this measurement.**

- The first attempt, ended from the "Processes" tab of Task Manager, sent a close signal to the window and took the normal shutdown path. It is not counted
- The 47 seconds of the forced kill were counted by a person from the kill. The kill time is not in any log

### 4.2 GUI-Only Flow

The record of running the pass conditions of `spec.md` T-7 in one flow with two GUI machines. Both machines were started without command-line arguments.

| Item | Value |
|------|-----|
| Date | 2026-11-02 |
| Build | The `field-kit-p8` GUI distribution. Both machines used the same distribution |
| Networks | A on network P (mobile hotspot), F on network W (the friend's home Wi-Fi) |
| Roles | A is the host and F the player |
| Setup | Succeeded. Direct connection. On both sides the local and public addresses differed |

| Condition | Evidence | Verdict |
|-----------|----------|---------|
| The host creates a room and reads the code | The room code appeared in A's window | Pass |
| The player joins with that code | F entered `CONNECTED` | Pass |
| Both lists show the other's name | F's roster carried both members with connection bit 1. F appeared as Connected in A's window and A in F's window (the owner's observation, and F's observation relayed by the owner) | Pass |
| Disconnecting one side changes the other's list to disconnected | F closed the window (`attempt.end reason=shutdown`). A received `CLOSE`, went to `CLOSED`, and F's row was removed from A's list | Pass |
| A failure code is shown | F joined with a nonexistent room code. It received `room_not_found` and the window's status line showed `FAIL CONTROL_PLANE_EXCHANGE_FAILED` with the guidance sentence | Pass |

**Result.** T-7 passed. Together with 4.1, all T-7 conditions of the `roadmap.md` Phase 8 verification have been run.

**Also seen.** With the same two machines a room was created again, and A's Minecraft opened a single-player world to LAN. F joined at A's virtual IP and that port and stayed about 48 seconds. It connected over the tunnel without a separately started server.

**Limits of this measurement.**

- A's list was captured only after F's row was removed. The intermediate display with connection bit 0 was not recorded
- One network pair

## 5. Limits of the Telemetry Tables

These are the limits for reading the tables produced by the analysis script of `roadmap.md` Phase 9 ([`tools/tm-analysis`](../../tools/tm-analysis/README.md)). What is
counted and how belongs to the `roadmap.md` Phase 9 tasks. There are no results yet.

**The `room_known` filter.**

- An attempt that is `OK` but whose `room_known` is not `true` drops out of both the successes and the denominator. Genuine records that arrived after the room item was deleted and
  records whose check read failed fall here ([`architecture.md`](architecture.md) 3.4 Room Check). Their count is written separately as "excluded OK"
- Failures are counted regardless of `room_known`, because a failure before establishment has no room to check. So fake failure records are not filtered and can raise the failure rate
- The check only sees whether the room exists. Fake records sent by someone who knows a live room code enter both the successes and the session values

**Lost records.**

- An attempt whose `establish` (send sequence number 0) was lost while later records arrived drops out of the attempt denominator. That attempt's loss count is at least 1, and any other gaps are added to it
- Losing the last record of an attempt or a whole attempt does not show in the loss count either
- If a session ends without an `end`, that session drops out of loss and session duration

**Jitter.** A record's `jitter_us` is one smoothed jitter value at the end of the interval (`architecture.md` chapter 9 Telemetry Upload). The jitter median in the tables is
"the median of end-of-interval smoothed jitters". It is not the median of differences of consecutive RTT samples. It is not compared with RFC 3550 jitter.
