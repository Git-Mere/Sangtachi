# Plan

**이 파일은 다음 세션의 인수인계다.** 무엇이 남았고 무엇을 먼저 하는지만 적는다. 끝난 일의
경과는 [`commit_history/`](commit_history/README.md)가 갖는다.

## 남은 일이 어디에 있는가

| 축 | 출처 | 지금 |
|----|------|------|
| 구현 | [`roadmap.md`](roadmap.md) | Phase 1~7 은 끝났다. Phase 8 진행 중이다 |
| 실측 | [`field-test/required-test.md`](../../field-test/required-test.md) | 사람이 직접 하는 실측. 두 번째 기기나 시연 상대가 있어야 한다 |
| 대기 | 이 파일 "대기 중인 것" | 실측 밖의 대기 |
| 구현 때 같이 볼 자리 | 이 파일 같은 이름의 절 | `roadmap.md` 에 없는 것만 둔다 |
| 문서 부채 | 이 파일 "문서 부채" 절 | 없음 |

## 다음에 할 일

Phase 8 이다. 착수 전 결정은 [ADR 0034](decisions/0034-phase-8-착수-전-결정.md) 다. 같은 기기에서 사람이 보는 GUI 와 콘솔 확인은
끝났다. 콘솔 창 닫기와 GUI 로그오프로 잰 호스트 종료도 끝났다. 그 전 시연 망의 리허설과 세 기기의 멤버 목록 단절도 끝났다([`experiments.md`](experiments.md) 3.3, 4장). 남은 것은 아래다.

1. **Linux 콘솔 클라이언트의 뒷정리** ([ADR 0035](decisions/0035-linux-콘솔-클라이언트.md)). 구현과 두 방향 검증(`roadmap.md` Phase 8
   작업의 같은 항목)은 끝났다. Linux 기기는 `mere-galaxy`(Ubuntu 26.04, `ssh mere@mere-galaxy`)다. 남은 것은 아래다
   - `mere-galaxy` 는 시연의 호스트로 계속 쓴다. 시연 때는 소유자의 집 Wi-Fi 에 사람 없이 둔다([ADR 0038](decisions/0038-시연-호스트를-집-wi-fi-에-둔다.md)). 잠들지 않게 하고 전원을 연결한다. 시연이 끝나면 아래 둘을 같이 정리한다(소유자 결정)
     - `/etc/sudoers.d/90-sangtachi`(암호 없는 `sudo`)를 지운다
     - Wi-Fi 절전을 되돌린다([이슈 0001](issue_history/0001-linux-wifi-절전으로-느린-터널-ping.md)). 지금은 `iw dev wlo1 set power_save off` 로 꺼 두었다. 재부팅하거나 다른 망에 붙으면 켜지므로 실측 전에 확인한다.
       켜져 있으면 무선 지연이 평균 96ms 까지 오른다
   - `mere-galaxy` 에 Java 25 와 Minecraft 서버 26.3(`~/mc-server`, 화이트리스트에 `mere_ko`, `Mere_galaxy`)을 깔았다. 서버는 꺼 두었다. 리허설에서는 `Mere_galaxy` 계정을 다른 기기가 썼다. 친구가 참가하면 그 계정을 화이트리스트에 넣는다
   - `field-kit-p8` 의 Linux 빌드는 `mere-galaxy` 의 `~/sangtachi-head/build/client/` 다. 앞서 쓰던 `~/sangtachi-b` 는 그 전 Debug 빌드다
   - `.prime/linux-port/` 는 git 이 무시하는 작업 폴더다. 작업자 보고와 `mere-galaxy` 에서 띄우던 스크립트가 있다. 리허설의 호스트는 `r-host.sh`, `r-host-stop.sh` 로 띄우고 끝냈다
   - 학교 유선 LAN 에서는 Tailscale 제어 서버에 닿지 않아 `ssh mere@mere-galaxy` 가 끊긴다. 그때는 같은 핫스팟 Wi-Fi 에 붙어 로컬 주소로 들어가거나, 방에 들어간 동안 터널의 가상 IP 로 들어간다
2. **새 시연 망의 리허설**, **T-7 의 GUI 만의 흐름**, **시연** (2026-11-05). 남은 실측은 [`field-test/required-test.md`](../../field-test/required-test.md) 가 갖는다. 시연 플레이어의 GUI 가 기동 때 `adapter.error reason=eof` 로 끝나면 다시 띄운다([이슈 0006](issue_history/0006-첫-gui-실행이-adapter-eof-로-끝났다.md))

`experiments.md` 의 구성과 텔레메트리 서비스의 보고서 생성은 Phase 9 착수 전에 정한다(`roadmap.md` Phase 9).

## 대기 중인 것

| 무엇 | 막힌 이유 | 풀리는 조건 |
|------|-----------|-------------|
| [`windows-prereq.md`](windows-prereq.md) 실물 검증의 나머지 | 3절 저장된 주소의 잔존 | Phase 8(시연) |

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
