# Decisions before Phase 9

## Why

`roadmap.md` Phase 9 asks that the telemetry service spec and resource isolation within one instance be decided before starting. The
repository owner chose each item over several rounds of questions, and the result was moved into
[ADR 0039](../../decisions/0039-decisions-before-phase-9.md) and the living documents.

## What changed

| What | Where |
|------|-------|
| Decisions, rejected alternatives, gains and costs | New ADR 0039 |
| Telemetry service: Split 2 and 3 wording, room check, input limits, storage, ten CloudWatch metrics, resource isolation, configuration and deployment | `architecture.md` 3.4 |
| `--telemetry` input and build default | `architecture.md` 3.5 |
| One operation `report`, fields of the three record kinds, server checks | `architecture.md` 6.1, 6.2, 6.3 |
| When records are made, session bytes, smoothed jitter, RTT sample cap | `architecture.md` chapter 9 Telemetry Upload |
| Ring size rationale, telemetry interval timer, `[telemetry]` waits for cleanup-complete at shutdown, address resolution | `concurrency.md` chapters 1, 4, 6, 7 |
| FR-14, C-3, NFR-10 limit wording | `spec.md` |
| Two tables share the free capacity, IAM, port sentence, two rows removed from chapter 10 | `control_plane.md` 7.6 and 10 |
| 10-second snapshot sentence replaced by a chapter 9 reference | `protocol.md` 4.5 |
| Phase 9 tasks and verification | `roadmap.md` |
| Next steps | `plan.md` |

## Decisions

ADR 0039 owns the choice for each item. The values below were filled in by this work, not chosen by the owner, and the owner confirmed them.

- With neither an argument nor a build default for the telemetry address, telemetry is off
- `instance_id` has the same format as `client_nonce`. A random source failure is a startup failure
- Cap of 16 RTT samples per interval and `rtt_overflow`
- Session bytes are counted separately. `tx_bytes` is process-wide, so the host cannot split it per session
- There is no build version string, so only `build_kind` is sent
- Integer ranges for server checks, storage key `<instance_id>#<attempt>` and `seq`, three values of `room_known`
- Environment variable and unit names, CloudWatch namespace `Sangtachi`, metrics sent in batches every 60 seconds

Two decisions were changed during the questions because of existing documents. The owner accepted both.

- **DynamoDB capacity.** The telemetry table was to have 25/25 like the control plane, but the free capacity is per account, so the
  excess is billed (`control_plane.md` 7.6). It was split into 20/20 and 5/5
- **Waiting for the last upload.** It was set to 3 seconds, but the join limit in `concurrency.md` chapter 7 is 2 seconds, and on window
  close or logoff the OS ends the process right after cleanup. It was matched to 2 seconds

## Verification

| What | Result |
|------|--------|
| `docgate.py` | `VERDICT: pass` |
| Traced one player session and a host with three players through startup, establishment, intervals, `TUNNEL_DROPPED`, process shutdown, upload, server checks, room check, storage, and CloudWatch | Done with the scenario lens of review round 1. The findings below were fixed |

## Cross-model review

Codex (`gpt-6.1-sol`). Two lenses ran separately on the Korean diff: (1) correctness and end-to-end tracing, (2) stale claims, duplication,
and privacy. After the fixes, one more run covered only the fixed areas. Subagents synchronized the English mirror.

| Finding | Disposition |
|---------|-------------|
| On the shutdown event, `[telemetry]` can end before (3) enqueues `end` | Fixed. It waits for (7) cleanup-complete, then drains the queue |
| `Rtt`, `Jitter`, `SessionDuration` units off by 1000 | Fixed. Divided by 1000 |
| Interval loss differences can be negative because of late packets (`protocol.md` 4.5) | Fixed. `expected` and `accepted` are sent, and `LossRate` comes only from `end` totals |
| Genuine records that arrive after the room item is deleted | Recorded as a limit. Only sessions lasting more than a day past expiry |
| Metric population vs DynamoDB population | Fixed. Only stored records go into metrics. Batches missing only from the dashboard after a send failure are recorded as a limit |
| No integer ranges or per-kind required fields | Fixed. Server check list |
| `seq` gaps do not show tail losses | Fixed. Stated as visible gaps only |
| Writing time ranges in `experiments.md` conflicts with the dates-only rule of ADR 0001 | Fixed. They go in a non-public local file. Graph time axes also hide clock times |
| The confirmed value in the deployment table was changed to the target 20/20 | Fixed. Kept 25/25 and noted that the split gives 20/20 |
| The IAM sentence read as if the permissions already existed | Fixed. They are added at deployment |
| 60 seconds and table capacity restated in other documents | Fixed. They reference the owning section |

Finally, the full staged diff including the English mirror and the records ran with the Korean-English match and record consistency lenses. Two English mirror wordings were fixed (the translation of "배포물 안내" in ADR 0039 and the condition on the capacity row in `control_plane.md` 7.6), and a rerun on those spots had no findings.
