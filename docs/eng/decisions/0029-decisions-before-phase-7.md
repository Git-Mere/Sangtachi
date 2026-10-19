# 0029. Decisions before Phase 7

- Status: Accepted
- Date: 2026-10-19
- Related: [`../roadmap.md`](../roadmap.md) Phase 6 and 7, [`../windows-prereq.md`](../windows-prereq.md) 2 and 14, [`../protocol.md`](../protocol.md) 8.5 and chapter 12, [`../concurrency.md`](../concurrency.md) chapter 3, [`../architecture.md`](../architecture.md) chapter 7, [ADR 0027](0027-decisions-for-phase-6-implementation.md)

## Context

Comparing the documents and the code before starting Phase 7 turned up places that were not decided and places that collide with the
current code. The repository owner picked one of the options for each.

- `windows-prereq.md` 2 fixed only the shape of the inbound allow rules scoped to the adapter. Who creates them, when they are removed,
  and how an existing rule is trusted were missing
- The rules in that section open only TCP 25565 and ICMP echo requests. Which port the file transfer in the `roadmap.md` Phase 7
  verification (SMB or a temporary TCP server) uses was missing
- The Phase 6 test build's ICMP responder answers echo requests read from the adapter without looking at the destination. Once the read
  path is connected to routing, the test build would answer its own `ping` to the peer's virtual IP
- The `roadmap.md` Phase 7 verification compares the sent/received byte counters with a capture, but no counter counts bytes
- The operating system sends packets on its own as soon as the adapter is up (in Phase 6, `adapter_rx` was not 0 before the `ping`).
  Once the read path is connected to routing, the multicast and broadcast among them raise `tx_drop_no_route` and blur the
  verification and diagnosis of unicast with no route
- The injection failure verification needs the real Wintun send ring to be full, and there is no way to produce that state deterministically

**Whether the rules still apply after the adapter is recreated was measured in an elevated shell.** With the adapter up, two rules were
created with `New-NetFirewallRule -InterfaceAlias Sangtachi`, then the client was ended so the adapter went away, and started again.

- The firewall stores a rule's interface by GUID, not by name. The rule text in the registry was `IF={E2085BB3-83CE-4597-8DD3-68795CCFD00A}`
- While the adapter was gone, the rule's `InterfaceAlias` showed the GUID string, and it returned to `Sangtachi` when an adapter with the
  same GUID came back up
- The rules neither disappeared nor got disabled

The procedure is [`tools/winprereq/Probe-FirewallRuleBinding.ps1`](../../../tools/winprereq/Probe-FirewallRuleBinding.ps1), and the
machine and the output are in [the change record of this decision](../commit_history/2026-10-19/02-phase7-착수-전-결정.md).

Our adapter's GUID is a code constant (ADR 0027). So a rule created once is bound to the adapter of the next run. Whether a bound rule
actually lets traffic through needs a peer machine, so the Phase 7 two-machine verification checks it.

## Decision

| Item | Choice |
|------|---------|
| Firewall rule lifetime | Right after creating the adapter, the client creates the rules if they are missing. It does not remove them. The removal command lives in `windows-prereq.md` 2 |
| Existing rules | Verified before use. The same verification also runs once right after creating them. `windows-prereq.md` 2 owns the checks |
| A rule that fails verification | Startup failure. The client does not fix or remove it. The `ERROR` line carries the failed check and the removal command |
| Rule creation failure | Startup failure |
| Checks beyond the minimum | Whether the firewall is in `BlockInboundAlways` is checked. Other block rules on the same port are not |
| Implementation | A PowerShell script next to the executable, called with Windows PowerShell from `System32` and judged by its exit code. The same way as the subnet overlap check |
| What is opened | TCP 25565 and ICMPv4 echo requests (type 8). The host and the player use the same rules |
| File transfer verification | Send through a temporary TCP server and client on 25565 and compare the SHA-256 on both sides |
| Test build ICMP responder | Answers only echo requests whose destination is inside `10.100.0.0/24` with a last octet greater than `MAX_PEERS` and less than 255. The rest goes to routing. The two-machine verification runs on the product build |
| Byte counters | `tx_bytes`, `rx_bytes`, `tx_datagrams`, `rx_datagrams` in the process table. They count the values returned by successful `sendto` and `recvfrom`. The capture filters by that socket's local port |
| Multicast and broadcast destinations | Filtered separately before the route lookup in transmit-side validation, raising `tx_drop_not_unicast` |
| Adapter MTU | Keep the 1400 of `protocol.md` chapter 12 and fix it after the two-machine measurement |
| Injection failure verification | A fake adapter unit test (`tests/adapter_loop_test.cpp`) makes the verdict |

