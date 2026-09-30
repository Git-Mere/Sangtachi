# Phase 2. The STUN Client

## What Was Done

Find the public UDP endpoint of a client behind NAT. This is [`roadmap.md`](../../roadmap.md)
Phase 2. The scope is the STUN usage scope of [`protocol.md`](../../protocol.md) chapter 13,
retries and deadlines are chapter 11 timers, and server selection and startup input are exactly
what [`architecture.md`](../../architecture.md) 3.5 sets.

## How It Was Split

The work was cut into three, two of them run in parallel. **Empty files and the CMake wiring were
made first and the build confirmed**, then each agent got only its own files. If two agents edit
the same file in the same tree they overwrite each other.

| Piece | What |
|-------|------|
| Seams | Name keys, cancellation and one-shot in `TimerSet`, `platform::random_bytes`, `platform::resolve_ipv4` |
| Codec | `network/stun`. Message building and parsing. **Pure functions only** |
| Wiring | Receive classification, the send seam, `network/stun_client`, startup input, logs and counters |

**Taking randomness, timers and sockets out of the codec is the point of this split.** If the
transaction ID comes in as an argument, a test can fix the value. If it is drawn inside, golden
vectors cannot be made.

## What the Design Settled

- **The retries 500ms, 1s, 2s are intervals.** Sends happen at 0, 500, 1500, 3500. Read as
  absolute times the intervals become 500/500/1000, which is not the doubling sequence of RFC 5389
  7.2.1
- **An empty slot is refilled on a response, not only at the deadline.** Refilling only at the
  deadline makes a four-entry list end at 15s when the first server answers at 1s and the last
  hits its deadline at 5s. The cap is 10s
- **A response is matched by transaction ID only.** The source address is not looked at as well
  (`protocol.md` chapter 7)
- **The three ending paths (response, deadline, stage end) pass through the same place.** That is
  what keeps a late response to a finished transaction from being counted as a discard
- **The random source, the clock, the socket and name resolution are all injected.** Tests do not
  actually wait 500ms and do not depend on DNS

## What Was Fixed in the Documents

The documents were put first so the code would not settle things first.

| What | Where |
|------|-------|
| A success response with no address is discarded | `protocol.md` chapter 13 |
| **Two or more `XOR-MAPPED-ADDRESS` means discard.** For `ERROR-CODE` only the first is read | Same section |
| Parse failures are counted as `drop_stun_parse`. The reason is not split per code | Same section |
| **Entries that resolve to the same endpoint count as one** | `architecture.md` 3.5 |
| The own-subnet broadcast judgement of chapter 7 source hygiene is hung on Phase 4 | [`roadmap.md`](../../roadmap.md) Phase 4 |
| The `FAIL` standard output line and process behaviour after a failed attempt are hung on Phase 3 | `roadmap.md` Phase 3 |

**Why duplicate attributes are handled in two ways.** `XOR-MAPPED-ADDRESS` becomes the public
endpoint we announce to the other side, so picking the wrong one sends the whole hole punching
that follows to the wrong place. `ERROR-CODE` is only used in logs. **That is the difference
between a value that enters a judgement and one that does not.**

## Verification

| What | Result |
|------|--------|
| Build | `build ok` (MSVC /W4 /WX, 0 warnings) |
| Tests | `0 checks failed` / `all tests passed`. 101 -> **154** |
| e2e | `0 checks failed`. Checks 28 -> 35 |
| `platformgate` | `VERDICT: pass` |
| `docgate` | 0 findings of kind `link` |

**The e2e does not call public STUN servers.** The script brings up two minimal STUN responders
and checks all the way to whether `mapped` in `stun.result` matches that client's local port.
Before this was done, real Google STUN responses mixed into `rx.raw` and broke 6 of the Phase 1
checks.

**The default STUN list is in three places.** `DEFAULT_STUN_SERVERS` in `natprobe.py` (line 68),
the code block in `architecture.md` 3.5, and `kDefaultStunServers` in `stun_client.hpp` (line 99)
were compared by eye down to the name, port and order of all four entries. **`natprobe.py` did not
change in this commit.** That file is the source and the other two copy it, so there is nothing to
change there.

## Cross-Model Review (Codex)

