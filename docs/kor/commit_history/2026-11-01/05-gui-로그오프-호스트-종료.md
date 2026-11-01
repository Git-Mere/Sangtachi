# GUI 로그오프로 호스트 종료를 쟀다

## 왜 했나

`roadmap.md` Phase 8 작업 "호스트 종료가 OS 유예 안에 드는지 잰다" 의 GUI 쪽이다. GUI 빌드는 로그오프로 `WM_ENDSESSION` 경로를 잰다.
같은 기기 시험은 콘솔 플레이어가 함께 떠 있어 결과가 섞였으므로([이슈 0005](../../issue_history/0005-콘솔-로그오프-지연-경고.md)) 플레이어를
`mere-galaxy` 에 두고 쟀다. 세 번 돌았다.

## 1회차: `CLOSE` 수신부터 정리까지

10-29 에 만든 배포물(`573f56a`, `faf06df` 를 담지 않는다)로 돌았다. 방 `T6AQP2`.

- GUI 로그 끝은 `attempt.end reason=shutdown`, `to=CLOSED`, `adapter.close` 순이다. 플레이어는 `CLOSE` 를 받아 `attempt.end reason=closed` 를 냈다
- 두 기기 시계를 같은 NTP 서버(`time.google.com`)에 대 보고 차이를 뺐다. 이 기기는 Windows Time 서비스가 꺼져 있어 약 0.55초 빨랐다. 플레이어의 `CLOSE` 수신부터 GUI 로그의 마지막 쓰기까지 약 0.11초였다
- OS 유예는 `WM_ENDSESSION` 을 받은 때부터 센다. GUI 로그에 시각이 없어 그 앞부분을 재지 못했다. Codex 리뷰가 두 번 blocker 로 짚었다. 그래서 이 회차로 닫지 않고 `session_end.cleanup` 줄을 더했다([`04-gui-로그오프-정리-시간-로그.md`](04-gui-로그오프-정리-시간-로그.md))
- 앞선 조사에서 띄운 Debug 콘솔(`--no-adapter`, 로비)이 이 기기에 남아 있다가 로그오프 때 `0x40010004` 로 끝났다. 표준 입력이 끝나도 종료하지 않는 설계다. `winsrvext` 100 은 없었다

## 2회차: `WM_ENDSESSION` 부터 정리까지

HEAD `9c94d30` 으로 `scripts/package-gui.ps1` 이 만든 배포물로 돌았다. 방 `4PH9QA`. 플레이어는 `mere-galaxy` 의 Linux 콘솔 클라이언트이고
어댑터를 붙였다. `CONNECTED` 뒤 `10.100.0.1` 로 `ping` 3/3, 평균 41ms 였다. 저장소 소유자가 시작 메뉴의 로그아웃으로 끝냈다.

| 확인 | 결과 |
|------|------|
| `session_end.cleanup` | `elapsed_ms=140`. 3초 안이다 |
| GUI 로그 끝 | `attempt.end reason=shutdown`, `to=CLOSED`, `adapter.close name=Sangtachi`, `session_end.cleanup elapsed_ms=140` 순이다 |
| 플레이어 | `CONNECTED` 에서 `CLOSED` 로 가고 `attempt.end reason=closed` 를 냈다 |
| System 로그 | 로그오프 앞뒤로 `winsrvext` 100 이 없다. Winlogon 7002 와 7001 이 있다 |
| 남은 것 | 다시 로그인한 뒤 Wintun 어댑터 0개, `10.100.0.*` 주소 0개 |

- 140ms 는 `WM_ENDSESSION` 을 받은 때부터 정리 완료 신호를 준비하기 시작한 때까지다. 그 뒤의 줄 쓰기, 화면 큐 넣기, 신호는 들지 않는다(`architecture.md` 9장 로그 출력)
- 세션은 하나였다. `roadmap.md` 는 `CLOSE` 최대 네 개를 재라고 한다. Codex 리뷰가 blocker 로 짚었다. 그래서 3회차를 돌았다

