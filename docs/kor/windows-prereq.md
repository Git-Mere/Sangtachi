# Windows 사전조건

클라이언트를 처음 돌리는 PC에서 무엇이 갖춰져 있어야 하는지, 무엇이 자동으로 처리되고
무엇을 사람이 미리 해둬야 하는지를 적는다.

> English version: [`../eng/windows-prereq.md`](../eng/windows-prereq.md)

## 0. 읽는 법

**절마다 확인 방법과 통과 조건이 있다.** 무엇을 보면 통과인지 없으면 그 절은 미완이다.

대부분은 명령이지만 **명령이 없는 절도 있다.** 11절(LAN 탐색)은 확인할 상태가 아니라 사람이
하는 우회 절차이므로, 명령 대신 절차와 통과 조건을 적는다.

**1~7절은 갖춰지지 않으면 진행할 수 없다. 8~13절은 알고 있어야 하는 제약이다.** 앞은
막히면 멈추고, 뒤는 증상을 알아보지 못하면 엉뚱한 곳을 고치게 된다.

각 절 머리에 검증 상태를 적는다.

| 표시 | 뜻 |
|------|-----|
| **실측** | Windows 11 24H2 한 대에서 확인 명령을 돌렸고 통과 조건까지 그 출력으로 판정된다 |
| **부분 실측** | 명령은 돌렸으나 **통과 조건이 아직 판정되지 않는다.** 무엇이 빠졌는지 적는다 |
| **미검증** | 돌려 보지 않았다. 사유와 무엇이 있으면 검증되는지 적는다 |

현재 실측 4절, 부분 실측 4절, 미검증 5절이다.

**본문에 측정 날짜를 적지 않는다.** 실측의 원본 기록은 `commit_history/` 와
`tools/nat-probe/records/` 에 있다. 이 문서는 현재 사양과 판정 기준만 담는다.

**조회 실패와 "없음" 을 구분한다.** 조회의 종류마다 처리가 다르다.

- 진행 여부를 가르는 조회(3절, 8절)는 `ObjectNotFound` 만 0 으로 돌리고 나머지 오류는 올린다
- 2절과 7절은 판정이 스크립트에 있고, 그 스크립트는 같은 구분을 `unknown` 판정으로 낸다.
  `unknown` 은 통과가 아니다
- 참고용 조회(13절의 정책 값과 `Zone.Identifier`)는 `-ErrorAction SilentlyContinue` 를 쓰고,
  없는 것이 정상이라는 뜻을 그 자리에 적는다

> **왜.** 권한 부족을 "없음" 으로 읽으면 잔존물을 못 본 채 진행한다.

**"실측" 은 명령이 이 기기에서 돈다는 뜻이지 시연 산출물로 확인했다는 뜻이 아니다.**
클라이언트 어댑터는 아직 존재하지 않으므로, 우리 어댑터를 대상으로 하는 통과 조건은
Phase 6 이후에 실물로 확인한다.

**한 대에서 나온 출력은 그 한 대의 사실이다.** 어댑터 이름, 메트릭, 대역은 기기마다 다르다.
비교할 것은 값 자체가 아니라 **판정 기준**이다. 기준을 각 절에 적었다.

초기화된 PC에서의 종단 검증은 Phase 8 시연 준비 시점이며 이 문서의 완료 조건이 아니다.
Phase 1 구현이 아직 시작되지 않아 클라이언트가 하는 자동 처리는 **설계상의 구분**이고
동작 확인이 아니다.

## 1. 관리자 권한

**검증: 실측**

Wintun 어댑터 생성, IP 할당, 라우트 추가는 전부 관리자 권한을 요구한다. **이것은 Windows API
전제이고 이 기기에서 확인한 것이 아니다.** 여기서 실측한 것은 셸이 승격됐는지 여부뿐이다.

세 연산 중 어디서 어떤 오류로 실패하는지는 Phase 6 에서 어댑터를 처음 만들 때 확인한다.

**분류: 사람.** 클라이언트가 스스로 권한을 올릴 수는 있으나(UAC 재실행) 사용자가 동의해야 한다.

```powershell
([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
  [Security.Principal.WindowsBuiltInRole]::Administrator)
```

기대 출력은 `True` 다. 관리자로 띄우지 않은 PowerShell 에서는 `False` 가 나온다.

**판정 기준.** `True` 여야 진행한다. `False` 면 클라이언트는 어댑터를 만들기 전에 중단하고
무엇이 필요한지 알린다. **어댑터를 만들다 실패하는 것보다 시작 전에 막는 편이 낫다.**
중간에 실패하면 3절의 잔존물이 생긴다.

**이 요구는 이미 존재한다.** 뒤에 나오는 방화벽 규칙 등록(2절)이 관리자 권한을 *새로*
만드는 것이 아니다. [ADR 0002](decisions/0002-리바인딩-복구-미보장.md)가 옵션 a의 비용으로
"관리자 권한이 생긴다"를 들었으나, 어댑터 생성 때문에 이미 필요하다. 한계 비용은
"이미 있는 권한으로 규칙을 하나 더 만든다" 이다.

## 2. 방화벽과 네트워크 프로필

**검증: 부분 실측.** 기본 인바운드 정책과 기존 어댑터의 프로필 분류는 실제로 봤다.
**새로 만든 가상 어댑터가 Public 으로 분류되는 것은 관측하지 못했다.** 어댑터가 아직 없다.
Phase 6 에서 확인한다.

Windows 방화벽은 **인바운드 기본 차단**이다. 새로 만든 가상 어댑터는 게이트웨이가 없어
"식별되지 않은 네트워크"로 분류되고, 그러면 Public 프로필이 붙는다. Public 에서는 ICMP 에코
요청과 TCP 25565 인바운드가 막힌다.

터널 UDP 는 방향에 따라 다르다.

- 우리가 먼저 보낸 흐름에 대한 응답은 상태 기반 처리로 통과한다. 실측으로 확인했다
- **요청하지 않은 인바운드는 통과하지 않는다.** 같은 실측에서 6건 전부 `blocked` 였다.
  그쪽이 필요하면 인바운드 허용 규칙이 있어야 한다

**그 6건이 방화벽 때문이라고 단정하지 않는다.** 판정은
[`tools/nat-probe/unsolicited-firewall-test.ps1`](../../tools/nat-probe/unsolicited-firewall-test.ps1)
가 Windows 필터링 플랫폼에 물어본 결과이고, 방화벽을 끈 대조군은 돌리지 않았다. 방화벽 단독
귀속은 상대 피어가 생겨 인바운드 실측을 돌릴 때 확인한다.

**두 문서가 같은 현상을 말하는 것으로 읽지 않는다.**

- [`protocol.md`](protocol.md) 10.4 엔드포인트 학습은 같은 성질의 관측(출발지 포트만 다른
  UDP 가 도착하지 않음)을 NAT 의 포트 제한 필터링으로 설명한다
