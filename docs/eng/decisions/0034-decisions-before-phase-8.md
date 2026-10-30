# 0034. Decisions before Phase 8

- Status: Accepted
- Date: 2026-10-29
- Related: [`../roadmap.md`](../roadmap.md) Phase 8, [`../concurrency.md`](../concurrency.md) chapters 1 and 7, [`../architecture.md`](../architecture.md) 3.5, [`../control_plane.md`](../control_plane.md) 6.4, [`../windows-prereq.md`](../windows-prereq.md) 1, 4, and 14, [`../experiments.md`](../experiments.md), [ADR 0001](0001-no-direct-connection-fallback.md), [ADR 0007](0007-gui-core-scope-qt.md), [ADR 0030](0030-startup-input-and-console-screen.md)

## Context

`roadmap.md` Phase 8 lists five things to decide before starting: the GUI thread model, the startup input path the GUI uses, the
relation between GUI operation and the control plane rate limit, the timing of privilege elevation and leftover cleanup, and the
distribution form of the Qt runtime. Reading the documents turned up more gaps.

- The last condition of T-7 is that a player sees another player's state. That information arrives only through the roster the host
  sends (`protocol.md` 5.7 ROSTER). So three machines are needed. Two processes cannot run on one machine: the adapter GUID is a
  constant, so the second fails to start (ADR 0027)
- When a player is cut, the connection bit in the roster first becomes 0, and after the host reports the departure with `host_report`,
  that member is dropped from the roster. How the screen shows those two steps was missing
- The measurement criteria table only says the game-displayed latency is read from the screen. The Tab list of vanilla Minecraft shows
  only bars and no number
- `experiments.md`, which holds the Phase 8 results, is a Phase 9 deliverable and did not exist yet

The repository owner ran one Minecraft session with `field-kit-p7b` on 2026-10-28. The result is in `experiments.md` 3.1.

## Decision

The repository owner chose the following for each item.

| Item | Choice |
|------|---------|
| Work order | The Minecraft verification is done first with `field-kit-p7b`. The GUI comes after it. The GUI tests and T-7 use a new kit `field-kit-p8` |
| Machines for T-7 | Three. One host and two players |
| Demo networks | The host on the owner's mobile hotspot at school, the players on home Wi-Fi |
| GUI thread | The Qt thread is the process main thread. `[loop]` moves to a worker thread. The GUI thread takes the place of `[console]` and pushes commands to the same command queue. What goes from `[loop]` to the GUI is one new queue |
| Executable | The GUI is a separate target `sangtachi_gui.exe`. It is a Windows subsystem executable, so it has no console window. The console target stays for tests and the field kit. The distribution does not include the console executable |
| Server address | Only the build default is used. The window has no field for it. Configuring the GUI target with an empty default stops the configuration |
| Name | The field is prefilled with the default name. A human can edit it |
| Room code | Shown, with a copy button |
| Failure display | The status line inside the window shows the code, the counterpart, and the sentence. The owner may decide again after seeing the real thing |
| Rate limit | While a control request is outstanding, the create room and join buttons are disabled. A received failure is shown as is. No separate wait time on the window side |
| Elevation refusal | The administrator manifest is kept. If it is refused, Windows does not create the process, so no window appears |
| Timing of leftover cleanup | The window comes up first. It shows `Loading...` and disables the buttons until preparation finishes. The worker thread does the leftover cleanup and adapter preparation |
| Startup failure | A dialog shows the `ERROR` line and the log file name, and closing it ends the process |
| Closing the window | It ends without confirmation. `Closing...` is shown until cleanup finishes |
| Qt | Qt Widgets. Version 6.12.0. Obtained with the official online installer. The distribution is built with dynamic linking and `windeployqt`. The licence is LGPLv3. The owner said the approval covers this |
| Member list | The roster is shown as is. The connection bit shows two states, and a member dropped from the roster has its line removed. There is a virtual IP column |
| GUI tests | The display logic is kept in C++ without Qt and tested with Catch2. A human checks the widgets by hand |
| Minecraft | Java Edition, version label 26.3 in the launcher |
| Game-displayed latency | Read from the network graph on the F3 debug screen. The owner saw the graph appear in 26.3 |
| Results document | `experiments.md` is created now. It holds results starting from Phase 8 |
| Check before the demo | Instead of re-measuring with `tools/nat-probe`, rehearse with the client on the same network pair as the demo. Once a few days before and once on the day. If a rehearsal fails, nat-probe tells the cause. This replaces the Phase 8 preparation and the just-before-demo re-measurement of ADR 0001 |
| Mapping lifetime of the demo networks | Both the host's mobile hotspot and the player's home Wi-Fi are measured. The player's home is not one of the two homes already measured |
| Wi-Fi drop | Turn off the player-side Wi-Fi for about 10 seconds and about 60 seconds and record the behavior. It is not judged |
| 3-second measurement | Both closing the window in the console build and logging off in the GUI build are measured |
| When it ends on its own | If `[loop]` ends without a human closing the window, the same dialog as a startup failure is shown |
| Block and chat criterion | "All within 2 seconds, 20 each" becomes "no noticeable delay". The owner watches and judges directly |
| Own line in the member list | The own line gets `(you)` |
| Distribution | Qt components the window does not use are not included. For the VC++ runtime, the DLLs are placed next to the executable instead of the installer |
| Release GUI build | Built with the server address embedded. This is also the GUI build used day to day |
| When the screen queue is read | `[loop]` does not wake `[gui]`. `[gui]` reads it with a timer every 100ms |

