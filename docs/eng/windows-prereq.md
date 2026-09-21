# Windows preconditions

What must be in place on a PC that runs the client for the first time, what the client handles
automatically, and what a person has to do beforehand.

The source is [`design-audit.md`](design-audit.md) chapter 3 section D (blockers 11-17) and the
6 platform warns in chapter 4. The work item is [`plan.md`](plan.md) number 3.

> 한국어 원본: [`../kor/windows-prereq.md`](../kor/windows-prereq.md)

## 0. How to read this

**Every section states how to check it and what counts as a pass.** A section without a pass
condition is not finished.

Most use a command, but **not all of them do.** Section 11 (LAN discovery) is a manual workaround
rather than a state to inspect, so it records the procedure and the pass condition instead of a
command.

Each section states its verification status.

| Mark | Meaning |
|------|---------|
| **Measured** | The check command was run on one Windows 11 24H2 machine on 2026-09-21 and its output decides the pass rule |
| **Partly measured** | The command was run, but **the pass rule is not decided yet.** What is missing is stated |
| **Unverified** | Not run. The reason and what would verify it are stated |

Right now that is 4 measured, 4 partly measured and 5 unverified.

**A failed query and an absent object are not the same.** A query that gates progress
(sections 3 and 8) returns 0 only for `ObjectNotFound` and raises everything else; reading a
permission failure as "absent" walks past leftovers. Sections 2 and 7 moved the decision into
scripts, and those report the same distinction as an `unknown` verdict. **`unknown` is not a
pass.** **Informational queries** (the policy value
and `Zone.Identifier` in section 13) use `-ErrorAction SilentlyContinue`, and each says on the
spot that absence is the normal case.

**"Measured" means the command runs on this machine, not that it was confirmed against the demo
artefact.** The client adapter does not exist yet, so any pass rule that targets our own adapter
is confirmed for real in Phase 6 or later.

**Output from one machine is a fact about that machine.** Adapter names, metrics and ranges
differ per machine. What transfers is not the value but the **pass rule**, which each section
states.

End-to-end verification on a clean PC belongs to the Phase 8 demo preparation and is not the
completion condition for this document. Phase 1 implementation has not started, so what the
client automates is a **design split**, not observed behaviour.

## 1. Administrator rights (blocker 11)

**Status: measured**

Creating the Wintun adapter, assigning the IP and adding the route all require administrator
rights. **That is a Windows API premise, not something measured on this machine**; what was
measured here is only whether the shell is elevated. Which of the three fails first, and with
which error, **is confirmed in Phase 6 when the adapter is first created.**

**Owner: person.** The client can elevate itself by relaunching through UAC, but the user has
to accept it.

```powershell
([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
  [Security.Principal.WindowsBuiltInRole]::Administrator)
```

The expected output is `True`. A PowerShell that was not started as administrator prints `False`.

**Pass rule.** Proceed only on `True`. On `False` the client stops before creating the adapter
and says what is missing. **Stopping before the work is better than failing in the middle of it**,
because a mid-run failure leaves the residue that section 3 deals with.

**This requirement already exists.** Registering a firewall rule (section 2) does not *create* a
new need for administrator rights. [ADR 0002](decisions/0002-no-rebinding-recovery.md) listed
"administrator rights appear" as a cost of option a, but adapter creation already needs them.
The marginal cost is "make one more rule with rights you already hold".

## 2. Firewall and network profile (blocker 12)

**Status: partly measured.** The default inbound policy and the profile category of existing
adapters were seen for real. **A freshly created virtual adapter being classified Public was not
observed**, because the adapter does not exist yet. Confirmed in Phase 6.

The Windows firewall **blocks inbound by default**. A freshly created virtual adapter has no
gateway, so it is classified as an unidentified network, which gets the **Public profile**. Under
Public, ICMP echo requests and inbound TCP 25565 are dropped. For tunnel UDP, **the reply to a flow we started**
passes through stateful handling ([`plan.md`](plan.md) 5.8, measured). **Unsolicited inbound does
not.** In the same measurement all 6 cases were `blocked`. An inbound allow rule is required if
that direction is needed.

