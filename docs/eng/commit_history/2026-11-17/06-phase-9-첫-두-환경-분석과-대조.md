# Phase 9 analysis of the first two environments and dashboard comparison

## Why

The owner created the CloudWatch dashboard and ran `tools/tm-analysis` in AWS CloudShell, then handed over the results. This PC and the instance role cannot read the telemetry table. This does the `roadmap.md` Phase 9 verification "compare the dashboard's `SetupTime` and `Rtt` percentiles with the analysis script's values once".

## What was done

| What | Result |
|------|------|
| Analysis tool | The owner ran it in CloudShell with `--save`. 82 records, 0 attempts outside any batch. Re-running the same records with `--load` on this PC gave byte-identical output |
| CloudWatch values | The ten metrics for 21:55-22:55 UTC were fetched at 1-minute resolution with the instance role's `GetMetricStatistics`. With the same role, percentiles over each environment's whole batch range as one period were fetched too |
| Dashboard definition | `tools/tm-analysis/local/dashboard.json`, ignored by git. The owner created it in the console |

## Analysis results

| Environment | Successes/attempts | Player setup time median | RTT median (interquartile range) | Jitter median | Loss |
|------|-----------|---------------------------|--------------------------|-------------|------|
| Same LAN | 10/10, Wilson 95% 72.2-100.0% | 10796.5ms | 12.4ms (9.6-17.1), n=119 | 2.1ms | 0/350 |
| Home network vs mobile hotspot | 10/10, Wilson 95% 72.2-100.0% | 10250.5ms | 111.0ms (96.2-132.1), n=117 | 6.9ms | 8/359 (2.23%) |

- In both environments `stun_same_endpoint` is `true` for the player and for the host of the same room
- The two controlled failures were counted separately: `HOLE_PUNCH_TIMEOUT` 1, `STUN_DISCOVERY_FAILED` 1. The fake host was made through the API only and has no host record, so the paired table shows "짝 없음" (no pair)
- The host setup time median is about 6.2 seconds in both environments. Its starting point differs, so it is not compared with the player value (`architecture.md` chapter 9)

## Comparison

| Environment | Value | CloudWatch | Analysis tool |
|------|----|------------|-----------|
| Same LAN | `SetupTime` p50 | 10769.1ms | 10796.5ms |
| Same LAN | `Rtt` p50 | 12.3ms | 12.4ms |
| Home vs hotspot | `SetupTime` p50 | 10158.6ms | 10250.5ms |
| Home vs hotspot | `Rtt` p50 | 111.6ms | 111.0ms |

In each environment both sides have the same sample counts: `SetupTime` 10, `Rtt` 119 for same LAN and 117 for the hotspot. The differences are within 1.1% (largest for same-LAN `Rtt`, 12.26ms vs 12.4ms). CloudWatch percentiles are approximate and the analysis tool sorts and interpolates linearly, so the same samples give slightly different values. As `roadmap.md` says, `experiments.md` uses the analysis tool's numbers.

## Not verified

- The `room_known` of the three test records received while only the control server was down. They are outside the batch range, so the Scan did not include them

## What changed

| What | Where |
|------|------|
| How to run the analysis tool in CloudShell, where the CloudWatch values are kept, comparison done | `plan.md` |

## Verification

| What | Result |
|------|------|
| `docgate.py` | `VERDICT: pass` |

## Cross-model review

Codex (`gpt-6.1-sol`). Lenses: numbers against the raw output (A), Korean-English agreement (B), agreement with `plan.md` (C), public-repo exposure (D). The raw output was given as a local file that is not committed.

| Finding | Handling |
|------|------|
| [A] warn. Same-LAN `Rtt` differs by 1.0978%, so "within 1%" is wrong | Applied. Now within 1.1%, naming the largest pair |

After the fix, the final staged diff was run again with the same lenses. `LGTM - no blockers`.
