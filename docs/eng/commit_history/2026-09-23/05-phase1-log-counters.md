# Phase 1 Logs and Counters

**Target commit:** this commit
**Phase:** 1 (core UDP networking)

The contract set by `architecture.md` chapter 9 log output was moved into code. Every later Phase
verification item reads these lines, so the format must stand first.

## The Chapter 9 Rule Conflicted With Itself

**The quoting the previous commit added broke chapter 9.** The chapter 9 "unit" contract says "do
not use escape notation", but the previous commit's `main.cpp` wrapped values like `arg="a b"` and
escaped `"` and `\`. At that time `plan.md` said "that rule is not in the document", and **that was
a wrong diagnosis.** The rule was there and I broke it.

**But the original rule did not hold either.** Chapter 9 said "carry line breaks and control
characters as a single space". The line shape is `<level> <event key> <name>=<value> ...`, so **if a
value contains a space, the boundary of `name=value` disappears.** The rule that turns control
characters into spaces creates that problem itself. It was a rule from the time when values could
not hold a space, and it broke when a field that carries a user-supplied string appeared.

| Option | What | Adopted |
|----|------|:----:|
| A | Turn spaces and control characters into **a single underscore** | **Yes** |
| B | Wrap the value in quotes and escape quotes | No |
| C | Follow the rule, turn them into spaces, and leave the boundary problem | No |

B was dropped because chapter 9 already forbids it. Carrying a value with a quote needs escaping,
and then the reading side must interpret the notation. With C the line that verification reads
becomes ambiguous.

The price of A is that **the substitution cannot be reversed**. That fact was written into chapter 9
together with "do not make a verdict that needs the original text from the log". The input of a
verdict is the local record file.

## What Was Added

| Target | What |
|------|------|
| `log.hpp` / `log.cpp` | `INFO`/`WARN`/`ERROR`, `sanitize_value`, `format_line`, `emit` |
| `counters.hpp` / `counters.cpp` | 39 counters, `Counters`, `format_all`, `emit_all` |
| `main.cpp` | Drops the temporary `quote_for_log` and uses the log module. Prints all counters before exit |

**`format_line` was left as a pure function.** A test can decide the line shape without capturing
standard error or touching global state. `emit` adds the line break on top of it and writes with a
single `fwrite`. If it were split in two, a line from another thread could land in between.

**The 39 counter names were collected from three documents and not coined here.** It also holds
names that nobody raises yet. Chapter 9 set "print counters whose value is 0 as well", and
verification can decide "it did not rise" only when a missing counter and a zero counter are
distinguished.

**The `[loop]` sole ownership rule was carried over as is** (`architecture.md` 3.2.5). The
exception `console_queue_dropped` is an atomic variable that `[console]` raises, so the queue side
holds it, and `[loop]` reflects it into this table with `set` when it prints. So `Counters` itself
has no atomic variable.

**Overflow saturates instead of wrapping.** If it wrapped, verification that looks for "it did not
rise" would pass.

## Verification

| What | Count |
|------|:---:|
| Catch2 cases | 25 -> **40** |
| CLI cases | 18 -> **19** (1 case for printing all counters) |
| `build.ps1 -SelfTest` | 7 |

13 mutants were run and **all died.**

| Target | Mutants | Killed |
|------|:---:|:---:|
| `log.cpp` | 8 | 8 |
| `counters.cpp` | 5 | 5 |

The 8 on the log side shift the boundaries of the sanitizing one by one (space only, control
characters only, `DEL` only, up to high bytes) and break the line shape (the separating space, `=`,
the level token). The 5 on the counter side are saturation, ignoring the increment, disabling `set`,
dropping a zero counter, and a duplicate name.

## Cross-Model Review

Codex was run with two lenses (correctness, contract). Both findings were taken.

| Grade | Finding | Handling |
|------|------|------|
| warn | `sanitize_value` leaves bytes of `0x80` and above as they are, so a Unicode line separator like U+2028 stays in a value. On a reading side that knows Unicode, "do not carry spaces and control characters" breaks | **Taken.** Decoding is not done. Instead, **the fact that the contract is byte based and that limit were written into chapter 9** and pinned with a case. A verdict splits on `0x0A` |
| warn | The review section of this record is still a placeholder | **Taken.** This table is that place |

**Why Unicode decoding was not added.** The log is a diagnostic that people read, and the input of a
verdict is the local record file (chapter 9). Adding a decoder makes that decoder a test target, and
what it buys is only the display in a tool this project does not use. Instead, what is guaranteed
and what is not was written down.

## What Is Left

Phase 1 still has the UDP socket wrapper and the `[loop]` event loop. The lack of a document-to-code
check of the counter names was written into the `plan.md` document debt.
