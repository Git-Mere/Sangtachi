# Phase 2 Full-Text Targeted Audit

## Why

This is `CLAUDE.md` recurrence-prevention rule 2. **Diff review catches only local defects, so a full-text
targeted audit is run separately at the end of every Phase.** The scope is the documents and code
that Phase touched.

Phase 2 went through 5 rounds of diff review and handled 1 blocker and 7 warns. After that, this
audit produced **14 more blockers and 30 more warns**. Three lenses were run separately.

| Lens | What it looked at |
|------|-------------------|
| Scenario tracing | Four paths end to end from startup. Success, only one server answering, a late response, STUN during shutdown |
| Verification criteria | For each `roadmap.md` Phase 2 verification item, "what judges this today" |
| Full document text | Every sentence that depended on the four definitions this Phase changed |

**Three places were found by all three lenses.** The `timer.tick` description, the gaps in the
module table, and the wrong handover in `plan.md`. When different lenses point at the same place,
that place is a defect for certain.

## What Was Fixed

### Where a document diverged from the code

| What | How |
|------|-----|
| **`timer.tick` exists only in test builds** | Wrong. Phase 2 hung the STUN timers on the same set, so it goes out in product builds too. Chapter 9 now reads "every expiring timer emits it. The test build adds one more, `probe200`" |
| The 3.1 module table and the chapter 10 tree | The five things Phase 2 made (`network/stun`, `network/stun_client`, `platform/random`, `platform/resolve`, and the `win32` tree) were missing |
| **The default list must match in "two places"** | Phase 2 made a third place. Changed to "three" |
| What makes the server rotate | 3.5 records only the deadline, while the code also hands the slot on for success and error responses. **The three triggers were made into a table** |

### Rules that were not in any document

| What | Where it was written |
|------|----------------------|
| A response to a slot past its deadline is not used | `protocol.md` chapter 11. The only source was a code header |
| What `drop_stun_parse` counts | Chapter 13 of the same document. **Three case tables** (a waiting slot, a finished slot, no slot) |
| The three retry values are intervals, not deadlines | Chapter 11. Read as absolute times they stop doubling |
| The "list length" in the stage cap is the length after deduplication | Chapter 11 |
| A purely numeric name is not resolved | `architecture.md` 3.5 |
| `rx.raw` goes out for every datagram received | Same section |

### Code

| What | Where |
|------|-------|
| **The `WAIT_FAILED` path did not signal the shutdown event** | `loop.cpp`. Item (1) of shutdown in `concurrency.md` chapter 7 was missing on that path |
| A place where `socket.error` was missing the required field `code` | The random-failure line in `stun_client.cpp` |

**The `WAIT_FAILED` item costs nothing today.** No thread waits on that event yet. It becomes a
defect in Phase 3, when `[control]` arrives.

### Verification criteria

| What | How |
|------|-----|
| **The Phase 2 output example cannot come out of a correct implementation** | The example shows the two Google names, and the deduplication added this time drops the second. The example was changed to the real output and a `WARN stun.duplicate` line was added |
| **`plan.md` recorded the remaining Phase 2 verification as "measurement on two machines"** | Wrong. No Phase 2 verification item needs two machines. What is left is observation against public STUN servers, and one machine does it |
| Where the observation is recorded | Nowhere. `roadmap.md` now says it is kept outside the repo. Public IPs go into it, for the same reason as the `tools/nat-probe/` measurement originals |

## What Fixing One Test Taught Us

The case reproducing `WAIT_FAILED` **failed with a 30-second timeout.** The wait did not fail; it
waited forever.

The cause is handle value reuse. The event was **closed first** and its value handed to the loop,
and the shutdown event the loop constructor makes received **the same value just freed**. The same
handle went into the wait set twice, which made it a valid wait. It was fixed by changing the order
so the close happens after the loop is made, and that reason is in a comment on the case.

## What Was Left

Places the tests do not hold were hung on `plan.md`.

- There is no unit test for the success path when the list has exactly two entries. The one thing
  holding that place today is the e2e
- No test looks at the six kinds of STUN diagnostic logs. `emit` has no injection point, so a unit
  test cannot see the logs
- The passing-side table of the `--stun` host syntax has no digits or hyphens inside a label

**There is no way to judge "the STUN socket is the same as the tunnel socket" from the logs.** The
`roadmap.md` Phase 2 verification asks for it, and the logs have no socket identity field. That item
says of itself "this is for reference. The judgement seen on the wire is Phase 4's job", so it was
not changed this time.

## On the Audit Itself

**5 rounds of diff review found 1 blocker, and the three full-text audits found 14.** It is the
same code. The difference is the way of looking. Diff review looks at the lines that changed; a
full-text audit looks at whether the lines that did not change fit the lines that did. **Most of
the blockers this time were "a rule we made this time making an old sentence of our own documents
stale."**
