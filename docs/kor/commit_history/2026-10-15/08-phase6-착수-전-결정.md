# Phase 6 착수 전 결정 (ADR 0026)

## 왜 했나

`roadmap.md` Phase 6 이 착수 전에 정하라고 한 셋을 닫았다. Wintun 배포물 기대값을 둘 자리, 대체 대역을 넣을지, Wintun 주입
실패가 나오는 모습이다. 선택지를 저장소 소유자에게 보였고 소유자가 추천안대로 골랐다.

## 무엇을 바꿨나

| 무엇 | 자리 |
|------|------|
| Wintun 0.14.1 의 버전, 주소, zip 해시, DLL 해시, `Subject` 를 담은 기대값 파일. 받아서 비교하는 빌드 단계는 아직 없다 | `cmake/Wintun.cmake` |
| 기대값의 자리, 판정 2번을 문자열 전체 비교로, 판정 3번을 DLL 기대값 비교로. 공개 해시가 zip 의 것이라는 사실, 인증서 만료를 판정하지 않는 이유, 0.14.1 로 세 판정을 돌린 결과 | `windows-prereq.md` 4절 |
| 대체 대역이 없고 중단이 v1 의 최종 동작이라는 것과 그 한계. `pick another range` 의 조치 문구 | `windows-prereq.md` 7절 |
| `inject` 의사 코드, 확인한 Wintun API 모양, `drop_inject_ring_full`, 주입 쪽 `ERROR_HANDLE_EOF`. "확인한 뒤에 나눈다" 단락을 지웠다 | `concurrency.md` 3장 |
| 착수 전 셋을 지우고 배포물 받기 작업과 그 검증을 넣었다. 주입 작업이 3장을 가리킨다 | `roadmap.md` Phase 6 |
| 링을 채운 시험이 보는 카운터를 `drop_inject_ring_full` 로 | `roadmap.md` Phase 7 |
| 카운터 `drop_inject_ring_full`. 개수 55 -> 56 | `counters.hpp`, `counters.cpp`, `counters_test.cpp` |
| 결정과 버린 대안 | ADR 0026 |
| 다음 할 일을 배포물 받기와 어댑터 작업으로. `-AllowShadow` 를 클라이언트에 열지의 자리 | `plan.md` |

## 중요한 결정

결정과 버린 대안은 ADR 0026 이다. 이 기록에는 그 밖의 판단만 적는다.

- **판정 3번은 원래 문구로는 맞을 수 없었다.** 공식 사이트가 공개하는 해시는 zip 의 것인데 4절은 DLL 해시를 그 값과
  비교하라고 했다. 선택지를 정리하려고 배포물을 실제로 받아 보다가 드러났다
- 기대값 파일을 지금 만들었다. 받기 단계는 Phase 6 작업이지만, 확인한 값을 둘 곳이 없으면 이 기록에만 남는다. 4절이 그
  파일을 가리킬 수 있게 하려는 것이기도 하다
- 카운터를 코드에도 지금 넣었다. `counters.hpp` 는 문서가 이름 지은 카운터를 아직 아무도 올리지 않아도 전부 들고 있다
- `-AllowShadow` 를 클라이언트에 열지는 정하지 않았다. 소유자에게 보인 선택지 밖이라 `plan.md` 에 자리만 두었다

## 확인한 사실

- `https://www.wintun.net/builds/wintun-0.14.1.zip` 의 SHA-256 이 사이트 공개값과 같다
- `bin/amd64/wintun.dll` 의 SHA-256 은 `e5da8447dc2c320edc0fc52fa01885c103de8c118481f683643cacc3220dafce` 다
- 서명 `Status` 는 `Valid`, 인증서 만료일은 2021-12-14, 발급자는 DigiCert EV Code Signing CA (SHA2) 다
- `Subject` 문자열은 PowerShell 7.6 의 `Get-AuthenticodeSignature` 와 Windows PowerShell 5.1 의
  `X509Certificate.CreateFromSignedFile` 이 같은 값을 냈다. 5.1 의 `Get-AuthenticodeSignature` 는 pwsh 에서 띄운 환경에서
  모듈 적재가 실패해 비교하지 못했다
- `wintun.h` 와 0.14.1 의 `api/session.c` 에서 `WintunAllocateSendPacket` 은 `ERROR_HANDLE_EOF` 와 `ERROR_BUFFER_OVERFLOW`
  만 내고, `PacketSize` 상한을 검사하지 않는다. `WintunSendPacket` 은 `VOID` 다

## 검증

- `python tools/docgate/docgate.py` 와 `--claims` 둘 다 `VERDICT: pass`
- `pwsh -NoProfile -File scripts/test.ps1` 통과. ctest 613건과 cli, e2e, lobby, session 확인이 모두 통과했다
  (DynamoDB local 을 띄워 돌렸다)

## 크로스 모델 리뷰

Codex 로 staged diff 를 두 번 보였다. 한 번은 기본 렌즈에 미러 일치, 근거 없는 주장, roadmap 에서 지운 의무의 행방,
Wintun API 주장을 더했다. 한 번은 결함이 있다고 가정하고 낡은 문장, 기대값 파일, 카운터 순서와 개수를 보게 했다. 두 번 다
`LGTM - no blockers` 였다. 반영하거나 기각한 지적은 없다.

## 세션 밖 변경

같은 세션에서 `CLAUDE.md` 에 Prime Agent 셸 경로 절을 넣었다. 저장소 소유자의 지시다. 이 커밋과 따로 커밋한다.
