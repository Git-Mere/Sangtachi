# field-test

사람이 직접 하는 실측을 모두 여기서 한다. 시험 절차, 날짜별 보고서, 원본 데이터가 한 폴더에 있다.
같은 기기에서 되는 것은 이미 돈다(`scripts/test.ps1`, `scripts/soak-check.ps1`, 캡처 C0~C9). 무엇을 아직 재야 하는지는
[`required-test.md`](required-test.md) 가 갖는다.

## 기록하는 법

```text
field-test/
  README.md                  이 파일. 준비와 시험 절차
  required-test.md           아직 재야 할 것과 순서. plan.md 처럼 늘 지금 상태로 고친다
  TEMPLATE.md                보고서 양식
  nat-probe-index.md         9월 nat-probe 실측의 라벨 표
  YYYY-MM-DD/                실측한 날 (현지 날짜)
    <측정 이름>.md            보고서 하나. TEMPLATE.md 를 복사해 채운다
    <측정 이름>/              그 측정의 원본. 받은 그대로 둔다
```

1. 실측하기 전에 날짜 폴더를 만들고 보고서의 요약 표와 "방법" 을 먼저 채운다
2. 실측한 뒤 원본을 `<측정 이름>/` 에 모으고 "결과" 와 "다음" 을 채운다
3. `required-test.md` 를 고친다. 끝난 항목을 지우고, 새로 생긴 실측을 더한다. 실측 결과로 문서를 고칠 일이 생기면
   그것은 `docs/kor/plan.md` 에 옮긴다

**이 README, `required-test.md`, `TEMPLATE.md` 만 커밋한다** (`.gitignore`). 보고서와 원본에는 실측 시각과 기록이 있고 원본에는 두 집의
공인 IP 가 있다. 공개 레포다. 보고서를 밖에 낼 때의 규칙은 [ADR 0001](../docs/kor/decisions/0001-직접-연결-대비책-미도입.md)
"따라야 할 조치" 2번이다.

## 시험 목록

| 시험 | 어디서 | 시간 | 이 한 번으로 보는 항목 |
|------|--------|:---:|------------------------|
| 1 | 서로 다른 두 집 (원격, 메신저로 맞춤) | 약 40분 | `tools/nat-probe` 재측정(호스트 네 쌍), 로비 체류가 매핑에 미치는 영향, Phase 3 산출물, Phase 4 "서로 다른 가정망 양방향", 캡처 D1(AWS 미경유)·D2(배포 환경 NFR-9), M-2, 기록 파일 |
| 2 | 같은 공유기 아래 두 PC (함께 있을 때) | 약 5분 | Phase 4 "같은 LAN 두 PC", 캡처 D3 |
| 3 (선택) | 시연 상대의 집 | 약 10분 | 시연 상대 망의 매핑 수명. 상대가 이미 잰 두 집이 아닐 때만 |
| 4 (선택) | 시험 1 과 같이 | 약 20분 | 방화벽 인바운드 실측. ADR 0002 를 다시 열 때만 |
| 5 | 서로 다른 두 집 (시험 1 뒤에 이어서 해도 된다) | 약 70분 | Phase 5 "장시간(1시간 이상) 연결 유지" 의 두 집 관측. 판정은 같은 기기의 `scripts/soak-check.ps1` 장면 `hour` 가 하고 이것은 관측값이다 |
| 6 | 서로 다른 두 집 (시험 1·5 와 같은 날) | 약 40분 | Phase 7 검증의 두 기기 항목. 가상 IP `ping`, 파일 전송(NFR-2), 최대 크기 패킷과 MTU, 바이트 카운터와 캡처 대조 |

## 준비 (한 번)

**키트.** 친구에게 폴더 하나를 준다. 실행 파일은 C 런타임을 정적으로 넣은 Release 빌드라 Visual Studio 없이 돈다.

