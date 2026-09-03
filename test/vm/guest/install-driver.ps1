# In-guest / lab-host: trust the test cert and install the driver package.
# Usage (from a drop directory containing ttkmd.sys, the INF, ttkmd.cat and
# tt-test.cer; devgen.exe is needed only for the soft device):
#   powershell -ExecutionPolicy Bypass -File install-driver.ps1
#
# A Debug drop carries ttkmd-test.inf (ttsim/QEMU model + ROOT\TTKMD_SOFT);
# a Release drop carries ttkmd.inf (p150a only). The soft load-test node is
# created only when the test package is present.
$ErrorActionPreference = 'Stop'
$drop = Split-Path -Parent $MyInvocation.MyCommand.Path

certutil -addstore -f Root "$drop\tt-test.cer" | Out-Null
certutil -addstore -f TrustedPublisher "$drop\tt-test.cer" | Out-Null

$testInf = Join-Path $drop 'ttkmd-test.inf'
$inf = if (Test-Path $testInf) { $testInf } else { Join-Path $drop 'ttkmd.inf' }
Write-Host "Installing $inf"
pnputil /add-driver "$inf" /install
if ($LASTEXITCODE -ne 0) { throw "pnputil /add-driver failed: $LASTEXITCODE" }

if ($inf -ne $testInf) {
    Write-Host 'Release package installed (no soft device in this package).'
    exit 0
}

# Soft devnode for hardware-free load testing (M0 acceptance).
# devgen.exe ships in the WDK (Tools\10.0.26100.0\x64) and is copied into the drop.
# Note: devgen instance IDs are ROOT\DEVGEN\{guid}; match on the HARDWARE id.
function Get-SoftDev {
    Get-PnpDevice -PresentOnly | Where-Object { $_.HardwareID -contains 'ROOT\TTKMD_SOFT' } | Select-Object -First 1
}

if (-not (Get-SoftDev)) {
    & "$drop\devgen.exe" /add /bus ROOT /hardwareid "ROOT\TTKMD_SOFT"
    if ($LASTEXITCODE -ne 0) { throw "devgen failed: $LASTEXITCODE" }
}

Start-Sleep -Seconds 3
$dev = Get-SoftDev
if (-not $dev) { throw 'soft device not found after devgen' }
if ($dev.Status -ne 'OK') {
    Get-PnpDeviceProperty -InstanceId $dev.InstanceId -KeyName DEVPKEY_Device_ProblemCode |
        Format-List | Out-String | Write-Host
    throw "soft device status: $($dev.Status)"
}
Write-Host "PASS: $($dev.InstanceId) started with driver $((Get-PnpDeviceProperty -InstanceId $dev.InstanceId -KeyName DEVPKEY_Device_Service).Data)"
