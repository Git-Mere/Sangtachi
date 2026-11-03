# 0006. The first GUI run on another machine ended with `adapter.error reason=eof`

- Status: Unresolved
- Found: 2026-11-02
- Related: [`../concurrency.md`](../concurrency.md) chapter 3, [`../windows-prereq.md`](../windows-prereq.md) section 4 Distribution Packaging,
  [`../experiments.md`](../experiments.md) 3.3,
  [`../commit_history/2026-11-02/01-시연-리허설과-t7.md`](../commit_history/2026-11-02/01-시연-리허설과-t7.md)

## Symptom

On the rehearsal day, the `field-kit-p8` GUI distribution was started for the first time on the demo player's Windows machine (`C` in
`experiments.md`). After the window appeared, the startup failure dialog appeared.

```text
Stopped.
ERROR adapter.error reason=eof
```

The log is normal through `adapter.open`, `adapter.firewall verdict=pass`, and `socket.bind`, and the next line is `adapter.error reason=eof`.
There is no `attempt.start`. It was before the person pressed create or join. After that it wrote the process counters and ended with `adapter.close`.

The two later runs on the same machine were normal with the same adapter GUID. The rehearsal and T-7 were both done with those two processes. This
error has never been seen on the owner's machine `A`.

## Investigation

| Suspected | Seen | Judgement |
|-----------|------|-----------|
| The code classified another error as `eof` | `reason=eof` comes only from the one branch where `drain_wintun` receives `ERROR_HANDLE_EOF` from `WintunReceivePacket`. The same value is returned when there is no session, but the session had been opened (after `adapter.open`) | No. Wintun returned "the adapter is terminating" |
| The client reacted wrongly to that value | It signalled shutdown and cleaned up as [`concurrency.md`](../concurrency.md) chapter 3 says | No. It follows the specification |
| The distribution files were corrupt | The next two runs on the same machine with the same distribution were normal | No |

## Cause

**Confirmed.** Right after the first start, the Wintun session returned `ERROR_HANDLE_EOF`. That means the adapter was terminating at that moment.

**Unknown.** What terminated the adapter. The candidates are Windows restarting the device right after the first install, and that machine's
antivirus intervening. That machine's antivirus raised a warning when the distribution was downloaded. There is no data to tell which. That
machine's `C:\Windows\INF\setupapi.dev.log` and system event log were not examined.

## Handling

The code and the specification were not changed. Starting again is enough.

The way to look again is to find a device restart at that time in `setupapi.dev.log` and the system event log of a machine that shows the same symptom.

## Result

It happened once. That machine started normally twice after that. If it happens again in the demo, start it again.
