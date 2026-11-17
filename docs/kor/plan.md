# Plan

**이 파일은 다음 세션의 인수인계다.** 무엇이 남았고 무엇을 먼저 하는지만 적는다. 끝난 일의
경과는 [`commit_history/`](commit_history/README.md)가 갖는다.

## 남은 일이 어디에 있는가

| 축 | 출처 | 지금 |
|----|------|------|
| 구현 | [`roadmap.md`](roadmap.md) | Phase 1~8 은 끝났다. Phase 9 를 하고 있다 |
| 실측 | [`field-test/required-test.md`](../../field-test/required-test.md) | 사람이 직접 하는 실측. 두 번째 기기나 시연 상대가 있어야 한다 |
| 대기 | 이 파일 "대기 중인 것" | 실측 밖의 대기 |
| 구현 때 같이 볼 자리 | 이 파일 같은 이름의 절 | `roadmap.md` 에 없는 것만 둔다 |
| 문서 부채 | 이 파일 "문서 부채" 절 | 없음 |

## 다음에 할 일

Phase 9 의 코드(텔레메트리 서비스, 클라이언트의 레코드와 업로드, 배포 도구의 `tm` 대상, 분석 도구 `tools/tm-analysis`)는 구현했다.
텔레메트리 서비스는 인스턴스에 배포했고 연기 시험과 전체 상한까지의 부하 확인을 통과했다. Linux 빌드와 GUI 빌드도 확인했다.
`roadmap.md` Phase 9 검증 가운데 상대가 필요 없는 것(두 서비스 분리, 함께 내린 10분 동안의 터널 유지, 포트 8001, 통제된 실패 둘)도 통과했다. 실측은 동일 LAN 과 가정망 대 모바일 핫스팟을 10회씩 돌렸다.
AWS 콘솔 작업은 대시보드까지 끝났다. 남은 것은 아래다.

| 무엇 | 누가 | 메모 |
|------|------|------|
| 남은 실측 | 소유자 | 가정망 대 가정망, 캠퍼스망 대 가정망, 가용한 다른 NAT 거동(`roadmap.md` Phase 9 테스트 환경 표). 상대와 일정은 소유자가 잡는다. 환경마다 10회 이상이고 묶음마다 `tools/tm-analysis/local/batches.txt` 에 적는다. 그 파일에 지금 묶음 넷(통제된 실패 둘, 동일 LAN, 가정망 대 핫스팟)이 있다 |
| 분석 도구 실행 | 소유자 | 이 PC 와 인스턴스 역할은 텔레메트리 테이블을 읽지 못한다. 묶음을 더할 때마다 AWS CloudShell 에서 `tmanalysis.py --save` 로 돌려 `report.md` 와 `records.json` 을 `tools/tm-analysis/local/` 에 받는다. 그 뒤는 `--load` 로 이 PC 에서 다시 낸다. 제어 서버만 내린 동안 받은 시험 레코드 셋(2026-11-17 21:57 UTC)의 `room_known` 은 묶음 밖이라 아직 보지 않았다 |
| 재현 시행 1회 | 미정 | `spec.md` A-4 |
| CloudWatch 그래프 | 소유자, 실측 뒤 | 대시보드는 만들었다. 정의는 git 이 무시하는 `tools/tm-analysis/local/dashboard.json` 이다. 2026-11-17 묶음의 1분 해상도 값은 인스턴스 역할의 `GetMetricStatistics` 로 받아 `tools/tm-analysis/local/cw-2026-11-17-1min.json` 에 두었다. 다음 묶음도 측정하고 15일 안에 같은 식으로 받는다. 분석 도구와의 백분위수 대조는 끝났다 |
| `experiments.md` 의 Phase 9 장 | 실측 뒤 | 장 구성은 실측 계획과 같이 정한다. 분석의 한계는 이미 5장에 있다 |

## 시험 기기 `mere-galaxy`

Linux 시험 기기는 `mere-galaxy`(Ubuntu 26.04, `ssh mere@mere-galaxy`)다([ADR 0035](decisions/0035-linux-콘솔-클라이언트.md)). 시연 뒤에도 시험에 계속 쓴다.

- **메모.** `/etc/sudoers.d/90-sangtachi`(암호 없는 `sudo`)를 남겨 둔다. 계속 시험에 쓰기 때문이다(소유자 결정). 이 기기를 시험에서 뺄 때 지운다
- Wi-Fi 절전은 재부팅하거나 다른 망에 붙으면 다시 켜진다([이슈 0001](issue_history/0001-linux-wifi-절전으로-느린-터널-ping.md)). 켜져 있으면 무선 지연이 평균 96ms 까지 오르므로 실측 전에 `iw dev wlo1 set power_save off` 로 끈다
- `field-kit-p8` 의 Linux 빌드는 `~/sangtachi-head/build/client/` 다. 앞서 쓰던 `~/sangtachi-b` 는 그 전 Debug 빌드다
- Phase 9 커밋 `5d5b594` 의 Debug 시험 빌드는 `~/sangtachi-p9/build/` 다. 소스는 `git archive` 로 옮긴 `~/sangtachi-p9/src/` 다
- Phase 9 실측의 Release 콘솔 빌드는 `~/sangtachi-p9r/build/client/` 다(커밋 `5f9f710`). 이 PC 쪽은 `C:\me\build\test\Release-p9\client\` 다. 실측 시행을 돌린 스크립트는 git 이 무시하는 `.prime/p9-field/field_run.py` 이고, 시행마다 클라이언트를 새로 띄워 30초 유지하고 60초 쉰다
- `linux_tun` 의 어댑터 시험은 `CAP_NET_ADMIN` 이 없으면 건너뛴다. 이 기기에서는 `sudo ctest` 로 돌린다. 호스트가 떠 있으면 `Sangtachi` 장치를 쥐고 있어 `TUNSETIFF` 가 EBUSY(16)로 실패한다
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
