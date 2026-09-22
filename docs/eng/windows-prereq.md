# Windows prerequisites

What must be in place on a PC that runs the client for the first time, what is handled
automatically, and what a person has to do beforehand.

> Korean version: [`../kor/windows-prereq.md`](../kor/windows-prereq.md)

## 0. How to read this

**Every section has a check method and a pass condition.** If a section does not say what to look
at to pass, that section is unfinished.

Most are commands, but **some sections have no command.** Section 11 (LAN discovery) is not a state
to check but a workaround a person performs, so it records a procedure and a pass condition instead
of a command.

**Sections 1-7 block progress when they are not in place. Sections 8-13 are constraints you must
know.** The first group stops you when it fails. In the second group, if you do not recognise the
symptom you end up fixing the wrong place.

Each section header states its verification status.

| Mark | Meaning |
|------|---------|
| **Measured** | The check command was run on one Windows 11 24H2 machine and the pass condition is decided from that output |
| **Partly measured** | The command was run, but **the pass condition is not decided yet.** What is missing is stated |
| **Unverified** | Not run. The reason and what would verify it are stated |

Currently 4 sections are measured, 4 partly measured, 5 unverified.

**No measurement dates are written in the body.** The raw records of the measurements are in
`commit_history/` and `tools/nat-probe/records/`. This document holds only the current spec and the
pass criteria.

**A failed query and "absent" are distinguished.** Queries that gate progress (sections 3 and 8)
return 0 only for `ObjectNotFound` and raise every other error. Sections 2 and 7 moved the decision
into scripts, and those scripts report the same distinction as an `unknown` verdict. **`unknown` is
not a pass.** Reading a permission failure as "absent" walks past leftovers. **Informational
queries** (the policy value and `Zone.Identifier` in section 13) use `-ErrorAction
SilentlyContinue`, and state on the spot that absence is the normal case.

**"Measured" means the command runs on this machine, not that it was confirmed with the demo
artefact.** The client adapter does not exist yet, so pass conditions that target our own adapter
are confirmed for real in Phase 6 or later.

**Output from one machine is a fact about that one machine.** Adapter names, metrics and ranges
differ per machine. What to compare is not the value itself but the **pass criterion**. Each
section states its criterion.

End-to-end verification on a freshly reset PC belongs to Phase 8 demo preparation and is not the
completion condition of this document. Phase 1 implementation has not started, so the automatic
handling by the client is a **design split**, not confirmed behaviour.

## 1. Administrator rights

**Status: measured**

Creating the Wintun adapter, assigning the IP and adding the route all require administrator
rights. **This is a Windows API premise, not something confirmed on this machine.** What was
measured here is only whether the shell is elevated. Which of the three operations fails, and with
which error, **is confirmed in Phase 6 when the adapter is first created.**

**Owner: person.** The client can elevate itself (UAC relaunch), but the user has to consent.

```powershell
([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
  [Security.Principal.WindowsBuiltInRole]::Administrator)
```

The expected output is `True`. A PowerShell that was not started as administrator prints `False`.

**Pass criterion.** Proceed only on `True`. On `False` the client stops before creating the adapter
and reports what is needed. **Blocking before start is better than failing while creating the
adapter.** A mid-way failure leaves the leftovers of section 3.

**This requirement already exists.** The firewall rule registration later (section 2) does not
*newly* create the need for administrator rights. [ADR 0002](decisions/0002-no-rebinding-recovery.md)
listed "administrator rights become required" as a cost of option a, but adapter creation already
needs them. The marginal cost is "one more rule with rights we already have".

## 2. Firewall and network profile

**Status: partly measured.** The default inbound policy and the profile classification of existing
adapters were actually observed. **A newly created virtual adapter being classified as Public was
not observed.** The adapter does not exist yet. Confirmed in Phase 6.

Windows Firewall is **inbound-block by default.** A newly created virtual adapter has no gateway,
so it is classified as an "unidentified network", and that gets the **Public profile**. Under
Public, ICMP echo requests and inbound TCP 25565 are blocked. Of the tunnel UDP, **replies to flows
we sent first** pass through stateful handling. This was confirmed by measurement. **Unsolicited
inbound is not.** In the same measurement all 6 cases were `blocked`. If that direction is needed,
an inbound allow rule must exist.

**Owner: client + person.** Profile classification and rule registration can be done by the client
when it has administrator rights. The Minecraft (Java) exception has to be checked by a person
because of section 12.

### Checking the default policy

