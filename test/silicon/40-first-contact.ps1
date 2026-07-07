# Real-silicon bring-up phase 4: first DMA-era contact on a booted, recoverable
# system. Enables the device under Verifier, checks health, runs the READ-ONLY
# ttinfo rungs. Run ELEVATED after the 30-verifier-arm reboot.
#Requires -RunAsAdministrator
param([string]$Drop = (Join-Path $PSScriptRoot '..\..\out\Release'))
$ErrorActionPreference = 'Stop'
$since = Get-Date

if (-not (verifier /query | Select-String -SimpleMatch 'ttkmd.sys')) {
    Write-Warning 'Verifier is NOT armed for ttkmd.sys — run 30-verifier-arm.ps1 first (recommended).'
}

$card = Get-PnpDevice | Where-Object { $_.InstanceId -like 'PCI\VEN_1E52*' -and $_.Present }
if (-not $card) { throw 'No present PCI\VEN_1E52 device.' }
Write-Host "== Enabling $($card.InstanceId)..."
Enable-PnpDevice -InstanceId $card.InstanceId -Confirm:$false
Start-Sleep -Seconds 5

$card = Get-PnpDevice -InstanceId $card.InstanceId
$card | Format-List FriendlyName, Status, Problem
if ($card.Status -ne 'OK') { throw "CHECKPOINT FAIL: device Status=$($card.Status) Problem=$($card.Problem)" }

Write-Host '== Rung b: read-only ttinfo (info/mappings/telemetry — no TLB, no DMA, no reset)...'
& (Join-Path $Drop 'ttinfo.exe')
$ttinfoExit = $LASTEXITCODE
Write-Host "ttinfo exit code: $ttinfoExit (0=pass 1=fail 2=no device)"

Write-Host '== Post-step event scan (WHEA / bugcheck since start)...'
$whea = Get-WinEvent -FilterHashtable @{LogName='System'; ProviderName='Microsoft-Windows-WHEA-Logger'; StartTime=$since} -ErrorAction SilentlyContinue
$bug  = Get-WinEvent -FilterHashtable @{LogName='System'; Id=1001; StartTime=$since} -ErrorAction SilentlyContinue |
        Where-Object ProviderName -eq 'Microsoft-Windows-WER-SystemErrorReporting'
if ($whea) { Write-Warning "WHEA events:"; $whea | Format-List TimeCreated, Message }
if ($bug)  { Write-Warning "Bugcheck records:"; $bug | Format-List TimeCreated, Message }
if (-not $whea -and -not $bug -and $ttinfoExit -eq 0) {
    Write-Host '== CHECKPOINT PASS: rung a+b clean. Proceed per ladder: ttinfo --only tlb, then dma, then pin; resets last.'
}
