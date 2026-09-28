# 0006. Make Multi-Peer a No-Relay Star

- Status: accepted
- Date: 2026-09-28
- Related: [`../spec.md`](../spec.md) FR-4, FR-10, FR-12, NFR-2, [`../architecture.md`](../architecture.md) chapter 7, chapter 12, [`../protocol.md`](../protocol.md) chapter 1, 8.5, [`../control_plane.md`](../control_plane.md) 2.5, 4.4

## Context

Until now a room was 2 peers. `protocol.md` chapter 1 writes "2 peers per room" and the 2.6
constants of `control_plane.md` pin `MAX_PEERS = 2`. `architecture.md` chapter 12 wrote it as an
item outside the initial scope.

> - **Mesh of 3 or more**: initially 2 peers. The routing table structure is left extensible but not implemented.

The repository owner decided to raise the maximum head count of one room to 5. That decision
reverses the line above.

We investigated the blast radius first. The 2-peer premise is not one constant; it is spread over
the whole document set. Documents write "the remote" in the singular, write one session, and write
decision rules as a "both sides" dichotomy. The representative one is check 6 of `protocol.md` 8.5
Transmit-Side Validation.

> | 6 | Destination IP == the peer's virtual IP | `tx_drop_no_route` |

`concurrency.md` chapter 7 pushed that premise down into the shutdown path. It writes that "in the
minimum scope there is one session, so once that session ends" the process ends.

So **the topology had to be decided before raising the head count.** How many sessions come into
being decides which way the descriptions above are rewritten.

The full list of what the investigation found is held by this commit's record.

## Decision

**Raise the maximum head count to 5 and make the topology a no-relay star.** Six items are part of
this decision.

**1. A tunnel is formed only as a pair of the host and one player.** Players do not form tunnels
with each other. With 5 people in a room there are 4 tunnels. Hole punching is 4 pairs as well.

**2. The host does not relay.** A packet player A sends to player B's virtual IP has no route.
Check 6 of `protocol.md` 8.5 Transmit-Side Validation drops it as `tx_drop_no_route`. The host does
not enter the data path of someone else's traffic.

The basis is the structure of the target application. Minecraft Java multiplayer is client-server,
and `spec.md` FR-12 writes that as a requirement. A player connects to `10.100.0.1:25565`.
**There is no game traffic between players to begin with.**

**3. The session count is asymmetric.** The host keeps N-1 sessions and a player keeps 1.
Multi-session complexity appears on the host side only. On a player's transmit path the remote is
the host alone, so check 6 of 8.5 stays correct as the singular comparison it is.

**On the host side it does not end with turning that comparison into a set comparison.** Row 1 of
the 8.5 table is "State is `CONNECTED`", and when there are several sessions, which session that
state belongs to is not fixed until a session is picked by destination. The check order changes.
This ADR does not decide that order.

**4. The host role is fixed and there is no migration.** The room creator is the host and its
virtual IP is `10.100.0.1`. `architecture.md` chapter 7 already wrote it that way and this decision
does not change it. When the host's sessions end, every tunnel of that room ends.

**5. The virtual IP pool runs from `10.100.0.1` to `10.100.0.5`.** The host takes `.1`, and the
participant pool is the four addresses from `.2` to `.5`. The range and the sequential assignment
rule are owned by `architecture.md` chapter 7 and `control_plane.md` 2.5.

**6. The role does not split the code path of the data plane.** The host and a player run the same
code. The difference shows up only as the data called **the size of the session list**. For the
host that list has N-1 entries and for a player it has 1.

The role itself remains. The three below still split by role.

- Control plane calls. The host calls `create_room`, a player calls `join_room` (`control_plane.md` 4.2, 4.3)
- Virtual IP. The host is fixed at `10.100.0.1`, a player is assigned one from the pool (`architecture.md` chapter 7)
- What a person sees. Creating a room and entering a room code

> **Why.** Splitting the data plane by role makes two code paths and doubles the tests. And a place
> where a branch is missing does not crash; it produces a wrong value. Generalizing to the session
> list lets the rule be written in one sentence, and a player becomes the case where the list has
> 1 entry.

### What This ADR Does Not Decide

It decides the topology only. The items below come with this decision but have to be decided
separately. The place where they are decided is the living documents, and the timing is owned by
`roadmap.md`.

- Whether ready is seen on a room capacity basis or on a pair basis (`control_plane.md` 4.4)
- Whether a room that has become `ready` allows a late join (`control_plane.md` 5.1)
- The relation between session end and process end. Decision 6 set the direction. Generalized to
  ending when the session list is empty, a player has a list of 1 entry, so the result is the same
  as now. The final sentence is owned by `concurrency.md` chapter 7