The decision moved to [`tools/winprereq/Test-FirewallPolicy.ps1`](../../tools/winprereq/Test-FirewallPolicy.ps1).
The 225 cases are inside the script and run with `-SelfTest`.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\winprereq\Test-FirewallPolicy.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File tools\winprereq\Test-FirewallPolicy.ps1 -InterfaceAlias '<adapter name>'
powershell -NoProfile -ExecutionPolicy Bypass -File tools\winprereq\Test-FirewallPolicy.ps1 -SelfTest
```

**Pass condition. The last line is `VERDICT: pass` and the exit code is 0.** `unknown` (code 2) is
not a pass but "cannot decide". Before Phase 6, when our adapter does not exist, `unknown` is normal
when `-InterfaceAlias` is given. If even one confirmed defect exists, the verdict is `fail` even
when undecidable items are present too.

The script looks at three things. **What it looks at is this section's job; how it counts is the
script's job.**

- Is the default inbound policy of all three profiles Block
- **Is it not `BlockInboundAlways`.** That mode ignores every inbound allow rule. Registering a
  rule then does nothing, so this section and option a of
  [ADR 0002](decisions/0002-no-rebinding-recovery.md) are neutralised wholesale
- **Is the profile firewall enabled.** Even with a Block policy, inbound is not blocked when the
  firewall is off. Then the cause of an observed "blocked" is not the firewall

Measured output from this machine.

```
Domain Profile Settings:
----------------------------------------------------------------------
Firewall Policy                       BlockInbound,AllowOutbound

Private Profile Settings:
----------------------------------------------------------------------
Firewall Policy                       BlockInbound,AllowOutbound

Public Profile Settings:
----------------------------------------------------------------------
Firewall Policy                       BlockInbound,AllowOutbound
Ok.
```

All three profiles were `Enabled: True`.

**Do not filter lines by English words.** The **labels** of `netsh` are translated according to the
display language, so a filter like `Select-String 'Profile Settings|State'` catches nothing on a
Korean-display Windows. **The value `BlockInbound` is a keyword and is not translated.** That is
why the decision is made on values only.

> **Do not decide with `Get-NetFirewallProfile`.** On the same machine it shows this.
>
> ```
> Name                 : Public
> Enabled              : True
> DefaultInboundAction : NotConfigured
> ```
>
> **`NotConfigured` does not mean "not set"; it means "use the built-in default", and that default is Block.**
> A check written as `DefaultInboundAction -eq 'Block'` **is false on a healthy machine.** This is
> exactly the trap of recurrence-prevention rule 3 (first check that a correct implementation
> passes). The script pins this counterexample as a case.

### Checking the profile classification

```powershell
Get-NetConnectionProfile | Select-Object InterfaceAlias, NetworkCategory, IPv4Connectivity
```

Measured output. The physical Wi-Fi is already `Public`, and another tunnel adapter (Tailscale) is
`Private`.

```
InterfaceAlias NetworkCategory IPv4Connectivity
-------------- --------------- ----------------
Wi-Fi                   Public         Internet
Tailscale              Private     LocalNetwork
```

**Pass criterion.** After our adapter exists, look at the `NetworkCategory` of its entry. If it is
`Public`, either change it to Private or register a `-Profile Any` rule that is independent of the
profile.

**Use a narrowly scoped rule instead of changing the profile.** Changing the profile opens *every*
Private rule attached to that adapter, so the exposure is large, and **on an unidentified network it
can fail outright because of local security policy.** A Wintun adapter without a gateway is exactly
that case.

**`-Profile Any` alone is not enough.** It means "matches under any profile", so it opens on the
physical network as well. **Narrow it to our adapter and the exact protocol and port.**

```powershell
New-NetFirewallRule -DisplayName 'sangtachi-tunnel-in' `
  -Direction Inbound -Action Allow -Profile Any `
  -InterfaceAlias '<adapter name>' -Protocol TCP -LocalPort 25565
