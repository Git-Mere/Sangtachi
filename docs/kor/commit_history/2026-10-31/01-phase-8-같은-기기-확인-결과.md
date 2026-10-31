# Phase 8 같은 기기 확인 결과, plan 정리

## 왜 했나

`plan.md` 의 Phase 8 "다음에 할 일" 가운데 사람이 같은 기기에서 보는 확인과 호스트 종료 측정을 저장소 소유자가 돌렸다.
결과를 로그로 대조하고 남은 일을 다시 적었다.

## 결과

빌드는 HEAD `8366bab` 의 GUI 배포물(`scripts/package-gui.ps1`)과 같은 설정의 Release 콘솔이다. 둘 다 서버 주소와 관리자 권한
매니페스트를 넣었다.

| 확인 | 결과 | 근거 |
|------|------|------|
| GUI 기동, 방 만들기, 참가, 방 나가기, 창 닫기 | 통과 | 소유자가 화면을 봤다. 로그에 `create_room ok=true`, 콘솔 플레이어(`--no-adapter`)의 `CONNECTED`, 두 멤버가 든 명부, 호스트 `reason=leave` 와 플레이어 `reason=closed` 가 같은 시각, `adapter.close` 가 있다 |
| 콘솔 로비(`> `, 안내, `help`, `name x`, `quit`) | 통과 | 소유자가 화면을 봤다. 로그에 `lobby.name name=x` 가 있다. 첫 실행에는 이 줄이 없어 다시 돌렸다 |
| Qt 런타임 | 판정 못 함 | 친구 PC 에서 GUI 배포물이 떴다. 그 PC 에 Qt 와 VC++ 런타임이 없었는지는 확인하지 않았다. VC++ 런타임은 설치돼 있었을 가능성이 높다 |
| 콘솔 창 닫기 종료 | 통과 | 플레이어가 `CONNECTED` 인 채 호스트 창을 닫았다. 플레이어가 `CLOSE` 를 받았고(`reason=closed`), 호스트 로그의 `adapter.close` 가 그 약 0.12초 뒤에 쓰였다. 소유자가 직접 잰 종료는 거의 1초 안이다 |
| GUI 로그오프 종료 | 판정 못 함 | 아래 |

첫 회차의 콘솔 창 닫기는 플레이어 없이 돌아 `CLOSE` 를 보내지 않았다. 그 사이 어댑터를 쓰는 콘솔을 하나 더 띄웠다가
`WintunCreateAdapter` 가 `0x800700B7` 로 실패했다. 같은 기기에 어댑터를 쓰는 프로세스는 하나뿐이다(`field-test/required-test.md`
T-7 행). 첫 회차의 "로그오프" 는 화면 잠금이었다. `explorer.exe` 가 그대로 살아 있었다.

**GUI 로그오프.** 두 번째 회차에는 실제 로그오프가 있었다(User Profile Service 의 세션 1 로그오프와 세션 2 로그온). GUI 호스트는
`CLOSE` 를 보냈고 `adapter.close` 가 그 약 0.14초 뒤에 쓰였다. 그러나 같은 기기의 콘솔 플레이어가 로그오프 지연 경고를 남겼다.

- System 로그 `winsrvext` 100: `sangtachi_client.exe is delaying system shutdown after 5000 milliseconds`
- 그 플레이어 로그에는 `attempt.end reason=shutdown` 이 없다. 세션은 호스트의 `CLOSE` 로 끝났다
- 로그 줄에 시각이 없어 `WM_ENDSESSION` 이 언제 왔는지 모른다. 두 프로세스에 신호가 간 순서도 모른다

그래서 이 회차로 GUI 로그오프 종료를 판정하지 않는다. 플레이어를 다른 기기에 두고 다시 잰다. 콘솔이 `CTRL_LOGOFF_EVENT` 를
[`concurrency.md`](../../concurrency.md) 7장 종료대로 처리했는지는 따로 조사한다. 2026-10-01 에도 Debug 빌드 콘솔이 같은
이벤트를 남겼다. 원인은 모른다.

## 무엇을 바꿨나

| 무엇 | 자리 |
|------|------|
| 끝난 같은 기기 확인과 콘솔 종료 측정을 빼고, Qt 런타임의 남은 조건, GUI 로그오프 재측정, 콘솔 로그오프 조사를 남은 일로 | `plan.md` |
| GUI 로그오프 때 호스트 종료 시간을 두 기기 실측으로 | `field-test/required-test.md` |

로그 원본은 빌드 폴더(`gui-dist\logs`, `latest_build\logs`)에 있다. 커밋하지 않는다.

## 검증

| 무엇 | 결과 |
|------|------|
| 클라이언트 로그, 기록 파일, Windows 이벤트 로그 대조 | 위 표 |
| `docgate.py` | `VERDICT: pass` |

## 크로스 모델 리뷰

Codex(`gpt-6.1-sol`). 인수인계·기록 일관성 렌즈와 근거·한영 미러 렌즈를 한 번에 돌렸다. warn 둘을 받아들였다.

| 지적 | 처리 |
|------|------|
| `plan.md` 의 "Qt 가 없는 PC 는 봤다" 는 근거가 없고 `windows-prereq.md` 4절과 맞지 않는다 | 반영. 친구 PC 에 Qt 와 VC++ 런타임이 없었는지는 확인하지 않았다고 고쳤다. `windows-prereq.md` 는 그대로 둔다 |
| `winsrvext` 100 은 5000ms 를 넘겨 늦추고 있다는 경고이지 총 지연이 5초라는 근거가 아니다 | 반영. `required-test.md` 와 이 기록의 "5초 늦췄다" 를 "지연 경고를 남겼다" 로 고쳤다 |
