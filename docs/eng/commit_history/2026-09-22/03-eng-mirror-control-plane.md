# English mirror update: control_plane.md and three lagging commits (2026-09-22)

A push was requested, so per `CLAUDE.md` the English mirror was brought up to date first. The mirror
had fallen behind commit `3274ef7` (control plane document) and the two before it, `6b33466`
(telemetry split, DynamoDB) and `3f2c2a9` (design audit 2, follow-up 1).

## Changes

| Kind | Files |
|------|-------|
| New | `eng/control_plane.md`, `eng/decisions/0003-telemetry-service-split.md`, `eng/decisions/0004-state-store-dynamodb.md`, `eng/audit-history/design-audit2.md`, `eng/commit_history/2026-09-21/15-design-audit2-followup1.md`, `eng/commit_history/2026-09-22/01-텔레메트리-분리-dynamodb.md`, `eng/commit_history/2026-09-22/02-control-plane-doc.md` |
| Full re-translation | `eng/architecture.md`, `eng/protocol.md`, `eng/roadmap.md`, `eng/spec.md`, `eng/windows-prereq.md`. Not patched; the current Korean text was translated in full |
| Korean | One `> English version:` line added to `kor/control_plane.md` |

**Why full re-translation rather than patching.** Three commits' worth of changes had accumulated;
following the diffs would miss things. Translating the full Korean text and letting the gate check the
structure is more reliable.

## Method

Five translation workers ran in parallel, one per file group. Each was given exactly the constraints
of `docgate.py` `parity`: identical heading structure, table row count, code block count, and link
count; code block contents unchanged. ADRs with Korean file names were paired by their English names
(`0003-telemetry-service-split.md`, `0004-state-store-dynamodb.md`); `commit_history/` pairs by exact
name, so `2026-09-22/01-텔레메트리-분리-dynamodb.md` keeps the same name on the English side. Links to
`plan.md` point to `../kor/plan.md` from the English side, since that document is Korean-only.

Three pieces of Hangul remain in the English files: the adapter name `이더넷 4` in measured output in
`windows-prereq.md`, and the Korean ADR file names inside a record file. Both are facts and were left
untranslated.

## Verification

- `docgate.py`: **`VERDICT: pass`.** pairs 44, links 629, findings 0. HEAD (`3274ef7`) failed with
  `mirror` 7 and `parity` 12
- Residual Hangul scan: all English files scanned with `[가-힣]`; only the three items above remain

## Cross-model review

One Codex pass with a translation-fidelity lens. Input was the full `eng/control_plane.md`, and the
reviewer was also asked to compare `architecture.md` 3.2.8 and 3.5 against the Korean. It looked for
inverted meaning, dropped negations, changed numbers or section references, changed claim strength,
and omitted sentences. Result: **`LGTM`**. Structural parity was not delegated to the reviewer; the
gate already judges it.
