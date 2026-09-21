# 0002. Do Not Recover From NAT Rebinding via Endpoint Learning

- Status: accepted
- Date: 2026-09-20
- Related: [`../protocol.md`](../protocol.md) 10.4 and 9.5, [`../design-audit.md`](../design-audit.md) blocker 12

## Context

`protocol.md` 10.4 chose **learning** over fixed nomination, and one of its three reasons was this.

> **NAT rebinding**: the epoch does not change when the mapping does, so a fixed scheme has no
> trigger for renegotiation and must wait for the idle timeout.

10.4 (c) accepts a packet from an address outside the candidate set after path validation. That
procedure begins with **"when a packet arrives from an address not in the candidate set"**.

The 2026-09-20 measurements tested that premise directly. After the punch succeeded, each side
sent to the peer from a **new socket with a new source port**, and checked whether the peer
received it.

| Measurement | Host OS | Host firewall | Result |
|------|-----------|---------------|------|
| WA to WA | Windows 11 both | Inbound default `Block`, no `python.exe` rule | `blocked` both ways |
| WA to MI | Windows 11 / **macOS** | The macOS Application Firewall is off by default | `blocked` both ways |
| Hotspot to WA | **Linux** / Windows 11 | On Linux, `/etc/ufw/ufw.conf` says `ENABLED=no` (checked directly) | `blocked` both ways |

**All 6 were `blocked`.** In every measurement the sync succeeded (`sync.peer_ready: true`), the
peer actually sent 20 probes (`peer_sent_reported: 20`), and 0 arrived.

**It was blocked even on hosts believed to have no active firewall filtering.** The most likely cause is therefore not
the host but what sits in front of it: **port-restricted filtering in the NAT**. That the NAT
drops packets from a source port we have never sent to is consistent with every observation.

**It is not stated as certain.** The macOS case is inferred from defaults and was not queried
directly. On Linux only `ufw` was confirmed off, and without root the full `nft` and `iptables`
rulesets were not seen. No packet capture was taken on either side. The observations are merely
**consistent with** a port-restricted NAT.

**What can be done depends on which cause it is.** If the peer NAT or peer host filtering is
responsible, there is nothing we can do from our side. If **our own Windows firewall is
responsible, our client could register a rule and fix its own reception.** That is why this
decision is provisional, and the condition that would overturn it is written below.

So the premise of 10.4 (c) does not hold in a real environment. Step 1 of the procedure (record
the new address as a tentative path) never starts, so nothing after it runs.

Renegotiation in 9.5 is blocked for the same reason. It is triggered by receiving a `HELLO` with a
different epoch, and a rebound peer's `HELLO` also arrives from a new source port.

## Decision

**v1 does not guarantee recovery from NAT rebinding through endpoint learning. This is stated in
`protocol.md`.** Precisely, it is not guaranteed for **a rebinding matching the tested behaviour,
where only the source port changes and the observed filtering applies.** An actual rebinding was
never induced and tested, so this does not claim that every rebinding fails.

- Path validation in 10.4 (c) is **not removed,** for two reasons. Its **defence against forged
  source addresses** remains (whenever a packet does arrive from outside the candidate set, that
  validation is needed), and **on a NAT that uses address-restricted (restricted cone) filtering
  the packet can arrive**
- It is documented that **rebinding recovery is not guaranteed.** One of the three reasons 10.4
  chose learning over nomination is void by measurement. The other two (deadlock behind the same
  NAT, asymmetric nomination) still hold, so the learning approach itself stays
- When rebinding happens, both sides reach the idle timeout (50s) and the session ends with
  `TUNNEL_DROPPED`. Under `protocol.md` 9.6, **if a retry happens** it starts from `IDLE` as a new
  attempt and goes through the control plane again. **Whether that retry is automatic is not
  defined in the documents.** What can be confirmed today stops at "the session ends"; whether
  what follows is automatic or requires a person to run it again is undecided

**The client does not register firewall rules.** The NAT is the likely cause, so it would have no
effect. **This is provisional.** See the revisit condition below.

