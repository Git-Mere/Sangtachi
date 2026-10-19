# Decisions before Phase 7 (ADR 0029)

## Why

The next step in `plan.md` was Phase 7. Before starting, the documents and the code were compared, and the places that were not decided
or that collide with the current code were shown to the repository owner with options. The owner chose that the client creates the
firewall rules and does not remove them (B), and said an existing rule should be used after minimal verification. The verification design
was reported separately and approved. For the other places the owner took the recommendations. The decision and the rejected alternatives
are ADR 0029. This commit does not change client code. It changes only documents, the pending list and name shape of the document gate, and the measurement script. The implementation is the next commits.

## What changed

| What | Where |
|------|------|
| The two rules the client creates, the group, the checks, the failure behavior, the check beyond the minimum, where the judging script lives, the GUID measurement, the removal command. The status and owner lines at the head and row 2 of the section 14 table | `windows-prereq.md` 2 and 14 |
| A firewall rule verification row in the adapter lifetime table. The shutdown row says the rules are not removed | `architecture.md` chapter 7 |
| A unicast check and `tx_drop_not_unicast` in transmit-side validation. The `10.100.0.255`, `224.0.0.1`, `255.255.255.255` rows of the case table. A check-number citation inside the case table became a counter name | `protocol.md` 8.5 |
| The range of the test build ICMP responder after `route_and_send`. The four sent/received byte and datagram counters and the layer list | `concurrency.md` chapter 3 |
| Phase 7 tasks: rule verification and creation, responder range, new counters. Verification: product build, checking other block rules, file transfer on 25565 with SHA-256, fake adapter verdict. Measurement point and packet count of the capture comparison table | `roadmap.md` Phase 7 |
| The five new counters in `PENDING_COUNTERS` as Phase 7. `_bytes` and `_datagrams` endings in the counter name shape | `tools/docgate/docgate.py` |
| The measurement procedure added to the repository | `tools/winprereq/Probe-FirewallRuleBinding.ps1`, `tools/winprereq/README.md` |
| A measurement boundary row (empty) in the capture comparison table, the receive-side valid interval, the receive-side reason in the pass condition | `roadmap.md` Phase 7 |
| The firewall row of the usability comparison table | `windows-prereq.md` 15 |
| Three next steps and waiting for the answer on the second machine | `plan.md` |
| Decision and rejected alternatives | ADR 0029 |

## Important decisions

The decision and the rejected alternatives are ADR 0029. This record has only the other judgments.

- **Whether a rule survives adapter recreation was measured first.** B holds only if a rule created once applies to the adapter of the
  next run. If not, the interface check of the verification would fail on every run from the second on. So it was measured before
  writing the documents. The result is in Confirmed facts below
- **"go to check 6" in the `protocol.md` 8.5 case table became a counter name.** One more check shifts the numbers. The same section
  says check numbers are not cited, yet the table cited one
- **`10.100.0.255` moved to the not-unicast side.** It is the broadcast address of the virtual range. Before, it sat in the out-of-range
  row as `tx_drop_no_route`
- **The upper end of the responder range is written as less than 255.** 255 is broadcast, which the unicast check handles
- **The five new counters went into the docgate pending list.** They are in the documents and not yet in the code. They come out during
  the implementation, or the gate fails. `tx_bytes` and the like were not of a shape the gate reads as a counter at first. A review
  finding added `_bytes` and `_datagrams` endings to the shape. Among code spans in the living documents, only these four have those endings
- **The capture comparison filter changed from the 5-tuple to the local port.** The counters do not distinguish peer or type, so the
  capture must not either to be equal

## Confirmed facts

`C:\me\build\fw-binding-probe.ps1` was run in an elevated shell. Windows 10.0.26300, PowerShell 7.6.7. At first it was kept outside the
repository as a one-time measurement. A review pointed out that it could not be reproduced, so it was added as
`tools/winprereq/Probe-FirewallRuleBinding.ps1`. The added version takes the paths as arguments and drops the last reverse lookup that
failed below. After the final review it was changed to remove only the rules this run created. No difference changes the
measured result.

