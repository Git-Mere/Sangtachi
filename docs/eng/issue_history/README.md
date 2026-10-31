# Issue History

Issues we ran into and how they were handled are recorded here as `NNNN-title.md`. The number is the order of recording. There are no per-date folders.

Each document has five sections: Symptom / Investigation / Cause / Handling / Result.

- **When to write one.** For a defect or environment problem whose cause took investigation to find. Typos or simple mistakes fixed on the spot are not written
- **One document per issue.** An issue that spans several commits is still written as one document. The record of each commit is held by [`commit_history/`](../commit_history/README.md),
  and this document points at those commits
- **The investigation also records rejected hypotheses.** What was suspected and what showed it was not the cause. It corresponds to "Alternatives" in a decision record
- **The cause separates what was confirmed from what was inferred.** Only what a reproduction or a before/after comparison showed is written as confirmed
- **Status** is one of three
  - Resolved: the cause was fixed
  - Worked around: the cause is left as is and the symptom is avoided. Environment problems outside the code usually land here
  - Unresolved: the cause is unknown or not fixed yet
- **Closed issues are not edited.** If the same symptom comes back, it is written under a new number that points at the earlier one. Documents that are unresolved or worked around may
  update their status and result
- Remaining work is held by [`plan.md`](../../kor/plan.md). This document does not copy remaining work; `plan.md` points at the issue number

> Korean version: [`../../kor/issue_history/README.md`](../../kor/issue_history/README.md)
