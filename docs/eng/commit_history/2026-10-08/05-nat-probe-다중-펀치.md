# Multi-Punch (host, players) Added to nat-probe

## Why

This is the Phase 4 pre-start `tools/nat-probe` re-measurement, first in "next" of `plan.md`. `roadmap.md` asks to
measure, at the same time, how one host behaves while punching four pairs at once, but the existing `punch` is 1:1.
The repository owner chose "emulate four players with four sockets on the friend's single machine" and "pass when
three consecutive runs have all four pairs `success`".

## What changed

| File | What |
|------|------|
| `tools/nat-probe/natprobe.py` | `run_links` (several pairs in one loop), `link_result`, `MappingKeeper`, `recheck_endpoint`, subcommands `host` and `players`. `links`, `player_sockets`, and `prepunch` in the result JSON |
| `tools/nat-probe/test_natprobe.py` | Pair verdict table, pair count limit, host and players over loopback, a silent peer, sources outside the list, the single-pair case, three forged PONGs |
| `tools/nat-probe/README.md` | Procedure for `host` and `players`, keeping mappings alive during preparation and the pre-punch check, the attribution rule, the limits of the emulation |
| `tools/nat-probe/RECORD-TEMPLATE.md` | 4.5 multi-punch tables |
| `roadmap.md` Phase 4 | Tool and pass criterion in the re-measurement item. Only runs that pass the pre-punch check count |

## Important decisions

- **`run_punch` stays as is.** The 22 two-party measurements ran with it and tests cover it. The new loop is
  separate. Both use the same verdict rule (`success`, `one-way`, `failure`)
- **PONGs are attributed by a per-pair session and the source is not checked.** Same reason as `run_punch`: when
  the peer NAT uses a different mapping, arriving from another port is normal
- **PINGs are attributed by source only when a socket has several pairs.** If no pair matches, they count only in
  `unattributed_inbound`. With one pair there is no source filter, like `run_punch`
- **Endpoints are asked during the run.** Public addresses are not left on the command line
- The emulation's limit (one remote IP) is written in the README and in the measurement record

## Verification

| What | Result |
|------|------|
| `python test_natprobe.py` | 194/194 |
| Mutants | Of the first nine, two survived (the PONG socket check, the timestamp check) and two forged-PONG tests were added. After the review fixes, fourteen (including duplicate PONG, PONG source filtering, pre-punch comparison, keeper stop, first-server choice) were run again and all were killed |
| Smoke test | Ran `host --peers 1` and `players --count 2` on this machine for 1 second against a silent loopback peer. STUN, endpoint output, input, verdict, and save all run |
| Document gate | `docgate.py` `VERDICT: pass` |

## Cross-model review

Codex. Two lenses (code, measurement design and docs) run separately on the staged diff.

| Lens | Finding | Handling |
|------|------|------|
| Code | warn. Tests still pass with the duplicate-PONG guard removed | Accepted. A test that sends the same PONG twice |
| Code | warn. Tests still pass if PONGs are filtered by source | Accepted. A test where a PONG from another port is `success` with `source_matches_expected` false |
| Design | warn. If mappings expire while endpoints are exchanged by hand, failures may come from stale endpoints, not punching | Accepted. STUN Binding Request every 10 seconds during preparation, re-check right before punching. If different, that run is not used for the verdict |
| Design | warn. A nonempty `unattributed_inbound` does not prove the peer NAT used another mapping | Accepted. Described only as "PINGs of unknown origin" and matched against the other side's results |
| Design | warn. The record template holds only one pair | Accepted. `RECORD-TEMPLATE.md` 4.5 |

The second review (repair check) raised one warn. The `players` keepalive sent every socket to the first socket's server. With
destination-dependent mappings the announced mappings would not stay alive. Accepted. Each socket sends to its own server. One test
and one mutant were added.

The third review raised one warn. `players` runs STUN socket by socket, so if later sockets' STUN runs long (up to 45 seconds when
three servers do not answer), earlier sockets wait without keepalive. Accepted. Each socket is added to the keepalive as soon as its
STUN finishes. A test was added.

The fourth review raised one warn. The pre-punch check ran after the shared Enter, so with many sockets and slow STUN the two
sides would start punching at different times. Accepted. The check runs before Enter and the keepalive runs until Enter.

The fifth review (checking that fix) returned `LGTM - no blockers`.
