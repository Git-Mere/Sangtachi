# Phase 7 implementation (ADR 0029)

## Why

The repository owner asked to take Phase 7 as far as possible. The owner will say when verification happens. The pre-start decisions are
ADR 0029 and this commit implements them. The checks that need two machines and an elevated shell remain (`plan.md`).

## What changed

| What | Where |
|------|------|
| Packets read from the adapter go to 8.5 transmit-side validation and `DATA`. The test build first answers ICMP echo requests outside the routing range | `main.cpp` |
| `DATA` that passed 8.4 is injected in both builds when the adapter exists. Without the adapter nothing changes | `main.cpp` |
| The 8.5 unicast check and `tx_drop_not_unicast` | `tunnel/router.hpp`, `router.cpp`, `tunnel.cpp`, `tunnel.hpp` comment |
| Five counters. Count 57 -> 62 | `counters.hpp`, `counters.cpp` |
| Bytes and datagrams of successful `sendto`. Bytes and datagrams of successful `recvfrom` (the 1473-byte oversize included, `WSAEMSGSIZE` excluded, zero bytes included) | `loop.cpp`, `loop.hpp`, `network/udp_socket.hpp`, `platform/win32/udp_socket.cpp` |
| Destination range of the test build ICMP responder | `testbuild/instrument.cpp`, `instrument.hpp` |
| The script that verifies and creates the firewall rules. 46 cases | `tools/winprereq/Initialize-FirewallRules.ps1` (new file), `tools/winprereq/README.md` |
| The script sits next to the executable and is called at startup. The run code was split out as `run_script` so the subnet check uses the same code | `client/CMakeLists.txt`, `platform/tun.hpp`, `platform/win32/tun.cpp`, `main.cpp` |
| The two-machine file transfer tool | `tools/transfer/Test-Transfer.ps1`, `tools/transfer/README.md` (new files) |
| A firewall scenario and a no-route scenario in the elevated check | `scripts/adapter-check.ps1` |
| Tests. Unicast rows and check order of the 8.5 case table, the destination table of the ICMP responder, byte/datagram counters, zero-byte receive, injection and timer interval while the ring is full, the counter list | `tunnel_test.cpp`, `instrument_test.cpp`, `loop_test.cpp`, `adapter_loop_test.cpp`, `counters_test.cpp` |
| The five new counters came out of the pending list | `tools/docgate/docgate.py` |
| Rule names, `ERROR` line, judging script, implementation status | `windows-prereq.md` 0, 2, 14 |
| `platform/tun` in the module table, `adapter.firewall` and `adapter.error` in the chapter 9 fixed keys | `architecture.md` |
| `route_and_send` and the injection table brought to the current state | `concurrency.md` chapter 3 |
| Where the file transfer tool and the no-route check live | `roadmap.md` Phase 7 verification |
| `AssignAddress` also sets the tunnel's own virtual IP | `control/runner.cpp`, `lobby.hpp` and `tunnel.hpp` comments, `runner_test.cpp` |
| A failed test build shows its last 40 lines | `scripts/harness-lib.ps1` |
| Two remaining items | `plan.md` |

## Important decisions

The decision and the rejected alternatives are ADR 0029. This record has only what was decided while implementing.

- **Spaces were removed from the rule names.** They are `Sangtachi-TCP-25565-In` and `Sangtachi-ICMPv4-Echo-In`. The approved design put
  the removal command on the `ERROR` line, but `architecture.md` chapter 9 does not put spaces in log values. So the line carries the rule
  name and the check, and the removal command lives in `windows-prereq.md` 2. With spaces, even the name would turn into underscores.
  Reported to the repository owner
- **The two events used for the firewall verdict went into the chapter 9 fixed-key table.** ADR 0027 did not give the adapter events fixed
  keys. Back then the verdict came from OS queries and counters. The firewall scenario judges by the `adapter.firewall` and
  `adapter.error reason=firewall` lines, so it falls under chapter 9's "events that verification looks for get fixed keys". A review finding
- **The script run code is shared with the subnet check.** The process part of `run_subnet_check` was split out as `run_script`. Limiting
  inherited handles, removing `PSModulePath`, temp-file output, and forced termination after the cap are unchanged. The subnet verdict
  code did not change
- **The ICMP rule's local port accepts both `Any` and `RPC`.** At first only `Any` was expected. When the repository owner ran
  `adapter-check.ps1` in an elevated shell, the client failed startup with `failed=Sangtachi-ICMPv4-Echo-In:port`.
  `Get-NetFirewallPortFilter` returned the ICMP rule we created without ports as `LocalPort=RPC`, `IcmpType=8`. The built-in ICMP rule
  (`FPS-ICMP4-ERQ-In`) is the same. ICMP has no ports, so nothing can widen through this field. `IcmpType` accepts both `8` and the
  registry form `8:*`