- 여기의 6건은 로컬 정책 조회이고 그쪽은 망을 건넌 실측이다. 경로가 다르면 원인도 다를 수 있다

**분류: 클라이언트 + 사람.** 프로필 분류와 규칙 등록은 관리자 권한이 있으면 클라이언트가
할 수 있다. Minecraft(Java) 쪽 예외는 12절 때문에 사람이 확인해야 한다.

### 기본 정책 확인

판정은 [`tools/winprereq/Test-FirewallPolicy.ps1`](../../tools/winprereq/Test-FirewallPolicy.ps1)
에 있다. 케이스 225건은 스크립트 안에 있고 `-SelfTest` 로 돈다.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\winprereq\Test-FirewallPolicy.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File tools\winprereq\Test-FirewallPolicy.ps1 -InterfaceAlias '<어댑터 이름>'
powershell -NoProfile -ExecutionPolicy Bypass -File tools\winprereq\Test-FirewallPolicy.ps1 -SelfTest
```

**통과 조건. 마지막 줄이 `VERDICT: pass` 이고 종료 코드가 0이다.** `unknown` (코드 2) 은 통과가
아니라 판정 불가다. 우리 어댑터가 없는 Phase 6 이전에는 `-InterfaceAlias` 를 주면 `unknown` 이
정상이다. 확인된 결함이 하나라도 있으면 판정 불가가 같이 있어도 `fail` 이다.

스크립트가 보는 것은 셋이다. **무엇을 보는지가 이 절의 몫이고, 어떻게 세는지는 스크립트의 몫이다.**

- 세 프로필의 기본 인바운드 정책이 차단인가
- **`BlockInboundAlways` 가 아닌가.** 그 모드는 인바운드 허용 규칙을 전부 무시한다. 규칙을
  등록해도 아무 일이 일어나지 않으므로 이 절과
  [ADR 0002](decisions/0002-리바인딩-복구-미보장.md) 옵션 a 가 통째로 무력해진다
- **프로필 방화벽이 켜져 있는가.** 정책이 차단이라도 방화벽이 꺼져 있으면 인바운드가 막히지
  않는다. 그러면 "막혔다" 는 관측의 원인이 방화벽이 아니게 된다

이 기기의 실측 출력이다.

```
Domain Profile Settings:
----------------------------------------------------------------------
Firewall Policy                       BlockInbound,AllowOutbound

Private Profile Settings:
----------------------------------------------------------------------
Firewall Policy                       BlockInbound,AllowOutbound

Public Profile Settings:
----------------------------------------------------------------------
Firewall Policy                       BlockInbound,AllowOutbound
Ok.
```

세 프로필 모두 `Enabled: True` 였다.

**영어 낱말로 줄을 거르지 않는다.** 값으로만 판정한다.

> **왜.** `netsh` 의 레이블은 표시 언어에 따라 번역되므로 `Select-String 'Profile
> Settings|State'` 같은 필터는 한국어 표시 Windows 에서 아무것도 못 잡는다. 값인
> `BlockInbound` 는 키워드라 번역되지 않는다.

> **`Get-NetFirewallProfile` 로 판정하지 않는다.** 같은 기기에서 이렇게 나온다.
>
> ```
> Name                 : Public
> Enabled              : True
> DefaultInboundAction : NotConfigured
> ```
>
> `NotConfigured` 는 "설정 안 됨"이 아니라 "내장 기본값을 쓴다"이고, 그 기본값이 Block 이다.
> 그래서 `DefaultInboundAction -eq 'Block'` 으로 검사를 쓰면 정상 기기에서 거짓이 나온다.
> 재발 방지 규칙 3번(정상 구현이 통과하는지부터 확인)에 그대로 걸리는 자리다. 스크립트는 이
> 반례를 케이스로 고정해 두었다.

### 프로필 분류 확인

```powershell
Get-NetConnectionProfile | Select-Object InterfaceAlias, NetworkCategory, IPv4Connectivity
```

실측 출력이다. 물리 Wi-Fi 가 이미 `Public` 이고, 다른 터널 어댑터(Tailscale)는 `Private` 다.

```
InterfaceAlias NetworkCategory IPv4Connectivity
-------------- --------------- ----------------
Wi-Fi                   Public         Internet
Tailscale              Private     LocalNetwork
```

**판정 기준.** 우리 어댑터가 생긴 뒤 그 항목의 `NetworkCategory` 를 본다. `Public` 이면
Private 로 바꾸거나, 프로필과 무관하게 `-Profile Any` 규칙을 등록한다.

**프로필을 바꾸는 대신 범위를 좁힌 규칙을 쓴다.** 프로필을 바꾸는 것은 그 어댑터에 붙은
*모든* Private 규칙을 여는 것이라 노출이 크고, **식별되지 않은 네트워크에서는 로컬 보안 정책
때문에 아예 실패할 수 있다.** 게이트웨이 없는 Wintun 어댑터가 바로 그 경우다.

**`-Profile Any` 만으로는 부족하다.** 그것은 "어느 프로필에서도 걸린다" 는 뜻이라 물리
네트워크에서도 열린다. **우리 어댑터와 정확한 프로토콜·포트로 범위를 좁힌다.**

```powershell
New-NetFirewallRule -DisplayName 'sangtachi-tunnel-in' `
  -Direction Inbound -Action Allow -Profile Any `
  -InterfaceAlias '<어댑터 이름>' -Protocol TCP -LocalPort 25565
```

`-InterfaceAlias` 가 없으면 같은 규칙이 Wi-Fi 에도 걸린다. ICMP 를 열어야 하면 같은 방식으로
`-Protocol ICMPv4 -IcmpType 8` 을 쓴다. **종류까지 적는다.** 종류를 안 적으면 모든 ICMP 를
여는 규칙이 되고, 스크립트가 `wide` 나 `unknown` 으로 판정한다.

**`-InterfaceAlias` 를 못 쓰는 기기가 있다.** 그런 기기에서는 이 절의 조치를 그대로 따를 수
없다. 스크립트의 `scope-param` 줄이 그것을 판정한다.

**규칙은 둘 다 만들고 켠 상태로 둔다.** TCP 25565 와 ICMP 에코가 각각 필요하고, 하나만 열면
다른 쪽이 안 된다.

- 꺼진 규칙은 덮음으로 세지 않는다. 스크립트의 `endpoint-coverage` 줄이 둘 다 덮였는지
  판정한다
- 넓은 규칙이나 엉뚱한 어댑터에 묶인 규칙은 덮은 것으로 치지 않는다
- 규칙이 제대로 좁혀졌는지는 스크립트의 `-InterfaceAlias` 모드가 판정한다

분류를 굳이 바꾸려면 이 명령이고, **실패할 수 있으므로 결과를 확인한다.**

```powershell
Set-NetConnectionProfile -InterfaceAlias '<어댑터 이름>' -NetworkCategory Private
Get-NetConnectionProfile -InterfaceAlias '<어댑터 이름>' | Select-Object NetworkCategory
```

