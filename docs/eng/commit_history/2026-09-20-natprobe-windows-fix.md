# 2026-09-20 Fix natprobe failures on Windows

## Changes

Fixed **2 defects that stopped `tools/nat-probe/natprobe.py` from starting on Windows**. The tool
was added in [`2026-09-18-nat-probe.md`](2026-09-18-nat-probe.md). Both showed up on a real
Windows run.

- `SIO_UDP_CONNRESET` is now set by calling `WSAIoctl` in `ws2_32.dll` directly
- Added `setup_console()`, which sets the console encoding to UTF-8 at startup
- Exits with an ASCII message when Python is older than 3.8
- Added `socket.udp_connreset_disabled` and `socket.udp_connreset_detail` to the JSON
- Added README 5.3 "When it fails on Windows", a table of symptoms and actions
- Self-checks went from 38 to 41

## Problems

**1. `ValueError: invalid ioctl command 2550136844`**

`2550136844` is `0x9800000C`, that is `SIO_UDP_CONNRESET`. **Python's `socket.ioctl()` does not
support this command.** CPython allows only `SIO_RCVALL`, `SIO_KEEPALIVE_VALS`, and
`SIO_LOOPBACK_FAST_PATH`, and rejects everything else with `ValueError`. There is no
`socket.SIO_UDP_CONNRESET` constant either, so the `getattr` fallback passed the raw value and it
was refused.

This item is required by `protocol.md` chapter 15, so it cannot be skipped. Without it,
`recvfrom` breaks with `WSAECONNRESET` after an ICMP port unreachable arrives. **Early in the
punch, the peer has not opened its port yet, so that ICMP is expected.** Reception becomes
unreliable exactly in the part that matters most.

**2. `UnicodeEncodeError: 'charmap' codec`**

The output of this tool is Korean. If the Windows console code page is cp1252 (the English
default), the first `print` kills the process. The same happens when output is redirected to a
file. It does not show up on Korean Windows (cp949).

## Decisions

**Call `WSAIoctl` directly through `ctypes`.** Python blocks this path, so it must be bypassed.
Argument types are declared explicitly. `SOCKET` is 8 bytes on 64-bit Windows, so it is passed as
`c_void_p`.

**A failure here does not stop the measurement.** The outcome is recorded in the JSON instead. The
receive path already catches `OSError` and continues, so the tool does not die. The result can
still be affected, so it is recorded in `socket.udp_connreset_disabled` and printed on screen.
**An unrecorded failure is the worst case.**

**For encoding, `errors="replace"` is the last line of defence.** Characters may be mangled, but a
30 second measurement will not die halfway. The JSON is always written correctly in UTF-8.

## Verification

- `test_natprobe.py` passes 41/41. Added 3 cases: the `SIO_UDP_CONNRESET` constant, the state
  recorded after socket creation, and the bind plus non-blocking check
- Reproduced `UnicodeEncodeError` by running the old version with `PYTHONIOENCODING=cp1252`, and
  confirmed the fixed version completes
- `probe` works on Linux, and `udp_connreset_disabled` is recorded as `null`
- **Windows execution will be confirmed at the next measurement.** This fix has not been verified
  on Windows

## Cross-model review

- Reviewer: Codex (GPT). The `WSAIoctl` path cannot be executed on Linux, so this relied on static
  review
- Result: 2 warns, **both applied, none rejected**

| Finding | Decision |
|------|------|
| `ctypes.get_last_error()` does not reliably retrieve Winsock errors, so a failure may be recorded as 0 or as an unrelated error | Applied. Dropped `use_last_error=True` and call `WSAGetLastError` directly. That option makes ctypes swap the thread last-error value around the call, which pollutes any later lookup. The function is resolved before the call, because `GetProcAddress` itself can overwrite the last error |
| On non-Windows, `udp_connreset_detail` holds a descriptive string, contradicting the contract that detail is present only when `disabled` is `false` | Applied. It is set to `None`. `disabled: null` already means not applicable |

## Remaining

The Windows measurement still has not been done. The "Remaining" section of
`2026-09-18-nat-probe.md` still applies. The measurement JSON and the 2 records are still not
committed, because the repository is public and they contain the public IPs of two households.
