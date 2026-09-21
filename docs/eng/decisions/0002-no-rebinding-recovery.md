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

**The decisive test lives in `tools/nat-probe/README.md` chapter 12.** It does not turn the
firewall off; it adds and removes one narrowly scoped temporary inbound allow rule and
re-measures. That procedure exercises exactly what option a would do.

The procedure is not kept here because of rule 6 in
[`../design-audit.md`](../design-audit.md) chapter 6: a script for a test that has not been run
belongs in the tool, and the decision record only points at it.

- Procedure: [`tools/nat-probe/README.md`](../../../tools/nat-probe/README.md) 12.1
- Reading the result: 12.2 in the same document

Until that test is run, **"the client does not register firewall rules" is a provisional
decision.**

## Alternatives

| Option | Content | Reason for rejection |
|----|------|-----------|
| a | Have the client register an inbound allow rule | **Deferred, not rejected.** It was blocked even on hosts believed to have no active firewall filtering (macOS, Linux), so the NAT is the likely cause, in which case a rule would not help. It also adds a caveat to the "without manual port forwarding" claim in `spec.md`. **Administrator rights are not a cost**: adapter creation already needs them (blocker 11, [`windows-prereq.md`](../windows-prereq.md) section 1). Corrected 2026-09-21. **However, the firewall has not been excluded as the cause for directions where Windows is the receiver.** See the revisit condition below |
| b | Predict the peer's port range and send to several ports at once | Ports under destination-dependent mapping are not reliably predictable. It increases traffic and risks conflicting with the candidate hygiene rule in 10.1 (do not fire at third parties from an unbounded candidate list) |
| c | Add a rebinding renegotiation trigger to the control plane | It grows the scope of Phase 3. Rebinding itself is rare in a 30 minute demo, and the 15s keepalive keeps the mapping alive. Left out of v1 scope |
| d | Delete 10.4 (c) entirely | Path validation is the mechanism that stops an attacker from redirecting traffic to a third party with a forged source address (the security argument in 10.4). Deleting it removes that defence |

## Consequences

**What this gains.**

- The "without manual port forwarding" claim in `spec.md` stands
- **The original line, "no administrator rights requirement appears", was wrong.** Adapter
  creation already needs them ([`windows-prereq.md`](../windows-prereq.md) section 1). What this
  decision buys is not avoided rights but an unregistered rule. Corrected 2026-09-21, and **the
  cost of option a drops accordingly**
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
