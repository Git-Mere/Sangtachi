# 0010. Fix the Platform Porting Seams Now

- Status: accepted
- Date: 2026-09-29
- Related: [`../spec.md`](../spec.md) Excluded, Stretch goals, C-1, [`../architecture.md`](../architecture.md) 3.1, chapter 10, chapter 12, [`../concurrency.md`](../concurrency.md) chapter 2, chapter 4, [`../protocol.md`](../protocol.md) chapter 6, [`../roadmap.md`](../roadmap.md) Stretch Goals

## Context

Four living documents speak about platforms. All four say "we do not do it".

| Document | What it writes |
|----------|----------------|
| `spec.md` Excluded | Mobile support, macOS support |
| `spec.md` Stretch goals | Linux interoperability |
| `architecture.md` chapter 12 | "macOS / mobile: not supported. Linux is reviewed for interoperability only if time is left" |
| `roadmap.md` Stretch Goals, `spec.md` priorities | P4 = encryption + relay + additional platforms |

**"We might do it some day" is written down, and "what has to be rewritten then" is nowhere.**
None of the nine ADRs covers porting, and the client module table of `architecture.md` 3.1 does not
hold the seams by name. In some places the module name itself is a vendor (`adapter/wintun_adapter`).

Counting the code shows the seams are small. In the client **before** this decision was written,
five sources include an OS header (`console.cpp`, `hash.cpp`, `loop.cpp`, `network/udp_socket.cpp`,
`network/wsa.cpp`), and the public headers (`client/include/`) had no OS header at all. That is not
an accident; Phase 1 was written that way. `udp_socket.hpp` hands out the socket as
`std::uintptr_t` and the event as `void*`, and `wsa.hpp` keeps a `VersionWord` alias instead of
`WORD`.

**The reason for doing it now is Phase 6.** Today the seams are the socket and the loop only. Once
Wintun and IP Helper come in they spread to the adapter, the routing, and the privileges, and by
then the same work is several times larger.

## Decision

**We do not port. We only fix the seams by name.** Five items are part of this decision.

**1. There are five seams.** These are the places that get rewritten when porting, and nothing
outside them gets rewritten.

| # | Seam | Source of the contract | Code location at the time of this decision |
|---|------|------------------------|----------------|
| 1 | Socket | `protocol.md` chapter 6 socket ownership | `platform/win32/wsa.cpp`, `platform/win32/udp_socket.cpp` |
| 2 | Wait and monotonic clock | `concurrency.md` chapter 2 wait, chapter 4 timers | `platform/win32/wait.cpp` |
| 3 | Virtual adapter and routes | `concurrency.md` chapter 3, `architecture.md` chapter 7 | Does not exist yet. Phase 6 |
| 4 | Hash | `architecture.md` chapter 9 (`sha256` of `rx.raw`) | `platform/win32/hash.cpp` |
| 5 | Startup and privileges | `windows-prereq.md` | Not code but an execution premise |

The right-hand column is the location at the time this decision was made. **The source of the
current layout is the target structure of `architecture.md` chapter 10, not this table.** When an
adapter source appears, that document is the one that gets fixed.

> **Why five.** The first count was four: socket, wait, adapter, and startup. Reading the code added
> the hash. `sha256_hex` calls `BCryptHash`. Moving it needs another implementation, and if that is
> third party it becomes subject to `spec.md` NFR-5 approval. Without counting it, that place would
> have been missing from the porting estimate.

**2. There is one rule. A client source that includes an OS header lives only under
`client/src/platform/`.** No OS header exists under `client/include/`. New platform code is made in
`client/src/platform/win32/`.

> **Why "include" is the criterion.** It is the only axis a machine can count. The judgement "this
> leans on Windows" comes out differently from each person every time, and then the rule shakes from
> round to round.

**3. That does not make the meaning of the headers neutral too.** `udp_socket.hpp` hands out the
integer error of `WSAGetLastError` and the Win32 event handle as they are, and
`RecvStatus::WouldBlock` points at `WSAEWOULDBLOCK`. `kInfiniteTimeout` of `timer.hpp` is the same
value as `INFINITE`. **This ADR does not change that.** Making the names neutral alone would look
ported while it is not. The neutral types for error codes and handles are decided when a second
implementation is written.

**4. The wait primitive of `concurrency.md` chapter 2 gets rewritten when porting.** The reason
`WaitForMultipleObjects` was chosen was that "the Wintun read wait is a Win32 event handle, not a
socket", and `epoll` and `kqueue` do not have that problem at all. Since the ground for the choice
exists only on that OS, the conclusion exists only on that OS too. `concurrency.md` writes that fact
in one line.

