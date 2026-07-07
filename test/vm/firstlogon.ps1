# First-logon provisioning for the ttkmd test VM (invoked from autounattend.xml).
# Lab VM only: enables test signing and remote automation.
$ErrorActionPreference = 'Continue'
Start-Transcript -Path C:\tt\provision.log -Append

New-Item -ItemType Directory -Force -Path C:\tt | Out-Null

# Kernel test signing (M0+: all driver artifacts are test-signed)
bcdedit /set testsigning on
bcdedit /set nointegritychecks off

# OpenSSH server for host-driven automation
Add-WindowsCapability -Online -Name OpenSSH.Server~~~~0.0.1.0
Set-Service sshd -StartupType Automatic
Start-Service sshd
New-NetFirewallRule -Name sshd -DisplayName 'OpenSSH Server' -Enabled True `
    -Direction Inbound -Protocol TCP -Action Allow -LocalPort 22 | Out-Null

# Keep the lab VM awake and quiet
powercfg /change standby-timeout-ac 0
powercfg /change monitor-timeout-ac 0
powercfg /change hibernate-timeout-ac 0

# Don't let Windows Update replace our test driver
New-Item -Path 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\WindowsUpdate' -Force | Out-Null
Set-ItemProperty -Path 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\WindowsUpdate' `
    -Name ExcludeWUDriversInQualityUpdate -Value 1 -Type DWord

# PowerShell over SSH defaults to cmd; make powershell available and default
New-ItemProperty -Path 'HKLM:\SOFTWARE\OpenSSH' -Name DefaultShell `
    -Value 'C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe' `
    -PropertyType String -Force | Out-Null

Set-Content -Path C:\tt\provisioned.txt -Value ("provisioned " + (Get-Date -Format o))
Stop-Transcript

# Reboot so testsigning takes effect
shutdown /r /t 5 /c "ttkmd provisioning reboot"
