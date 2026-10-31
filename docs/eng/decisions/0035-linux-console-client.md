# 0035. Build a Linux console client

- Status: Accepted
- Date: 2026-10-31
- Related: [`../spec.md`](../spec.md) C-1, C-2 and scope, [`../architecture.md`](../architecture.md) chapters 7, 10 and 12, [`../concurrency.md`](../concurrency.md) chapters 2, 4 and 7, [`../protocol.md`](../protocol.md) chapter 6, [`../linux-prereq.md`](../linux-prereq.md), [ADR 0010](0010-platform-porting-seams.md)

## Context

The two-machine field test of Phase 8 is tied to the time of the person who has the other machine. The repository owner has one more Ubuntu machine.
If the client runs on that machine, the owner can run the field test alone with two machines, a Windows machine and a Linux machine.

[ADR 0010](0010-platform-porting-seams.md) decided "do not port; only fix the seams". Thanks to that decision, the place to rewrite is
`client/src/platform/` alone. The client sources outside the platform passed a compile check with g++ 13. There were 0 errors, and
the only warning was one unused variable.

## Decision

**Build an Ubuntu x64 console client. It includes the virtual adapter.** The "do not port" of ADR 0010 is reversed for the Linux console
client only. The five seams and the rule for placing OS headers stay as they are.

**1. Scope.** The target is Ubuntu 24.04 or later x64, and the compiler is g++ 13 or later. It is one console executable, `sangtachi_client`.
The GUI and distribution packaging are Windows only. macOS is out of scope and is decided separately. The wire protocol and the control plane
contract do not depend on the OS, so Windows clients and Linux clients mix in one room.

**2. Placement.** POSIX common sources go in `client/src/platform/posix/`, and Linux-only sources in `client/src/platform/linux/`.
This is to reduce what has to be rewritten when macOS is added later.

| Place | Sources |
|------|------|
| `posix/` | Wait, UDP socket, TCP, name resolution, random, standard output, record file, shutdown signals, an empty implementation in the Winsock place, session end, SHA-256 |
| `linux/` | Monotonic clock, executable path, interface and default route lookup, TUN device with address and route, privilege check, subnet overlap check |

**3. The types of the public headers do not change.** A file descriptor is stored in the `void*` handle. "The neutral type is decided when the second
implementation is written", which decision 3 of ADR 0010 left open, is settled by this decision as "keep the current type". Two public declarations change.

- The list of files to keep next to the executable, `kAdapterFiles`, becomes a function the platform defines. On Linux it is an empty list
- "Not checked" is added to the firewall verdict. The log value is `adapter.firewall verdict=skipped`

**4. Waiting is pipes and `poll`.** One event is a pair of non-blocking pipes. An auto reset event drains the pipe to consume only the one the wait
woke for, and a manual reset event is not drained. When several are ready, the lowest index is returned. This reproduces the Windows wait semantics
as they are. The source of the rules is [`concurrency.md`](../concurrency.md) chapter 2. `eventfd` is not used because macOS does not have it.

**5. Shutdown signals are received by a signal-waiting thread.** `SIGINT`, `SIGQUIT`, `SIGHUP` and `SIGTERM` are blocked on every thread, and a dedicated thread
receives them with `sigwaitinfo`. If cleanup were awaited inside a signal handler, the `[loop]` thread the signal was delivered to would stop. The mapping table is
owned by `concurrency.md` chapter 7.

**6. The subnet overlap check is done in C++.** The same decision table as the Windows `Test-SubnetOverlap.ps1` is applied to the main route table and the IPv4
addresses read over netlink. The decision table is owned by [`linux-prereq.md`](../linux-prereq.md). The TUN device is non-persistent and disappears when the process ends,
so there is no step that excludes our own adapter.

**7. The client does not touch the firewall.** Firewall tools differ by distribution (ufw, firewalld, nftables). The inbound the host has to accept is
opened by a person. The procedure is owned by `linux-prereq.md`.

**8. SHA-256 is implemented directly.** The Linux product path does not use the hash. Only `data.tx` and `data.rx` of the test build use it. Bringing in a third
party needs [`spec.md`](../spec.md) NFR-5 approval. A FIPS 180-4 implementation is under 150 lines and is checked with test vectors.

**9. Test scope.** Tests of the shared code are taken as passed on Windows. On Linux only the tests of the newly written platform sources and the platform tests
that compile without porting are run. This is the repository owner's decision. The gain of bringing the two-machine field test forward was judged larger than the cost
of testing the shared code again on Linux.

**10. Log tokens stay as they are.** Tokens such as `op=WaitForMultipleObjects` written by code outside the platform are the same strings on Linux too.
A token is a name that points at the failed step. Changing them would need a shape where the platform returns the token, and the Windows output would change.
Tokens written directly by platform sources are the real call names (`poll`, `recvfrom`, `TUNSETIFF` and so on).

## Alternatives

**Alternative 1. Do not port, and wait for a field test partner.**
Rejected. The field test schedule stays tied to the other person's time. That is the reason for this decision.

**Alternative 2. Run all the shared code tests on Linux as well.**
Rejected. Some test helpers use Winsock directly and the test entry point is PowerShell. What it gains is catching defects caused by compiler
differences; what it costs is porting the test infrastructure. This remains as the cost of decision 9.

**Alternative 3. Use `eventfd` and `epoll`.**
Rejected. Looking at Linux alone it is simpler. The cost is rewriting the wait when macOS is added. There are five events or fewer, so the cost of `poll` is not an issue.

**Alternative 4. The client opens the firewall.**
Rejected. ufw, firewalld and nftables would each have to be handled, and it changes the policy of the whole machine. It is not worth taking on for one field test machine.

## Consequences

**What this gains.**

- The owner can run the two-machine field test alone with a Windows machine and a Linux machine
- Windows and Linux mix in one room, so the stretch item "Linux interoperability" of `spec.md` is met for the console only
- What has to be rewritten when macOS is added is the adapter and route lookup in `linux/`, the clock, and the executable path

**What this costs.**

- The shared code is not tested on Linux. A defect that shows only under g++ is first seen in the field test
- There is no firewall check. If the host does not open the inbound, the Minecraft connection is blocked and the client does not report it
- Some log tokens are Windows API names. A person reading Linux logs has to read those names as step names
- Starting without `--no-adapter` needs root or `CAP_NET_ADMIN`