키트는 태그 `field-kit-p7`(Phase 7 끝)로 만든다. 남은 Phase 4~5 실측(시험 1, 5)과 Phase 7 실측(시험 6)을 이 키트 하나로 같은
날 돈다. 친구 PC 의 준비가 한 번으로 끝난다. 시험 1, 2, 5 는 `--no-adapter` 를 붙여 Phase 5 까지와 같은 경로로 돈다
(`docs/kor/architecture.md` 3.5 기동 입력). 키트를 바꿀 때는 [`required-test.md`](required-test.md) 의 "키트" 를 먼저 고친다.

```powershell
git worktree add C:\me\field-kit-src-p7 field-kit-p7   # 한 번. 태그의 원본을 작업 트리 밖에 꺼낸다
. scripts\vsdevshell.ps1
cmake -S C:\me\field-kit-src-p7 -B C:\me\build-field-p7 -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DSANGTACHI_BUILD_TESTS=OFF
cmake --build C:\me\build-field-p7 --target sangtachi_client
```

키트 폴더에 아래를 한 폴더로 넣는다. 앞의 넷은 같은 폴더에 있어야 한다. 클라이언트가 실행 파일 옆에서만 찾는다
(`docs/kor/architecture.md` 7장 어댑터 수명).

| 파일 | 어디서 |
|------|--------|
| `sangtachi_client.exe`, `wintun.dll`, `Test-SubnetOverlap.ps1`, `Initialize-FirewallRules.ps1` | `C:\me\build-field-p7\client\` |
| `natprobe.py` | `C:\me\field-kit-src-p7\tools\nat-probe\` |
| `Test-Transfer.ps1` | `C:\me\field-kit-src-p7\tools\transfer\` |
| `natlife.py` (시험 3 상대에게만) | `C:\me\field-kit-src-p7\tools\nat-lifetime\` |

**두 PC 모두.**

- 키트를 쓸 수 있는 폴더(바탕화면 등)에 둔다. `Program Files` 아래는 안 된다(실행 파일 옆에 `records\` 를 만든다)
- Python 3.8 이상
- VPN 을 끈다. **Tailscale 도 끈다.** 켜 두면 nat-probe 결과가 오염된다
- 첫 실행 때 뜨는 Windows 방화벽 창에서 **허용**을 누른다. 거부하면 펀치가 늘 실패한다
- 명령은 **cmd** 에서 친다(PowerShell 5.1 은 `2>` 로 돌린 로그를 깨뜨린다)
- **시험 6 은 관리자 cmd 에서 친다.** 시작 메뉴에서 cmd 를 오른쪽 클릭하고 "관리자 권한으로 실행". 어댑터를 만들고 방화벽 규칙 둘을
  만든다(`docs/kor/windows-prereq.md` 1절, 2절). 규칙은 남는다. 다 끝나면 관리자 PowerShell 에서 `Remove-NetFirewallRule -Group 'Sangtachi'`
  로 지운다. 시험 1, 2, 5 는 관리자가 아니어도 된다
- 시험 6 에서 클라이언트가 `ERROR adapter.error reason=subnet` 으로 끝나면 그 PC 의 망이 `10.100.0.0/24` 와 겹친다. 그 PC 로는 시험 6 을
  못 한다(`windows-prereq.md` 7절)
- 서버 주소 `<서버>` 는 `deploy/aws.local.md` 의 Elastic IP 다. 친구에게 따로 알려 준다(레포에 적지 않는다)

## 시험 1. 서로 다른 두 집

A = 나(호스트), B = 친구.

**1-1. nat-probe 호스트 네 쌍.** 절차의 출처는 `tools/nat-probe/README.md` 의 `host`, `players` 절이다.

```text
A:  python tools\nat-probe\natprobe.py host --peers 4 --label home-a
B:  python natprobe.py players --count 4 --label home-b
```

엔드포인트를 메신저로 주고받아 입력하고(B 의 넷 -> A, A 의 하나 -> B), "셋, 둘, 하나" 에 같이 Enter.
**통과: A 쪽 `all_success` 인 회차가 3회 연속.** 펀치 직전 확인(`prepunch.checks`)이 틀린 회차는 세지 않고 다시 한다.

**1-2. 클라이언트.** A 는 먼저 캡처를 켠다. 끝까지 켜 둔다.

```text
A (다른 cmd 창):  "C:\Program Files\Wireshark\dumpcap.exe" -i Wi-Fi -f udp -w t1.pcapng
A:  sangtachi_client.exe host --no-adapter --name A --server <서버> 2> a.log
B:  sangtachi_client.exe player --no-adapter --room <A 의 ROOM 코드> --name B --server <서버> 2> b.log
```

1. 두 창의 로그(`a.log`, `b.log`)에 `to=CONNECTED` 가 나오면 양쪽에서 `send 5`. A 는 다른 창에서
   `powershell "Get-NetUDPEndpoint -OwningProcess (Get-Process sangtachi_client).Id" > a-ports.txt` 를 한 번 친다
2. 20초 뒤 양쪽 `leave`. **3분 기다린다**(로비 체류)
3. A `host` -> 새 `ROOM` 코드 -> B `join <코드>` -> 다시 `to=CONNECTED`
4. A `quit`, B `quit`. A 의 캡처를 Ctrl+C 로 끈다

**통과(그 자리에서 볼 것): 두 번 다 양쪽이 `CONNECTED`. `FAIL` 줄이 없다.** 나머지 판정(AWS 미경유, 출발지 포트,
두 STUN 이 같은 공인 주소를 봤는지, 로비 뒤 매핑이 바뀌었는지, 기록 줄 수)은 받은 파일로 한다.

## 시험 2. 같은 공유기 아래 두 PC

두 PC 를 같은 Wi-Fi 에 붙인다. 캡처는 필요 없다.

```text
A:  sangtachi_client.exe host --no-adapter --name A --server <서버> 2> a2.log
B:  sangtachi_client.exe player --no-adapter --room <코드> --name B --server <서버> 2> b2.log
```

양쪽 `to=CONNECTED` 뒤 20초 기다리고 둘 다 `quit`.
**통과: 양쪽 `CONNECTED`, 그리고 각 로그의 `session.endpoint ... endpoint=` 값(`IP:포트` 전체)이 같은 로그의
`control.peers ... candidates=` 가운데 상대의 로컬 후보(사설 주소인 것)와 같다.** 상대의 공인 후보와 같으면 실패가 아니라
"헤어핀으로 수렴" 으로 적는다.

## 시험 3 (선택). 시연 상대 망의 매핑 수명

시연 상대가 이미 잰 두 집(이 기기 집, 친구 집)이 아닐 때만 한다. 서버 쪽 준비(보안 그룹을 상대 공인 IP `/32` 로 열고
응답기를 띄우는 것)는 내가 한다. 상대는 아래 한 줄을 **두 번** 돌린다(한 번에 약 6분). 절차의 출처는 `tools/nat-lifetime/README.md` 다.

```text
python natlife.py run --server <서버>:47000 --label demo
```

**통과: 두 번 다 판정이 `bounded` 나 `at_least` 이고 두 구간이 겹치며, 잰 하한(온 지연의 최댓값)이 45초보다 크다**
(`protocol.md` 11장 `간격 15초 × 3 < 수명`). 판정 표는 `tools/nat-lifetime/README.md` "판정" 이다. 판정 불가(`control_missing` 등)면 다시 잰다.

## 시험 4 (선택). 방화벽 인바운드 실측

ADR 0002 를 다시 열 때만 한다. A 는 **관리자** PowerShell, B 는 시행마다 `natprobe.py punch --unsolicited` 를 같이 돌린다.
절차와 남는 규칙의 정리는 `tools/nat-probe/README.md` "그 밖" 과 그 스크립트 머리에 있다.

```text
A:  powershell -ExecutionPolicy Bypass -File tools\nat-probe\unsolicited-firewall-test.ps1 -Label home-a
B:  python natprobe.py punch --unsolicited --label home-b      (A 의 시행마다 한 번)
```

## 시험 5. 1시간 유지

A = 나(호스트), B = 친구. 같은 기기에는 NAT 가 없어 keepalive 와 매핑 수명이 맞물리는 것을 보지 못한다. 그것을 두 집 사이에서 한 번
관측한다(`docs/kor/roadmap.md` Phase 5). 판정이 아니라 관측값이다.

```text
A (다른 cmd 창):  "C:\Program Files\Wireshark\dumpcap.exe" -i Wi-Fi -f udp -w t5.pcapng
A:  sangtachi_client.exe host --no-adapter --name A --server <서버> 2> a5.log
B:  sangtachi_client.exe player --no-adapter --room <코드> --name B --server <서버> 2> b5.log
```

로그 줄에는 시각이 없다. 시각은 A 의 캡처가 갖는다.

1. 양쪽 로그에 `to=CONNECTED` 가 나오면 시각을 적는다
2. 그대로 60분 이상 둔다. 10분마다 양쪽에서 `send 5` 를 친다. 제품 빌드라 받은 쪽은 `drop_no_sink` 로 센다
3. 60분이 지나면 양쪽에서 `counters`, 그 뒤 `quit`

**적을 것:** 그 동안 `FAIL` 줄과 `to=FAILED`·`to=CLOSED` 가 있었는지, 양쪽 마지막 `session.loss` 줄의 `expected`, `accepted`,
`lost`, 상대 쪽 `drop_no_sink` 가 보낸 수와 같은지. `TUNNEL_DROPPED` 가 났으면 `FAIL` 줄을 본 대략의 시각을 적는다. 마지막으로 오간 `KEEPALIVE` 의 시각은 받은 `t5.pcapng` 로 본다.

## 시험 6. Phase 7 두 기기

A = 나(호스트, `10.100.0.1`), B = 친구(플레이어, `10.100.0.2`). **둘 다 관리자 cmd.** 판정 기준은 `docs/kor/roadmap.md` Phase 7
검증이 갖고, 여기는 그 순서와 명령이다. 제품 빌드(키트)로 돈다. 시험 빌드의 ICMP 응답기가 끼지 않는다.

**6-0. 다른 차단 규칙 확인 (양쪽, 관리자 PowerShell).** 클라이언트의 기동 검증은 우리 규칙만 보고 같은 포트를 막는 남의 차단 규칙은
보지 않는다(`windows-prereq.md` 2절). 차단은 허용보다 앞선다.

```powershell
Get-NetFirewallRule -Direction Inbound -Action Block -Enabled True | ForEach-Object {
  $p = $_ | Get-NetFirewallPortFilter; $a = $_ | Get-NetFirewallApplicationFilter
  '{0} | {1} | {2} | {3}' -f $_.DisplayName, (@($p.Protocol) -join ','), (@($p.LocalPort) -join ','), (@($a.Program) -join ',') }