## 3. 잔존 어댑터·주소·라우트 정리

**검증: 부분 실측.** 조회 명령은 돌렸고 남의 잔존 어댑터(OpenVPN TAP)가 실제로 보였다.
**우리 클라이언트가 비정상 종료해서 남긴 잔존물은 관측하지 못했다.** 어댑터가 아직 없다.
Phase 6 에서 확인한다.

비정상 종료하면 어댑터, 주소, 라우트가 남는다. 다음 실행에서 같은 이름으로 또 만들면
중복 생성 오류가 난다.

**분류: 클라이언트.** 시작할 때 자기 이름의 잔존물을 찾아 재사용하거나 지운다.

```powershell
Get-NetAdapter -IncludeHidden | Select-Object Name, InterfaceDescription, Status
```

실측 출력에서 잔존물이 실제로 보인다. `이더넷 4` 는 쓰지 않는 OpenVPN TAP 어댑터가
연결 끊김 상태로 남은 것이다.

```
Name                     InterfaceDescription                       Status
----                     --------------------                       ------
이더넷 4                 TAP-Win32 Adapter V9                       Disconnected
Wi-Fi                    Realtek 8852CE WiFi 6E PCI-E NIC           Up
Tailscale                Tailscale Tunnel                           Up
vEthernet (FSE HostVnic) Hyper-V Virtual Ethernet Container Adapter Up
```

**판정 기준.** 우리 어댑터 이름이 이미 있으면 새로 만들지 않는다. 재사용하거나 지우고 만든다.
**무조건 지우지 않는다.** 이름이 겹치는 남의 어댑터를 지울 수 있다.

**소유 판정은 기록해 둔 `InterfaceGuid` 하나로 한다.** 7절 서브넷 충돌과 같은 기준이다.
이름과 `InterfaceDescription` 은 소유의 증거가 아니다. 둘 다 남이 똑같이 쓸 수 있다.

> **왜.** 여기서 틀리면 남의 어댑터를 지운다. 7절이 같은 기준으로 계산에서 빼는 것보다 이쪽이
> 더 파괴적이므로 기준을 낮출 이유가 없다.

| 상태 | 조치 |
|------|------|
| 기록된 GUID 와 같은 어댑터가 있다 | 우리 것이다. 재사용하거나 지우고 만든다 |
| 기록이 없다 (첫 실행, 상태 파일 유실) | **지우지 않는다.** 이름이 비어 있으면 그 이름으로 만들고, 이미 쓰이고 있으면 다른 이름으로 만든다 |
| 기록은 있는데 그 GUID 의 어댑터가 없다 | 이미 사라진 것이다. 기록을 지우고 새로 만든다. 같은 이름의 남의 어댑터가 보여도 건드리지 않는다 |

**주소와 라우트의 잔존물도 같은 기준으로 지운다.** 그 GUID 의 인터페이스에 붙은 것만
대상이다. 대역(`10.100.0.0/24`)만 보고 지우면 같은 대역을 쓰는 남의 어댑터를 건드린다.

**세 가지를 다 본다.** 어댑터만 지우면 주소와 라우트가 따라 사라지지만, 어댑터가 살아 있고
주소만 남은 중간 상태가 있다.

```powershell
function Invoke-NetQuerySafe([scriptblock]$Query) {
  try { return @(& $Query) }
  catch {
    $isNoMatch = ($_.CategoryInfo.Category -eq 'ObjectNotFound') -and
                 ($_.FullyQualifiedErrorId -like 'CmdletizationQuery_NotFound*')
    if ($isNoMatch) { return @() }
    throw
  }
}
@(Invoke-NetQuerySafe { Get-NetAdapter   -Name '<어댑터 이름>'           -ErrorAction Stop }).Count
@(Invoke-NetQuerySafe { Get-NetIPAddress -InterfaceAlias '<어댑터 이름>' -ErrorAction Stop }).Count
@(Invoke-NetQuerySafe { Get-NetRoute     -InterfaceAlias '<어댑터 이름>' -ErrorAction Stop }).Count
```

**판정 기준. 셋 다 `0` 이어야 깨끗한 상태다.**

**`-ErrorAction SilentlyContinue` 로 "없음" 을 판정하지 않는다.** 8절 on-link 라우트 중복
생성의 `Get-RouteSafe` 와 같은 구분이며, 셋을 하나의 래퍼로 묶은 것이다.

> **왜.** 권한 부족이나 서비스 이상도 같은 무출력이 되어 잔존물을 못 본 채 깨끗하다고
> 보고한다.

실측 확인이다. 없는 이름으로 셋을 돌리면 전부 `0` 이고 실재 어댑터는 `1` 이었다. 원래 오류는
셋 다 `Category=ObjectNotFound` 이고 ID 는 각각
`CmdletizationQuery_NotFound_Name,Get-NetAdapter`,
`CmdletizationQuery_NotFound_InterfaceAlias,Get-NetIPAddress`,
`CmdletizationQuery_NotFound_InterfaceAlias,Get-NetRoute` 였다.

## 4. Wintun 패키징과 서명

**검증: 미검증.** Wintun 을 아직 도입하지 않았다. Phase 6 작업이다. 아래 명령은 `wintun.dll`
을 배포물에 넣은 뒤에 돌릴 수 있다.

Wintun 은 DLL 하나로 배포되고 그 안에 드라이버가 들어 있다. **아키텍처가 맞아야 한다**
(x64 클라이언트에는 `bin/amd64/wintun.dll`).

**분류: 사람(빌드 시) + 클라이언트(실행 시 확인).**

```powershell
$sig = Get-AuthenticodeSignature .\wintun.dll
$sig.Status
$sig.SignerCertificate.Subject
Get-FileHash .\wintun.dll -Algorithm SHA256 | Select-Object -ExpandProperty Hash
```

**판정 기준. 세 가지가 모두 맞아야 한다.**

1. `Status` 가 `Valid`. `NotSigned` 나 `HashMismatch` 면 배포물이 손상됐거나 잘못된 파일이다
2. `Subject` 가 **기대한 발행자**여야 한다. `Valid` 는 "신뢰된 누군가가 서명했다" 는 뜻이므로,
   서명된 다른 DLL 을 끼워 넣어도 통과한다. Wintun 공식 배포물의 발행자 문자열을 배포 시점에
   적어 두고 그것과 비교한다
3. 해시가 **받아온 릴리스의 공개 해시**와 같아야 한다

형태 확인이다. 이 기기의 `kernel32.dll` 로 돌려 `Status: Valid`,
`Subject: CN=Microsoft Windows, O=Microsoft Corporation, ...` 를 얻었고, 기대 발행자를
`CN=WireGuard LLC` 로 두면 `False` 가 나오는 것까지 봤다. **`Status` 만 보면 이 검사가
통과한다는 뜻이다.**

