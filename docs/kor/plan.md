# Plan

**이 파일은 다음 세션의 인수인계다.** 무엇이 남았고 무엇을 먼저 하는지만 적는다. 끝난 일의
경과는 [`commit_history/`](commit_history/README.md)가 갖는다.

## 남은 일이 어디에 있는가

| 축 | 출처 | 지금 |
|----|------|------|
| 구현 | [`roadmap.md`](roadmap.md) | Phase 1~9 는 끝났다. 남은 것은 스트레치 목표다 |
| 실측 | [`field-test/required-test.md`](../../field-test/required-test.md) | 없음 |
| 대기 | 이 파일 "대기 중인 것" | 없음 |
| 문서 부채 | 이 파일 "문서 부채" 절 | 루트 README 의 GUI 스크린샷과 배포물 링크 |

## 다음에 할 일

| 무엇 | 누가 | 메모 |
|------|------|------|
| 스트레치 목표에 들어갈지와 무엇부터 할지 | 소유자 | 목록과 조건은 `roadmap.md` 스트레치 목표가 갖는다. Phase 9 그래프를 다시 그리는 법은 `tools/tm-plot` README 이고, 입력인 레코드와 CloudWatch 1분 해상도 값은 git 이 무시하는 `tools/tm-analysis/local/` 에 있다 |

## 시험 기기 `mere-galaxy`

Linux 시험 기기는 `mere-galaxy`(Ubuntu 26.04, `ssh mere@mere-galaxy`)다([ADR 0035](decisions/0035-linux-콘솔-클라이언트.md)).

- `/etc/sudoers.d/90-sangtachi`(암호 없는 `sudo`)를 스트레치 목표에 쓰려고 남겨 둔다(소유자 결정). 이 기기를 시험에서 뺄 때 지운다
- Wi-Fi 절전은 재부팅하거나 다른 망에 붙으면 다시 켜진다([이슈 0001](issue_history/0001-linux-wifi-절전으로-느린-터널-ping.md)). 실측 전에 `sudo iw dev wlo1 set power_save off` 로 끈다
- Release 콘솔 빌드는 `~/sangtachi-p9r/build/client/` 다. 이 PC 쪽은 `C:\me\build\test\Release-p9\client\` 다
- `linux_tun` 의 어댑터 시험은 `sudo ctest` 로 돌린다. 호스트가 떠 있으면 `TUNSETIFF` 가 EBUSY(16)로 실패한다
- 실측과 호스트를 띄우는 스크립트는 git 이 무시하는 `.prime/p9-field/`, `.prime/linux-port/` 에 있다
- 학교 유선 LAN 에서는 Tailscale 이 막힐 때가 있다. 그때는 핫스팟 Wi-Fi 에 같이 붙는다
- 학교 유선 LAN 에서 인스턴스로 바로 SSH 하면 다른 호스트 키가 온다([이슈 0008](issue_history/0008-학교-유선-lan-에서-ssh-호스트-키가-달랐다.md)). `ssh -J mere@mere-galaxy` 로 거친다

## 대기 중인 것

없음.

## 문서 부채

| 자리 | 무엇 |
|------|------|
| 루트 `README.md`, `README.ko.md` | GUI 스크린샷과 배포물 내려받기 링크가 없다. 배포물을 올리면 두 파일의 맨 위 링크 줄에, GUI 화면을 찍으면 두 파일의 Results(실측 결과) 절 앞에 더한다 |

## 세션을 시작할 때

읽는 순서, 문서 역할, 재발 방지 규칙은 저장소 루트 `CLAUDE.md`에 있다. **여기에 옮겨 적지
않는다.**
