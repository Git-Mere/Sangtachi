# 집에서 두 기기로 Minecraft 실측을 했다

## 왜 했나

`required-test.md` 의 Phase 8 실측 가운데 두 기기로 되는 것을 저장소 소유자 혼자 돌았다. Windows 기기(A)가 플레이어, Linux 기기
`mere-galaxy`(G)가 호스트다. 소유자가 Minecraft 계정을 하나 더 구해 G 에서도 클라이언트를 돌릴 수 있게 됐다.

## 결과

결과와 한계는 [`experiments.md`](../../experiments.md) 3.2 두 번째 세션이 갖는다. 요약이다.

| 항목 | 결과 |
|------|------|
| 파일 전송(NFR-2) | 통과. 16 MiB, SHA-256 일치 |
| 30분 연속 플레이 | 통과 |
| 게임 표시 지연과 터널 RTT | 통과. 중앙값 72ms 대 68.2ms. 짝지은 차이의 중앙값 +3.3ms |
| 블록 배치와 채팅 | 통과. 소유자 판정 |
| Wi-Fi 끊김 | 기록. 10초는 세션과 게임이 유지됐다. 60초는 게임이 약 20초에, 터널이 약 50초에 끊기고 플레이어는 로비로 갔다 |

## 무엇을 바꿨나

| 무엇 | 자리 |
|------|------|
| Minecraft 검증 키트를 `field-kit-p8` 로 바꾸고 NFR-2 는 파일 전송을 다시 돌아 지킨다. ADR 0034 의 작업 순서를 뒤집는다 | [ADR 0036](../../decisions/0036-phase-8-minecraft-검증-키트.md) |
| 두 번째 세션의 결과와 한계, 기기 `G` 의 표기 | `experiments.md` 1장, 3.2 |
| 키트 표를 `field-kit-p8` 로, 끝난 네 행을 지웠다 | `field-test/required-test.md` |
| `mere-galaxy` 의 Wi-Fi 절전은 다른 망에 붙어도 켜진다. Linux 키트 빌드의 자리, 화이트리스트의 두 번째 계정 | `plan.md` |
| 태그 `field-kit-p8` 을 `3b92ced` 에 달았다. 로컬에만 있다 | git |

## 어떻게 쟀나

- 망: G 는 소유자의 휴대폰 핫스팟, A 는 집 Wi-Fi 다. 두 기기의 공인 주소가 다르고 양쪽 모두 NAT 뒤다
- 빌드: G 는 `3b92ced` 의 Linux Release(`~/sangtachi-head`), A 는 `9c94d30` 으로 만든 GUI 배포물이다. 두 커밋의 차이는 Linux 플랫폼 파일 하나다
- 파일 전송: A 에서 `tools/transfer/Test-Transfer.ps1 -Connect 10.100.0.1 -GenerateMiB 16`. G 는 같은 와이어의 Python 수신기를 임시로 썼다. 휴대폰 데이터를 아끼려고 64 MiB 대신 16 MiB 를 보냈다. 판정은 해시 일치라 크기와 상관없다
- 게임 표시 지연: A 의 F3 에서 `F3+3` 의 Ping 그래프를 켜고 6~13초 간격으로 21장 캡처했다. 각 장의 avg 를 읽었다. 처음 찍은 8장은 그래프가 꺼진 화면이라 버렸다(`21 tx, 507 rx` 는 초당 패킷 수다). 캡처는 값을 옮긴 뒤 지웠다
- 터널 RTT: G 의 로그 줄마다 받은 시각을 붙여 남기고, 캡처 시각 구간의 `session.rtt` 를 잘랐다. A 의 로그에는 시각이 없다
- Wi-Fi 끊김: A 에서 스크립트가 `netsh wlan disconnect` 와 `netsh wlan connect` 를 정한 시간에 부르고, 터널과 인터넷 `ping`, GUI 로그 새 줄을 0.05초 단위로 남겼다. G 는 `ping -D -i 0.5` 를 남겼다. 두 시계는 SNTP 로 맞췄다(A 가 약 0.6초 빠르다)

## 작업 중에 생긴 일

- 새 계정의 이름으로 받은 `MERE7410` 은 Mojang 에 없었다. 거절된 접속의 서버 로그에서 실제 Java 프로필 이름 `Mere_galaxy` 를 얻었다
- G 를 핫스팟으로 옮기자 Wi-Fi 절전이 다시 켜져 있었다. 다시 껐다
- ssh 명령줄에 `pkill -f` 나 `pgrep -f` 의 패턴을 넣으면 그 명령줄을 실행하는 셸도 맞아 ssh 가 먼저 끊겼다. 두 번 겪었다. 그 뒤로는 스크립트 파일로 보냈다
- 정리 때 G 의 표준 오류 사본(`host.err`, 줄에 시각을 붙인 것)이 `attempt.end reason=shutdown` 뒤 카운터 몇 줄에서 끊겼다. 시각을 붙이던 파이프를 정리하며 같이 끝낸 탓이다. 클라이언트 자신의 로그 파일은 `adapter.close` 까지 남았고 `Sangtachi` 장치는 사라졌다
- 파일 전송 동안 A 가 `socket.error op=sendto code=10035` 를 265번 냈다. `experiments.md` 3.2 의 한계에 적었다

## 검증

| 무엇 | 결과 |
|------|------|
| 원본 | A 의 GUI 로그 `20261101T182927Z-22896.log` 와 기록 파일, G 의 `~/sangtachi-head/host.err` 와 기록 파일, `~/mc-server/server.out`, Wi-Fi 끊김 스크립트 로그. 커밋하지 않는다 |
| 수치 | 중앙값과 사분위수는 numpy 의 선형 보간으로 구했다 |
| `docgate.py`, `--claims` | `VERDICT: pass` |

## 크로스 모델 리뷰

Codex(`gpt-6.1-sol`). 근거·계약(A)과 한영·기록 일관성(B) 렌즈를 한 번에 돌렸다. warn 둘을 받아들였다.

| 지적 | 처리 |
|------|------|
| [B] ADR 0036 은 두 빌드를 같은 커밋으로 만든다고 정의하는데 실측의 Windows 는 `9c94d30`, Linux 는 `3b92ced` 다 | 반영. 정의를 "두 빌드에 드는 원본이 태그 커밋과 같다" 로 고치고 이번 Windows 배포물의 차이를 적었다. 파일 전송과 Minecraft 가 같은 두 실행 파일이었다는 것도 적었다 |
| [A] `roadmap.md` 는 차이의 중앙값과 사분위 범위를 실으라고 하는데 두 분포와 중앙값의 차이만 실었다 | 반영. 판독마다 시각이 가장 가까운 터널 표본과 짝지어 차이의 중앙값 +3.3ms, 사분위 범위 +0.6~+8.1ms 를 실었다. 판정은 그대로 중앙값의 비교다 |

고친 뒤 전체 staged 차이를 같은 렌즈로 다시 돌렸다.