## Alternatives

**GUI thread.** Rejected keeping one thread and changing the `[loop]` wait to `MsgWaitForMultipleObjects` so Qt events are handled inside
it as well. When the window is dragged or resized, Windows enters a modal loop, and during that `[loop]` stops and keepalive stops too.
Also rejected having the GUI executable start the current console client as a child process and talk to it over standard input and
output. It had the advantage of not touching the core. The cost was emitting the roster, the virtual IP, and the return to the lobby as
new standard output lines, and coordinating the shutdown of two processes.

**Distribution.** Rejected leaving the default options of `windeployqt` as they are. Of 58 files, about 94MiB, more than 60MB was what the
window does not use. Also rejected including the VC++ runtime installer (`vc_redist.x64.exe`). The user has to run it separately, and if
they do not, including it means nothing.

**When the screen queue is read.** Rejected having `[loop]` wake `[gui]` with a Qt queued call after pushing. Qt takes the event queue
lock inside that call, which brings a lock into `[loop]` (`concurrency.md` chapter 6). Also rejected having `[loop]` notify the window
with Win32 `PostMessage`. It is reflected immediately, but the window handle has to be passed to the `[loop]` side and one more seam has to
be placed in `platform/`. The cost is that the screen updates up to 100ms late.

**Executable.** Rejected one executable with modes. One file cannot have both the console subsystem and the Windows subsystem.

**Server address.** Rejected a field in the window. There is one control server and its address is fixed (ADR 0030).

**Name.** Rejected leaving the field empty and requiring input. A default name already exists (ADR 0030).

**Failure display.** Rejected a dialog. It adds one more window, and it has to be closed before the next action.

**Rate limit.** Rejected blocking the buttons on the window side for a few seconds after a failure. It needs one more value that the
documents do not have. The server's budget counts only failure responses, so normal operation does not spend the budget
(`control_plane.md` 6.4).

**Elevation refusal.** Rejected starting without the manifest and showing a button to run again if not administrator. That reopens
alternative 6 of ADR 0030 for the GUI only, and adds code for the rerun path.

**Timing of leftover cleanup.** Rejected showing the window after preparation finishes. Nothing is on the screen for several seconds.

**Closing the window.** Rejected showing a confirmation window when in a room. It is a screen outside FR-15.

**Qt.** Rejected Qt Quick (QML). The QML engine and the Quick modules add more to the distribution. Rejected static linking. Qt would
have to be built statically by hand, and the LGPLv3 obligations get heavier.

**Member list.** Rejected keeping a member dropped from the roster as "left". It creates a state on the window side that is not in the
roster. It conflicts with the rule that the roster is display-only and the screen shows it as is (`protocol.md` 5.7).

**GUI tests.** Rejected testing down to the widgets automatically with Qt Test. What needs a verdict is the display logic, and that is
tested without Qt.

**Check before the demo.** Rejected keeping the nat-probe re-measurement as is. The current client asks two STUN servers on the same
socket and actually hole punches. A rehearsal also covers the adapter, the firewall, the control server, and Minecraft on top of that.
What only nat-probe covers is the filtering test and telling failure causes apart, and that can be used when a rehearsal fails.

**Block and chat criterion.** Rejected keeping the 20 each within 2 seconds. Measuring 2 seconds across two remote screens needs
recording and clock alignment. What this test wants to see is whether the game runs smoothly.

**Game-displayed latency.** Rejected the ms read by hovering over the bars in the multiplayer server list. It is one sample per refresh,
and it does not measure the same interval during play.

## Consequences

**What this gains.**

- The sole-ownership rule of `[loop]` stays as is. The GUI thread holds only one end of the queues, like `[console]`
- The console build's standard output and tests stay as is
- The Minecraft verification runs on the same tunnel build as the Phase 7 file transfer. This is the condition of NFR-2

**What this costs.**

- In the GUI build, `[loop]` is not the main thread. `concurrency.md` chapter 1 states a different composition per build
- The GUI build has no console control signals. Closing the window and logoff have to be received separately
- A person who refuses elevation sees no message at all
- T-7 needs three machines and three people
- The propagation test has no numeric criterion and relies on human judgement
- The Qt and VC++ runtimes are attached to the distribution. It is about 30 MB (`windows-prereq.md` 4 Qt runtime)

**What was not confirmed.**

- Whether the distribution starts on a PC without Qt and the VC++ runtime installed. This is the pass condition of `windows-prereq.md` 4 Qt runtime
- The display resolution of the F3 network graph values. The tolerance of the measurement criteria table uses that value. It is written at
  the first measurement
