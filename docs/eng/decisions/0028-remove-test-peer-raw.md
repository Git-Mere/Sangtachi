# 0028. Remove the test-only `--peer`, `raw`, and `rx.raw`

- Status: Accepted
- Date: 2026-10-19
- Related: [`../architecture.md`](../architecture.md) 3.5 and chapter 9, [`../concurrency.md`](../concurrency.md) chapters 1 and 3, [`../roadmap.md`](../roadmap.md) Phase 1 and 7

## Context

Phase 1 confirmed send and receive between two known endpoints, with no control plane and no STUN. Three things were added for that check.

- The startup argument `--peer <IPv4>:<port>`. It takes the remote address directly
- The console command `raw <byte count>`. It sends one test byte string with no tunnel header to that address
- The log `rx.raw`. It records `len` and `sha256` for every datagram received, and for each one sent by `raw`

The documents kept all three as test-only and did not decide when to remove them. `scripts/e2e-check.ps1` ran the Phase 1 verification with them.

Once Phase 7 joins the adapter and the tunnel, real traffic flows as `DATA`. Two things then become a problem.

- `rx.raw` emits one `INFO` line per datagram received. With game traffic the log floods
- `--peer` and `raw` send a test byte string to any address the user gives, with no session and no authentication. Once a product path exists there is no reason to keep them

## Decision

Remove all three as the first task of Phase 7. The repository owner decided this.

| Item | Choice |
|------|---------|
| `--peer` | Removed. Giving it is an unknown argument and a startup failure (exit code 2) |
| `raw` | Removed. Typing it is an unknown command and leaves one `WARN` line |
| `rx.raw` | Removed. Without the sending-side partner there is nothing to compare against |
| `hash` module | Kept. The `wintun.dll` hash check and `data.tx` and `data.rx` in the test build use it. The 16-character short form that only `rx.raw` used is removed |
| `scripts/e2e-check.ps1` | Kept and reduced. It checks startup and bind of two processes, `counters`, an unknown command, and the exit code after `quit` |

Byte-for-byte match is checked by the `DATA` round-trip match (`data.tx`, `data.rx`) in the Phase 4~5 verification. That a failed send raises
`tx_err_send`, leaves the error code in a `socket.error` line, and that the loop still handles console commands afterwards is checked by the failed-send test in `tests/loop_test.cpp`.

## Alternatives

**Keep them until the final cleanup Phase.** Rejected. From Phase 7, `rx.raw` emits a line for every game datagram. Removing only `rx.raw`
first would break the `raw` send/receive comparison and leave the other two useless.

**Keep them only in the test build.** Rejected. The Phase 1 send/receive check is done, and the `DATA` round-trip match checks the same
property. Keeping them would carry a startup argument that exists only in the test build, and tests that filter it, from then on.

**Delete `scripts/e2e-check.ps1` and move the remaining checks into other scripts.** Rejected. The remaining checks run without a control
server, so putting them in `scripts/lobby-check.ps1`, which needs DynamoDB local, makes them heavier to run.

## Consequences

**Gains.**

- Real traffic does not produce a log line per datagram
- There is no path that sends arbitrary bytes to an address the user gives. Only STUN Binding Requests go to the addresses given by `--stun`
- One startup argument and one console command fewer

**Costs.**

- There is no end-to-end test that checks byte match between two processes on one machine without administrator rights. The `DATA`
  round-trip match needs a session, so it needs a control server
- There is no test that checks a failed send at the executable level. The unit test checks it with the same loop code
