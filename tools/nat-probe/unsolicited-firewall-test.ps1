<#
.SYNOPSIS
  요청하지 않은 인바운드가 Windows 방화벽에 막히는지 시험한다.

.DESCRIPTION
  natprobe 의 --unsolicited 가 blocked 로 나왔을 때, 그 원인이 호스트 방화벽인지
  NAT 인지 좁힌다. 방화벽을 끄지 않고 범위를 좁힌 임시 인바운드 허용 규칙 하나를
  넣었다 뺀다. 그 규칙이 ADR 0002 옵션 a 가 실제로 할 일과 같다.

  대조 시행과 규칙 시행을 번갈아 돌리고, 쌍마다 순서를 뒤집는다. 한 번의 변화는
  NAT 상태나 시각 차이로도 생기고, 늘 같은 순서로 돌리면 매핑 워밍업 같은 시간
  의존 효과가 방화벽 효과를 그대로 흉내 낸다.

  **규칙에 상대 주소를 넣지 않는다.** 포트로만 좁힌다. 상대 주소로 좁히면
  (a) 요청하지 않은 패킷의 출발지 IP 가 그 주소와 같다고 가정하게 되고, 주소 의존
  NAT 이면 규칙이 아예 걸리지 않아 "방화벽이 원인이 아니다" 라는 틀린 결론이 나온다.
  (b) 주소를 입력받아 다루는 동안 터미널 기록과 로그로 샌다.
  대가는 규칙이 조금 넓어지는 것이다. UDP 한 포트에 대해 한 시행 동안만 열린다.

  **비정상 종료 시 규칙이 영구히 남는다.** New-NetFirewallRule 은 기본으로
  PersistentStore 에 규칙을 만든다. 프로세스를 강제 종료하거나 창을 닫거나 재부팅하면
  finally 가 돌지 않아 UDP 포트가 모든 출발지에 열린 채로 남는다. 시작 전에 이 점을
  화면에 알리고, 남았을 때 지우는 명령도 함께 찍는다.

  docs/kor/decisions/0002-리바인딩-복구-미보장.md 의 재검토 조건에서 부른다.

.PARAMETER Label
  natprobe 에 넘길 --label 의 앞부분. 시행마다 조건이 뒤에 붙는다.

.PARAMETER Port
  고정 로컬 UDP 포트. 규칙을 걸려면 포트가 고정돼야 한다.

.PARAMETER Trials
  대조/규칙 쌍을 몇 번 돌릴지. **짝수여야 하고 최소 2다.** 기본 4.
  쌍마다 순서를 뒤집으므로 짝수여야 두 순서가 같은 횟수로 나온다.

.EXAMPLE
  .\unsolicited-firewall-test.ps1 -Label us-home
#>
[CmdletBinding()]
param(
    # natprobe 가 파일 이름을 만들 때 이미 영숫자·하이픈·밑줄 외를 치환하므로 경로
    # 탈출은 그쪽에서도 막힌다. 여기서 한 번 더 막는 것은 심층 방어다. 관리자 권한으로
    # 도는 스크립트가 검증하지 않은 값을 그대로 넘기지 않는다.
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[A-Za-z0-9._-]+$')]
    [ValidateScript({ $_ -notmatch '\.\.' })]
    [string]$Label,
    [ValidateRange(1, 65535)][int]$Port = 47000,
    [ValidateRange(2, 10)][int]$Trials = 4
)

# 방화벽 cmdlet 은 기본이 비종료 오류다. 규칙 생성이 실패했는데 측정이 그대로
# 돌면 "규칙을 넣어도 막힌다" 는 거짓 결론이 나온다. 전부 종료 오류로 올린다.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if ($Trials % 2 -ne 0) {
    throw ("-Trials 는 짝수여야 한다 (받은 값: $Trials). 쌍마다 순서를 뒤집으므로 " +
           "홀수면 한쪽 순서가 한 번 더 돌아 순서 효과가 남는다.")
}

$Group = 'natprobe-unsolicited-test'

$mutex    = New-Object System.Threading.Mutex($false, "Global\natprobe-unsolicited-firewall-test")
$acquired = $false

