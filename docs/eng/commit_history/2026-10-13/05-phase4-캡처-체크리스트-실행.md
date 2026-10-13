# Phase 4 Capture Checklist C0-C5 Run

## Why

This is the first run of the same-machine sections (C0 to C5) of [`tools/phase4-capture`](../../../../tools/phase4-capture/README.md),
made in round 4 K. The repository owner allowed installing Wireshark and Npcap. D1 to D3, which need a second
machine, and C6 to C9, which are on hold, were not run.

## What Changed

| What | Where |
|------|-------|
| Filled the C0 table on this machine and replaced the "not checked" paragraph with the result. Wrote that only the install needs administrator rights and capturing works as a normal user | `tools/phase4-capture/README.md` |
| Added to C2 how to leave out STUN from other programs on the same machine. Record the UDP ports the client processes own during the run and count only what leaves those ports | same file |
| Removed the capture row from the next tasks and moved the on-hold C6 to C9 to "waiting" | `plan.md` |

## Run

| Item | Value |
|------|-------|
| Date | 2026-10-13 |
| Commit | `82cebaa` |
| Build | product, Debug |
| Machine and network | this machine (Windows 11), home Wi-Fi. Tailscale runs on the same machine |
| Capture tool | `dumpcap` of Wireshark 4.6.9, Npcap 1.89. The loopback adapter and `Wi-Fi` together, capture filter `udp`. Analysis extracted rows with `tshark` and counted them with a script |
| Setup | The control server harness (DynamoDB local), one capture file per run (R1 to R4). The capture files are kept outside the repository |

## Results

| Section | Run | Observation | Verdict |
|---------|-----|-------------|---------|
| C0 | R1 | Harness STUN on loopback, A ↔ B on the LAN address on loopback, `192.0.2.1` on `Wi-Fi` | pass (setup check) |
| C1 | R1 | 0 sends that are neither tunnel nor STUN. Version 1 on all. All eight types appeared (`DATA` from B's `send 1`, `ROSTER` from host A). Every `payload_length` equals UDP length - 28 and matches the per-type length. The header `peer_id` read big-endian equals the value in the peer's log. The virtual IP of `HELLO` is in the same order as `control.peers`. One non-zero epoch per side. `sequence` only increases from 1. First 4 bytes `53 41 4e 47` | pass |
| C2 | R1, R4 | R1: the STUN source port set and the tunnel source port set are both `{P_A, P_B}`. R4 (run once more to record port ownership): during the run each client process owned exactly one UDP port, its own `P` (`Get-NetUDPEndpoint`). STUN and tunnel sent from those ports come from `{P_A, P_B}`, and the responses go to each client's `P`. The only other STUN source was 41641, owned by `tailscaled`. The two `stun.result` lines of each client are the same `<public IP>:P` (the public IP is not written down). The home NAT kept the port | pass. The same public IP:Port check also passes |
| C3 | R2 first scene | 49 `HELLO`s to the fake peer's one candidate, 190-215ms apart, 9.918s from the first (`t0`) to the last (before `t0 + 10.2s`). From `t0 + 10.2s` to `t0 + 20.2s`, 0 `HELLO` left `P_A` to any destination. A printed `FAIL HOLE_PUNCH_TIMEOUT Fake1` | pass |
| C4-1 | R2 second scene | `quit` in `PUNCHING` before learning. 0 `CLOSE` from A | pass |
| C4-2 | R1 | One `CLOSE` from A, `reason` 0x00, to the learned address of `session.endpoint` (the peer's LAN address). None to the `192.0.2.1` candidate. B was already `CLOSED` by A's `CLOSE` and sent none | pass |
| C4-3 | R3 second scene | `quit` in `HANDSHAKING` after learning. One `CLOSE`, `reason` 0x01. Destination (observation) is the fake peer's address | pass (count and `reason`). Destination judgment on hold |
| C4-4 | R3 first scene | One `CLOSE` on `PEER_HANDSHAKE_FAILED`, `reason` 0x03, then 0 `HELLO`. Destination (observation) is the fake peer's address | pass (count, `reason`, stop). Destination judgment on hold |
| C4-5 | R2 first scene | 0 `CLOSE` on `HOLE_PUNCH_TIMEOUT` before learning | pass |
| C5 | R1 | On each side the first `KEEPALIVE` is 1ms after the last `HELLO`, gaps 15.001-15.011s, 4 in the 60-second window from the first, 0 `HELLO` after the first `KEEPALIVE`, every `KEEPALIVE` to the one learned address | pass |
| C6-C9 | - | - | on hold |
| D1-D3 | - | - | second machine |

The fake peers behaved as the README expects. `silent` sent nothing to A, and `hello_only` sent only `HELLO` while A
entered `HANDSHAKING`.

## Verification

| What | Result |
|------|--------|
| `docgate.py` | `VERDICT: pass` |

## Cross-Model Review

Codex ran on the evidence of the record and the consistency of the README.

| Finding | Handling |
|---------|----------|
| warn. The C3 "nothing after the stop" window was written as 10 seconds from the last `HELLO`. The README says 10 seconds from `t0 + 10.2s` | Fixed. That window was recounted from the capture and is 0 to any destination |
| warn. The R4 filter of C2 assumed `stun.result server=` in the log holds the resolved address. That value is the name before resolution | Fixed. The README now filters by the UDP ports the client processes own during the run, and R4 was run again that way |