**Owner: client and person.** With administrator rights the client can set the profile and
register rules. The Minecraft (Java) exception needs a person because of section 12.

### Checking the default policy

The decision moved into [`tools/winprereq/Test-FirewallPolicy.ps1`](../../tools/winprereq/Test-FirewallPolicy.ps1).
Its 225 cases live in the script and run under `-SelfTest`.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\winprereq\Test-FirewallPolicy.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File tools\winprereq\Test-FirewallPolicy.ps1 -InterfaceAlias '<adapter name>'
powershell -NoProfile -ExecutionPolicy Bypass -File tools\winprereq\Test-FirewallPolicy.ps1 -SelfTest
```

**Pass condition: the last line is `VERDICT: pass` and the exit code is 0.** `unknown` (code 2) is
not a pass; it means no decision could be made. Before Phase 6, with our adapter missing, passing
`-InterfaceAlias` returns `unknown` and that is normal. One confirmed defect makes the result
`fail` even when undecidable items are present too.

The script looks at three things. **What to look at belongs to this section; how to count belongs
to the script.**

- Whether the default inbound policy of the three profiles blocks
- **Whether it is not `BlockInboundAlways`.** That mode ignores every inbound allow rule.
  Registering a rule then does nothing, which takes out this section and option a of
  [ADR 0002](decisions/0002-no-rebinding-recovery.md) entirely
- **Whether the profile firewall is on.** Even with a blocking policy, a firewall that is off does
  not block inbound. Then a "blocked" observation has some other cause

Measured output on this machine:

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

**Do not filter lines by English words.** `netsh` **labels** are translated with the display
language, so a filter like `Select-String 'Profile Settings|State'` catches nothing on a
Korean-display Windows. **The value `BlockInbound` is a keyword and is not translated.** That is
why the decision uses values only.

> **`Get-NetFirewallProfile` does not decide this.** On the same machine it reports:
>
> ```
> Name                 : Public
> Enabled              : True
> DefaultInboundAction : NotConfigured
> ```
>
> **`NotConfigured` does not mean "not set"; it means "use the built-in default", and that default
> is Block.** A check written as `DefaultInboundAction -eq 'Block'` **returns false on a healthy
> machine.** This lands exactly on recurrence rule 3 (first check that a correct implementation
> passes). The script pins this counterexample as a case.

### Checking the profile category

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

**Pass rule.** After our adapter exists, read its `NetworkCategory`. If it is `Public`, either
switch it to Private or register a `-Profile Any` rule that does not depend on the profile.

**Prefer a narrowed rule over changing the profile.** Changing the profile opens *every* Private
rule attached to that adapter, which is a large exposure, and **on an unidentified network it can
fail outright because of local security policy.** A gateway-less Wintun adapter is exactly that
case.

**`-Profile Any` alone is not enough.** It means "applies under every profile", so it also opens on
the physical network. **Narrow it to our adapter and to the exact protocol and port.**

```powershell
New-NetFirewallRule -DisplayName 'sangtachi-tunnel-in' `
  -Direction Inbound -Action Allow -Profile Any `
  -InterfaceAlias '<adapter name>' -Protocol TCP -LocalPort 25565
