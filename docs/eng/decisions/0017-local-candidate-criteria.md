# 0017. Local Candidate Decision Criteria

- Status: Accepted
- Date: 2026-10-08
- Related: [`../protocol.md`](../protocol.md) Chapter 7 and 10.1, [`../control_plane.md`](../control_plane.md) 8.4, [`../windows-prereq.md`](../windows-prereq.md) Section 3, [`../roadmap.md`](../roadmap.md) Phase 4, [`../spec.md`](../spec.md) supported network conditions, [ADR 0010](0010-platform-porting-seams.md)

## Context

`protocol.md` 10.1 defined local candidates as addresses of "active IPv4 interfaces" and said to exclude
loopback, APIPA, our own Wintun, and other tunnel/VPN adapters. It did not define how "active" and
"tunnel/VPN" are decided. `roadmap.md` Phase 4 made this a decision to take before starting.

The two directions of a wrong decision cost different things.

- A wrongly included address receives `HELLO` every 200ms per attempt until the punch deadline (10 s)
- A wrongly excluded one removes the same-LAN candidate, so the same-LAN connection, a required topology
  in `spec.md`, relies on hairpinning

Values read on this machine (Windows, development machine) with `Get-NetAdapter`, `Get-NetIPAddress`,
and `Get-NetRoute`.

| Adapter | `IfType` | State | IPv4 | Default route | `HardwareInterface` |
|---|---|---|---|---|---|
| Wi-Fi (Realtek 8852CE) | 71 | Up | `10.0.0.49/24` `Preferred` | Yes | True |
| Ethernet (Realtek GbE) | 6 | Disconnected | `169.254.199.113/16` `Tentative` | No | True |
| Tailscale | 53 | Up | `100.102.127.59/32` `Preferred` | No | False |
| TAP-Win32 Adapter V9 | 6 | Disconnected | `169.254.187.141/16` `Tentative` | No | False |
| vEthernet (FSE HostVnic), Hyper-V | 6 | Up | None | No | False |
| WAN Miniport (PPPOE) | 23 | Disconnected | None | No | False |
| Teredo, 6to4, IP-HTTPS | 131 | Not Present | None | No | False |

Three things followed.

- Tailscale's driver is `Wintun Userspace Tunnel` (ComponentID `Wintun`). It shares both `IfType` and
  driver with our Wintun, so our adapter can only be told apart by its recorded identifier
- TAP and Hyper-V have `IfType` 6, the same as Ethernet
- Disconnected adapters keep addresses. Holding an address is not evidence of being active

Two standards were checked in the original text.

- RFC 8445 (ICE) 5.1.1.1 gathers host candidates from every address "including VPN interfaces" and
  mandatorily excludes only loopback
- RFC 8828 (WebRTC) Mode 2 uses only the default-route interface and its private addresses

## Decision

The repository owner decided the following.

**1. Active means the interface `OperStatus` is `Up` and the address `DadState` is `Preferred`.**

**2. Excluded are `IfType` 24, 53, 131, 23, the loopback and APIPA ranges, and our own Wintun with a matching recorded `InterfaceGuid`.**
An `IfType` not in the list is included. Our own Wintun is checked separately by identifier regardless of
`IfType`.

**3. Virtual adapters that appear as `IfType` 6 are not filtered.** That cost is accepted.

**4. The registered list is reflexive candidates, then local candidates of the default-route interface, then the remaining local candidates.**
After removing duplicates, the first `MAX_CANDIDATES` are kept. The default route is the `0.0.0.0/0` route
with the smallest route metric plus interface metric.

**5. The own-subnet set for the Chapter 7 directed broadcast decision is built from addresses that passed only the active stage.**
The exclude stage does not apply. Loopback-range addresses are not included, and only prefixes from 0 to 30 are.

**6. Local candidates and the own-subnet set come from the same collection just before `register_candidate`.**

