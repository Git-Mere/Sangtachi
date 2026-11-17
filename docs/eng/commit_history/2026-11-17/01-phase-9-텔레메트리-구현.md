# Phase 9 Telemetry Implementation

## Why

Did the code parts of the `roadmap.md` Phase 9 tasks: the telemetry service, the client's records and upload, the telemetry target of
the deployment tool, and the analysis script. Three workers took non-overlapping areas, and the answers to the gaps the workers asked
about were moved into the living documents.

## What Changed

| What | Where |
|------|------|
| Telemetry service. One `report`, record checks, room check, storage, CloudWatch metrics, input limits | New `telemetry-server/telemetry/` |
| Parameters added to the control server code so telemetry can use it. The defaults are the current behavior | `control-server/controlplane/server.py`, `log.py`, `__main__.py`, mutant tables |
| The client's `--telemetry`, records and session values, the `[telemetry]` thread and upload, `telemetry_upload_failed` | `client/`, `tests/`, `scripts/cli-check.ps1`, CMake |
| The deployment tool's `--target tm`, smoke test, `verify.py --tm-load` | `tools/cp-deploy` |
| Analysis script | New `tools/tm-analysis`. Added the measurement batch file location `tools/tm-analysis/local/` to `.gitignore` |
| Removed `telemetry_upload_failed` from `PENDING_COUNTERS` | `tools/docgate/docgate.py` |
| Package layout, reuse of `controlplane`, where and how the global cap counts, logs and counters, `boto3` time limits, CloudWatch time and resolution, dropping batches at shutdown, the journald drop-in, the difference between the dashboard success rate and the analysis | `architecture.md` 3.4 |
| `instance_id` is made only when telemetry is on | `architecture.md` 3.5, 6.3 |
| Definitions of the three NAT observations, `room_id` format check | `architecture.md` 6.3 |
| The next `interval`, finished sessions, holding jitter as a real number and rounding it, fixed log keys `telemetry.off` and `telemetry.upload_failed` | `architecture.md` chapter 9 |
| Repository layout | `architecture.md` chapter 10 |
| Where the interval timer lives, the two sole-ownership exceptions, the upload connect time limit | `concurrency.md` chapters 4, 6, 7 |
| Analysis counting (excluded OK, `ABORTED`, `PEER_FAILED`, Wilson 95%), when the load method is decided, the shape of load records | `roadmap.md` Phase 9 |
| Limits of the telemetry tables | `experiments.md` chapter 5 |
| What is collected and what is not sent | Root `README.md`, package `NOTICE.txt` (`scripts/package-gui.ps1`) |
| Next work | `plan.md` |

## Decisions

Decided while answering worker questions. All were moved to the places above.

- Analysis: attempts and successes count only player `establish`. `OK` is a success only when `room_known` is `true`; otherwise it is removed
  from the denominator and numerator and written as "excluded OK". `ABORTED` is removed from the denominator. Other results, including
  `PEER_FAILED`, are failures. The interval is Wilson 95%
- The public IP of the NAT observations is the mapped address of the first successful response of the STUN phase. `same_lan` is `false`
  when the counterpart has no reflexive candidate. If the STUN phase did not succeed, the STUN fields are `null`
- `room_id` must be `null` or the canonical form of `control_plane.md` 2.1. Otherwise `bad_request`
- The global cap counts only requests that passed parsing and the per-source check. Token bucket of capacity 5, 5 per second
- Six counters and three log events for the telemetry service. `unavailable` counts the concurrent-connection cap and the global cap together
- `boto3` time limits are the same as the control server's DynamoDB calls. The CloudWatch client too. Metric batches not yet sent at shutdown are dropped
- The journald cap is a drop-in in `/etc/systemd/journald.conf.d/`
- `telemetry_upload_failed` is an atomic variable incremented by `[telemetry]`. The upload connect time limit is 1.5 seconds
- The telemetry service imports `controlplane` instead of extracting a shared module. What differs is only one path, the per-source budget
  values and counting, the log's `src` and event table, and the counter names

## Verification

Results from the worker reports. No test that touches real AWS was run.

