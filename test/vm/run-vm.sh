#!/usr/bin/env bash
# Steady-state boot of the installed test VM (no install media).
# Snapshot workflow:
#   qemu-img snapshot -c clean win11.qcow2     # take snapshot "clean"
#   qemu-img snapshot -a clean win11.qcow2     # revert to it (VM must be off)
set -euo pipefail
cd "$(dirname "$0")"

MEM=${MEM:-8192}
SMP=${SMP:-8}
SSH_PORT=${SSH_PORT:-2222}
QEMU_BIN=${QEMU_BIN:-qemu-system-x86_64}   # point at test/qemu-ttsim/qemu/build/... for ttsim
OVMF_CODE=$({ ls /usr/share/OVMF/OVMF_CODE_4M.fd /usr/share/OVMF/OVMF_CODE.fd 2>/dev/null || true; } | head -1)
EXTRA_DEVICES=${EXTRA_DEVICES:-}   # e.g. "-device ttsim-bh,ttsim-lib=/path/libttsim.so" (M1+)

[ -f win11.qcow2 ] || { echo "run make-vm.sh first" >&2; exit 1; }

exec "$QEMU_BIN" \
    -enable-kvm -machine q35 -cpu host -smp "$SMP" -m "$MEM" \
    -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
    -drive if=pflash,format=raw,file=OVMF_VARS.fd \
    -drive file=win11.qcow2,if=none,id=d0,format=qcow2,cache=none,discard=unmap \
    -device nvme,drive=d0,serial=ttkmd-disk \
    -netdev user,id=n0,hostfwd=tcp:127.0.0.1:${SSH_PORT}-:22 -device e1000e,netdev=n0 \
    -display none -vnc 127.0.0.1:7 \
    -qmp unix:qmp.sock,server,nowait \
    $EXTRA_DEVICES \
    -name ttkmd-test-vm
