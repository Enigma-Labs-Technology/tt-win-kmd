# Real-silicon bring-up phase 2: trust the test cert and install ttkmd on the
# REAL card. Run ELEVATED. Unlike out\Release\install-driver.ps1 (an M0
# soft-device artifact), this validates the physical PCI\VEN_1E52 device and
# creates no ROOT\TTKMD_SOFT node.
#Requires -RunAsAdministrator
param([string]$Drop = (Join-Path $PSScriptRoot '..\..\out\Release'))
$ErrorActionPreference = 'Stop'

if (Confirm-SecureBootUEFI) { throw 'Secure Boot is ON - the test-signed driver cannot load. Disable it in UEFI first (phase 0).' }
if (-not (bcdedit /enum '{current}' | Select-String 'testsigning\s+Yes')) { throw 'testsigning is OFF - run phase 0 and reboot first.' }

$inf = Join-Path $Drop 'ttkmd.inf'
$cer = Join-Path $Drop 'tt-test.cer'
if (-not (Test-Path $inf) -or -not (Test-Path $cer)) { throw "Driver drop incomplete at $Drop" }

Write-Host '== Trusting test certificate (Root + TrustedPublisher)...'
certutil -addstore -f Root $cer | Out-Null
certutil -addstore -f TrustedPublisher $cer | Out-Null

Write-Host '== Installing driver package...'
pnputil /add-driver $inf /install

Start-Sleep -Seconds 3
$card = Get-PnpDevice | Where-Object { $_.InstanceId -like 'PCI\VEN_1E52*' -and $_.Present }
if (-not $card) { throw 'CHECKPOINT FAIL: card not present after install.' }
$svc = (Get-PnpDeviceProperty -InstanceId $card.InstanceId -KeyName DEVPKEY_Device_Service).Data

$card | Format-List FriendlyName, InstanceId, Status, Problem
Write-Host "Service: $svc"
if ($card.Status -eq 'OK' -and $svc -eq 'ttkmd') {
    Write-Host '== CHECKPOINT PASS: ttkmd bound and started on the real card.'
} elseif ($card.Problem -eq 52) {
    throw 'CHECKPOINT FAIL: Code 52 (signature) - Secure Boot/testsigning/cert-store problem.'
} elseif ($card.Problem -eq 54 -or $card.Problem -eq 12) {
    throw "CHECKPOINT FAIL: Code $($card.Problem) - DMA-guard block (54) or resource shortfall/BAR4 not granted (12). Check msinfo32 'Kernel DMA Protection' and BIOS Above-4G decoding."
} else {
    throw "CHECKPOINT FAIL: Status=$($card.Status) Problem=$($card.Problem) Service=$svc"
}
