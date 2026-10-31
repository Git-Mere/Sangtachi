# Linux 콘솔 클라이언트 (ADR 0035)

## 왜 했나

두 기기 실측을 소유자의 Windows 기기와 Linux 기기(`mere-galaxy`, Ubuntu 26.04)로 하기 위해서다. 결정과 버린 대안은
[ADR 0035](../../decisions/0035-linux-콘솔-클라이언트.md) 가 갖는다. 구현은 하위 작업자 둘이 나눠 했다. 하나는 POSIX 공통 계층,
하나는 Linux 어댑터와 경로 조회다. 부모가 결함 하나를 고치고 두 방향 검증과 Windows 회귀를 돌렸다.

## 무엇을 바꿨나

| 무엇 | 자리 |
|------|------|
| POSIX 공통 플랫폼 계층: 대기(파이프 이벤트와 `poll`), UDP, TCP, 이름 해석, 난수, SHA-256, 표준 출력, 기록 파일, 종료 신호, 세션 종료 | `client/src/platform/posix/` |
| Linux 전용: 시계, 실행 파일 경로, rtnetlink 조회, 인터페이스와 기본 경로, TUN 장치, 서브넷 판정 | `client/src/platform/linux/` |
| 실행 파일 옆 파일 목록을 `adapter_files()` 로, 방화벽 판정에 `kSkipped`, `adapter.open guid=` 표기를 `interface_id_text` 로 | `tun.hpp`, `interfaces.hpp`, `win32/`, `client_process.cpp`, `main.cpp` |
| 표준 입력 한 줄 읽기를 `platform::read_stdin_line` 으로. Linux 판은 stdio 를 거치지 않는다(아래 결함) | `console_ctrl.hpp`, `win32/console_ctrl.cpp`, `posix/console_ctrl.cpp`, `loop.cpp` |
| CMake 의 Linux 분기, Linux 시험 목록 | `client/CMakeLists.txt`, `client/src/CMakeLists.txt`, `tests/CMakeLists.txt` |
| POSIX/Linux OS 헤더 24개 | `tools/platformgate/` |
| Linux 시험: 대기, UDP 크기 경계, 신호, 기록 파일, 표준 입력, 서브넷 판정표, TUN 수명. SHA-256 벡터 추가 | `tests/` |
| 실행 대상, 실행 전제, 스레드와 종료, 시험 규칙 | `spec.md`, `roadmap.md`, `architecture.md`, `concurrency.md`, `protocol.md` 6장, `linux-prereq.md`(새 문서), `CLAUDE.md` |

## 결함: Linux 정상 종료가 표준 입력에 막혔다

종료 신호 뒤 정리는 `adapter.close` 까지 끝나는데 프로세스가 표준 입력에 한 줄이 오거나 EOF 가 될 때까지 끝나지 않았다.

- **원인을 재현으로 확인했다.** `mere-galaxy`(glibc 2.43)에서 다른 스레드가 `std::getline(std::cin)` 이나 `fgets` 로 읽는 중에
  `main` 이 반환하면 표준 입력이 닫힐 때까지 끝나지 않았다(4006ms). 같은 자리를 `read(0)` 로 읽으면 바로 끝났다(308ms).
  블록한 stdio 읽기가 stdin 잠금을 쥐고 `exit` 가 그 잠금을 기다린다
- **고친 것.** `[console]` 이 `platform::read_stdin_line` 으로 읽는다. Windows 판은 이전과 같은 `std::getline(std::cin, line)` 이다.
  Linux 판은 `read(0)` 로 읽고 줄을 나눈다. 규칙은 `concurrency.md` 7장 종료의 `[console]` 항목에 있다
- **버린 것.** 정리 뒤 `std::_Exit`. Windows 종료 경로까지 바꾸게 된다

## 검증

