# The Name Was Changed to sangtachi

**Target commit:** this commit
**Phase:** none. Behavior does not change

The old name `hamychi` was still in the code, the build files and the historical records. The
living documents already used `Sangtachi`. The repository owner asked for a repo-wide change.

## What Was Changed

| What | Before | After |
|------|-----|-----|
| Header directory | `client/include/hamychi/` | `client/include/sangtachi/` |
| CMake module | `cmake/HamychiWarnings.cmake` | `cmake/SangtachiWarnings.cmake` |
| CMake project | `project(Hamychi)` | `project(Sangtachi)` |
| CMake targets | `hamychi_core`, `hamychi_client`, `hamychi_tests`, `hamychi_warnings` | `sangtachi_*` in the same places |
| CMake options | `HAMYCHI_BUILD_TESTS`, `HAMYCHI_TEST_BUILD` | `SANGTACHI_BUILD_TESTS`, `SANGTACHI_TEST_BUILD` |
| Namespaces | `hamychi`, `hamychi::network`, `hamychi::protocol` | `sangtachi`, `sangtachi::network`, `sangtachi::protocol` |
| Include paths | `#include "hamychi/..."` | `#include "sangtachi/..."` |
| Default build path | `%LOCALAPPDATA%\Hamychi\build` | `%LOCALAPPDATA%\Sangtachi\build` |
| Executable | `hamychi_client.exe` | `sangtachi_client.exe` |

Git recognizes the directory and file moves as renames. `git status` marks them `R`.

**Line endings did not change.** No file appears in the diff as a whole-file rewrite. Only the
lines carrying the name changed, in the counts given in the record table below. Reading and writing
in text mode turns CRLF into LF and spreads the diff across the whole file.

## The Record Folders Were Changed Too

`CLAUDE.md` treats `commit_history/` and `decisions/` as facts of their moment and forbids editing
them. This change is an exception the repository owner decided.

| File | Places changed (lines) |
|------|:---:|
| `2026-09-23-phase1-endpoint-args.md` (kor, eng) | 1 each (1 line) |
| `2026-09-23-phase1-event-loop.md` (kor, eng) | 1 each (1 line) |
| `2026-09-23-phase1-빌드-뼈대.md` (kor, eng) | 6 each (4 lines) |
| `decisions/0005` (kor, eng) | 4 each (3 lines) |

> **Why.** A record that keeps the old name makes the next session read a target name that does
> not exist. Only the identifier changed; the numbers, verdicts and wording stayed.

## Verification

Behavior does not change here, so the question is whether the existing checks still pass.

| What | Result |
|------|------|
| `scripts/build.ps1 -Fresh` | Success. Zero `/W4 /WX` warnings |
| Catch2 cases | 83 / 83 passed |
| CLI cases | 19 / 19 passed |
| End-to-end checks (`e2e-check.ps1`) | 0 failures |
| `docgate.py` | `VERDICT: pass`. pairs=57, links=525, findings=0 |
| Full search for the old name | 0 hits outside `.git`, the build output and these two record files |

**No mutation testing was run.** This diff changes identifiers only and does not touch control
flow. No new decision axis appeared, so there is no mutant to kill.

**The full search was not limited by file extension.** The old name was spread over five kinds of
file: `.hpp`, `.cpp`, `.txt` (CMake), `.ps1`, `.md`. Searching only `.md` would have left the
build broken.

## Cross-Model Review

Two lenses were run separately. The reviewer is codex.

| Lens | Result |
|------|------|
| Completeness and correctness of the rename. Half-renamed identifiers, target names that disagree between files, include paths that no longer resolve, strings that must not change | `LGTM - no blockers` |
| Stale references and record integrity. Leftovers anywhere in the repo, whether the record folders changed any fact beyond the identifier, whether Korean and English changed symmetrically | `LGTM - no blockers` |

### Round 3 — Record Accuracy and Mirror Parity

Only the two record files were sent. Two findings came back (the same one in Korean and English)
and both were taken.

| Grade | Finding | Handling |
|------|------|------|
| warn | The record says `decisions/0005` changed 3 places each, but the diff that was sent contains no such change, so the count cannot be reproduced | **Taken.** The stated reason is wrong: the change is staged and simply outside the scope given to that lens. **The number was wrong all the same.** The table mixed identifier counts with line counts. Three records used counts (6, 1, 1) while `decisions/0005` alone used lines (3). The count (4) and the line count (3) are now both written, which fixes the unit |

**A wrong finding pointed at a real defect.** Narrowing the lens scope caused the finding, but
recounting that row is what exposed the mixed units.

### Round 4 — Final (Scenario Lens + Record Lens)

Two lenses were put on the full diff. The scenario lens had no findings and the record lens
returned two. Both were taken.

| Grade | Finding | Handling |
|------|------|------|
| warn | The record said the old name was "only on the code side", yet the same diff also changes the old name inside the historical records | **Taken.** It now says the name was in the code, the build files and the historical records |
| nit | "went through `git mv`" states the command used, which the repository content cannot prove | **Taken.** Replaced with what can be checked: Git recognizes the moves as renames and `git status` marks them `R` |

### Round 5 — Repair Check

Only the repaired record files were sent. One finding came back and was taken.

| Grade | Finding | Handling |
|------|------|------|
| warn | The round 3 wording says "four entries, 6, 6, 1, 1" in Korean but "three rows, 6, 1, 1" in English, so the mirrors do not state the same fact | **Taken.** The table has four rows and three of them carried counts. The Korean was matched to the English |

**A sentence written in an earlier round became the next round's finding.** This is the mechanism
`CLAUDE.md` describes. A repair round reads the new wording as well as the repaired spot.

### Round 6 — Toward Checkable Claims

The same scope was sent again. Two findings came back, one per language, and both were taken.

| Grade | Finding | Handling |
|------|------|------|
| warn | The "0 hits" claim is contradicted by this record, which writes the old name several times | **Taken.** These two record files are now named as the exception. The before-and-after table needs the old name to make sense |
| warn | "read as bytes" states the method used, which cannot be reproduced from the repo | **Taken.** Replaced with something visible: no file appears in the diff as a whole-file rewrite |

**The same kind of finding arrived twice.** Writing down the method leaves the reader unable to
check it. A record states the result and what the result was checked with.

### Round 7 — One Dismissal

The round 3 finding came back. It is dismissed.

| Grade | Finding | Handling |
|------|------|------|
| warn | The `decisions/0005` row cannot be reproduced from the diff that was sent | **Dismissed.** That change is in this commit. The cause is the review input being narrowed to the record files, not an error in the record. The next round was run with `decisions/` in scope and that fact written into the prompt |

**The same finding arrived twice because of scope.** When a narrowed scope produces the finding,
widen the scope or write the dismissal into the prompt. Otherwise the same spot returns every
round.

### Round 8 — Last

The round was run with `decisions/` in scope and the dismissal written into the prompt. The round 7
finding did not come back. One nit came back and was taken.

| Grade | Finding | Handling |
|------|------|------|
| nit | The Korean said "the table above" while the table is below and the English says "below" | **Taken.** The Korean now says below |

**The round log stops here.** Writing a round down makes that sentence the input of the next
round, which never ends.

**Result of the final check round.** Zero blockers, one warn. That warn was the absence of this
very line, and writing the line is how it was taken. No finding has touched the code diff since
round 4. The commit goes in without another round.

**`LGTM` was not used as evidence.** The "nothing is left" claim was checked by machine instead:
the full search with 0 hits, the `docgate.py` verdict, and the passing build and tests.

## What Is Left

Nothing. The next task is held by `plan.md`.