**What would overturn this decision.** This is why option a is **deferred** rather than rejected.

The cause narrows differently per direction.

| Direction | What could block it | How far it narrows |
|------|-----------------|-------------|
| Windows to macOS / Linux | The peer NAT, the peer host firewall | The peer firewall is probably off, so the evidence **leans toward the NAT.** The macOS case is inferred from defaults and only `ufw` was checked on Linux, so host filtering is not excluded |
| macOS / Linux to Windows | Our NAT, our Windows firewall | **Both remain. It does not narrow** |

So for directions where our Windows host is the receiver, the firewall is still a possible cause.
If it is, the client could register an inbound rule and fix **its own reception**. That would not
fix the whole path if the peer NAT still blocks, but it would remove our side as a factor.

**The decisive test.** Do not turn the firewall off. **Adding and removing one narrowly scoped
inbound rule is safer and more precise.** Disabling a profile risks disabling the wrong one
depending on which profile is active, and restoring it can overwrite its original value.

Run this in an administrator PowerShell. The port must be fixed so a rule can target it.

```powershell
# Administrator PowerShell.
$group = "natprobe-unsolicited-test"     # An identifier used only by this test

# 0. Clear rules left by a previous run first. An interruption or reboot can skip finally.
#    Find them by the dedicated Group, not by a name wildcard.
#    Run this only when no other copy of this test is running on the machine.
Get-NetFirewallRule -Group $group -ErrorAction SilentlyContinue | Remove-NetFirewallRule

# 1. Read the address. Do not put it on the command line; PowerShell history keeps it on disk.
#    Read-Host also accepts "Any" and CIDR, so check for a public unicast IPv4 address.
$raw  = Read-Host "Peer public IP"
$peer = [System.Net.IPAddress]::Parse($raw)      # throws if it is not an address
if ($peer.AddressFamily -ne 'InterNetwork') { throw "must be IPv4: $raw" }
$b = $peer.GetAddressBytes()
$notPublic =
  ($b[0] -eq 0) -or ($b[0] -eq 10) -or ($b[0] -eq 127) -or ($b[0] -ge 224) -or
  ($b[0] -eq 169 -and $b[1] -eq 254) -or
  ($b[0] -eq 172 -and $b[1] -ge 16  -and $b[1] -le 31)  -or
  ($b[0] -eq 192 -and $b[1] -eq 168) -or
  ($b[0] -eq 100 -and $b[1] -ge 64  -and $b[1] -le 127) -or
  ($b[0] -eq 192 -and $b[1] -eq 0   -and ($b[2] -eq 0 -or $b[2] -eq 2)) -or
  ($b[0] -eq 198 -and ($b[1] -eq 18 -or $b[1] -eq 19)) -or
  ($b[0] -eq 198 -and $b[1] -eq 51  -and $b[2] -eq 100) -or
  ($b[0] -eq 203 -and $b[1] -eq 0   -and $b[2] -eq 113)
if ($notPublic) { throw "must be a public unicast IPv4 address: $raw" }

$rule = $null
try {
    $rule = New-NetFirewallRule -DisplayName "natprobe-temp-$((New-Guid).Guid)" `
      -Group $group -Direction Inbound -Protocol UDP -LocalPort 47000 `
      -RemoteAddress $peer.IPAddressToString -Action Allow -Profile Any
    # Assumes the repository root. The explicit path avoids depending on the working directory.
    py .\tools\nat-probe\natprobe.py punch --label us-home --port 47000 --unsolicited
}
finally {
    # Remove **the object**, not a name or a group. Safe even with a concurrent run.
    if ($rule) { Remove-NetFirewallRule -InputObject $rule }
}

# 2. Verify. This must print nothing.
Get-NetFirewallRule -Group $group -ErrorAction SilentlyContinue
```

**The rule is persistent.** `New-NetFirewallRule` creates a rule that survives on disk by default.
If the process dies, the window is closed, or the machine reboots during the measurement,
`finally` never runs and the rule stays. That is why step 0 runs every time and step 2 checks.
**If step 2 prints anything, remove it and check again.**

