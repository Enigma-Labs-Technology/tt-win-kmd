#!/usr/bin/env bash
# Creates the Windows 11 test VM from scratch (unattended install).
# Prereqs (one-time): sudo apt-get install -y qemu-system-x86 qemu-utils genisoimage ovmf
# Requires: /dev/kvm access (user in kvm group or run via sudo).
set -euo pipefail
cd "$(dirname "$0")"

WIN_ISO=${WIN_ISO:-/mnt/c/VMs/iso/Win11_24H2_English_x64.iso}
DISK=win11.qcow2
DISK_SIZE=60G
MEM=8192
SMP=8
SSH_PORT=2222
OVMF_CODE=$(ls /usr/share/OVMF/OVMF_CODE_4M.fd /usr/share/OVMF/OVMF_CODE.fd 2>/dev/null | head -1)
OVMF_VARS_SRC=$(ls /usr/share/OVMF/OVMF_VARS_4M.fd /usr/share/OVMF/OVMF_VARS.fd 2>/dev/null | head -1)

[ -f "$WIN_ISO" ] || { echo "Windows ISO not found: $WIN_ISO" >&2; exit 1; }
[ -n "$OVMF_CODE" ] || { echo "OVMF not installed (sudo apt-get install ovmf)" >&2; exit 1; }
[ -e "$DISK" ] && { echo "$DISK already exists; delete it to reinstall" >&2; exit 1; }

qemu-img create -f qcow2 "$DISK" "$DISK_SIZE"
cp "$OVMF_VARS_SRC" OVMF_VARS.fd

# Unattend CD: autounattend.xml is auto-discovered at any drive root by setup.
genisoimage -quiet -J -R -o unattend.iso autounattend.xml firstlogon.ps1

# First boot: Windows UEFI CD boot shows "Press any key to boot from CD or DVD".
# We poke Enter through the QMP monitor for the first 40 s to get past it.
qemu-system-x86_64 \
    -enable-kvm -machine q35 -cpu host -smp "$SMP" -m "$MEM" \
    -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
    -drive if=pflash,format=raw,file=OVMF_VARS.fd \
    -drive file="$DISK",if=none,id=d0,format=qcow2,cache=none,discard=unmap \
    -device nvme,drive=d0,serial=ttkmd-disk \
    -drive file="$WIN_ISO",media=cdrom,if=none,id=cd0 -device ide-cd,drive=cd0 \
    -drive file=unattend.iso,media=cdrom,if=none,id=cd1 -device ide-cd,drive=cd1 \
    -netdev user,id=n0,hostfwd=tcp:127.0.0.1:${SSH_PORT}-:22 -device e1000e,netdev=n0 \
    -display none -vnc 127.0.0.1:7 \
    -qmp unix:qmp.sock,server,nowait \
    -name ttkmd-test-vm &
QEMU_PID=$!

sleep 3
for _ in $(seq 1 40); do
    printf '{"execute":"qmp_capabilities"}\n{"execute":"send-key","arguments":{"keys":[{"type":"qcode","data":"ret"}]}}\n' \
        | timeout 2 socat - UNIX-CONNECT:qmp.sock >/dev/null 2>&1 || true
    sleep 1
done

echo "Install running (QEMU pid $QEMU_PID). Watch via VNC 127.0.0.1:5907."
echo "Poll:  ssh -p $SSH_PORT ttdev@localhost 'type C:\\tt\\provisioned.txt'"
echo "(Install + provisioning + reboot typically takes 20-60 minutes.)"
wait "$QEMU_PID"