The code diff was 176KB, so it was cut per lens and run one at a time.

| Lens | Scope | blocker | warn |
|------|------|:---:|:---:|
| Parser hardening | Codec | 0 | 1 |
| State machine | `stun_client` | 0 | 3 |
| Receive classification and wiring | Loop, `main`, arguments | 1 | 0 |
| Seams | Timers, randomness, resolution | 0 | 2 |

### Accepted

| Finding | How |
|---------|-----|
| A duplicate `XOR-MAPPED-ADDRESS` is decided as "the first one" | Accepted. Changed to discard with a new reason (`kDuplicateMappedAddress`). Piling it onto an existing reason would make one log token mean two different conditions |
| A late response to a transaction ended by the deadline is counted as a discard | Accepted. The three ending paths were pulled into one place. **This was a place where the implementer's written rule and the code diverged** |
| `owns_timer` claims the whole `stun.` prefix as its own | Accepted. Changed to a shape match. A prefix match is an exclusion list; a shape match is an allow list |
| The tests use real randomness, so ID uniqueness is a probabilistic claim | Accepted. The random source is injected. The `random_bytes` failure path now has a test |
| One case in `resolve_test` ends at `SUCCEED()` when it succeeds, so it holds nothing | Accepted. It checks the premise instead of assuming it, then asserts |
| Old-form IPv4 leaks through to `getaddrinfo` | Accepted. The numeric-shape judgement was put in front with a case table attached. **See "What Was Not Kept" below, though** |

### Hung on a Point in Time (1 blocker)

**The own-subnet broadcast judgement of chapter 7 source hygiene is missing.** It was not
dismissed. It needs the interface address and the netmask to compute, and the place that reads
those is local candidate collection in `protocol.md` 10.1, which is Phase 4. Blocking it by
guessing while the netmask is unknown is what `CLAUDE.md` rule 7 forbids. It was hung as an item
on `roadmap.md` Phase 4 and **the risk that remains until then** was written in that place. The
code that produces replies (`HELLO_ACK`, `PONG`) appears in the same Phase, so there is no reply to
amplify yet.

## What Was Not Kept

**A mutation of the numeric-shape judgement is not caught by the tests.** The mutant that deletes
that call in `resolve_ipv4` passes, because `getaddrinfo` on this machine rejects notations like
`1.2.3`, `0x7f.1`, `010.1.1.1` by itself, so the return value is the same. **So what holds that
rule is the one case table of the judgement function**, and the assertions on the `resolve_ipv4`
side only pin the behaviour. That fact was written in both the header and the test comments.

**The mechanism the reviewer raised does not reproduce on this machine.** The eight notations above
were queried with `AF_INET`/`SOCK_DGRAM` and all failed to resolve. So "Winsock resolves the old
`inet_addr` form" was not written as an assertion, and the second, checkable mechanism (NXDOMAIN
interception, search suffixes) was left as the reasoning instead.

## What the Implementer Found

This is a finding from the implementer, not from a review. **The first two entries of the default
list resolve to the same IP.** The `tools/nat-probe/` measurement recorded it that way. As list
entries they are "two different servers", but the actual destination is one, and then the judgement
of the supported network conditions in `spec.md` does not hold, while **by entry count it looks
like two.** A rule that filters duplicates by endpoint was written into `architecture.md` 3.5 and
implemented. With 4 list entries and 3 actual destinations the stage cap stays at
`5s x ceil(3/2)` = 10 seconds.

## What This Round Confirmed

- **There are two reasons a mutant survives.** A weak test, and a mutant sitting where nothing
  reaches it. The implementer distinguished these by itself after a mutant placed in dead code
  passed. Without that distinction you reach the wrong conclusion, "it passed, so the test is weak"
- **Under MSVC debug, mutation testing hangs on a modal dialog.** A mutant deleting a bounds check
  raised a `span subscript out of range` dialog and the harness stopped. The user saw the screen and
  told us. `tests/crt_report_guard.cpp` now routes assertions, `abort()` and error reporting to
  standard error. **A crashing mutant is now caught as a failure instead of hanging**
- **The parent added that guard file and told the subagent to rebuild without having run a build.**
  The whole-repo build stopped at that file and the subagent pointed at it exactly and sent it back