```

Without `-InterfaceAlias` the same rule also applies to Wi-Fi. If ICMP must be opened, use
`-Protocol ICMPv4 -IcmpType 8` the same way. **Write the type too.** Without the type the rule
opens all ICMP, and the script judges it `wide` or `unknown`.

**Some machines cannot use `-InterfaceAlias`.** On such machines the measures in this section
cannot be followed as written. The script's `scope-param` line decides that.

**Create both rules and leave them enabled.** TCP 25565 and ICMP echo are each needed; opening
only one leaves the other not working. A disabled rule does not count as coverage.
The script's `endpoint-coverage` line decides whether both are covered. A wide rule, or a rule
bound to the wrong adapter, does not count as coverage. **Whether the rule is properly narrowed is
decided by the script's `-InterfaceAlias` mode.**

If you insist on changing the classification, this is the command, and **since it can fail, check
the result.**

```powershell
Set-NetConnectionProfile -InterfaceAlias '<adapter name>' -NetworkCategory Private
Get-NetConnectionProfile -InterfaceAlias '<adapter name>' | Select-Object NetworkCategory
```

## 3. Cleaning up leftover adapters, addresses and routes

**Status: partly measured.** The query commands were run and a leftover adapter belonging to
**someone else** (OpenVPN TAP) was actually visible. **Leftovers from an abnormal exit of our own
client were not observed.** The adapter does not exist yet. Confirmed in Phase 6.

An abnormal exit leaves the adapter, address and route behind. Creating the same name again on the
next run gives a duplicate-creation error.

**Owner: client.** At start it looks for leftovers under its own name and reuses or deletes them.

```powershell
Get-NetAdapter -IncludeHidden | Select-Object Name, InterfaceDescription, Status
```

A leftover is actually visible in the measured output. `이더넷 4` is an unused OpenVPN TAP adapter
left in the disconnected state.

```
Name                     InterfaceDescription                       Status
----                     --------------------                       ------
이더넷 4                 TAP-Win32 Adapter V9                       Disconnected
Wi-Fi                    Realtek 8852CE WiFi 6E PCI-E NIC           Up
Tailscale                Tailscale Tunnel                           Up
vEthernet (FSE HostVnic) Hyper-V Virtual Ethernet Container Adapter Up
```

**Pass criterion.** If our adapter name already exists, do not create a new one. Reuse it, or
delete it and then create. **Do not delete unconditionally.** You could delete someone else's
adapter with a matching name. Whether we created it is also checked by whether
`InterfaceDescription` is Wintun.

**Look at all three.** Deleting the adapter alone removes the address and route with it, but there
is an intermediate state where the adapter is alive and only the address remains.

```powershell
function Invoke-NetQuerySafe([scriptblock]$Query) {
  try { return @(& $Query) }
  catch {
    $isNoMatch = ($_.CategoryInfo.Category -eq 'ObjectNotFound') -and
                 ($_.FullyQualifiedErrorId -like 'CmdletizationQuery_NotFound*')
    if ($isNoMatch) { return @() }
    throw
  }
}
@(Invoke-NetQuerySafe { Get-NetAdapter   -Name '<adapter name>'           -ErrorAction Stop }).Count
@(Invoke-NetQuerySafe { Get-NetIPAddress -InterfaceAlias '<adapter name>' -ErrorAction Stop }).Count
@(Invoke-NetQuerySafe { Get-NetRoute     -InterfaceAlias '<adapter name>' -ErrorAction Stop }).Count
```

**Pass criterion. All three must be `0` for a clean state.**

**Do not decide "absent" with `-ErrorAction SilentlyContinue`.** A permission failure or a service
fault produces the same empty output and **reports clean without seeing the leftovers.** This is
the same distinction as `Get-RouteSafe` in section 8, with the three queries wrapped in one
wrapper.

Measured confirmation. Running all three with a nonexistent name gave `0` for each, and a real
adapter gave `1`. The original errors were all `Category=ObjectNotFound`, and the IDs were
`CmdletizationQuery_NotFound_Name,Get-NetAdapter`,
`CmdletizationQuery_NotFound_InterfaceAlias,Get-NetIPAddress`,
`CmdletizationQuery_NotFound_InterfaceAlias,Get-NetRoute` respectively.

## 4. Wintun packaging and signature

**Status: unverified.** Wintun has not been brought in yet. That is Phase 6 work. The commands
below can be run after `wintun.dll` is placed in the distribution.

Wintun ships as a single DLL with the driver embedded in it. **The architecture must match**
(`bin/amd64/wintun.dll` for the x64 client).

**Owner: person (at build time) + client (check at run time).**

```powershell
$sig = Get-AuthenticodeSignature .\wintun.dll
$sig.Status
$sig.SignerCertificate.Subject
Get-FileHash .\wintun.dll -Algorithm SHA256 | Select-Object -ExpandProperty Hash
```

**Pass criterion. All three must hold.**

1. `Status` is `Valid`. `NotSigned` or `HashMismatch` means the distribution is corrupted or the
   file is wrong
2. `Subject` must be the **expected publisher**. `Valid` means "someone trusted signed it", so
   swapping in a different signed DLL also passes. Record the publisher string of the official
   Wintun release at packaging time and compare against it
3. The hash must equal the **published hash of the downloaded release**

Shape check. Running it against this machine's `kernel32.dll` gave `Status: Valid`,
`Subject: CN=Microsoft Windows, O=Microsoft Corporation, ...`, and with the expected publisher set
to `CN=WireGuard LLC` the comparison gave `False`. **That means a check that looks only at
`Status` would pass here.**

**This confirms the origin of the DLL; it does not guarantee that the driver loads.** The
Authenticode signature is on the DLL file; the kernel driver must separately pass catalog signing
and the driver signing policy. **Whether the driver loaded is confirmed by the result of adapter
creation.** On failure the cause is recorded in `C:\Windows\INF\setupapi.dev.log`.

The architecture is read from the PE header.

```powershell
$fs = [IO.File]::OpenRead('.\wintun.dll')
$br = New-Object IO.BinaryReader($fs)
$fs.Position = 0x3C; $pe = $br.ReadInt32(); $fs.Position = $pe + 4
'0x{0:X}' -f $br.ReadUInt16()
$br.Close()
```

**Pass criterion.** It must be `0x8664`. `0x14C` is x86 and `0xAA64` is ARM64.

**When blocked.** A driver load failure shows up as the adapter creation API failing. At that point
it is indistinguishable from missing administrator rights (section 1), so the client must check the
two conditions **separately** and say which one is the cause.

## 5. Minecraft server bind address

**Status: unverified.** The Minecraft server has not been run yet. That is Phase 8 work.

If `server-ip` in `server.properties` is set to **another interface's address**, connections
arriving at the virtual IP `10.100.0.1:25565` are refused. Left empty, it binds to all interfaces.

**Owner: person.** It is a configuration file on the server side.

Set this in `server.properties`.

```
server-ip=
server-port=25565
```

Check from the client PC after the server is up.

```powershell
Test-NetConnection -ComputerName 10.100.0.1 -Port 25565
```

**Pass criterion.** `TcpTestSucceeded : True` is required. On `False`, check in order.

1. Did the server process bind to that address — run the following on the server PC

```powershell
Get-NetTCPConnection -LocalPort 25565 -State Listen -ErrorAction SilentlyContinue |
  Select-Object LocalAddress, LocalPort, OwningProcess
