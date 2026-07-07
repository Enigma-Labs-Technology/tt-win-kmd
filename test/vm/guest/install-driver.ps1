# In-guest: trust the test cert, install the driver package, create the soft devnode.
# Usage (from C:\tt\drop containing ttkmd.sys/.inf/.cat, tt-test.cer, devgen.exe):
#   powershell -ExecutionPolicy Bypass -File install-driver.ps1
$ErrorActionPreference = 'Stop'
$drop = Split-Path -Parent $MyInvocation.MyCommand.Path

certutil -addstore -f Root "$drop\tt-test.cer" | Out-Null
certutil -addstore -f TrustedPublisher "$drop\tt-test.cer" | Out-Null

pnputil /add-driver "$drop\ttkmd.inf" /install
if ($LASTEXITCODE -ne 0) { throw "pnputil /add-driver failed: $LASTEXITCODE" }

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
