# Phase 1 UDP Socket Wrapper

**Target commit:** this commit
**Phase:** 1 (core UDP networking)

The contract set by `protocol.md` chapter 6 socket ownership was moved into code. It is the
"implement the UDP socket wrapper" item of `roadmap.md` Phase 1.

## What Was Added

| Target | What |
|------|------|
| `protocol_constants.hpp` | The `protocol.md` chapter 3 constant block was moved over |
| `network::UdpSocket` | Open, `send_to`, `recv_from`, `enumerate_events`, read events |
| `open_udp_socket` | Opens by the chapter 6 contract. On failure it returns which call failed and the error code |

What chapter 6 set was moved over one by one. bind with `INADDR_ANY` and port 0, read the real port
with `getsockname`, request `SO_RCVBUF` 262144 and read the applied value with `getsockopt`,
`SO_EXCLUSIVEADDRUSE`, no `connect()`, turn `SIO_UDP_CONNRESET` off, and bind `FD_READ` with
`WSAEventSelect`.

**`recv_from` separates an oversized datagram into two paths.** The buffer is `MAX_DATAGRAM + 1`
(1473), so exactly 1473 bytes is caught by the length comparison and 1474 bytes or more is caught by
`WSAEMSGSIZE` (`architecture.md` 3.2.3). Both return `Oversize`, but the `error` value differs, so
which path it was stays distinguishable.

**A `sendto` failure is not swallowed.** Raising a counter is the caller's job (chapter 6). The
wrapper returns the failure and the error code and does not resend.

## Verification

Catch2 cases grew from 40 to 55. The socket tests open real sockets and send over loopback only.
Each case runs in a separate process, so they do not contaminate each other.

Four things that only real hardware can confirm were put in as cases.

| What | How |
|------|--------|
| `SIO_UDP_CONNRESET` | Send 4 packets to a closed port, then check that the receive is empty instead of `WSAECONNRESET` |
| The 1473 boundary | Send exactly 1473 bytes and check that it is caught by length with no error code |
| The 1474 boundary | Send 1474 bytes and check that it is caught by `WSAEMSGSIZE` |
| Event reset | Read everything, call `enumerate_events`, then wait and check that it does not return at once |

## A Defect in the Test Harness Showed Up First

One mutant appeared **as an endless wait instead of a `FAIL`.** Removing `WSAEventSelect` leaves the
socket blocking, and the `recvfrom` of the "receive from an empty socket" case never returns.
`ctest` had no time limit, so the whole harness hung.

**That is a defect of the harness, not of the mutant.** Two things were fixed.

- `TIMEOUT 30` was set on `catch_discover_tests`. That is more than a hundred times the slowest case
- A 300 second limit was put in the mutation driver, and **a hung mutant is counted as killed**

> With no limit, one hanging case holds the harness forever. Without the mutation test, this hole
> would have been met in the long tests of Phase 4.

## Mutation Testing

16 of 17 died. The review work added two guards, so three more were run.

| Killed mutants |
|------|
| Leaving `SIO_UDP_CONNRESET` on, not setting `SO_RCVBUF`, removing the length comparison, removing the `WSAEMSGSIZE` branch, removing the `WSAEWOULDBLOCK` branch, bind with a fixed port, loopback-only bind, removing `WSAEventSelect`, disabling `enumerate_events`, ignoring the `getsockname` result, leaving the handle after a move, not converting the port byte order, not converting the address byte order |

### The One Survivor Was Confirmed by Measurement as an Equivalent Mutant

The mutant that **writes the requested value as is** instead of reading the applied value with
`getsockopt` survived. At first it looked like a hole in the cases, so a case that compares against
the value asked back from the socket was added. **It still survived.**

So how Windows handles `SO_RCVBUF` was measured directly. A temporary test set eight values and read
them back at once.

| Requested | Applied | Different |
|-----:|-----:|:---:|
| 0 | 0 | No |
| 1 | 1 | No |
| 512 | 512 | No |
| 2048 | 2048 | No |
| 65536 | 65536 | No |
| 262144 | 262144 | No |
| 1073741824 | 1073741824 | No |
| 2147483647 | 2147483647 | No |

**Windows on this machine returns the requested value as is.** So "the value read from the socket"
and "the requested value written as is" never split on any input. It is not a mutant that more cases
can kill.

**The comparison case and `native_handle()` still stay.** In an environment where the values split,
that case catches it, and keeping it is also a way to write the contract into the code. The probe
test had its numbers moved here and was deleted. A test that asserts nothing is not kept in the
repo.

**`protocol.md` chapter 6 was not changed.** That sentence is a conditional statement, "the OS may
not give the requested value as is", and one machine returning it as is does not overturn that
sentence.

## Cross-Model Review

Codex was given two lenses (Winsock correctness, test strength). Of 4 findings, **3 were taken and 1
was rejected.**

| Grade | Finding | Handling |
|------|------|------|
| warn | `send_to` narrows `payload.size()` to `int`. The size can wrap | **Taken.** Anything over `INT_MAX` is returned as `WSAEMSGSIZE`. It is not clamped to `MAX_DATAGRAM`, because a test sends something larger on purpose to check the oversize path on the receiving side |
| warn | `recv_from` narrows `buffer.size()` to `int` | **Taken.** The read length is clamped to `kRecvBufferSize`. This is more than a narrowing problem. If the caller gives a larger buffer, 1474 bytes or more simply fits and the `WSAEMSGSIZE` branch never runs |
| warn | The "send where there is no route" case asserts only on failure, so it checks nothing | **Taken.** It was changed to a broadcast address. Failure is deterministic on a socket without `SO_BROADCAST`, so `WSAEACCES` can be asserted |
| warn | The "unreachable port" case does not assume that another process can take the port that was just closed | **Rejected.** It does not produce a wrong failure. If somebody took it, no ICMP comes back and the receive is still empty. All that is lost is that this run checks nothing, and the effect was confirmed by the `SIO_UDP_CONNRESET` mutant actually dying. That limit was written into the test comment |

**The second finding pointed at something bigger than narrowing.** If the buffer length is not
clamped, one of the two paths designed by 3.2.3 disappears depending on the caller's buffer size. A
case that blocks that was added with it, and a mutant that removes the clamp dies in that case.

## What Is Left

Phase 1 still has the `[loop]` event loop and `[console]`.
