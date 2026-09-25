# 2026-09-20 Add the unsolicited inbound test

## Changes

Added an `--unsolicited` test to `natprobe.py` that measures whether the Windows firewall passes
UDP arriving from a source **this machine has never sent to**.

- Added `run_unsolicited()`. After the punch succeeds, it sends from a new socket to the peer, and
  the peer receives it on its main socket and returns an acknowledgement. Both directions are
  measured in one pass
- Added packet kinds `PUNCH_UNSOL` (3), `PUNCH_UNSOL_ACK` (4), and `PUNCH_PHASE_READY` (5). The
  format and length (28 bytes) are unchanged
- Split out `unsolicited_verdict()`. The verdict logic is a pure function so it can be tested
- Added the `--unsolicited` and `--unsolicited-duration` arguments
- Added an `unsolicited` block to the JSON
- Added README 4.3 and 9.5
- Self-checks went from 41 to 51

## Problem

In the 2026-09-20 US-KO measurement, no Windows firewall prompt appeared, yet all inbound traffic
arrived. Querying the environment narrowed down the cause.

| Check | Value |
|------|-----|
| `DefaultInboundAction` (ActiveStore) | **Block** on Domain, Private, and Public |
| `NotifyOnListen` | `True` everywhere. Notifications were on |
| Wi-Fi profile | **Public**, the strictest one |
| Firewall rules for `python.exe` | **None** |

Only one explanation fits all four. **Stateful UDP.** Because we sent first, the replies were
classified as solicited traffic, no rule was needed, and so there was no reason for a prompt.

**That means the firewall check is only half done.** The state is opened only for the address and
port we sent to. If the peer's NAT uses destination-dependent mapping, the peer arrives from a
port we never sent to, and our firewall blocks it even though the NAT let it through. In all 6
measurements so far, `source_matches_expected` was `true`, so this path was never exercised.

It can be **predicted** that it is blocked, but it has not been **measured**. Recurrence
prevention rule 1 in `design-audit.md` chapter 6 says to trace a scenario end to end before
declaring completion.

## Decisions

**Never write `blocked` without evidence.** The peer may be running an old version or may have
omitted `--unsolicited`. Recording that as blocked invents a defect that does not exist.
`blocked` is used only when there is evidence that the peer ran this phase (a received packet or a
received acknowledgement); otherwise the verdict is `unknown`.

**It runs only when the punch succeeded.** Measuring on a failed path says nothing about what was
measured.

**It is documented that `blocked` does not isolate the cause.** It may be the firewall or NAT
filtering. However, if one side is not behind NAT (its `socket.local_ip` equals its public IP),
that side's result can be read as a firewall-only result. The Korean side meets that condition, so
the next measurement will produce a clean value.

**The acknowledgement is sent from the main socket.** The peer's new socket has already sent, so
its firewall treats the reply as solicited. If the acknowledgement were blocked too, the two
directions could not be told apart.

## Verification

- `test_natprobe.py` passes 51/51. Added 3 round-trip cases for the new packet kinds, 2
  undefined-kind rejections, and 5 verdict logic cases
- 3 two-process scenarios
  - Both sides with `--unsolicited` gives `allowed` / `allowed`, and `recv_sources` points at the
    peer's **new** port
  - Only one side gives `unknown` / `unknown`. It does not say `blocked`
  - A failed punch skips the phase and records the reason
- **It has not been run over a real firewall and NAT path yet.** That happens at the next Windows
  measurement

## Cross-model review

- Reviewer: Codex (GPT). Two rounds. The code changed substantially after round 1, so round 2 was
  run again
- Result: round 1 gave 3 warns; round 2 gave 1 blocker, 3 warns, and 2 nits. **All applied, none
  rejected**

| Round | Finding | Decision |
|--------|------|------|
| 1 | UNSOL is counted without checking the source, so stray or forged packets create false verdicts | Authenticate by source **IP** and the **peer session value**. The source port is deliberately not filtered, because a different port is the premise of this test. The peer session is already known from the peer's PINGs during the punch, so no new exchange is needed |
| 1 | Without a readiness signal, a skewed start makes a passing path record as `blocked` | Added a `PHASE_READY` sync stage. READY keeps being sent during the measurement too, so a peer that enters late can still sync |
| 1 | Local IP equal to public IP does not make `blocked` a firewall-only result; upstream ACLs and ISP filtering remain | Applied. The claim is reduced to "only local NAT translation is excluded" |
| 2 | **blocker.** UNSOL from the already-contacted main endpoint is counted, producing a false `allowed` | Reject `src == peer`. That path is already solicited and proves nothing |
| 2 | UNSOL_ACK is not matched against probes actually sent, so duplicates and forgeries create a false `allowed` | Record the sequence and timestamp of each probe sent and match against it. Each sequence counts once |
| 2 | The peer is recorded as `blocked` even when every send failed | `unsolicited_verdict` now takes `sent`. With no successful send the verdict is `unknown` |
| 2 | `unsolicited_verdict` is defined twice | Removed the duplicate |
| 2 | The self-check count and packet kind description in the commit record do not match reality | Corrected |

**Another tool defect was found while applying these.** It surfaced while testing the sync with a
10 second skew. `run_punch` was falling through to the `PONG` branch for every kind that was not
`PING`. Adding 3 kinds broke that premise while the comment stayed the same, so a peer that
finished punching first would have its `PHASE_READY` added to `recv_pong`. Kinds are now filtered
first and counted separately as `next_phase_datagrams`, kept apart from `non_punch_datagrams`,
which means "the port collided with someone else's traffic" and is not normal.

**Removing the duplicate definition also deleted `_send_ready` and `_sync_phase`.** All 51 unit
checks still passed, and the two-process integration run exposed it as a `NameError`. It is
recorded here as a case where unit checks alone were not enough.

## Remaining

If this test comes back `blocked`, the client gains a requirement. It must register an inbound
allow rule, and that needs administrator rights. blocker 11 (administrator rights) and blocker 12
(firewall) in `design-audit.md` meet here, and follow-up 3 gains one more item.

It also affects the "without manual port forwarding" claim in `spec.md`. Port forwarding goes
away, but firewall rule registration appears, so the statement that the kind of setup burden
changes must be updated too.

Nothing is changed until the result is in.