- Whether to provide a means for a player to know that other players exist. Providing one needs a
  new tunnel message. An element of `peers` in the `get_peers` response has only the three fields
  `peer_id`, `virtual_ip`, and `candidates`, so it does not carry whether a peer is connected
  (`control_plane.md` 4.5). Tunnel message types are `0x01` to `0x07` and values outside that list
  are dropped (`protocol.md` chapter 5)
- Whether the virtual IP slot of a peer that left is reclaimed while the room is alive
  (`control_plane.md` 2.5). **Until this is decided, the current rule stands.** That section says
  "a slot stays with that peer even after the peer leaves", so when one player drops, the address
  is not freed and the room stays at full capacity. **No new join can take the vacated slot; only
  that peer can re-join** (`control_plane.md` 4.3)
- The order of the transmit-side validation pipeline. Decisions 3 and 6 set the direction. The
  order picks a session by destination and then looks at that session's state, and the side whose
  session list has 1 entry runs the same order. The final table is owned by `protocol.md` 8.5
- When the host shuts down, sending one `CLOSE` per session is at most 4. Whether that fits inside
  the 3-second OS grace of `CTRL_CLOSE_EVENT` (`concurrency.md` chapter 7)
- Whether to judge the risk of the host's NAT before room creation and warn the person.
  "Consequences" below writes that risk

## Alternatives

**Alternative 1. Full mesh.**
Dropped. With 5 people every peer keeps 4 sessions and the punch pairs become 10. Multi-session
complexity appears for everyone. What it gains is direct communication between players, and the
target application does not use it.

**Dropping it loses one thing.** In a full mesh the remaining pairs still connect even when some
pairs do not punch through. In a no-relay star the host-side paths are everything, so there is no
such state as a partial connection. It is written under "Consequences" below.

**Alternative 2. Relaying star.**
Dropped. In this option the host receives packets between players and forwards them. What it gains
is communication between players, and what it pays is that the host becomes a router. The transmit
path of `protocol.md` 8.5 and the receive validation of 8.1 would have to handle "what came to me"
and "what goes to someone else" separately, and forwarding loop prevention and a forwarding volume
cap become newly necessary. It redesigns the data plane for a feature Minecraft does not need.

**This is a different thing from the relay fallback of the `spec.md` stretch goals.** That one is an
alternative path for a peer whose direct connection failed, and this one carries someone else's
traffic even in a state where the direct connection succeeded.

**Alternative 3. Keep it at 2 people.**
Dropped. It is the repository owner's decision. The strength of this alternative was that it
changes none of the current documents.

## Consequences

**What we gain.**

- Multi-session complexity appears on the host side only. A player's code path keeps nearly the
  current design as it is
- Punch pairs grow linearly with the head count. It is not the square of a full mesh. At 5 people
  it is 4 pairs against 10
- A player who starts late punches with the host only. There is no effect on the sessions of
  existing players
- The host holds every session directly, so it **already knows who is attached.** Even without
  putting a new device into the control plane to announce room membership, the host side has that
  information
- Thanks to decision 6 we write only one set of rules. Shutdown, transmit check order, counter
  unit, and record unit are not written separately for the host and for a player

**What we pay.**

- **The host is a single point of failure.** If the host's NAT does not allow hole punching, the
  whole room fails. That holds even in the case where, in a full mesh, some pairs would have
  connected
- **If the host drops, the room ends.** There is no migration (decision 4)
- **Players do not see each other even by virtual IP.** The "virtual network" description of
  `spec.md` FR-4 and `architecture.md` chapter 7 has to write this constraint. Windows routing
  attaches a `10.100.0.0/24` route to the adapter (`architecture.md` chapter 7), so the operating
  system puts a packet headed for `.3` into the adapter. What drops it is our client
- **NFR-2 holds.** What goes into the tunnel layer is not Minecraft-specific logic; the topology is
  narrowed instead. But the scope of the sentence "replacing Minecraft with another IP-based
  application" narrows to client-server applications. We write that narrowing into the documents
- Traffic concentrates on the host's uplink. But as long as the game server runs on the host, that
  concentration is not created by this decision

**What we could not check.**

- **The size of the price of decision 6.** Existing descriptions and tests that assume one session
  go stale. How many of them there are was not counted at the time this ADR was written. Depending
  on how far Phase 1 has gone, it may be "the cost of writing it differently from the start" rather
  than "the cost of fixing it"
- **The actual success rate per host NAT type.** The 22 measured runs of `tools/nat-probe/` are on
  a 2-party basis. The behavior when one host punches with 4 remotes at the same time was not
  measured. The measurement timing is owned by `roadmap.md`
- The contention behavior of the control plane when 5 people join at the same time. The current
  case table is on a 2-party basis (`control_plane.md` 4.4)
