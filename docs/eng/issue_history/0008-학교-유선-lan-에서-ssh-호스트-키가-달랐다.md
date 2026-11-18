# 0008. SSH to the instance from the school wired LAN showed a different host key

- Status: Worked around
- Found: 2026-11-18
- Related: the test device section of [`plan.md`](../../kor/plan.md) (Korean only), [`../commit_history/2026-11-18/04-phase-9-오늘-세-묶음-분석.md`](../commit_history/2026-11-18/04-phase-9-오늘-세-묶음-분석.md)

## Symptom

At school, this PC tried to SSH into the control plane instance to fetch today's batches' CloudWatch 1-minute values. Both Git Bash `ssh` and Windows OpenSSH refused with `REMOTE HOST IDENTIFICATION HAS CHANGED`. The ED25519 key the server presented differed from the one in `known_hosts`.

## Investigation

| Suspected | Observed | Verdict |
|-----------|-------|------|
| The instance's host key changed | The ED25519 key seen with `ssh-keyscan` from `mere-galaxy` on home Wi-Fi matched the key in `known_hosts` | No |
| Each SSH client has a different `known_hosts` | Both clients refused citing the same entry in the same `known_hosts` | No |
| The key received depends on the path out of this PC | `ssh` bound to the school wired LAN address got a different key. `ProxyJump` through `mere-galaxy` passed the stored key with `StrictHostKeyChecking=yes` | Yes |
| The phone hotspot path | This PC's Wi-Fi was disconnected at the time, so it was not checked | Unknown |

## Cause

**Confirmed.** The host key received depends on the path. When port 22 is reached through this PC's school wired LAN path, a key different from `known_hosts` arrives; from the home network the key matches `known_hosts`.

**Inferred.** On the school wired LAN path, something other than the instance appears to answer. A device on the school network appears to intercept and inspect SSH. The device and the policy were not checked. Whether the UDP tunnel and the control plane's TCP 8000 get the same treatment was not checked. All of today's trials on the school wired LAN connected.

## Handling

From the school wired LAN, do not SSH to the instance directly. Go through `mere-galaxy` with `ProxyJump`. The TCP connection to the instance then leaves from the home network, and host key checking and key authentication run end to end between this PC and the instance. `known_hosts` was not changed.

## Result

With `ProxyJump` the stored key passed and the CloudWatch values were fetched. Direct SSH to the instance from the school wired LAN still gets a different key.
