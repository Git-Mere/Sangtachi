# 0022. Counters keep a table per session

- Status: Accepted
- Date: 2026-10-08
- Related: [`../architecture.md`](../architecture.md) section 9, [`../protocol.md`](../protocol.md) section 8, [`../concurrency.md`](../concurrency.md) section 5

## Context

`architecture.md` section 9 said that "all counters of that session" are emitted when a session reaches a terminal state, but the
code had one counter table per process. Building the Phase 4 session state machine exposed this mismatch. A host has several
sessions, so with one table it cannot tell which peer a drop was with.

## Decision

The repository owner chose per-session tables, to check afterwards from the log alone which peer had a bad connection.

**1. Tables come in two scopes.** One process table and one table per session.

**2. Which table a counter rises in is decided by the place where it rises, not by its name.** A place where the session is known
(every receive decision after check 7 of 8.1, and the session's sends, timers, renegotiation, endpoint learning) uses the session
table, and other places use the process table. The same name can exist in both tables.

**3. `counter` lines of a session table carry `peer_id` and `peer_name`.**

**4. When they are emitted.** At a terminal state, that session's table. At process shutdown and on the console command, the process
table and the table of every remaining session.

## Alternatives

**Alternative 1. Keep one table and fix the section 9 wording.**
Rejected. Simplest. The cost is that with several sessions the peer of a drop cannot be told apart.

**Alternative 2. Give each name a single scope.**
Rejected. Lines are easier to read. The cost is that when one name rises both where the session is known and where it is not
(`drop_type_length`, `tx_err_send`), one side would have to invent a session or drop the count.

## Consequences

**Gains.**

- Per-peer drops are visible from the log alone
- No session is invented where none is known

**Costs.**

- Log lines grow by the number of names per session. Sessions with a table are up to `MAX_PEERS - 1` live ones plus up to `MAX_PEERS - 1` kept in a terminal state (`protocol.md` 5.6), so at most twice that
- The whole value is the sum of the two scopes. No total line is emitted
