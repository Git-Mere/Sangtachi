# 0001. Wi-Fi power saving on the Linux machine made tunnel ping slow

- Status: Worked around
- Found: 2026-10-31
- Related: [ADR 0035](../decisions/0035-linux-console-client.md), [`../linux-prereq.md`](../linux-prereq.md),
  [`../commit_history/2026-10-31/02-linux-콘솔-클라이언트.md`](../commit_history/2026-10-31/02-linux-콘솔-클라이언트.md)

## Symptom

This was the first two-machine check of the Linux console client. The Linux machine `mere-galaxy` was the host with the adapter (`10.100.0.1`), and the Windows machine was
the player (`10.100.0.2`). Both machines were on the wireless LAN of the same router.

| Direction | Result |
|------|------|
| Windows → `10.100.0.1` | 4/4, 168~208ms, average 189ms |
| `mere-galaxy` → `10.100.0.2` | 6/6, 5~23ms |

It was the same tunnel, but only one direction was about ten times slower.

## Investigation

| Suspected | Seen | Judgement |
|-----------|-------|------|
| The client reads from TUN late | `strace` shows the TUN fd in the `poll` wait set of `[loop]`, and `read` comes right after `POLLIN` | No |
| The TUN path itself is slow | The direction `mere-galaxy` sends first goes through the same TUN read and inject, and it is 5~23ms | No |
| The wireless hop is slow | Without the client, `ping` from Windows to the LAN address of `mere-galaxy` was also 3~208ms, average 96ms | Yes. It is outside our program |
| Wireless power saving | `iw dev wlo1 get power_save` was `on`. When `mere-galaxy` sent `ping` to the router every 50ms to keep the wireless awake, the same LAN `ping` dropped to 3~19ms, average 9ms | Taken as the cause |

## Cause

**Confirmed.** What is slow is not the tunnel but the wireless hop of `mere-galaxy`. The same delay appeared without the client. Keeping the wireless awake made
the delay go away.

**Inferred.** For a wireless device in power saving, the router holds incoming frames and hands them over when the device wakes. So only packets that arrive first
from outside are late. In the direction `mere-galaxy` sends first, the device is awake at the moment it sends and gets the reply right away. That matches only one direction being slow.
The router-side settings were not looked at.

## Handling

Wireless power saving on `mere-galaxy` was turned off for the field test. No code was changed.

```bash
sudo iw dev wlo1 set power_save off
```

This setting reverts on reboot. A permanent setting (NetworkManager's `wifi.powersave`) was not added.

## Result

`ping` in the same setup after turning power saving off.

| Direction | Result |
|------|------|
| Windows → `10.100.0.1` | 4/4, 10~78ms, average 31ms |
| `mere-galaxy` → `10.100.0.2` | 4/4, 25~113ms, average 76ms |

The direction sent from Windows dropped from an average of 189ms to 31ms. The opposite direction was actually higher than at first (5~23ms). Its cause was not looked at.
The two measurements are different sessions with four or six samples, so nothing can be said about the distribution.

The values were copied from the `ping` output at the time. The raw output was not kept in the repository.

- Before a field test, check with `iw dev wlo1 get power_save` that power saving is off. With a wired connection this issue does not occur
- Reverting power saving is held by `plan.md`
