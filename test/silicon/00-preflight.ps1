# Real-silicon bring-up phase 0: pre-state capture + rollback point.
# Run ELEVATED. Safe to re-run. See test\silicon\README.md for the ladder.
#Requires -RunAsAdministrator
$ErrorActionPreference = 'Stop'

$ts  = Get-Date -Format 'yyyyMMdd-HHmmss'
$log = "C:\tt-firstrun\$ts"
New-Item -ItemType Directory -Force $log | Out-Null
Write-Host "== Pre-state capture -> $log"

pnputil /enum-drivers | Out-File "$log\enum-drivers.txt"
pnputil /enum-devices /connected | Out-File "$log\enum-devices.txt"
Get-PnpDevice | Where-Object { $_.InstanceId -match '1E52' } |
    Format-List * | Out-File "$log\tt-device.txt"
bcdedit /enum '{current}' | Out-File "$log\bcd.txt"

# --- BitLocker: changing Secure Boot with protection ON triggers recovery ---
$blv = Get-BitLockerVolume -MountPoint 'C:' -ErrorAction SilentlyContinue
if ($blv -and $blv.ProtectionStatus -eq 'On') {
    Write-Warning 'BitLocker protection is ON for C:. Suspend it (Suspend-BitLocker -MountPoint C: -RebootCount 2) or have the recovery key in hand BEFORE toggling Secure Boot in firmware.'
} else {
    Write-Host 'BitLocker: protection off or not present — Secure Boot change is safe.'
}

# --- Secure Boot / test signing gates ---
$sb = Confirm-SecureBootUEFI
"SecureBoot=$sb" | Out-File "$log\secureboot.txt"
if ($sb) {
    Write-Warning 'Secure Boot is ON. The test-signed ttkmd.cat will NOT load. Reboot into UEFI setup, disable Secure Boot, then re-run this script.'
}
$tsig = (bcdedit /enum '{current}' | Select-String 'testsigning\s+Yes') -ne $null
if (-not $tsig) {
    if ($sb) {
        Write-Host 'testsigning is off (and would be ignored while Secure Boot is on).'
    } else {
        Write-Host 'Enabling testsigning (takes effect after reboot)...'
        bcdedit /set '{current}' testsigning on | Out-Null
        Write-Warning 'testsigning enabled — REBOOT required before driver install.'
    }
} else {
    Write-Host 'testsigning: already on.'
}

# --- Restore point (bypass the 24h throttle) ---
Enable-ComputerRestore -Drive 'C:\'
New-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\SystemRestore' `
    -Name SystemRestorePointCreationFrequency -Value 0 -PropertyType DWord -Force | Out-Null
Checkpoint-Computer -Description 'pre-ttkmd-silicon' -RestorePointType MODIFY_SETTINGS
Get-ComputerRestorePoint | Select-Object -First 3 | Format-Table | Out-File "$log\restorepoints.txt"
Write-Host '== Restore point created.'

Write-Host "== Phase 0 complete. Gates: SecureBoot=$sb (need False), testsigning=$tsig (need True after reboot)."
