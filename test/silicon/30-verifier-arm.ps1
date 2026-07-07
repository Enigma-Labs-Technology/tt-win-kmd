# Real-silicon bring-up phase 3: arm Driver Verifier RECOVERY-SAFE.
# The device is disabled BEFORE the verifier reboot so a first-contact bugcheck
# cannot loop (ttkmd is PnP-auto-started; 0x9BB flags are boot-persistent).
# Run ELEVATED. Reboot manually after it completes, then run 40-first-contact.
#Requires -RunAsAdministrator
$ErrorActionPreference = 'Stop'

$card = Get-PnpDevice | Where-Object { $_.InstanceId -like 'PCI\VEN_1E52*' -and $_.Present }
if (-not $card) { throw 'No present PCI\VEN_1E52 device.' }

Write-Host "== Disabling $($card.InstanceId) before arming Verifier..."
Disable-PnpDevice -InstanceId $card.InstanceId -Confirm:$false

# Milestone flag set (verifier-on.ps1): special pool | force IRQL | pool track |
# I/O | deadlock | DMA | security | misc.
verifier /flags 0x9BB /driver ttkmd.sys
verifier /query

Write-Host @'
== Verifier armed; device disabled. NOW:
   1. Reboot (shutdown /r /t 0).
   2. After a clean boot, run 40-first-contact.ps1 (elevated).
== RECOVERY if the machine bugcheck-loops anyway:
   Safe Mode (ttkmd will not bind there): verifier /reset ; reboot.
   Offline (WinRE): load SYSTEM hive, clear VerifyDrivers/VerifyDriverLevel
   under ControlSet001\Control\Session Manager\Memory Management.
'@