```

Without `-InterfaceAlias` the same rule also applies to Wi-Fi. To open ICMP, use
`-Protocol ICMPv4 -IcmpType 8` the same way. **State the type.** Without it the rule opens every
ICMP type, and the script reports `wide` or `unknown`.

**Some machines cannot use `-InterfaceAlias`.** On those, the action in this section cannot be
followed as written. The script decides that on its `scope-param` line.

**Create both rules and leave them enabled.** TCP 25565 and ICMP echo are each required, and
opening one leaves the other closed. A disabled rule does not count as covering. The script decides coverage of both on its `endpoint-coverage` line. A wide rule, or a rule
bound to the wrong adapter, does not count as covering. **Whether the rule is narrowed correctly is decided by
the script's `-InterfaceAlias` mode.**

To change the category anyway, this is the command, and **it can fail, so check the result.**

```powershell
Set-NetConnectionProfile -InterfaceAlias '<adapter name>' -NetworkCategory Private
Get-NetConnectionProfile -InterfaceAlias '<adapter name>' | Select-Object NetworkCategory
```

## 3. Leftover adapters, addresses and routes (blocker 13)

**Status: partly measured.** The query commands were run and **somebody else's** leftover adapter
(an OpenVPN TAP) really was visible. **Residue from our own client exiting abnormally was not
observed**, because the adapter does not exist yet. Confirmed in Phase 6.

An abnormal exit leaves the adapter, the address and the routes behind. Creating them again under
the same name on the next run fails as a duplicate.

**Owner: client.** At startup it finds its own leftovers and either reuses or removes them.

```powershell
Get-NetAdapter -IncludeHidden | Select-Object Name, InterfaceDescription, Status
```

A leftover is visible in the measured output. `이더넷 4` is an unused OpenVPN TAP adapter left
behind in the disconnected state.

```
Name                     InterfaceDescription                       Status
----                     --------------------                       ------
이더넷 4                 TAP-Win32 Adapter V9                       Disconnected
Wi-Fi                    Realtek 8852CE WiFi 6E PCI-E NIC           Up
Tailscale                Tailscale Tunnel                           Up
vEthernet (FSE HostVnic) Hyper-V Virtual Ethernet Container Adapter Up
```

**Pass rule.** If our adapter name already exists, do not create another one. Reuse it, or remove
it and create it again. **Do not remove unconditionally**: a name collision could be somebody
else's adapter. Confirm that `InterfaceDescription` says Wintun as well.

**Look at all three.** Removing the adapter takes the address and routes with it, but there is an
in-between state where the adapter is alive and only the address is stale.

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

**Pass rule. All three must be `0` for a clean state.**

**Do not decide "absent" with `-ErrorAction SilentlyContinue`.** A permission or service failure
produces the same silence and **reports a clean state while leftovers are still there.** This is
the same distinction as `Get-RouteSafe` in section 8, with the three probes behind one wrapper.

Measured. Running all three against a missing name gave `0` each, and an existing adapter gave
`1`. The original errors were all `Category=ObjectNotFound`, with ids
`CmdletizationQuery_NotFound_Name,Get-NetAdapter`,
`CmdletizationQuery_NotFound_InterfaceAlias,Get-NetIPAddress` and
`CmdletizationQuery_NotFound_InterfaceAlias,Get-NetRoute`.

## 4. Wintun packaging and signature (blocker 14)

**Status: unverified.** Wintun has not been adopted yet; that is Phase 6 work. The commands below
can run once `wintun.dll` is part of the build output.

Wintun ships as a single DLL with the driver inside it. **The architecture has to match**: a x64
client needs `bin/amd64/wintun.dll`.

**Owner: person at build time, client at run time.**

```powershell
$sig = Get-AuthenticodeSignature .\wintun.dll
$sig.Status
$sig.SignerCertificate.Subject
Get-FileHash .\wintun.dll -Algorithm SHA256 | Select-Object -ExpandProperty Hash
```

**Pass rule. All three have to hold.**

1. `Status` is `Valid`. `NotSigned` or `HashMismatch` means the build output is damaged or the
   wrong file
2. `Subject` is **the expected publisher**. `Valid` only means "somebody trusted signed this", so
   a different signed DLL dropped in its place also passes. Record the publisher string of the
   official Wintun release at packaging time and compare against it
3. The hash matches **the published hash of the release** that was downloaded

Shape check: run against `kernel32.dll` on this machine it gave `Status: Valid` and
`Subject: CN=Microsoft Windows, O=Microsoft Corporation, ...`, and with the expected publisher set
to `CN=WireGuard LLC` the comparison returned `False`. **That is what `Status` alone lets
through.**

**This checks the provenance of the DLL; it does not guarantee the driver loads.** Authenticode
covers the DLL file, while the kernel driver has to satisfy catalog signing and the driver signing
policy separately. **Whether the driver loaded is decided by the adapter creation result**, and a
failure leaves its cause in `C:\Windows\INF\setupapi.dev.log`.

The architecture is read out of the PE header.

```powershell
$fs = [IO.File]::OpenRead('.\wintun.dll')
$br = New-Object IO.BinaryReader($fs)
$fs.Position = 0x3C; $pe = $br.ReadInt32(); $fs.Position = $pe + 4
'0x{0:X}' -f $br.ReadUInt16()
$br.Close()
```

**Pass rule.** It must print `0x8664`. `0x14C` is x86 and `0xAA64` is ARM64.

**When it fails.** A driver that does not load shows up as a failing adapter-creation call, which
looks the same as missing administrator rights (section 1). The client therefore has to check the
two conditions **separately** and say which one is the cause.

## 5. Minecraft server bind address (blocker 15)

**Status: unverified.** No Minecraft server has been run yet; that is Phase 8 work.

If `server-ip` in `server.properties` points at **a different interface address**, a connection
to the virtual IP `10.100.0.1:25565` is refused. Leaving it empty binds to every interface.

**Owner: person.** It is a server-side configuration file.

Set it in `server.properties` like this.

```
server-ip=
server-port=25565
```

Check it from the client PC once the server is up.

```powershell
Test-NetConnection -ComputerName 10.100.0.1 -Port 25565
```

**Pass rule.** `TcpTestSucceeded : True`. On `False`, look in this order.

1. Did the server process bind that address: run this on the server PC

```powershell
Get-NetTCPConnection -LocalPort 25565 -State Listen -ErrorAction SilentlyContinue |
  Select-Object LocalAddress, LocalPort, OwningProcess
