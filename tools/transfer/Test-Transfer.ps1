<#
.SYNOPSIS
  가상 IP 사이의 파일 전송 확인. `roadmap.md` Phase 7 검증의 NFR-2 판정 항목이다.

.DESCRIPTION
  한쪽이 `-Listen` 으로 TCP 25565 에서 기다리고, 다른 쪽이 `-Connect <가상 IP>` 로 파일을 보낸다. 받는 쪽은 받은
  바이트의 SHA-256 을 보낸 쪽에 돌려주고, 보낸 쪽이 자기 SHA-256 과 비교해 판정한다. 눈으로 해시를 비교하지 않는다.

  25565 를 쓰는 이유는 클라이언트가 만드는 방화벽 규칙이 그 포트만 열기 때문이다(`windows-prereq.md` 2절).
  Minecraft 서버가 같은 기기에서 25565 를 쓰고 있으면 먼저 끈다.

  와이어는 이렇다. 보내는 쪽이 길이 8바이트(빅 엔디언)와 내용을 보내고, 받는 쪽이 소문자 16진 SHA-256 64바이트를
  돌려준다.

  보내는 쪽의 마지막 줄은 `VERDICT: pass` 또는 `VERDICT: fail` 이고 종료 코드는 0 또는 1 이다. 연결이나 전송이
  실패하면 `VERDICT: unknown` 과 2 다. 받는 쪽은 받은 길이와 해시를 찍고 0 으로 끝난다.

  Windows PowerShell 5.1 에서 돈다. 상대 기기에 PowerShell 7 이 없어도 된다. 출력은 ASCII 로만 적는다.

.EXAMPLE
  # 받는 쪽 (호스트, 10.100.0.1)
  powershell -NoProfile -ExecutionPolicy Bypass -File Test-Transfer.ps1 -Listen

.EXAMPLE
  # 보내는 쪽 (플레이어). 64 MiB 무작위 파일을 만들어 보낸다
  powershell -NoProfile -ExecutionPolicy Bypass -File Test-Transfer.ps1 -Connect 10.100.0.1 -GenerateMiB 64
