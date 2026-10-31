# Linux console client (ADR 0035)

## Why

To run the two-machine field tests with the owner's Windows machine and Linux machine (`mere-galaxy`, Ubuntu 26.04). The decision and the
rejected alternatives are in [ADR 0035](../../decisions/0035-linux-console-client.md). Two sub-workers did the implementation. One did the
common POSIX layer, the other the Linux adapter and route queries. The parent fixed one defect and ran the checks in both directions and the
Windows regression.

## What changed

| What | Where |
|------|-------|
| Common POSIX platform layer: waits (pipe events and `poll`), UDP, TCP, name resolution, random, SHA-256, standard output, record files, shutdown signals, session end | `client/src/platform/posix/` |
| Linux only: clock, executable path, rtnetlink queries, interfaces and default route, TUN device, subnet verdict | `client/src/platform/linux/` |
| The list of files next to the executable becomes `adapter_files()`, the firewall verdict gains `kSkipped`, the `adapter.open guid=` text goes through `interface_id_text` | `tun.hpp`, `interfaces.hpp`, `win32/`, `client_process.cpp`, `main.cpp` |
| Reading one line of standard input becomes `platform::read_stdin_line`. The Linux version does not go through stdio (defect below) | `console_ctrl.hpp`, `win32/console_ctrl.cpp`, `posix/console_ctrl.cpp`, `loop.cpp` |
| The Linux branch of CMake, the Linux test list | `client/CMakeLists.txt`, `client/src/CMakeLists.txt`, `tests/CMakeLists.txt` |
| 24 POSIX/Linux OS headers | `tools/platformgate/` |
| Linux tests: waits, UDP size boundary, signals, record files, standard input, the subnet verdict table, TUN lifetime. More SHA-256 vectors | `tests/` |
| Targets, prerequisites, threads and shutdown, test rules | `spec.md`, `roadmap.md`, `architecture.md`, `concurrency.md`, `protocol.md` chapter 6, `linux-prereq.md` (new), `CLAUDE.md` |

## Defect: a normal Linux shutdown was held up by standard input

After a shutdown signal the cleanup finished up to `adapter.close`, but the process did not end until a line arrived on standard input or it hit EOF.

- **The cause was confirmed by a reproduction.** On `mere-galaxy` (glibc 2.43), when `main` returns while another thread reads with
  `std::getline(std::cin)` or `fgets`, the process did not end until standard input closed (4006ms). Reading the same way with `read(0)` ended at once (308ms).
  A blocked stdio read holds the stdin lock and `exit` waits for that lock
- **The fix.** `[console]` reads with `platform::read_stdin_line`. The Windows version is the same `std::getline(std::cin, line)` as before.
  The Linux version reads with `read(0)` and splits lines. The rule is in the `[console]` item of `concurrency.md` chapter 7 Shutdown
- **Rejected.** `std::_Exit` after cleanup. It would change the Windows shutdown path too

## Verification

| What | Result |
|------|--------|
| `mere-galaxy` ctest (g++ 15, Debug) | 74 of 75 passed, 1 skipped (`linux_tun` lifetime test, needs root). That test was run separately with `sudo` six times and passed every time |
| WSL Ubuntu 24.04 (g++ 13) build | Passed without warnings |
| Mutations | 7 in the POSIX layer (waits, UDP, SHA-256, signals, standard output), 10 in the subnet verdict, 2 in standard input (back to `getline`, dropping the last line before EOF). The tests caught all of them |
| Persistent TUN device with the same name | With a device made by `ip tuntap add` present, the build before the fix attached to it and put the address on it. The fixed build failed to start with `adapter.error reason=open op=TUNSETIFF code=16` and left the device alone |
| Windows `scripts/test.ps1` | `all tests passed`. 664 ctest cases, 0 failures in the CLI, e2e, lobby and session checks |
| platformgate | `VERDICT: pass`, 74 tests passed |
| `docgate.py`, `--claims` | `VERDICT: pass` |

**Field checks in both directions.** The control server is the production server. Both sides used the adapter (Windows as administrator, Linux with `sudo`).

| Setup | Joining the room | Virtual IP `ping` | Shutdown signal and `CLOSE` |
|-------|------------------|-------------------|-----------------------------|
| Linux host, Windows player | `CONNECTED`, roster with two members | Windows→`10.100.0.1` 4/4 (mean 31ms), Linux→`10.100.0.2` 4/4 (mean 76ms) | The host ended 80ms after `SIGINT`, `adapter.close`, device gone. Player `CONNECTED to=CLOSED`, `reason=closed` |
| Windows host, Linux player | `CONNECTED`, roster with two members | Linux→`10.100.0.1` 4/4 (mean 50ms), Windows→`10.100.0.2` 4/4 (mean 54ms) | The player ended 81ms after `SIGINT`. Host `CONNECTED to=CLOSED` |