```

If `LocalAddress` is `0.0.0.0` or `10.100.0.1`, pass. If only a single other address is present,
the problem is this section.

**If only `::` is shown, the result is undecidable.** Whether the IPv6 wildcard also accepts IPv4
depends on the socket's dual-stack setting, and this query cannot tell. In that case the result of
the `Test-NetConnection 10.100.0.1 -Port 25565` above is the decision. **Do not use
`netstat -ano | findstr 25565`** — it also catches lines whose remote port is 25565, and it does not
distinguish listening from not listening.
2. Is the server PC firewall blocking inbound 25565 — section 2
3. Is the tunnel carrying the packet — that is a tunnel layer problem, not this section

## 6. Control plane EC2

**Status: unverified.** The EC2 instance has not been launched yet. That is Phase 3 work.

Three things each block the connection.

| Item | Symptom |
|------|---------|
| The security group has no inbound rule | The connection times out. No response, not a refusal |
| The server bound to `127.0.0.1` | Works inside the instance and not from outside |
| The public IP changed on restart | Section 10 |

**Owner: person.**

Check the bind address inside the instance. **The control plane is Linux EC2** ([`spec.md`](spec.md)).
Port 8000 comes from `CONTROL_PORT` in [`control_plane.md`](control_plane.md) 2.6. If that value
changes, the port in the commands below changes with it. The following are Linux commands.

```bash
ss -ltnp 'sport = :8000'
```

**Decide with an IPv4-only query.**

```bash
ss -ltnp4 'sport = :8000'
```

**Pass criterion. `0.0.0.0:8000` or `*:8000` must appear.** If nothing appears, it is not
accepting over IPv4. `127.0.0.1:8000` is reachable only inside the instance.

**Do not treat a result showing only `[::]:8000` in the query above as a pass.** Under the Linux
default (`net.ipv6.bindv6only=0`) a dual-stack socket also accepts IPv4, but **that sysctl can be
changed.** That is why the IPv4-only query is the decision, and the final confirmation is the
external connection below.

Check reachability from the client PC.

```powershell
Test-NetConnection -ComputerName <EC2 public address> -Port 8000
```

**Pass criterion.** `TcpTestSucceeded : True`. On a timeout, check the security group first.

## 7. Subnet overlap

**Status: partly measured.** The decision moved to [`tools/winprereq/Test-SubnetOverlap.ps1`](../../tools/winprereq/Test-SubnetOverlap.ps1)
and it was run on this machine. The 85 cases are inside the script and run with `-SelfTest`.
**The path that actually excludes a live Wintun adapter was not observed.** The adapter does not
exist yet. That path was confirmed with 2 cases only; confirmation with the real thing is Phase 6.

If `10.100.0.0/24` overlaps the real LAN, a Hyper-V virtual switch or another VPN, traffic goes to
the wrong interface, or conversely **we shadow someone else's range.** Check before creating the
adapter.

**Owner: client.** On a block condition it picks an alternative range or stops.

**Windows picks routes by longest prefix match.** What we will create is a `/24` on-link route.
Overlapping routes are split into three classes **by prefix length.** **This table is the source of
the pass criterion.**

| Overlapping existing route | Longest-match result | Verdict |
|----------------------------|----------------------|---------|
| Length `> 24` (`10.100.0.5/32`) | **The existing one wins** | **Block** (`BLOCK-route`) |
| Length `= 24` (`10.100.0.0/24`) | Same length, so **the metric decides.** Creation itself may fail as a duplicate | **Block** (`BLOCK-route`) |
| Length `2~23` (`10.0.0.0/8`) | Our `/24` wins | **Block** (`BLOCK-shadow`). Our winning is the problem. It shadows the real hosts in that range |
| Length `0~1` (`0.0.0.0/0`, `0.0.0.0/1` of a full-tunnel VPN) | Our `/24` wins | **Normal.** `/0` exists on any ordinary internet-connected machine and `/1` comes from full-tunnel VPNs |

**The second row is a block, not a warning.** Our `/24` wins, so the tunnel works, but the user can
no longer reach the `10.100.0.x` hosts they used to reach. **Working while cutting off someone
else's is not better than not working.** An alternative-range mechanism exists, so the cost is
small. `-AllowShadow` exists for the case where a person knows that range is actually empty. The
default is block.

**Do not decide by string comparison.** `DestinationPrefix -like '10.100.0.*'` **misses overlaps
whose string differs.** Confirmed values.

```
10.100.0.0/24    like-match=True
10.100.0.5/32    like-match=True
10.100.0.0/23    like-match=True
10.100.0.0/16    like-match=True
10.0.0.0/8       like-match=False   <- overlaps but is missed
0.0.0.0/1        like-match=False   <- overlaps but is missed
```

`10.0.0.0/8` covers our range, so it is `BLOCK-shadow`, yet the string match does not catch it. The
more fundamental problem is that **the string does not tell you the prefix length.** Separating the
three classes above needs the length, so parse and compare.

**Check method.**

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\winprereq\Test-SubnetOverlap.ps1 -Target 10.100.0.0/24
powershell -NoProfile -ExecutionPolicy Bypass -File tools\winprereq\Test-SubnetOverlap.ps1 -SelfTest
```

