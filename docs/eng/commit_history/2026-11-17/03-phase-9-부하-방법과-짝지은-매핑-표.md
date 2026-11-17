# Phase 9 load method, paired mapping-behavior table, dashboard timing

## Why

The owner made three decisions from the remaining items in `plan.md`.

- The telemetry load up to the global cap is applied inside the instance from several loopback sources
- The field environments are the five in the `roadmap.md` Phase 9 test environment table. The owner arranges partners and schedule
- A mapping-behavior table paired with host records is needed

On the owner's view that the CloudWatch dashboard is better made once measurements exist, its timing moved to after the field measurements.

## What changed

| What | Where |
|------|------|
| Took the dashboard out of the console work done "before the service is first deployed" and moved it after the measurements. Per-minute graphs are taken within 15 days of measuring | `roadmap.md` Phase 9 tasks |
| Turned the item that said to decide the load method into the decision. Run against `127.0.0.1` inside the instance, rotating the source over loopback addresses starting at `127.0.0.2` | `roadmap.md` Phase 9 tasks |
| Added a comparison paired with the mapping behavior of the host in the same room to the analysis items | `roadmap.md` Phase 9 tasks |
| `--tm-sources N`. Rotates loopback sources per request. Only then is `--host 127.0.0.1` accepted. Argument checks gathered into `check_args`. `report` is sent with `http.client` and binds to the source address | `tools/cp-deploy/verify.py`, README |
| Paired table. Pairs player `establish` `stun_same_endpoint` and `local_differs_public` with the values of host `establish` records of the same `room_id`. "짝 없음" (no pair) when absent, "불일치" (conflict) when they disagree | `tools/tm-analysis/tmanalysis.py`, README |
| Fixed the remaining-work table. Removed the question row and wrote the load check and field measurement rows as the owner's | `plan.md` |

## Key decisions

- **The pair is found by room, not by `peer`.** The host's STUN runs once when the room is created, so a room has one host value (`architecture.md` 6.3). Finding by room gets the value from another host record of the same room even when the host `establish` for that player is missing. When values disagree it records "불일치" instead of guessing
- **The paired table's denominator is the same as the existing per-field tables.** `ABORTED` and `OK` with `room_known` not `true` are left out and failures are in
- **The load sources are loopback.** The server tells sources apart by the connection's peer IP (`peername`). No cost or infrastructure change, and the deployed configuration is used as is. That the load program uses the same instance's resources and does not cross the network is recorded with the result

## Verification

| What | Result |
|------|------|
| `pytest tools/tm-analysis` | 62 passed. Added a 9-row pairing case table, 1 denominator test, 1 output test |
| `pytest tools/cp-deploy` | 53 passed. A 14-row `verify.py` argument case table, and 1 test that sends from three sources to the real telemetry accept loop and checks the three are tracked separately in the source table |
| Mutation. 4 in `tmanalysis.py` (drop the host `kind` filter, drop the conflict check, give a roomless player a room, drop the roomless-host filter) | All 4 caught as failures |
| Mutation. 4 in `verify.py` (drop rotation, start addresses at 1, loosen the count cap, drop the loopback target check) | All 4 caught as failures |
| `docgate.py` | `VERDICT: pass` |

## Not verified

- `--tm-sources` was not run on the instance. It runs on the deployment target, so it waits for owner approval

## Cross-model review

Codex (`gpt-6.1-sol`). Ran the lenses paired-table correctness (A), `--tm-sources` correctness and robustness (B), docs-code agreement and weak tests (C), and public-repo exposure (D) in one pass.

| Finding | Handling |
|------|------|
| [C] warn. The new source test fails alone with `ModuleNotFoundError`; an earlier test sets the telemetry import path | Applied. Both server package paths are set at module top. Passes alone |
| [C] warn. Creating the dashboard within 15 days does not keep minute data. The graphs must be taken within 15 days | Applied. The deadline is on per-minute graphs, not the dashboard. Fixed Korean, English, and `plan.md` together |

After the fixes, the final staged diff was run again with the same lenses. `LGTM - no blockers`.
