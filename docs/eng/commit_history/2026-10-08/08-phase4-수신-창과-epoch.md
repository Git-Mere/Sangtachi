# Phase 4 Wave 1 B: Acceptance Window and Epoch

## Why

Phase 4 has started. The repository owner chose to start before the `tools/nat-probe` re-measurement. `roadmap.md` lists the
re-measurement as a before-start item, but the only spot that depends on its result is the host NAT warning before room creation,
and none of these slices contains it. That decision and the slice order are in `plan.md`.

Wave 1 is four slices that touch different files (A wire codec, B acceptance window and epoch, C local candidates and hygiene, D
control server). Subagents built them in parallel, each in its own git worktree. This commit is B.

## What changed

| What | Where |
|------|-------|
| `tunnel/replay`: `is_newer` (4.4) and the acceptance window (4.5). Decision (`check`) and update (`commit`) are split, and the normal path is `accept`, which joins them | `client/include/sangtachi/tunnel/replay.hpp`, `client/src/tunnel/replay.cpp` |
| `peer/epoch`: `session_epoch` generation (redraws 0, injected random source, default `platform/random`), retired list (2 minutes, 16 entries, evicts the oldest), attempt nonces and their retired list | `client/include/sangtachi/peer/epoch.hpp`, `client/src/peer/epoch.cpp` |
| 40 test cases | `tests/tunnel/replay_test.cpp`, `tests/peer/epoch_test.cpp` |
| The two modules in the module table and tree | `architecture.md` 3.1, section 10 |
| Phase 4 slice order, the decision to start before the re-measurement, five implementation follow-ups | `plan.md` |

Three of the new follow-ups in `plan.md` (order of the per-type checks, the first `ROSTER` `generation`, duplicate name checks) came
from A's report. They are in the same handoff document, so they are written here together.

## Important decisions

- **The code settled three behaviors the documents do not define.** None contradicts a documented rule
  - Re-adding an epoch to the retired list removes the old entry and appends a new one, restarting the 2 minutes. One value does not
    take two slots
  - When the random source fails, no value is returned (`nullopt`). It is not replaced by a predictable value
  - Redrawing a 0 epoch or a retired nonce has no retry limit. The documents set none
- **4.5 `pkt_pos` wraps for a number older than the first packet.** The code follows the 4.5 formula and a test pins that behavior.
  The document gets fixed before Phase 5 loss accounting (`plan.md`)
- **A packet dropped after passing 8.3 is still recorded in the window.** The 4.5 pseudocode updates at the 8.3 step, so that is the
  current rule. It gets stated in a sentence in `protocol.md` 8.3 when E starts (`plan.md`)

## Verification

| What | Result |
|------|--------|
| Full test run (`scripts/test.ps1`, after merging into main) | 331 unit tests pass, cli/e2e/lobby checks `0 checks failed`, `all tests passed` |
| Mutation (subagent) | The tests caught all 23 kinds. One (E6) survived at first, and a `size()` assertion was added. For two (R5, E8), the first form was blocked at build time by `/WX` warnings, so they were rerun in a form that avoids the warning |
| The two cases added after review | `-Filter "replay|epoch|nonce"` 43 pass. A mutant truncating the position to 32 bits and one truncating the registration time to 32 bits were each caught by its case |
| `docgate.py`, `platformgate.py` | Both `VERDICT: pass` |

## Cross-model review

The subagent ran Codex review twice in its worktree.

| Finding | Disposition |
|---------|-------------|
| warn. `pkt_pos` of a reorder older than the first packet wraps | The code keeps the 4.5 formula and a test pins it. Handed over as a document decision (above) |
| warn. The expiry assertions used the implementation constant, so changing the TTL still passed | Applied. `STATIC_REQUIRE` pins 120000 and the boundaries are literals |
| nit. Asserting that two epochs from the real random source differ fails a correct implementation with probability 2^-32 | Applied. The assertion was removed |

After merging into main, Codex reviewed the staged diff once more.

| Finding | Disposition |
|---------|-------------|
| warn. No test takes `position` past 2^32, so an implementation truncating it to 32 bits survives | Applied. Added a case that advances three times by less than 2^31 each. Confirmed with a mutant |
| warn. All retired-list registration times fit in 32 bits, so an implementation truncating them survives | Applied. Added a case checking both lists' boundaries at a time above 2^32. Confirmed with a mutant |
| warn. The new `plan.md` rows describe `tunnel/packet`, not yet in main, as existing | Applied. Reworded to "built by wave 1 A" |

Round 2 (confirming those fixes) returned `LGTM - no blockers`.