The product build client was started without a room to bring the adapter up, two rules (group `SangtachiProbe`) were created with
`New-NetFirewallRule -InterfaceAlias Sangtachi`, then the client was `quit` and started again.

| When | `InterfaceAlias` | Interface in the registry rule text |
|------|------------------|-------------------------------------|
| Right after creation (adapter present) | `Sangtachi` | `IF={E2085BB3-83CE-4597-8DD3-68795CCFD00A}` |
| After `quit` (adapter absent) | `e2085bb3-83ce-4597-8dd3-68795ccfd00a` | Same |
| After restart (adapter back up) | `Sangtachi` | Same |

- The rules were enabled at all three points. The client exit code was 0
- The adapter's ifIndex was 64 in both runs
- The final reverse lookup by alias failed because `Get-NetFirewallInterfaceFilter` has no `-InterfaceAlias` parameter. A script mistake.
  `finally` ended the second client and removed the rules (0 rules left). It was also checked that no adapter was left afterwards
- Whether the rules actually let traffic through was not checked. It needs a peer machine

## Verification

- `python tools/docgate/docgate.py` passed. The first run failed because `tx_drop_not_unicast` was only in the documents, and passed
  after adding it to the pending list
- `cd tools/docgate && python -m unittest test_docgate` passed 139 tests. The same after widening the name shape
- Three scenarios were traced through the documents. `224.0.0.251` sent by the operating system goes through the adapter read and the
  unicast check of transmit-side validation to `tx_drop_not_unicast`. A player's `ping 10.100.0.1` goes through routing, `DATA`, the
  peer's 8.4 and injection, and the section 2 ICMP rule. A test build `ping 10.100.0.9` is answered by the responder and does not go to routing

## Cross-model review

The staged diff was shown to Codex twice. Once to follow four scenarios (startup, noise packets, test build `ping`, capture comparison)
and look for contradictions with the contract and with other documents and code. Once for Korean/English meaning and stale statements
across the repository. There were seven findings, all `warn`. One was raised by both reviews, so six in all. All were applied.

| # | Finding | Disposition |
|---|------|------|
| 1 | The responder range in the ADR does not exclude 255 and differs from `concurrency.md` (both reviews, mirror included) | Applied. Added less than 255 to both ADRs |
| 2 | The section 15 usability comparison still lists adapter profile setup as a person's burden (mirror included) | Applied. Changed to the server port exception the client creates and checking the Java rules |
| 3 | The four new byte/datagram counters are in neither the code nor the pending list, and fall outside the gate's name shape, so their implementation status is missed | Applied. Added them to the pending list and `_bytes`, `_datagrams` endings to the shape |
| 4 | Verification ignores the group, but the removal command removes by group. A rule whose group changed passes verification or survives removal and keeps blocking startup | Applied. Added a group check, and the `ERROR` line carries the command that removes that rule by name |
| 5 | Zero capture loss does not guarantee equality with the `recvfrom` counters. Socket buffer drops and `WSAEMSGSIZE` are only in the capture. There is no measurement boundary either | Applied. Added the operating system UDP receive error statistics to the receive-side valid interval and wrote that the generated sizes do not produce `WSAEMSGSIZE`. There is no basis yet to fix the measurement boundary, so it is a "still empty" row. It is decided when writing the comparison procedure |
| 6 | The GUID measurement has conclusions but no procedure or output to reproduce it | Applied. The procedure is in `tools/winprereq/`, and the documents and ADR point at it. The output is the table in Confirmed facts above |

The final confirmation review raised three more findings. All were applied.

| # | Finding | Disposition |
|---|------|------|
| 7 | `blocker`. When the measurement script stops on leftover probe rules, `finally` deletes those rules, which this run did not create | Applied. The leftover check moved before `try`, and only the names of rules this run created are removed |
| 8 | `warn`. A file name alone for `-LogPath` has an empty parent, so `Join-Path` fails | Applied. It is first made absolute relative to PowerShell's current location. The first fix, `GetFullPath`, used the process directory, and a review pointed that out again |
| 9 | `nit`. The record says only documents change, but the gate code and the measurement script change too (mirror included) | Applied. Changed to "does not change client code" |
