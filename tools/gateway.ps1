param(
    [ValidateSet('start','stop','status')][string]$Task='start',
    [ValidateRange(1024,65535)][int]$Port=8787
)
$ErrorActionPreference='Stop'
$PaperdeskRoot=Split-Path -Parent $PSScriptRoot
$PaperdeskLocal=Join-Path $PaperdeskRoot '.local'
$PaperdeskScript=Join-Path $PaperdeskRoot 'bridge/server.py'
$PaperdeskPidFile=Join-Path $PaperdeskLocal 'gateway-process.json'

function Get-PaperdeskProcess {
    if (-not (Test-Path -LiteralPath $PaperdeskPidFile)) { return $null }
    try {
        $record=Get-Content -LiteralPath $PaperdeskPidFile -Raw | ConvertFrom-Json
        $candidate=Get-Process -Id ([int]$record.processId) -ErrorAction Stop
        $info=Get-CimInstance Win32_Process -Filter "ProcessId = $($candidate.Id)"
        $sameStart=$candidate.StartTime.ToUniversalTime().Ticks.ToString() -eq [string]$record.startTicks
        $sameScript=$info.CommandLine -and $info.CommandLine.IndexOf($PaperdeskScript,[StringComparison]::OrdinalIgnoreCase) -ge 0
        if ($sameStart -and $sameScript) { return $candidate }
    } catch { return $null }
    return $null
}

$PaperdeskRunning=Get-PaperdeskProcess
if ($Task -eq 'status') {
    if ($PaperdeskRunning) {
        $PaperdeskRecord=Get-Content -LiteralPath $PaperdeskPidFile -Raw | ConvertFrom-Json
        Write-Output "Paperdesk gateway running: PID $($PaperdeskRunning.Id), http://127.0.0.1:$($PaperdeskRecord.port)"
    } else { Write-Output 'No running gateway managed by this launcher.' }
    exit 0
}
if ($Task -eq 'stop') {
    if ($PaperdeskRunning) {
        Stop-Process -InputObject $PaperdeskRunning
        Write-Output 'Paperdesk gateway stopped.'
    } else { Write-Output 'No matching gateway process to stop.' }
    exit 0
}
if ($PaperdeskRunning) {
    Write-Output "Paperdesk gateway already running (PID $($PaperdeskRunning.Id))."
    exit 0
}
if (Get-NetTCPConnection -LocalPort $Port -State Listen -ErrorAction SilentlyContinue) {
    throw "Port $Port is already in use. Check the existing service or choose another -Port."
}
$PaperdeskPython=(Get-Command python.exe -ErrorAction Stop).Source
New-Item -ItemType Directory -Path $PaperdeskLocal -Force | Out-Null
$PaperdeskArguments=@(('"'+$PaperdeskScript+'"'),'--host','0.0.0.0','--port',$Port.ToString())
$PaperdeskProcess=Start-Process -FilePath $PaperdeskPython -ArgumentList $PaperdeskArguments -WorkingDirectory $PaperdeskRoot -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $PaperdeskLocal 'gateway.stdout.log') -RedirectStandardError (Join-Path $PaperdeskLocal 'gateway.stderr.log')
Start-Sleep -Milliseconds 700
$PaperdeskProcess.Refresh()
if ($PaperdeskProcess.HasExited) { throw 'Gateway exited during startup. See .local/gateway.stderr.log.' }
@{processId=$PaperdeskProcess.Id;startTicks=$PaperdeskProcess.StartTime.ToUniversalTime().Ticks.ToString();port=$Port} | ConvertTo-Json | Set-Content -LiteralPath $PaperdeskPidFile -Encoding utf8
Write-Output "Paperdesk gateway started: http://127.0.0.1:$Port"
Write-Output "Config and device token: $(Join-Path $PaperdeskLocal 'gateway.json')"
Write-Output "On the device, use this computer's LAN IPv4 address with port $Port."
