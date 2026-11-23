# Phase 9 field measurement chapter and graphs, analysis-only dependency matplotlib

## Why

The next item in `plan.md` was the Phase 9 chapter of `experiments.md`. The `roadmap.md` Phase 9 tasks said to take the graphs from the CloudWatch dashboard, but the dashboard cannot draw per environment and its horizontal axis is clock time. The repository owner chose a graph tool and approved matplotlib.

## What was done

| What | Where |
|------|------|
| The decision that the analysis tool draws the graphs | [ADR 0042](../../decisions/0042-experiment-graphs-drawn-by-analysis-tool.md) |
| The graph sentence of FR-14. Widened the NFR-5 test-only dependency to a test- and analysis-only dependency and put matplotlib on the approved list | `spec.md` |
| matplotlib in the external dependency table, the graph sentence of the CloudWatch metrics section | `architecture.md` |
| The visualization task, the sentence that 1-minute resolution values are fetched within 15 days | `roadmap.md` |
| Chapter 6 Connection Setup and Session Quality. Pointed chapter 5's "There are no results yet" to chapter 6 | `experiments.md` |
| Graph tool | `tools/tm-plot/` |
| Three figures | `docs/assets/phase9/` |
| Changed the next item to closing Phase 9 | `plan.md` |

The CloudWatch 1-minute resolution values were fetched over SSH on the control-plane instance with the instance role. The range covers the batches of 2026-11-19 and 23 and they are in `tools/tm-analysis/local/`, ignored by git.

## The end of the home network vs home network batch was corrected

[The earlier record of the same day](01-phase-9-가정망-대-가정망-다시-재기와-분석.md) set the end of the home network vs home network batch at the time the trial script finished. The friend host's last `end` arrived a few seconds later. The analysis tool does not read records that arrive after the end of the last batch, so that one record was missing. It was found while drawing figure 1, by comparing the CloudWatch `Rtt` sample count (120) with the analysis tool's RTT sample count (114).

The batch end was moved later and the owner ran `--save` again in CloudShell. The records went up by one to 461. The output produced again with `--load` on this PC was the same except line endings.

| Value | Earlier record | After the fix |
|----|---------|---------|
| Home network vs home network RTT | 25.8ms (18.7~57.7), n=114 | 25.3ms (18.7~57.4), n=120 |
| Home network vs home network jitter | 7.8ms (6.2~15.5), n=19 | 7.7ms (6.2~15.0), n=20 |
| Home network vs home network loss | 0/315 | 0/330 |
| Real environments combined RTT | 27.9ms, n=572 | 27.7ms, n=578 |
| Real environments combined jitter | 6.6ms, n=97 | 6.6ms, n=98 |
| Real environments combined loss | 1/1582 | 1/1597 |

The other lines of the report are the same. After the fix, the RTT sample counts of the four real environments equal CloudWatch's per-batch `Rtt` sample counts.

## Graph tool

- Figure 1 is the per-minute median and quartiles of CloudWatch `Rtt`. A 1-minute data point's time is the start of its interval, so every interval that overlaps the batch is included. At first only intervals after the batch start were included and the first interval was dropped
- Figures 2 and 3 use `assign`, `summarize`, and `wilson` of `tmanalysis` as is. The values are the same as the numbers in the tables
- Drawing twice from the same input gave byte-identical PNGs. The PNG metadata is only the matplotlib version and dpi
- At first `uv pip install matplotlib` followed `VIRTUAL_ENV` and installed into `control-server/.venv`. It was removed, and `uv pip check` and the control server tests (747 passed, 290 skipped) were run again. The tool runs with `uv run --no-project --with matplotlib`

## Decisions

- The peer of this home network vs home network batch is written as device F, network W of `experiments.md`. The name in the `host.cmd` given to the friend is `F_host`
- In this home network vs home network batch, A's network is written as H. A's public address logged by the trial script is the same as the public address of A and G in the 2026-11-19 same LAN batch. This confirms what the earlier record said could not be confirmed
- What the instructor accepted in ADR 0005 is the split for test runners. Putting analysis tools in the same kind is the repository owner's judgment, written in ADR 0042

## Verification

| What | Result |
|------|------|
| `docgate.py` | `VERDICT: pass` |
| `experiments.md` 6.2 table and the analysis tool output | Checked by script that the values in the table (successes/attempts and Wilson interval, setup times, RTT, jitter median, loss) appear in the report |
| Graph tool | Redrawn with the README command, the figures matched the committed ones. Runs with a batch with no attempts, a batch with no `Rtt` data points, and a batch file where one 1-minute interval straddles two batches showed a 0/0 mark, a normal exit after a warning, and a stop with an error respectively |

## Cross-model review

Two Codex (`gpt-6.1-sol`) reviews were run separately. The document review used the lenses raw output and number check (A), Korean/English parity (B), consistency among living documents (C), and public repo exposure (D). The raw output was handed over as an uncommitted local file. The code review looked at the plotted values, empty data, clock time exposure, and the README command of `tools/tm-plot`.

| Finding | Handling |
|------|------|
| [C] blocker. `spec.md` FR-14 still says the dashboard draws the graphs | Accepted. Changed it to: the metrics are viewed in the dashboard and the field report's graphs are drawn by the analysis tool |
| [C] blocker. "The professor accepted this split" covers analysis tools too | Accepted. Narrowed what the professor accepted to the split between product and test-only, and wrote analysis tools as the owner's judgment |
| [A] warn. The changed values after the fix missed the jitter interquartile range and the combined jitter n | Accepted. Added the two lines to the table |
| [C] warn. Chapter 1's definition of RTT and loss differs from chapter 6's sources | Accepted. Limited the log definitions to chapters 3 and 4 and stated chapter 6's sources |
| [C] warn. No evidence for writing A as network H in home network vs home network | Confirmed by comparing the public address with the 2026-11-19 same LAN batch. Written in the raw material and in this record's Decisions |
| [A] warn. The verification "the only values not in the table are …" is broader than the actual comparison | Accepted. Only the compared values are listed |
| nit. Figure 2's "one dot is one attempt" does not say failed attempts are left out | Accepted |
| [C] warn. Attributing all of chapter 6 to telemetry analysis also covers the log values of 6.5 | Accepted. The sources of 6.2~6.4 and 6.5 are written separately |
| Code blocker. Division by zero for an environment with zero attempts | Accepted. It writes only 0/0 and skips |
| Code warn. A 1-minute interval starting before the batch is drawn at negative elapsed time | Accepted. It is placed at the batch start |
| Code warn. A 1-minute interval can mix samples of another batch | Accepted. It stops when one interval straddles two batches, and the README states the premise about the minute before a batch starts |
| Code warn. An environment's batches out of time order in the file are joined backward | Accepted. They are sorted by start time |
| Code warn. The README's `--label` example, Bash notation, angle brackets and spaces | Accepted. Replaced with the actual command and ran it as is |
| Code warn. With no `Rtt` data points it stops at the log axis | Accepted. It warns and skips |
| [A] warn. The raw material lacks evidence for the graph tool checks | Accepted. Added the commands, outputs, and exit codes to the raw material |

After the fixes, the final staged diff was run again with the same lenses. `LGTM - no blockers`.
