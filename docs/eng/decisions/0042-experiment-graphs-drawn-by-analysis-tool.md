# 0042. Experiment graphs are drawn by the analysis tool

- Status: Accepted
- Date: 2026-11-23
- Related: [ADR 0039](0039-decisions-before-phase-9.md) aggregation and graphs, [`../spec.md`](../spec.md) NFR-5, [`../roadmap.md`](../roadmap.md) Phase 9 visualization of experimental results

## Context

ADR 0039 chose CloudWatch for aggregation and graphs. The service puts custom metrics and the owner views them in a dashboard. Putting a graph library in the service was rejected because of the instructor approval NFR-5 needs and the resources of t3.micro.

When building the graphs for `experiments.md`, the dashboard could not do two things.

- The dashboard's horizontal axis is clock time. ADR 0001 required follow-up 2 publishes times only down to the date. A screen capture would need the times removed
- The metrics have no dimensions. Only the time ranges of the local batch file separate environments, so the dashboard cannot draw them per environment. It also does not produce the Wilson interval of the success rate

## Decision

**The graphs of `experiments.md` are drawn by `tools/tm-plot`.**

| Where | Decided |
|------|---------|
| Values over time | Fetch the CloudWatch 1-minute resolution metrics with `get-metric-statistics` and draw them. The horizontal axis is elapsed time in the batch |
| Distributions and success rate | Draw from the records the analysis script (`tools/tm-analysis`) reads, with its calculation functions. The values are the same as the numbers in the tables |
| Library | matplotlib. The repository owner approved it as a test- and analysis-only dependency (NFR-5) |
| Runtime environment | A temporary environment from `uv run --with matplotlib`. It is not added to the dependency lists of the control server or the telemetry service |
| Image files | `docs/assets/phase9/`. The Korean and English documents use the same files |

The rest of ADR 0039 stays. The service puts metrics into CloudWatch, and the dashboard remains where the owner looks.

## Alternatives

| Option | Content | Reason rejected |
|----|------|-----------|
| A | Capture the dashboard screen and remove the times | It cannot draw per environment and has no Wilson interval. A capture cannot be rebuilt |
| B | Write SVG directly with the standard library | No dependency approval is needed, but the graph code must be maintained by hand. The repository owner chose matplotlib |
| C | Publish tables only, no graphs | The `roadmap.md` Phase 9 tasks require visualizing the experimental results |

## Consequences

**Gains.** The per-environment graphs can be rebuilt from the same data. No clock time enters the figures.

**Accepted limits.**

- It widens the NFR-5 test-only dependency to a test- and analysis-only dependency. What the instructor accepted in [ADR 0005](0005-test-framework-catch2.md) is the split for test runners. Putting analysis tools in the same kind is the repository owner's judgment
- The graphs over time can be redrawn only if the values are fetched within the 15 days CloudWatch keeps 1-minute resolution
- The image files stay in the repo as binaries. When the data changes, they are redrawn and committed together
