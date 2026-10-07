# Client [control] thread, lobby commands, FAIL line

## Why

This is the client side of the `roadmap.md` Phase 3 work. The server and deployment were finished in the
earlier records (08~11). Now the C++ client calls the control plane, creates and joins rooms with lobby
commands, and reports failures as `FAIL` lines.

## What changed

Four workers split the building, and the director took the documents and the verification.

| Branch | Files | What |
|--------|-------|------|
| Codec | `client/*/control/{json,http,ops,constants}` | Hand-written JSON parser and serializer (NFR-5), 3.3 request bytes, 3.5 response checks, requests and parsing of the five operations, 8.3 classification, sanitation of received candidates (protocol.md 10.1), 4.6 host action classification, `client_nonce` |
| `[control]` thread | `platform/tcp`, `control/{exchange,channel}`, `spsc_ring.hpp`, `loop`, `main`, `args` | A new TCP connection per request (connect 3 seconds, receive stage 3 seconds), two SPSC rings, wait set rank 4, `_exit` after a 2-second join at shutdown. `--server` required, role-less `--room` fails startup |
| Lobby | `control/{lobby,runner}`, `platform/stdout` | A thread-less state machine and its runner. A new `StunClient` per attempt. Startup STUN removed. Standard output is UTF-8 + LF when redirected, and Unicode writes when it is a console |
| Harness | `control-server/tests/harness/`, `scripts/lobby-check.ps1` | Starts the real server on a new DynamoDB local table and injects delay, errors, failure after commit, clock and rate limit through an admin port. `lobby-check.ps1` runs every item of the roadmap "Lobby" and "Client-side contract" groups. The Phase 2 STUN checks of e2e were moved here |

`-Jobs` was added to `scripts/test.ps1`. The default is `ctest -j` with the number of cores. A run without a
filter also runs `lobby-check.ps1`.

## Decided in the documents first

Places the workers reported as "not in the documents" were decided in the documents before the code.

- `control_plane.md`
  - 2.4: If the CSPRNG fails, `CONTROL_PLANE_EXCHANGE_FAILED` without a request
  - 2.6: `CLIENT_JSON_MAX_DEPTH = 32`. `MAX_HEADER_BYTES` also for the response header
  - 3.3: The `Host` value is the name and port of `--server`
  - 3.5: The client checks were spelled out as 1~8. A success with a wrong type is a temporary error, and a code not in the table is a final error
  - 4.6: "Other final errors" and a success with a wrong type were added to the host error table
  - 8.2: Send is per call, receive is 3 seconds for the whole stage
  - 8.4: The host does `confirm` on a peer with `ready: false`. If 0 candidates remain after sanitation, it is not ready
- `concurrency.md` chapter 7 `_exit` exit code 0, chapter 8 response queue full, `[control]` wait failure, 50ms shutdown check, a
  `host_report` whose turn comes while one is pending, and retry
- `protocol.md` 10.1: The order for a received list (type, sanitation, duplicates, limit) and the broadcast range
- `architecture.md` 3.5: Commands with leftover words, standard output encoding. Chapter 8: the
  `CONTROL_PLANE_EXCHANGE_FAILED` sentence was changed to also fit the host. Chapter 9: `unknown_code`, `self_in_peers`
- `roadmap.md` Phase 3: The client of this Phase stops at step 8 of 8.4 (the review had it moved out of 8.4).
  How to produce the transport error of the retry verification on loopback, and the injection point of the second `internal`

## Important decisions

- **The full test run requires DynamoDB local.** This is because of `lobby-check.ps1`. The repository owner
  approved it. If it is missing, the run fails instead of skipping. The client tests use the test-only
  dependency approved by ADR 0014 (Docker DynamoDB local), and it is not a new dependency
- **A connect timeout cannot be produced on Windows loopback.** In a measurement that filled the listen
  backlog, the result was a `WinError 10061` refusal after about 2.03 seconds. Transport errors are produced
  with a server that does not answer and a closed port
- **Server sanitation rejects a loopback STUN mapping.** The harness STUN responder writes `192.0.2.1` and the
  source port. Running the Phase 4 punch with this harness needs other candidates
- The mutation that removes the runner's re-entry guard (F12) survives. The queue is FIFO, so the inner `run`
  drains the outer remaining actions in the same order. It was judged equivalent by reading the code

## Verification

| What | Result |
|------|--------|
| `scripts/test.ps1` without a filter | unit 280/280, `cli-check` passed, `e2e` 0 failures, `lobby-check` 0 failures 0 skipped |
| `control-server` `pytest -q --store` | 877 passed |
| Platform gate | `VERDICT: pass` |
| Mutations | codec 85, lobby 71, `[control]` 46 (one F12 equivalent remains). For the harness and scripts, harness mutations, injection off, no DynamoDB, an injected client defect and an injected partial failure all gave FAIL. The worker reports are the source of the counts and tables |
| Real server | Two processes, host and player, on this machine against the deployed EC2 (b20224e). After responses from two public STUN servers, `create_room`, `join_room`, `register_candidate`, two host `host_report`s and sixteen player `get_peers`, each side emitted one `control.peers` line. Virtual IPs: host `10.100.0.1`, player `10.100.0.2`. No `FAIL`, both ended with `quit` and exit code 0 |

Two clients on different networks (a Phase 3 deliverable) need a second machine. This time it was two
processes behind one NAT.

## Cross-model review

Codex. Five scopes (codec, `[control]` thread, lobby, harness and scripts, documents), two lenses each.

| Round | Scope | Result | Handling |
|-------|-------|--------|----------|
| c1 | Codec | warn 2. The 9th candidate is fully valid, so an implementation that truncates before the type check passes. The depth test is derived from the implementation constant | Accepted. A 9th case with a wrong type, and the document value 32 written directly. Two mutations fail on the new cases |
| c1 | `[control]` thread | warn 2. It counts an auto-reset event as eight notifications. bind comes before DNS resolution (steps 2 and 3 of 8.4) | Accepted. Waits for the cumulative count together with the deadline. bind after resolution. cli-check confirms there is no `socket.bind` when resolution fails |
| c1 | Lobby | warn 1. Passing its own `peer_id` in `host_report` is not tested | Accepted. A runner test added, and a mutation fails on it |
| c1 | Harness and scripts | blocker 2, warn 3. An injected delay blocks shutdown and the table is left behind. The role-less `--room` check can hang forever. Startup failure cleanup, the harness exit code not checked | Accepted. The delay can now be woken, and a shutdown-during-delay test was added. The rest were fixed too and confirmed with mutations |
| c1 | Documents | warn 3, nit 2. The send delay description, generalisation of transport errors, `control_plane.md` holding the Phase scope, experiment history, references without section titles | Accepted. The Phase 3 scope was moved to `roadmap.md` |
| c2 | Fixed places | warn 2. The same blocking read remains in `cli-check.ps1`. The startup cleanup of the harness fixture wraps only part of it | Accepted. Searched all of `scripts/` for the same pattern and confirmed it was the only one |
| c3 | Fixed places | warn 1. The process start helpers of `lobby-check` and `e2e-check` leave streams and children behind on partial failure | Accepted. Every helper that acquires resources was judged with a table. The one left (a `mutate.py` timeout can leave grandchild processes) was written in `plan.md` |
| c4 | Fixed places | warn 3. Sockets and pipes inside tests in `test_server.py` are not closed on partial failure | Rejected. What was closed this time are resources that remain after the run (processes, tables, temporary folders). These three are handles inside the pytest process, so they are reclaimed when the process ends, and they occur only in tests that are already failing |