**7. If interface enumeration fails, register without local candidates and keep the previous own-subnet set.** A
route lookup failure is the same as having no default route. Both leave one `WARN` line and are not
retried. Interfaces that change between collections are missing from the set until the next collection,
and this is recorded as a residual risk.

## Alternatives

**Alternative 1. Name or description strings ("TAP", "Hyper-V", "VPN", etc.).**
Rejected. It catches the TAP family too. The cost is a list that never ends, aliases that are localized and
user-editable, and dropping the correct candidate in setups where the real LAN address sits on a virtual
adapter, like a Hyper-V external switch. It also conflicts with "a name is not evidence of ownership" in
`windows-prereq.md` Section 3.

**Alternative 2. Only interfaces with a gateway.**
Rejected. It filters most host-only virtual adapters. The cost is dropping normal LANs without a gateway
(the side running a mobile hotspot, a second LAN without internet).

**Alternative 3. Only interfaces with `HardwareInterface` true.**
Rejected. On this machine it splits most cleanly. The cost is dropping the correct candidate in setups
where the real LAN address sits on a virtual adapter (Hyper-V external switch, mobile hotspot).

**Alternative 4. Only the default-route interface (RFC 8828 Mode 2).**
Rejected. It is consistent with the reflexive candidate's interface. The cost is dropping the same cases
as Alternative 2. This decision uses that criterion only for ordering, not for filtering (Decision 4).

**Alternative 5. No filtering (RFC 8445).**
Rejected. It never loses a correct candidate. The cost is that two peers on the same overlay VPN connect
over that VPN path, muddying the "direct path" verification, and the VPN address goes to the peer.

**Alternative 6. An `IfType` allowlist (6, 71, etc.).**
Rejected. It drops real networks outside the list (e.g. WWAN). This is not a safety decision, so unknowns
are included.

**Alternative 7. Active judged by `OperStatus` alone, or by address presence alone.**
Rejected. The former includes addresses whose DAD has not finished; the latter includes addresses left on
disconnected adapters, as observed above.

**Alternative 8. Make the own-subnet set the same set as the local candidates.**
Rejected. A broadcast source from the subnet of an adapter excluded from candidates would not be blocked.

**Alternative 9. Take the default route as the best route toward the STUN server.**
Rejected. With a full-tunnel VPN on, that route is the VPN interface and the ordering loses its meaning. If
the VPN uses two `0.0.0.0/1` lines, `0.0.0.0/0` stays on the original interface. If the VPN takes
`0.0.0.0/0` and that interface is excluded, the order stays as enumerated (`protocol.md` 10.1 registered
list case table O2).

**Alternative 10. End the attempt on enumeration failure.**
Rejected. The failure becomes visible. The cost is losing even the cross-network connection, which
reflexive candidates alone can make.

**Alternative 11. Recollect on interface change notifications.**
Rejected. The gap between collections disappears. The cost is adding notification handling and its thread
ownership to v1. Replies in the gap are bound by the common unverified-destination budget, unless the address is an approved candidate or `peer_endpoint`.

## Consequences

**Gained.**

- The decision inputs are only values the OS provides. A machine can judge it with case tables
- Fewer cases of losing the same-LAN candidate than Alternatives 1-4
- Even when the cap is hit, the reflexive candidates remain, and if there are fewer than 8 distinct reflexive candidates the default-route LAN candidate remains too

**Paid.**

- Addresses of virtual adapters with `IfType` 6, like TAP, Hyper-V, and VMware, go to the peer as
  candidates. `HELLO` goes to those addresses per attempt
- If such an address collides with another device on the peer's LAN, `HELLO` goes to that device. The
  hygiene rules block broadcast but not a single unicast host

**Not verified.** The following rest on known Windows behavior and were not observed on this machine. The
same-LAN verification of `roadmap.md` Phase 4 is the first check on real machines.

- The real LAN address moving to vEthernet under a Hyper-V external switch
- The address and gateway on the side running a mobile hotspot
- Windows built-in VPN (RAS) appearing as `IfType` 23
- The `IfType` of Hamachi, ZeroTier, and Radmin VPN