**이것은 DLL 의 출처 확인이지 드라이버가 올라간다는 보장이 아니다.** Authenticode 서명은
DLL 파일에 대한 것이고, 커널 드라이버는 catalog 서명과 드라이버 서명 정책을 따로 통과해야
한다. **드라이버 적재 성공 여부는 어댑터 생성 결과로 확인한다.** 실패하면 그 원인은
`C:\Windows\INF\setupapi.dev.log` 에 남는다.

아키텍처는 PE 헤더에서 읽는다.

```powershell
$fs = $null; $br = $null
try {
  $fs = [IO.File]::OpenRead('.\wintun.dll')
  $br = New-Object IO.BinaryReader($fs)
  if ($fs.Length -lt 0x40) { throw 'too small' }
  $fs.Position = 0x3C
  $pe = $br.ReadInt32()
  if ($pe -lt 0x40 -or $pe + 6 -gt $fs.Length) { throw "bad e_lfanew $pe" }
  $fs.Position = $pe
  if ($br.ReadUInt32() -ne 0x00004550) { throw 'no PE signature' }   # 'PE\0\0'
  '0x{0:X}' -f $br.ReadUInt16()
}
finally {
  if ($br) { $br.Close() }
  elseif ($fs) { $fs.Close() }
}
```

**판정 기준.** `0x8664` 여야 한다. `0x14C` 는 x86, `0xAA64` 는 ARM64 다. **그 셋이 아니거나
`throw` 가 나면 판정 불가이고 통과가 아니다.**

**서명 네 바이트를 먼저 확인하는 이유.** `0x3C` 의 값을 믿고 바로 읽으면 PE 가 아닌 파일이나
잘린 파일에서도 **어떤 2바이트가 나오고 그것이 판정으로 쓰인다.** 우연히 `0x8664` 가 나오면
틀린 통과다. 길이 검사와 `finally` 도 같은 종류다. 규칙 7의 "판정을 내는 절차는 검증을 먼저
쓴다" 가 문서 안의 절차에도 걸린다.

**막혔을 때.** 드라이버 적재 실패는 어댑터 생성 API 가 실패하는 형태로 나타난다. 이때
관리자 권한 부족(1절)과 구분되지 않으므로, 클라이언트는 두 조건을 **각각** 확인해 무엇이
원인인지 말해야 한다.

## 5. Minecraft 서버 바인드 주소

**검증: 미검증.** Minecraft 서버를 아직 돌려 보지 않았다. Phase 8 작업이다.

`server.properties` 의 `server-ip` 가 **다른 인터페이스 주소**로 지정돼 있으면 가상 IP
`10.100.0.1:25565` 로 들어오는 접속이 거부된다. 비워 두면 모든 인터페이스에 바인드한다.

**분류: 사람.** 서버 쪽 설정 파일이다.

`server.properties` 에 이렇게 둔다.

```
server-ip=
server-port=25565
```

확인은 서버를 띄운 뒤 클라이언트 PC 에서 한다.

```powershell
Test-NetConnection -ComputerName 10.100.0.1 -Port 25565
```

**판정 기준.** `TcpTestSucceeded : True` 여야 한다. `False` 면 순서대로 본다.

1. 서버 프로세스가 그 주소에 바인드했는가 — 아래 "바인드 확인" 을 본다
2. 서버 PC 방화벽이 25565 인바운드를 막는가 — 2절 방화벽과 네트워크 프로필
3. 터널이 그 패킷을 옮기는가 — 터널 계층 문제이지 이 절이 아니다

**바인드 확인.** 서버 PC 에서 아래를 돌린다.

```powershell
Invoke-NetQuerySafe { Get-NetTCPConnection -LocalPort 25565 -State Listen } |
  Select-Object LocalAddress, LocalPort, OwningProcess
```

3절의 `Invoke-NetQuerySafe` 로 감싸는 이유는 0절이 정한 대로 **조회 실패와 "없음" 을 구분**
하기 위해서다. 이 조회는 진행 여부를 가르는 판정에 쓰이므로 `-ErrorAction SilentlyContinue`
로 무출력을 만들면 "리스닝이 없다" 와 "조회가 실패했다" 가 같은 모양이 된다.

`LocalAddress` 가 `0.0.0.0` 또는 `10.100.0.1` 이면 통과다. 다른 주소 하나만 있으면 이 절의
문제다. **출력이 비어 있으면 리스닝이 없다는 뜻이고 서버가 뜨지 않았거나 다른 포트를 쓴다.**
이 절의 문제가 아니다.

**`::` 만 보이면 판정 불가다.** IPv6 와일드카드가 IPv4 까지 받는지는 소켓의 이중 스택 설정에
달렸고 이 조회로는 알 수 없다. 그때는 위의 `Test-NetConnection 10.100.0.1 -Port 25565` 결과가
판정이다.

**`netstat -ano | findstr 25565` 는 쓰지 않는다.** 원격 포트가 25565 인 줄도 걸리고
리스닝인지 아닌지 구분되지 않는다.

## 6. 제어 평면 EC2

**검증: 미검증.** EC2 인스턴스를 아직 띄우지 않았다. Phase 3 작업이다.

세 가지가 각각 접속을 막는다.

| 항목 | 증상 |
|------|------|
| 보안 그룹에 인바운드 규칙이 없다 | 연결이 타임아웃된다. 거부가 아니라 무응답이다 |
| 서버가 `127.0.0.1` 에 바인드했다 | 인스턴스 안에서는 되고 밖에서는 안 된다 |
| 공인 IP 가 재시작으로 바뀌었다 | 10절 |

**분류: 사람.**

인스턴스 안에서 바인드 주소를 본다. **제어 평면은 Linux EC2 다**([`spec.md`](spec.md)).
포트 8000 의 출처는 [`control_plane.md`](control_plane.md) 2.6 상수의 `CONTROL_PORT` 다.
그 값이 바뀌면 아래 명령의 포트도 따라 바뀐다. 아래는 Linux 명령이다.

```bash
ss -ltnp 'sport = :8000'
```

**판정은 IPv4 전용 조회로 한다.**

```bash
ss -ltnp4 'sport = :8000'
```

**판정 기준. `0.0.0.0:8000` 또는 `*:8000` 이 나와야 한다.** 아무것도 안 나오면 IPv4 로는
받지 않는 것이다. `127.0.0.1:8000` 이면 인스턴스 안에서만 닿는다.

**위 조회에서 `[::]:8000` 만 보이는 경우를 통과로 단정하지 않는다.** 그래서 IPv4 전용 조회를
판정으로 쓰고, 최종 확인은 아래 외부 접속으로 한다.

> **왜.** Linux 기본값(`net.ipv6.bindv6only=0`)에서는 이중 스택 소켓이 IPv4 도 받지만 그
> sysctl 은 바꿀 수 있다.

클라이언트 PC 에서 닿는지 본다.

```powershell
Test-NetConnection -ComputerName <EC2 공인 주소> -Port 8000
```

