# English Mirror Update. Five Phase 1 Implementation Commits

**Target commit:** this commit

Five Phase 1 implementation commits had changed only the Korean documents. This commit brings
`docs/eng` in line. There is no content change.

## What Was Aligned

| Document | What |
|------|------|
| `architecture.md` | The per-span thread table in 3.2.1, two console commands and the sending-side `rx.raw` in 3.5, the whole argument-error section, the log unit contract and its byte-level limit in chapter 9, the tree in chapter 10, Catch2 in chapter 11 |
| `spec.md` | NFR-5 and the two-kind table under it |
| `roadmap.md` | Two Phase 1 tasks and the `/WX` verification |
| `commit_history/` | Five new Phase 1 implementation records |
| `decisions/` | New ADR 0005 |

## How It Was Checked

The document gate reports `VERDICT: pass` (`pairs=56 links=525 findings=0`).

**The gate alone was not trusted.** The gate checks heading structure, table row counts, code
block counts, link counts, and link reachability. It does not check that the content carried
over. So for each pair the **sets of numbers were compared by script.** The question was whether
every figure in the Korean document also appears in the English one.

One case came up. `8` was absent on the English side, but that was the original "8진수" rendered
as "octal", so nothing was lost. The other five pairs had no difference.

> **Why numbers.** Figures are what a translation loses quietly, and these records hold measured
> values. If a figure is wrong, the next session reads a wrong basis.

## It Was Split Up

Five commit records and one ADR were handed to three subagents. The files do not overlap, so
there was no conflict. The three living documents are edits layered onto existing English text,
so those were done directly.

## Cross-Model Review

Run with a translation-fidelity lens. Of four findings, **two were taken and two were dismissed.**

| Grade | Finding | Handling |
|------|------|------|
| warn | "Product dependency" was defined as shipping inside the product executable, but `boto3` is server side and Wintun is a driver. The definition is narrower than the examples | **Taken.** This was not a translation problem but **a defect in the Korean original**. Both sides were fixed. The judgement became "does the shipped artifact run without it", with the check spelled out for C++ and for Python |
| nit | The `counters` sentence in 3.5 reads awkwardly | **Taken** |
| warn | One English mirror uses a Korean file name | **Dismissed.** That is the convention of this repository. Two of the same form already exist in `docs/eng/commit_history/`, and the document gate pairs `commit_history/` **by identical file name.** Renaming breaks the pair. Only `decisions/` has per-language names, and that directory pairs by number |
| warn | The ADR links to a path with a Korean file name | **Dismissed.** Same reason. That is the actual file name |

**The translation lens found a defect in the original.** Reading the same sentence again while
moving it is itself a check.