| 무엇 | 결과 |
|------|------|
| `mere-galaxy` ctest (g++ 15, Debug) | 75건 중 74 통과, 1건 건너뜀(`linux_tun` 수명 시험, root 필요). 그 시험은 `sudo` 로 따로 6번 돌려 모두 통과했다 |
| WSL Ubuntu 24.04 (g++ 13) 빌드 | 경고 없이 통과 |
| 변이 | POSIX 계층 7개(대기, UDP, SHA-256, 신호, 표준 출력), 서브넷 판정 10개, 표준 입력 2개(`getline` 으로 되돌림, EOF 앞 남은 줄 버림). 모두 시험이 잡았다 |
| 같은 이름의 영구 TUN 장치 | `ip tuntap add` 로 만든 장치가 있을 때 고치기 전 빌드는 그 장치에 붙어 주소를 붙였다. 고친 빌드는 `adapter.error reason=open op=TUNSETIFF code=16` 으로 기동에 실패했고 그 장치를 건드리지 않았다 |
| Windows `scripts/test.ps1` | `all tests passed`. ctest 664건, CLI·e2e·로비·세션 확인 실패 0 |
| platformgate | `VERDICT: pass`, 시험 74개 통과 |
| `docgate.py`, `--claims` | `VERDICT: pass` |

**두 방향 실측.** 제어 서버는 운영 서버다. 양쪽 모두 어댑터를 붙였다(Windows 관리자, Linux `sudo`).

| 구성 | 방 참가 | 가상 IP `ping` | 종료 신호와 `CLOSE` |
|------|---------|----------------|---------------------|
| Linux 호스트, Windows 플레이어 | `CONNECTED`, 두 멤버 명부 | Windows→`10.100.0.1` 4/4(평균 31ms), Linux→`10.100.0.2` 4/4(평균 76ms) | 호스트 `SIGINT` 뒤 80ms 에 종료, `adapter.close`, 장치 사라짐. 플레이어 `CONNECTED to=CLOSED`, `reason=closed` |
| Windows 호스트, Linux 플레이어 | `CONNECTED`, 두 멤버 명부 | Linux→`10.100.0.1` 4/4(평균 50ms), Windows→`10.100.0.2` 4/4(평균 54ms) | 플레이어 `SIGINT` 뒤 81ms 에 종료. 호스트 `CONNECTED to=CLOSED` |

- **Linux 호스트의 Minecraft 접속.** `mere-galaxy` 에 Java 25 와 Minecraft 서버 26.3 을 깔았다. 소유자가 Windows 에서 `10.100.0.1` 로
  접속해 월드에 들어갔다(서버 로그 `mere_ko joined the game`, 주소 `10.100.0.2`). 첫 시도는 서버 기본값 `white-list=true` 에 막혔다.
  그때도 TCP 연결과 계정 인증은 터널을 지났다
- 두 실측 모두 표준 입력을 열어 둔 채 신호로 끝냈다. 위 결함이 실물에서도 풀렸다
- 실측 기기의 ufw 는 꺼져 있었다. `linux-prereq.md` 5절의 규칙은 보지 않았다
- **지연.** 처음 Windows→`10.100.0.1` 이 168~208ms 였다. 작업자가 `mere-galaxy` 의 Wi-Fi 절전으로 판단했다. 프로그램 없이 LAN 주소로
  보낸 `ping` 도 평균 96ms 였고, 무선을 깨워 두면 평균 9ms 였다. 실측 동안 절전을 껐다. 위 표의 값은 끈 뒤다

## 작업자가 직접 정한 것

작업 지시에 없어 구현자가 정한 값 가운데 계약에 닿는 것이다. 모두 살아 있는 문서와 어긋나지 않는지 부모가 봤다.

- 대기 핸들 인코딩은 내부 헤더 `posix/fd_handle.hpp` 하나에 둔다. 큰 유한 타임아웃은 `poll` 의 `int` 상한 때문에 나눠 기다린다
- 실패 op 토큰은 실제 호출 이름이다(`fcntl`, `poll`, `RTM_NEWADDR` 등)
- 경로 삭제의 "없음" 에 `ESRCH` 도 성공으로 친다. 만든 경로는 `RTPROT_BOOT` 로 만들고 지워 커널 경로를 건드리지 않는다
- 서브넷 판정 함수는 OS 자료와 떨어진 헤더 전용 `linux/subnet_check.hpp` 다
- `signal.h`, `time.h` 는 ISO C 헤더이기도 해서 platformgate 의 OS 헤더 목록에 넣지 않는다

## 크로스 모델 리뷰

Codex(`gpt-6.1-sol`). 범위를 넷으로 나눠 렌즈를 둘씩 줬다. 고친 차이만 회귀·미러 렌즈로 다시 돌렸고 LGTM 이었다. 마지막으로 스테이징한 전체 차이를 기록·인수인계 일관성 렌즈로 돌렸고 LGTM 이었다.

