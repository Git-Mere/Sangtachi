# 0023. Decisions Before Round 3 H and I

- Status: Accepted
- Date: 2026-10-12
- Related: [`../roadmap.md`](../roadmap.md) Phase 4, [`../protocol.md`](../protocol.md) 5.7, Chapter 11, [`../concurrency.md`](../concurrency.md) Chapter 3, [`../architecture.md`](../architecture.md) Chapter 9

## Context

Before starting H (routing table, `DATA` send, the `send` command, the echo responder) and I (`ROSTER` sending and its timing) of
Phase 4 round 3, there were seven points the documents did not define. The repository owner picked one of the options for each. For
the M-5 comparison and the roster-change judgment only one recommendation was offered and the owner accepted it. The alternatives for
those two were written while writing this record.

## Decision

| Point | Choice |
|-------|--------|
| Keeping the payload hook out of the product build | The hook slot stays in core and the product build leaves it empty. Test-only code (the sender's `data.tx`, and `data.rx` and the echo responder attached to the slot) is compiled only under the test-build CMake option |
| Destination of `send` | `send <n> [size] [to]`. `to` is the destination virtual IP. It may be omitted only when there is exactly one `CONNECTED` session |
| How to run the M-5 comparison across two processes | The test-build log events `data.tx` and `data.rx` carry `len` and `sha256` at the comparison points, and a script matches them |
| First value of `ROSTER` `generation` | 0 when the room is set up. One per host |
| Overlapping immediate sends on entering `CONNECTED` | One send. The entry send of the Chapter 11 period and the immediate send of a 5.7 roster change are the same send |
| What counts as a roster change | The content to carry (members, names, bit 0) differs from the previous roster |
| Where a player shows a received roster | One `roster` log line per accepted roster |

I is built up to taking the member list as input. Connecting that input to the `host_report` response is round 4 J.

## Alternatives

**Hook.** Rejected keeping it safe only by `main.cpp` leaving it empty, as now. Compilation would not stop an accidental hook. Also
rejected splitting the slot itself by test build. The slot leads to the Wintun injection path in Phase 6.

**`send` destination.** Rejected sending to every `CONNECTED` session and sending to the first session. Sending to all mixes other
pairs' sends into a test that looks at one pair, and "the first session" is not visible in the command.

**M-5 comparison.** Rejected putting the whole byte string in the log as hex. A thousand 1452-byte packets would make the log several
MB. The hash is computed over the whole byte string at the comparison point, so the matching judgment is just as strong.

**First `generation` value.** Rejected starting at 1. The receiver accepts any first value, so there is no behavioral difference; the
choice matching 5.7's "the receiver accepts from 0" was picked.

**Overlapping sends.** Rejected sending twice. There is no reason to send the same content twice at the same moment.

**Roster display.** Rejected adding a console command `members` and doing nothing until the Phase 8 GUI. The round 4 K verification
script needs a way to see that the roster arrived, and one log line is the smallest.

## Consequences

**Gains.**

- Test code does not get into the product binary
- Even on the host, the command shows which peer `send` goes to
- Round-trip matching across two processes can be judged from logs alone

**Costs.**

- The `send` contract changed from `send <n> [size]`. One more argument
- Three more fixed log events. Two of them exist only in the test build
- In a session that renegotiates often, `generation` rises and an immediate send follows every time bit 0 changes
