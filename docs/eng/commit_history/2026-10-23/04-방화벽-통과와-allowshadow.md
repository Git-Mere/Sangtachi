# Firewall rules passing traffic reflected, `-AllowShadow` not exposed (ADR 0033)

## Why

The repository owner asked whether Phases 1–7 close once 6-5 passes. Comparing `plan.md` with the living documents
found two loose ends tied to Phases 1–7.

- `windows-prereq.md` section 2 said it had not yet been seen whether the firewall rules the client creates let traffic
  through. In the 2026-10-22 field test, 6-2 (virtual IP `ping` 4/4 both ways) and 6-3 (64 MiB transfer over TCP 25565
  passed) saw exactly that. In 6-1 B created the two rules fresh. This was missed in
  [01](01-두-집-실측-결과-반영.md), which reflected the results
- The `-AllowShadow` row of `plan.md` said it would be decided "when the subnet check is connected to the client in
  Phase 6", but Phase 6 ended without deciding. The repository owner decided not to expose it to users

## What changed

| What | Where |
|------|-------|
| Section 2 status is now measured. The evidence that traffic passed is written and the preamble's "not seen yet" removed. Section counts now 7 measured, 3 partly measured, 2 unverified, 1 human-checked. Section 10's human check used to be counted as unverified | `windows-prereq.md` preamble, section 2 |
| The client does not expose `-AllowShadow` to users | `windows-prereq.md` section 7 |
| The decision above and the rejected alternative | ADR 0033 |
| The waiting row now covers only section 3 leftover addresses (Phase 8). The `-AllowShadow` row removed | `plan.md` |

## What remains for Phases 1–7 after 6-5

Nothing. The remaining waiting items in `plan.md` (section 3 leftover addresses, NFR-5 professor confirmation, nat-probe
follow-up) and the places to check during implementation are Phase 8 or later, or are checked when their trigger occurs.

## Verification

| What | Result |
|------|--------|
| `docgate.py`, `--claims` | `VERDICT: pass` |
| Section counts | Recounted the `Status:` lines: 7 measured, 3 partly measured, 2 unverified, 1 human-checked |
| The client does not pass `-AllowShadow` | The string does not occur in `client/` |

## Cross-model review

Codex, with the stale-claim and correctness lens and the evidence and mirror lens in one run.

| Finding | Disposition |
|---------|-------------|
| warn. Section 10 is "human-checked", so 3 unverified in the section counts was wrong | Fixed (both languages). Counted separately as 2 unverified, 1 human-checked |

After the fix, the confirmation review of the full staged diff was `LGTM - no blockers`.
