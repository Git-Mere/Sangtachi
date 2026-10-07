---
name: sangtachi-review-workflow
description: Run Sangtachi-specific review and cross-review procedures. Use when reviewing code or documentation changes, performing a Phase audit, repairing review findings, preparing a staged diff for commit, deciding review scope or lenses, or validating that a cross-model review is complete.
---

# Sangtachi Review Workflow

Apply this workflow alongside `code-review-and-quality`.

This skill defines Sangtachi-specific review orchestration, evidence handling, and cross-review rules.

## 1. Self-review before external review

Before sending a diff to another reviewer:

- inspect the diff yourself,
- search for contradictions created by the change,
- search for duplicated or stale versions of modified claims,
- run deterministic repository gates first.

Do not spend reviewer effort on failures a script can detect.

## 2. Review using distinct lenses

One clean review does not imply the change is clean.

Select lenses appropriate to the change.

Useful lenses include:

- correctness and contract preservation,
- end-to-end scenario tracing,
- evidence and unsupported-claim review,
- stale-description or duplicated-claim review,
- regression and repair review,
- record and handoff consistency,
- security or permissions,
- concurrency and lifecycle behavior.

Different lenses may find disjoint defects.

Do not drop an important lens merely because another lens returned LGTM.

## 3. Keep each review scope narrow

For repair rounds, provide the reviewer only the files relevant to that lens.

Prefer scoped diffs such as:

```bash
git diff --cached -- <paths>
```

Use the complete staged diff only for the final confirmation round.

Do not repeatedly send already-reviewed unrelated content.

## 4. Handle historical records separately

During ordinary repair rounds, normally exclude:

- `audit-history/`
- old `commit_history/`
- unrelated `decisions/`

They are historical snapshots and create unnecessary review context.

For the final pre-commit review, include:

- `plan.md`,
- the new commit-history entry,
- other records created or modified by the current change.

These must receive at least one final review because they affect future sessions.

## 5. Automate countable checks

Do not ask an LLM reviewer to be the primary authority for:

- counts,
- enumerated lists,
- numbering consistency,
- mirror existence,
- mechanically checkable links,
- deterministic gate conditions.

Use scripts for these.

Use reviewers for judgment:

- semantic contradictions,
- missing contracts,
- bad assumptions,
- architecture problems,
- misleading claims,
- incomplete failure handling.

## 6. Verify "missing" findings against HEAD

A reviewer may inspect only the provided diff and falsely conclude that something does not exist.

For findings such as:

- "there is no test",
- "this validation is missing",
- "this requirement is undocumented",
- "this function does not handle X",

inspect repository HEAD before accepting the finding.

Do not reject the concern automatically either.

A technically incorrect finding may still expose a real observability, documentation, or test-coverage gap.

## 7. Treat rejected findings as useful signals

When rejecting a finding:

1. explain why the reported defect is not present,
2. determine why the reviewer reasonably suspected it,
3. check whether better tests, comments, contracts, or structure would make the behavior provable.

A rejected finding does not necessarily mean no change is useful.

Record significant rejection rationale in the current commit history.

## 8. Bundle lenses conservatively

When combining review lenses into one reviewer call:

- combine at most two substantial lenses,
- require findings to identify which lens produced them,
- verify that both requested lenses were actually addressed.

If a requested lens is omitted, rerun that lens separately.

Use individual review calls for high-risk or blocker-oriented lenses when needed.

## 9. Do not rely on process-start claims

A review is considered complete only when a valid review artifact or result exists.

"Started", "running", or process-spawn success is not review completion.

Do not create review markers merely because a reviewer process was launched.

## 10. Cross-review before commit

Run the repository's `cross-review` workflow before commit as required by CLAUDE.md.

The staged diff hash used by the gate must correspond to the diff that was actually reviewed.

If the staged diff changes after review, review the changed staged diff again.

Never fabricate or manually create a successful marker without a valid review result.

## 11. Iterate until disposition is complete

For every finding, produce one of:

- fixed,
- accepted for explicitly scheduled follow-up,
- rejected with technical rationale.

After repairs, review the repaired area again.

Continue until there are no unresolved blocking findings.

## 12. Final confirmation

The final confirmation round should inspect the complete staged change and verify:

- current code/docs,
- `plan.md`,
- current commit-history record,
- cross-document consistency,
- tests/gates,
- unresolved findings.

Only this final round should routinely receive the full staged diff.

## Exit criteria

Review is complete only when:

- deterministic gates were run,
- appropriate semantic lenses were applied,
- "missing" claims were checked against HEAD,
- every finding has a disposition,
- repaired areas were rechecked,
- current records were included in final review,
- a valid cross-review result exists for the staged diff.
