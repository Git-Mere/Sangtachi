---
name: sangtachi-document-workflow
description: Follow Sangtachi-specific documentation rules and validation workflow. Use whenever editing, restructuring, reviewing, reducing, or adding claims to docs/kor or docs/eng, changing a living specification, synchronizing Korean/English mirrors, modifying document links or structure, or preparing documentation for review or commit.
---

# Sangtachi Documentation Workflow

Apply this workflow in addition to the repository's CLAUDE.md.

Do not replace the repository's source-of-truth rules with generic documentation conventions.

## 1. Identify the source of truth

Before editing, determine which living document owns the claim:

- requirements and success criteria -> `spec.md`
- Phase scope, timing, deliverables, verification -> `roadmap.md`
- system composition and module architecture -> `architecture.md`
- tunnel/STUN wire format, protocol states, protocol timers -> `protocol.md`
- client threads, loops, waits, timers, and state ownership -> `concurrency.md`
- control-plane API, HTTP/JSON, room/peer state, DynamoDB, client/server contract -> `control_plane.md`
- execution prerequisites -> `windows-prereq.md`
- current remaining work and handoff state -> `plan.md`

Do not duplicate a normative claim across living documents.

Put the authoritative claim in one document and reference it elsewhere.

Historical records may contain snapshots but are not the authority for current behavior.

`decisions/` ADRs may be linked for rationale and rejected alternatives, but current normative values still come from living documents.

## 2. Inspect before changing

If changing an existing definition, requirement, identifier, section structure, or repeated claim:

1. Search the entire repository for dependent wording and references.
2. Do not restrict the search to `*.md`.
3. Exclude `.git`, but inspect source, scripts, configuration, and documentation.
4. Make a list of affected locations before editing them.

If weakening or strengthening a claim, search for every place that depends on the old strength.

## 3. Preserve obligations when deleting or moving text

Before removing a statement, determine what obligation it carries.

For every removed requirement, constraint, validation step, warning, or decision:

- keep it at the existing source of truth,
- move it to the correct source of truth,
- explicitly retire it through an approved decision,
- or prove that another existing rule already carries the obligation.

Never delete first and discover the missing obligation during review.

## 4. Trace one complete scenario

Before declaring a substantial document change complete, trace at least one relevant scenario end to end.

Examples:

- one packet through its full path,
- one failure from detection through recovery,
- one restart sequence,
- one control-plane operation from request through storage and response.

Component lists and isolated validation bullets are not enough.

Confirm that the documents collectively define the entire scenario.

## 5. Do not invent facts to fill gaps

When a technical fact cannot currently be verified:

- do not state it as fact,
- define only the contract that is known,
- record what still needs verification,
- place the verification timing in `roadmap.md` when appropriate.

Do not invent API behavior, platform behavior, error semantics, or measured values because a document appears incomplete.

## 6. Treat executable procedures like code

When documentation includes an executable procedure, review:

- input validation,
- failure handling,
- cleanup,
- required permissions,
- timeout behavior,
- interpretation of results,
- conditions under which the result is valid.

Prefer implementing fragile procedures as tools/scripts and linking to them from documentation.

Do not place an unexecuted test script into a normative living document and present it as validated procedure.

## 7. Verdict-producing procedures require cases first

For any procedure or function that classifies or produces a verdict:

1. Write the case table first.
2. Include positive, negative, boundary, and unknown cases.
3. Ensure the cases can be executed where the procedure lives.
4. Default unknown or unverified situations to "cannot determine", not success.

Prefer explicit allowlists over broad exclusion logic where safety depends on classification.

Do not trust a standard-library predicate solely from its name.

## 8. Maintain living-document readability

For living documents:

- separate the rule from its rationale,
- link the same document only on first meaningful use within a section,
- add the actual heading name when referring to numbered sections,
- use at most one emphasis target per paragraph, bullet, or table cell,
- remove historical "we changed X to Y" narration from living specifications,
- do not mix criteria, procedure, rationale, and counterexamples in one oversized bullet or table cell.

If rationale is short, place it in a clearly separated rationale block.

If rationale is substantial, place it in an appropriate rationale section or ADR.

Do not remove information merely to reduce length.

Preserve statements whose absence could make an incorrect result appear correct.

## 9. Korean and English mirrors

Korean is authoritative.

During active editing, update `docs/kor` first.

Temporary mirror/parity failures are allowed while the change is incomplete.

Before final review, commit, or push:

1. synchronize the corresponding `docs/eng` mirror,
2. except for `plan.md`, which is Korean-only,
3. run the document gates.

## 10. Run document gates

Run:

```bash
python tools/docgate/docgate.py
python tools/docgate/docgate.py --claims
```

The normal gate must end with:

```text
VERDICT: pass
```

Use `--claims` to inspect strong normative assertions when changing claim strength or definitions.

Do not ignore mirror, heading, link, table, or code-block failures merely because the prose appears correct.

## 11. Review your own repair

After fixing a review finding, inspect not only the old text but also explanatory text added during previous repair rounds.

When a definition changes:

1. identify every sentence that depended on that definition,
2. reread those sentences,
3. update all consequences in the same repair.

Do not search only for the exact stale phrase.

## 12. Record the result

For a meaningful commit, update the required `commit_history` record with:

- what changed,
- important decisions,
- validation performed,
- cross-model review results,
- accepted findings,
- rejected findings and rationale.

Do not rewrite historical records when the current design later changes.

## Exit criteria

Documentation work is complete only when:

- the authoritative source is clear,
- dependent claims have been checked,
- obligations were not silently lost,
- at least one relevant end-to-end scenario was traced,
- unsupported facts were not introduced,
- Korean/English mirrors are correct,
- document gates pass,
- review findings are resolved or explicitly rejected with rationale.
