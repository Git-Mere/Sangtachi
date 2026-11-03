# 0038. Put the demo host on the owner's home Wi-Fi

- Status: Accepted
- Date: 2026-11-02
- Related: [ADR 0034](0034-decisions-before-phase-8.md), [ADR 0037](0037-one-rehearsal-before-the-demo.md), [`../roadmap.md`](../roadmap.md) Phase 8 tasks, [`../experiments.md`](../experiments.md) 3.2 and 3.3

## Context

[ADR 0034](0034-decisions-before-phase-8.md) set the demo networks as "the host on the owner's mobile hotspot at school, the player on home Wi-Fi".
The 2026-11-02 rehearsal used that network pair. The host was the Linux machine `G`, and the Minecraft server is also on `G` (`experiments.md` 3.3).

Two things were seen on the rehearsal day.

- On the school wired LAN, the Tailscale control server was unreachable, so other machines could not be handled remotely
- When the owner left with the hotspot, the host dropped and the room had to be created again

## Decision

This is the repository owner's decision.

| Item | Choice |
|------|--------|
| Host | `G`. It stays on the owner's home Wi-Fi and runs the Linux console host and the Minecraft server |
| Owner | Joins with the GUI of the Windows machine `A` at school on the mobile hotspot. The phone's Wi-Fi is turned off |
| Demo player | Joins with the GUI from her home Wi-Fi |
| Another player | If the owner's friend can take part, he joins with the GUI from his home Wi-Fi |

The rehearsal rule of [ADR 0037](0037-one-rehearsal-before-the-demo.md) stands. Since the network pairs changed, the 2026-11-02 rehearsal does not
cover these demo networks. It is done once more with the new network pairs before the demo.

## Alternatives

**Alternative 1. Keep the demo networks of ADR 0034.** Rejected. The host is tied to the phone, and the demo drops if the phone moves away.

**Alternative 2. Put the owner's `A` on the school wired LAN.** Rejected. Tailscale is blocked on that network, so `G` at home cannot be restarted.
A connection between home Wi-Fi and the school wired LAN has never been measured.

**Alternative 3. Make `A` the GUI host and open Minecraft with "Open to LAN".** Rejected. Using the rehearsed host and server as they are was preferred.

## Consequences

- Between the host and the owner are home Wi-Fi and the mobile hotspot. These are the same two networks as 3.2, with only the host side swapped
- Between the host and the demo player, and between the host and the friend, are network pairs never measured. The rehearsal checks them
- `G` runs at home with nobody there. It is kept from sleeping and left with Wi-Fi power saving off. The room stays open while the host is alive ([`../control_plane.md`](../control_plane.md) 5.1)
- Since the demo host is a console, the demo does not fill the GUI-only flow of `spec.md` T-7