```

A `LocalAddress` of `0.0.0.0` or `10.100.0.1` passes. A single different address is this
section's problem.

**`::` on its own is inconclusive.** Whether the IPv6 wildcard also accepts IPv4 depends on the
socket's dual-stack setting, which this query does not show. In that case the
`Test-NetConnection 10.100.0.1 -Port 25565` result above is the verdict. **Do not use `netstat -ano | findstr 25565`**: it
also matches rows whose remote port is 25565 and does not separate listening sockets.
2. Does the server PC firewall block inbound 25565: section 2
3. Does the tunnel carry that packet: a tunnel-layer problem, not this section

## 6. Control plane on EC2 (blocker 16)

**Status: unverified.** No EC2 instance is running yet; that is Phase 3 work.

Three separate things block the connection.

| Item | Symptom |
|------|---------|
| The security group has no inbound rule | The connection times out. No refusal, just silence |
| The server bound `127.0.0.1` | It works inside the instance and nowhere else |
| The public IP changed on restart | Section 10 |

**Owner: person.**

Inside the instance, look at the bind address. **The control plane runs on Linux EC2**
([`spec.md`](spec.md)), so the command below is a Linux one.

```bash
ss -ltnp 'sport = :8000'
```

**Decide with the IPv4-only query.**

```bash
ss -ltnp4 'sport = :8000'
```

**Pass rule. It must show `0.0.0.0:8000` or `*:8000`.** No output means nothing accepts IPv4.
`127.0.0.1:8000` is reachable only inside the instance.

**Do not treat a bare `[::]:8000` in the earlier output as a pass.** With the Linux default
(`net.ipv6.bindv6only=0`) a dual-stack socket does accept IPv4, but **that sysctl can be
changed.** So the IPv4-only query is the verdict, and the external connection below is the final
confirmation.

From the client PC, check that it is reachable.

```powershell
Test-NetConnection -ComputerName <EC2 public address> -Port 8000
```

**Pass rule.** `TcpTestSucceeded : True`. A timeout points at the security group first.

## 7. Subnet conflict (blocker 17)

**Status: partly measured.** The decision moved into [`tools/winprereq/Test-SubnetOverlap.ps1`](../../tools/winprereq/Test-SubnetOverlap.ps1)
and was run on this machine. Its 85 cases live in the script and run under `-SelfTest`.
**Excluding a live Wintun adapter was not observed**, because the adapter does not exist yet. That
path is covered by 2 cases only; the real check is Phase 6.

If `10.100.0.0/24` overlaps the real LAN, a Hyper-V virtual switch or another VPN, traffic leaves
through the wrong interface, or the other way round, **we shadow somebody else's range.** Check
before creating the adapter.

**Owner: client.** On a blocking condition it picks an alternative range or stops.

**Windows selects a route by longest prefix match**, and what we will add is an on-link `/24`.
Overlapping routes split into three classes **by prefix length**. **This table is the source of the
decision rule.**

| Overlapping existing route | Longest match result | Verdict |
|----------------------------|----------------------|---------|
| Length `> 24` (`10.100.0.5/32`) | **The existing one wins** | **Block** (`BLOCK-route`) |
| Length `= 24` (`10.100.0.0/24`) | Same length, so **the metric decides.** Creation itself may fail as a duplicate | **Block** (`BLOCK-route`) |
| Length `2-23` (`10.0.0.0/8`) | Our `/24` wins | **Block** (`BLOCK-shadow`). Winning is the problem: we shadow the real hosts in that range |
| Length `0-1` (`0.0.0.0/0`, a full-tunnel VPN's `0.0.0.0/1`) | Our `/24` wins | **Normal.** `/0` is there on a normal internet-connected machine; `/1` comes from a full-tunnel VPN |

**The second row blocks rather than warns.** Our `/24` wins, so the tunnel works, but the user
loses the `10.100.0.x` hosts they could reach before. **Working while cutting somebody else off is
not better than not working.** The alternative-range mechanism makes the cost small. For the case
where a person knows that range is empty, `-AllowShadow` exists. The default is to block.

**A string comparison does not decide this.** `DestinationPrefix -like '10.100.0.*'` **misses
overlaps whose text differs.** Measured values:

```
10.100.0.0/24    like-match=True
10.100.0.5/32    like-match=True
10.100.0.0/23    like-match=True
10.100.0.0/16    like-match=True
10.0.0.0/8       like-match=False   <- overlaps but is missed
0.0.0.0/1        like-match=False   <- overlaps but is missed
```

`10.0.0.0/8` covers our range, so it is `BLOCK-shadow`, yet the string never matches. The deeper
problem is that **a string does not carry the prefix length.** Splitting the three classes above
needs the length, so the prefix is parsed and compared.

**How to check.**

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\winprereq\Test-SubnetOverlap.ps1 -Target 10.100.0.0/24
powershell -NoProfile -ExecutionPolicy Bypass -File tools\winprereq\Test-SubnetOverlap.ps1 -SelfTest
```