## Alternatives

**Firewall rule lifetime.** Creating the rules on every run and removing them on exit was rejected. It leaves nothing for the user, but
it edits policy on every run. The measurement above showed that a rule survives adapter recreation, so a rule created once can be used.
Having a person run a setup script once was also rejected. The client would not change security policy, but a person gets one more task
on each machine.

**A rule that fails verification.** Removing and recreating the rules with our name was rejected. It needs no person, but it deletes
someone else's policy on the basis of a name alone.

**Checks beyond the minimum.** Also checking block rules on the same port was rejected. Block wins over allow, so it is a real cause of
blocking, but the decision becomes a scope computation over every rule, and `windows-prereq.md` 12 leaves the Java rules to a person. The
two-machine verification procedure checks it.

**Implementation.** Calling the firewall COM API from C++ was rejected. It starts one PowerShell fewer, but the representation of the
interface scope would have to be confirmed separately, and the code that reads and judges rules would exist once more in a different
language from `tools/winprereq/Test-FirewallPolicy.ps1`.

**What is opened.** Taking the port as a startup argument was rejected. It serves users who change the server port, but adds an argument
and tests. It is written as a v1 limit. Opening everything that comes into our adapter was also rejected. Exposure is limited to room
members, but it reverses the decision of `windows-prereq.md` 2 and of its judging script.

**File transfer verification.** SMB was rejected. It needs 445 and file sharing opened separately.

**Test build ICMP responder.** Removing it was rejected. `scripts/adapter-check.ps1`, which checks read and inject together on one machine
in an elevated shell, would go away. Answering only when not in a room was also rejected. The same `ping` would end differently depending
on state.

**Byte counters.** Putting them in the session table was rejected. It would show per-peer values, but a receive does not know its session
at `recvfrom` time. Datagrams dropped in the early validation steps would be missing and disagree with the capture.

**Multicast and broadcast destinations.** Keeping them in `tx_drop_no_route` was rejected. Operating system noise raising the same counter
would let a broken implementation pass a live verification that judges by that counter's increase. Reducing the noise on the operating
system side was also rejected. It changes one more system setting.

**Adapter MTU.** 1452 was rejected. The outer UDP fragments on a PPPoE (1492) path. 1280 was also rejected. It covers a tunnel inside a
tunnel, but efficiency drops and there is no measured reason to go lower.

**Injection failure verification.** Filling the real send ring was rejected. The result depends on how fast the operating system drains
the ring, so it is not deterministic.

## Consequences

**Gains.**

- No person creates firewall rules. Each machine has that much less to do
- If an existing rule differs from the shape we create, it shows up at startup
- Even in the test build, a `ping` to the peer's virtual IP goes through the tunnel
- The byte counters are measured at the same point as the capture
- A rise in `tx_drop_no_route` means unicast with no route

**Costs.**

- The client does not remove the rules, so they stay on the system. A person removes them
- Startup gets slower by one PowerShell call
- If the server port is changed from 25565, the rules do not cover that port
- Other block rules on the same port are not caught by the startup verification
- The byte counters are not split per peer
