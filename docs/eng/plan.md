# Plan

The current working checklist. The larger Phase-level plan is in [`roadmap.md`](roadmap.md).

The source is the follow-up table in section 5 of [`design-audit.md`](design-audit.md). The blocker
and warning numbers for each item are in sections 3 and 4 of that document.

> Korean version: [`../kor/plan.md`](../kor/plan.md)

## Follow-ups

| # | Step | Resolves | Status |
|---|------|----------|--------|
| 1 | Write the settled `protocol.md` | blockers 1,2,3,6-10 plus 13 warnings | done 2026-09-14 |
| 2 | Settle the concurrency model | blockers 4,5 plus 2 warnings | done 2026-09-15 |
| 3 | Add a Windows prerequisites section | blockers 11-17 plus 6 warnings | done 2026-09-21 |
| 4 | Fix the spec logic errors | blockers 18,19 plus 3 warnings | todo |
| 5 | Contingency for direct connection being impossible | blocker 20 | done 2026-09-20 |
| 6 | Loosen the over-tightened verification criteria | 2 warnings (4 criteria) | todo |

**Step 5 was completed by measurement on 2026-09-20.** It was handled first because it was the
most dangerous item. The worst case of blocker 20 was not observed and no fallback is being added
(5.7-5.9, [ADR 0001](decisions/0001-no-direct-connection-fallback.md)). A residual risk remains
if conditions change, so re-measurement gates are set at three points.

**Step 3 was completed on 2026-09-21.** The result is [`windows-prereq.md`](windows-prereq.md):
13 items, one section each, 8 of them written from commands actually run on Windows 11 (4 measured, 4 partly measured).

**Next is step 4 or step 6.** Both are document work and neither needs a peer.

The earlier recommendation is kept below for the record. It is better to write down the runtime prerequisites before
starting the Phase 1 implementation, and the step 5 measurements already produced observations
about firewall and NAT behaviour.

---

## 3. Windows Prerequisites Section

**Done 2026-09-21.** The result is [`windows-prereq.md`](windows-prereq.md).
How the verification criteria were met is recorded at the end of this section.

**Problem.** Nothing in the documents states the runtime prerequisites. The first run on a demo PC
will stall.

| To cover | Source |
|----------|--------|
| Administrator privileges, required for adapter creation and route configuration | blocker 11 |
| Firewall. A new adapter is classified into the Public profile, blocking inbound ICMP and TCP 25565; client UDP also depends on the first-run prompt | blocker 12 |
| Cleanup of leftover adapter, address, and routes after an abnormal exit, and preventing duplicate creation on the next run | blocker 13 |
| Wintun DLL and driver packaging, architecture (x64), signing | blocker 14 |
| A `server-ip` in `server.properties` pointing at a different interface address refuses connections to `10.100.0.1:25565`. It must be blank or explicitly `10.100.0.1` | blocker 15 |
| EC2 security group inbound rules, bind address (never `127.0.0.1`) | blocker 16 |
| Detecting a `10.100.0.0/24` collision with the real LAN, Hyper-V, or another VPN, and the fallback range | blocker 17 |
| The on-link `/24` route already exists, so creating it again errors | warning |
| Starting a test while the address is still tentative causes intermittent failures | warning |
| The public IP changes when EC2 restarts; Elastic IP or DNS | warning |
| Minecraft LAN discovery is multicast and does not work over a unicast tunnel; direct IP entry is the workaround | warning |
| A Java update invalidates path-based firewall exceptions | warning |
| SmartScreen and Defender quarantine an unsigned client | warning |

**The usability limitation is covered here too.** The claim in `spec.md` is "without manual port
forwarding", and that claim holds. What it costs instead is administrator privileges, a driver
install, and firewall rules. The configuration burden did not disappear; it changed kind, and
whether that is easier than port forwarding needs a separate argument. The report must state this
honestly.

**Verification.** Going end to end on a real PC is not possible right now: Phase 1 has only its
CMake setup, and nothing from the Winsock2 wrapper onward has started. So this item is verified
against the document itself.

- Each of the 13 items above has its own subsection
- Each subsection carries a check command and its **expected output** (`ipconfig`, `route print`,
  `netsh advfirewall`, `Get-NetAdapter`, `Get-NetFirewallProfile`). A command with no statement of
  what passing looks like is incomplete
- Automatic and manual steps are marked separately: what the client does versus what a person must
  set up beforehand