**Pass condition: the last line is `VERDICT: proceed` and the exit code is 0.**

- `pick another range` (code 1) means change the range
- **`unknown` (code 2) is not a pass.** A query or a parse failed. Remove the cause and run again.
  When a definite block is present as well, `pick another range` wins
- Changing the target with `-Target` carries the decision with it. Addresses go through the same
  overlap check as `/32`, so a `/23` or `/20` alternative range does not fail silently. **Only
  lengths 2-32 are accepted.** `/0` and `/1` are not virtual LAN ranges, and they collide with the
  `catch-all` exemption below, hiding an equal-length conflict

**Order matters. Run the cleanup in section 3 first, then this check.** If our adapter is still
alive with an address or a route from an earlier run, that overlaps our own range and **the machine
tells itself to pick another range.** Our own adapter is excluded from the calculation.

**A name and a driver description are not proof of ownership.** Anyone can use both. Reading a
foreign Wintun adapter that carries our name as ours makes **a real conflict disappear silently.**
The ownership marker is the `InterfaceGuid`, and nothing else.

**The contract the client must keep:**

- Record the `InterfaceGuid` used to create the adapter. Wintun accepts a GUID at creation, so the
  client is the side that knows the value
- Pass it as `-OwnInterfaceGuid` when calling this check. `-OwnAlias` is not grounds for exclusion;
  it only re-checks that the name matches
- Delete the record when the adapter is deleted. A stale GUID excludes nothing, so the error can
  only go toward a stricter verdict

