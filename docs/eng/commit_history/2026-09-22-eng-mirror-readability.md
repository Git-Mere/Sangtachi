# English mirror update and document rule cleanup (2026-09-22)

A push was requested, so per the `CLAUDE.md` convention the English mirror was brought up to date
first. In the same commit the finished section of `plan.md` was removed and the readability rules were
added to `CLAUDE.md`.

## The mirror had fallen two commits behind

It was not only `7ae6ad0` (readability cleanup). The commit before it, `de1ef0c` (design audit 3
follow-up), had also changed Korean only and left English untouched. **This was missed at first, and
the workers were told to port only the structural changes of `7ae6ad0`.**

| Document | Lines `de1ef0c` added to Korean |
|------|------:|
| `control_plane.md` | 224 |
| `protocol.md` | 170 |
| `windows-prereq.md` | 108 |
| `architecture.md` | 71 |
| `roadmap.md` | 68 |
| `spec.md` | 4 |

**Left as it was, the result would have been a mirror with matching structure but missing content.**
`parity` counts table rows and links, so missing content makes the numbers disagree; forcing the
numbers to agree by inventing rows that do not exist or deleting rows that do gives **a result that
passes the machine check while the content is wrong.**

It surfaced because the `spec.md` worker reported "the English table cells are staler than the Korean
ones. `parity` does not look at cell contents, so it is green. This is outside the current scope, so I
left it." A judgment that stayed inside its scope exposed the hole in the instruction.

The scope was corrected and resent to everyone. The order is **content (`de1ef0c`) first, structure
(`7ae6ad0`) second**. Four of the six had already found the same thing on their own and were acting on
it before the correction arrived.

## Changes

| Kind | Files |
|------|------|
| New | See the "New files" table below |
| Updated | `eng/architecture.md`, `eng/control_plane.md`, `eng/protocol.md`, `eng/roadmap.md`, `eng/spec.md`, `eng/windows-prereq.md` |
| Korean | `kor/plan.md` finished section deleted, one number in `kor/protocol.md` 5.6 |
| Convention | `CLAUDE.md` |

### New files

| Path | Lines |
|------|-------|
| `eng/audit-history/design-audit3.md` | 690 |
| `eng/commit_history/2026-09-22-문서-가독성-정리.md` | 156 |
| `eng/commit_history/2026-09-23-design-audit3-followup.md` | 176 |
| `eng/commit_history/2026-09-23-first-design-to-records.md` | 94 |
| `kor/commit_history/2026-09-22-eng-mirror-readability.md` | the Korean original of this file |
| `eng/commit_history/2026-09-22-eng-mirror-readability.md` | this file |

### `plan.md` 80 lines -> 45 lines

The "design audit follow-up" section was deleted. All six groups are finished and the 3 owner
decisions are settled; the work is fully done. `CLAUDE.md` states that `plan.md` "does not carry the
history of finished work".

**Before deleting, the obligations that section carried were checked.** Two of the 3 owner decisions
were already in `CLAUDE.md` (ADR links allowed, location of `first_design.md`). One was **nowhere.**

| Obligation | Where it went |
|------|-------------|
| A live document may link to an ADR in `decisions/` | Already in `CLAUDE.md` |
| `first_design.md` stays in `decisions/` and is not edited | Already in `CLAUDE.md` |
| The `**학기:**` line at the head of `roadmap.md` stays | **It was nowhere.** Written as an exception under the date rule in `CLAUDE.md` |

Had that one not been moved, the next session would have read "do not write dates in live documents"
and deleted it.

### Six readability rules in `CLAUDE.md`

The rules written during `7ae6ad0` were added to the "when working on documents" section. The
repository owner gave approval. The scope (live documents only, record folders excluded) and the
source commit record were written with them.

### One number in `kor/protocol.md` 5.6

`de1ef0c` added `0x03` to the `CLOSE` reason, making four values, but the sentence above it still
said "receive handling is the same for **three** of them". It was changed to four and the English was
matched.

**The English worker found it and reported it without fixing it.** It is a live document, not a
record, but Korean is the original, so fixing English first would put the two languages out of step.
Korean was fixed first.

## Method

Nine workers ran, one unit of work each: 6 live documents, 1 for `design-audit3.md`, and
1 for the group of 3 `commit_history/` files ran in parallel; the English pair of this record ran after them.

**The 6 live documents were instructed as structure porting, not re-translation.** The English text
already existed, so instead of translating the sentences again, the structural changes applied to the
Korean were applied identically to the English. Only the newly created places (heading titles, group
labels, quote block labels) are written in English.

The constraints are exactly `parity` from `docgate.py`: matching heading level order, table row count,
code block count, and link count.

