# 0026. Decisions Before Phase 6

- Status: Accepted
- Date: 2026-10-15
- Related: [`../roadmap.md`](../roadmap.md) Phase 6, [`../windows-prereq.md`](../windows-prereq.md) 4 and 7, [`../concurrency.md`](../concurrency.md) Chapter 3

## Context

`roadmap.md` Phase 6 asked for three decisions before starting. The repository owner picked one option for each.

- `windows-prereq.md` 4 says to compare the Wintun DLL's `Subject` and hash against expected values, but there is no place for
  those values. Also, the SHA-256 the official site publishes is the hash of the release zip, so criterion 3, which compares the
  DLL hash against that value, can never hold
- `windows-prereq.md` 7 only stops when the subnet overlaps. An alternative range needs every peer to use the same value, but the
  control plane has no field to carry it, and `spec.md` FR-4 and `control_plane.md` 2.5 pin the range as a constant
- `concurrency.md` chapter 3 postponed splitting "ring full" into its own counter because the Wintun header had not been checked

Downloading the official Wintun 0.14.1 zip showed the following. The zip hash equals the published value. The signature of
`bin/amd64/wintun.dll` is `Valid`, and its `Subject` starts with `CN=WireGuard LLC, O=WireGuard LLC, …`. The signing certificate has
already expired, but the signature has a timestamp. `wintun.h` and `api/session.c` show that the only inject failure is a `NULL`
return from `WintunAllocateSendPacket`, with two causes: `ERROR_BUFFER_OVERFLOW` (the ring is full) and `ERROR_HANDLE_EOF` (the
adapter is terminating). `WintunSendPacket` has no return value.

## Decision

| Point | Choice |
|------|---------|
| Where the expected values live | One file, `cmake/Wintun.cmake`. It holds the version, the download URL, the zip hash, the DLL hash and the whole `Subject` string. The build downloads from that URL and compares against the expected values |
| Expected DLL hash | Computed from the DLL extracted from a zip whose hash matches the published one |
| `Subject` comparison | The whole string |
| Certificate validity period | Not judged |
| Alternative range | Not added. Stopping is the final v1 behaviour |
| Inject failure counters | Split in two. `ERROR_BUFFER_OVERFLOW` is `drop_inject_ring_full`, anything else is `drop_inject_error` |
| `ERROR_HANDLE_EOF` on the inject side | Does not signal shutdown. Drop and increment `drop_inject_error`. Shutdown handling stays in the one receive-side branch |

## Alternatives

**Where the expected values live.** A table in `windows-prereq.md` 4 was rejected. It is the simplest, but once the build or the
client uses the same values a second copy appears in code. A separate machine-readable file (JSON or similar) read by both CMake
and a script was rejected too. It keeps one source, but adds two readers. Committing the DLL to the repo was rejected. The build
would not need the network, but a public repo would carry a binary and its licence file. Keeping it in one CMake file enforces the
zip hash on every download and keeps binaries out of the repo. The cost is network access at build time.

**`Subject` comparison.** Comparing only `CN` and `O` was rejected. It breaks less on upgrades, but cannot tell apart another
certificate that uses the same names. Pinning the certificate thumbprint was rejected too. It is the strongest, but it is certain to
break when the next release changes certificates. The 0.14.1 certificate has already expired, so the next release will likely use
a new one.

**Certificate validity period.** Treating expiry as a failure was rejected. It would reject the correct 0.14.1 distribution.

**Alternative range.** Having the host pick the range and the control plane carry it was rejected. `spec.md` FR-4,
`control_plane.md` 2.5, Chapter 4 and 6.3, the sender-side validation in `protocol.md`, the client router, the server and their
tests would all change. Even then only the host-side conflict is solved. If a player's side overlaps, the room's range is already
fixed and the player still cannot join. Having every peer agree on a range from a fixed candidate list was rejected because an
overlap with a late player would force the whole room to move. Having users set the same value in configuration was rejected
because it is the same as the client deciding alone.

**Inject failure counters.** Keeping one counter and putting the cause only in the log was rejected. The ring-full count is a load
diagnostic, and mixing it with adapter termination blurs its meaning. Signalling shutdown on inject-side `ERROR_HANDLE_EOF` too was
deferred. Whether the receive side sees the same value in the same iteration has not been checked, and it would put the shutdown
path in two places.

## Consequences

**Gains.**

- The three criteria in `windows-prereq.md` 4 can run. The problem that criterion 3 could never hold goes away
- Phase 6 can start with nothing left to decide
- Inject failures can be told apart as load or something else

**Costs.**

- The build needs the network
- Upgrading Wintun means changing the four values in `cmake/Wintun.cmake` together
- A machine whose subnet overlaps with an interface it cannot bring down cannot use v1
- One more counter