Exclusion happens only when the GUID resolves to exactly one adapter, that adapter has a Wintun
description, and the name matches too. On any mismatch **nothing is excluded and the reason is
printed.** A failed query lands on the same side. **A false block beats a false pass.** This is the
rule section 3 uses for "do not delete a foreign adapter with a colliding name."

Measured output on this machine:

```
Verdict   Len Prefix    Interface
-------   --- ------    ---------
catch-all   0 0.0.0.0/0 Wi-Fi

BLOCK routes: 0
BLOCK addresses: 0
VERDICT: proceed
```

**The predicate and its counterexamples do not live in this document** (recurrence rules 5, 6, 7).
Prefix format rejection, `[int]''` becoming `0`, the digit limit, IPv6 input, comparing with the
shorter mask, and excluding our own adapter are all in the script's case table. A second copy here
would be updated on one side only. The previous revision had drifted exactly that way.

**When blocked.** Use an alternative range. Both sides must use the same value, so it comes from
the control plane. **If the client decides alone, the two sides use different ranges.**

## 8. Duplicate on-link route (warn)

**Status: measured (automatic creation observed)**

Assigning an address with a `/24` prefix makes Windows **create the on-link route automatically.**
Adding the same route explicitly afterwards fails with an "already exists" error.

**Owner: client.**

In the measured route table one address assignment produced three lines.

```
10.0.0.0    255.255.255.0   On-link   10.0.0.49   286     <- subnet on-link
10.0.0.49   255.255.255.255 On-link   10.0.0.49   286     <- host
10.0.0.255  255.255.255.255 On-link   10.0.0.49   286     <- broadcast
```

**Pass rule.** After `New-NetIPAddress -PrefixLength 24`, do not add that subnet route separately.
If it has to be added, check first.

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

**Decide on the count.** Create it on `0`, skip it otherwise.

**Do not paper over it with `-ErrorAction SilentlyContinue`.** That makes "absent" and "could not
look because of a permission or service failure" the same result. Reading it as absent and
creating the route fails as a duplicate, and by then the cause is gone. **Return 0 only for
absent, and raise everything else.**

Measured. The missing prefix gave `0`, an existing one (`10.0.0.0/24`) gave `1`, and the original
error was `Category=ObjectNotFound` with
`FullyQualifiedErrorId=CmdletizationQuery_NotFound_DestinationPrefix,Get-NetRoute`.

**The reason for not catching by type is the same as `Get-TestRuleSafe` in
`unsolicited-firewall-test.ps1`**, where the same "no matching object" arrived under changing
exception types. The leftover queries in section 3 need the same distinction. **Do not swallow the error.** A
duplicate that can be ignored and a permission failure are not the same thing.

## 9. Tentative address state (warn)

**Status: measured**

Right after assignment, duplicate address detection has not finished and the address is
`Tentative`. Binding or sending in that window fails intermittently.

**Owner: client.** Wait until it is `Preferred`.

```powershell
Get-NetIPAddress -AddressFamily IPv4 |
  Select-Object InterfaceAlias, IPAddress, AddressState
```

Measured output. Both states exist on the same machine at once.

```
InterfaceAlias  IPAddress       AddressState
--------------  ---------       ------------
이더넷 4        169.254.187.141 Tentative
Wi-Fi           10.0.0.49       Preferred
Tailscale       100.102.127.59  Preferred
```

**Pass rule.** Move on only after our address reports `Preferred`. **Do not wait on a clock.** A
fixed `Start-Sleep 2` breaks on a slow machine and wastes time on a fast one. Poll the state.

**Alternative.** DAD means little on a point-to-point tunnel adapter. Turning it off removes the
`Tentative` window.

```powershell
Set-NetIPInterface -InterfaceAlias '<adapter name>' -AddressFamily IPv4 -DadTransmits 0
```

The setting applies **to that interface only**. If it is turned off, record why in a code comment.

## 10. EC2 public IP changes (warn)

**Status: unverified.** No instance is running yet.

An **auto-assigned** public IPv4 changes when the instance is stopped and started. An address
baked into the client breaks the demo on the day. **An Elastic IP does not change**, which is the
fix below.

