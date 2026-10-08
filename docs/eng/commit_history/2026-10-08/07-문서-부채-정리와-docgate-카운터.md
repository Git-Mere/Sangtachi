# Document Debt Cleanup and the docgate Counter Check

## Why

Item 2 of "next" in `plan.md`. This cleans up the three document debts that could be done alone. One waiting item decided in the same session is included.

## What changed

| What | Where |
|------|-------|
| Added a `counter` check to `docgate.py`. It compares counter names in the living documents with `client/src/counters.cpp` in both directions | `tools/docgate/docgate.py`, `test_docgate.py`, `README.md` |
| The four counters not yet implemented are listed in `PENDING_COUNTERS` with their Phase | `docgate.py` |
| Wrote the record for `31e4240` (CMake smoke) retroactively | `commit_history/2026-09-11/01-cmake-smoke.md` |
| Made rows 2 and 4 of the section 14 table in `windows-prereq.md` match each section's `Owner:` line. Removed the count from the "5 items" sentence | `windows-prereq.md` in both languages |
| Removed the Phase 1 two-machine measurement from the waiting list | `plan.md` waiting items |
| Removed the four finished document debts. Five down to one | `plan.md` |

## Important decisions

- **No general check of "counts stated in documents" was built.** A claim such as "5 items" inside a sentence cannot be extracted
  reliably by a machine. Only counters, a countable set of names, are compared, and when a wrong count turns up, the count is taken out
  of the sentence so there is nothing to count. Section 14 is the first such case. The repository owner left the docgate design to us
- **Names on the code side are not filtered by shape.** Only the document side recognizes counters by a shape such as `drop_`. A name
  in the code must appear in the documents as is, regardless of shape, so a new counter outside the shape rule is also caught
- **The `counters` and `pending` arguments of `run()` are test seams.** They let the existing document-check tests run without a counter
  fixture. The command line always uses the defaults, and a separate test guards that
- **The stash for linking duplicated claims was dropped.** It was made on 09-22 and, despite its name, mixed in new requirements (NFR-11,
  "peer discovery" in NFR-4). The repository owner decided to drop it. The section 14 mismatch that stash had found was still in HEAD, so
  it was fixed separately
- **The Phase 1 two-machine measurement no longer waits on its own.** The Phase 1 verification in `roadmap.md` also accepts two
  processes on one machine, and traffic between two real machines is covered by the same-LAN test of Phase 4. The repository owner decided this
- Deleted the untracked `tools/nat-probe/natprobe_EN.py`. It was a stale copy without `host` and `players`. Being untracked, it is not in this diff

## Verification

| What | Result |
|------|--------|
| docgate tests | Cases were written first and seen to fail, then implemented. 139 pass |
| Mutation | 24 mutants, each removing one rule or restoring the pre-review implementation. The tests caught all 24 |
| Document gate | `docgate.py` and `--claims` both `VERDICT: pass` |

## Cross-model review

Codex. Two lenses were run separately on the staged diff (correctness of the counter check, facts and consistency of the documents). Below is round 1.

| Finding | Disposition |
|---------|-------------|
| blocker. If some `case` lines are read but one is not, such as a `case` with a comment, the check passes silently | Applied. Comments are stripped before reading, and the check fails when the number of `case Counter::` differs from the number of names read. The character limit on code-side names was also removed |
| warn. Code spans ignore the backtick run length. ``` `` drop_typo `` ``` is missed and an unmatched backtick is counted | Applied. Following CommonMark, a span closes with a run of the same length and one space is trimmed at each end |
| warn. A name ending in an underscore, such as `drop_new_`, is missed | Applied. Matched to the README's "starts with" rule |
| warn. `test_exit_one_on_failure` ran without a counter fixture, so it still passed with the parity check removed | Applied. It now breaks only parity on a counter-clean fixture and also checks that the failure is parity |
| warn. The CMake record stated an intent not in the diff ("a smoke test that the toolchain runs") | Applied. Only the commit message is kept |
| warn. The debt row was removed although a general count and arithmetic check is not implemented | Dismissed. Per the decision above, it will not be built. It was taken out of scope, not left as debt |

Round 2 checked the round 1 fixes.

| Finding | Disposition |
|---------|-------------|
| blocker. With spaces around `::`, as in `case Counter ::New`, both the count and the read miss it and the check passes silently | Applied. Spaces and namespace qualification are read, and the count now covers every `case` in the file, so any unread one fails the check |
| warn. A closing backtick was taken from the end of a longer run. The round 1 record's "a span closes with a run of the same length" was not true | Applied. The closing run must be a whole run of exactly the same length. HTML comment handling also uses this function, and all existing tests pass |
| warn. The unmatched-backtick test passed by accident because of the shape of the result | Applied. It compares the result of `code_spans()` directly, and a separate case covers a wrong closure that would invent a counter |

Round 3 raised one warn. HTML comment handling skipped an unmatched opening backtick run one character at a time, so the rest of
that run reopened a span and exposed a counter name inside the following comment (`` ``<!-- `x` --> ``). Applied. An unmatched run is
skipped whole as text. The defect predates this change and surfaced because the counter check reads that result.

Round 4 (confirming that fix) returned `LGTM - no blockers`.
