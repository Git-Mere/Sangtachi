# Phase 9 load check up to the global cap

## Why

This is the `roadmap.md` Phase 9 verification "load the instance running both services; control plane operations respond normally and the telemetry service stays within its systemd limits". The method is the loopback sources decided in the previous commit. The owner approved running it.

## What was done

Three things ran together for the same 5 minutes.

| What | Where | Command |
|------|--------|------|
| Telemetry load | Inside the instance. `verify.py` and `deploy.py` uploaded to a temporary folder and run with the system `python3` (3.14) | `verify.py --target tm --host 127.0.0.1 --tm-load 300 --tm-rate 5 --tm-sources 12` |
| Control plane load | This PC | `verify.py --host <Elastic IP> --soak 300` |
| Monitoring | Inside the instance | Every 10 seconds, `MemoryCurrent`, `CPUUsageNSec`, `NRestarts` of both services, load average, available memory |

The temporary folder was removed afterwards. The 1499 load records (`build_kind` `test`) stay in the telemetry table.

## Results

| What | Result |
|------|------|
| Telemetry load | 1499 requests in 300 seconds, all `ok`. No `rate_limited` or `unavailable`. Accepted at the global cap (5 per second) without rejection |
| Telemetry journal | 1500 `http.request` lines in the load window. The only non-200 is the smoke test's `bad_request`. The counters `internal_error` 1 and `http_read_timeout` 4 have the same values at 21:40:58 UTC before the load and after it |
| Telemetry resources | Memory 44-45MB, peak 45MB (limit 256M). CPU about 6.6 seconds in 300 seconds, about 2.2% (limit 20%). 0 restarts |
| Control plane | `--soak` `VERDICT: pass`. 2064 `get_peers`, median 71ms, p95 162ms, max 332ms. 0 retries. 0 non-200 responses in the journal for the load window. 0 restarts, peak memory 65MB |
| Instance | Load average at most 0.13, available memory at least 454MB |
| Telemetry table | `ConsumedWriteCapacityUnits` about 300 per minute (5 WCU per second). `WriteThrottleEvents` has no data points. Both metrics were read in one loop that changed only the metric name, with the same `TableName` dimension. Consumed capacity returned data, so the query is right and throttling is read as 0 |

**Limits.** The load program ran on the same instance and did not cross the network or the security group. A load over the cap (`unavailable`) was not run this time. The cap decisions are covered by the telemetry service's unit tests.

## What changed

| What | Where |
|------|------|
| Removed the load check row from the remaining-work table and wrote that it passed | `plan.md` |
| Wrote that `--tm-load` was run on the real instance | `tools/cp-deploy/README.md` |

## Verification

| What | Result |
|------|------|
| `docgate.py` | `VERDICT: pass` |

## Cross-model review

Codex (`gpt-6.1-sol`). Lenses: numbers against the raw output (A), Korean-English agreement (B), agreement with `plan.md` and the README (C), public-repo exposure (D). The raw output was given as a local file that is not committed.

| Finding | Handling |
|------|------|
| [A] warn. The 20:11 line shows only one internal error event. It does not show both counters had the same values since then | Applied. Now says the values at 21:40:58 UTC before the load equal the values after it |
| [A] warn. The raw output has no query dimensions, so it does not show the two metrics used the same dimension | Applied. Added the command to the raw output file and wrote that one loop changed only the metric name |

After the fixes, the final staged diff was run again with the same lenses. `LGTM - no blockers`.