- **The tunnel knows its own virtual IP from the moment it joins a room.** Before, it learned it when creating the first session. In the
  elevated `adapter-check.ps1`, a lone host sending `ping 10.100.0.3` did not raise `tx_drop_no_route`. The log showed that every packet
  read from the adapter until then was `tx_drop_bad_src` (93), because there was no own virtual IP to compare the source with. Up to Phase 6
  adapter packets did not reach transmit-side validation, so this did not show. `AssignAddress` now puts the value into the tunnel too. A
  runner test was added, and removing that line fails it
- **The timer tolerance of the injection failure test is 50ms.** On a 100ms period it leaves margin beyond the timer clock resolution
  (about 15ms)
- **In the file transfer tool the receiver returns the hash and the sender judges.** A person does not compare two hashes by eye

## Verification

- `pwsh -NoProfile -File scripts/test.ps1` passed. 627 ctest cases and the cli, e2e, lobby and session checks
- `python tools/docgate/docgate.py` and `python tools/platformgate/platformgate.py` passed
- `Initialize-FirewallRules.ps1 -SelfTest` passed 46 cases. In a non-elevated shell the verdict mode could not create the rules and gave
  two `FAIL <name> create` lines, `VERDICT: fail`, exit code 1
- All twelve mutants were killed
  - C++: removing the unicast check, removing the responder's range check, not counting oversize receives, counting sent bytes wrong,
    retrying for 5ms when the ring is full
  - Firewall script: removing the group check, comparing the interface by containment, removing the remote port check, removing the policy
    line count check, removing undecidable, removing the rule that a defect beats undecidable
  - File transfer tool: `VERDICT: fail` when the receiver returns a wrong hash
- The file transfer tool ran on loopback (127.0.0.1:25565) with 8 MiB and gave `VERDICT: pass`
- The repository owner ran `scripts/adapter-check.ps1` four times in an elevated shell
  - The first stopped at the link step of the test build. The build output was discarded, so no cause was left. Building again right
    away passed. It now shows the last 40 lines on failure
  - In the second, all four scenarios failed startup with `failed=Sangtachi-ICMPv4-Echo-In:port`, because of the ICMP rule's `RPC`
    form (decision above)
  - In the third, 58 of 59 checks passed. Creating the firewall rules, reusing them, startup failure on a disabled rule and its
    restoration, and the `Test-FirewallPolicy` verdict passed. The one failure was `tx_drop_no_route` in the route scenario, the own
    virtual IP defect (decision above)
  - The fourth ran with the own virtual IP fix and passed everything (`0 checks failed`). `tx_drop_no_route` in the route scenario went
    from 0 to 2, and `tx_drop_not_unicast` was 63 by then: multicast and broadcast the operating system sent to the adapter
- **Not yet seen.** The two-machine verification. Whether the firewall rules actually let the peer's traffic through is seen then too

## Cross-model review

The staged diff was shown to Codex twice. Once for correctness of the C++ and PowerShell and tests that cannot fail, once for the contract
agreement of code and documents. There were seven findings, all `warn`. One was raised by both reviews, so six in all. All were applied.

| # | Finding | Disposition |
|---|------|------|
| 1 | A zero-byte datagram does not count in `rx_datagrams` (both reviews) | Applied. A successful receive counts regardless of length. Only `WSAEMSGSIZE` is excluded. A test was added |
| 2 | Comments in `tunnel.hpp` and `loop.hpp` still say the product build leaves the hook empty and that this is Phase 7 work | Applied |
| 3 | `plan.md` still lists the implementation this commit does as upcoming | Applied. The remaining work became two checks that need two machines |
| 4 | The `adapter.firewall` and `adapter.error` fields that `adapter-check.ps1` reads are missing from the chapter 9 fixed-key table (mirror included) | Applied. Two rows were added |
| 5 | The script's `UNKNOWN` lines do not reach the client log, so when a query failure blocks startup nothing says what failed | Applied. An `unknown` field was added to the `ERROR` line |
| 6 | The `created=-` regex in `adapter-check.ps1` anchors at the end of the whole log, so a normal run fails. A failure to re-enable the rule is hidden | Applied. Switched to multiline mode, and re-enabling is called with `Stop` and then checked |

The repairs alone were shown to Codex once more. Two findings, both `warn`. Both were applied.

| # | Finding | Disposition |
|---|------|------|
| 7 | `plan.md` still says the ICMP local port form is unknown | Applied. The fourth run passed, so the row was removed |
| 8 | The records say `adapter-check.ps1` was not run, yet also say the owner's runs exposed two defects (mirror included) | Applied. Each run and its result are recorded |

The final confirmation of the whole staged diff raised two more findings. Both were applied.

| # | Finding | Disposition |
|---|------|------|
| 9 | `warn`. The axis table in `plan.md` still says Phase 6 is the last finished | Applied. It now says Phase 7 implementation and the one-machine checks are done and the two-machine verification remains |
| 10 | `nit`. The records count three remaining items while `plan.md` has two (mirror included) | Applied |