Actual end-to-end verification on a freshly installed PC belongs to the Phase 8 demo and is not a
completion condition for this item.

**Verification result (2026-09-21).**

| Criterion | Result |
|-----------|--------|
| Each of the 13 items has its own subsection | [`windows-prereq.md`](windows-prereq.md) sections 1-13 |
| How to check and what passes | In every section: 12 use a command and section 11 (LAN discovery) is a manual procedure. **4 measured and 4 partly measured**, with 5 marked `unverified` and the reason given |
| Automatic and manual marked separately | Section 14 table |

**The commands were run before going into the document, and two things came out of it**
(recurrence-prevention rule 6).

- `DefaultInboundAction` from `Get-NetFirewallProfile` is `NotConfigured` on a normal machine.
  A check written as `-eq 'Block'` **fails a correct machine.** The criteria list above named that
  cmdlet, but the document uses it only as a counterexample and decides with `netsh advfirewall`
- Printing the Java path check through `Format-Table -AutoSize` **cuts off the verdict column**
  because the path is long. It was changed to formatted-string output

---

## 4. Fix the spec Logic Errors

| To cover | Source |
|----------|--------|
| The C-5 priority order does not protect the minimum deliverable. Dropping P1 removes M-5 and dropping P3 removes M-6 along with the required course analysis. Within Phases 1-9 the only genuinely droppable tier is P2 (Wintun, virtual IP, and Minecraft, Phases 6-8); P4 is a stretch tier and falls outside the calculation | blocker 18 |
| The traceability table assigns M-5 to Phase 4, but `DATA` does not exist until Phase 5 | blocker 19 |
| M1 (Phases 1-4) and M2 (Phases 5-8) are both 5 weeks and unbalanced | warning |
| The Phase 4 keepalive verification requires an external UDP observation point that the design does not have | warning |
| The HTTPS IP lookup in the M-2 verification may take a different path than the UDP send | warning |

**Verification.**

- After C-5 is fixed, removing **only the tiers marked droppable** leaves M-1 through M-6 all
  holding. As currently written, dropping P1 or P3 does not, and that is precisely blocker 18. So
  this check is not "every tier can be removed" but "the droppable marking matches reality"
- Every M/T/A entry in the traceability table points at a Phase where the corresponding capability
  actually exists, checked against the per-Phase deliverable lists

---

## 5. Contingency for Direct Connection Being Impossible

**Blocker 20, the most dangerous one.** If both available home networks show destination-dependent
mapping, the minimum success is impossible. A relay is a post-Phase-9 stretch goal and cannot
rescue it. The whole project is hostage to an external condition nobody controls.

**Do this first.** Measure, even before Phase 2. The decision cannot be made without it. Two things
are needed, and **the first alone is not enough.**

1. **Mapping behavior.** Send a Binding Request to two public STUN servers and compare the observed
   endpoints. **Both requests must use the same bound UDP socket and the same local endpoint.**
   Separate sockets give different source ports and therefore different mappings, which cannot be
   told apart from destination dependence (the same reason as NFR-9).
2. **An actual punch trial.** Binding results reveal mapping behavior only; they say **nothing about
   filtering behavior.** Whether hole punching works depends on both, so attempt a real round trip
   between the two networks with a minimal punch script. Endpoints can be exchanged by hand; no
   control plane is needed.

Deciding on item 1 alone misses the case where the mapping is fine and filtering is what blocks it.

**Options.** None of them works while M-1 and M-4 stand as written, and that has to be acknowledged
first. The current minimum success is defined as direct UDP between two home networks: A is not
that topology, B is not direct, and C does not make the two required paths pass. **So whichever
option is chosen, the success criteria themselves must be revised with it.**

| Option | Content | Cost |
|--------|---------|------|
| A | A NAT emulation testbed producing the mapping behaviors to be verified | Not real-world evidence, which weakens the analysis. M-4's definition of "supported NAT environment" must widen to include emulation |
| B | Promote a minimal relay to P1 as a fallback when direct fails | Weakens the direct-first claim and adds a Phase. Fallback success must be defined as its own tier |
| C | Add a third path such as a mobile hotspot to the required test topology | CGNAT could make it worse. The required-topology contract must be revised |

**Verification.** Reflecting it in the documents is not completion. Choosing option C and having
that path fail too leaves blocker 20 exactly where it was.

- Record the measurement numerically: the observed endpoint per server, and whether the mapping
  changes with destination