| 범위 | 렌즈 | 결과 |
|------|------|------|
| `platform/posix/`, 그 시험, `loop.cpp` | 계약 준수, 동시성과 수명 | blocker 2, warn 1 |
| `platform/linux/`, 그 시험 | rtnetlink·TUN 정확성, 보안과 권한 | blocker 2, warn 2 |
| 공유 파일, `win32/`, CMake, platformgate | Windows 바이트 불변, 빌드와 게이트 | LGTM |
| 문서와 `CLAUDE.md` | 문서 간 일관성, 근거 | warn 5 |

| 지적 | 처리 |
|------|------|
| blocker: `TUNSETIFF` 가 같은 이름의 영구 장치에 붙어 그 주소와 MTU 를 바꾸고, 닫은 뒤에도 장치가 남는다 | 반영. `IFF_TUN_EXCL` 을 건다. 위 검증 표의 실측으로 고치기 전 거동과 고친 뒤 거동을 둘 다 봤다. `linux-prereq.md` 3절에 적었다 |
| blocker: `NLMSG_DONE` 의 오류 값과 `NLM_F_DUMP_INTR` 를 보지 않아 덜 읽은 덤프로 `proceed` 가 나올 수 있다 | 반영. 둘 다 조회 실패로 돌린다. 차단할 것이 없으면 판정은 `unknown` 이고 기동 실패다. 커널이 이 둘을 내게 만들 수단이 없어 시험은 없다 |
| warn: 속성이 깨지면 조용히 멈추고 짧은 값은 0 이 된다 | 기각. 이 소켓에 보내는 것은 커널뿐이고 커널은 정렬된 속성을 낸다. 깨진 입력을 다루는 코드는 시험할 수 없는 분기만 늘린다 |
| warn: `recv` 가 잘림을 드러내지 않는다. 상한 시간도 없다 | 기각. 받는 버퍼가 64KB 이고 커널은 덤프 묶음을 그보다 작게 끊는다. 커널은 덤프를 언제나 `NLMSG_DONE` 으로 끝낸다 |
| blocker: 자동 리셋 이벤트를 둘이 함께 기다리면 둘 다 신호를 받을 수 있다 | 기각. 자동 리셋 이벤트는 셋이고(콘솔, 제어 요청, 제어 응답) 기다리는 스레드가 각각 하나다. 복제하는 이벤트는 종료 이벤트뿐이고 수동 리셋이다 |
| blocker: `read_stdin_line` 의 정적 버퍼가 `exit` 때 소멸하는데 떼어 둔 `[console]` 이 아직 쓸 수 있다 | 반영. 버퍼를 소멸시키지 않는다 |
| warn: 값 1 같은 핸들은 fd -1 로 풀리고 `poll` 이 건너뛰어 무한 대기가 돌아오지 않는다 | 반영. 음수 fd 는 대기 실패(`EINVAL`)다. 시험을 더했다 |
| warn: `spec.md` FR-9 가 Wintun 과 IP Helper 만 적는다 | 반영. Linux 는 TUN 과 rtnetlink 로 같은 일을 한다고 적었다 |
| warn: `architecture.md` 9장 `adapter.firewall` 의 `verdict` 가 `pass` 만 적는다 | 반영. Linux 의 `skipped` 를 적었다 |
| warn: 기록 파일 경로의 와이드 문자 API 가 Linux 에도 걸린다 | 반영. Windows 에 한정했다 |
| warn: `linux-prereq.md` 4절의 "조회 실패면 `unknown`" 이 차단이 있을 때와 맞지 않는다 | 반영. 차단할 것을 찾지 못했을 때만 `unknown` 이라고 고쳤다 |
| warn: `architecture.md` 의 Linux 어댑터 수명 표가 `linux-prereq.md` 의 규칙을 되풀이한다 | 반영. 그 문서의 절을 가리키게 줄였다 |

**리뷰 밖에서 찾은 것.** 고친 뒤 `linux_tun` 수명 시험이 6번 중 5번 실패했다. `IFF_TUN_EXCL` 을 빼도 같았다. 열자마자 받은 패킷이 있어
"빈 상태" 단정이 깨졌다. 장치를 올리면 커널이 내보내는 IPv6 링크 로컬 패킷으로 본다. 패킷 내용은 확인하지 않았다. 시험이 받은 패킷을 비운 뒤
빈 상태를 보게 고쳤다. 고친 뒤 6번 모두 통과했다.