**Pass condition. The last line is `VERDICT: proceed` and the exit code is 0.**

- `pick another range` (code 1) means change the range
- **`unknown` (code 2) is not a pass.** It means a query or a parse failed. Remove the cause and
  run again. If a definite block is present as well, `pick another range` wins
- Changing the target range with `-Target` moves the decision with it. Addresses are also turned
  into `/32` and fed into the same overlap check, so it does not go quietly wrong on a `/23` or
  `/20` alternative range. **Only lengths 2~32 are accepted.** `/0` and `/1` are not virtual LAN
  ranges, and they would collide with the `catch-all` exemption below and hide same-length
  conflicts

**There is an order. Run the section 3 cleanup first, then this check.** If our adapter is alive
with the address or route of a previous run, it overlaps our own range and **produces "change the
range" because of itself.** That is why our own adapter is excluded from the computation.

**Name and driver description are not proof of ownership.** Anyone else can use both identically.
Reading someone else's Wintun adapter with our name as ours makes **a real conflict slip through
quietly.** The only ownership mark is `InterfaceGuid`.

**Contract the client must keep.**

- Record the `InterfaceGuid` used when creating the adapter in its own state. Wintun lets you
  specify the GUID at creation, so the client is the side that knows the value
- Pass that value as `-OwnInterfaceGuid` when calling this check. `-OwnAlias` is not grounds for
  exclusion; it is only for checking once more that the name matches
- Delete the record when deleting the adapter. A stale GUID excludes nothing, so the decision can
  only err toward the strict side

