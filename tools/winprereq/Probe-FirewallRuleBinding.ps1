# windows-prereq.md 2절의 실측: 어댑터로 좁힌 방화벽 규칙이, 같은 이름과 GUID 로 어댑터를 지웠다 다시 만든 뒤에도
# 그 어댑터에 묶이는가. 관리자 PowerShell 에서 돌린다. 클라이언트는 제품 빌드를 쓴다.
#
#   pwsh -NoProfile -File tools\winprereq\Probe-FirewallRuleBinding.ps1 -ClientExe <sangtachi_client.exe> -LogPath <log>
#
# 규칙을 만든 직후, 클라이언트를 끝내 어댑터가 없을 때, 다시 띄워 어댑터가 섰을 때 규칙의 InterfaceAlias 와
# 레지스트리 원문을 찍는다. 시험용 규칙(그룹 SangtachiProbe)은 끝에 지운다. 판정은 출력을 사람이 읽는다.
# 이 스크립트가 출력하는 문자열은 ASCII 로만 적는다.

param(
    [Parameter(Mandatory = $true)][string]$ClientExe,
    [Parameter(Mandatory = $true)][string]$LogPath
)

$ErrorActionPreference = 'Stop'
$LogPath = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($LogPath)
$log = $LogPath
Start-Transcript -Path $log -Force | Out-Null
$exe = $ClientExe
$errDir = Split-Path -Parent $LogPath
$alias = 'Sangtachi'
$group = 'SangtachiProbe'
$regPath = 'HKLM:\SYSTEM\CurrentControlSet\Services\SharedAccess\Parameters\FirewallPolicy\FirewallRules'

function Start-Client([string]$errPath) {
    $info = New-Object System.Diagnostics.ProcessStartInfo
    $info.FileName = $exe
    $info.Arguments = '--server 127.0.0.1:9 --name probe --stun 127.0.0.1:9 --stun 127.0.0.1:19'
    $info.UseShellExecute = $false
    $info.RedirectStandardInput = $true
    $info.RedirectStandardError = $true
    $info.CreateNoWindow = $true
    $writer = [System.IO.StreamWriter]::new($errPath, $false)
    $writer.AutoFlush = $true
    $p = [System.Diagnostics.Process]::Start($info)
    $sid = 'probe-' + [guid]::NewGuid().ToString('N')
    Register-ObjectEvent -InputObject $p -EventName ErrorDataReceived -SourceIdentifier $sid -MessageData $writer `
        -Action { if ($null -ne $EventArgs.Data) { $Event.MessageData.WriteLine($EventArgs.Data) } } | Out-Null
    $p.BeginErrorReadLine()
    return @{ proc = $p; writer = $writer; sid = $sid; err = $errPath }
}

function Wait-Adapter([bool]$present, [int]$ms = 20000) {
    $deadline = [Environment]::TickCount64 + $ms
    while ([Environment]::TickCount64 -lt $deadline) {
        $a = @(Get-NetAdapter -Name $alias -ErrorAction SilentlyContinue)
        if (($a.Count -gt 0) -eq $present) { return $a }
        Start-Sleep -Milliseconds 200
    }
    throw "adapter present=$present not reached"
}

function Stop-Client($c) {
    $c.proc.StandardInput.WriteLine('quit')
    $c.proc.StandardInput.Flush()
    if (-not $c.proc.WaitForExit(15000)) { $c.proc.Kill() }
    Unregister-Event -SourceIdentifier $c.sid -ErrorAction SilentlyContinue
    $c.writer.Dispose()
    Write-Host "client exit code: $($c.proc.ExitCode)"
}

function Show-Rules([string]$label) {
    Write-Host "===== $label"
    $rules = @(Get-NetFirewallRule -Group $group -ErrorAction SilentlyContinue)
    foreach ($r in $rules) {
        $f = $r | Get-NetFirewallInterfaceFilter
        Write-Host ("rule={0} enabled={1} InterfaceAlias=[{2}]" -f $r.DisplayName, $r.Enabled, ($f.InterfaceAlias -join ','))
        $raw = (Get-ItemProperty -Path $regPath -Name $r.Name -ErrorAction SilentlyContinue).($r.Name)
        Write-Host ("  registry: {0}" -f $raw)
    }
    $a = @(Get-NetAdapter -Name $alias -ErrorAction SilentlyContinue)
    if ($a.Count -gt 0) { Write-Host ("adapter: ifIndex={0} guid={1} status={2}" -f $a[0].ifIndex, $a[0].InterfaceGuid, $a[0].Status) }
    else { Write-Host 'adapter: none' }
}

# 이 실행이 만든 규칙만 지운다. 남아 있던 시험용 규칙은 건드리지 않고 멈춘다.
if (@(Get-NetFirewallRule -Group $group -ErrorAction SilentlyContinue).Count -gt 0) {
    Stop-Transcript | Out-Null
    throw "leftover probe rules in group $group. Inspect and remove them by hand first"
}
$created = @()
$c1 = $null; $c2 = $null
try {

    $c1 = Start-Client (Join-Path $errDir 'fw-probe-run1.err')
    $a1 = Wait-Adapter $true
    Write-Host ("run1 adapter ifIndex={0} guid={1}" -f $a1[0].ifIndex, $a1[0].InterfaceGuid)

    $created += (New-NetFirewallRule -DisplayName 'SangtachiProbe TCP 25565 In' -Group $group -Direction Inbound -Action Allow `
        -Profile Any -InterfaceAlias $alias -Protocol TCP -LocalPort 25565).Name
    $created += (New-NetFirewallRule -DisplayName 'SangtachiProbe ICMPv4 Echo In' -Group $group -Direction Inbound -Action Allow `
        -Profile Any -InterfaceAlias $alias -Protocol ICMPv4 -IcmpType 8).Name
    Show-Rules 'after create (adapter up, run 1)'

    Stop-Client $c1; $c1 = $null
    Wait-Adapter $false | Out-Null
    Show-Rules 'after run 1 quit (adapter gone)'

    $c2 = Start-Client (Join-Path $errDir 'fw-probe-run2.err')
    $a2 = Wait-Adapter $true
    Write-Host ("run2 adapter ifIndex={0} guid={1}" -f $a2[0].ifIndex, $a2[0].InterfaceGuid)
    Show-Rules 'after run 2 start (adapter recreated)'

    Stop-Client $c2; $c2 = $null
} finally {
    foreach ($c in @($c1, $c2)) { if ($null -ne $c -and -not $c.proc.HasExited) { $c.proc.Kill() } }
    foreach ($name in $created) { Remove-NetFirewallRule -Name $name -ErrorAction SilentlyContinue }
    Write-Host ("probe rules left: {0}" -f @(Get-NetFirewallRule -Group $group -ErrorAction SilentlyContinue).Count)
    Stop-Transcript | Out-Null
}
