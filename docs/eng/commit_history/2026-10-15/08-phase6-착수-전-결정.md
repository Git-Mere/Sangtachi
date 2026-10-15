# Decisions before Phase 6 (ADR 0026)

## Why

Closed the three items `roadmap.md` Phase 6 required before starting: where the Wintun distribution's expected values live,
whether to add an alternative range, and how Wintun inject failures appear. The options were shown to the repository owner, who
picked the recommended ones.

## What changed

| What | Where |
|------|------|
| Expected-values file for Wintun 0.14.1 holding the version, URL, zip hash, DLL hash and `Subject`. The build step that downloads and compares does not exist yet | `cmake/Wintun.cmake` |
| Where the expected values live, criterion 2 as a whole-string comparison, criterion 3 against the expected DLL hash. The fact that the published hash is the zip's, why certificate expiry is not judged, the result of running the three criteria on 0.14.1 | `windows-prereq.md` 4 |
| No alternative range, stopping is the final v1 behaviour, and its limit. The action text of `pick another range` | `windows-prereq.md` 7 |
| `inject` pseudocode, the confirmed Wintun API shape, `drop_inject_ring_full`, inject-side `ERROR_HANDLE_EOF`. Removed the "split after confirmation" paragraph | `concurrency.md` Chapter 3 |
| Removed the three pre-start items, added the distribution download task and its verification. The inject task points to Chapter 3 | `roadmap.md` Phase 6 |
| The counter the ring-full test checks is now `drop_inject_ring_full` | `roadmap.md` Phase 7 |
| Counter `drop_inject_ring_full`. Count 55 -> 56 | `counters.hpp`, `counters.cpp`, `counters_test.cpp` |
| Decisions and rejected alternatives | ADR 0026 |
| Next work is the distribution download and the adapter work. A slot for whether to expose `-AllowShadow` in the client | `plan.md` |

## Key decisions

The decisions and rejected alternatives are in ADR 0026. This record only adds other judgements.

- **Criterion 3 could never hold as written.** The hash the official site publishes is the zip's, but section 4 said to compare the
  DLL hash against it. This surfaced while downloading the distribution to lay out the options
- The expected-values file was created now. The download step is Phase 6 work, but without a place for the confirmed values they
  would live only in this record. It also lets section 4 point to the file
- The counter was added to the code now as well. `counters.hpp` holds every counter the documents name, even ones nothing
  increments yet
- Whether to expose `-AllowShadow` in the client was not decided. It was outside the options shown to the owner, so only a slot was
  added to `plan.md`

## Confirmed facts

- The SHA-256 of `https://www.wintun.net/builds/wintun-0.14.1.zip` equals the value published on the site
- The SHA-256 of `bin/amd64/wintun.dll` is `e5da8447dc2c320edc0fc52fa01885c103de8c118481f683643cacc3220dafce`
- Signature `Status` is `Valid`, the certificate expired on 2021-12-14, and the issuer is DigiCert EV Code Signing CA (SHA2)
- PowerShell 7.6's `Get-AuthenticodeSignature` and Windows PowerShell 5.1's `X509Certificate.CreateFromSignedFile` produced the same
  `Subject` string. 5.1's `Get-AuthenticodeSignature` could not be compared because its module failed to load in an environment
  launched from pwsh
- In `wintun.h` and 0.14.1's `api/session.c`, `WintunAllocateSendPacket` produces only `ERROR_HANDLE_EOF` and
  `ERROR_BUFFER_OVERFLOW` and does not check the `PacketSize` limit. `WintunSendPacket` is `VOID`

## Verification

- `python tools/docgate/docgate.py` and `--claims` both `VERDICT: pass`
- `pwsh -NoProfile -File scripts/test.ps1` passed. All 613 ctest cases and the cli, e2e, lobby and session checks passed
  (run with DynamoDB local up)

## Cross-model review

The staged diff was shown to Codex twice. Once with the default lenses plus mirror agreement, unsupported claims, where obligations
removed from roadmap went, and the Wintun API claims. Once assuming a defect exists, looking at stale sentences, the expected-values
file, and counter order and count. Both returned `LGTM - no blockers`. No findings were accepted or rejected.

## Change outside this commit

In the same session a Prime Agent shell path section was added to `CLAUDE.md`, at the repository owner's direction. It is committed
separately.
