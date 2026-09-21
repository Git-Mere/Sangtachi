<#
.SYNOPSIS
  서브넷 충돌을 판정한다. `windows-prereq.md` 7절의 판정식을 스크립트로 뺀 것이다.

.DESCRIPTION
  우리가 만들 대역(기본 `10.100.0.0/24`)이 이 기기의 기존 라우트·주소와 겹치는지 본다.
  Windows 는 최장 접두사 일치로 라우트를 고른다. 겹치는 것을 접두사 길이로 나눈다.

  | 겹치는 기존 항목        | 판정           |
  |-------------------------|----------------|
  | 길이 >= 대상 길이       | `BLOCK-route`  |
  | 길이 2 ~ 대상 길이-1    | `BLOCK-shadow` |
  | 길이 0~1 (대상은 `/2` 이상) | `catch-all` |
  | 겹치지 않음             | `ignore`       |

  `catch-all` 은 충돌로 세지 않는다. `0.0.0.0/0` 은 보통의 기기에 있고 `0.0.0.0/1` 은
  풀터널 VPN 에서 나온다. 이것을 충돌로 읽으면 정상 기기가 떨어진다.

  **이 파일은 케이스 표를 구현보다 먼저 쓴다** (design-audit 6장 규칙 7). 표는 아래
  `$script:Cases` 이고 함수 정의보다 앞에 있다. 케이스는 "정상 입력이 통과하는가"(규칙 3)와
  "결함을 잡는가"를 쌍으로 넣는다. 대부분은 2026-09-21 크로스 모델 리뷰 31라운드에서
  이미 확보된 반례다. 케이스마다 그 라운드 번호를 적었다.

  모드는 둘이다.

  - `-SelfTest`: 케이스 표 전체를 돌린다. 마지막 줄은 `SELFTEST: pass` 또는 `SELFTEST: fail`.
    종료 코드 0 또는 1. 실패한 케이스는 기대값과 실제값을 같이 찍는다.
  - 기본: 이 기기의 라우트와 주소로 판정한다. 마지막 줄은 `VERDICT: <값>`.
    `proceed` 0, `pick another range` 1, `unknown` 2.

  **판정할 수 없으면 통과로 적지 않는다.** 조회가 실패했거나 IPv4 모양인데 해석할 수 없는
  항목이 있으면 `unknown` 이다. 다만 확실한 차단이 이미 하나라도 있으면 그것이 답이므로
  `pick another range` 가 이긴다.

  **순서가 있다.** `windows-prereq.md` 3절의 잔존물 정리를 먼저 돌리고 이 검사를 한다.
  우리 어댑터가 이전 실행의 주소를 달고 살아 있으면 자기 자신 때문에 차단이 나온다.
  우리 어댑터는 `-OwnInterfaceGuid` 로만 뺀다. 소유 계약은 `Resolve-OwnInterface` 주석에 있다.

.PARAMETER Target
  우리가 쓰려는 대역. `10.100.0.0/24` 형식. 네트워크 주소여야 하고 **길이는 2~32 다.**
  `/0` 과 `/1` 을 거부하는 이유는 `Get-SubnetVerdict` 의 대상 검증 주석에 적었다.

.PARAMETER OwnInterfaceGuid
  우리 Wintun 어댑터의 `InterfaceGuid`. **소유를 증명하는 유일한 값이다.** 클라이언트가
  어댑터를 만들 때 쓴 GUID 를 그대로 넘긴다. 계약은 `Resolve-OwnInterface` 주석에 적었다.

.PARAMETER OwnAlias
  우리 어댑터 이름. **이것만으로는 아무것도 빼지 않는다.** 이름은 남이 쓸 수 있다.
  `-OwnInterfaceGuid` 와 같이 주면 GUID 로 찾은 어댑터의 이름이 이 값과 같은지 한 번 더 본다.
  다르면 상태가 낡은 것이므로 아무것도 빼지 않는다.

.PARAMETER AllowShadow
  `BLOCK-shadow` 를 차단으로 세지 않는다. 그 대역이 비어 있다는 것을 사람이 아는 경우에만
  쓴다. 기본값은 차단이다.

