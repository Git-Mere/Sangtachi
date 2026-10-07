# 0014. Control Server Tests Run with pytest and DynamoDB Local Runs in Docker

- Status: accepted
- Date: 2026-10-06
- Related: [`../spec.md`](../spec.md) NFR-5, [`../control_plane.md`](../control_plane.md) section 9, [`../roadmap.md`](../roadmap.md) Phase 3, [ADR 0004](0004-state-store-dynamodb.md), [ADR 0005](0005-test-framework-catch2.md)

## Context

The first task of Phase 3 is to move the case tables of `control_plane.md` into `control-server/tests/`
and attach a mutation test to each table. In a table, one row is one test. Each row has to run under
its own name, and when a mutant is injected, a machine has to be able to read **which row** failed.

The three Python tools (`tools/docgate/`, `tools/platformgate/`, `tools/nat-probe/`) sit on the
standard library `unittest`. The control server could have done the same.

ADR 0004 and `roadmap.md` Phase 3 set DynamoDB local as the means of local testing. But they did not
write how it is started, or whether it is subject to approval under [`spec.md`](../spec.md) NFR-5.

## Decision

**1. The test runner of the control server is pytest.** The repository owner approved it. It is a
test-only dependency, so the approver under NFR-5 is the repository owner. The version is pinned in
`control-server/requirements-dev.txt`.

**2. The deployed server process does not import pytest.** Product dependencies go in
`control-server/requirements.txt` and test-only dependencies go in `requirements-dev.txt`, kept apart.
The NFR-5 verdict ("the deployed process does not import it") stands on this split.

**3. DynamoDB local runs as a Docker container.** The repository owner decided this. DynamoDB local is
also put on the NFR-5 list as a test-only dependency. The image tag and the run command are put into
a tool **after it is actually started once**, and the document points at that tool. This is because of
rule 6 of `CLAUDE.md`, which says not to keep a procedure that has not been run yet in a document.

**4. Mutation tests run with a runner on top of pytest.** `control-server/tests/mutate.py` copies the
original to a temporary folder, changes one place, and reads from the pytest output whether the rows
that the mutant has to fail actually fail. There are four verdicts (KILLED, PARTIAL, SURVIVED, ERROR),
and the default is "cannot determine".

## Alternatives

**Alternative 1. `unittest`.**
Dropped. It is the repository owner's judgment. It has the advantage of no dependency, and it was the
first default. The reasons for dropping it are parametrization that expands a case table into
per-row tests, and output that reports a failed row by node id. `unittest`'s `subTest` reports
failures, but it cannot pick out a row to run on its own or point at it by node id, so it is hard for
the mutation runner to read "which row failed".

**Alternative 2. Run DynamoDB local as a jar.**
Dropped. It is the repository owner's judgment. It runs without Docker, but it needs a Java runtime,
and the version of the downloaded jar has to be managed separately outside the repo. Docker pins the
version with an image tag.

## Consequences

**What we gain.**

- A row of a case table is one pytest parameter. A failed row comes out as a node id, so the verdict
  of a mutation test is left as machine output
- The version of DynamoDB local is pinned by one image tag

**What we pay.**

- The NFR-5 approval list is now six items. Three are test-only
- **Local tests need a Docker daemon.** On this machine, it was confirmed that the Docker client is
  present but the daemon is not running. Tests that need the store run only after the daemon is
  started. Rows of the case tables that are decided by pure functions run without the daemon
- The first image download needs a network
- The limit that DynamoDB local cannot expose consistency defects and transaction conflicts stays as
  it is. ADR 0004 "What we pay" and `control_plane.md` 7.6 Configuration and Deployment wrote it down.
  This decision does not change it

**What we could not check.**

Whether splitting NFR-5 into product dependencies and test-only dependencies is accepted by the
course. It is the same question as ADR 0005, and `plan.md` owns when it is checked.