**Korean file names remain in the English tree.** A review raised this; it is dismissed.
`pair_documents` in `docgate.py` pairs `commit_history/` and `audit-history/` by **identical file
name** only. Leading-number pairing applies inside `NUMBER_PAIRED_DIRS` (`decisions/` alone).
Renaming to an English slug breaks the pair. The precedent is
`eng/commit_history/2026-09-22-텔레메트리-분리-dynamodb.md`.

**Places appeared where a worker had to guess another worker's English heading name.** This happens
when one document refers to a section of another by name and that document is not mirrored yet. Three
workers reported this risk. After the work, every case was checked against the result and all matched.

| Guessed name | Actual |
|-------------|------|
| `2.5 Virtual IP Pool`, `2.6 Constants`, `3.2 Address` | Match |
| `6.1 Storage Contracts`, `8.2 Time Limits` | Match |
| `10.4 Endpoint Learning` | Match |
| `7.4 Clock` | **Wrong.** The actual name is `7.4 Clocks`. Fixed |

The work was uncoordinated, yet the new names came out uniform: 86 of `> **Why.**`, 3 of
`### Why It Was Decided This Way`, and 1 of `### Test Harness`. The rationale block counts match the
Korean in all six documents.

| Document | kor | eng |
|------|:---:|:---:|
| `spec.md` | 3 | 3 |
| `roadmap.md` | 26 | 26 |
| `architecture.md` | 19 | 19 |
| `control_plane.md` | 21 | 21 |
| `protocol.md` | 11 | 11 |
| `windows-prereq.md` | 8 | 8 |

## Verification

- `docgate.py` -> `VERDICT: pass`. `pairs=50 links=503 findings=0`. Mirror pairs went from 45 to 50
- Instrument -> all six English documents show 0 duplicate links, 0 unnamed number references, 0 long
  lines
- **The instrument was fixed once.** The exemption for a paragraph-leading label stopped at 40
  characters, so the same place was counted differently per language. The Korean
  "**후보를 넣을 수 있는 쪽은...**" (26 characters) was exempt while the English of the same meaning
  (66 characters) was counted as emphasis. Raising it to 100 characters left Korean at 0 and brought
  English down from 27 to 1. The remaining 1 is a place where the English sentence starts with an
  article, so the bold is not at the head of the paragraph. The bold layout is the same as the Korean
- **A cross-document section reference checker was written.** It finds `` `doc.md` N.N Name `` places
  and compares them against the actual heading titles of that document. The first version produced 8
  false positives. It read places where a sentence, not a name, follows the number
  (`8.4 검사 12번이 본다`) as a name. It was narrowed to compare the first word only
- The workers each checked code spans and numbers one by one and confirmed 0 losses

## What the workers caught

Places the machine checks missed, or things outside the instruction that were found and reported.

| Who | What |
|------|------|
| `spec.md` | The English table cells were staler than the Korean. This exposed one more lagging commit |
| `spec.md` | Used `git log` to check the lagging commits exhaustively and showed there were only two |
| `control_plane.md` | `[ADR 0004](...)` was split by a line break into `[ADR` / `0004](...)`, so the gate could not read it as a link |
| `protocol.md` | The "three" in Korean 5.6 was stale after `0x03` was added |
| `protocol.md` | A line break cut across the `` `shift == 64` `` code span. `docgate` strips code spans per line, so this place can misjudge silently |
| `windows-prereq.md` | Another document was still being worked on, so its section name was taken from the `HEAD` revision; the worker reported that fact and the need to confirm |
| Cross-model review | Reading the English mirror, it found a contradiction in the Korean original. See the item below |

## One source defect the review found

The English mirror review reported something that was not a translation problem but a
**contradiction in the Korean original**. The mirror matched the original, so it was not a
mirror defect.

| Where | What |
|-------|------|
| `architecture.md` 3.5 | **`--room` was required for `player` only.** The rejoin form of `join_room` in `control_plane.md` 4.3 requires `room_id`, and a restarted host is also a rejoin. A host that passes `--rejoin` needs `--room` as well |

Korean was fixed first and English was matched to it. The client prints `ROOM <room_id>` on
standard output, so a restarted host can supply that value again. The value was never missing;
the Required cell was wrong.

## What is left

- 4 long lines in `eng/spec.md`. All are prose paragraphs that are one line in Korean as well, and
  they are outside the sections changed this time
- 1 excess-bold case in `eng/protocol.md`. This is the language difference noted in the instrument
  item above
- The "Supported Network Conditions" reference in `spec.md` differs from the actual heading
  (`Supported network conditions (defined in advance)`) only in capitalization. The reference predates
  this commit, so it was left alone