# 획득과 해제를 하나의 try/finally 로 감싼다. 획득 직후에 예외가 나도 놓아야 한다.
try {

try {
    $acquired = $mutex.WaitOne(0)
}
catch [System.Threading.AbandonedMutexException] {
    # 이전 실행이 강제 종료되며 뮤텍스를 버렸다. **소유권은 우리에게 넘어온다.**
    # 이것을 오류로 처리하면 정상 재실행이 막히고 뮤텍스도 계속 잡힌 채로 남는다.
    $acquired = $true
    Write-Warning ("이전 실행이 비정상 종료된 흔적이 있다. 남은 방화벽 규칙이 없는지 " +
                   "아래에서 확인한다.")
}
if (-not $acquired) {
    throw "이 시험이 이미 돌고 있다. 동시에 두 개를 돌리면 같은 포트와 규칙 그룹으로 서로를 방해한다."
}

# 방화벽 조회 실패를 "규칙 없음" 으로 읽으면 안 된다. 서비스 이상이나 권한 문제로
# 조회가 실패했는데 비어 있다고 판단하면, 남은 허용 규칙을 못 보고 측정하거나
# 정리가 끝났다고 잘못 보고한다.
#
# **예외 형식으로 잡지 않는다.** "일치 항목 없음" 은 CimJobException 으로도 오고
# CimException 으로도 온다. 형식 한정 catch 를 쓰면 정상적인 "없음" 이 빠져나가
# 깨끗한 초기 상태에서도 스크립트가 중단된다. 범주와 오류 ID 로 판별한다.
#
# **완벽히 구분하지는 못한다.** 아래 두 신호가 모두 "인스턴스 없음" 을 가리킬 때만
# 빈 목록으로 돌리고, 나머지는 전부 던진다.
#
# **호출할 때 반드시 @( ) 로 감싼다.** PowerShell 은 빈 배열을 파이프라인에서 풀어
# 아무것도 내보내지 않으므로 받는 쪽 변수가 $null 이 된다. StrictMode 에서는 $null 에
# .Count 를 쓰면 예외가 나고, 규칙이 하나도 없는 정상 상태에서 스크립트가 중단된다.
function Get-TestRuleSafe {
    param([hashtable]$Query)
    try {
        return @(Get-NetFirewallRule @Query -ErrorAction Stop)
    }
    catch {
        $isNoMatch = ($_.CategoryInfo.Category -eq 'ObjectNotFound') -and
                     ($_.FullyQualifiedErrorId -like 'CmdletizationQuery_NotFound*')
        if ($isNoMatch) { return @() }
        # 원본 예외는 방화벽 정책과 시스템 정보를 담을 수 있다. -Verbose 로만 보여 준다.
        Write-Verbose "조회 오류 원문: $_"
        throw ("방화벽 규칙 조회에 실패했다. 상태를 확인할 수 없어 중단한다. " +
               "자세한 원인은 -Verbose 로 다시 실행해 확인한다")
    }
}

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$probe    = Join-Path $PSScriptRoot 'natprobe.py'
if (-not (Test-Path $probe)) { throw "natprobe.py 를 찾을 수 없다: $probe" }

# --- 관리자 확인 -----------------------------------------------------------
$me = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $me.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "관리자 PowerShell 에서 실행해야 한다."
}

# --- 남은 규칙 확인 --------------------------------------------------------
# 지우지 않고 보고만 한다. 무조건 지우면 동시에 도는 다른 시행의 규칙을 없애
# 양쪽 측정을 다 망친다.
# 두 저장소를 다 본다. PersistentStore 에서 지워졌어도 ActiveStore 에 아직 적용 중인
# 규칙이 있으면 첫 대조 시행이 오염된다.
$stale  = @(Get-TestRuleSafe @{ Group = $Group })
$stale += @(Get-TestRuleSafe @{ PolicyStore = 'ActiveStore'; Group = $Group })
if ($stale.Count -gt 0) {
    $stale | Select-Object PolicyStoreSource, DisplayName, Enabled |
        Format-Table | Out-String | Write-Host
    throw ("이전 시행의 규칙이 남아 있다. 다른 시행이 돌고 있지 않은지 확인한 뒤 지워라: " +
           "Get-NetFirewallRule -Group '$Group' | Remove-NetFirewallRule")
}

