# 2026-09-15 동시성 모델 확정

## 변경

설계 감사([`../../audit-history/design-audit.md`](../../audit-history/design-audit.md)) 후속 2번을 수행했다. blocker 4번(데이터 레이스)과
5번(텔레메트리가 keepalive를 막음), warn 2건을 해소한다.

- `architecture.md` 3.2 신설. 스레드 구성, 대기 방식, 루프 한 바퀴, 타이머, 상태 소유,
  텔레메트리 격리, 종료 절차
- `architecture.md` 3.1의 4스레드 그림 제거, `udp_socket` 책임에서 `WSAPoll` 제거
- 기존 3.2 제어 평면을 3.3으로 이동
- `architecture.md` 9장 텔레메트리 수집 지점과 업로드 서술을 3.2에 맞춤
- `protocol.md` 6장에서 `architecture.md` 3.2로 역참조
- `protocol.md` 11장 타이머 우선순위를 도착 시각에서 **꺼낸 시각** 기준으로 변경
- `roadmap.md` Phase 1에 이벤트 루프 골격 작업과 검증 2건 추가
- `roadmap.md` Phase 9에 텔레메트리 스레드 분리 작업과 검증 2건 추가
- `design-audit.md` 8장에 이번 검증 기록 추가

## 결정

**4스레드에서 단일 이벤트 루프로.** 초기 안은 `[main]`/`[net_rx]`/`[tun_rx]`/`[timer]`였다.
잠금을 걸어 공유하는 대신 공유하지 않는 쪽을 택했다. 최종 구성은 `[loop]`와 `[telemetry]`
두 스레드이고, Phase 1~5에만 `[console]`이 추가된다.

**`WSAPoll` 대신 `WaitForMultipleObjects`.** `WintunGetReadWaitEvent`가 Win32 이벤트 핸들을
주므로 소켓과 한 번의 대기에 섞으려면 이쪽이어야 한다. 덕분에 Wintun 읽기에 별도 스레드가
필요 없고, Phase 6에서 어댑터가 들어와도 스레드가 늘지 않는다.

**drain에 예산을 둔다.** 비어질 때까지 도는 대신 소스별 한 바퀴 64개로 끊는다. 상한이 없으면
도착 속도가 처리 속도를 넘는 순간 타이머가 아예 돌지 않아 keepalive가 멈춘다. 대가로
마감 직전 도착한 데이터그램이 다음 바퀴로 밀릴 수 있어, `protocol.md` 11장의 우선순위 규칙을
도착 시각이 아니라 꺼낸 시각 기준으로 바꿨다.

**텔레메트리 큐는 락 없는 SPSC 링이고 가득 차면 새 레코드를 버린다.** 가장 오래된 것을 버리는
쪽이 지표로는 낫지만 생산자가 소비자의 tail을 움직여야 해서 락이 필요하다. 지표 하나 더
살리자고 데이터 평면에 락을 들이지 않는다.

**콘솔 핸들러는 신호 종류에 따라 반환 시점이 다르다.** `CTRL_CLOSE_EVENT` 계열은 핸들러가
반환하는 순간 Windows가 프로세스를 죽인다. 콘솔 창을 닫는 것은 이 프로그램의 정상적인 종료
방법이므로 예외가 아니라 주 경로로 다룬다.

**정리가 `[telemetry]` join보다 먼저다.** 이 순서 덕분에 핸들러가 텔레메트리를 기다리지 않고
반환할 수 있다. join 상한 2초를 넘기면 `_exit`로 끝낸다. 상한을 실제로 강제하는 것은 소켓
타임아웃이 아니라 이 `_exit`다.

## 검증

- `WintunGetReadWaitEvent`, `WintunReceivePacket`, `WintunReleaseReceivePacket`의 시그니처와
  문서화된 오류 코드(`ERROR_NO_MORE_ITEMS`, `ERROR_HANDLE_EOF`, `ERROR_INVALID_DATA`)를
  wintun.h 원본에서 확인
- eng/kor 헤딩 수 일치 (architecture 31, protocol 43, roadmap 45, design-audit 18, spec 15)
- `experiments.md`를 제외한 상대 링크 전부가 실재 경로. 미해결은 그 하나뿐이고 양쪽 동일 (Phase 9 산출물)

## 크로스 모델 리뷰

- 리뷰어: Codex (GPT), 6라운드. 1~4차는 렌즈 교체, 5~6차는 수렴 확인
- 결과: blocker 9, warn 11, nit 2. **전부 반영, 기각 없음.** 6차에서 `LGTM - no blockers`

| 라운드 | 렌즈 | blocker | warn |
|--------|------|---------|------|
| 1 | Windows API 정확성 | 2 | 2 (+nit 1) |
| 2 | 설계 내부 정합성 | 3 | 2 |
| 3 | 수정의 부작용, 의사코드 정밀도 | 2 | 4 |
| 4 | 3차 수정이 깨뜨린 것 | 1 | 2 (+nit 1) |
| 5 | 수렴 확인 (신규 변경 지점 집중) | 1 | 1 |
| 6 | 수렴 확인 (최종) | 0 | 0 |