**판정 기준.** `TcpTestSucceeded : True`. 타임아웃이면 보안 그룹부터 본다.

## 7. 서브넷 충돌

**검증: 부분 실측.** 판정은 [`tools/winprereq/Test-SubnetOverlap.ps1`](../../tools/winprereq/Test-SubnetOverlap.ps1)
에 있고 이 기기에서 돌렸다. 케이스 85건은 스크립트 안에 있고 `-SelfTest` 로 돈다.
**살아 있는 Wintun 어댑터를 실제로 제외하는 경로는 관측하지 못했다.** 어댑터가 아직 없다.
그 경로는 케이스 2건으로만 확인했고 실물 확인은 Phase 6 이다.

`10.100.0.0/24` 가 실제 LAN, Hyper-V 가상 스위치, 다른 VPN 과 겹치면 트래픽이 엉뚱한
인터페이스로 가거나, 반대로 **우리가 남의 대역을 가린다.** 어댑터를 만들기 전에 확인한다.

**분류: 클라이언트.** 차단 조건이면 중단한다. 아래 "막혔을 때" 를 본다.

**Windows 는 최장 접두사 일치로 라우트를 고른다.** 우리가 만들 것은 `/24` on-link 라우트다.
겹치는 라우트를 접두사 길이로 세 부류로 나눈다.

**이 표가 판정 기준의 출처다.**

| 겹치는 기존 라우트 | 최장 일치 결과 | 판정 |
|--------------------|----------------|------|
| 길이 `> 24` (`10.100.0.5/32`) | **기존 것이 이긴다** | **차단** (`BLOCK-route`) |
| 길이 `= 24` (`10.100.0.0/24`) | 같은 길이라 **메트릭이 가른다.** 생성 자체가 중복으로 실패할 수도 있다 | **차단** (`BLOCK-route`) |
| 길이 `2~23` (`10.0.0.0/8`) | 우리 `/24` 가 이긴다 | **차단** (`BLOCK-shadow`). 우리가 이기는 것이 문제다. 그 대역의 실제 호스트를 가린다 |
| 길이 `0~1` (`0.0.0.0/0`, 풀터널 VPN 의 `0.0.0.0/1`) | 우리 `/24` 가 이긴다 | **정상.** `/0` 은 인터넷에 연결된 보통의 기기에 있고 `/1` 은 풀터널 VPN 에서 나온다 |

**두 번째 줄을 경고가 아니라 차단으로 둔다.** 우리 `/24` 가 이기므로 터널은 동작하지만,
사용자가 원래 닿던 `10.100.0.x` 호스트에 닿지 못하게 된다. **동작하는데 남의 것을 끊는 것이
동작하지 않는 것보다 낫지 않다.** 그 대역이
실제로 비어 있다는 것을 사람이 아는 경우를 위해 `-AllowShadow` 를 둔다. 기본값은 차단이다.

**문자열 비교로 판정하지 않는다.** `DestinationPrefix -like '10.100.0.*'` 는 **겹치는데 문자열
이 다른 것을 놓친다.** 확인한 값이다.

```
10.100.0.0/24    like-match=True
10.100.0.5/32    like-match=True
10.100.0.0/23    like-match=True
10.100.0.0/16    like-match=True
10.0.0.0/8       like-match=False   <- 겹치는데 놓친다
0.0.0.0/1        like-match=False   <- 겹치는데 놓친다
```

`10.0.0.0/8` 은 우리 대역을 덮으므로 `BLOCK-shadow` 인데 문자열로는 걸리지 않는다. 더 근본적인
문제는 **문자열이 접두사 길이를 말해 주지 않는다**는 것이다. 위 세 부류를 가르려면 길이가
필요하므로 파싱해서 비교한다.

**확인 방법.**

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\winprereq\Test-SubnetOverlap.ps1 -Target 10.100.0.0/24
powershell -NoProfile -ExecutionPolicy Bypass -File tools\winprereq\Test-SubnetOverlap.ps1 -SelfTest
```

**통과 조건. 마지막 줄이 `VERDICT: proceed` 이고 종료 코드가 0이다.**

- `pick another range` (코드 1) 는 대역을 바꾼다
- **`unknown` (코드 2) 은 통과가 아니다.** 조회나 해석이 실패했다는 뜻이다. 원인을 없애고 다시
  돌린다. 확실한 차단이 함께 있으면 `pick another range` 가 이긴다
- 대상 대역을 `-Target` 으로 바꿔도 판정이 따라간다. 주소도 `/32` 로 만들어 같은 겹침 검사에
  넣으므로 `/23` 이나 `/20` 대체 대역에서 조용히 틀리지 않는다. **길이는 2~32 만 받는다.**
  `/0` 과 `/1` 은 가상 LAN 대역이 아니고, 아래 `catch-all` 면제와 겹쳐 같은 길이 충돌을 가린다

**순서가 있다. 3절 정리를 먼저 돌리고 이 검사를 한다.** 우리 어댑터가 이전 실행의 주소나
라우트를 달고 살아 있으면, 그것이 우리 대역과 겹쳐서 **자기 자신 때문에 "대역을 바꿔라" 가
나온다.** 그래서 우리 어댑터를 계산에서 뺀다.

**이름과 드라이버 설명은 소유의 증거가 아니다.** 둘 다 남이 똑같이 쓸 수 있다. 우리 이름을 쓰는
남의 Wintun 어댑터를 우리 것으로 읽으면 **진짜 충돌이 조용히 빠진다.** 소유 표식은
`InterfaceGuid` 하나다.

**클라이언트가 지켜야 할 계약이다.**

- 어댑터를 만들 때 쓴 `InterfaceGuid` 를 자기 상태에 기록한다. Wintun 은 생성 시 GUID 를 지정할
  수 있으므로 값을 아는 쪽은 클라이언트다
- 이 검사를 부를 때 그 값을 `-OwnInterfaceGuid` 로 넘긴다. `-OwnAlias` 는 제외 근거가 아니라
  이름이 같은지 한 번 더 보는 용도다
- 어댑터를 지우면 기록도 지운다. 낡은 GUID 는 아무것도 빼지 않으므로 판정이 엄격해지는 쪽으로만
  틀린다

GUID 로 어댑터가 정확히 하나 잡히고 그 설명이 Wintun 이며 이름까지 맞을 때만 뺀다. 하나라도
어긋나면 **아무것도 빼지 않고 그 이유를 출력에 적는다.** 조회가 실패해도 같은 쪽으로 넘어간다.

> **왜.** 거짓 통과보다 거짓 차단이 낫다. 3절 잔존 어댑터·주소·라우트 정리의 "이름이 겹치는
> 남의 어댑터를 지우지 않는다" 와 같은 규칙이다.

이 기기의 실측 출력이다.

```
Verdict   Len Prefix    Interface
-------   --- ------    ---------
catch-all   0 0.0.0.0/0 Wi-Fi