**5. Rule 2 is not counted by a person.** `tools/platformgate/` judges it. The last line has to be
`VERDICT: pass`. The six judgement rules that tool fixed are part of this decision.

| # | Rule | Why |
|---|------|-----|
| a | The preprocessor is not evaluated. An `#include` inside `#if 0` or `#ifdef _WIN32` is counted too | What the gate keeps is not the compilation result but the layout of the sources. A gate that passes once you wrap it in `#if 0` is the same as telling people how to get around it |
| b | An `#include` inside a comment or a string is not counted | Those are letters the compiler does not see |
| c | The allowed place is `client/src/platform/` alone, and it includes subdirectories | It keeps the rule to one line. Under `client/include/` there is no exception even for a directory named `platform` |
| d | Path comparison is case sensitive | The repository uses one spelling (`src/platform`) |
| e | The OS header judgement is a **name list**. A header not on the list is not judged and passes | An unknown header is not declared to be an OS header. It is not an allow list, so **once a new OS header starts being used it has to go into `OS_HEADERS` first.** That duty is owned by `roadmap.md` Phase 6 |
| f | The checked extensions are `.cpp` and `.hpp` only | The repository uses those two only. **If a source named `.h` or `.c` is brought in the gate does not see it, so `SOURCE_SUFFIXES` has to be fixed first.** It is the same kind of duty as e and is owned by the same place (`roadmap.md` Phase 6) |

**The scope of the gate is the `.cpp` and `.hpp` of `client/`.** `tests/` is not judged. A test's
job is to reproduce platform behaviour, so calling an OS API directly is normal there. **The price
is that the test code gets rewritten when porting too**, and this ADR did not put that into the seam
list. What else it cannot judge is held as a table by `tools/platformgate/README.md`.

### What This ADR Does Not Decide

- **Which OS is supported and when.** The Excluded and the Stretch goals of `spec.md` stay as they
  are. This decision does not widen the scope
- The neutral type design for error codes and handles (decision 3)
- What replaces `BCryptHash`. If it is third party, `spec.md` NFR-5 approval comes first
- The choice of the counterpart to Wintun. Seam 3 is looked at again once the code exists in Phase 6
- Qt was not put into the seams. The reason is that the toolkit already runs on several operating
  systems, and that the packaging of the deliverable differs per OS is owned by seam 5

## Alternatives

**Alternative 1. Do nothing.**
Dropped. The cost of keeping this state is 0 now and rises after Phase 6. Once the adapter comes in
the seams spread to the routing and the privileges, so doing the same work then means more places to
touch.

**Alternative 2. Write it in the documents only.**
Dropped. This is the plan of writing the seam list into `architecture.md` chapter 12 and stopping
there. What it gains is that no code changes, and what it pays is that there is no device that keeps
the rule. The place where `CLAUDE.md` decided that "counting is done by a script" is exactly this
kind of rule. It would tell us nothing even if the next Phase put `<windows.h>` into a new file.

**Alternative 3. A full platform abstraction layer.**
Dropped. This is the plan of wrapping socket, wait, adapter, and hash with abstract interfaces and
virtual functions. What it gains is a place to plug a second implementation into, and it pays two
things. First, an interface with only one implementation copies the shape of that first
implementation as it is. An interface that presumes `WSAEventSelect` gets dropped again on `epoll`.
Second, it delays P0. **Making the interface at porting time instead is the price of dropping this
alternative.**

**Alternative 4. Port now.**
Dropped. `spec.md` wrote macOS as excluded and Linux as a stretch goal, and this session does not
overturn that. The minimum success M-1 ~ M-6 comes out on one Windows machine.

## Consequences

**What we gain.**

- The porting estimate becomes a list. The files that get rewritten are in one directory and the
  source of their contracts is in a table
- The place the Wintun code of Phase 6 goes to is already fixed. There is one judgement less to make
- That the public headers hold no OS header is a property kept by accident today, and the gate turns
  it from a property into a rule
- What the gate confirms by machine is **one thing only: that an OS header on the list is not
  included outside `client/src/platform/`**. That is not a proof that "nothing needs rewriting". The
  coupling that remains was written by decision 3

**What we pay.**

- **Files moved, so the history looks cut.** `git log --follow` is needed
- **Code that follows a protocol contract lives inside `platform/win32/` too.** The socket options
  are that. Where the code sits does not move the source of the contract. That source is
  `protocol.md` chapter 6
- **The gate looks at "include" only.** A neutral header whose meaning is Win32 passes. Decision 3
  left it there knowingly; it is not something the gate misses
- There is one more rule to keep. Putting an OS header into a new source needs its place decided
  first
