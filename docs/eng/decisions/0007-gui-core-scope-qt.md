# 0007. Raise the Minimum GUI into the Core Scope and Use Qt

- Status: accepted
- Date: 2026-09-28
- Related: [`../spec.md`](../spec.md) Scope, FR-15, NFR-5, C-5, [`../roadmap.md`](../roadmap.md) Phase 8, [`../architecture.md`](../architecture.md) 3.1, chapter 11, chapter 12

## Context

Until now the GUI was outside the scope. `architecture.md` chapter 12 wrote it as "**GUI**: console
only.", and `spec.md` put "Advanced GUI" in the exclusions and "Simple GUI" in the stretch goals.
The priority tier was P4, and C-5 sets that when scope is exceeded the removal starts from the
lowest tier, so it was **the first place to be cut.**

The repository owner decided to include a GUI in the final product form. The owner named three
behaviors. A person joins by entering a room code, joining assigns a virtual IP, and **when one
user's connection drops, the other users' screens also show that user as having left.**

The first two already have contracts. They are `create_room` in `control_plane.md` 4.2 and
`join_room` in 4.3, and the 2.5 virtual IP pool. **The design has no device that makes the third
one.** A peer state is only `joined` or `registered` and neither of the two is whether the peer is
connected (`control_plane.md` 5.3). This ADR does not decide that device. It decides the scope, the
tier, and the toolkit only.

## Decision

**Raise the minimum GUI into the core scope.** Six items are part of this decision.

**1. Take "Simple GUI" out of the stretch goals.** The **"Advanced GUI" in the exclusion list of
`spec.md` stays as it is.** What is raised is the minimum GUI alone.

**2. The scope of the minimum GUI is five behaviors.** `spec.md` FR-15 holds that list. Creating a
room and showing the room code, entering a room code and joining, the member list with each
member's connection state, showing the per-stage code on failure, and quitting by closing the
window. A settings screen, a log viewer, themes, an install wizard, and a statistics screen are
outside the scope.

> **Why.** Without the scope written down, "core scope" has no boundary. `spec.md` is the document
> that writes the judgement criteria, so what comes in and what does not is kept as a list.

**3. The tier is P2.** P2 becomes "Wintun + virtual IP + Minecraft + minimum GUI" and P4 becomes
"encryption + relay + additional platforms". **The price is written with it.** FR-15 and T-7 enter
the list of what disappears when P2 is dropped, so once the scope is exceeded the GUI is cut whole
together with the Minecraft validation. There is no middle step.

**4. The implementation is absorbed into Phase 8.** No new Phase is made. Phase 8 is the
demonstration and doing the demonstration with the GUI is the natural fit. Putting a new Phase in
pushes Phase 9 back and forces a fix of the Phase references across the whole document set.

**5. It does not go into the minimum success criteria M-1 ~ M-6.** Instead T-7 is newly placed in
the target success.

> **Why.** M is the proof that the direct connection stands and it can be judged from the command
> line. Putting the GUI into M makes a window that does not open a failure of the minimum success
> by itself.

**6. The toolkit is Qt.** It is a product dependency under `spec.md` NFR-5 and **the instructor
approved it.** That message came from the repository owner and the basis is this commit's record.
The approved list of NFR-5 goes from three to four.

### What This ADR Does Not Decide

- **Which thread the GUI runs on.** Qt has its own event loop and the threads of `concurrency.md`
  chapter 1 set `[loop]` as the process main thread. One of the two has to yield. The time it is
  decided is the pre-start item of `roadmap.md` Phase 8
- **What carries a member's connection state.** It is the third behavior written in the context
  above. The `get_peers` response does not carry whether a peer is connected
  ([`../control_plane.md`](../control_plane.md) 4.5), and tunnel message types are `0x01` to `0x07`
  ([`../protocol.md`](../protocol.md) chapter 5). It is **one question** with the same open item of
  [ADR 0006](0006-star-topology-no-relay.md)
- **The startup input path the GUI uses.** Today the input is command line arguments alone, and the
  room code and the rejoin proof are on standard output (`architecture.md` 3.5). Where the values
  taken in the window are put is decided in Phase 8
- **The scope of the Qt approval.** Which version it is, and whether the linking method and the
  license conditions are part of the approval, was not passed on

## Alternatives

**Alternative 1. Win32 API directly.**
Dropped. It is the repository owner's decision. It had the strength that it needs no NFR-5 approval
and that the build configuration does not change, since Win32 is already used through Winsock2 and
Wintun. The reason it was dropped is the volume of handling list controls and dialogs directly.

**Alternative 2. Web view + HTML.**
Dropped. Making the screens is easy, but the runtime dependency and the install preconditions grow,
and that too is subject to NFR-5 approval. The approval cost is the same as Qt's while one more
deployment precondition is attached.

**Alternative 3. Leave it as a stretch goal.**
Dropped. It is the repository owner's decision. The strength of this alternative was that it
changes not one document, and the price is that under C-5 the GUI disappears the moment the scope
is exceeded.

## Consequences

**What we gain.**

- A user creates a room and joins with no command line arguments. T-7 judges that
- The scope is fixed as a list. A request to "make the GUI better" is pushed outside FR-15
- The demonstration ends in one place. Phase 8 shows the Minecraft session and the GUI at the same
  place

**What we pay.**

- **One product dependency is added.** The Qt runtime is attached to the deliverable. One more
  runtime precondition may be added to `windows-prereq.md`. Its size is under "What we could not
  check" below
- **The volume of P2 grows.** Wintun, the virtual IP, the Minecraft validation, and the GUI sit in
  the same tier together. Once the scope is exceeded the four are cut together
- **The thread model stays open.** Until that decision, `concurrency.md` does not cover the GUI
- **There is still no device that carries a member's connection state.** The third behavior of
  FR-15 and the last condition of T-7 hang on that device. It is the same question as the open item
  of ADR 0006, and **without deciding it T-7 cannot be judged**

**What we could not check.**

- The scope of the Qt approval. It is written under "What This ADR Does Not Decide" above
- The deployment size and procedure of the Qt runtime. What gets added to `windows-prereq.md` is
  checked in Phase 8
- Whether the Qt event loop collides with the single ownership rule of `concurrency.md`. Until it
  is actually used, it cannot be checked
