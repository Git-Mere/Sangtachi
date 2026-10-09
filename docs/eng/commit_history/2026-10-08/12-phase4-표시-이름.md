# Phase 4 Wave 1 D and D2: Display Names (FR-16)

## Why

Wave 1 D added display names to the control server. The server now requires `name` on `create_room` and `join_room`, so merging D
alone would break `lobby-check` because the client could not meet that contract. The client-side name work, planned for wave 4 J, was
split off as D2, built on top in the same worktree, and the two were merged as one commit. The repository owner chose this approach.

## What changed

**Control server (D).**

| What | Where |
|------|-------|
| The 2.7 name check (direct match against the 63-character set, 1-16 characters) and comparison key, `MAX_NAME_LEN`, `name_taken` | `controlplane/ids.py`, `constants.py`, `errors.py` |
| The `NAME#` claim in `create_room`/`join_room` (cancellation reason 3, a single transaction attempt), deletion of `NAME#` on both reclaim paths (found by `peer_id`), `name` in the `peers` elements | `controlplane/ops.py`, `store.py` |
| Candidate hygiene gains `0.0.0.0/8` and `240.0.0.0/4` | `controlplane/candidates.py` |
| Rejecting a space or tab inside a header name or between the name and the colon | `controlplane/server.py` |
| The 2.7 case table `NAME_CASES` and its mutants, updated existing tables and mutants, stale comments and test wording | `control-server/tests/` |

**Client (D2).**

| What | Where |
|------|-------|
| `--name` (required from Phase 4. Missing is `missing_name`, a bad format is `bad_name`, exit code 2) | `client/src/args.cpp` |
| Lobby command `name` (only in the lobby, a bad one is one `WARN` line and keeps the name, an accepted one logs `INFO lobby.name`) | `client/src/control/lobby.cpp` |
| `name` in the request body, the format check of `name` in the response `peers` elements (item 7 of 3.5), the `name_taken` definite error | `client/src/control/ops.cpp`, `runner.cpp` |
| Every format decision uses `tunnel::is_valid_name` from wave 1 A | |
| Tests and check scripts | `tests/`, `scripts/{cli,e2e,lobby}-check.ps1` |

**Documents.** The `ids.py` row of `control_plane.md` 7.1, 2.7 now points to the new case table, 2.7 in the section 9 case-table list,
and the `NAME#` key format in 6.3. `plan.md` removes three closed rows (stale server comments, server hygiene comments, duplicate name
check) and moves the control server deployment up to "next".

## Important decisions

- **Not deployed.** Rooms created before deployment have no `name` in `PEER#`, so requests for those rooms become `internal` (6.3).
  Deployment waits for approval
- **The code settled things the documents do not define.** None contradicts a documented rule
  - In the order of missing-argument checks, `missing_name` comes last. Error tokens through Phase 3 stay the same for the same input
  - The `WARN` tokens of the lobby `name` follow the shape of `join` (`missing_name`, `bad_name`)
  - In the lobby, `name` is accepted even while a request of a finished attempt is still pending. The "when accepted" column of
    `architecture.md` 3.5 only says "when in the lobby"
  - A new log event `lobby.name`. Section 9 allows adding events
- **To rejoin the same room right away, the name must change.** Before reclaim the same name is `name_taken` (2.7). The rejoin scene of
  `lobby-check` changes the name before joining again
- `is_valid_name` lives in `tunnel/packet.hpp`, so argument and control-plane code include the tunnel packet module. Moving it to a
  neutral header is left in `plan.md`

## Verification

| What | Result |
|------|--------|
| Full test run (`scripts/test.ps1`, after merging into main) | 395 unit tests pass, 0 failed cli/e2e/lobby checks, `all tests passed` |
| Server tests (main, `pytest -q --store`) | 967 pass |
| Server mutation (subagent) | `mutate.py --store` 572/572. The one ERROR of the first run (the old string of `room-no-type` appeared twice) was solved by fixing the mutant definition |
| Client mutation (subagent) | All 17 C++ mutants were caught by assertion failures. Two `lobby-check` mutants also failed, and one weak check found then was fixed |
| `docgate.py` | `VERDICT: pass` |

## Cross-model review

The subagents ran Codex review in the worktree.

| Finding | Disposition |
|---------|-------------|
| D blocker. With the new type check, the old string of the `room-no-type` mutant appears twice and the mutant errors | Applied. The old/new strings were extended so it appears once |
| D warn. The server reclaim scenario does not check that a deleted `NAME#` is left out of the renewal list | Applied. Added the assertion and the mutant `reclaim-removed-without-names` |

Round 2 of D and round 1 of D2 returned `LGTM - no blockers`.

After merging into main, Codex reviewed the whole staged diff (server and client) once more and returned `LGTM - no blockers`.