**Owner: person.** Attach an Elastic IP or use a DNS name.

```bash
aws ec2 describe-addresses \
  --filters "Name=instance-id,Values=<demo instance id>" \
  --query 'length(Addresses)'
```

**Pass rule. The output must be `1`.** `0` means no Elastic IP is attached and the address changes
on every restart. Anything above 1 needs a person to look at what is attached.

Use the same filter when the list itself is wanted.

```bash
aws ec2 describe-addresses \
  --filters "Name=instance-id,Values=<demo instance id>" \
  --query 'Addresses[].{ip:PublicIp,assoc:InstanceId,alloc:AllocationId}'
```

**Do not query the whole account.** Counting without a filter lets **an Elastic IP from another
project pass the check.** Several EIPs in one account is normal, and one of them being attached
says nothing about our instance.

**Cost.** A public IPv4 address is billed per hour **even while attached** to a running instance.
The old rule, where only an unattached address cost money, no longer applies. After the demo,
terminate the instance and release the Elastic IP. **Pricing changes, so check the current
pricing page before relying on this.**

## 11. Minecraft LAN discovery does not work (warn)

**Status: unverified (documented behaviour).** Not tried yet; it will be checked in Phase 8.

Minecraft **Java Edition** advertises an "Open to LAN" world over **multicast**. The widely quoted
endpoint is `224.0.2.60:4445`, but **we did not verify it and it can differ by edition and
version.**

**The conclusion does not depend on that value.** Our tunnel is a unicast point-to-point link and
**carries no multicast at all**, so whatever the address is, the peer's server never appears in
the LAN game list.

**Owner: person.** The workaround is to type the address.

Use `Direct Connect` on the multiplayer screen with `10.100.0.1:25565`.

**Pass rule.** Connecting is a pass. **Not appearing in the list is not a defect.** Treating it as
one and trying to fix the tunnel turns into out-of-scope work.

**Put this limit in the demo script.** Saying it up front beats explaining it live when an examiner
asks why the server is not listed.

## 12. Path-based Java firewall exception (warn)

**Status: measured (rules found)**

The firewall exception for a Minecraft server is registered against the **path** of `javaw.exe`.
A Java update changes the version part of that path (`jdk-21.0.1` to `jdk-21.0.2`) and the
exception stops matching. The rule is still there and no longer does anything, which makes it hard
to notice.

**Owner: person.**

```powershell
Get-NetFirewallApplicationFilter | Where-Object { $_.Program -match 'java' } |
  Select-Object Program, @{n='Rule';e={($_ | Get-NetFirewallRule).DisplayName}}
```

This machine had **2** rules pointing at java.

**Pass rule.** Each `Program` path must be **a file that exists**.

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

A line whose first column is `False` is a dead rule. Register it again against the new path.

**Expand environment variables first.** A rule's `Program` can be of the form
`%ProgramFiles%\...`, and feeding that straight to `Test-Path` **reports a healthy rule as
`False`.** On this machine `%ProgramFiles%\Common Files` was `False` unexpanded and `True`
expanded.

**Do not print it as a table.** Sending `Program` and `Exists` through `Format-Table -AutoSize`
**cuts the decision column off** because the path is long. The `Exists` column actually
disappeared on this machine. A check command that hides its own verdict is useless.

## 13. SmartScreen and Defender (warn)

**Status: partly measured.** Only the policy value was read. **The pass rule (starting without a
warning on the demo PC) is not decided**, because `client.exe` does not exist yet. It is confirmed
in the Phase 8 demo preparation.

SmartScreen is **reputation based**. It does not block every unsigned file outright; it shows
"Windows protected your PC" for an executable with no reputation. A file downloaded from the
internet carries the Mark of the Web, which makes it stricter. **Our build output is at risk here. That is an expectation, not an observation** — `client.exe`
does not exist yet. A locally built file usually carries no MOTW, so whether a warning actually
appears **depends on how the binary reached the demo PC.** Phase 8 checks it with that file.

**Owner: person.**