## 3회차: 플레이어 넷

같은 배포물이다. 방 `HPEGAG`. 플레이어 넷(`galaxy1`~`galaxy4`)을 모두 `mere-galaxy` 에 `--no-adapter` 로 띄웠다. 한 방의 인원
상한이 다섯이라(`MAX_PEERS`) 호스트와 플레이어 넷이 꽉 찬 방이다. 호스트 로그에 `CONNECTED` 가 넷 있었다.

| 확인 | 결과 |
|------|------|
| `session_end.cleanup` | `elapsed_ms=125`. 3초 안이다 |
| GUI 로그 끝 | `attempt.end reason=shutdown`, 세션 넷의 `to=CLOSED`, `adapter.close name=Sangtachi`, `session_end.cleanup elapsed_ms=125` 순이다 |
| 플레이어 넷 | 넷 모두 `CONNECTED` 에서 `CLOSED` 로 가고 `attempt.end reason=closed` 를 냈다. 호스트의 `CLOSE` 넷을 다 받았다 |
| System 로그 | `winsrvext` 100 이 없다 |
| 남은 것 | 다시 로그인한 뒤 Wintun 어댑터 0개, `10.100.0.*` 주소 0개 |

- 플레이어는 어댑터를 붙이지 않았다. 호스트 쪽 종료가 하는 일(`CLOSE` 송신과 호스트 어댑터 정리)은 플레이어의 어댑터 유무와 상관없다

## 무엇을 바꿨나

| 무엇 | 자리 |
|------|------|
| GUI 로그오프 측정 항목을 지우고 번호를 당겼다 | `plan.md` |
| "GUI 로그오프 때 호스트 종료 시간" 행을 지웠다 | `field-test/required-test.md` |

## 검증

| 무엇 | 결과 |
|------|------|
| 1회차 GUI 로그 `20261101T174302Z-36688.log` | 255줄. `.prime/` 에 백업했다. 배포물을 다시 만들며 `gui-dist` 가 지워졌다 |
| 2회차 GUI 로그 `gui-dist\logs\20261101T175839Z-35380.log` | 241줄. 위 표의 순서로 끝난다 |
| 3회차 GUI 로그 `gui-dist\logs\20261101T180409Z-27524.log` | 699줄. 3회차 표의 순서로 끝난다 |
| 플레이어 로그 `mere-galaxy:~/sangtachi-b/lplayer-ts.err`(1·2회차), `p1.err`~`p4.err`(3회차) | 모두 `to=CLOSED` 와 `attempt.end reason=closed` |
| 측정 뒤 `mere-galaxy` | 플레이어를 `SIGINT` 로 끝냈다. `Sangtachi` 장치와 남은 클라이언트 프로세스가 없다 |
| `docgate.py`, `--claims` | `VERDICT: pass` |

## 크로스 모델 리뷰

Codex(`gpt-6.1-sol`). 근거(A)와 한영·기록 일관성(B) 렌즈를 한 번에 돌렸다.

| 지적 | 처리 |
|------|------|
| blocker [A] (1회차 기록 초안) `CLOSE` 수신부터 정리까지는 `WM_ENDSESSION` 부터 `CLOSE` 수신까지를 빼므로 OS 유예 안에 드는지를 보이지 못한다. 두 번 나왔다 | 반영. `session_end.cleanup` 줄을 더하고(`04`) 2회차를 돌았다 |
| blocker [A] (2회차까지의 기록) `roadmap.md` 는 `CLOSE` 최대 네 개를 재라고 하는데 세션이 하나였다 | 반영. 플레이어 넷으로 3회차를 돌았다 |

3회차를 더한 전체 staged 차이를 같은 렌즈로 다시 돌렸다. nit 하나(빠진 구간을 `WM_ENDSESSION` 부터 `CLOSE` 수신까지로 적으라)를 반영했다.