BLOCK routes: 0
BLOCK addresses: 0
VERDICT: proceed
```

**판정식과 그 반례는 문서에 두지 않는다** (재발 방지 규칙 5, 6, 7). 접두사 형식 거부, `[int]''`
가 `0` 이 되는 문제, 자릿수 제한, IPv6 입력, 짧은 쪽 마스크로 비교하기, 자기 어댑터 제외까지
모두 스크립트의 케이스 표에 있다. 문서에 표를 한 벌 더 두면 한쪽만 고쳐져 어긋난다. 실제로
이전 판이 그렇게 어긋나 있었다.

**막혔을 때. v1 은 중단한다.** `ERROR` 한 줄에 겹친 라우트를 적고 끝낸다. 사용자는 그
인터페이스를 내리거나 대역을 바꾼 뒤 다시 시작한다.

**대체 대역은 아직 없다.** 대역이 바뀌면 양쪽이 같은 값을 써야 하므로 제어 평면이 알려주는
값이어야 하는데, [`control_plane.md`](control_plane.md) 4장의 연산에 그 필드가 없고
가상 IP 풀(2.5)이 대역을 상수로 고정한다. **클라이언트가 혼자 정하면 두 쪽이 다른 대역을
쓴다.** 그래서 "대체 대역을 고른다" 를 조치로 적지 않는다. 넣을지와 언제 넣을지는
[`roadmap.md`](roadmap.md) Phase 6 의 착수 전 항목이 갖는다.

## 8. on-link 라우트 중복 생성

**검증: 실측 (자동 생성 관측)**

주소를 `/24` 로 할당하면 Windows 가 on-link 라우트를 **자동으로 만든다.** 그 뒤에 같은
라우트를 명시적으로 추가하면 이미 있다는 오류가 난다.

**분류: 클라이언트.**

실측 라우트 표에서 주소 하나를 할당했을 뿐인데 세 줄이 생겨 있다.

```
10.0.0.0    255.255.255.0   On-link   10.0.0.49   286     <- 서브넷 on-link
10.0.0.49   255.255.255.255 On-link   10.0.0.49   286     <- 호스트
10.0.0.255  255.255.255.255 On-link   10.0.0.49   286     <- 브로드캐스트
```

**판정 기준.** `New-NetIPAddress -PrefixLength 24` 를 한 뒤에는 그 서브넷 라우트를 따로
만들지 않는다. 만들어야 한다면 먼저 확인한다.

```powershell
function Get-RouteSafe([string]$prefix) {
  try { return @(Get-NetRoute -DestinationPrefix $prefix -ErrorAction Stop) }
  catch {
    $isNoMatch = ($_.CategoryInfo.Category -eq 'ObjectNotFound') -and
                 ($_.FullyQualifiedErrorId -like 'CmdletizationQuery_NotFound*')
    if ($isNoMatch) { return @() }
    throw
  }
}
@(Get-RouteSafe '10.100.0.0/24').Count
```

**개수로 판정한다.** `0` 이면 만들고 아니면 만들지 않는다.

**`-ErrorAction SilentlyContinue` 로 때우지 않는다.** 없음만 0 으로 돌리고 나머지는 올린다.

> **왜.** 그러면 "없음" 과 "권한 부족이나 서비스 이상으로 못 봤음" 이 같은 결과가 된다.
> 없다고 잘못 읽고 만들면 중복 생성으로 실패하고, 그때 원인은 이미 지워져 있다.

실측 확인이다. 없는 접두사는 `0`, 있는 접두사(`10.0.0.0/24`)는 `1` 이었고, 원래 오류는
`Category=ObjectNotFound`, `FullyQualifiedErrorId=CmdletizationQuery_NotFound_DestinationPrefix,Get-NetRoute`
였다.

**오류를 삼키지 않는다.** 중복이라 무시해도 되는 오류와 권한 부족은 다르게 처리해야 한다.
3절 잔존 어댑터·주소·라우트 정리의 잔존물 조회도 같은 구분이 필요하다.

> **왜.** 형식으로 잡지 않는 이유는 `unsolicited-firewall-test.ps1` 의 `Get-TestRuleSafe` 와
> 같다. 같은 "일치 항목 없음" 이 예외 형식을 바꿔 가며 오는 사례를 그쪽에서 먼저 겪었다.

## 9. 주소 tentative 상태

**검증: 실측**

주소를 할당한 직후에는 중복 주소 검출(DAD)이 끝나지 않아 `Tentative` 상태다. 이때 바인드나
송신을 시작하면 간헐적으로 실패한다.

**분류: 클라이언트.** `Preferred` 가 될 때까지 기다린다.

```powershell
Get-NetIPAddress -AddressFamily IPv4 |
  Select-Object InterfaceAlias, IPAddress, AddressState
```

실측 출력이다. 같은 기기에 두 상태가 동시에 있다.

```
InterfaceAlias  IPAddress       AddressState
--------------  ---------       ------------
이더넷 4        169.254.187.141 Tentative
Wi-Fi           10.0.0.49       Preferred
Tailscale       100.102.127.59  Preferred
```

**판정 기준.** 우리 주소의 `AddressState` 가 `Preferred` 가 된 뒤에 다음 단계로 간다.
**고정 대기로 기다리지 않는다.** `Start-Sleep 2` 는 느린 기기에서 깨지고 빠른 기기에서
낭비다. 상태를 폴링한다.

**끝나는 조건이 셋이다. `Preferred` 하나만 기다리면 영원히 멈춘다.**

| 관측 | 조치 |
|------|------|
| `Preferred` | 다음 단계로 간다 |
| `Duplicate` 또는 `Invalid` | **실패로 중단한다.** DAD 가 충돌을 찾은 것이라 기다려도 바뀌지 않는다. 3절 정리를 돌리고 끝낸다 |
| 조회 결과가 비었거나 오류 | 어댑터가 사라졌거나 조회가 실패한 것이다. 같은 경로로 중단한다. 0절의 "조회 실패와 없음을 구분한다" 가 여기에도 걸린다 |
| 위 어느 것도 아닌 채 상한을 넘김 | 중단한다. **상한은 10초로 둔다.** DAD 는 보통 1초 안에 끝나고, 넘긴다는 것은 스택이 응답하지 않는다는 뜻이다 |

중단하는 모든 경로에서 3절 정리를 돌린다. 그러지 않으면 어댑터·주소·라우트가 남는다.

**대안.** 점대점 터널 어댑터에는 DAD 가 의미가 적다. 끄면 `Tentative` 단계가 사라진다.

```powershell
Set-NetIPInterface -InterfaceAlias '<어댑터 이름>' -AddressFamily IPv4 -DadTransmits 0
```

이 값은 **그 인터페이스에만** 적용된다. 껐다면 왜 껐는지 코드 주석에 남긴다.

## 10. EC2 공인 IP 변경

**검증: 미검증.** 인스턴스를 아직 띄우지 않았다.

**자동 할당된** 공인 IPv4 는 인스턴스를 중지했다 켜면 바뀐다. 클라이언트에 주소를 박아 두면
그날 시연이 깨진다. **Elastic IP 를 붙이면 바뀌지 않는다.** 그것이 아래 조치다.

**분류: 사람.** Elastic IP 를 붙이고 DNS 이름이 그것을 가리키게 한다. 클라이언트는 DNS
이름을 받는다([`control_plane.md`](control_plane.md) 3.2 주소). 둘 중 하나가 아니라 둘 다다.
DNS 만 쓰고 뒤의 주소가 바뀌면 클라이언트가 기동 시 한 번만 해석하므로
([`architecture.md`](architecture.md) 3.2.8 `[control]` 스레드) 켜 둔 클라이언트는 옛
주소로 간다.

```bash
aws ec2 describe-addresses \
  --filters "Name=instance-id,Values=<시연 인스턴스 ID>" \
  --query 'length(Addresses)'
