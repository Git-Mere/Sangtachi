# Required tests

**이 파일은 사람이 직접 하는 실측의 인수인계다.** 아직 무엇을 재야 하고 무엇을 먼저 하는지만 적는다. 실측 데이터를 받을 때마다
고친다. 시험 절차는 [`README.md`](README.md), 보고서와 원본은 날짜 폴더(로컬), 끝난 실측의 경과는
[`docs/kor/commit_history/`](../docs/kor/commit_history/README.md) 가 갖는다. 같은 기기에서 되는 확인은 여기 적지 않는다.

## 키트

| 항목 | 값 |
|------|-----|
| 태그 | `field-kit-p8`. Windows 는 GUI 배포물(`scripts/package-gui.ps1`, [`windows-prereq.md`](../docs/kor/windows-prereq.md) 4절 Qt 런타임), Linux 는 Release 콘솔([`linux-prereq.md`](../docs/kor/linux-prereq.md) 1절)이다. 이전 태그 `field-kit-p5`, `field-kit-p7`, `field-kit-p7b` 는 남겨 둔다 |
| 왜 이 빌드인가 | 남은 Phase 8 실측은 GUI 로 돈다(T-7). 두 기기 실측에 Linux 기기를 쓴다 |
| 만드는 법 | [`README.md`](README.md) "준비" 는 `field-kit-p7b` 의 콘솔 키트를 적는다. GUI 배포물과 Linux 빌드는 위 두 절이 적는다 |
| NFR-2 | Minecraft 시험과 같은 빌드로 파일 전송 시험을 다시 돈다([`roadmap.md`](../docs/kor/roadmap.md) Phase 7 검증, [ADR 0036](../docs/kor/decisions/0036-phase-8-minecraft-검증-키트.md)). 키트를 바꾸면 그 날 Minecraft 전에 한 번 돈다 |

## 다음에 할 것

Phase 8 실측이다. 범위와 통과 조건은 [`roadmap.md`](../docs/kor/roadmap.md) Phase 8 검증이 갖는다. 절차는 아직
[`README.md`](README.md) 에 없다. 첫 시험 날 전에 쓴다. 결과는 [`experiments.md`](../docs/kor/experiments.md) 에 싣는다.

| 무엇 | 기기와 망 | 키트 | 지금 |
|------|-----------|------|------|
| 시연 망의 매핑 수명 | 시험 3. 호스트의 휴대폰 핫스팟과 시연 플레이어의 집 Wi-Fi 각각. 그때마다 보안 그룹을 그 공인 IP `/32` 로 잠시 연다 | 시험 3 의 도구 | 남았다. 플레이어의 집은 이미 잰 두 집(이 기기 집 Wi-Fi 하한 58초, 친구 집 Wi-Fi 300초 이상)이 아니다 |
| 시연 리허설 | 시연과 같은 망 쌍. 호스트는 학교에서 휴대폰 핫스팟, 플레이어는 집 Wi-Fi. 방을 만들고 참가해 Minecraft 접속까지 | 시연에 쓸 키트 | 며칠 전 한 번, 시연 당일 한 번. 실패하면 nat-probe 로 원인을 가른다 |
| 멤버 목록과 단절(T-7) | **3대.** 호스트 하나와 플레이어 둘. 플레이어 하나를 끊고 다른 플레이어의 목록을 본다. 한 기기에 두 프로세스를 띄울 수 없다. 어댑터 GUID 가 상수라 둘째가 기동에 실패한다 | `field-kit-p8` | 같이 할 사람을 찾는 중이다. 정상 종료, 강제 종료, 호스트 끊기를 한 번씩 돈다 |

시험 1·5·6 은 끝났다. 시험 6 의 6-5 는 2026-10-25 A 를 휴대폰 핫스팟에 붙여 통과했다. Minecraft 30분 연속 플레이, 게임 표시 지연과 터널 RTT 의 차이, 블록 배치와 채팅의 반영, Wi-Fi 끊김 거동은 2026-11-01 소유자의 Windows 기기와 Linux 기기로 끝났다([`experiments.md`](../docs/kor/experiments.md) 3.2 두 번째 세션).

## 때가 되면 할 것

| 무엇 | 절차 | 언제 |
|------|------|------|
| 방화벽 인바운드 | 시험 4 | ADR 0002 를 다시 열 때만 |
