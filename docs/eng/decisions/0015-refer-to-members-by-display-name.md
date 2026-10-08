# 0015. Refer to Members by a Display Name a Person Chose

- Status: accepted
- Date: 2026-10-07
- Related: [ADR 0006](0006-star-topology-no-relay.md), [ADR 0011](0011-lobby-is-room-membership.md), [`../spec.md`](../spec.md) FR-15, FR-16, T-7, [`../control_plane.md`](../control_plane.md) 2.7, 4.1, 4.2, 4.3, 4.5, 4.6, 6.3, 6.4, 6.5, 7.6, [`../protocol.md`](../protocol.md) chapter 3, 5.7, [`../architecture.md`](../architecture.md) 3.5, chapter 8, [`../concurrency.md`](../concurrency.md) chapter 7, [`../roadmap.md`](../roadmap.md) Phase 4

## Context

`concurrency.md` chapter 7 Lobby decided that when one pair on the host side fails, the host emits
a `FAIL` line and stays in the room. Whether that line says which player it belongs to was not
decided, and `roadmap.md` Phase 4 left it as a decision to make before starting. A host has up to
four pairs, so the line alone does not tell whose connection failed.

There were only two values that could point to a member.

- `peer_id` is a 32-bit random number the server draws fresh for each join. It means nothing to a
  person
- `virtual_ip` is fixed while the peer stays, but it can change when the peer comes in again, and a
  new participant gets the address of someone who left (`control_plane.md` 4.3)

Neither is a value by which a person recognizes "who it is". The same problem exists in the GUI
member list (FR-15) and in the T-7 verdict.

## Decision

The repository owner decided the following.

**1. Every peer that enters a room chooses a display name.** The host chooses one too. It is a
required field of the `create_room` and `join_room` requests. The client receives it as a startup
argument and can change it in the lobby.

**2. The format is ASCII letters, digits, and underscores, at least 1 and at most 16 characters.**
Case is shown as registered and ignored when comparing for equality. Hangul is not accepted.

**3. A name is unique within the room.** The server guarantees it with a conditional write of the
`NAME#` item, and rejects a collision with the new error code `name_taken`.

**4. Names go from the server to the host, and from the host to the players.** The host and the
players receive the other side's name in the `host_report` and `get_peers` responses. Players
receive each other's names through `ROSTER`. The member layout of `ROSTER` changes from a fixed 9
bytes to a variable length.

**5. The name is for display only.** Receive validation, routing, and the matching of local records
do not look at the name. The identifier machines use is still `peer_id`.

**6. The `FAIL` line always has a counterpart field.** It is `FAIL <code> <counterpart> <sentence>`.
For a failure that came from a session it is the name of that session's counterpart; otherwise it
is `-`. This closes one of the Phase 4 decisions to make before starting.

## Alternatives

**Alternative 1. No names; put `peer_id` or `virtual_ip` on the `FAIL` line.**
Dropped. It is cheap because only the line grammar changes. The price is that a person who sees
that value does not know who it is, and that is the starting point of this decision.

**Alternative 2. Keep the `FAIL` line grammar and add the field only for pair failures.**
Dropped. The reader would have to guess whether the third word is a name or the start of the
sentence.

**Alternative 3. Put the name inside the sentence.**
Dropped. The table in `architecture.md` chapter 8 owns one sentence per code, and the GUI uses the
same sentences. The sentence would become a template and hard for machines to read.

**Alternative 4. Accept Hangul names.**
Dropped. It is better for people. The price is that the distinction between byte length and
character count, Unicode normalization, rejection rules for invisible characters and direction
control characters, and checks of the console and log output paths each attach to the server, the
client, and roster reception. Letters, digits, and underscores have no spaces, so they do not break
the `FAIL` line or the log `<name>=<value>` rule.

**Alternative 5. Accept duplicate names.**
Dropped. One item fewer, and the reclaim path gets simpler. The price is that a person cannot tell
two identical names apart, and then the purpose of having names disappears.

**Alternative 6. The host decides uniqueness.**
Dropped. In the star topology the host is indeed the source of the roster. But a join completes on
the server, so for the host to reject it, a new path to push out a peer that is already in would be
needed.

**Alternative 7. Carry the name in `HELLO`.**
Dropped. The length and validation of the handshake packet would change. The name is not needed to
establish a connection.

**Alternative 8. Have a default name and do not make it required.**
Dropped. If everyone uses the same default, decision 3 makes every join from the second one fail.

## Consequences

**What we gain.**

- The host's `FAIL` line and the member list show names a person recognizes
- The `FAIL` line grammar is the same on the player and on the host

**What we pay.**

- The finished Phase 3 code is fixed again. The control server's request checks, storage, and
  responses, and the client's startup arguments, lobby, response checks, and `FAIL` line
- The cap on stored items per room grows from 15 to 20, so the worst-case capacity grows
  (`control_plane.md` 7.6)
- `ROSTER` becomes variable length, so it gains one receive check and one drop counter
- The name is not authenticated. In v1 anyone on the path can build a `ROSTER`, so the names on
  the screen can be forged too. Decision 5 keeps that forgery from reaching validation and routing
