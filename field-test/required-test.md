# Required tests

**이 파일은 사람이 직접 하는 실측의 인수인계다.** 아직 무엇을 재야 하고 무엇을 먼저 하는지만 적는다. 실측 데이터를 받을 때마다
고친다. 시험 절차는 [`README.md`](README.md), 보고서와 원본은 날짜 폴더(로컬), 끝난 실측의 경과는
[`docs/kor/commit_history/`](../docs/kor/commit_history/README.md) 가 갖는다. 같은 기기에서 되는 확인은 여기 적지 않는다.

## 키트

| 항목 | 값 |
|------|-----|
| 태그 | `field-kit-p8`. Windows 는 GUI 배포물(`scripts/package-gui.ps1`, [`windows-prereq.md`](../docs/kor/windows-prereq.md) 4절 Qt 런타임), Linux 는 Release 콘솔([`linux-prereq.md`](../docs/kor/linux-prereq.md) 1절)이다. 이전 태그 `field-kit-p5`, `field-kit-p7`, `field-kit-p7b` 는 남겨 둔다 |
| 왜 이 빌드인가 | 남은 T-7 실측은 GUI 로 돈다. 시연과 두 기기 실측의 호스트로 Linux 기기를 쓴다 |
| 만드는 법 | [`README.md`](README.md) "준비" 는 `field-kit-p7b` 의 콘솔 키트를 적는다. GUI 배포물과 Linux 빌드는 위 두 절이 적는다 |
| NFR-2 | Minecraft 시험과 같은 빌드로 파일 전송 시험을 다시 돈다([`roadmap.md`](../docs/kor/roadmap.md) Phase 7 검증, [ADR 0036](../docs/kor/decisions/0036-phase-8-minecraft-검증-키트.md)). 키트를 바꾸면 그 날 Minecraft 전에 한 번 돈다 |

## 다음에 할 것

시연 리허설과 세 기기의 멤버 목록 단절은 2026-11-02 시연 플레이어와 함께 끝났다([`experiments.md`](../docs/kor/experiments.md) 3.3, 4장).
시연 망의 매핑 수명은 재지 않기로 했다([ADR 0037](../docs/kor/decisions/0037-시연-전-확인을-리허설-한-번으로.md)).

| 무엇 | 기기와 망 | 키트 | 지금 |
|------|-----------|------|------|
| T-7 의 GUI 만의 흐름 | GUI 기기 둘. 호스트가 GUI 로 방을 만들고 코드를 읽고, 플레이어가 GUI 로 참가하고, 양쪽 목록에 상대가 보이고, 한쪽을 끊어 다른 쪽 목록을 보고, 실패 코드 표시를 한 번 본다. 명령줄 인자를 쓰지 않는다 | `field-kit-p8` | 남았다. 2026-11-02 의 호스트는 콘솔이었다 |

시연에서 연결이 실패하거나 끊길 때의 원인 조사는 [`roadmap.md`](../docs/kor/roadmap.md) Phase 8 작업이 갖는다. 시연 호스트의 휴대폰은 Wi-Fi 를 끄고 핫스팟을 켠다. Wi-Fi 가 켜져 있으면
핫스팟이 학교 망을 거쳐 나가 리허설과 다른 망 쌍이 된다. 학교 망 안의 기기와도 연결되지 않았다(2026-11-02).

## 때가 되면 할 것

| 무엇 | 절차 | 언제 |
|------|------|------|
| 방화벽 인바운드 | 시험 4 | ADR 0002 를 다시 열 때만 |
