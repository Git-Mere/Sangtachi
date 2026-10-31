# 0003. The `linux_tun` lifetime test failed by receiving a packet right after opening

- Status: Resolved
- Found: 2026-10-31
- Related: `tests/platform/linux_tun_test.cpp`, [0004](0004-같은-이름의-영구-tun-장치에-붙었다.md), [`../commit_history/2026-10-31/02-linux-콘솔-클라이언트.md`](../commit_history/2026-10-31/02-linux-콘솔-클라이언트.md)

## Symptom

The `linux_tun` lifetime test (`[adapter]`), run with `sudo`, failed 5 times out of 6. It failed at one place.

```text
linux_tun_test.cpp:102: FAILED:
  CHECK( tun.receive().status == sangtachi::adapter::ReceiveStatus::kEmpty )
with expansion:
  0 == 1
```

Right after the device was opened, `receive()` returned a packet (`kPacket`) instead of the empty state (`kEmpty`). The same test had passed before in the worker's run and in
this session's first run.

## Investigation

| Suspected | Seen | Judgement |
|-----------|-------|------|
| `IFF_TUN_EXCL`, added just before ([0004](0004-같은-이름의-영구-tun-장치에-붙었다.md)) | Ran a build without it 6 times. All 6 failed at the same place | No |
| The test's assertion hits a race | If a packet the kernel sent after the device came up is already in the TUN, "empty right after opening" does not hold | Yes |

## Cause

**Confirmed.** Right after the device was opened, there was already a packet to read on the TUN. The test asserted the empty state at that moment.

**Inferred.** That packet is an IPv6 link-local packet (router solicitation and so on) the kernel sends when the device comes up. The host log on the same machine has
`tx_drop_not_ipv4 value=7`, so it is true that non-IPv4 packets come out of the TUN. The content of the packet received in this test was not looked at. Why it passed before
and failed this time is also unknown. It is taken as timing.

## Handling

The test was changed to drain received packets before checking the empty state. It drains at most 64 times. The client code was not changed. The client already
drops non-IPv4 packets (`tx_drop_not_ipv4`).

## Result

- The fixed test passed all 6 times with `sudo`. On `mere-galaxy`, ctest passed 74 of 75, and 1 was skipped because it needs root
- Commit `573f56a`
