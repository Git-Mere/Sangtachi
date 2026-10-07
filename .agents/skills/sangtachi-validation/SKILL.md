---
name: sangtachi-validation
description: Apply Sangtachi-specific validation rules for code changes. Use when adding or modifying tests, mutation-testing logic, verdict or measurement code, timers, network scheduling, socket behavior, refactoring shared paths, spawning processes, investigating hangs, or verifying Windows client code.
---

# Sangtachi Validation

Use this skill together with `test-driven-development`.

This skill contains Sangtachi-specific rules for proving that tests and measurement logic actually detect incorrect behavior.

## 1. A test case existing is not proof

Do not consider a behavior validated merely because a test named for that behavior passes.

For important guards, invariants, classifiers, timers, and failure paths:

1. identify the defect the test is intended to catch,
2. deliberately introduce or simulate that defect when practical,
3. verify that the test fails,
4. restore the correct implementation,
5. verify that the test passes again.

If the deliberately broken implementation still passes, the test does not protect the claimed behavior.

## 2. Use mutation testing for important logic

Mutation testing is especially valuable for:

- boundary checks,
- state transitions,
- timer scheduling,
- retry logic,
- classification logic,
- failure handling,
- measurement code.

Prefer mutations that directly remove or invert the guard being claimed.

Record whether the relevant mutation was killed.

Do not count unrelated crashes as proof that the intended assertion was tested.

## 3. Write cases before verdict logic

Before implementing a classifier or verdict-producing function:

1. define its input classes,
2. write expected outcomes,
3. include boundary and ambiguous cases,
4. include an explicit unknown/unverified outcome where appropriate.

Default uncertainty to inconclusive rather than success.

Do not infer safety from a broad standard-library predicate without checking its exact semantics.

## 4. Check coverage before creating shared failure points

Before extracting duplicated logic into a shared helper or abstraction:

1. identify every caller,
2. verify those paths are actually exercised by tests,
3. add missing coverage before or with the refactor.

A shared abstraction can turn two untested paths into one untested single point of failure.

Do not treat a large total test count as proof that a particular path is covered.

## 5. Treat measurement code as high risk

Measurement and timing code often fails silently by returning plausible but wrong values.

When changing measurement logic, compare old and new behavior for:

- deadline calculation,
- send schedule,
- interval spacing,
- timeout calculation,
- retry behavior,
- socket being read,
- socket being written,
- swallowed errors,
- cancellation,
- early termination,
- cleanup.

Review behavioral preservation explicitly.

"Does not crash" is not sufficient.

## 6. Use the canonical Windows test entrypoint

For client and mutation testing, use the repository's canonical test entrypoint:

```powershell
pwsh -NoProfile -File scripts/test.ps1
```

Do not invoke mutation-test executables directly when the canonical route goes through `ctest`.

`ctest` provides per-test time limits that protect the agent from:

- modal MSVC assertion dialogs,
- infinite loops,
- deadlocks,
- other hangs.

Do not bypass these timeouts for convenience.

## 7. Verify process state before reporting success

Launching a command is not proof that the process is running correctly.

Before reporting that a process or reviewer has started:

- inspect the returned process/handle status,
- confirm that the launch step actually executed,
- check immediate failure when practical.

Do not place unrelated editing, state changes, and process launch into one fragile compound action when failure in an earlier operation could prevent the launch.

## 8. Distinguish failure classes

When a test fails, determine whether the failure is:

- implementation defect,
- test defect,
- environment/setup failure,
- timeout/hang,
- external dependency failure,
- inability to determine.

Do not weaken a test simply to make the suite green.

Do not convert an unexplained failure into a pass.

## 9. Validate negative behavior

For error-handling and recovery paths, verify both:

- that the expected failure is detected,
- that cleanup and subsequent state are correct.

Where applicable, validate:

- resources released,
- state not partially committed,
- retry state correct,
- no stale timers,
- no stale sockets or handles,
- no misleading success telemetry.

## 10. Report exact evidence

At completion report:

- tests added or changed,
- commands run,
- pass/fail result,
- mutations attempted,
- whether they were killed,
- skipped validations and why,
- environment limitations.

Do not state that behavior was verified if the relevant verification did not run.

## Exit criteria

Validation is complete only when:

- relevant automated tests pass,
- important new tests were proven capable of failing when practical,
- shared abstractions have coverage,
- measurement behavior was checked explicitly when touched,
- canonical timeout-protected test routes were used,
- failures were classified rather than hidden,
- claimed verification is backed by actual executed evidence.
