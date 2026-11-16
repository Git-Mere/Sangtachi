# Plan

**이 파일은 다음 세션의 인수인계다.** 무엇이 남았고 무엇을 먼저 하는지만 적는다. 끝난 일의
경과는 [`commit_history/`](commit_history/README.md)가 갖는다.

## 남은 일이 어디에 있는가

| 축 | 출처 | 지금 |
|----|------|------|
| 구현 | [`roadmap.md`](roadmap.md) | Phase 1~8 은 끝났다. 다음은 Phase 9 다 |
| 실측 | [`field-test/required-test.md`](../../field-test/required-test.md) | 사람이 직접 하는 실측. 두 번째 기기나 시연 상대가 있어야 한다 |
| 대기 | 이 파일 "대기 중인 것" | 실측 밖의 대기 |
| 구현 때 같이 볼 자리 | 이 파일 같은 이름의 절 | `roadmap.md` 에 없는 것만 둔다 |
| 문서 부채 | 이 파일 "문서 부채" 절 | 없음 |

## 다음에 할 일

Phase 1~8 은 끝났고 시연(2026-11-05)도 끝났다. 다음은 Phase 9 구현이다. 착수 전 결정은 [ADR 0039](decisions/0039-phase-9-착수-전-결정.md) 로 끝났다.

- 남은 결정은 실측 계획 하나다. 실측 전에 정한다(`roadmap.md` Phase 9 작업)
- AWS 콘솔 작업(테이블 용량, 새 테이블, IAM, 보안 그룹, 대시보드)은 소유자가 한다. 목록은 `roadmap.md` Phase 9 작업에 있다
- `experiments.md` 의 Phase 9 장 구성은 실측 계획과 같이 정한다

## 시험 기기 `mere-galaxy`

Linux 시험 기기는 `mere-galaxy`(Ubuntu 26.04, `ssh mere@mere-galaxy`)다([ADR 0035](decisions/0035-linux-콘솔-클라이언트.md)). 시연 뒤에도 시험에 계속 쓴다.

- **메모.** `/etc/sudoers.d/90-sangtachi`(암호 없는 `sudo`)를 남겨 둔다. 계속 시험에 쓰기 때문이다(소유자 결정). 이 기기를 시험에서 뺄 때 지운다
- Wi-Fi 절전은 재부팅하거나 다른 망에 붙으면 다시 켜진다([이슈 0001](issue_history/0001-linux-wifi-절전으로-느린-터널-ping.md)). 켜져 있으면 무선 지연이 평균 96ms 까지 오르므로 실측 전에 `iw dev wlo1 set power_save off` 로 끈다
- `field-kit-p8` 의 Linux 빌드는 `~/sangtachi-head/build/client/` 다. 앞서 쓰던 `~/sangtachi-b` 는 그 전 Debug 빌드다
- `.prime/linux-port/` 는 git 이 무시하는 작업 폴더다. 작업자 보고와 `mere-galaxy` 에서 띄우던 스크립트가 있다. 호스트는 `r-host.sh` 로 띄우고 `r-host-stop.sh` 로 끝낸다
- 학교 유선 LAN 에서는 Tailscale 제어 서버에 닿지 않아 `ssh mere@mere-galaxy` 가 끊긴다. 그때는 같은 핫스팟 Wi-Fi 에 붙어 로컬 주소로 들어가거나, 방에 들어간 동안 터널의 가상 IP 로 들어간다

## 대기 중인 것

없음.

## 구현 때 같이 볼 자리

표의 "언제" 가 오면 같이 고칠 자리다. `roadmap.md` 에 이미 걸린 작업은 여기에 다시 적지 않는다.

| 자리 | 무엇 | 언제 |
|------|------|------|
| `tests/mutate.py` 의 시간 초과 | 상한에 걸리면 pytest 프로세스만 죽인다. 그 시험이 띄운 서버 프로세스가 남을 수 있다. 손자까지 거두려면 Windows Job Object 가 필요하다 | 변이 실행기를 다시 고칠 때 |
| `pending_pings` 상한 | 상한 분기(`protocol.md` 5.5 의 "그래도 가득 차면 새 `PING`을 보내지 않는다")를 밟는 시험이 없다. PING 간격과 항목 수명이 둘 다 5초라 지금 구성으로는 표가 둘 이상 차지 않는다 | 11장 PING 간격이나 5.5 항목 수명을 바꿀 때 |
| 콘솔 `send` 의 송신 버퍼 | 소켓 송신 버퍼가 차면 `WSAEWOULDBLOCK` 으로 작업을 끝낸다. 576·1452바이트는 한 번에 45~133개만 나간다. 다음 루프 바퀴에 이어 보내면 정한 개수를 채운다. 바이트 대조는 이것에 기대지 않는다(ADR 0032) | 콘솔 `send` 로 개수를 채워야 하는 시험이 생길 때 |
| `is_valid_name` 의 자리 | `control_plane.md` 2.7 검사가 `tunnel/packet.hpp` 에 있어 기동 인자와 제어 평면 코드가 터널 패킷 모듈을 포함한다. 중립 헤더로 옮기고 `packet.hpp` 가 그것을 쓰는 편이 의존 방향이 맞다 | 다음에 그 헤더를 고칠 때. GUI 의 이름 칸 검사가 그 계기일 수 있다 |

## 문서 부채

없음.

## 세션을 시작할 때

읽는 순서, 문서 역할, 재발 방지 규칙은 저장소 루트 `CLAUDE.md`에 있다. **여기에 옮겨 적지
않는다.**