- M-1, M-4, and the required test topology contract are **explicitly revised** to match the chosen
  option. Choosing an option without revising them leaves blocker 20 exactly where it was
- Under the revised criteria, the success path reproduces. The reproduction condition differs by
  option

| Option | Reproduction condition |
|--------|------------------------|
| A | Document the emulator configuration and succeed 3 times consecutively under it. The configuration is the controlled condition |
| B | Force a direct failure, then succeed 3 times consecutively over the fallback path |
| C | A hotspot is not under our control. Record carrier, plan, time of day, and signal state, and succeed 3 times consecutively **under those conditions**, noting that the result may change if they do |
- If the original "direct UDP between two home networks" is not achieved, state whether that is
  recorded as a separate tier or as a shortfall
- State what remains if that path also fails. If there is no contingency for the contingency, say so
- Record the decision and its basis as an ADR in `decisions/`

### 5.7 Measurement Results (complete, 2026-09-20)

A tool was built and 22 measurements were taken. The tool is in
[`../../tools/nat-probe/`](../../tools/nat-probe/); the filled records and raw JSON are under it
(not committed, because of the public IPs; see 5.9).

| Item | Value |
|------|-----|
| Measurements | 22 |
| Distinct networks | **7** (3 T-Mobile, 3 Comcast, 1 KT) |
| Mapping verdict | **`endpoint-independent` in all 22** |
| Punch result | **`success` in all 22** |
| Symmetric (destination-dependent) NAT | **0** |
| Network pairs attempted | 6. 0 failures |

| Measurement | Topology | NAT-NAT | Diff ISP | Win-Win | Unsolicited inbound |
|------|----------|:-------:|:--------:|:-------:|------------------------|
| 09-17 | 2 mobile hotspots, same carrier | yes | no | no | not tested |
| 09-18 | 2 Comcast homes, **3 in a row** | **yes** | no | no | not tested |
| 09-20 | US to Korea, **3 in a row** | no (KR has a public IP) | **yes** | **yes** | not tested |
| 09-20 | 2 Comcast homes, Windows | **yes** | no | **yes** | **`blocked`** |
| 09-20 | Washington to Michigan | **yes** | no | no | **`blocked`** |
| 09-20 | T-Mobile hotspot to Comcast home | **yes** | **yes** | no | **`blocked`** |

**Required test topology (2), "2 different home networks", succeeded 3 times in a row on 09-18.**
No condition changed between rounds and the end times agreed within 1 second each round.

**No single measurement satisfied all three conditions at once.** The combination is covered and
all 6 pairs attempted succeeded, but it was not proven within one measurement. This limit goes
into the report.

### 5.8 A Side Finding: the NAT Blocks Unsolicited Inbound

**This item was not in the original plan.** It surfaced while investigating why no Windows
firewall prompt appeared in the 09-20 measurement even though all inbound traffic arrived.

The default inbound action is `Block` and there is no allow rule for `python.exe`, yet the punch
succeeded. **Stateful UDP** explains it: because we sent first, the replies counted as solicited
traffic. Other explanations were not excluded. That left packets **from a source port we have
never sent to** untested.

An `--unsolicited` test was built and run on 3 pairs. **All 6 cases were `blocked`.**

| Host OS | Host firewall | Result |
|-----------|---------------|------|
| Windows 11 | Inbound default `Block`, no `python.exe` rule | `blocked` |
| macOS | Application Firewall off by default | `blocked` |
| Linux | `/etc/ufw/ufw.conf` says `ENABLED=no` (checked directly) | `blocked` |

**It was blocked even on hosts believed to have no active firewall filtering. The most likely
cause is not the host but port-restricted filtering in the NAT.** The macOS case is inferred from defaults and only `ufw`
was checked on Linux, so it is not stated as certain. The observations are merely consistent with
a port-restricted NAT.

Two things follow.

- **The client does not register firewall rules. This is provisional.** If the NAT is the cause,
  a rule would not help. For now no administrator rights requirement appears and the "without
  manual port forwarding" claim in `spec.md` stands. **However, the firewall is not excluded as
  the cause for directions where our Windows host is the receiver.** Adding one narrowly scoped temporary inbound
  rule on a Windows machine and re-running `--unsolicited` settles it. The condition that would overturn
  the decision is in [ADR 0002](decisions/0002-no-rebinding-recovery.md)