# --- 한 번 돌리기 -----------------------------------------------------------
function Invoke-Trial {
    param([bool]$WithRule, [int]$Pair, [int]$Slot)

    # 라벨에 쌍 번호, 순서, 규칙 유무를 싣는다. 결과 파일 이름만 봐도
    # 어느 조건에서 나온 값인지 알 수 있어야 한다.
    $kind = if ($WithRule) { 'rule' } else { 'control' }
    $trialLabel = "$Label-p$Pair-s$Slot-$kind"

    $rule = $null
    try {
        if ($WithRule) {
            $rule = New-NetFirewallRule `
                -DisplayName ("natprobe-temp-" + (New-Guid).Guid) `
                -Group $Group `
                -Direction Inbound -Protocol UDP -LocalPort $Port `
                -Action Allow -Profile Any

            # **ActiveStore 에서 확인한다.** 만든 규칙은 PersistentStore 에 들어가고,
            # GPO 병합 결과가 실제로 적용되는 것은 ActiveStore 다. 만든 객체의
            # Enabled 만 보면 적용되지 않은 규칙을 활성으로 오인해 결론이 뒤집힌다.
            # 정책 저장소 반영에 시간이 걸릴 수 있다. 한 번만 보고 단정하면 정상
            # 시행을 실패로 중단한다. 삭제 확인과 같은 방식으로 기다린다.
            # 21회다. 20회면 마지막 실제 확인이 4.75초이고, 그 뒤 250ms 동안 반영된
            # 정상 상태를 실패로 처리한다. 마지막 대기 뒤에 한 번 더 본다.
            $active = @()
            foreach ($attempt in 1..21) {
                $active = @(Get-TestRuleSafe @{ PolicyStore = 'ActiveStore'; Name = $rule.Name })
                if ($active.Count -eq 1) { break }
                if ($attempt -lt 21) { Start-Sleep -Milliseconds 250 }
            }
            if ($active.Count -ne 1) {
                throw "규칙이 5초 안에 ActiveStore 에 적용되지 않았다: $($rule.Name)"
            }
            $active = $active[0]
            if ($active.Enabled -ne 'True' -or $active.Action -ne 'Allow' -or
                $active.Direction -ne 'Inbound') {
                throw ("ActiveStore 의 규칙 상태가 기대와 다르다. " +
                       "Enabled=$($active.Enabled) Action=$($active.Action) " +
                       "Direction=$($active.Direction)")
            }

            # 존재와 Enabled 만으로는 부족하다. 정책 병합으로 범위가 달라진 규칙이
            # 적용될 수 있고, 그러면 우리가 의도한 규칙이 걸렸다고 착각한 채 측정한다.
            $portFilter = $active | Get-NetFirewallPortFilter
            if ($portFilter.Protocol -ne 'UDP' -or
                "$($portFilter.LocalPort)" -ne "$Port") {
                throw ("ActiveStore 규칙의 포트 범위가 기대와 다르다. " +
                       "Protocol=$($portFilter.Protocol) LocalPort=$($portFilter.LocalPort) " +
                       "(기대: UDP/$Port)")
            }
            if ($active.Profile -ne 'Any') {
                throw "ActiveStore 규칙의 프로필이 Any 가 아니다: $($active.Profile)"
            }

            # **이 시험의 핵심 전제는 "모든 출발지에 열린다" 는 것이다.** 정책 병합으로
            # 주소 범위가 좁혀진 규칙이 적용되면, 요청하지 않은 패킷이 그 범위 밖에서
            # 올 때 규칙이 걸리지 않는다. 그 결과를 "방화벽이 원인이 아니다" 로 읽게 된다.
            $addrFilter = $active | Get-NetFirewallAddressFilter
            if ($addrFilter.RemoteAddress -ne 'Any' -or $addrFilter.LocalAddress -ne 'Any') {
                throw ("ActiveStore 규칙의 주소 범위가 Any 가 아니다. " +
                       "RemoteAddress=$($addrFilter.RemoteAddress) " +
                       "LocalAddress=$($addrFilter.LocalAddress)")
            }
        }

        Push-Location $repoRoot
        try {
            & py $probe punch --label $trialLabel --port $Port --unsolicited
            # 측정이 실패했는데 계속 돌면 불완전한 결과를 정상 측정으로 읽게 된다.
            if ($LASTEXITCODE -ne 0) {
                throw "natprobe 가 종료 코드 $LASTEXITCODE 로 끝났다. 시행을 중단한다."
            }
        }
        finally { Pop-Location }
    }
    finally {
        if ($rule) { Remove-TestRule -Rule $rule }
    }
}

