# 0036. Phase 8 Minecraft 검증 키트를 `field-kit-p8` 로 바꾼다

- 상태: 채택
- 날짜: 2026-11-01
- 관련: [ADR 0034](0034-phase-8-착수-전-결정.md), [ADR 0035](0035-linux-콘솔-클라이언트.md), [`../roadmap.md`](../roadmap.md) Phase 7·8 검증, [`../experiments.md`](../experiments.md) 3.2

## 맥락

[ADR 0034](0034-phase-8-착수-전-결정.md) 는 작업 순서를 "Minecraft 검증은 `field-kit-p7b` 로 먼저 한다" 로 정했다. `roadmap.md` Phase 7
검증이 Minecraft 시험과 Phase 7 파일 전송을 같은 터널 빌드로 돌려야 NFR-2 가 성립한다고 하기 때문이다.

그 뒤 [ADR 0035](0035-linux-콘솔-클라이언트.md) 로 Linux 콘솔 클라이언트가 생겼다. 저장소 소유자가 Windows 기기와 Linux 기기로 혼자
두 기기 실측을 하려면 Linux 쪽 빌드가 필요한데 `field-kit-p7b` 에는 Linux 클라이언트가 없다.

## 결정

**Phase 8 의 Minecraft 검증은 `field-kit-p8` 로 한다.** 저장소 소유자의 결정이다.

- `field-kit-p8` 은 Windows 의 GUI 배포물과 Linux 의 Release 콘솔이다. 두 빌드에 드는 원본이 태그 커밋과 같아야 한다. 첫 실측의 Windows 배포물은 그 앞 커밋으로 만들었고, 태그 커밋과의 차이는 Windows 빌드에 들지 않는 Linux 플랫폼 파일 하나다
- **NFR-2 의 같은 빌드 조건은 파일 전송을 그 키트로 다시 돌아 지킨다.** 키트를 바꾸면 그 날 Minecraft 전에 같은 두 실행 파일로 한 번 돈다
- ADR 0034 의 나머지 결정은 그대로다. T-7 의 기기는 세 대다

## 대안

**대안 1. `field-kit-p7b` 를 그대로 쓴다.** 버렸다. Linux 기기를 쓸 수 없고 GUI 도 없다. 두 기기 실측이 상대의 시간에 다시 묶인다.

**대안 2. Windows 는 `field-kit-p7b`, Linux 는 새 빌드를 쓰고, `field-kit-p7b` 이후 터널 데이터 경로가 바뀌지 않았다는 근거로 NFR-2 를
판정한다.** 버렸다. 그 근거는 `git diff` 를 읽은 판단이라 같은 빌드로 다시 재는 것보다 약하다. 파일 전송은 몇 분이면 된다.

## 결과

- 소유자 혼자 두 기기로 Minecraft 검증 가운데 30분 연속 플레이, 게임 표시 지연과 터널 RTT 의 차이, 블록 배치와 채팅, Wi-Fi 끊김을 돌았다
- 키트마다 파일 전송을 다시 돈다. 받는 쪽이 Linux 면 `Test-Transfer.ps1` 의 받는 쪽이 돌지 않아 같은 와이어의 수신기를 따로 쓴다