```

**통과: 아래를 모두 만족하는 줄이 없다.** 판정은 사람이 한다. 있으면 그 규칙을 끄거나 지우고 시작한다.

- 프로토콜이 `Any`, `TCP`, `ICMPv4` 가운데 하나다
- 포트가 `Any` 이거나 25565 를 포함한다. 범위(`25000-26000`)와 쉼표 목록도 본다
- 프로그램이 `Any` 이거나 `powershell.exe` 다. 6-3 에서 파일을 받는 것이 Windows PowerShell 이다

`java` 를 가리키는 줄은 이 시험과 무관하고 Phase 8 에서 본다.

**6-1. 기동과 캡처.** A 는 캡처를 먼저 켜고 클라이언트를 띄운다. 6-4 의 판정이 이 캡처를 쓴다.

```text
A (다른 cmd 창):  "C:\Program Files\Wireshark\dumpcap.exe" -i Wi-Fi -f udp -w t6.pcapng
A:  sangtachi_client.exe host --name A --server <서버> 2> a6.log
B:  sangtachi_client.exe player --room <코드> --name B --server <서버> 2> b6.log
```

**통과(그 자리에서 볼 것): 양쪽 로그에 `INFO adapter.firewall verdict=pass` 와 `to=CONNECTED`.** `adapter.error` 로 끝나면 그 줄을 그대로
받아 둔다.

**6-2. 가상 IP `ping`.**

```text
B:  ping -n 4 10.100.0.1
A:  ping -n 4 10.100.0.2
```

**통과: 양쪽 4개 모두 응답.**

**6-3. 파일 전송 (NFR-2).** A 의 다른 cmd 창에서 받고 B 가 보낸다. 같은 PC 에서 Minecraft 서버가 25565 를 쓰고 있으면 먼저 끈다.

```text
A:  powershell -NoProfile -ExecutionPolicy Bypass -File Test-Transfer.ps1 -Listen
B:  powershell -NoProfile -ExecutionPolicy Bypass -File Test-Transfer.ps1 -Connect 10.100.0.1 -GenerateMiB 64
```

**통과: B 의 마지막 줄이 `VERDICT: pass`.** 출력 전체를 `b-transfer.txt` 로 받아 둔다.

**6-4. 최대 크기 패킷과 MTU.**

```text
B:  ping -n 4 -f -l 1372 10.100.0.1
B:  ping -n 1 -f -l 1373 10.100.0.1
B (클라이언트 창):  send 1 1452
```

1. `-l 1372` 는 IP 20 + ICMP 8 을 더해 어댑터 MTU 1400 바이트 패킷이다. **통과: 4개 모두 응답**
2. `-l 1373` 은 그보다 1바이트 크다. B 의 운영체제가 조각내야 한다고 거절해야 한다. 응답이 오면 어댑터 MTU 가 1400 이 아니다
3. `send 1 1452` 는 내부 패킷의 상한(`MAX_INNER`)이다. 바깥 UDP 페이로드가 1472바이트, IP 패킷이 1500바이트가 된다. A 의 캡처에서
   그 데이터그램이 조각 없이 한 개로 왔는지 받은 파일로 본다. 이것이 경로 MTU 가 1500 인지의 실측이다(`docs/kor/protocol.md` 12장)
4. 양쪽 `quit`. A 의 캡처를 Ctrl+C 로 끈다

**6-5. 바이트 카운터와 캡처 대조.** 판정 기준은 `roadmap.md` Phase 7 의 표다. **측정 경계는 A 의 클라이언트 프로세스 수명 전체다.**
카운터는 0 에서 시작하고 `quit` 때 전량이 한 번 더 나온다. 캡처를 기동 전에 켜고 종료 뒤에 끄므로 같은 구간을 본다. 그래서 6-1~6-4 와
다른 프로세스로 새로 한다. 64 MiB 전송이 섞이면 캡처가 커지고 유실될 수 있다.

```text
A (다른 cmd 창):  "C:\Program Files\Wireshark\dumpcap.exe" -i Wi-Fi -f udp -w t6c-1000.pcapng
A (다른 cmd 창):  netstat -s -p udp > a-udp-before-1000.txt
A:  sangtachi_client.exe host --name A --server <서버> 2> a6c-1000.log
B:  sangtachi_client.exe player --room <코드> --name B --server <서버> 2> b6c-1000.log
```

1. 양쪽 `to=CONNECTED` 뒤, 양쪽 클라이언트 창에서 `send 1000 20`, `send 1000 576`, `send 1000 1452` 를 차례로 친다. 한 줄마다
   `console.send sent=1000` 이 나온 뒤 다음 줄을 친다. `send` 는 한 번에 1,000 개까지다
2. **B 가 먼저 `quit` 한다.** B 의 `CLOSE` 가 A 에 닿는다. 그 뒤 B 쪽에서는 A 로 아무것도 오지 않는다. **5초 기다린다.** 그 사이 A 의 루프가
   남은 수신을 비운다
3. A `quit`. A 의 `t6c-1000.pcapng` 창을 Ctrl+C 로 끈다. dumpcap 이 마지막에 내는 `Packets dropped` 를 `a-dumpcap-1000.txt` 에 옮겨 적는다
4. A: `netstat -s -p udp > a-udp-after-1000.txt`

> **왜 B 가 먼저인가.** A 가 먼저 끝나면 B 의 keepalive 와 `PING` 이 A 의 마지막 `recvfrom` 뒤에도 와서 캡처에는 남고 카운터에는
> 없다. 캡처 유실 0 과 수신 오류 0 으로는 이것을 거르지 못해 정상 구현이 떨어진다. B 가 먼저 끝나면 A 의 종료 때까지 A 로 오는 UDP 가
> 없다. 제어 평면은 다른 소켓의 TCP 라 캡처 필터(`-f udp`)에도 포트에도 들지 않는다.

**개수는 표의 사다리를 따른다.** 1,000 개로 한 번 돌고, 유실이 0 이면 6-5 를 처음부터 다시 하면서 5,000, 10,000, 50,000 개로 늘린다. 1단계의
`send 1000 <크기>` 를 크기마다 5번, 10번, 50번 친다. 유실이 처음 0 이 아닌 단계에서 멈추고 그 앞 단계의 개수를 고른다. 단계마다 캡처와
로그와 증거 파일 이름 끝에 개수를 붙인다(`t6c-5000.pcapng`, `a6c-5000.log`, `a-udp-before-5000.txt`, `a-udp-after-5000.txt`,
`a-dumpcap-5000.txt`). 덮어쓰면 고른 단계의 유효 구간 증거가 사라진다. 위 명령은 1,000 단계의 이름이다.

판정은 받은 파일로 한다. A 의 로컬 포트 `P` 는 `a6c-1000.log` 의 `socket.bind local=0.0.0.0:<P>` 다.

```powershell
$P = <로컬 포트>
$tx = 0; & 'C:\Program Files\Wireshark\tshark.exe' -r t6c-1000.pcapng -Y "udp.srcport==$P" -T fields -e udp.length | ForEach-Object { $tx += [int]$_ - 8 }
$rx = 0; & 'C:\Program Files\Wireshark\tshark.exe' -r t6c-1000.pcapng -Y "udp.dstport==$P" -T fields -e udp.length | ForEach-Object { $rx += [int]$_ - 8 }
"capture tx=$tx rx=$rx"
Select-String -Path a6c-1000.log -Pattern '^INFO counter name=(tx_bytes|rx_bytes|tx_datagrams|rx_datagrams) ' | Select-Object -Last 4
```

**통과: dumpcap 유실 0, `a-udp-before-1000.txt` 와 `a-udp-after-1000.txt` 의 수신 오류(`Receive Errors`) 가 같고, 마지막 `tx_bytes`, `rx_bytes` 가
캡처의 합과 같다.** 유실이나 수신 오류가 늘었으면 실패가 아니라 버리고 다시 잰다. **카운터와의 대조는 아직 돌려 보지 않았다.** tshark 합산
명령은 2026-10-14 시험 1 의 캡처로 돌려 값이 나오는 것만 봤다. 그 빌드에는 바이트 카운터가 없었다.

## 모으기

공인 IP 가 들어 있으니 커밋하지 않는다. 시험마다 아래를 `YYYY-MM-DD/<측정 이름>/A`, `B` 에 모은다.

| 시험 | 파일 |
|------|------|
| 1 | A·B 의 nat-probe `results\*.json`, `a.log`, `b.log`, 양쪽 `records\*.log`, A 의 `t1.pcapng` 와 `a-ports.txt` |
| 2 | `a2.log`, `b2.log`, 양쪽 `records\*.log` |
| 3 | 상대의 `results\*.json` |
| 4 | A·B 의 `results\*.json` |
| 5 | `a5.log`, `b5.log`, 양쪽 `records\*.log`, A 의 `t5.pcapng` |
| 6 | `a6.log`, `b6.log`, 양쪽 `records\*.log`, 6-0 의 출력, A 의 `t6.pcapng`, 개수 단계마다의 `t6c-<개수>.pcapng`, `a6c-<개수>.log`, `b6c-<개수>.log`, `a-udp-before-<개수>.txt`, `a-udp-after-<개수>.txt`, `a-dumpcap-<개수>.txt`, B 의 `b-transfer.txt` 와 `ping` 출력 |

A 쪽 파일은 흩어져 생긴다. nat-probe JSON 은 `tools\nat-probe\results\` 에, `t1.pcapng` 와 `a-ports.txt` 는 그 명령을 친
폴더에 생긴다.