```powershell
(Get-ItemProperty 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\System' -Name EnableSmartScreen `
  -ErrorAction SilentlyContinue).EnableSmartScreen
```

This machine printed `1`.

**No output only means there is no Group Policy override.** The effective setting can also come
from the Windows Security app or a per-user setting, so this query alone decides nothing. **The
valid check is launching that binary on the demo PC.**

**First confirm what is being run.** Both ways below **turn off Windows protection for an
unsigned executable.** Turning it off without checking provenance creates exactly the situation
SmartScreen exists to stop.

```powershell
Get-FileHash .\client.exe -Algorithm SHA256
```

**Pass rule.** The value must **match the hash of the build output**, taken with the same command
on the build machine. If it differs, do not run the file. The step is unnecessary when the demo PC
is the build machine.

**After that check, there are two ways around it. First, strip the Mark of the Web from the
file.**

```powershell
Unblock-File -LiteralPath .\client.exe
Get-Item -LiteralPath .\client.exe -Stream Zone.Identifier -ErrorAction SilentlyContinue
```

No output from the second command means there is no MOTW **on that file**. Alternate data streams
are an NTFS feature, so a file on FAT32 or exFAT media never has one; there, no output means "not
possible on this file system" rather than "removed".

**Second, move it over on a USB stick or build it locally.** A file that arrives that way
**usually** has no MOTW. **That is not a guarantee.** NTFS-formatted media can carry alternate data
streams as they are, and extracting a downloaded archive with Explorer can propagate the MOTW.
**It depends on the extractor and the Windows version.** Whichever path was used, confirm it with
the `Zone.Identifier` query above.

**Pass rule.** The executable starts on the demo PC without a warning.

**We are not code signing.** The certificate cost and issuing time do not fit the term schedule.
Instead, **run the client once on the demo PC beforehand.**

**"Only the first run" applies to that one file.** After it is allowed, **the same file on the
same demo PC** stops asking. Rebuilding the binary, moving it to another path, running it as a
different user, or downloading a fresh copy **brings the prompt back.** If the binary changed
right before the demo, run that file once more.

## 14. Automatic and manual

| # | Item | Who | When |
|---|------|-----|------|
| 1 | Administrator rights | Person | Every run |
| 2 | Firewall profile and rules | Client | Right after adapter creation |
| 3 | Leftover cleanup | Client | At startup |
| 4 | Shipping and signing `wintun.dll` | Person (build) | When the build output is made |
| 5 | `server.properties` | Person | When the server is first set up |
| 6 | EC2 security group and bind address | Person | When the instance is set up |
| 7 | Subnet conflict check | Client | Before creating the adapter |
| 8 | Avoiding the duplicate on-link route | Client | Right after address assignment |
| 9 | Waiting for `Preferred` | Client | Right after address assignment |
| 10 | Elastic IP | Person | When the instance is set up |
| 11 | Telling people LAN discovery fails | Person | In the demo script |
| 12 | Java path exception | Person | After a Java update |
| 13 | Getting past SmartScreen | Person | First run on the demo PC |

**All 5 client-side items land in Phase 6 or later.** For now they are a design split.

## 15. Usability limits

The claim in [`spec.md`](spec.md) is "without manual port forwarding" and **that claim holds.**
Nothing in the table above asks anyone to configure a router.

**But the setup burden did not disappear; it changed kind.** Administrator rights, a driver load,
firewall rules and a server configuration file take its place. Whether that is easier than port
forwarding **needs a separate argument, and this document does not make it.** The final report
treats it honestly.

The comparison, stated plainly.

| | Port forwarding | This project |
|---|---|---|
| Router configuration | Required. Every model differs, and CGNAT makes it impossible | None |
| Administrator rights | Not needed | **Needed** |
| Driver | Not needed | **Needed** (Wintun) |
| Firewall | One server-port exception | Server port exception plus the adapter profile |
| Who carries it | The server side only | **Both sides** |

**Neither side is plainly easier.** This approach is the only option where the router cannot be
touched: CGNAT, a dormitory, a router owned by a parent. That condition is stated as a premise in
the report.
