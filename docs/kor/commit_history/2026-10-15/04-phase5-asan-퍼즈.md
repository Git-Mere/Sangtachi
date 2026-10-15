# Phase 5 ASan 시험 빌드와 퍼즈 두 단계

## 왜 했나

`plan.md` 의 Phase 5 다음 일이다. 퍼즈 시험의 "메모리 오류 없음" 을 ASan 빌드가 판정한다(ADR 0025, `roadmap.md` Phase 5).

## 무엇을 바꿨나

| 무엇 | 자리 |
|------|------|
| CMake 선택지 `SANGTACHI_ASAN`. 켜면 모든 대상(Catch2 포함)에 `/fsanitize=address`, Debug 기본 플래그의 `/RTC` 를 빼고 `/INCREMENTAL:NO` | `CMakeLists.txt` |
| `-Asan` 스위치. 빌드 디렉터리는 `<Config>-asan` 으로 따로 둔다. 켜고 끄는 값을 늘 명시해 캐시가 조용히 이기지 않게 했다 | `scripts/build.ps1` |
| `-Asan` 스위치. 그 빌드로 단위 시험만 돈다. 프로세스 단위 스크립트는 일반 빌드가 이미 본다 | `scripts/test.ps1` |
| 퍼즈 1단계. 고정 seed 의 1~2000바이트 무작위 데이터그램 10만 개를 루프백 소켓으로 보내 루프가 받게 한다. 32개씩 보내고 전부 처리된 것을 센 뒤 다음을 보낸다. 여덟에 하나는 STUN 분류 자리(상위 2비트, magic cookie)를 맞춘다 | `tests/tunnel/tunnel_test.cpp` |
| 퍼즈 2단계. magic·version·`peer_id`·고정 epoch 를 맞춘 헤더 뒤를 무작위로 채운 10만 개를 수신 주입 진입점으로 넣는다. `type`, 본문 길이, `payload_length`, `sequence` 도 흔든다. `DATA` 는 반을 그럴듯한 내부 IPv4 로 채운 뒤 몇 바이트를 흔든다. 세션이 `CLOSE` 로 끝나면 새 epoch 로 다시 세운다 | 같은 파일 |
| `scripts/test.ps1 -Asan` 을 가리키는 한 줄 | `roadmap.md` Phase 5 작업 |
| 다음 할 일에서 이 항목을 지웠다 | `plan.md` |

## 중요한 결정

- 퍼즈 시험은 일반 빌드에서도 돈다. 그때는 크래시만 보고, 메모리 오류 판정은 ASan 빌드가 한다
- 1단계는 roadmap 대로 소켓으로 넣는다. 커널이 버리지 않게 묶음 크기(32 × 2000바이트)를 `SO_RCVBUF` 256KB 안에 두고, 묶음마다 받은 수
  (`drop_oversize_datagram` + `drop_bad_source` + `drop_unclassified` + 터널 갈래 + STUN 갈래)가 보낸 수와 같은지 본다. 대기가 무한이 되지
  않게 세션 마감을 늘 지난 것으로 둔다
- 무작위 바이트가 STUN 분류에 닿을 확률은 2^-34 라 처음 실행에서 STUN 갈래가 0 이었다. 여덟에 하나를 그 자리에 맞추게 고쳤다.
  roadmap 1단계가 "분류" 를 보므로 분류의 세 갈래를 다 밟게 했다
- 2단계가 파이프라인을 밟았는지는 카운터로 본다. `drop_magic` 이 0 이고, 8.1 의 세 사유와 세션 표의 8.3 둘, 8.4 둘, `drop_no_sink`,
  `drop_bad_reason`, `drop_unmatched_pong`, `ROSTER` 구조의 `drop_type_length` 가 0 보다 커야 한다
- seed, 개수, 길이 범위는 시작할 때 표준 오류에 한 줄로 낸다. 크래시해도 ctest 출력에 남는다

## 검증

| 무엇 | 결과 |
|------|------|
| `pwsh -NoProfile -File scripts/test.ps1 -Asan` | 608/608 통과. 부하 아래 퍼즈 1단계 11.6초, 2단계 12.1초(ctest 케이스 상한 30초) |
| `pwsh -NoProfile -File scripts/test.ps1` (DynamoDB local 포함) | `all tests passed` |
| ASan 이 실제로 잡는가 | 터널 수신 입구에 정확한 크기의 힙 복사본을 만들고 한 바이트 뒤를 읽게 했다. 일반 빌드의 퍼즈 2단계는 통과했고 ASan 빌드는 `heap-buffer-overflow` 를 그 줄로 보고하며 실패했다. 되돌렸다 |
| ASan 빌드가 정말 계측하는가 | 그 빌드의 `build.ninja` 에 `/fsanitize=address` 가 185번, `RTC1` 이 0번, `/INCREMENTAL:NO` 가 있다 |
| `docgate.py`, `--claims` | `VERDICT: pass` |

## 크로스 모델 리뷰

Codex. ASan 선택지의 플래그 처리와 캐시, 퍼즈 두 단계가 roadmap 을 그대로 따르는가, 1단계 대기가 멈추거나 헛통과할 수 있는가,
도달 단정의 뜻, 재현성을 렌즈로 줬다. `LGTM - no blockers`.
