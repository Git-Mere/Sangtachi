# 2026-09-10 Repository Scaffold

## Changes

Set up the basic structure of an empty repository, following the items in the CSP GitHub Best Practices lecture (src/03).

- `.gitignore`: excludes `.env`, build output, dependencies, caches, editor files, OS files
- `.editorconfig`: UTF-8, LF, 4 spaces (2 for md/yml/json)
- `.env.example`: secrets live in `.env`; only example keys are committed
- `LICENSE`: MIT (without a license the repository is effectively all rights reserved)
- `README.md`: overview, install/run, environment variables, structure, license sections
- `docs/`: `spec.md`, `plan.md`, `decisions/`, `commit_history/`
- `src/`, `tests/`, `scripts/`: empty folders with `.gitkeep`

## Decisions

- Build files (`pyproject.toml` / `package.json`) and version pins were left out because the stack was undecided.
- `.github/workflows/` CI was also left out. Without lint or test commands it would be an empty shell.
- `dist/` and `lib/` were not created. The former is ignored anyway and the latter is created when needed.
- `docs/decisions/` and `docs/commit_history/` got a `README.md` describing the writing convention instead of a `.gitkeep`, so the folder's purpose is visible.

## Verification

- `git check-ignore -v`: `.env.example` stays tracked through the `!.env.example` negation rule
- `git status`: all 12 intended new files appear

## Cross-model Review

- Reviewer: Codex (codex-cli 0.151.0)
- Target: the full staged diff
- Result: `LGTM - no blockers` (no findings)