| What | Result |
|------|------|
| `telemetry-server` `pytest -q --store` | 657 passed |
| `telemetry-server` `tests/mutate.py --store` | All 57 mutants killed |
| `control-server` `pytest -q`, `pytest -q --store` | 747 passed 290 skipped, 1037 passed |
| `control-server` `tests/mutate.py http log rate_limit server` | All 179 mutants killed |
| `tools/cp-deploy` `pytest` | 38 passed |
| `tools/tm-analysis` `pytest` | 51 passed. All 21 mutants killed |
| Local end to end (telemetry service on DynamoDB local) | Normal 200, same record 400 `bad_request`, `{"v":2}` 400. No source IP in the log |
| `scripts/test.ps1 -Full` (after the review fixes) | Passed. `ctest` 689, `cli-check` 49/49, `e2e-check`, `lobby-check`, `session-check` 0 failed 0 not run |
| Client mutants | 13 before review and 2 for the review fixes, all caught |
| Local end to end, client and telemetry service | Both sides have `establish` (seq 0, `OK`, `room_known` `true`), `interval` (seq 1, 60000ms and 60016ms, 11 RTT samples), and `end` (seq 2, `CLOSED`) in the table. `telemetry_upload_failed` 0 |
| `docgate.py`, `--claims` | `VERDICT: pass` |


Not checked: the Linux build (`mere-galaxy` unreachable), the GUI build and running `package-gui.ps1` (syntax check only), `deploy.py --target tm` and `verify.py --target tm` on a real instance.

## Cross-Model Review

Each worker's diff was reviewed separately.

| Target | Finding | Handling |
|------|------|------|
| Analysis | `--max-rcu` accepts NaN and infinity | Fixed. `math.isfinite` check and a test |
| Analysis | `--load` does not validate record structure | Rejected. The input is this tool's `--save` output, and the records were checked by the server before storage |
| Telemetry service | The `room_id` format check conflicts with the opaque-value contract (Split 3) | Rejected. A normal client sends only the canonical form, and the server stores the value unchanged, so it does not conflict with the opaque contract |
| Telemetry service | `SystemMaxUse` caps only the persistent journal | Fixed. Added `RuntimeMaxUse=200M` |
| Client | The destructor joins without a time bound | Fixed. When the body returns a nonzero code, `[telemetry]` is also stopped and joined with the 2-second cap, and on overrun the process calls `_Exit` with that code. One `cli-check` case. No test takes the `_Exit` branch directly |
| Client | Registration-list deduplication makes `local_differs_public` `true` on a public-IP host | Fixed. Candidate collection also returns the local addresses before deduplication, and the comparison uses them. A public-IP interface test and 2 mutants |
| Client | `interval` is rescheduled from processing time and slips | Rejected. `interval_ms` carries the actual length and the slip is in milliseconds |
| Client | An invalid `--telemetry` value goes to the log | Rejected. Same handling as `--server`. What 6.3 does not upload is about upload records, not the local log |

The final review used three lenses: the client fixes, documents against code, and the whole staged change.

| Lens | Finding | Handling |
|------|------|------|
| Whole change | The server accepts `OK` with `setup_ms` `null`, so the analysis quartiles crash with `TypeError` | Fixed. The analysis drops a `null` setup time from the distribution only and still counts the success. One test |
| Client fixes | Flushing stdout and stderr before `_Exit` can block | Rejected. This behavior predates this change and `concurrency.md` chapter 7 shutdown defines it |
| Client fixes | An exception escaping the body takes the unbounded destructor join | Rejected. Only exceptions such as out of memory reach that point |
| Client fixes | On early failure returns the `[control]` channel destructor joins without a bound | Rejected. This `[control]` behavior predates this change and is outside its scope |
| Documents | The sentence that rejected `--tm-load` requests count as loss differs from the code | Fixed. The sequence number grows only on accepted requests |
| Documents | The sentence that an attempt that lost `establish` always shows as 1 | Fixed. At least 1 |
| Documents | The sentence that one upload takes at most 4.5 seconds | Fixed. A split send can take longer |
| Documents | The package notice lacks the NAT observations and the grouping keys | Fixed |
| Documents | README says the NAT observations are always true/false | Fixed. `null` before observation |
| Documents (repair check) | The package notice still says true/false only | Fixed |
| Documents (repair check) | The `SetupTime` non-null condition and the resulting dashboard and analysis difference were missing | Fixed. `architecture.md` 3.4. The next check added that the difference applies only to player `establish` records with `room_known` `true` |