# 이름이나 그룹이 아니라 그 객체를 지운다. 동시 시행이 있어도 안전하다.
# **삭제 실패를 숨기지 않는다.** 허용 규칙이 남은 채 다음 대조 시행을 돌리면
# 대조군이 오염돼 비교가 무의미해지고, 규칙이 영구히 남는다.
function Remove-TestRule {
    param($Rule)

    $removeError = $null
    try { Remove-NetFirewallRule -InputObject $Rule }
    catch { $removeError = $_ }

    # **ActiveStore 에서 사라질 때까지 기다린다.** PersistentStore 에서 지워도
    # 적용 중인 규칙이 잠깐 남아 다음 대조 시행을 오염시킬 수 있다.
    # 확인 조회가 실패해도 여기서 빠져나가면 안 된다. 아래 안내가 실행되지 않으면
    # 모든 출발지에 열린 영구 규칙이 남은 채 사용자가 규칙 이름조차 모르게 된다.
    $gone = $false
    $verifyError = $null
    # 21회다. 20회면 마지막 실제 확인이 4.75초이고, 그 뒤 250ms 동안 사라진 정상
    # 상태를 실패로 처리한다. 마지막 대기 뒤에 한 번 더 본다.
    foreach ($attempt in 1..21) {
        try {
            $inPersistent = @(Get-TestRuleSafe @{ Name = $Rule.Name })
            $inActive     = @(Get-TestRuleSafe @{ PolicyStore = 'ActiveStore'; Name = $Rule.Name })
        }
        catch { $verifyError = $_; break }
        if ($inPersistent.Count -eq 0 -and $inActive.Count -eq 0) { $gone = $true; break }
        if ($attempt -lt 21) { Start-Sleep -Milliseconds 250 }
    }

    if (-not $gone -or $removeError -or $verifyError) {
        $cmd = "Remove-NetFirewallRule -Name '$($Rule.Name)'"
        $why = if ($verifyError)   { "삭제 여부를 확인할 수 없다" }
               elseif (-not $gone) { "규칙이 5초 안에 사라지지 않았다" }
               else                { "삭제가 실패했다" }
        # 원본 예외는 방화벽 정책과 시스템 정보를 담을 수 있다. 기본 출력에서는 빼고
        # -Verbose 를 준 경우에만 보여 준다. 정리에 필요한 것은 이름과 명령이다.
        Write-Verbose "삭제 오류 원문: $removeError"
        Write-Verbose "확인 오류 원문: $verifyError"
        throw ("$why. 대조 시행이 오염되므로 전체를 중단한다.`n" +
               "  규칙 이름: $($Rule.Name)`n" +
               "  직접 지워라: $cmd`n" +
               "  자세한 원인은 -Verbose 로 다시 실행해 확인한다")
    }
}

# --- 본체 -------------------------------------------------------------------
$total    = $Trials * 2
$perOrder = $Trials / 2
Write-Host ""
Write-Warning ("이 시험은 UDP $Port 를 모든 출발지에 여는 임시 규칙을 쓴다. " +
               "정상 종료하면 매 시행 끝에 지운다.")
Write-Warning ("**강제 종료, 창 닫기, 재부팅이면 규칙이 영구히 남는다.** 그때는 직접 지운다:")
Write-Host    "    Get-NetFirewallRule -Group '$Group' | Remove-NetFirewallRule"
Write-Host    "    Get-NetFirewallRule -PolicyStore ActiveStore -Group '$Group'"
Write-Host ""
Write-Host "대조(규칙 없음)와 규칙 시행을 $Trials 쌍, 총 $total 회 번갈아 돌린다."
Write-Host "순서는 쌍마다 뒤집는다. 각 순서가 $perOrder 쌍씩이다."
Write-Host "상대도 매 시행마다 같이 돌려야 한다. 상대가 돌릴 횟수도 $total 회다."
Write-Host "상대 명령(레포 루트에서): python tools/nat-probe/natprobe.py punch --label <상대라벨> --port $Port --unsolicited"
Write-Host ""

for ($i = 1; $i -le $Trials; $i++) {
    # 홀수 쌍은 대조 먼저, 짝수 쌍은 규칙 먼저. 순서 효과를 상쇄한다.
    $ruleFirst = ($i % 2 -eq 0)
    $order = if ($ruleFirst) { @($true, $false) } else { @($false, $true) }

    for ($slot = 1; $slot -le 2; $slot++) {
        $withRule = $order[$slot - 1]
        $what = if ($withRule) { '규칙 있음' } else { '대조 (규칙 없음)' }
        Write-Host "=== 쌍 $i/$Trials, $slot 번째: $what - 상대도 지금 시작 ==="
        Invoke-Trial -WithRule $withRule -Pair $i -Slot $slot
    }
}

Write-Host ""
Write-Host "결과 JSON 의 unsolicited.inbound_unsolicited 를 쌍별로 비교한다."
Write-Host "파일 이름의 -p<쌍>-s<순서>-control|rule 로 조건을 구분한다."
Write-Host "해석은 tools/nat-probe/README.md 12.2 를 본다."

# --- 정리 확인 --------------------------------------------------------------
# 시작 확인과 같은 방식으로 두 저장소를 다 본다. 기본 저장소만 보고 "없음" 을
# 찍으면, ActiveStore 에 남은 광범위한 허용 규칙을 놓친 채 끝났다고 보고한다.
$left  = @(Get-TestRuleSafe @{ Group = $Group })
$left += @(Get-TestRuleSafe @{ PolicyStore = 'ActiveStore'; Group = $Group })
if ($left.Count -gt 0) {
    $left | Select-Object PolicyStoreSource, DisplayName | Format-Table | Out-String | Write-Host
    throw ("규칙이 남았다. 위 목록을 지워라: " +
           "Get-NetFirewallRule -Group '$Group' | Remove-NetFirewallRule")
}
Write-Host "남은 규칙 없음 (PersistentStore, ActiveStore 둘 다 확인)."

}
finally {
    if ($acquired) { $mutex.ReleaseMutex() }
    $mutex.Dispose()
}