Exclude only when exactly one adapter matches by GUID, its description is Wintun, and the name
matches too. If any one of those is off, **exclude nothing and write the reason in the output.** A
failed query goes the same way. **A false block is better than a false pass.** It is the same rule
as "do not delete someone else's adapter with a matching name" in section 3.

Measured output from this machine.

```
Verdict   Len Prefix    Interface
-------   --- ------    ---------
catch-all   0 0.0.0.0/0 Wi-Fi

BLOCK routes: 0
BLOCK addresses: 0
VERDICT: proceed
```

**The decision formula and its counterexamples are not kept in the document** (recurrence-prevention
rules 5, 6, 7). Rejecting prefix formats, `[int]''` becoming `0`, digit limits, IPv6 input,
comparing with the shorter mask, excluding our own adapter — all of it is in the script's case
table. A second copy of the table in the document gets fixed on one side only and drifts. The
previous version had actually drifted that way.

**When blocked.** Use an alternative range. When the range changes both sides must use the same
value, so it must be a value the control plane hands out. **If the client decides alone, the two
sides use different ranges.**

## 8. Duplicate on-link route creation

**Status: measured (automatic creation observed)**

When an address is assigned as `/24`, Windows **creates the on-link route automatically.** Adding
the same route explicitly afterwards gives an "already exists" error.

**Owner: client.**

In the measured route table, assigning one address produced three lines.

```
10.0.0.0    255.255.255.0   On-link   10.0.0.49   286     <- subnet on-link
10.0.0.49   255.255.255.255 On-link   10.0.0.49   286     <- host
10.0.0.255  255.255.255.255 On-link   10.0.0.49   286     <- broadcast
```

**Pass criterion.** After `New-NetIPAddress -PrefixLength 24`, do not create that subnet route
separately. If you must create it, check first.

```powershell
function Get-RouteSafe([string]$prefix) {
  try { return @(Get-NetRoute -DestinationPrefix $prefix -ErrorAction Stop) }
  catch {
    $isNoMatch = ($_.CategoryInfo.Category -eq 'ObjectNotFound') -and
                 ($_.FullyQualifiedErrorId -like 'CmdletizationQuery_NotFound*')
    if ($isNoMatch) { return @() }
    throw
  }
}
@(Get-RouteSafe '10.100.0.0/24').Count
```

**Decide by count.** Create on `0`, otherwise do not.

**Do not paper over it with `-ErrorAction SilentlyContinue`.** Then "absent" and "could not see it
because of missing permission or a service fault" become the same result. Misreading it as absent
and creating fails as a duplicate, and by then the cause is already gone. **Return 0 only for
absent, raise everything else.**

Measured confirmation. A nonexistent prefix gave `0`, an existing prefix (`10.0.0.0/24`) gave `1`,
and the original error was
`Category=ObjectNotFound`, `FullyQualifiedErrorId=CmdletizationQuery_NotFound_DestinationPrefix,Get-NetRoute`.

**The reason for not matching on shape is the same as `Get-TestRuleSafe` in
`unsolicited-firewall-test.ps1`.** That script ran into the same "no match" arriving in changing
exception shapes first. The leftover queries in section 3 need the same distinction. **Do not
swallow errors.** A duplicate error that may be ignored and a permission failure must be handled
differently.

## 9. Address tentative state

**Status: measured**

Right after an address is assigned, duplicate address detection (DAD) has not finished and the
address is in the `Tentative` state. Binding or sending at that point fails intermittently.

**Owner: client.** Wait until it becomes `Preferred`.

```powershell
Get-NetIPAddress -AddressFamily IPv4 |
  Select-Object InterfaceAlias, IPAddress, AddressState
```

Measured output. Both states exist at the same time on the same machine.

```
InterfaceAlias  IPAddress       AddressState
--------------  ---------       ------------
이더넷 4        169.254.187.141 Tentative
Wi-Fi           10.0.0.49       Preferred
Tailscale       100.102.127.59  Preferred
```

**Pass criterion.** Move to the next step after the `AddressState` of our address becomes
`Preferred`. **Do not wait by time.** A fixed wait like `Start-Sleep 2` breaks on a slow machine
and wastes time on a fast one. Poll the state.

**Alternative.** DAD has little meaning on a point-to-point tunnel adapter. Turning it off removes
the `Tentative` phase.

```powershell
Set-NetIPInterface -InterfaceAlias '<adapter name>' -AddressFamily IPv4 -DadTransmits 0
```

This value applies **to that interface only.** If you turn it off, leave a code comment saying why.

## 10. EC2 public IP change

