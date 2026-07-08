# Real-silicon bring-up phase 1: return the p150a from the Hyper-V DDA stack
# (PCIP\... "Dismounted") to the host PCI tree. Run ELEVATED.
#Requires -RunAsAdministrator
$ErrorActionPreference = 'Stop'

$location = 'PCIROOT(C0)#PCI(0101)#PCI(0000)'   # bus 0xC1(193) dev 0 fn 0 on this host

Write-Host '== Checking that no VM still claims the device...'
$claims = @()
foreach ($vm in Get-VM) {
    $d = Get-VMAssignableDevice -VMName $vm.Name -ErrorAction SilentlyContinue
    if ($d) { $claims += [pscustomobject]@{ VM = $vm.Name; Device = $d.LocationPath } }
}
if ($claims) {
    $claims | Format-Table
    throw "A VM still holds the device. Power it Off, then: Remove-VMAssignableDevice -VMName '<vm>' -LocationPath '$location'"
}

$hostDev = Get-VMHostAssignableDevice
if ($hostDev) {
    Write-Host "Dismounted host-assignable devices:"; $hostDev | Format-List InstanceID, LocationPath
    Write-Host "== Remounting $location to the host..."
    Mount-VMHostAssignableDevice -LocationPath $location
} else {
    Write-Host 'No dismounted devices found - card may already be mounted.'
}

pnputil /scan-devices | Out-Null
Start-Sleep -Seconds 2

$card = Get-PnpDevice | Where-Object { $_.InstanceId -like 'PCI\VEN_1E52*' -and $_.Present }
$pcip = Get-PnpDevice | Where-Object { $_.InstanceId -like 'PCIP\VEN_1E52*' -and $_.Present }
if ($card) {
    Write-Host '== CHECKPOINT PASS: card is on the host PCI tree:'
    $card | Format-List FriendlyName, InstanceId, Status, Problem
    if ($card.Problem -eq 28) { Write-Host '(Code 28 = no driver yet - expected before install)' }
} else {
    throw 'CHECKPOINT FAIL: no present PCI\VEN_1E52 device after remount.'
}
if ($pcip) { Write-Warning 'A PCIP\VEN_1E52 node is still present - dismount state may linger; verify before install.' }