```

**판정 기준. 출력이 `1` 이어야 한다.** `0` 이면 붙은 Elastic IP 가 없어 주소가 재시작마다
바뀐다. 2 이상이면 무엇이 붙었는지 사람이 확인한다.

**DNS 쪽도 확인한다. 둘 다라고 했으므로 판정도 둘이다.** 위에서 얻은 Elastic IP 를 `<EIP>`
라 하고, 클라이언트에 줄 이름을 `<이름>` 이라 한다.

```powershell
(Resolve-DnsName -Name '<이름>' -Type A).IPAddress
```

**판정 기준. 출력이 `<EIP>` 하나여야 한다.** 다른 주소가 나오거나 비면 A 레코드가 그
Elastic IP 를 가리키지 않는 것이다. 여러 개가 나오면 클라이언트가 첫 IPv4 하나만 쓰므로
(`control_plane.md` 3.2 주소) 어느 것이 먼저 올지에 기대게 된다. 하나로 줄인다.

**클라이언트에 IPv4 리터럴을 주는 운영이면 이 절반은 건너뛴다.** `--server` 는 이름과
리터럴을 모두 받는다(`control_plane.md` 3.2 주소). 그때도 Elastic IP 는 필요하다. 리터럴이
바뀌면 시연 전에 사람이 다시 알려 줘야 한다.

목록이 필요하면 같은 필터로 본다.

```bash
aws ec2 describe-addresses \
  --filters "Name=instance-id,Values=<시연 인스턴스 ID>" \
  --query 'Addresses[].{ip:PublicIp,assoc:InstanceId,alloc:AllocationId}'
```

**계정 전체를 조회하지 않는다.** 필터 없이 세면 **다른 프로젝트의 Elastic IP 가 통과시켜
준다.** 계정에 EIP 가 여러 개 있는 것은 흔하고, 그중 하나가 붙어 있다는 사실은 우리 인스턴스에
대해 아무것도 말해 주지 않는다.

**대가.** 공인 IPv4 주소는 인스턴스에 붙어 있어도 시간당 요금이 붙는다. 붙어 있지 않을 때만
과금되던 시절의 규칙이 아니다. 시연이 끝나면 인스턴스를 지우고 Elastic IP 도 해제한다.

**요금 체계는 바뀌므로 쓰기 전에 현재 요금 페이지를 확인한다.**

## 11. Minecraft LAN 탐색은 안 된다

**검증: 미검증 (문헌 근거).** 실제로 시도해 보지 않았다. Phase 8 에서 확인한다.

Minecraft Java Edition 의 "LAN 에 공개" 는 멀티캐스트로 광고한다. 널리 인용되는 값은
`224.0.2.60:4445` 이지만 **직접 확인하지 않았고 에디션과 판에 따라 다를 수 있다.**

**결론은 그 값에 의존하지 않는다.** 우리 터널은 유니캐스트 점대점이고 **어떤 멀티캐스트도
옮기지 않는다.** 주소가 무엇이든 상대 서버는 "LAN 게임" 목록에 뜨지 않는다.

**분류: 사람.** 우회는 직접 주소 입력이다.

멀티 플레이 화면에서 `직접 연결` 에 `10.100.0.1:25565` 를 넣는다.

**판정 기준.** 접속되면 정상이다. **목록에 안 뜨는 것은 결함이 아니다.** 이것을 결함으로
보고 터널을 고치려 들면 범위 밖 작업이 된다.

**이 한계를 시연 대본에 적는다.** 심사 중에 "왜 목록에 안 뜨죠" 를 즉석에서 설명하는 것보다
미리 말하는 편이 낫다.

## 12. Java 경로 기반 방화벽 예외

**검증: 실측 (규칙 존재 확인)**

Minecraft 서버용 방화벽 예외는 `javaw.exe` **경로**로 등록된다. Java 를 업데이트하면 경로의
버전 부분이 바뀌고(`jdk-21.0.1` → `jdk-21.0.2`) 예외가 무효가 된다. 규칙은 남아 있는데
동작하지 않으므로 알아채기 어렵다.

**분류: 사람.**

```powershell
Get-NetFirewallApplicationFilter | Where-Object { $_.Program -match 'java' } |
  Select-Object Program, @{n='Rule';e={($_ | Get-NetFirewallRule).DisplayName}}
```

이 기기에는 java 를 가리키는 규칙이 **2건** 있었다.

**판정 기준.** 나온 `Program` 경로가 **실재하는 파일**이어야 한다.

```powershell
Get-NetFirewallApplicationFilter | Where-Object { $_.Program -match 'java' } |
  ForEach-Object {
    $path = [Environment]::ExpandEnvironmentVariables($_.Program)
    '{0,-5} {1}' -f (Test-Path $path), $path
  }
```

```
True  C:\users\...\java-runtime-epsilon\windows-x64\java-runtime-epsilon\bin\javaw.exe
```

첫 칸이 `False` 인 줄이 있으면 그 규칙은 죽은 규칙이다. 새 경로로 다시 등록한다.

**환경 변수를 먼저 푼다.** 규칙의 `Program` 이 `%ProgramFiles%\...` 형태일 수 있고, 그대로
`Test-Path` 에 넣으면 **멀쩡한 규칙이 `False` 로 나온다.** 이 기기에서
`%ProgramFiles%\Common Files` 는 확장 전 `False`, 확장 후 `True` 였다.

**표로 찍지 않는다.** `Format-Table -AutoSize` 로 `Program` 과 `Exists` 를 함께 내면 경로가
길어 **판정 열이 잘려 나간다.** 이 기기에서 실제로 `Exists` 열이 사라졌다. 판정을 보여 주지
않는 판정 명령은 쓸모가 없다.

## 13. SmartScreen 과 Defender

**검증: 부분 실측.** 정책 값만 확인했다. **통과 조건(시연 PC 에서 경고 없이 실행)은
판정되지 않았다.** `client.exe` 가 아직 없다. Phase 8 시연 준비에서 확인한다.

SmartScreen 은 **평판 기반**이다. 서명이 없다고 무조건 막는 것이 아니라, 평판이 없는
실행 파일에 대해 "Windows 가 PC 를 보호했습니다" 를 띄운다. 인터넷에서 받은 파일에는
Mark of the Web 이 붙어 더 엄격해진다.

**우리 빌드 산출물이 여기에 걸릴 위험이 있다.** 관측한 것이 아니라 예상이다. `client.exe`
가 아직 없다. 로컬에서 빌드한 파일에는 보통 MOTW 가 붙지 않으므로, 실제로 경고가 뜨는지는 그
바이너리를 시연 PC 에 올린 방법에 달려 있다. Phase 8 에서 그 파일로 확인한다.

**분류: 사람.**

```powershell
(Get-ItemProperty 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\System' -Name EnableSmartScreen `
  -ErrorAction SilentlyContinue).EnableSmartScreen