.PARAMETER SelfTest
  케이스 표를 돌린다. 네트워크 조회를 하지 않는다.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File .\Test-SubnetOverlap.ps1 -SelfTest

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File .\Test-SubnetOverlap.ps1 -Target 10.100.0.0/24
#>
[CmdletBinding()]
param(
    [string]$Target = '10.100.0.0/24',
    [string]$OwnInterfaceGuid,
    [string]$OwnAlias,
    [switch]$AllowShadow,
    [switch]$SelfTest
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# ---------------------------------------------------------------------------
# 케이스 표. 구현보다 먼저 쓴다 (design-audit 6장 규칙 7).
#
# Kind 는 둘이다. `정상` 은 정상 입력이 통과하는지 본다 (규칙 3). `결함` 은 결함을 잡는지
# 본다. Round 는 2026-09-21 커밋 기록의 리뷰 라운드 번호다. `-` 는 그 표에 없고 이번에
# 추가한 것이다.
#
# Check 는 문자열 하나를 낸다. Expect 와 글자 그대로 비교한다. 거부를 보는 케이스는
# `Get-RejectReason` 이 내는 이유 토큰을 비교한다. **거부되는 것만 보지 않고 어느 검사가
# 걸렀는지까지 본다** (27라운드).
# ---------------------------------------------------------------------------
$script:Cases = @(

    # --- 접두사 한 건의 판정 ------------------------------------------------
    @{ Name = 'route-same-length-24-blocks'; Round = '12'; Kind = '결함'
       Expect = 'BLOCK-route'; Check = { Get-CaseVerdict '10.100.0.0/24' } }
    @{ Name = 'route-host-32-blocks'; Round = '4'; Kind = '결함'
       Expect = 'BLOCK-route'; Check = { Get-CaseVerdict '10.100.0.5/32' } }
    @{ Name = 'route-longer-25-blocks'; Round = '4'; Kind = '결함'
       Expect = 'BLOCK-route'; Check = { Get-CaseVerdict '10.100.0.128/25' } }
    @{ Name = 'shadow-23-blocks'; Round = '7'; Kind = '결함'
       Expect = 'BLOCK-shadow'; Check = { Get-CaseVerdict '10.100.0.0/23' } }
    @{ Name = 'shadow-8-string-would-miss'; Round = '4,22'; Kind = '결함'
       Expect = 'BLOCK-shadow'; Check = { Get-CaseVerdict '10.0.0.0/8' } }
    @{ Name = 'shadow-2-blocks'; Round = '7'; Kind = '결함'
       Expect = 'BLOCK-shadow'; Check = { Get-CaseVerdict '8.0.0.0/2' } }
    @{ Name = 'catchall-half-default-passes'; Round = '3'; Kind = '정상'
       Expect = 'catch-all'; Check = { Get-CaseVerdict '0.0.0.0/1' } }
    @{ Name = 'catchall-default-passes'; Round = '3'; Kind = '정상'
       Expect = 'catch-all'; Check = { Get-CaseVerdict '0.0.0.0/0' } }
    @{ Name = 'ignore-other-10-24'; Round = '-'; Kind = '정상'
       Expect = 'ignore'; Check = { Get-CaseVerdict '10.0.0.0/24' } }
    @{ Name = 'ignore-neighbour-10-101'; Round = '-'; Kind = '정상'
       Expect = 'ignore'; Check = { Get-CaseVerdict '10.101.0.0/24' } }
    @{ Name = 'ignore-192-168-16'; Round = '-'; Kind = '정상'
       Expect = 'ignore'; Check = { Get-CaseVerdict '192.168.0.0/16' } }
    @{ Name = 'ignore-host-outside-range'; Round = '21기각'; Kind = '정상'
       Expect = 'ignore'; Check = { Get-CaseVerdict '10.0.0.49/32' } }
    @{ Name = 'shorter-mask-catches-host-route'; Round = '-'; Kind = '결함'
       # 짧은 쪽 마스크로 비교한다. 자기 마스크로만 보면 우리 /24 안의 /32 를 놓친다.
       Expect = 'True'; Check = { (Test-PrefixOverlap '10.100.0.200/32' $script:T.Net $script:T.Len).Overlap.ToString() } }

    # --- 잘못된 입력의 거부. 어느 검사가 걸렀는지까지 본다 -------------------
    @{ Name = 'reject-no-slash'; Round = '26'; Kind = '결함'
       Expect = 'format'; Check = { Get-RejectReason { Get-CaseVerdict '10.100.0.0' } } }
    @{ Name = 'reject-two-slashes'; Round = '26'; Kind = '결함'
       Expect = 'format'; Check = { Get-RejectReason { Get-CaseVerdict '10.100.0.0/24/8' } } }
    @{ Name = 'reject-empty-address'; Round = '26'; Kind = '결함'
       Expect = 'format'; Check = { Get-RejectReason { Get-CaseVerdict '/24' } } }
    @{ Name = 'reject-empty-length'; Round = '28'; Kind = '결함'
       # [int]'' 는 0 이다. 비어 있으면 /0 으로 읽혀 catch-all 로 통과한다.
       Expect = 'digits'; Check = { Get-RejectReason { Get-CaseVerdict '10.100.0.0/' } } }
    @{ Name = 'reject-space-in-length'; Round = '26'; Kind = '결함'
       Expect = 'digits'; Check = { Get-RejectReason { Get-CaseVerdict '10.100.0.0/ 24' } } }
    @{ Name = 'reject-negative-length'; Round = '26'; Kind = '결함'
       Expect = 'digits'; Check = { Get-RejectReason { Get-CaseVerdict '10.100.0.0/-1' } } }
    @{ Name = 'reject-huge-length'; Round = '31'; Kind = '결함'
       # 자릿수를 막지 않으면 [int] 캐스팅에서 예외가 난다. 검증 함수가 먼저 거른다.
       Expect = 'digits'; Check = { Get-RejectReason { Get-CaseVerdict '10.100.0.0/999999999999' } } }
    @{ Name = 'reject-length-33'; Round = '26'; Kind = '결함'
       Expect = 'range'; Check = { Get-RejectReason { Get-CaseVerdict '10.100.0.0/33' } } }
    @{ Name = 'reject-ipv6-with-128'; Round = '27'; Kind = '결함'
       # 길이가 세 자리라 자릿수 검사에서 먼저 걸린다. IPv6 가드까지 가지 않는다.
       Expect = 'digits'; Check = { Get-RejectReason { Get-CaseVerdict '2001:db8::1/128' } } }
    @{ Name = 'reject-ipv6-without-slash'; Round = '27'; Kind = '결함'
       Expect = 'format'; Check = { Get-RejectReason { Get-CaseVerdict '2001:db8::1' } } }
    @{ Name = 'reject-ipv6-burns-family-guard'; Round = '23,27'; Kind = '결함'
       # IPv6 가드를 실제로 태우는 입력은 이것뿐이다. 앞 4바이트를 IPv4 로 읽으면 안 된다.
       Expect = 'family'; Check = { Get-RejectReason { Get-CaseVerdict '2001:db8::1/24' } } }
    @{ Name = 'reject-octet-over-255'; Round = '-'; Kind = '결함'
       Expect = 'parse'; Check = { Get-RejectReason { Get-CaseVerdict '10.100.0.256/24' } } }
    @{ Name = 'reject-shorthand-address'; Round = '-'; Kind = '결함'
       # '10.100.0' 을 .NET 판에 따라 10.100.0.0 으로 읽는다. 판을 믿지 않고 거부한다.
       Expect = 'shorthand'; Check = { Get-RejectReason { Get-CaseVerdict '10.100.0/24' } } }
    @{ Name = 'reject-leading-zero-octet'; Round = '-'; Kind = '결함'
       # '010.1.1.1' 을 8진수로 읽는 판이 있다. 판마다 값이 달라지므로 거부한다.
       Expect = 'leadingzero'; Check = { Get-RejectReason { Get-CaseVerdict '010.1.1.1/24' } } }

    # --- 대상 대역 자체의 검증 ---------------------------------------------
    @{ Name = 'target-host-bits-rejected'; Round = '-'; Kind = '결함'
       Expect = 'hostbits'; Check = { Get-RejectReason { Get-SubnetVerdict -Target '10.100.0.5/24' } } }
    @{ Name = 'target-length-zero-rejected'; Round = '-'; Kind = '결함'
       Expect = 'targetlen'; Check = { Get-RejectReason { Get-SubnetVerdict -Target '0.0.0.0/0' } } }
    @{ Name = 'target-length-one-rejected'; Round = '7라운드 warn'; Kind = '결함'
       # **대상 `/1` 을 받으면 같은 `/1` 라우트를 catch-all 로 읽고 놓친다.** 그래서 거부한다.
       Expect = 'targetlen'; Check = { Get-RejectReason { Get-SubnetVerdict -Target '0.0.0.0/1' } } }
    @{ Name = 'target-length-one-upper-half-rejected'; Round = '7라운드 warn'; Kind = '결함'
       Expect = 'targetlen'; Check = { Get-RejectReason { Get-SubnetVerdict -Target '128.0.0.0/1' } } }
    @{ Name = 'target-length-two-accepted'; Round = '7라운드 warn'; Kind = '정상'
       # 경계 바로 위다. `/2` 는 받는다 (규칙 3).
       Expect = 'proceed'; Check = {
           (Get-SubnetVerdict -Target '64.0.0.0/2' -Routes @(New-Route '0.0.0.0/0')).Verdict } }
    @{ Name = 'target-length-two-same-length-blocks'; Round = '7라운드 warn'; Kind = '결함'
       Expect = 'pick another range'; Check = {
           (Get-SubnetVerdict -Target '64.0.0.0/2' -Routes @(New-Route '64.0.0.0/2' 'VPN')).Verdict } }
    @{ Name = 'same-length-beats-catchall-exception'; Round = '7라운드 warn'; Kind = '결함'
       # 판정 **순서**를 고정한다. 대상 검증 덕에 지금은 도달할 수 없는 조합이지만, 검증이
       # 느슨해지면 여기가 유일한 방어선이다. 함수를 직접 불러 순서만 본다.
       Expect = 'BLOCK-route'; Check = { (Test-PrefixOverlap '0.0.0.0/1' 0 1).Verdict } }
    @{ Name = 'zero-length-route-vs-zero-target-blocks'; Round = '7라운드 warn'; Kind = '결함'
       Expect = 'BLOCK-route'; Check = { (Test-PrefixOverlap '0.0.0.0/0' 0 0).Verdict } }
    @{ Name = 'catchall-exempt-survives-for-short-target'; Round = '7라운드 warn'; Kind = '정상'
       # 순서를 바꿔도 **정상 경로는 그대로다.** 대상이 `/8` 이어도 기존 `/1` 은 충돌이 아니다.
       Expect = 'catch-all'; Check = {
           (Test-PrefixOverlap '0.0.0.0/1' (ConvertTo-UInt32Ip '10.0.0.0') 8).Verdict } }
    @{ Name = 'target-alt-range-23-accepted'; Round = '8'; Kind = '정상'
       Expect = 'proceed'; Check = { (Get-SubnetVerdict -Target '10.100.0.0/23' -Routes @(New-Route '0.0.0.0/0')).Verdict } }

    # --- 기기 전체 판정 -----------------------------------------------------
    @{ Name = 'clean-machine-proceeds'; Round = '3'; Kind = '정상'
       Expect = 'proceed'; Check = {
           (Get-SubnetVerdict -Target '10.100.0.0/24' `
                -Routes @(New-Route '0.0.0.0/0' 'Wi-Fi' '192.168.1.1'; New-Route '192.168.1.0/24') `
                -Addresses @(New-Addr '192.168.1.10')).Verdict } }
    @{ Name = 'fulltunnel-vpn-proceeds'; Round = '3'; Kind = '정상'
       # 풀터널 VPN 기기다. /1 두 줄을 충돌로 읽으면 정상 기기가 떨어진다.
       Expect = 'proceed'; Check = {
           (Get-SubnetVerdict -Target '10.100.0.0/24' `
                -Routes @(New-Route '0.0.0.0/1' 'VPN'; New-Route '128.0.0.0/1' 'VPN'; New-Route '0.0.0.0/0')).Verdict } }
    @{ Name = 'gateway-route-still-blocks'; Round = '1'; Kind = '결함'
       # NextHop 이 게이트웨이인 VPN 라우트다. NextHop 으로 거르면 이것을 놓친다.
       Expect = 'pick another range'; Check = {
           (Get-SubnetVerdict -Target '10.100.0.0/24' `
                -Routes @(New-Route '0.0.0.0/0'; New-Route '10.0.0.0/8' 'VPN' '192.168.1.1')).Verdict } }
    @{ Name = 'non-10-prefix-is-scanned'; Round = '4'; Kind = '결함'
       # 대역이 10.* 가 아니어도 전체 라우트를 훑는다.
       Expect = 'pick another range'; Check = {
           (Get-SubnetVerdict -Target '172.20.5.0/24' `
                -Routes @(New-Route '0.0.0.0/0'; New-Route '172.16.0.0/12' 'Hyper-V')).Verdict } }
    @{ Name = 'same-24-blocks-machine'; Round = '12'; Kind = '결함'
       Expect = 'pick another range'; Check = {
           (Get-SubnetVerdict -Target '10.100.0.0/24' -Routes @(New-Route '10.100.0.0/24' 'Hyper-V')).Verdict } }
    @{ Name = 'ipv6-rows-do-not-throw'; Round = '29'; Kind = '정상'
       # -AddressFamily IPv4 를 줘도 ::/0 이 섞일 수 있다. 판정 대신 예외가 나면 안 된다.
       Expect = 'proceed'; Check = {
           (Get-SubnetVerdict -Target '10.100.0.0/24' `
                -Routes @(New-Route '::/0' 'Wi-Fi'; New-Route 'fe80::/64' 'Wi-Fi'; New-Route '0.0.0.0/0') `
                -Addresses @(New-Addr 'fe80::1'; New-Addr '192.168.1.10')).Verdict } }
    @{ Name = 'malformed-ipv4-row-is-unknown'; Round = '26'; Kind = '결함'
       # IPv4 모양인데 해석할 수 없다. 통과로 적지 않는다.
       Expect = 'unknown'; Check = {
           (Get-SubnetVerdict -Target '10.100.0.0/24' `
                -Routes @(New-Route '0.0.0.0/0'; New-Route '10.100.0.256/24')).Verdict } }
    @{ Name = 'unavailable-source-is-unknown'; Round = '9'; Kind = '결함'
       # 조회 실패를 "없음" 으로 읽지 않는다.
       Expect = 'unknown'; Check = {
           (Get-SubnetVerdict -Target '10.100.0.0/24' -Unavailable @('routes')).Verdict } }
    @{ Name = 'definite-block-beats-unknown'; Round = '-'; Kind = '결함'
       # 확실한 차단이 있으면 그것이 답이다.
       Expect = 'pick another range'; Check = {
           (Get-SubnetVerdict -Target '10.100.0.0/24' -Unavailable @('addresses') `
                -Routes @(New-Route '10.100.0.5/32')).Verdict } }

    # --- 우리 어댑터 제외. 소유 계약은 `Resolve-OwnInterface` 주석에 있다 -------
    # 결과 문자열은 `<소유 판정 이유>/<대역 판정>/r<BLOCK-route>a<BLOCK-address>` 다.
    # 소유 판정과 대역 판정을 한 케이스에서 같이 고정한다. 하나만 보면 반대쪽이 깨진다.
    @{ Name = 'own-guid-excludes-own-rows'; Round = '14'; Kind = '정상'
       # 우리 어댑터 하나뿐인 정상 기기다. 잔존 주소·라우트 때문에 자기 자신이 차단되면 안 된다.
       Expect = 'ok/proceed/r0a0'; Check = {
           Invoke-OwnCheck -Guid '{11111111-1111-4111-8111-111111111111}' -Alias 'Sangtachi' `
               -Adapters @(New-Adapter 'Sangtachi' 'Wintun Userspace Tunnel' '{11111111-1111-4111-8111-111111111111}' 12) `
               -Routes @(New-Route '0.0.0.0/0'; New-Route '10.100.0.0/24' 'Sangtachi' '0.0.0.0' 12) `
               -Addresses @(New-Addr '10.100.0.1' 'Sangtachi' 12) } }
    @{ Name = 'own-guid-keeps-other-adapter-rows'; Round = '14'; Kind = '결함'
       Expect = 'ok/pick another range/r1a0'; Check = {
           Invoke-OwnCheck -Guid '{11111111-1111-4111-8111-111111111111}' -Alias 'Sangtachi' `
               -Adapters @(New-Adapter 'Sangtachi' 'Wintun Userspace Tunnel' '{11111111-1111-4111-8111-111111111111}' 12) `
               -Routes @(New-Route '10.100.0.0/24' 'Hyper-V' '0.0.0.0' 5) } }
    @{ Name = 'foreign-wintun-with-our-alias-not-excluded'; Round = '3라운드 blocker'; Kind = '결함'
       # 요청한 이름을 쓰는 **남의** Wintun 어댑터다. 이름과 설명만 보면 우리 것으로 착각한다.
       # GUID 가 다르므로 소유가 확정되지 않고, 그 어댑터의 겹침이 그대로 남아야 한다.
       Expect = 'not-found/pick another range/r1a1'; Check = {
           Invoke-OwnCheck -Guid '{11111111-1111-4111-8111-111111111111}' -Alias 'Sangtachi' `
               -Adapters @(New-Adapter 'Sangtachi' 'Wintun Userspace Tunnel' '{22222222-2222-4222-8222-222222222222}' 7) `
               -Routes @(New-Route '10.100.0.0/24' 'Sangtachi' '0.0.0.0' 7) `
               -Addresses @(New-Addr '10.100.0.9' 'Sangtachi' 7) } }
    @{ Name = 'foreign-wintun-shares-our-alias'; Round = '3라운드 blocker'; Kind = '결함'
       # 우리 것과 남의 것이 같은 이름을 쓴다. 우리 것만 빠지고 남의 겹침은 남아야 한다.
       Expect = 'ok/pick another range/r1a0'; Check = {
           Invoke-OwnCheck -Guid '{11111111-1111-4111-8111-111111111111}' -Alias 'Sangtachi' `
               -Adapters @(New-Adapter 'Sangtachi' 'Wintun Userspace Tunnel' '{11111111-1111-4111-8111-111111111111}' 12
                           New-Adapter 'Sangtachi' 'Wintun Userspace Tunnel' '{22222222-2222-4222-8222-222222222222}' 7) `
               -Routes @(New-Route '10.100.0.0/24' 'Sangtachi' '0.0.0.0' 12
                         New-Route '10.100.0.0/24' 'Sangtachi' '0.0.0.0' 7) } }
    @{ Name = 'alias-only-excludes-nothing'; Round = '21,3라운드 blocker'; Kind = '결함'
       # 이름만으로는 아무것도 빼지 않는다. 거짓 통과보다 거짓 차단이 낫다.
       Expect = 'alias-only/pick another range/r1a0'; Check = {
           Invoke-OwnCheck -Guid '' -Alias 'Sangtachi' `
               -Adapters @(New-Adapter 'Sangtachi' 'Wintun Userspace Tunnel' '{11111111-1111-4111-8111-111111111111}' 12) `
               -Routes @(New-Route '10.100.0.0/24' 'Sangtachi' '0.0.0.0' 12) } }
    @{ Name = 'bad-guid-excludes-nothing'; Round = '-'; Kind = '결함'
       Expect = 'bad-guid/pick another range/r1a0'; Check = {
           Invoke-OwnCheck -Guid 'not-a-guid' -Alias 'Sangtachi' `
               -Adapters @(New-Adapter 'Sangtachi' 'Wintun Userspace Tunnel' '{11111111-1111-4111-8111-111111111111}' 12) `
               -Routes @(New-Route '10.100.0.0/24' 'Sangtachi' '0.0.0.0' 12) } }
    @{ Name = 'stale-guid-excludes-nothing'; Round = '-'; Kind = '결함'
       # 어댑터를 지웠는데 기록이 남은 경우다. 판정이 엄격해지는 쪽으로만 틀린다.
       Expect = 'not-found/pick another range/r1a0'; Check = {
           Invoke-OwnCheck -Guid '{11111111-1111-4111-8111-111111111111}' -Alias '' `
               -Adapters @(New-Adapter 'Wi-Fi' 'Intel(R) Wi-Fi 6' '{22222222-2222-4222-8222-222222222222}' 5) `
               -Routes @(New-Route '10.100.0.0/24' 'Sangtachi' '0.0.0.0' 12) } }
    @{ Name = 'duplicate-guid-excludes-nothing'; Round = '21'; Kind = '결함'
       Expect = 'ambiguous/pick another range/r1a0'; Check = {
           Invoke-OwnCheck -Guid '{11111111-1111-4111-8111-111111111111}' -Alias 'Sangtachi' `
               -Adapters @(New-Adapter 'Sangtachi' 'Wintun Userspace Tunnel' '{11111111-1111-4111-8111-111111111111}' 12
                           New-Adapter 'Sangtachi2' 'Wintun Userspace Tunnel' '{11111111-1111-4111-8111-111111111111}' 13) `
               -Routes @(New-Route '10.100.0.0/24' 'Sangtachi' '0.0.0.0' 12) } }
    @{ Name = 'guid-on-non-wintun-excludes-nothing'; Round = '-'; Kind = '결함'
       # GUID 는 맞는데 드라이버가 Wintun 이 아니다. 기록이 낡았거나 GUID 가 재사용됐다.
       Expect = 'not-wintun/pick another range/r1a0'; Check = {
           Invoke-OwnCheck -Guid '{11111111-1111-4111-8111-111111111111}' -Alias 'Sangtachi' `
               -Adapters @(New-Adapter 'Sangtachi' 'Realtek PCIe GbE Family Controller' '{11111111-1111-4111-8111-111111111111}' 12) `
               -Routes @(New-Route '10.100.0.0/24' 'Sangtachi' '0.0.0.0' 12) } }
    @{ Name = 'alias-mismatch-excludes-nothing'; Round = '-'; Kind = '결함'
       Expect = 'alias-mismatch/pick another range/r1a0'; Check = {
           Invoke-OwnCheck -Guid '{11111111-1111-4111-8111-111111111111}' -Alias 'Sangtachi' `
               -Adapters @(New-Adapter '다른이름' 'Wintun Userspace Tunnel' '{11111111-1111-4111-8111-111111111111}' 12) `
               -Routes @(New-Route '10.100.0.0/24' '다른이름' '0.0.0.0' 12) } }
    @{ Name = 'no-index-excludes-nothing'; Round = '-'; Kind = '결함'
       Expect = 'no-index/pick another range/r1a0'; Check = {
           Invoke-OwnCheck -Guid '{11111111-1111-4111-8111-111111111111}' -Alias 'Sangtachi' `
               -Adapters @(New-Adapter 'Sangtachi' 'Wintun Userspace Tunnel' '{11111111-1111-4111-8111-111111111111}' '') `
               -Routes @(New-Route '10.100.0.0/24' 'Sangtachi' '0.0.0.0' 12) } }
    @{ Name = 'adapter-query-failure-is-unknown'; Round = '9,4라운드 blocker'; Kind = '결함'
       # **조회 실패를 빈 목록으로 읽으면 안 된다.** 소유를 확정하지 못한 기기가 proceed 로
       # 끝나면 blocker 17 을 닫지 못한다. 자료원을 사용 불가로 올려 unknown 을 낸다.
       Expect = 'query-failed/unknown/r0a0'; Check = {
           Invoke-OwnCheck -QueryFailed -Guid '{11111111-1111-4111-8111-111111111111}' -Alias 'Sangtachi' -Adapters @() `
               -Routes @(New-Route '0.0.0.0/0') } }
    @{ Name = 'adapter-query-failure-block-still-wins'; Round = '4라운드 blocker'; Kind = '결함'
       # 확실한 차단이 있으면 그것이 답이다. 그때만 차단이 unknown 을 이긴다.
       Expect = 'query-failed/pick another range/r1a0'; Check = {
           Invoke-OwnCheck -QueryFailed -Guid '{11111111-1111-4111-8111-111111111111}' -Alias 'Sangtachi' -Adapters @() `
               -Routes @(New-Route '10.100.0.0/24' 'Sangtachi' '0.0.0.0' 12) } }
    @{ Name = 'empty-adapter-list-is-not-unknown'; Round = '4라운드 warn'; Kind = '정상'
       # **조회에 성공했는데 없는 것**은 사실이다. 실패와 구분한다. 여기서 unknown 을 내면
       # 정상 기기가 판정 불가로 떨어진다 (규칙 3).
       Expect = 'not-found/proceed/r0a0'; Check = {
           Invoke-OwnCheck -Guid '{11111111-1111-4111-8111-111111111111}' -Alias 'Sangtachi' -Adapters @() `
               -Routes @(New-Route '0.0.0.0/0') } }
    @{ Name = 'queried-machine-stays-proceed'; Round = '4라운드 warn'; Kind = '정상'
       # 조회가 되는 정상 기기다. 어댑터가 여러 개여도 unknown 이 아니다 (규칙 3).
       Expect = 'ok/proceed/r0a0'; Check = {
           Invoke-OwnCheck -Guid '{11111111-1111-4111-8111-111111111111}' -Alias 'Sangtachi' `
               -Adapters @(New-Adapter 'Wi-Fi' 'Intel(R) Wi-Fi 6' '{33333333-3333-4333-8333-333333333333}' 5
                           New-Adapter 'Sangtachi' 'Wintun Userspace Tunnel' '{11111111-1111-4111-8111-111111111111}' 12) `
               -Routes @(New-Route '0.0.0.0/0'; New-Route '10.100.0.0/24' 'Sangtachi' '0.0.0.0' 12) `
               -Addresses @(New-Addr '10.100.0.1' 'Sangtachi' 12) } }
    @{ Name = 'bad-guid-with-failed-query-is-unknown'; Round = '4라운드 blocker'; Kind = '결함'
       # GUID 가 잘못돼도 조회 실패는 그대로 자료원 문제다. 먼저 걸린 이유가 그것을 가리면 안 된다.
       Expect = 'bad-guid/unknown/r0a0'; Check = {
           Invoke-OwnCheck -QueryFailed -Guid 'not-a-guid' -Alias 'Sangtachi' -Adapters @() `
               -Routes @(New-Route '0.0.0.0/0') } }
    @{ Name = 'no-guid-never-queries-adapters'; Round = '4라운드 warn'; Kind = '정상'
       # GUID 를 안 주면 라이브가 어댑터를 조회하지 않는다. 그때는 실패도 없다.
       Expect = 'no-guid/proceed/r0a0'; Check = {
           Invoke-OwnCheck -Guid '' -Alias '' -Adapters @() -Routes @(New-Route '0.0.0.0/0') } }
    @{ Name = 'guid-format-variants-resolve'; Round = '-'; Kind = '정상'
       # 중괄호 없는 소문자 표기도 같은 GUID 다. 표기 차이로 정상 기기를 떨어뜨리지 않는다.
       Expect = 'ok/proceed/r0a0'; Check = {
           Invoke-OwnCheck -Guid '11111111-1111-4111-8111-111111111111' -Alias 'Sangtachi' `
               -Adapters @(New-Adapter 'Sangtachi' 'Wintun Userspace Tunnel' '{11111111-1111-4111-8111-111111111111}' 12) `
               -Routes @(New-Route '10.100.0.0/24' 'Sangtachi' '0.0.0.0' 12) } }
    @{ Name = 'no-guid-no-alias-is-clean'; Round = '-'; Kind = '정상'
       Expect = 'no-guid/proceed/r0a0'; Check = {
           Invoke-OwnCheck -Guid '' -Alias '' -Adapters @() -Routes @(New-Route '0.0.0.0/0') } }
    @{ Name = 'null-index-keeps-indexless-row'; Round = '23'; Kind = '결함'
       # OwnIndex 가 없을 때 인덱스 없는 행을 걸러내면 "아무것도 빼지 않는다" 가 거짓이 된다.
       Expect = 'pick another range'; Check = {
           (Get-SubnetVerdict -Target '10.100.0.0/24' `
                -Routes @(New-Route '10.100.0.0/24' '' '0.0.0.0' '')).Verdict } }
    @{ Name = 'exclusion-keys-on-index-not-name'; Round = '21'; Kind = '결함'
       # 인덱스가 다르면 이름이 같아도 남는다.
       Expect = 'pick another range'; Check = {
           (Get-SubnetVerdict -Target '10.100.0.0/24' -OwnIndex 12 `
                -Routes @(New-Route '10.100.0.0/24' 'Sangtachi' '0.0.0.0' 7)).Verdict } }

    # --- 자료원 조회: 실패와 "없음" 을 구분한다 (4라운드) ---------------------
    @{ Name = 'query-source-failure-is-flagged'; Round = '4라운드 blocker'; Kind = '결함'
       Expect = 'failed=True items=0'; Check = {
           $s = Get-QuerySource 'x' { throw '접근이 거부되었습니다' }
           'failed={0} items={1}' -f $s.Failed, @($s.Items).Count } }
    @{ Name = 'query-source-empty-is-not-failure'; Round = '4라운드 warn'; Kind = '정상'
       # 조회에 성공한 빈 목록은 실패가 아니다. 이것을 실패로 읽으면 정상 기기가 떨어진다.
       Expect = 'failed=False items=0'; Check = {
           $s = Get-QuerySource 'x' { @() }
           'failed={0} items={1}' -f $s.Failed, @($s.Items).Count } }
    @{ Name = 'query-source-passes-items'; Round = '-'; Kind = '정상'
       Expect = 'failed=False items=3'; Check = {
           $s = Get-QuerySource 'x' { 1; 2; 3 }
           'failed={0} items={1}' -f $s.Failed, @($s.Items).Count } }
    @{ Name = 'query-source-not-needed-is-not-failure'; Round = '4라운드 warn'; Kind = '정상'
       # 부르지 않은 조회는 실패가 아니다. GUID 를 안 준 기기가 unknown 이 되면 안 된다.
       Expect = 'failed=False items=0'; Check = {
           $s = Get-QuerySource 'x' { throw '불러선 안 된다' } $false
           'failed={0} items={1}' -f $s.Failed, @($s.Items).Count } }

    # --- 라이브 경로 전체 (cmdlet 만 바꿔 같은 순서로 엮는다) ------------------
    @{ Name = 'live-path-all-queries-ok-proceeds'; Round = '4라운드 warn'; Kind = '정상'
       # 조회가 되는 정상 기기다 (규칙 3).
       Expect = 'ok/proceed'; Check = {
           Invoke-LivePath -RouteQuery { New-Route '0.0.0.0/0' } -AddrQuery { New-Addr '192.168.1.10' } `
               -AdapterQuery { New-Adapter 'Sangtachi' 'Wintun Userspace Tunnel' '{11111111-1111-4111-8111-111111111111}' 12 } -Guid '{11111111-1111-4111-8111-111111111111}' -Alias 'Sangtachi' } }
    @{ Name = 'live-path-adapter-query-failure-is-unknown'; Round = '4라운드 blocker'; Kind = '결함'
       # **라이브가 어댑터 조회 실패를 삼키면 안 된다.** 이 케이스가 그 갈래를 실제로 밟는다.
       Expect = 'query-failed/unknown'; Check = {
           Invoke-LivePath -RouteQuery { New-Route '0.0.0.0/0' } -AddrQuery { @() } `
               -AdapterQuery { throw '접근이 거부되었습니다' } -Guid '{11111111-1111-4111-8111-111111111111}' -Alias 'Sangtachi' } }
    @{ Name = 'live-path-route-query-failure-is-unknown'; Round = '9'; Kind = '결함'
       Expect = 'ok/unknown'; Check = {
           Invoke-LivePath -RouteQuery { throw '서비스 이상' } -AddrQuery { @() } `
               -AdapterQuery { New-Adapter 'Sangtachi' 'Wintun Userspace Tunnel' '{11111111-1111-4111-8111-111111111111}' 12 } -Guid '{11111111-1111-4111-8111-111111111111}' -Alias 'Sangtachi' } }
    @{ Name = 'live-path-no-guid-skips-adapter-query'; Round = '4라운드 warn'; Kind = '정상'
       # GUID 를 안 주면 어댑터를 조회하지 않는다. 그 조회가 실패해도 판정은 영향을 받지 않는다.
       Expect = 'no-guid/proceed'; Check = {
           Invoke-LivePath -RouteQuery { New-Route '0.0.0.0/0' } -AddrQuery { @() } `
               -AdapterQuery { throw '불러선 안 된다' } } }

    # --- 주소 검사 ----------------------------------------------------------
    @{ Name = 'address-without-route-blocks'; Round = '-'; Kind = '결함'
       # 라우트 없이 주소만 남은 중간 상태가 있다 (3절).
       Expect = 'pick another range'; Check = {
           (Get-SubnetVerdict -Target '10.100.0.0/24' -Routes @(New-Route '0.0.0.0/0') `
                -Addresses @(New-Addr '10.100.0.7' 'Hyper-V')).Verdict } }
    @{ Name = 'address-in-alt-range-23-blocks'; Round = '8'; Kind = '결함'
       # 문자열 '10.100.0.*' 로는 10.100.1.5 를 못 잡는다. 대체 대역에서 조용히 틀린다.
       Expect = 'pick another range'; Check = {
           (Get-SubnetVerdict -Target '10.100.0.0/23' -Addresses @(New-Addr '10.100.1.5')).Verdict } }
    @{ Name = 'address-outside-range-passes'; Round = '21기각'; Kind = '정상'
       Expect = 'proceed'; Check = {
           (Get-SubnetVerdict -Target '10.100.0.0/24' -Routes @(New-Route '0.0.0.0/0') `
                -Addresses @(New-Addr '10.0.0.49'; New-Addr '192.168.1.10'; New-Addr '127.0.0.1')).Verdict } }

    # --- 명시적 무시 --------------------------------------------------------
    @{ Name = 'allow-shadow-overrides-shadow'; Round = '7'; Kind = '정상'
       Expect = 'proceed'; Check = {
           (Get-SubnetVerdict -Target '10.100.0.0/24' -AllowShadow `
                -Routes @(New-Route '0.0.0.0/0'; New-Route '10.0.0.0/8' 'VPN')).Verdict } }
    @{ Name = 'allow-shadow-keeps-route-block'; Round = '7'; Kind = '결함'
       # 명시적 무시는 shadow 에만 쓴다. 우리가 지는 겹침까지 열지 않는다.
       Expect = 'pick another range'; Check = {
           (Get-SubnetVerdict -Target '10.100.0.0/24' -AllowShadow `
                -Routes @(New-Route '10.100.0.5/32' 'VPN')).Verdict } }

    # --- 출력 계약 ----------------------------------------------------------
    @{ Name = 'verdict-is-the-last-line'; Round = '5,16'; Kind = '정상'
       # 목록만 찍고 사람이 눈으로 세게 하지 않는다. 마지막 줄이 판정이다.
       Expect = 'VERDICT: pick another range'; Check = {
           $r = Get-SubnetVerdict -Target '10.100.0.0/24' -Routes @(New-Route '10.0.0.0/8' 'VPN')
           @(Format-VerdictReport $r)[-1] } }
    @{ Name = 'verdict-line-even-when-nothing-overlaps'; Round = '5'; Kind = '정상'
       Expect = 'VERDICT: proceed'; Check = {
           $r = Get-SubnetVerdict -Target '10.100.0.0/24' -Routes @(New-Route '0.0.0.0/0')
           @(Format-VerdictReport $r)[-1] } }
    @{ Name = 'exit-code-proceed'; Round = '-'; Kind = '정상'
       Expect = '0'; Check = { [string](Get-VerdictExitCode 'proceed') } }
    @{ Name = 'exit-code-block'; Round = '-'; Kind = '결함'
       Expect = '1'; Check = { [string](Get-VerdictExitCode 'pick another range') } }
    @{ Name = 'exit-code-unknown'; Round = '9'; Kind = '결함'
       Expect = '2'; Check = { [string](Get-VerdictExitCode 'unknown') } }
)

# ---------------------------------------------------------------------------
# 구현
# ---------------------------------------------------------------------------

function ConvertTo-UInt32Ip {
    <# IPv4 주소를 32비트 수로 바꾼다. IPv4 가 아니면 던진다. #>
    param([string]$Ip)
    $addr = $null
    if (-not [ipaddress]::TryParse($Ip, [ref]$addr)) { throw "[parse] 주소를 읽을 수 없다: $Ip" }
    if ($addr.AddressFamily -ne [System.Net.Sockets.AddressFamily]::InterNetwork) {
        throw "[family] IPv4 가 아니다: $Ip"
    }
    $b = $addr.GetAddressBytes()
    [Array]::Reverse($b)
    return [BitConverter]::ToUInt32($b, 0)
}

function ConvertTo-Prefix {
    <# 접두사 문자열을 검증해 {Net, Len} 으로 바꾼다.

    검증 순서가 곧 오류 이유다. 이유 토큰을 대괄호로 앞에 붙인다. 거부되는 것만 보고
    어느 검사가 걸렀는지 보지 않으면, 정작 그 가드가 한 번도 안 돈 채로 "검증했다" 가 된다.
    #>
    param([string]$Prefix)

    if ([string]::IsNullOrEmpty($Prefix)) { throw '[format] 접두사가 비었다' }
    $parts = $Prefix -split '/'
    if ($parts.Count -ne 2 -or $parts[0] -eq '') { throw "[format] 접두사 형식이 아니다: $Prefix" }

    # 자릿수를 먼저 막는다. [int]'' 는 0 이라 빈 길이가 /0 으로 읽히고, 긴 숫자는
    # 캐스팅에서 예외가 난다. 둘 다 여기서 거부한다.
    if ($parts[1] -notmatch '^\d{1,2}$') { throw "[digits] 길이가 한두 자리 숫자가 아니다: $Prefix" }
    $len = [int]$parts[1]
    if ($len -gt 32) { throw "[range] IPv4 접두사 길이가 아니다: $Prefix" }

    $addrText = $parts[0]
    if ($addrText -notmatch ':') {
        # IPv6 는 여기서 거르지 않는다. 뒤의 family 가드가 잡아야 한다.
        if ($addrText -notmatch '^\d{1,3}(\.\d{1,3}){3}$') {
            throw "[shorthand] 점 넷으로 끊긴 IPv4 가 아니다: $Prefix"
        }
        if ($addrText -match '(^|\.)0\d') { throw "[leadingzero] 8진수로 읽힐 수 있다: $Prefix" }
    }

    return [pscustomobject]@{ Net = (ConvertTo-UInt32Ip $addrText); Len = $len }
}

function Get-PrefixMask {
    <# 길이를 32비트 마스크로 바꾼다. 길이 0 은 마스크 0 이다. #>
    param([int]$Length)
    return [uint32](([uint64]1 -shl 32) - ([uint64]1 -shl (32 - $Length)))
}

function Test-PrefixOverlap {
    <# 접두사 하나가 대상 대역과 겹치는지 보고 판정을 낸다.

    **짧은 쪽 마스크로 비교한다.** 자기 마스크로만 비교하면 우리 대역 안의 호스트
    라우트를 "덮지 않는다" 로 읽는다. 겹침은 양방향이다.
    #>
    param([string]$Prefix, [uint32]$TargetNet, [int]$TargetLen)

    $p = ConvertTo-Prefix $Prefix
    $mask = Get-PrefixMask ([Math]::Min($p.Len, $TargetLen))
    $overlap = (($p.Net -band $mask) -eq ($TargetNet -band $mask))

    # **순서가 판정이다.** 같은 길이 이상인 겹침을 `catch-all` 예외보다 먼저 본다.
    # 반대로 두면 대상이 `/1` 일 때 같은 `/1` 라우트를 `catch-all` 로 읽고 놓친다.
    # 대상 검증이 `/0`·`/1` 을 막으므로 지금은 도달할 수 없는 경로지만, 검증이 느슨해지는
    # 날 조용히 틀리지 않도록 순서로 막아 둔다. 케이스로 고정했다.
    $verdict = if (-not $overlap)             { 'ignore' }
               elseif ($p.Len -ge $TargetLen) { 'BLOCK-route' }
               elseif ($p.Len -le 1)          { 'catch-all' }
               else                           { 'BLOCK-shadow' }

    return [pscustomobject]@{ Prefix = $Prefix; Len = $p.Len; Overlap = $overlap; Verdict = $verdict }
}

function ConvertTo-NormalGuid {
    <# GUID 문자열을 한 가지 모양으로 맞춘다. 읽을 수 없으면 $null 이다.
       중괄호 유무와 대소문자는 표기 차이일 뿐이라 여기서 흡수한다. #>
    param([string]$Text)
    $g = [guid]::Empty
    if ([string]::IsNullOrEmpty($Text)) { return $null }
    if (-not [guid]::TryParse($Text, [ref]$g)) { return $null }
    return $g.ToString('B').ToUpperInvariant()
}

function New-OwnDecision {
    param([string]$Reason, $Index = $null, [string]$Name = '', [string]$Guid = '')
    [pscustomobject]@{ Reason = $Reason; Index = $Index; Name = $Name; Guid = $Guid }
}

function Resolve-OwnInterface {
    <# 판정에서 뺄 우리 어댑터를 정한다.

    ## 소유 계약 (여기가 원문이다. 다른 곳에 옮겨 적지 않는다)

    **이름과 드라이버 설명은 소유의 증거가 아니다.** 둘 다 남이 똑같이 쓸 수 있다.
    요청한 이름을 쓰는 남의 Wintun 어댑터가 있으면, 그 어댑터의 겹치는 라우트와 주소가
    조용히 빠져 **진짜 충돌이 통과한다.** 그래서 소유는 어댑터 하나를 가리키는 값으로만
    증명한다.

    소유 표식은 `InterfaceGuid` 다. **클라이언트가 지켜야 할 계약은 이것이다.**

    1. 클라이언트는 Wintun 어댑터를 만들 때 쓴 `InterfaceGuid` 를 자기 상태에 기록한다.
       Wintun 은 어댑터 생성 시 GUID 를 지정할 수 있으므로 값을 아는 쪽은 클라이언트다.
    2. 이 검사를 부를 때 그 값을 `-OwnInterfaceGuid` 로 넘긴다.
    3. 어댑터를 지우면 기록도 지운다. 낡은 GUID 는 `not-found` 가 되고, 그러면 아무것도
       빼지 않으므로 판정이 엄격해지는 쪽으로만 틀린다.

    아래 다섯이 모두 참일 때만 뺀다. 하나라도 어긋나면 **아무것도 빼지 않고** 이유를 낸다.

    | 조건 | 어긋났을 때 이유 |
    |------|------------------|
    | `-OwnInterfaceGuid` 를 줬다 | `no-guid` / `alias-only` |
    | 그 값이 GUID 로 읽힌다 | `bad-guid` |
    | **어댑터 목록을 실제로 조회했다** | `query-failed` |
    | 그 GUID 를 가진 어댑터가 정확히 하나다 | `not-found` / `ambiguous` |
    | 그 어댑터의 설명이 Wintun 이다 | `not-wintun` |
    | `-OwnAlias` 를 줬다면 이름도 같다 | `alias-mismatch` |

    **조회 실패와 "없음" 은 다르다** (`windows-prereq.md` 0장의 계약). 실패를 빈 목록으로 읽으면
    소유를 확정하지 못한 기기가 조용히 `proceed` 로 끝난다. 그래서 `query-failed` 는 다른 이유와
    달리 **대역 판정 자체를 `unknown` 으로 올린다.** 조회에 성공했는데 우리 GUID 가 없는
    `not-found` 는 사실이므로 `unknown` 이 아니다. 두 경우를 케이스로 나눠 고정했다.

    **제외는 `InterfaceIndex` 로 한다.** 이름으로 빼면 같은 이름의 다른 어댑터까지 빠진다.
    인덱스를 읽을 수 없으면 `no-index` 이고 역시 아무것도 빼지 않는다.

    **거짓 통과보다 거짓 차단이 낫다.** 우리 잔존물 때문에 차단이 나오는 경우는
    `windows-prereq.md` 3절 정리를 먼저 돌려서 없앨 수 있다. 반대로 남의 어댑터를 우리 것으로
    착각해 빼면 blocker 17 을 닫지 못한다.
    #>
    param([string]$OwnInterfaceGuid, [string]$RequestedAlias, [object[]]$Adapters = @(),
          [switch]$AdapterQueryFailed)

    if ([string]::IsNullOrEmpty($OwnInterfaceGuid)) {
        if (-not [string]::IsNullOrEmpty($RequestedAlias)) { return (New-OwnDecision 'alias-only') }
        return (New-OwnDecision 'no-guid')
    }

    $want = ConvertTo-NormalGuid $OwnInterfaceGuid
    if ($null -eq $want) { return (New-OwnDecision 'bad-guid') }

    # **빈 목록으로 흉내 내지 않는다.** 실패는 호출하는 쪽이 이 스위치로 알려 준다.
    if ($AdapterQueryFailed) { return (New-OwnDecision 'query-failed') }

    $match = @($Adapters | Where-Object {
        (ConvertTo-NormalGuid ([string]$_.InterfaceGuid)) -eq $want
    })
    # 조회에 성공했는데 없는 것이다. 사실이므로 unknown 이 아니다. 다만 아무것도 빼지 않는다.
    if ($match.Count -eq 0) { return (New-OwnDecision 'not-found') }
    if ($match.Count -gt 1) { return (New-OwnDecision 'ambiguous') }

    $a = $match[0]
    if ([string]$a.InterfaceDescription -notlike '*Wintun*') { return (New-OwnDecision 'not-wintun') }
    if (-not [string]::IsNullOrEmpty($RequestedAlias) -and [string]$a.Name -ne $RequestedAlias) {
        return (New-OwnDecision 'alias-mismatch')
    }
    $index = [string]$a.InterfaceIndex
    if ($index -notmatch '^\d+$') { return (New-OwnDecision 'no-index') }

    return (New-OwnDecision 'ok' $index ([string]$a.Name) $want)
}

function Get-SubnetVerdict {
    <# 라우트와 주소 목록을 받아 기기 한 대의 판정을 낸다.

    조회는 하지 않는다. 자료를 받기만 한다. 그래야 케이스 표로 돌릴 수 있다.
    `Unavailable` 은 조회하지 못한 자료원 이름이다. 실패를 "없음" 으로 읽지 않는다.
    #>
    param(
        [Parameter(Mandatory = $true)][string]$Target,
        [object[]]$Routes = @(),
        [object[]]$Addresses = @(),
        [object]$OwnIndex = $null,
        [switch]$AllowShadow,
        [string[]]$Unavailable = @()
    )

    $t = ConvertTo-Prefix $Target
    # **대상은 `/2` 이상만 받는다.** `/0` 과 `/1` 은 가상 LAN 대역이 아니고, 받아들이면
    # `catch-all` 예외(길이 0~1 은 충돌로 세지 않는다)와 **같은 길이 충돌**이 겹쳐서
    # 같은 `/1` 라우트를 놓친다. 거부가 더 단순하고 안전하다.
    #
    # `/2` 이상을 받으면서도 기존 `/0`·`/1` 라우트를 계속 `catch-all` 로 두는 것은
    # 문서(`windows-prereq.md` 7절)의 결정이다. 기본 라우트를 가리는 것은 어떤 터널이든
    # 하는 일이므로 충돌로 세지 않는다. 그 결정은 그대로 두고, 같은 길이 충돌만 막는다.
    if ($t.Len -lt 2) { throw "[targetlen] 대상 대역 길이는 2~32 이다: $Target" }
    if (($t.Net -band (Get-PrefixMask $t.Len)) -ne $t.Net) {
        throw "[hostbits] 대상 대역이 네트워크 주소가 아니다: $Target"
    }

    # **제외는 인터페이스 인덱스로만 한다.** 이름은 남이 쓸 수 있다. 소유 판정은
    # `Resolve-OwnInterface` 가 하고 여기는 그 결과만 받는다.
    $ownIndexText = if ($null -eq $OwnIndex) { '' } else { [string]$OwnIndex }

    $rows = @()
    $unparsed = @()

    foreach ($r in $Routes) {
        $prefix = [string]$r.DestinationPrefix
        # IPv6 는 이 판정의 대상이 아니다. -AddressFamily IPv4 를 줘도 섞일 수 있다.
        if ($prefix.Contains(':')) { continue }
        $alias = [string]$r.InterfaceAlias
        if ($ownIndexText -and [string]$r.InterfaceIndex -eq $ownIndexText) { continue }
        try {
            $hit = Test-PrefixOverlap $prefix $t.Net $t.Len
        }
        catch {
            $unparsed += "route '$prefix': $($_.Exception.Message)"
            continue
        }
        if ($hit.Overlap) {
            $rows += [pscustomobject]@{
                Verdict = $hit.Verdict; Len = $hit.Len; Prefix = $prefix; Interface = $alias
            }
        }
    }

    $addrRows = @()
    foreach ($a in $Addresses) {
        $ip = [string]$a.IPAddress
        if ($ip.Contains(':')) { continue }
        $alias = [string]$a.InterfaceAlias
        if ($ownIndexText -and [string]$a.InterfaceIndex -eq $ownIndexText) { continue }
        try {
            # 주소를 /32 로 만들어 같은 겹침 함수에 넣는다. 대역이 바뀌어도 따라간다.
            $hit = Test-PrefixOverlap ($ip + '/32') $t.Net $t.Len
        }
        catch {
            $unparsed += "address '$ip': $($_.Exception.Message)"
            continue
        }
        if ($hit.Overlap) {
            $addrRows += [pscustomobject]@{
                Verdict = 'BLOCK-address'; Len = 32; Prefix = $ip; Interface = $alias
            }
        }
    }

    $blockRoute  = @($rows | Where-Object { $_.Verdict -eq 'BLOCK-route' }).Count
    $blockShadow = @($rows | Where-Object { $_.Verdict -eq 'BLOCK-shadow' }).Count
    $blockAddr   = $addrRows.Count
    $shadowBlocks = if ($AllowShadow) { 0 } else { $blockShadow }

    # 확실한 차단이 하나라도 있으면 그것이 답이다. 없을 때만 판정 불가를 낸다.
    $verdict = if (($blockRoute + $shadowBlocks + $blockAddr) -gt 0) { 'pick another range' }
               elseif ($Unavailable.Count -gt 0 -or $unparsed.Count -gt 0) { 'unknown' }
               else { 'proceed' }

    return [pscustomobject]@{
        Target       = $Target
        OwnIndex     = $ownIndexText
        AllowShadow  = [bool]$AllowShadow
        Rows         = @($rows + $addrRows)
        BlockRoute   = $blockRoute
        BlockShadow  = $blockShadow
        BlockAddress = $blockAddr
        Unparsed     = $unparsed
        Unavailable  = $Unavailable
        Verdict      = $verdict
    }
}

function Get-QuerySource {
    <# 자료원 하나를 조회한다. **실패와 "없음" 을 구분해서 낸다** (`windows-prereq.md` 0장).

    조회를 스크립트 블록으로 받는 이유는 이 구분을 케이스 표로 돌리기 위해서다. 라이브 안에
    try/catch 를 숨겨 두면 그 갈래를 자기검사가 못 밟고, 실패를 삼키는 변이가 통과한다.
    실제로 한 번 통과했다.

    `Needed` 가 거짓이면 조회하지 않는다. 그것은 실패가 아니다.
    #>
    param([string]$Name, [scriptblock]$Query, [bool]$Needed = $true)

    if (-not $Needed) {
        return [pscustomobject]@{ Name = $Name; Items = @(); Failed = $false; Error = '' }
    }
    try {
        return [pscustomobject]@{ Name = $Name; Items = @(& $Query); Failed = $false; Error = '' }
    }
    catch {
        return [pscustomobject]@{ Name = $Name; Items = @(); Failed = $true; Error = $_.Exception.Message }
    }
}

function Invoke-Check {
    <# 조회부터 판정까지의 **유일한 배선**이다. 라이브는 실제 cmdlet 을, 자기검사는 가짜
    스크립트 블록을 넘긴다. 배선을 두 벌 두면 자기검사가 라이브를 흉내 내게 되고, 흉내가
    틀렸을 때 케이스 표가 틀린 계약을 고정한다. 이 파일에서 두 번 겪었다. #>
    param(
        [Parameter(Mandatory = $true)][string]$Target,
        [scriptblock]$RouteQuery,
        [scriptblock]$AddrQuery,
        [scriptblock]$AdapterQuery,
        [string]$OwnInterfaceGuid = '',
        [string]$OwnAlias = '',
        [switch]$AllowShadow
    )

    $routeSrc = Get-QuerySource 'routes'    $RouteQuery
    $addrSrc  = Get-QuerySource 'addresses' $AddrQuery
    # GUID 를 안 줬으면 어댑터를 조회하지 않는다. 부르지 않은 조회는 실패가 아니다.
    $adapterSrc = Get-QuerySource 'adapters' $AdapterQuery ([bool]$OwnInterfaceGuid)

    # 실패한 자료원만 이름을 올린다. 라우트와 주소는 판정의 재료이므로 없으면 판정 불가다.
    $unavailable = @(@($routeSrc, $addrSrc) | Where-Object { $_.Failed } |
                     ForEach-Object { "$($_.Name) ($($_.Error))" })

    return Get-OwnAndVerdict -Target $Target -Routes $routeSrc.Items -Addresses $addrSrc.Items `
                             -Adapters $adapterSrc.Items -OwnInterfaceGuid $OwnInterfaceGuid -OwnAlias $OwnAlias `
                             -AdapterQueryFailed:$adapterSrc.Failed -AdapterError $adapterSrc.Error `
                             -AllowShadow:$AllowShadow -Unavailable $unavailable
}

function Get-OwnAndVerdict {
    <# 소유 판정 -> 제외 -> 대역 판정. **라이브와 자기검사가 같은 이 경로를 쓴다.**

    이 함수를 따로 둔 이유가 있다. 케이스가 라이브 코드를 흉내 내면, 흉내가 틀렸을 때
    케이스 표가 틀린 계약을 고정하고 방패가 된다. 실제로 한 번 그렇게 됐다.

    `query-failed` 를 `Unavailable` 에 넣는 자리는 여기 한 곳이다.
    #>
    param(
        [Parameter(Mandatory = $true)][string]$Target,
        [object[]]$Routes = @(),
        [object[]]$Addresses = @(),
        [object[]]$Adapters = @(),
        [string]$OwnInterfaceGuid,
        [string]$OwnAlias,
        [switch]$AdapterQueryFailed,
        [string]$AdapterError = '',
        [switch]$AllowShadow,
        [string[]]$Unavailable = @()
    )

    $own = Resolve-OwnInterface -OwnInterfaceGuid $OwnInterfaceGuid -RequestedAlias $OwnAlias `
                                -Adapters $Adapters -AdapterQueryFailed:$AdapterQueryFailed
    # **이유가 아니라 사실로 판단한다.** 조회가 실패했다면 `bad-guid` 처럼 먼저 걸린 이유가
    # 있어도 자료원은 여전히 없다. 이유로 판단하면 입력 오류가 조회 실패를 가린다.
    $sources = @($Unavailable)
    if ($AdapterQueryFailed) {
        $sources += if ($AdapterError) { "adapters ($AdapterError)" } else { 'adapters' }
    }

    $result = Get-SubnetVerdict -Target $Target -Routes $Routes -Addresses $Addresses `
                                -OwnIndex $own.Index -AllowShadow:$AllowShadow -Unavailable $sources
    return [pscustomobject]@{ Own = $own; Result = $result }
}

function Format-VerdictReport {
    <# 판정을 줄 목록으로 만든다. **마지막 줄이 판정이다.**

    Format-Table 을 쓰지 않는다. 값이 길면 열이 통째로 사라져 판정을 숨긴다. 이 저장소에서
    이미 한 번 겪은 일이다.
    #>
    param([Parameter(Mandatory = $true)]$Result)

    $lines = @("대상 대역: $($Result.Target)")
    if ($Result.OwnIndex) { $lines += "제외한 어댑터: ifIndex $($Result.OwnIndex)" }
    else { $lines += '제외한 어댑터: 없음' }
    if ($Result.AllowShadow) { $lines += 'BLOCK-shadow 를 명시적으로 무시한다 (-AllowShadow)' }

    if (@($Result.Rows).Count -eq 0) {
        $lines += '겹치는 항목: 없음'
    }
    else {
        $lines += '겹치는 항목:'
        foreach ($row in (@($Result.Rows) | Sort-Object Len -Descending)) {
            $lines += ('  {0,-13} /{1,-2}  {2,-20} {3}' -f $row.Verdict, $row.Len, $row.Prefix, $row.Interface)
        }
    }
    foreach ($u in $Result.Unavailable) { $lines += "조회 실패: $u" }
    foreach ($u in $Result.Unparsed)    { $lines += "해석 실패: $u" }

    $lines += ("BLOCK-route: {0}  BLOCK-shadow: {1}  BLOCK-address: {2}" -f
               $Result.BlockRoute, $Result.BlockShadow, $Result.BlockAddress)
    $lines += "VERDICT: $($Result.Verdict)"
    return $lines
}

function Get-VerdictExitCode {
    param([string]$Verdict)
    switch ($Verdict) {
        'proceed'            { return 0 }
        'pick another range' { return 1 }
        default              { return 2 }
    }
}

# --- 케이스 표를 위한 보조 ---------------------------------------------------

function New-Route {
    # NextHop 을 일부러 담는다. 판정이 그것을 쓰지 않는다는 것을 케이스로 고정한다.
    param([string]$Prefix, [string]$Alias = 'Wi-Fi', [string]$NextHop = '0.0.0.0', $Index = 5)
    [pscustomobject]@{ DestinationPrefix = $Prefix; InterfaceAlias = $Alias
                       NextHop = $NextHop; InterfaceIndex = $Index }
}

function New-Addr {
    param([string]$Ip, [string]$Alias = 'Wi-Fi', $Index = 5)
    [pscustomobject]@{ IPAddress = $Ip; InterfaceAlias = $Alias; InterfaceIndex = $Index }
}

function New-Adapter {
    param([string]$Name, [string]$Description, [string]$Guid = '{11111111-1111-4111-8111-111111111111}', $Index = 12)
    [pscustomobject]@{ Name = $Name; InterfaceDescription = $Description
                       InterfaceGuid = $Guid; InterfaceIndex = $Index }
}

function Invoke-OwnCheck {
    <# 소유 판정과 대역 판정을 한 줄로 낸다. `<이유>/<판정>/r<route>a<address>` 다.
       **라이브와 같은 `Get-OwnAndVerdict` 를 부른다.** 흉내 내지 않는다. #>
    param($Guid, $Alias, [object[]]$Adapters = @(), [object[]]$Routes = @(),
          [object[]]$Addresses = @(), [string]$Target = '10.100.0.0/24', [switch]$QueryFailed)
    $b = Get-OwnAndVerdict -Target $Target -Routes $Routes -Addresses $Addresses -Adapters $Adapters `
                           -OwnInterfaceGuid $Guid -OwnAlias $Alias -AdapterQueryFailed:$QueryFailed
    return "{0}/{1}/r{2}a{3}" -f $b.Own.Reason, $b.Result.Verdict, $b.Result.BlockRoute, $b.Result.BlockAddress
}

function Invoke-LivePath {
    <# 라이브와 **같은 `Invoke-Check`** 를 부른다. cmdlet 자리에 가짜 조회만 넣는다. #>
    param([scriptblock]$RouteQuery, [scriptblock]$AddrQuery, [scriptblock]$AdapterQuery,
          [string]$Guid = '', [string]$Alias = '')
    $b = Invoke-Check -Target '10.100.0.0/24' -RouteQuery $RouteQuery -AddrQuery $AddrQuery `
                      -AdapterQuery $AdapterQuery -OwnInterfaceGuid $Guid -OwnAlias $Alias
    return "{0}/{1}" -f $b.Own.Reason, $b.Result.Verdict
}

function Get-CaseVerdict {
    param([string]$Prefix)
    return (Test-PrefixOverlap $Prefix $script:T.Net $script:T.Len).Verdict
}

function Get-RejectReason {
    <# 거부 이유 토큰만 낸다. 던지지 않으면 'no-throw' 다. #>
    param([scriptblock]$Action)
    try { & $Action | Out-Null }
    catch {
        if ($_.Exception.Message -match '^\[(\w+)\]') { return $Matches[1] }
        return "untagged: $($_.Exception.Message)"
    }
    return 'no-throw'
}

function Invoke-SelfTest {
    <# 케이스 표 전체를 돌린다. 마지막 줄이 SELFTEST: pass 또는 fail 이다. #>
    $script:T = ConvertTo-Prefix '10.100.0.0/24'

    $failed = @()
    foreach ($case in $script:Cases) {
        $actual = $null
        try { $actual = [string](& $case.Check) }
        catch { $actual = "예외: $($_.Exception.Message)" }

        if ($actual -ceq [string]$case.Expect) {
            Write-Host ('PASS  r{0,-7} {1,-8} {2}' -f $case.Round, $case.Kind, $case.Name)
        }
        else {
            $failed += $case.Name
            Write-Host ('FAIL  r{0,-7} {1,-8} {2}' -f $case.Round, $case.Kind, $case.Name)
            Write-Host ("        기대: '{0}'" -f $case.Expect)
            Write-Host ("        실제: '{0}'" -f $actual)
        }
    }

    $total = @($script:Cases).Count
    Write-Host ''
    Write-Host ("케이스 {0}건 중 {1}건 통과, {2}건 실패" -f $total, ($total - $failed.Count), $failed.Count)
    if ($failed.Count -gt 0) {
        Write-Host ('실패: ' + ($failed -join ', '))
        Write-Host 'SELFTEST: fail'
        return 1
    }
    Write-Host 'SELFTEST: pass'
    return 0
}

function Invoke-LiveCheck {
    <# 이 기기를 조회해 판정한다. **조회 실패를 "없음" 으로 읽지 않는다.**

    이 함수에 남은 것은 cmdlet 이름과 출력뿐이다. 배선은 `Invoke-Check`, 조회 구분은
    `Get-QuerySource`, 판정은 `Get-OwnAndVerdict` 가 하고 셋 다 케이스 표가 돌린다.
    #>
    param([string]$Target, [string]$OwnInterfaceGuid, [string]$OwnAlias, [switch]$AllowShadow)

    $bundle = Invoke-Check -Target $Target `
        -RouteQuery   { Get-NetRoute -AddressFamily IPv4 -ErrorAction Stop } `
        -AddrQuery    { Get-NetIPAddress -AddressFamily IPv4 -ErrorAction Stop } `
        -AdapterQuery { Get-NetAdapter -IncludeHidden -ErrorAction Stop } `
        -OwnInterfaceGuid $OwnInterfaceGuid -OwnAlias $OwnAlias -AllowShadow:$AllowShadow
    $own = $bundle.Own

    if ($own.Reason -eq 'ok') {
        Write-Host "소유 확인: '$($own.Name)' ifIndex $($own.Index) GUID $($own.Guid). 이 어댑터만 뺀다."
    }
    elseif ($own.Reason -eq 'query-failed') {
        Write-Host "소유 미확정 (query-failed). 어댑터 조회가 실패했다. 아무것도 빼지 않고 판정을 unknown 으로 올린다."
    }
    elseif ($own.Reason -ne 'no-guid') {
        # 소유를 확정할 수 없다. 그 사실을 적는다. 조용히 넘어가지 않는다.
        Write-Host "소유 미확정 ($($own.Reason)). 아무것도 빼지 않는다. 우리 잔존물이 있으면 차단으로 나올 수 있다."
    }

    Format-VerdictReport $bundle.Result | ForEach-Object { Write-Host $_ }
    return (Get-VerdictExitCode $bundle.Result.Verdict)
}

# ---------------------------------------------------------------------------
# 진입점
# ---------------------------------------------------------------------------
if ($SelfTest) {
    exit (Invoke-SelfTest)
}

try {
    $code = Invoke-LiveCheck -Target $Target -OwnInterfaceGuid $OwnInterfaceGuid `
                             -OwnAlias $OwnAlias -AllowShadow:$AllowShadow
}
catch {
    # 판정을 낼 수 없다. 통과로 적지 않는다.
    Write-Host "오류: $($_.Exception.Message)"
    Write-Host 'VERDICT: unknown'
    exit 2
}
exit $code
