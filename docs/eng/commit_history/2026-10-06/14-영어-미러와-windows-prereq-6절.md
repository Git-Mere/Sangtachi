# English mirror, windows-prereq section 6 hands-on check

## Why

These are two items of `plan.md` "Next". The English mirror must be done before a push, and the document
gate was blocked by `mirror` and `parity`. `windows-prereq.md` section 6 could run only after the server
was deployed, and it is deployed now.

## What changed

| File | What |
|------|------|
| `docs/eng/control_plane.md` | All Korean changes after `2c1d684`. Case tables replaced by links to `control-server/tests/`, constants, the 7.2 pseudocode, 7.6 deployment, call settings and CloudWatch, chapters 8~9 |
| `docs/eng/architecture.md`, `concurrency.md`, `protocol.md`, `roadmap.md`, `spec.md` | The Korean changes in the same range |
| `docs/eng/decisions/0014-control-server-tests-pytest-and-docker.md` | New mirror. The file name is English like the other English ADRs. The gate pairs ADRs by number |
| `docs/eng/commit_history/2026-10-06/05~12` | Eight new mirrors |
| `docs/{kor,eng}/control_plane.md` 6.3 | "There are two exceptions" to "three". Three items follow. The translating worker found it |
| `docs/{kor,eng}/windows-prereq.md` section 6 | Status from "partial" to "measured". The status count at the top is now measured 5, partly measured 4, unverified 4 |
| `docs/kor/plan.md` | Removed the two "Next" items. Removed the English mirror item from document debt |

The mirror work was split among three sub-workers (`control_plane.md`, the other living documents and
the ADR, the records). They were told to translate only and not to change the Korean.

## windows-prereq section 6

Two read-only commands.

| Decision | Result |
|------|------|
| Inside the instance, `ss -ltnp4 'sport = :8000'` | `0.0.0.0:8000`. The non-IPv4-only form of the same query shows the same. `net.ipv6.bindv6only = 0`, service `active` |
| `Test-NetConnection -Port 8000` on this machine | `TcpTestSucceeded : True` |

The public address is only in `deploy/aws.local.md` and is not written here.

## Raised by the workers and not changed

- Decision 3 of ADR 0014 and record 10 point to "rule 6 of `CLAUDE.md`". The current `CLAUDE.md` has no
  numbered rules, and that rule is rule 6 of the `sangtachi-document-workflow` skill. Both are records and
  were correct at the time, so they are not changed
- The code block in `control_plane.md` 8.4 is one line longer in English. It was so before this sync and
  the meaning is the same

## Verification

| What | Result |
|------|------|
| `python tools/docgate/docgate.py` | `VERDICT: pass` |
| `python tools/docgate/docgate.py --claims` | `VERDICT: pass` |

## Review input of the previous record (13)

In the three cross reviews of 13, the command that passed the diff on standard input resolved the path
wrongly in the shell (`type` and backslashes), so **standard input was empty.** The reviewer read the
staged diff from the repository itself, read-only, and judged it. The files and line numbers of the
findings match the staged content at that time. From this commit on, the diff is passed with `cat`.

## Cross-model review

Codex. Two lenses (translation fidelity of `control_plane.md`; translation fidelity of the other files plus `windows-prereq` and `plan.md`).

| Lens | Result | Handling |
|------|--------|----------|
| `control_plane.md` | `LGTM` | - |
| the rest | nit 1. "20 sends" in the English of record 08 lost "20번씩" (`503` and `413` were each sent 20 times) | Applied |
