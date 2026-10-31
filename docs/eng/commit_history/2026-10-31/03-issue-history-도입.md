# Introduce issue_history, when to clean up the Linux field-test machine

## Why

Decisions live in `decisions/` and per-commit changes in `commit_history/`. There was no single place to find an issue that spans
several commits or has its cause outside the code, from symptom to handling. The repository owner chose a numbered record that follows the `decisions/` format.

## What changed

| What | Where |
|------|-------|
| New record folder. When to write one, the structure (Symptom / Investigation / Cause / Handling / Result), three states (Resolved, Worked around, Unresolved), the update rule | `issue_history/README.md` |
| The four issues met in this Linux work | `issue_history/0001` to `0004` |
| The list of record documents, the update rule, allowed links, where to read at session start. The owner approved the wording | `CLAUDE.md` |
| The passwordless `sudo` and Wi-Fi power saving on `mere-galaxy` are cleaned up together after the two-machine field tests (owner's decision). The Wi-Fi item points at issue 0001 | `plan.md` |

- **When to write one.** A defect or environment problem whose cause needed investigation. So the Minecraft whitelist, Docker not running and the
  empty value in the usage skill from this work were not written. Their cause was obvious at once
- **English file names are the same as the Korean ones.** As in `commit_history/`. English names as in `decisions/` would need a change to
  `NUMBER_PAIRED_DIRS` in docgate, so they were not used
- Corrected while writing issue 0004: the field checks in both directions were run with the build before `IFF_TUN_EXCL`. The previous record
  ([`02-linux-콘솔-클라이언트.md`](02-linux-콘솔-클라이언트.md)) does not say they were run with the fixed build, so it is left as is

## Verification

| What | Result |
|------|--------|
| `docgate.py`, `--claims` | `VERDICT: pass` |

## Cross-model review

Codex (`gpt-6.1-sol`). An evidence lens and a consistency lens in one run. Two warns, one nit.

| Finding | Disposition |
|---------|-------------|
| warn: the 189ms in issue 0001 and the ranges after turning power saving off are not in the cited record or the worker reports | Partly accepted. The values were copied from `ping` output seen in this session, so they stay. The document now says the raw output is not in the repository |
| warn: the 58ms and 0.13 seconds in issue 0002 are written without kept output | Partly accepted. For the same reason they stay, and the document says they were copied from the measurement script output and the raw output was not kept |
| nit: "four samples" in 0001 does not match the first measurement of six | Accepted. Changed to "four or six" |
