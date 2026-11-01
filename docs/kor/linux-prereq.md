# Linux 실행 전제

> English version: [`../eng/linux-prereq.md`](../eng/linux-prereq.md)

Linux 콘솔 클라이언트가 도는 데 필요한 OS 쪽 전제다. 결정과 그 이유는 [ADR 0035](decisions/0035-linux-콘솔-클라이언트.md) 가 갖는다.
Windows 의 같은 문서는 [`windows-prereq.md`](windows-prereq.md) 다. 각 절의 "검증" 은 이 문서의 주장을 실물로 확인했는지를 적는다.

## 1. 대상과 빌드

**검증: 부분 실측.** Ubuntu 24.04(WSL, g++ 13)와 Ubuntu 26.04(g++ 15)에서 경고 없이 빌드하고 시험을 돌렸다. Debug 빌드였다.
위 Release 설정은 Ubuntu 26.04(g++ 15)에서 서버 기본값 없이 빌드했다. Ubuntu 24.04 에서는 돌리지 않았다.

| 항목 | 값 |
|------|-----|
| OS | Ubuntu 24.04 이상 x64 |
| 컴파일러 | g++ 13 이상. 경고는 오류다 |
| 빌드 | CMake 와 Ninja. Wintun, Qt, PowerShell 은 쓰지 않는다 |
| 실행 파일 | `sangtachi_client` 하나. GUI 는 없다 |

```bash
cmake -S <레포> -B <빌드 폴더> -G Ninja -DCMAKE_BUILD_TYPE=Release -DSANGTACHI_BUILD_TESTS=OFF -DSANGTACHI_DEFAULT_SERVER=<서버>
cmake --build <빌드 폴더> --target sangtachi_client
```

기록 파일과 로그는 실행 파일 폴더 아래 `records/`, `logs/` 에 생긴다. 그 폴더에 쓸 수 있어야 한다.

## 2. 권한

**검증: 부분 실측.** `sudo` 로 띄워 어댑터를 열었다. 그 뒤 `sudo` 없이 띄우면 기동에 실패하는 것을 봤다. `setcap` 은 쓰지 않았다.

가상 어댑터를 만들고 주소와 경로를 붙이려면 `CAP_NET_ADMIN` 이 필요하다. 클라이언트는 기동할 때 이 권한이 있는지 보고 없으면
기동 실패다. `--no-adapter` 로 띄우면 권한이 필요 없다.

둘 중 하나로 준다.

- `sudo` 로 띄운다. `records/`, `logs/` 와 그 파일은 root 소유가 된다. 그 뒤 `sudo` 없이 띄우면 그 폴더에 쓰지 못해 기동 실패다.
  두 방식을 섞으려면 먼저 소유자를 되돌린다(`sudo chown -R $USER records logs`)
- 실행 파일에 권한을 붙인다. 빌드할 때마다 다시 붙여야 한다

```bash
sudo setcap cap_net_admin+ep ./sangtachi_client
```

## 3. TUN 장치

**검증: 부분 실측.** 종료 신호로 끝내면 장치와 주소, 경로가 사라지는 것을 봤다. 비정상 종료 뒤는 보지 않았다.

클라이언트는 `/dev/net/tun` 으로 이름 `Sangtachi` 의 TUN 장치를 비영구로 연다. 프로세스가 끝나면 커널이 장치와 그 주소, 경로를 지운다.
그래서 Windows 의 3절 잔존물 정리에 해당하는 일이 없다. `/dev/net/tun` 이 없으면 `tun` 커널 모듈을 올린다.

같은 기기에서 어댑터를 쓰는 프로세스는 하나다. 같은 이름의 장치가 이미 열려 있으면 둘째가 기동에 실패한다. 둘째는 `--no-adapter` 로 띄운다.
같은 이름의 영구 장치(`ip tuntap add` 로 만든 것)가 있으면 그것에 붙지 않고 기동에 실패한다. 그 장치를 지우고 띄운다.

## 4. 서브넷 충돌

**검증: 부분 실측.** 평소 기동은 `proceed` 였다. 어댑터 프로세스가 떠 있을 때 띄운 둘째 어댑터 프로세스는 `block` 으로
기동에 실패했다. `unknown` 은 보지 않았다.

판정표는 [`windows-prereq.md`](windows-prereq.md) 7절 서브넷 충돌의 표다. Linux 도 최장 접두사 일치로 경로를 고르므로 같은 표가
그대로 맞는다. 다른 것은 자료와 실행 방식이다.

- 자료는 main 경로 표의 IPv4 경로 전부와 모든 인터페이스의 IPv4 주소다. 주소는 `/32` 로 같은 겹침 검사에 넣는다. 정책 라우팅의 다른
  경로 표는 보지 않는다
- 클라이언트가 netlink 로 직접 읽고 판정한다. 스크립트를 부르지 않는다
- 검사할 때 우리 장치는 아직 없다(3절). 그래서 자기 어댑터를 빼는 단계가 없다
- 차단할 것을 찾지 못했는데 조회가 실패했으면 `unknown` 이고 기동 실패다

## 5. 방화벽

**검증: 미실측.** 실측 기기는 ufw 가 꺼져 있었다. 그 상태로 Minecraft 접속과 두 방향 `ping` 이 됐다. ufw 를 켜고 아래 규칙을
넣은 상태는 보지 않았다.

클라이언트는 방화벽을 검사하지도 고치지도 않는다. 로그에 `adapter.firewall verdict=skipped` 를 낸다.

- **플레이어는 할 것이 없다.** 터널 UDP 는 이쪽이 먼저 보낸 흐름의 응답으로 들어온다. 상태 추적 방화벽(ufw 의 기본 정책)은 그것을 받는다
- **호스트는 Minecraft 인바운드를 연다.** 가상 어댑터로 들어오는 TCP 25565 다. ufw 면 아래 한 줄이다. ufw 의 기본 규칙은 ICMP echo 를 받으므로
  가상 IP `ping` 에는 따로 할 것이 없다

```bash
sudo ufw allow in on Sangtachi to any port 25565 proto tcp
```

firewalld 나 nftables 를 쓰는 기기는 같은 뜻의 규칙을 그 도구로 넣는다.

## 6. 다른 VPN 과 가상 인터페이스

**검증: 미실측.**

Tailscale 같은 다른 TUN 장치는 로컬 후보에서 빠진다. 인터페이스 형식이 가상(`ARPHRD_NONE`)이라 [`protocol.md`](protocol.md) 10.1 의
제외 형식에 든다. 실측에서 NAT 를 볼 때는 Windows 와 같이 다른 VPN 을 끈다. 이유는 [`field-test/README.md`](../../field-test/README.md) 에 있다.
