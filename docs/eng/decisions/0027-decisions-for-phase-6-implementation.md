# 0027. Phase 6 Implementation Decisions

- Status: Accepted
- Date: 2026-10-15
- Related: [`../architecture.md`](../architecture.md) 3.5 and Chapter 7, [`../concurrency.md`](../concurrency.md) Chapters 2, 3 and 5, [`../windows-prereq.md`](../windows-prereq.md) 1, 3, 4, 7, 8 and 9, [`../roadmap.md`](../roadmap.md) Phase 6 and 7, [ADR 0026](0026-decisions-before-phase-6.md)

## Context

Implementing Phase 6 hit several points the documents did not decide. The repository owner asked for Phase 6 to be carried through
to the end, and the below was decided during implementation. It was reported to the owner afterwards.

- An adapter can only be created with administrator rights. But the same-machine tests in `scripts/test.ps1` run several processes
  without administrator rights, and only one adapter can exist per machine
- `windows-prereq.md` 3 only said the adapter's GUID is recorded in a "state file". The file's place and format were missing
- When to create the adapter and when to attach the address, and the order and failure behaviour of the startup checks, were
  scattered or missing
- The subnet conflict decision of section 7 lives in a script, but how the client uses it was missing
- What the "client (check at run time)" in section 4 looks at was missing
- The counter for "other errors" in `drain_wintun` of `concurrency.md` chapter 3 had no name
- Where the Phase 6 read path parts from Phase 7 routing was missing

## Decision

| Point | Choice |
|------|---------|
| Run without the adapter | Startup argument `--no-adapter`. Both builds accept it. The default creates the adapter. All same-machine test scripts pass it |
| Adapter record | The name, tunnel type and GUID are code constants (`client/include/sangtachi/adapter.hpp`). No state file. The tunnel type is `Wintun`, because the subnet conflict script only excludes an adapter whose description is Wintun as its own |
| Adapter lifetime | It lives as long as the process. The address is attached on a `create_room` or `join_room` success response and removed on leaving the room. The table is in `architecture.md` chapter 7 |
| Startup checks | Administrator rights, subnet conflict, DLL hash, in that order. Any mismatch is a startup failure (exit code 2) |
| Subnet conflict | Call `Test-SubnetOverlap.ps1` next to the executable with Windows PowerShell from `System32` and judge by its exit code. Remove `PSModulePath` from the environment, restrict the inherited handles to the two standard streams, and cap it at 30 seconds |
| Run-time check | Load `wintun.dll` only from the executable's folder, and before loading compare its SHA-256 with the expected value. The expected value comes through a header generated from `cmake/Wintun.cmake` |
| Address setup | Attach as `/24`, turn DAD off, create the on-link route only if missing. Wait up to 10 seconds for `Preferred`. On failure, one `ERROR` line and shutdown |
| Other adapter receive errors | Counter `rx_err_adapter` |
| Phase 6 read path | A read packet is counted as `adapter_rx` and handed only to the test build's ICMP responder. The product build drops it. Routing and injecting `DATA` are Phase 7 |
| Firewall rule | Moved to Phase 7 work. The ping of the Phase 6 verification is the reply to a request this machine sent, so no inbound rule is needed |
| Administrator verification | `scripts/adapter-check.ps1`. It needs administrator rights, so it is not in `scripts/test.ps1` |

## Alternatives

**Run without the adapter.** Continuing without an adapter when not elevated was rejected. `windows-prereq.md` 1 decided to "block
before start", and if a rights problem passes silently the tunnel pretends to be alive. Turning the adapter off only in the test
build was rejected too. The Phase 6 ICMP responder is in the test build, so the test build must also create the adapter. A switch
that turns the adapter on (`--adapter`) was rejected because the product's default would be no adapter.

**Adapter record.** Writing a random GUID to a state file was rejected. The file's place, format and loss handling would be new. A
constant cannot be lost. Someone else's adapter with the same GUID does not exist unless made on purpose.

**Adapter lifetime.** Creating the adapter at the first room was rejected. Administrator rights and subnet conflicts would only be
known after the room is set up. `concurrency.md` chapter 7 Lobby already says "the adapter session is kept".

**Subnet conflict.** Porting the decision to C++ was rejected. The 85-case table would exist twice. Using the script's output for
the decision was rejected too. The output is in the console code page, so names can be garbled. The decision is the exit code, and
the output is only used as diagnosis in the `ERROR` line.

**Run-time check.** Checking the signature again at run time was rejected. It adds a `WinVerifyTrust` call, and if the hash
matches it is the very file the build judged.

**Address setup.** Leaving DAD on and waiting for `Preferred` was rejected. It is a point-to-point tunnel with nobody to ask, and
`[loop]` would block meanwhile. Moving address setup to `[control]` was rejected too. It runs once per room, and with DAD off the
wait is almost nothing.

**Tunnel type.** It was first set to `Sangtachi`. In the administrator check the adapter description became `Sangtachi Tunnel`, and
the subnet conflict script did not exclude our own live adapter (`not-wintun`). Widening the script's condition was rejected. That
condition is one of the guards against reading someone else's adapter as ours.

**Phase 6 read path.** Connecting read packets straight to the tunnel was rejected. `roadmap.md` puts that in Phase 7, and it cannot
be verified without two machines.

## Consequences

**Gains.**

- Same-machine tests keep running without administrator rights
- One value identifies the adapter, so the subnet check, the exclusion in candidate collection and the leftover cleanup use the
  same value
- The build and the run see the same expected-values file

**Costs.**

- A run that uses the adapter needs administrator rights, and only one runs per machine
- Startup is slower by one PowerShell run
- The adapter verification has to be run by a person in an administrator shell