The address check uses the same rules as `classify_nat` in `natprobe.py`. It rejects private,
loopback, link-local, CGNAT, documentation, benchmarking, and multicast ranges. One typo should
not create a broad rule.

`Read-Host` is used so the peer's public IP is not written into command history. This is the same
reason the raw measurements are not committed to this repository
([ADR 0001](0001-no-direct-connection-fallback.md)).

**That this test needs administrator rights is itself an observation.** If option a were adopted,
the client would have to do the same thing, and the cost shows up right here.

| Result | Reading | Action |
|------|------|------|
| Still `blocked` with the rule in place | **The default inbound policy is not the cause.** This does not exclude the firewall as a whole: an explicit block rule takes precedence over an allow rule, and third-party security products can still filter. The evidence leans further toward the NAT but is not conclusive. Confirming it needs the effective WFP rules or packet captures on both sides | This ADR stands. Further checks are optional |
| Becomes `allowed` with the rule | **The Windows firewall was the cause.** Revise this ADR | Reconsider option a, and carry the administrator rights requirement into `spec.md` and follow-up 3 |

Until that test is run, **"the client does not register firewall rules" is a provisional
decision.**

## Alternatives

| Option | Content | Reason for rejection |
|----|------|-----------|
| a | Have the client register an inbound allow rule | **Deferred, not rejected.** It was blocked even on hosts believed to have no active firewall filtering (macOS, Linux), so the NAT is the likely cause, in which case a rule would not help. It would also require administrator rights, increasing the burden of blocker 11 and adding a caveat to the "without manual port forwarding" claim in `spec.md`. **However, the firewall has not been excluded as the cause for directions where Windows is the receiver.** See the revisit condition below |
| b | Predict the peer's port range and send to several ports at once | Ports under destination-dependent mapping are not reliably predictable. It increases traffic and risks conflicting with the candidate hygiene rule in 10.1 (do not fire at third parties from an unbounded candidate list) |
| c | Add a rebinding renegotiation trigger to the control plane | It grows the scope of Phase 3. Rebinding itself is rare in a 30 minute demo, and the 15s keepalive keeps the mapping alive. Left out of v1 scope |
| d | Delete 10.4 (c) entirely | Path validation is the mechanism that stops an attacker from redirecting traffic to a third party with a forged source address (the security argument in 10.4). Deleting it removes that defence |

## Consequences

**What this gains.**

- No administrator rights requirement appears. The "without manual port forwarding" claim in
  `spec.md` stands
- Follow-up 3 (the Windows prerequisites section) does not gain a firewall rule registration item
- The documents and the actual behaviour agree

**Risks accepted.**

- **A rebinding drops the session after up to 50 seconds.** Those 50 seconds bound **failure
  detection**, not recovery. Because automatic retry is undefined (decision item 3 above),
  **recovery time is unbounded.** The chance of a rebinding during a 30 minute demo is low but
  not zero
- **All observations were consistent with port-restricted filtering.** It was not confirmed. In an environment with an
  address-restricted (restricted cone) NAT, 10.4 (c) may work normally. This decision says "not
  guaranteed", not "impossible"
- **The macOS firewall state on the friend's machine was not checked directly.** The Linux side
  was checked; the macOS case is inferred from defaults. Checking it would add one more piece of
  evidence
- **On Linux there was no root access, so the full `nft` and `iptables` rulesets were not seen.**
  Only that `ufw` is off was confirmed

**Required follow-up.**

1. Write the premise and the limit into `protocol.md` 10.4. State that rebinding recovery is not
   guaranteed
2. Measure mapping lifetime during the Phase 4 keepalive work, and check whether the 15s interval
   actually prevents rebinding
3. **Decide whether a retry after `TUNNEL_DROPPED` is automatic.** This gap surfaced while writing
   this ADR. `protocol.md` 9.6 only says a retry starts from `IDLE`; it does not say who starts it
   or when. Once rebinding recovery is left to a full restart, that gap has to be closed
4. Put this observation in the report. It is an honest record of the design process: the learning
   approach was chosen, but one of its reasons was invalidated by measurement