- **Minecraft on the Linux host.** Java 25 and Minecraft server 26.3 were installed on `mere-galaxy`. The owner connected from Windows to `10.100.0.1`
  and entered the world (server log `mere_ko joined the game`, address `10.100.0.2`). The first try was stopped by the server default `white-list=true`.
  Even then the TCP connection and account authentication went through the tunnel
- Both field checks ended with a signal while standard input was still open. The defect above is fixed on the real machine too
- ufw was off on the test machine. The rule in `linux-prereq.md` section 5 was not checked
- **Latency.** At first Windows→`10.100.0.1` was 168–208ms. The worker attributed it to Wi-Fi power saving on `mere-galaxy`. A `ping` to the LAN address
  without our program also averaged 96ms, and 9ms with the radio kept awake. Power saving was turned off for the field checks. The values in the table are after that

## What the workers decided themselves

Values that were not in the work brief and that touch a contract. The parent checked that none of them disagrees with the living documents.

- The wait handle encoding lives in one internal header, `posix/fd_handle.hpp`. A large finite timeout is waited in pieces because of the `int` limit of `poll`
- Failure op tokens are the real call names (`fcntl`, `poll`, `RTM_NEWADDR` and so on)
- When deleting a route, `ESRCH` also counts as "not there" and success. Routes we make are made and deleted with `RTPROT_BOOT` so kernel routes are not touched
- The subnet verdict function is the header-only `linux/subnet_check.hpp`, separate from OS data
- `signal.h` and `time.h` are also ISO C headers, so they are not in the platformgate OS header list

## Cross-model review

Codex (`gpt-6.1-sol`). The scope was split in four with two lenses each. Only the repair diff was run again with a regression and mirror lens, and it was LGTM. Last, the whole staged diff was run with a record and handoff consistency lens, and it was LGTM.

| Scope | Lenses | Result |
|-------|--------|--------|
| `platform/posix/`, its tests, `loop.cpp` | Contract conformance, concurrency and lifetime | blocker 2, warn 1 |
| `platform/linux/`, its tests | rtnetlink and TUN correctness, security and permissions | blocker 2, warn 2 |
| Shared files, `win32/`, CMake, platformgate | Windows byte-for-byte unchanged, build and gates | LGTM |
| Documents and `CLAUDE.md` | Cross-document consistency, evidence | warn 5 |

| Finding | Disposition |
|---------|-------------|
| blocker: `TUNSETIFF` attaches to a persistent device with the same name, changes its address and MTU, and the device stays after close | Accepted. `IFF_TUN_EXCL` is set. The field check in the verification table saw both the behavior before and after the fix. Written in `linux-prereq.md` section 3 |
| blocker: the error value of `NLMSG_DONE` and `NLM_F_DUMP_INTR` are not checked, so an incomplete dump can give `proceed` | Accepted. Both become a query failure. If nothing blocks, the verdict is `unknown` and it is a startup failure. There is no way to make the kernel produce either, so there is no test |
| warn: a broken attribute stops parsing silently and a short value becomes 0 | Rejected. Only the kernel sends to this socket and the kernel emits aligned attributes. Code for broken input only adds branches that cannot be tested |
| warn: `recv` does not expose truncation. There is no deadline either | Rejected. The receive buffer is 64KB and the kernel cuts dump batches smaller than that. The kernel always ends a dump with `NLMSG_DONE` |
| blocker: two waiters on one auto-reset event can both get the signal | Rejected. There are three auto-reset events (console, control request, control response) and each has exactly one waiting thread. The only duplicated event is the shutdown event, which is manual-reset |
| blocker: the static buffer of `read_stdin_line` is destroyed at `exit` while the detached `[console]` may still use it | Accepted. The buffer is never destroyed |
| warn: a handle such as the value 1 decodes to fd -1, `poll` skips it and an infinite wait never returns | Accepted. A negative fd is a wait failure (`EINVAL`). A test was added |
| warn: `spec.md` FR-9 names only Wintun and IP Helper | Accepted. It now says Linux does the same with TUN and rtnetlink |
| warn: `verdict` of `adapter.firewall` in `architecture.md` chapter 9 lists only `pass` | Accepted. Linux's `skipped` is listed |
| warn: the wide-character API for record file paths also applies to Linux | Accepted. Limited to Windows |
| warn: "`unknown` if a query fails" in `linux-prereq.md` section 4 disagrees with the case where something blocks | Accepted. Now `unknown` only when nothing to block was found |
| warn: the Linux adapter lifetime table in `architecture.md` repeats rules of `linux-prereq.md` | Accepted. Cut down to point at that document's sections |

**Found outside the review.** After the fixes, the `linux_tun` lifetime test failed 5 times out of 6. It was the same without `IFF_TUN_EXCL`. A packet
was already there right after opening, which broke the "empty" assertion. We take it to be the IPv6 link-local packets the kernel sends when the device
comes up. The packet contents were not checked. The test now drains received packets before checking for the empty state. After the fix it passed all 6 times.
