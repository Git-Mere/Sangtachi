# 2026-09-18 NAT measurement tool and blocker 20 field test

## Changes

Built a tool to measure `design-audit.md` blocker 20 (direct connection may be impossible) and
ran it twice. This is follow-up 5 in [`../../../kor/plan.md`](../../../kor/plan.md) chapter 5.

- Added `tools/nat-probe/natprobe.py`. STUN mapping behaviour measurement (`probe`) and UDP hole
  punch test (`punch`). Single file, standard library only
- Added `tools/nat-probe/test_natprobe.py`. 20 self-checks that run without a network
- Added `tools/nat-probe/README.md` and `RECORD-TEMPLATE.md`. Usage and record form
- `tools/nat-probe/results/` and `records/` are added as empty directories only

**The measurement data (8 JSON files) and the 2 filled records are not in this commit.** This
repository is public, and those files contain the public IPs of two households together with
minute-level timestamps. Publishing a friend's home IP in a public repository cannot be undone, so
that is decided separately. The files remain in the working tree.

**The documents (`spec.md`, `plan.md`, `design-audit.md`, `decisions/`) are not changed in this
commit either.** See "Remaining" below.

## Tool design

**Kept separate from the client itself.** Phase 1 has not started, so measuring in C++ would mean
waiting for the implementation. The measurement is needed now. Instead the tool follows the
contracts in [`../../protocol.md`](../../protocol.md) so the numbers come from the same conditions as
the client.

- STUN and punch use the **same socket and the same local endpoint**. Same reason as `spec.md`
  NFR-9. Separate sockets get different source ports, so the mapping differs too and destination
  dependence cannot be told apart
- No `connect()`, `SO_EXCLUSIVEADDRUSE` set, `SIO_UDP_CONNRESET` off. This is the `protocol.md`
  chapter 15 checklist. Without turning off `SIO_UDP_CONNRESET`, the ICMP port unreachable that
  arrives while the peer has not opened its port yet breaks `recvfrom` with `WSAECONNRESET` on
  Windows
- STUN retries at 0/500ms/1s/2s, deadline 5s. Same values as the `protocol.md` chapter 11 timer
  table
- Only `XOR-MAPPED-ADDRESS` is interpreted. This is the `protocol.md` chapter 13 scope

**The verdict is deliberately conservative.** Mapping is judged only when at least two servers
with different IPs answer; otherwise the result is `unknown`. Querying one IP means one
destination, which cannot show destination dependence. `unknown` is a failed measurement, not a
NAT behaviour.

**`probe` alone does not decide.** A STUN Binding response reports mapping behaviour only, not
filtering behaviour. Hole punching depends on both. This is why `plan.md` chapter 5 says the
first check alone is not enough, and why `punch` was built alongside it.

## Measurement results

| Measurement | Topology | Mapping (A/B) | Result | Record |
|------|----------|------------|------|------|
| 2026-09-17 | 2 mobile hotspots (both T-Mobile) | EI / EI | `success` once | not committed |
| 2026-09-18 | **2 home networks** (both Comcast, different /17) | EI / EI | **`success` 3 times in a row** | not committed |

EI = `endpoint-independent`.

The home network measurement matches required test topology (2) in `spec.md` line 122. Median RTT
37-39ms, loss under 1%, and `source_matches_expected` was `true` in all 6 runs. No condition was
changed between rounds, and the end times of each round agree within 1 second.

**The worst case of blocker 20 (both sides symmetric, so minimum success is impossible) did not
happen.** The current judgement is that none of options A/B/C in `plan.md` chapter 5 is needed and
that M-1 and M-4 can stay as written. This is not yet reflected in the documents, for the reasons
below.

## Fixes

**The precondition in README 5.2 was stricter than `spec.md`.** It said the two networks must be
on **different ISP lines**, but `spec.md` line 122 requires "2 different home networks". Left as
written, this measurement (both sides Comcast) would have been discarded as not meeting the
criterion. This is an instance of recurrence-prevention rule 3 in `design-audit.md` chapter 6
("check first whether a correct implementation passes the criterion"). It now explains how to
confirm separate lines, and states the limit that the same ISP narrows the scope of the evidence.

**The `loss_pct` denominator was wrong.** A PONG for a PING sent before the first inbound packet
was counted in the numerator, so the numerator could exceed the denominator. The hotspot JSON
still shows this (numerator 149 > denominator 148). It now matches the interval by sequence
number. **The original JSON files were not edited, because they are measurement records.** The
conclusion (loss under 1%) does not change.

