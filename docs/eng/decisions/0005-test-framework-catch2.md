# 0005. Use Catch2 as the C++ Test Framework

- Status: accepted
- Date: 2026-09-23
- Related: [`../spec.md`](../spec.md) NFR-4, NFR-5, [`../architecture.md`](../architecture.md) chapter 11, [`../roadmap.md`](../roadmap.md) Phase 1

## Context

The C++ standard library has no test runner. The Python tool set solved this with the standard
library. The 109 cases of `tools/docgate/` and the 163 cases of `tools/nat-probe/` sit on
`unittest`, and `tools/winprereq/` runs itself through PowerShell `-SelfTest`. The client has that
slot empty.

Phase 1 verification asks for value comparison and mutation testing. It needs a runner that
registers cases by name, reports only the failures with their location, and gives a verdict through
the exit code. Once there are dozens of cases, a list of `if` statements inside `main()` cannot show
which one failed and why.

`spec.md` NFR-5 asks for prior approval of non-default third-party dependencies. The reason behind
that requirement is the same as what NFR-4 writes down. STUN, NAT traversal, hole punching, the
tunnel protocol, routing, and session management have to be implemented by us, and replacing them
with a library turns the deliverable of this project into a shell around someone else's code.

## Decision

**Use Catch2 v3 in the test executable only.** Five items are part of this decision.

**1. Pin the version by tag.** `FetchContent` fetches `v3.16.0`. It does not point at a branch.
Pointing at a branch breaks our build for no reason when upstream changes, and the test results of
that moment cannot be reproduced later.

**2. Product targets do not link Catch2.** The link lists of `sangtachi_core` and `sangtachi_client`
have no Catch2. Only the test executable `sangtachi_tests` links it. That is the mechanical
definition of "test only", and the link list shows it.

**3. Our warning policy is not applied to Catch2.** `/W4 /WX` lives on the `sangtachi_warnings`
interface target, and only targets in this repository link to it. Breaking someone else's code with
our warning policy ties the build to that library version.

**4. Test case names use ASCII only.** `catch_discover_tests` registers case names with CTest and,
at run time, passes those names back to the executable as command arguments. If a name has a
character outside ASCII, it is mangled through the console code page and matches no case. In a
measured run, three cases all reported `No tests ran` and CTest reported failure. Descriptions go
in comments and tags.

**5. Register with CTest.** `catch_discover_tests` makes one CTest entry per case, so each case runs
in its own process. Cases that look at process-global state do not pollute each other. A case that
checks the Winsock reference count is one example. CTest is part of CMake, so it is not subject to
NFR-5.

### Approval Scope

**The repository owner approved it.** The instruction and its reasoning are in
[`../commit_history/2026-09-23/03-phase1-빌드-뼈대.md`](../commit_history/2026-09-23/03-phase1-빌드-뼈대.md).
The judgment is that what NFR-5 blocks is replacing the product deliverable with an external
library, and a test runner does not go into the product executable, so it is not in that category.
The NFR-4 list of things we must implement ourselves has no test runner either.

`spec.md` NFR-5 was changed to match this approval. The requirement now writes the approving party
for product dependencies and for test-only dependencies apart, and the repository owner is the party
for the test-only side. **Catch2 is approved as that requirement asks and is not pending.**

**What is not confirmed is that split itself.** Whether splitting the requirement in two is accepted
by the course is not a fact that can be checked inside this repository. `plan.md` holds the check
point. If the answer differs, we reverse this decision and write a new ADR. The cost of doing that
is written under "Consequences" below.

## Alternatives

**Alternative 1. A dependency-free in-house test harness.**
Dropped. This is the repository owner's judgment. Its strength was that there is no approval
question at all, and it was the first recommendation. It was dropped for two reasons. One, the
harness itself becomes untested code, and if that code is wrong every case passes silently. Two, we
would have to write the failure reporting (value output, location, exception handling) ourselves,
which moves Phase 1 work time over to test tooling.

**Alternative 2. GoogleTest.**
Dropped. gmock comes with it, and there is no place yet that needs mock objects. It also has more
build targets than Catch2.

**Alternative 3. doctest.**
Dropped. It is lighter and compiles faster. But its approval category is the same as Catch2, so the
approval cost stays, and the only gain is compile time. The test executable of this project is
small.

**Alternative 4. Copy the Catch2 amalgamated sources into the repository.**
Dropped. It has the advantage of building without a network. But tens of thousands of lines of
someone else's code enter the repository, `git log` and review diffs are polluted by that much, and
every version bump has to be copied by hand.

## Consequences

**What we gain.**

- We do not write case registration, failure location reporting, and exit code verdicts ourselves.
  Phase 1 time goes to the target code, not to test tooling
- Each case gets its own process. We can write cases that look at process-global state (whether
  Winsock is initialized)
- The verdict of a mutation test is left as machine output. When a defect is injected, the output
  says **which case** fails, so what a case protects is checked by result, not by words

**What we pay.**

- **The first configure needs a network.** `FetchContent` fetches from GitHub. With no cache and no
  network, the configure step fails. What was fetched stays in the build directory, so later builds
  run without a network
- **There are more build targets.** The Catch2 targets attach to our 3-source build, and the first
  build was 116 steps under ninja. Later incremental builds run only our sources
- The NFR-5 approval list is now three items: Wintun, `boto3`, and Catch2
- **The cost of reversing is the macros, not the case bodies.** Cases are written with the two
  macros `TEST_CASE` and `REQUIRE`, so moving to an in-house harness means defining those two and
  changing the header. The case bodies go over as they are

**What we could not check.**

Whether splitting NFR-5 into product dependencies and test-only dependencies is accepted by the
course. It is written under "Approval Scope" above. It is not about whether Catch2 is approved.