- **The premise of path validation in [`protocol.md`](protocol.md) 10.4 (c) breaks.** Of the three
  reasons 10.4 chose learning over nomination, **NAT rebinding recovery** is not guaranteed under the
  filtering observed in the tested environments. The decision is recorded in
  [ADR 0002](decisions/0002-no-rebinding-recovery.md) and applied to `protocol.md`

### 5.9 Conclusion and Remaining Actions

**Blocker 20 is resolved.** No fallback is added and M-1 and M-4 stay exactly as written. Options
A (emulation), B (promoting the relay to P1), and C (making the hotspot mandatory) were all
rejected. The reasoning and the accepted risks are in
[ADR 0001](decisions/0001-no-direct-connection-fallback.md).

**2 decisions.**

| Item | Decision |
|------|------|
| Whether to publish the raw measurements | **Keep them local.** `tools/nat-probe/results/` and `records/` are in `.gitignore`. This repository is public and the files carry the public IPs of two households with minute-level timestamps. Publishing a friend's home IP cannot be undone and was not agreed to. Attachments are not the raw files. A submission system and shared links create the same exposure. **Replace public addresses with irreversible pseudonyms (`network A`, `network B`) and keep timestamps only to the date.** Masking the last IPv4 octet is not enough and does not apply to IPv6. Otherwise obtain explicit consent from the people involved. State in the report which was done. **Cost:** the raw data does not follow the repository to another machine. The verdicts and the reasoning stay in this document and in the ADRs, so the conclusions are not lost |
| Writing the ADRs | **Done.** 0001 and 0002 in `decisions/` |

**Remaining actions.**

1. ~~Add "re-measure right before the demo" to the Phase 8 preparation steps~~ **Done.** It was
   added to the Phase 8 tasks in `roadmap.md`. This decision rests on observations at one point in
   time and can be invalidated by a router replacement or an ISP configuration change. There is no
   fallback for the fallback
2. Measure mapping lifetime during the Phase 4 keepalive work, and check whether the 15s interval
   actually prevents rebinding
3. Carry the limits of the evidence into the report: no measurement satisfied all three conditions
   at once, the sample leans toward Comcast, and one side of the different-ISP pair is a hotspot
---

## 6. Loosen the Over-Tightened Verification Criteria

The four items are not the same problem. One is a criterion a correct implementation cannot pass
(violating recurrence-prevention rule 3), two have no measurement method and so admit no verdict at
all, and one is invalid as evidence. They are handled separately.

| To cover | Kind | Source |
|----------|------|--------|
| Phase 7 "send and receive byte counters match the packet capture exactly, no tolerance". Capture point placement, NIC offload, and capture loss itself keep a correct implementation from matching exactly | unpassable | warning |
| Phase 8 game latency of ±30 ms, with no measurement method or measurement point, so no verdict is possible at all | underspecified | warning |
| A-2 fixes a controlled failure scenario (blocked UDP, nonexistent endpoint) as a required case. An artificial failure is not evidence about the real failure distribution | evidence validity | warning |
| Phase 9 statistical design is undefined: trial duration, independence, uncertainty reporting | underspecified | warning |

**Verification.** "Does a correct implementation pass it" is the starting question, not the test.
The completion condition differs by kind.

**Quantitative criteria** (Phase 7 counters, Phase 8 latency, Phase 9 statistics) need all four of
the following.

| Required | Meaning |
|----------|---------|
| Target document and section | Which criterion was changed |
| Measurement procedure | What is measured and how, including tool and measurement point |
| Tolerance and sample condition | A number, including sample size and the statistic (median, etc.) |
| Expected result | What value constitutes a pass |

**The qualitative criterion** (A-2) needs no numbers. It needs a statement of what the scenario is
evidence for and what it is not. An artificial failure is evidence that the failure handling path
works; it is not evidence about the failure rate in real environments. Without that distinction
A-2 becomes a wrong criterion again.

When loosening a criterion, state alongside it **what is no longer guaranteed.** A looser criterion
lets real defects through too, and without naming that range, loosening it is the same as deleting
it.

---

## Debt

| Item | Content |
|------|---------|
| `decisions/` is empty | The outcomes are written down: the single event loop in `architecture.md` 3.2, endpoint learning and the 20-byte header in `protocol.md`, relay scope in `spec.md` and `roadmap.md`. What is missing is a dedicated ADR recording **why the alternatives were rejected** |
| No `experiments.md` | A Phase 9 deliverable. Currently the only broken relative link |
| No record for `31e4240` | The CMake smoke commit has no `commit_history/` entry |
