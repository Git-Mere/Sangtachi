# 0006. 다른 기기의 첫 GUI 실행이 `adapter.error reason=eof` 로 끝났다

- 상태: 미해결
- 발견: 2026-11-02
- 관련: [`../concurrency.md`](../concurrency.md) 3장, [`../windows-prereq.md`](../windows-prereq.md) 4절 배포물 패키징,
  [`../experiments.md`](../experiments.md) 3.3,
  [`../commit_history/2026-11-02/01-시연-리허설과-t7.md`](../commit_history/2026-11-02/01-시연-리허설과-t7.md)

## 증상

시연 리허설 날 시연 플레이어의 Windows 기기(`experiments.md` 의 `C`)에서 `field-kit-p8` GUI 배포물을 처음 띄웠다. 창이 뜬 뒤
기동 실패 대화 상자가 떴다.

```text
Stopped.
ERROR adapter.error reason=eof
```

로그는 `adapter.open`, `adapter.firewall verdict=pass`, `socket.bind` 까지 정상이고 그다음 줄이 `adapter.error reason=eof` 다.
`attempt.start` 가 없다. 사람이 방 만들기나 참가를 누르기 전이다. 그 뒤 프로세스 카운터를 내고 `adapter.close` 로 끝났다.

같은 기기에서 다시 띄운 두 번은 같은 어댑터 GUID 로 정상이었다. 리허설과 T-7 을 모두 그 두 프로세스로 했다. 소유자의 기기 `A` 에서는
이 오류를 본 적이 없다.

## 조사

| 의심한 것 | 본 것 | 판단 |
|-----------|-------|------|
| 코드가 다른 오류를 `eof` 로 잘못 분류했다 | `reason=eof` 는 `drain_wintun` 이 `WintunReceivePacket` 의 `ERROR_HANDLE_EOF` 를 받은 분기 하나에서만 나온다. 세션이 없을 때도 같은 값을 내지만 세션은 열린 뒤였다(`adapter.open` 이후) | 아니다. Wintun 이 "어댑터가 끝나는 중" 을 돌려줬다 |
| 클라이언트가 그 값에 잘못 반응했다 | [`concurrency.md`](../concurrency.md) 3장대로 종료를 신호하고 정리했다 | 아니다. 사양대로다 |
| 배포물 파일이 깨졌다 | 같은 기기에서 같은 배포물로 그다음 두 번이 정상이었다 | 아니다 |

## 원인

**확인한 것.** 첫 기동 직후 Wintun 세션이 `ERROR_HANDLE_EOF` 를 돌려줬다. 그 어댑터가 그때 끝나고 있었다는 뜻이다.

**모르는 것.** 무엇이 어댑터를 끝냈는지다. 후보는 첫 설치 직후 Windows 가 장치를 다시 시작한 것, 그 기기의 백신이 개입한 것이다.
그 기기의 백신은 배포물을 받을 때 경고를 냈다. 어느 쪽인지 가를 자료가 없다. 그 기기의 `C:\Windows\INF\setupapi.dev.log` 와
시스템 이벤트 로그를 보지 않았다.

## 처리

코드와 사양은 바꾸지 않았다. 다시 띄우면 된다.

다시 볼 때 쓸 길은 같은 증상이 난 기기의 `setupapi.dev.log` 와 시스템 이벤트 로그에서 그 시각의 장치 다시 시작을 찾는 것이다.

## 결과

한 번 났다. 그 기기는 그 뒤 두 번 정상으로 기동했다. 시연에서 다시 나면 다시 띄운다.
