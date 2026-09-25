# Added `concurrency.md` to the Session Reading Order

## Changes

- Added `docs/kor/concurrency.md` as item 6 of the reading order in `CLAUDE.md`. The control plane
  item became item 7

## Decisions

**It is read unconditionally.** The repository owner called item 6 a core document. It does not
carry a qualifier such as "if the work touches that part", which item 7 `control_plane.md` has.

> **Why.** Thread ownership and loop order are rules that nearly all client code leans on.
> Whichever file is edited, it is edited inside those rules.

**This is a rule change, so it went in with the repository owner's approval.** The commit that
split out the concurrency model left the reading order alone and asked; approval came back, and it
goes in here.

## Verification

- `docgate.py` gives `VERDICT: pass`
- `CLAUDE.md` is not mirrored. It is a single Korean document at the repository root

## Cross-Model Review

One call with two lenses was run with Codex.

| Lens | Result |
|------|--------|
| Consistency. The renumbered list, agreement with the document role table and the living-document lists | `LGTM - no blockers` |
| Record. Honesty about the change, the reason and the approval, and the claim that `CLAUDE.md` is not mirrored | `LGTM - no blockers` |

There were no findings, so nothing was applied or dismissed.