**Status: unverified.** The instance has not been launched yet.

An **auto-assigned** public IPv4 changes when the instance is stopped and started. Hard-coding the
address in the client breaks the demo on the day. **An attached Elastic IP does not change.** That
is the measure below.

**Owner: person.** Attach an Elastic IP and point a DNS name at it. The client receives the DNS name
([`control_plane.md`](control_plane.md) 3.2). Not one of the two, but both. If only DNS is used and
the address behind it changes, a client that is already running keeps going to the old address,
because the client resolves once at startup ([`architecture.md`](architecture.md) 3.2.8).

```bash
aws ec2 describe-addresses \
  --filters "Name=instance-id,Values=<demo instance ID>" \
  --query 'length(Addresses)'
```

**Pass criterion. The output must be `1`.** `0` means no Elastic IP is attached and the address
changes on every restart. If 2 or more, a person checks what is attached.

If you need the list, look with the same filter.

```bash
aws ec2 describe-addresses \
  --filters "Name=instance-id,Values=<demo instance ID>" \
  --query 'Addresses[].{ip:PublicIp,assoc:InstanceId,alloc:AllocationId}'
```

**Do not query the whole account.** Counting without the filter lets **another project's Elastic
IP make the check pass.** Several EIPs in one account is common, and the fact that one of them is
attached says nothing about our instance.

**Cost.** A public IPv4 address is charged per hour **even while attached to an instance.** The old
rule of charging only while unattached no longer applies. When the demo is over, delete the
instance and release the Elastic IP too. **Pricing changes, so check the current pricing page before
use.**

## 11. Minecraft LAN discovery does not work

**Status: unverified (literature basis).** Not actually tried. Confirmed in Phase 8.

"Open to LAN" in Minecraft **Java Edition** advertises by **multicast.** The widely quoted value is
`224.0.2.60:4445`, but **it was not confirmed directly and may differ by edition and version.**

**The conclusion does not depend on that value.** Our tunnel is unicast point-to-point and **carries
no multicast at all.** Whatever the address, the peer's server does not appear in the "LAN games"
list.

**Owner: person.** The workaround is entering the address directly.

On the multiplayer screen, enter `10.100.0.1:25565` under `Direct Connection`.

**Pass criterion.** Connecting is normal. **Not appearing in the list is not a defect.** Treating
it as a defect and trying to fix the tunnel becomes out-of-scope work.

**Write this limitation into the demo script.** Saying it up front is better than explaining "why
does it not show up in the list" on the spot during the review.

## 12. Java path-based firewall exception

**Status: measured (rule existence confirmed)**

The firewall exception for the Minecraft server is registered by the **path** of `javaw.exe`.
Updating Java changes the version part of the path (`jdk-21.0.1` → `jdk-21.0.2`) and the exception
becomes void. The rule remains but does not work, so it is hard to notice.

**Owner: person.**

```powershell
Get-NetFirewallApplicationFilter | Where-Object { $_.Program -match 'java' } |
  Select-Object Program, @{n='Rule';e={($_ | Get-NetFirewallRule).DisplayName}}
```

This machine had **2** rules pointing at java.

**Pass criterion.** Each `Program` path shown must be **a file that actually exists.**

```powershell
Get-NetFirewallApplicationFilter | Where-Object { $_.Program -match 'java' } |
  ForEach-Object {
    $path = [Environment]::ExpandEnvironmentVariables($_.Program)
    '{0,-5} {1}' -f (Test-Path $path), $path
  }
```

```
True  C:\users\...\java-runtime-epsilon\windows-x64\java-runtime-epsilon\bin\javaw.exe
```

Any line whose first column is `False` is a dead rule. Register it again with the new path.

**Expand environment variables first.** A rule's `Program` may be of the form
`%ProgramFiles%\...`, and feeding it to `Test-Path` as-is **makes a perfectly good rule show
`False`.** On this machine `%ProgramFiles%\Common Files` was `False` before expansion and `True`
after.

**Do not print as a table.** Printing `Program` and `Exists` together with `Format-Table -AutoSize`
**truncates the verdict column** because the paths are long. On this machine the `Exists` column
actually disappeared. A verdict command that does not show the verdict is useless.

## 13. SmartScreen and Defender

**Status: partly measured.** Only the policy value was checked. **The pass condition (runs without
a warning on the demo PC) was not decided.** `client.exe` does not exist yet. Confirmed during
Phase 8 demo preparation.