```

이 기기에서는 `1` 이 나왔다.

**아무것도 출력되지 않으면 "그룹 정책 재정의가 없다" 는 뜻뿐이다.** 실제 동작 값은 Windows
보안 앱의 설정이나 사용자별 설정으로도 정해지므로 이 조회만으로 단정하지 않는다. **유효한
판정은 시연 PC 에서 그 바이너리를 실제로 실행해 보는 것이다.**

**먼저 무엇을 실행하는지 확인한다.** 아래 두 방법은 **미서명 실행 파일에 대한 Windows 의
보호를 끄는 것이다.** 출처를 확인하지 않고 끄면 SmartScreen 이 막으려던 바로 그 상황이 된다.

```powershell
Get-FileHash .\client.exe -Algorithm SHA256
```

**통과 조건.** 이 값이 **내가 빌드한 산출물의 해시와 같아야 한다.** 빌드 기기에서 같은
명령으로 얻은 값과 대조한다. 다르면 실행하지 않는다. 시연 PC 가 빌드 기기와 같으면 이
단계는 필요 없다.

**확인한 뒤에 우회한다. 방법은 둘이다. 첫째, 받은 파일의 Mark of the Web 을 제거한다.**

```powershell
Unblock-File -LiteralPath .\client.exe
Get-Item -LiteralPath .\client.exe -Stream Zone.Identifier -ErrorAction SilentlyContinue
```

두 번째 명령이 아무것도 내지 않으면 **그 파일에** MOTW 가 없는 것이다. 대체 데이터 스트림은
NTFS 기능이므로 FAT32·exFAT 매체 위의 파일에는 애초에 없다. 그 경우 무출력은 "제거됐다" 가
아니라 "이 파일 시스템에는 없다" 는 뜻이다.

**둘째, USB 나 로컬 빌드로 옮긴다.** 그 경로로 온 파일에는 보통 MOTW 가 붙지 않는다.
**보장은 아니다.** 어느 경로로 옮겼든 위의 `Zone.Identifier` 조회로 확인한다.

- NTFS 로 포맷된 매체는 대체 데이터 스트림을 그대로 나를 수 있다
- 내려받은 압축 파일을 Windows 기본 탐색기로 풀면 MOTW 가 따라붙을 수 있다. 압축 도구와
  Windows 판에 따라 다르다

**판정 기준.** 시연 PC 에서 실행 파일이 경고 없이 뜨면 통과다.

**코드 서명은 하지 않는다.** 인증서 비용과 발급 기간이 학기 일정에 맞지 않는다. 대신
시연 PC 에서 **미리 한 번 실행해 둔다.**

**"한 번만 막힌다" 는 그 파일에 한해서다.** 허용한 뒤에는 같은 시연 PC 의 같은 파일이 다시
묻지 않는다. 아래 넷 중 하나라도 하면 다시 묻는다.

- 바이너리를 다시 빌드한다
- 다른 경로로 옮긴다
- 다른 계정으로 실행한다
- 새로 내려받는다

시연 직전에 바이너리를 바꿨다면 그 파일로 한 번 더 실행해 둔다.

## 14. 자동과 수동

| # | 항목 | 누가 | 언제 |
|---|------|------|------|
| 1 | 관리자 권한 | 사람 | 실행할 때마다 |
| 2 | 방화벽 프로필과 규칙 | 클라이언트 | 어댑터 생성 직후 |
| 3 | 잔존물 정리 | 클라이언트 | 시작할 때 |
| 4 | Wintun DLL 동봉과 서명 | 사람(빌드) | 배포물을 만들 때 |
| 5 | `server.properties` | 사람 | 서버를 처음 세울 때 |
| 6 | EC2 보안 그룹과 바인드 주소 | 사람 | 인스턴스를 세울 때 |
| 7 | 서브넷 충돌 검사 | 클라이언트 | 어댑터를 만들기 전 |
| 8 | on-link 라우트 중복 회피 | 클라이언트 | 주소 할당 직후 |
| 9 | `Preferred` 대기 | 클라이언트 | 주소 할당 직후 |
| 10 | Elastic IP | 사람 | 인스턴스를 세울 때 |
| 11 | LAN 탐색 안 됨 안내 | 사람 | 시연 대본에 |
| 12 | Java 경로 예외 | 사람 | Java 를 업데이트한 뒤 |
| 13 | SmartScreen 우회 | 사람 | 시연 PC 첫 실행 |

**클라이언트가 하는 5건은 전부 Phase 6 이후에 구현된다.** 지금은 설계상의 구분이다.

## 15. 사용성 한계

[`spec.md`](spec.md) 의 주장은 "수동 포트 포워딩 없이" 이고 **그 주장은 유지된다.** 위
표의 어느 항목도 공유기 설정을 요구하지 않는다.

**다만 설정 부담이 사라진 것이 아니라 종류가 바뀌었다.** 대신 관리자 권한, 드라이버 적재,
방화벽 규칙, 서버 설정 파일이 필요하다. 포트 포워딩보다 쉬운지는 **별도 논증이 필요하고
이 문서는 그 논증을 하지 않는다.** 최종 보고서에서 정직하게 다룬다.

비교 대상을 분명히 해 둔다.

| | 포트 포워딩 방식 | 이 프로젝트 |
|---|---|---|
| 공유기 설정 | 필요. 기종마다 화면이 다르고 CGNAT 이면 불가능 | 없음 |
| 관리자 권한 | 불필요 | **필요** |
| 드라이버 | 불필요 | **필요**(Wintun) |
| 방화벽 | 서버 포트 예외 1건 | 서버 포트 예외 + 어댑터 프로필 |
| 양쪽 부담 | 서버 쪽만 | **양쪽 다** |

**한쪽이 일방적으로 쉽지 않다.** 공유기를 못 건드리는 환경(CGNAT, 기숙사, 부모 소유
공유기)에서만 이 방식이 유일한 선택지가 된다. 그 조건을 보고서의 전제로 적는다.
