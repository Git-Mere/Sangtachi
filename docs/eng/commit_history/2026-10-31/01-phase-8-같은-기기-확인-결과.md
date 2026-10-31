# Phase 8 same-machine check results, plan cleanup

## Why

The repository owner ran the Phase 8 items in `plan.md` "Next" that a person checks on one machine, plus the host shutdown
measurement. The results were checked against the logs and the remaining work was rewritten.

## Result

The builds are the GUI bundle (`scripts/package-gui.ps1`) and a Release console with the same settings, both from HEAD
`8366bab`. Both embed the server address and the administrator manifest.

| Check | Result | Evidence |
|-------|--------|----------|
| GUI start, create room, join, leave room, close window | Pass | The owner watched the screen. The logs have `create_room ok=true`, `CONNECTED` on the console player (`--no-adapter`), a roster with both members, host `reason=leave` and player `reason=closed` at the same time, and `adapter.close` |
| Console lobby (`> `, guide, `help`, `name x`, `quit`) | Pass | The owner watched the screen. The log has `lobby.name name=x`. The first run lacked this line, so it was run again |
| Qt runtime | Not judged | The GUI bundle started on a friend's PC. Whether that PC lacked Qt and the VC++ runtime was not checked. It most likely had the VC++ runtime installed |
| Console window close shutdown | Pass | The host window was closed while the player was `CONNECTED`. The player received `CLOSE` (`reason=closed`), and the host log's `adapter.close` was written about 0.12 s later. The owner timed the shutdown by hand at almost within 1 s |
| GUI logoff shutdown | Not judged | Below |

In the first round the console window close ran with no player, so no `CLOSE` was sent. Meanwhile a second console using the
adapter was started and `WintunCreateAdapter` failed with `0x800700B7`. Only one process per machine can use the adapter
(`field-test/required-test.md`, T-7 row). The first round's "logoff" was a screen lock. `explorer.exe` stayed alive.

**GUI logoff.** The second round had a real logoff (User Profile Service logoff of session 1 and logon of session 2). The GUI
host sent `CLOSE`, and `adapter.close` was written about 0.14 s later. But the console player on the same machine left a logoff
delay warning.

- System log `winsrvext` 100: `sangtachi_client.exe is delaying system shutdown after 5000 milliseconds`
- That player's log has no `attempt.end reason=shutdown`. Its session ended with the host's `CLOSE`
- Log lines carry no time, so when `WM_ENDSESSION` arrived is unknown. The order in which the two processes were signalled is also unknown

So this round does not judge the GUI logoff shutdown. It is measured again with the player on another machine. Whether the
console handled `CTRL_LOGOFF_EVENT` as [`concurrency.md`](../../concurrency.md) section 7 Shutdown says is investigated
separately. A Debug console left the same event on 2026-10-01. The cause is unknown.

## What changed

| What | Where |
|------|-------|
| Drop the finished same-machine checks and console shutdown measurement; list the rest of the Qt runtime condition, the GUI logoff re-measurement, and the console logoff investigation as remaining work | `plan.md` |
| Make the host shutdown time on GUI logoff a two-device field test | `field-test/required-test.md` |

The raw logs are in the build folders (`gui-dist\logs`, `latest_build\logs`). They are not committed.

## Verification

| What | Result |
|------|--------|
| Client logs, record files, and Windows event logs compared | Table above |
| `docgate.py` | `VERDICT: pass` |

## Cross-model review

Codex (`gpt-6.1-sol`). The handoff and record consistency lens and the evidence and Korean/English mirror lens were run
together. Both warns were accepted.

| Finding | Disposition |
|---------|-------------|
| `plan.md` "a PC without Qt was seen" has no evidence and disagrees with `windows-prereq.md` section 4 | Accepted. Rewritten to say that whether the friend's PC lacked Qt and the VC++ runtime was not checked. `windows-prereq.md` is left as is |
| `winsrvext` 100 warns that a process is still delaying after 5000 ms; it is not evidence that the total delay was 5 s | Accepted. "delayed by 5 s" in `required-test.md` and this record became "left a delay warning" |
