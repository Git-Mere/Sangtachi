# field-test

사람이 직접 하는 실측을 모두 여기서 한다. 시험 절차, 날짜별 보고서, 원본 데이터가 한 폴더에 있다.
같은 기기에서 되는 것은 이미 자동으로 돈다(`scripts/test.ps1`, 캡처 C0~C5). 무엇이 남았는지는
[`docs/kor/plan.md`](../docs/kor/plan.md) 가 갖는다.

## 기록하는 법

```text
field-test/
  README.md                  이 파일. 준비와 시험 절차
  TEMPLATE.md                보고서 양식
  nat-probe-index.md         9월 nat-probe 실측의 라벨 표
  YYYY-MM-DD/                실측한 날 (현지 날짜)
    <측정 이름>.md            보고서 하나. TEMPLATE.md 를 복사해 채운다
    <측정 이름>/              그 측정의 원본. 받은 그대로 둔다
```

1. 실측하기 전에 날짜 폴더를 만들고 보고서의 요약 표와 "방법" 을 먼저 채운다
2. 실측한 뒤 원본을 `<측정 이름>/` 에 모으고 "결과" 와 "다음" 을 채운다
3. 남은 일이 생기면 `plan.md` 에 옮긴다

**이 README 와 `TEMPLATE.md` 만 커밋한다** (`.gitignore`). 보고서와 원본에는 실측 시각과 기록이 있고 원본에는 두 집의
공인 IP 가 있다. 공개 레포다. 보고서를 밖에 낼 때의 규칙은 [ADR 0001](../docs/kor/decisions/0001-직접-연결-대비책-미도입.md)
"따라야 할 조치" 2번이다.

## 시험 목록

| 시험 | 어디서 | 시간 | 이 한 번으로 보는 항목 |
|------|--------|:---:|------------------------|
| 1 | 서로 다른 두 집 (원격, 메신저로 맞춤) | 약 40분 | `tools/nat-probe` 재측정(호스트 네 쌍), 로비 체류가 매핑에 미치는 영향, Phase 3 산출물, Phase 4 "서로 다른 가정망 양방향", 캡처 D1(AWS 미경유)·D2(배포 환경 NFR-9), M-2, 기록 파일 |
| 2 | 같은 공유기 아래 두 PC (함께 있을 때) | 약 5분 | Phase 4 "같은 LAN 두 PC", 캡처 D3 |
| 3 (선택) | 시연 상대의 집 | 약 10분 | 시연 상대 망의 매핑 수명. 상대가 이미 잰 두 집이 아닐 때만 |
| 4 (선택) | 시험 1 과 같이 | 약 20분 | 방화벽 인바운드 실측. ADR 0002 를 다시 열 때만 |

캡처 C6~C9 는 새 가짜 피어 모드가 있어야 해서 여기 없다(`plan.md` 의 대기).

## 준비 (한 번)

**키트.** 친구에게 폴더 하나를 준다. 실행 파일은 C 런타임을 정적으로 넣은 Release 빌드라 Visual Studio 없이 돈다.

```powershell
. scripts\vsdevshell.ps1
cmake -S . -B C:\me\build-field -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DSANGTACHI_BUILD_TESTS=OFF
cmake --build C:\me\build-field --target sangtachi_client
# 키트 = C:\me\build-field\client\sangtachi_client.exe + tools\nat-probe\natprobe.py (시험 3 상대에게는 tools\nat-lifetime\natlife.py)
```

**두 PC 모두.**

- 키트를 쓸 수 있는 폴더(바탕화면 등)에 둔다. `Program Files` 아래는 안 된다(실행 파일 옆에 `records\` 를 만든다)
- Python 3.8 이상
- VPN 을 끈다. **Tailscale 도 끈다.** 켜 두면 nat-probe 결과가 오염된다
- 첫 실행 때 뜨는 Windows 방화벽 창에서 **허용**을 누른다. 거부하면 펀치가 늘 실패한다
- 명령은 **cmd** 에서 친다(PowerShell 5.1 은 `2>` 로 돌린 로그를 깨뜨린다)
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
A:  sangtachi_client.exe host --name A --server <서버> 2> a.log
B:  sangtachi_client.exe player --room <A 의 ROOM 코드> --name B --server <서버> 2> b.log
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
A:  sangtachi_client.exe host --name A --server <서버> 2> a2.log
B:  sangtachi_client.exe player --room <코드> --name B --server <서버> 2> b2.log
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

## 모으기

공인 IP 가 들어 있으니 커밋하지 않는다. 시험마다 아래를 `YYYY-MM-DD/<측정 이름>/A`, `B` 에 모은다.

| 시험 | 파일 |
|------|------|
| 1 | A·B 의 nat-probe `results\*.json`, `a.log`, `b.log`, 양쪽 `records\*.log`, A 의 `t1.pcapng` 와 `a-ports.txt` |
| 2 | `a2.log`, `b2.log`, 양쪽 `records\*.log` |
| 3 | 상대의 `results\*.json` |
| 4 | A·B 의 `results\*.json` |

A 쪽 파일은 흩어져 생긴다. nat-probe JSON 은 `tools\nat-probe\results\` 에, `t1.pcapng` 와 `a-ports.txt` 는 그 명령을 친
폴더에 생긴다.
