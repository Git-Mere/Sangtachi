# Removed the "Difference from the Current State" Section from `architecture.md`

## Changes

- Deleted the `Difference from the Current State` section of chapter 10 Repository Structure in
  `docs/{kor,eng}/architecture.md`

## Decisions

**It was progress narration, so it was removed.** The section said the source tree was still only
`src/main.cpp` at the root. `client/`, `control-server/` and `telemetry-server/` already exist in
the repository, so it is not the current spec.

**The obligation it carried is already in `roadmap.md`.** The Phase 1 task list says "relocate the
repository into the target structure", which is the same thing. This was checked before deleting.

## Verification

- `docgate.py` gives `VERDICT: pass`. No link pointed at the section. Both the heading text and the
  two anchor forms were scanned and found zero references
- The same section was removed in Korean and English, 10 lines on each side

## Cross-Model Review

A deletion lens was run with Codex, because a deleting diff hides the hole it leaves.

| Lens | Result |
|------|--------|
| Deletion. Other files pointing at the section, obligations that vanished, the record's claim about `roadmap.md`, coherence of the remaining text | `LGTM - no blockers` |

There were no findings, so nothing was applied or dismissed.
