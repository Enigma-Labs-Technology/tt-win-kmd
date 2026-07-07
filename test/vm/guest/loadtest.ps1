# In-guest M0 load test: N disable/enable cycles + M uninstall/reinstall cycles
# on the ROOT\TTKMD_SOFT devnode, then scan for bugcheck/Verifier/WHEA events.
# Emits a JSON result on stdout for the host harness.
param([int]$ToggleCycles = 20, [int]$ReinstallCycles = 5)
$ErrorActionPreference = 'Stop'
$drop = Split-Path -Parent $MyInvocation.MyCommand.Path
$since = Get-Date

function Get-SoftDev {
    Get-PnpDevice | Where-Object { $_.HardwareID -contains 'ROOT\TTKMD_SOFT' } | Select-Object -First 1
}

$dev = Get-SoftDev
if (-not $dev) { throw 'soft device missing; run install-driver.ps1 first' }

for ($i = 1; $i -le $ToggleCycles; $i++) {
    pnputil /disable-device $dev.InstanceId | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "disable cycle ${i}: $LASTEXITCODE" }
    pnputil /enable-device $dev.InstanceId | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "enable cycle ${i}: $LASTEXITCODE" }
}

for ($i = 1; $i -le $ReinstallCycles; $i++) {
    pnputil /remove-device $dev.InstanceId | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "remove cycle ${i}: $LASTEXITCODE" }
    & "$drop\devgen.exe" /add /bus ROOT /hardwareid "ROOT\TTKMD_SOFT" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "devgen cycle ${i}: $LASTEXITCODE" }
    Start-Sleep -Seconds 2
    $dev = Get-SoftDev
    if (-not $dev -or $dev.Status -ne 'OK') { throw "reinstall cycle ${i}: device not OK" }
}

# Post-run health scan
$bad = @()
$bad += Get-WinEvent -FilterHashtable @{LogName='System'; Id=1001; StartTime=$since} -ErrorAction SilentlyContinue |
        Where-Object { $_.ProviderName -eq 'Microsoft-Windows-WER-SystemErrorReporting' }
$bad += Get-WinEvent -FilterHashtable @{LogName='System'; ProviderName='Microsoft-Windows-WHEA-Logger'; StartTime=$since} -ErrorAction SilentlyContinue

$result = [pscustomobject]@{
    toggleCycles    = $ToggleCycles
    reinstallCycles = $ReinstallCycles
    verifierActive  = [bool](verifier /query | Select-String -SimpleMatch 'ttkmd.sys')
    badEvents       = @($bad | ForEach-Object { $_.Message })
    pass            = ($bad.Count -eq 0)
}
$result | ConvertTo-Json -Depth 4
if (-not $result.pass) { exit 1 }
