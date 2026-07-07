# In-guest: enable Driver Verifier for ttkmd.sys per the porting spec
# (standard + special pool + DMA verification) and reboot.
# Flags 0x9BB = special pool | force IRQL | pool tracking | I/O verify |
#               deadlock detect | DMA verify | security checks | misc checks.
$ErrorActionPreference = 'Stop'
verifier /flags 0x9BB /driver ttkmd.sys
if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 2) { throw "verifier returned $LASTEXITCODE" }
Write-Host 'Verifier configured; rebooting in 5s'
shutdown /r /t 5 /c 'enable Driver Verifier'