#>
[CmdletBinding(DefaultParameterSetName = 'Listen')]
param(
    [Parameter(ParameterSetName = 'Listen', Mandatory = $true)][switch]$Listen,
    [Parameter(ParameterSetName = 'Listen')][string]$BindAddress = '0.0.0.0',
    [Parameter(ParameterSetName = 'Connect', Mandatory = $true)][string]$Connect,
    [Parameter(ParameterSetName = 'Connect')][string]$Path,
    [Parameter(ParameterSetName = 'Connect')][ValidateRange(1, 4096)][int]$GenerateMiB = 0,
    [int]$Port = 25565,
    [ValidateRange(1, 3600)][int]$TimeoutSeconds = 300
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-HexHash([System.Security.Cryptography.HashAlgorithm]$Hasher) {
    return (($Hasher.Hash | ForEach-Object { $_.ToString('x2') }) -join '')
}

function Read-Exactly([System.IO.Stream]$Stream, [byte[]]$Buffer, [int]$Count) {
    $done = 0
    while ($done -lt $Count) {
        $n = $Stream.Read($Buffer, $done, $Count - $done)
        if ($n -le 0) { throw "connection closed after $done of $Count bytes" }
        $done += $n
    }
}

if ($Listen) {
    $listener = New-Object System.Net.Sockets.TcpListener ([System.Net.IPAddress]::Parse($BindAddress)), $Port
    $listener.Start()
    Write-Output ("listening {0}:{1}" -f $BindAddress, $Port)
    try {
        $client = $listener.AcceptTcpClient()
        $client.ReceiveTimeout = $TimeoutSeconds * 1000
        $client.SendTimeout = $TimeoutSeconds * 1000
        $stream = $client.GetStream()
        try {
            $head = New-Object byte[] 8
            Read-Exactly $stream $head 8
            [array]::Reverse($head)
            $length = [BitConverter]::ToInt64($head, 0)
            $sha = [System.Security.Cryptography.SHA256]::Create()
            $buffer = New-Object byte[] 65536
            $left = $length
            while ($left -gt 0) {
                $want = [int][Math]::Min([int64]$buffer.Length, $left)
                $n = $stream.Read($buffer, 0, $want)
                if ($n -le 0) { throw "connection closed with $left bytes left" }
                [void]$sha.TransformBlock($buffer, 0, $n, $null, 0)
                $left -= $n
            }
            [void]$sha.TransformFinalBlock((New-Object byte[] 0), 0, 0)
            $hex = Get-HexHash $sha
            $reply = [System.Text.Encoding]::ASCII.GetBytes($hex)
            $stream.Write($reply, 0, $reply.Length)
            $stream.Flush()
            Write-Output ("received from={0} bytes={1} sha256={2}" -f $client.Client.RemoteEndPoint, $length, $hex)
        } finally {
            $stream.Dispose()
            $client.Close()
        }
    } finally {
        $listener.Stop()
    }
    exit 0
}

# 보내는 쪽
$generated = $false
try {
    if ($GenerateMiB -gt 0) {
        $Path = Join-Path ([System.IO.Path]::GetTempPath()) ("sangtachi_transfer_" + [guid]::NewGuid().ToString('N') + '.bin')
        $rng = [System.Security.Cryptography.RandomNumberGenerator]::Create()
        $chunk = New-Object byte[] (1024 * 1024)
        $out = [System.IO.File]::Create($Path)
        try {
            for ($i = 0; $i -lt $GenerateMiB; $i++) { $rng.GetBytes($chunk); $out.Write($chunk, 0, $chunk.Length) }
        } finally { $out.Dispose() }
        $generated = $true
    }
    if (-not $Path) { throw 'give -Path or -GenerateMiB' }
    $Path = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Path)

    $client = New-Object System.Net.Sockets.TcpClient
    $client.ReceiveTimeout = $TimeoutSeconds * 1000
    $client.SendTimeout = $TimeoutSeconds * 1000
    $client.Connect($Connect, $Port)
    $stream = $client.GetStream()
    $file = [System.IO.File]::OpenRead($Path)
    try {
        $length = $file.Length
        $head = [BitConverter]::GetBytes([int64]$length)
        [array]::Reverse($head)
        $stream.Write($head, 0, 8)
        $sha = [System.Security.Cryptography.SHA256]::Create()
        $buffer = New-Object byte[] 65536
        $watch = [System.Diagnostics.Stopwatch]::StartNew()
        while (($n = $file.Read($buffer, 0, $buffer.Length)) -gt 0) {
            [void]$sha.TransformBlock($buffer, 0, $n, $null, 0)
            $stream.Write($buffer, 0, $n)
        }
        [void]$sha.TransformFinalBlock((New-Object byte[] 0), 0, 0)
        $stream.Flush()
        $mine = Get-HexHash $sha
        $reply = New-Object byte[] 64
        Read-Exactly $stream $reply 64
        $watch.Stop()
        $theirs = [System.Text.Encoding]::ASCII.GetString($reply)
        Write-Output ("sent to={0}:{1} bytes={2} seconds={3:N1}" -f $Connect, $Port, $length, $watch.Elapsed.TotalSeconds)
        Write-Output ("sha256 sent={0}" -f $mine)
        Write-Output ("sha256 received={0}" -f $theirs)
    } finally {
        $file.Dispose()
        $stream.Dispose()
        $client.Close()
    }
} catch {
    Write-Output ("error {0}" -f ($_.Exception.Message -replace '[^\x20-\x7E]', '?'))
    Write-Output 'VERDICT: unknown'
    exit 2
} finally {
    if ($generated -and $Path) { Remove-Item -LiteralPath $Path -ErrorAction SilentlyContinue }
}

if ($mine -ceq $theirs) {
    Write-Output 'VERDICT: pass'
    exit 0
}
Write-Output 'VERDICT: fail'
exit 1