주요 blocker와 처리:

| 라운드 | 지적 | 반영 |
|--------|------|------|
| 1 | `WSAEventSelect` 이벤트는 수동 리셋이고 `recvfrom`은 이벤트 객체를 리셋하지 않음. `WSAEnumNetworkEvents` 없이는 첫 데이터그램 이후 루프가 스핀 | 루프에 `WSAEnumNetworkEvents` 추가. "비우는 것이 유일하게 올바른 패턴"이라는 근거를 배칭 정책으로 정정 |
| 1 | `CTRL_CLOSE`/`LOGOFF`/`SHUTDOWN`은 핸들러 반환 시 프로세스가 죽으므로 신호만 하고 돌아오면 정리가 실행되지 않음 | 신호 종류별 반환 시점 분리. 정리 완료 이벤트를 3초까지 대기 후 반환 |
| 2 | drain 루프에 상한이 없어 고부하에서 타이머가 굶음 | `MAX_DRAIN` 64 + `busy` 재진입 |
| 2 | Phase 1~5에 어댑터가 없는데 `drain_wintun` 무조건 호출. 콘솔 이벤트를 아무도 처리하지 않아 스핀 | 세션 NULL 가드, `drain_console` 신설, 자동 리셋 이벤트 |
| 2 | 뮤텍스 큐로는 "블록하지 않는다"를 보장 못 해 roadmap 기준이 통과 불가 | 락 없는 SPSC 링으로 교체 |
| 3 | `next_timeout`이 `now()`를 두 번 불러 그 사이 시계가 마감을 지나면 언더플로. 타임아웃 49.7일 | `t = now()` 한 번만 읽기 |
| 3 | `ERROR_INVALID_DATA` 분기에 반환이 없어 `NULL` 포인터가 `route_and_send`로 흘러감 | `RESTART` 반환값 도입, 세션 재생성과 대기 배열 재구성을 `[loop]`가 한 묶음으로 처리 |
| 4 | drain 예산이 `protocol.md` 11장의 "마감 이전 도착 우선" 규칙과 모순 | 11장을 꺼낸 시각 기준으로 재정의하고 근거 명시 |
| 5 | 11장은 꺼낸 시각으로 바꿨는데 `architecture.md` 3.2.4에 도착 시각 서술이 남아 두 문서가 다른 규칙을 말함 | 3.2.4 동기화 |

주요 warn과 처리:

| 라운드 | 지적 | 반영 |
|--------|------|------|
| 1 | 핸들 배열에 빈자리를 `NULL`로 채우면 `WAIT_FAILED` | 표의 번호는 논리적 순위이고 실제 배열은 조밀하게 구성함을 명시 |
| 1 | `GetTickCount64` 뺄셈이 언더플로하면 `DWORD` 변환 결과가 `INFINITE`에 착지 | `next_timeout` 의사코드에 비교 후 뺄셈, `INFINITE - 1` 클램프 |
| 2 | 업로드가 매달리면 join 상한 후 정리 완료가 영영 신호되지 않음 | 정리를 join보다 앞에 두고 소켓 타임아웃과 `_exit` 경로 명시 |
| 2 | "`[loop]`는 공유 상태 없음"이 큐와 이벤트를 무시한 서술 | "터널 상태 없음, 큐와 종료/정리 이벤트만 공유"로 정정 |
| 3 | `0..budget`은 `budget + 1`회 반복 | `budget회 반복`으로 표기 변경 |
| 3 | `WintunReleaseReceivePacket`이 `route_and_send` 뒤에 있어 조기 반환 시 링 패킷 누수 | 스코프 가드로 변경 |
| 3 | 종료 표식을 유실 허용 큐에 넣으면 가득 찬 순간 버려짐 | 표식 제거. 종료는 큐 밖 이벤트로만 전달 |
| 3 | `WAIT_FAILED`가 정리를 건너뛰고 바로 종료 | `shutdown()`을 거치도록 변경 |
| 4 | 3초 소켓 타임아웃이 2초 join 상한을 넘김 | 1.5초로 조정하고 상한을 강제하는 것이 `_exit`임을 명시 |
| 4 | 어댑터 재생성 실패 경로 미정의 | 실패 시 `shutdown()`. 조용히 어댑터 없이 도는 것이 더 위험한 이유 명시 |
| 5 | 재생성 실패 근거가 "`NULL` 자리 때문에 `WAIT_FAILED`"인데 조밀 배열 규칙과 모순 | 근거를 "대기는 정상 동작하므로 터널이 죽은 채 살아 있는 척한다"로 교체 |

## 남은 후속

`design-audit.md` 5장 기준 3~6번. 5번(직접 연결 불가 대비책)이 학기 리스크가 가장 크다.
