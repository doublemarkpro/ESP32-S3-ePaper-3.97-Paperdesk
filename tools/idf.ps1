param([ValidateSet('build','flash','backup','monitor','identify')][string]$Task='build', [switch]$SkipBackup)
$ErrorActionPreference='Stop'
$PaperdeskRoot=Split-Path -Parent $PSScriptRoot
$PaperdeskInstall=Get-Content 'C:/Espressif/tools/eim_idf.json' -Raw | ConvertFrom-Json
$PaperdeskIdf=$PaperdeskInstall.idfInstalled | Where-Object id -EQ $PaperdeskInstall.idfSelectedId | Select-Object -First 1
. $PaperdeskIdf.activationScript *> $null
if ($Task -eq 'build') {
    Push-Location (Join-Path $PaperdeskRoot 'firmware')
    try { & $PaperdeskIdf.python (Join-Path $PaperdeskIdf.path 'tools/idf.py') '-DIDF_TARGET=esp32s3' build; if ($LASTEXITCODE -ne 0) {throw 'Firmware build failed'} }
    finally {Pop-Location}
} else {
    $PaperdeskArgs = @((Join-Path $PSScriptRoot 'device.py'), $Task)
    if ($SkipBackup) { $PaperdeskArgs += '--skip-backup' }
    & $PaperdeskIdf.python @PaperdeskArgs
    if ($LASTEXITCODE -ne 0) {throw "Device task failed: $Task"}
}