SmartScreen is **reputation-based.** It does not block unconditionally for lack of a signature; it
shows "Windows protected your PC" for an executable with no reputation. Files downloaded from the
internet carry the Mark of the Web and are treated more strictly. **Our build artefact is at risk
of being caught here. This is an expectation, not an observation** — `client.exe` does not exist
yet. A locally built file usually does not carry MOTW, so whether a warning actually appears
**depends on how the binary was brought onto the demo PC.** Confirmed with that file in Phase 8.

**Owner: person.**

```powershell
(Get-ItemProperty 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\System' -Name EnableSmartScreen `
  -ErrorAction SilentlyContinue).EnableSmartScreen
```

This machine printed `1`.

**No output only means "there is no Group Policy override".** The effective behaviour is also set
by the Windows Security app or per-user settings, so do not conclude from this query alone. **The
valid decision is to actually run that binary on the demo PC.**

**First confirm what you are running.** The two methods below **turn off Windows' protection
against unsigned executables.** Turning it off without checking the origin creates exactly the
situation SmartScreen was trying to block.

```powershell
Get-FileHash .\client.exe -Algorithm SHA256
```

**Pass condition.** This value **must equal the hash of the artefact I built.** Compare it with the
value obtained by the same command on the build machine. If they differ, do not run it. If the
demo PC is the build machine, this step is unnecessary.

**Bypass after confirming. There are two methods. First, remove the Mark of the Web from the
downloaded file.**

```powershell
Unblock-File -LiteralPath .\client.exe
Get-Item -LiteralPath .\client.exe -Stream Zone.Identifier -ErrorAction SilentlyContinue
```

If the second command prints nothing, **that file** has no MOTW. Alternate data streams are an
NTFS feature, so files on FAT32 or exFAT media never have one in the first place. In that case no
output means "this file system does not have it", not "it was removed".

**Second, move it by USB or build locally.** A file that arrived by those routes **usually** does
not carry MOTW. **It is not a guarantee.** NTFS-formatted media can carry alternate data streams
intact, and extracting a downloaded archive with the default Windows Explorer can attach MOTW to
the contents. **It depends on the archive tool and the Windows version.** Whichever route it came
by, confirm with the `Zone.Identifier` query above.

**Pass criterion.** If the executable starts without a warning on the demo PC, pass.

**No code signing.** Certificate cost and issuance time do not fit the semester schedule. Instead,
**run it once in advance** on the demo PC.

**"Blocked only once" applies to that file only.** After allowing it, **the same file on the same
demo PC** does not ask again. Rebuilding the binary, moving it to another path, running it under a
different account or downloading it again **asks again.** If the binary was changed right before
the demo, run it once more with that file.

## 14. Automatic and manual

| # | Item | Who | When |
|---|------|-----|------|
| 1 | Administrator rights | person | every run |
| 2 | Firewall profile and rules | client | right after adapter creation |
| 3 | Leftover cleanup | client | at start |
| 4 | Bundling and signing the Wintun DLL | person (build) | when making the distribution |
| 5 | `server.properties` | person | when first setting up the server |
| 6 | EC2 security group and bind address | person | when setting up the instance |
| 7 | Subnet overlap check | client | before creating the adapter |
| 8 | Avoiding duplicate on-link routes | client | right after address assignment |
| 9 | Waiting for `Preferred` | client | right after address assignment |
| 10 | Elastic IP | person | when setting up the instance |
| 11 | Notice that LAN discovery does not work | person | in the demo script |
| 12 | Java path exception | person | after updating Java |
| 13 | SmartScreen bypass | person | first run on the demo PC |

**All 5 items owned by the client are implemented in Phase 6 or later.** For now this is a design
split.

## 15. Usability limits

The claim in [`spec.md`](spec.md) is "without manual port forwarding", and **that claim stands.**
No item in the table above requires router configuration.

**But the configuration burden did not disappear; its kind changed.** Instead it needs
administrator rights, driver loading, firewall rules and a server configuration file. Whether that
is easier than port forwarding **needs a separate argument, and this document does not make it.**
The final report treats it honestly.

To make the comparison explicit.

| | Port forwarding approach | This project |
|---|---|---|
| Router configuration | Required. The screen differs per model and it is impossible under CGNAT | None |
| Administrator rights | Not required | **Required** |
| Driver | Not required | **Required** (Wintun) |
| Firewall | 1 exception for the server port | Server port exception + adapter profile |
| Burden on both sides | Server side only | **Both sides** |

**Neither side is unilaterally easier.** Only in environments where the router cannot be touched
(CGNAT, dormitories, a router owned by parents) does this approach become the only option. That
condition is written as a premise of the report.
