# Control Server Deployment and Names in `verify.py`

## Why

The display names, candidate hygiene ranges, and whitespace-in-header-name rule of round 1 D were on main but not deployed.
After round 4 K was done, the repository owner ordered the deployment. After deploying, the check tool
`tools/cp-deploy/verify.py` sent `create_room` without a name and failed with `bad_request`. The server has required
the name since round 1 D (`control_plane.md` 2.7, 4.2, 4.3), but the tool was not updated then.

## What Changed

| What | Where |
|------|-------|
| `create_room` and `join_room` carry a display name. A hexadecimal tag that differs per run plus a sequence number, so names do not collide in a room | `tools/cp-deploy/verify.py` |
| Removed the deployment row | `plan.md` |

## Deployment

| What | Result |
|------|--------|
| SSH host key | It had been refused as a mismatch on the school Wi-Fi. On this machine's home network the ED25519 fingerprint from `ssh-keyscan` matched the one in `known_hosts`. `known_hosts` was not changed |
| `tools/cp-deploy/deploy.py` | Uploaded `controlplane/` of commit `fe47fdc`. Confirmed `server.started`, and `get_peers` for a missing room was `room_not_found`. `VERDICT: pass` |
| `verify.py --players 4` (after the fix) | One room, four joins, no overlapping virtual IP, four confirmations, four reclaims. `VERDICT: pass`. `--concurrent --client-retry` also `VERDICT: pass` |
| Two real clients | Started `host` and `player` on this machine with the deployed server and the default STUN list. Both reached `CONNECTED`, and the learned address was the local candidate (`10.0.0.49`). The player received a `ROSTER` with both names. Both record files have `result=OK` and `result=CLOSED` with the same attempt number, and the RTT of the end line was about 0.6ms |

**Whether rooms made before the deployment remained was not checked.** By the type rule of `control_plane.md` 6.3, a
room holding a `PEER#` without a name answers its requests with `internal`. As the display-name item of `roadmap.md`
Phase 4 says, the deployment is treated as ending all earlier rooms.

## Verification

| What | Result |
|------|--------|
| `python -m pytest -q -p no:cacheprovider tools/cp-deploy` | 25 passed (the control-server venv) |
| `docgate.py` | `VERDICT: pass` |

## Cross-Model Review

Codex ran on the correctness of `verify.py` and the consistency of the records.

| Finding | Handling |
|---------|----------|
| warn. Random 32-bit names do not guarantee uniqueness in a room. A collision fails the check with `name_taken` | Fixed. One tag per run plus a sequence number. Retries send the same body, so the name is the same |