## Verification

- `test_natprobe.py` passes **38/38**. Truncated response, wrong magic cookie, transaction ID
  mismatch, length field mismatch, IPv6 family discard, Binding Error Response, attribute padding
  walk, 4 mapping verdict cases, punch packet round trip and old-version rejection, 12 port and
  `--duration` input validation cases, and file name collision
- 8 measurements over real NAT paths. Success, failure, and timeout paths all observed
- Cross-checked the endpoints in both sides' JSON. All 6 runs point at each other exactly
- Confirmed the public IP allocations with RDAP (T-Mobile / Comcast, different blocks)
- Every relative link under `tools/nat-probe/` resolves to a real path

## Cross-model review

- Reviewer: Codex (GPT). Two runs with separate lenses (code / documents and claims)
- Result: 1 blocker, 11 warns, 1 nit. **The blocker was rejected; the other 12 were all applied**

| Finding | Decision |
|------|------|
| blocker: claims `natprobe.py`, tests, and result files were added, but they are not present | **Rejected.** An artifact of splitting the review into code and document scopes, so the document reviewer never saw the code diff. `git diff --cached --stat` lists all 18 files |
| warn: STUN replies are accepted from any port on the resolved IP | Applied. The source is now checked as an IP and port pair |
| warn: PONGs are counted without checking source, sequence, duplication, or timestamp | Applied. A 32-bit random session value is carried in the packet, and only PONGs whose session, sequence, and timestamp all match a PING actually sent are counted. Each sequence counts once. **The source is deliberately not filtered.** If the peer's NAT uses a different mapping, arriving from another port is normal and is itself the thing being observed |
| warn: unknown punch packet kinds still change first-inbound timing and metrics | Applied. `parse_punch` now validates the kind and drops them first |
| warn: a NaN or infinite `--duration` makes the loop run forever | Applied. `math.isfinite` plus a positive check |
| warn: port ranges are not checked, causing `OverflowError` | Applied. `valid_port` enforces 1..65535; only the local bind may be 0 |
| nit: second-resolution file names collide and overwrite an earlier measurement | Applied. Exclusive creation with a suffix retry. Round 2 actually produced the same second on both sides |
| warn: `RECORD-TEMPLATE.md` still treats "different ISP" as a validity condition, contradicting README 5.2 | Applied. Changed to "different home networks", with same-ISP recorded separately as a limitation |
| warn: declaring blocker 20 resolved is premature while Windows and the real tunnel protocol are untested | Applied. The verdict in the record is now **provisional**, with the conditions for making it final stated |
| warn: committing public IPs and precise timestamps to a public repository | Applied. The measurement JSON and records are excluded from this commit |
| warn: different public IPs and /17 blocks do not prove distinct subscriber lines | Applied. The claim is reduced to "not behind the same NAT", and the unverified WAN IP is added as a limitation |
| warn: `--peer` leaves the peer endpoint in shell history and process listings | Applied. README now warns against using it on shared computers |

The packet format changed from `NATPRB1` (24 bytes) to `NATPRB2` (28 bytes). The two versions
ignore each other, so **both sides must run the same version.** The existing measurements used
`NATPRB1`, and that is noted in the records.

## Remaining

**Document updates are deferred to the next commit.** The verdict is in, but two things are
missing.

1. **Windows is untested.** This measurement used Linux and macOS. M-4 is a criterion for the
   Windows client, and the Windows firewall was not part of the measurement. All 6 runs had
   matching ports, so the firewall behaviour when the peer replies from a different port was not
   exercised either
2. **Same ISP.** Both sides are Comcast, so the evidence is limited to "between Comcast home
   networks"

The documents will be updated in one pass after re-measuring with 2 Windows machines on different
ISPs. The targets are below.

| Target | Content |
|------|------|
| `decisions/` | ADR 1. The decision not to adopt a direct-connection fallback, and the discarded options A/B/C |
| `plan.md` chapter 5 | Mark the status complete |
| `design-audit.md` chapter 5 | Update follow-up 5 |
| `roadmap.md` Phase 8 | Add "re-measure right before the demo" to the preparation steps, because there is no fallback for the fallback |

This commit goes up first so the tool can be pulled and run on a Windows laptop.
