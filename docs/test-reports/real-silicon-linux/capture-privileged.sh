#!/bin/bash
# Privileged captures for the Blackhole p150a ground-truth reference set.
# Run as:  sudo bash reference/real-silicon-linux/capture-privileged.sh
# Read-only with two exceptions, both temporary and reverted:
#   - allocates 2x 2MB hugepages for the pin-pages test (restored after)
#   - enables tenstorrent pr_debug during one test run (disabled after)
set -u
BDF=0000:c1:00.0
OUT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TEST=~/tt-kmd/test/ttkmd_test

[ "$EUID" -eq 0 ] || { echo "run with sudo"; exit 1; }

echo "== 1. Full lspci -vvv (capabilities, DevCtl MPS/MRRS, LnkSta, MSI/MSI-X caps) =="
lspci -vvvnn -s "$BDF" | tee "$OUT/lspci-vvv-root.txt"

echo "== 2. Full config space baseline =="
lspci -xxxx -s "$BDF" | tee "$OUT/lspci-config-baseline.txt"

echo "== 3. Hugepages: save current, allocate 2 =="
HP=/sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
HP_SAVED=$(cat "$HP")
echo "nr_hugepages before: $HP_SAVED"
echo 2 > "$HP"
echo "nr_hugepages now: $(cat "$HP")"

echo "== 4. Test suite as root with pr_debug tracing =="
if [ -f /sys/kernel/debug/dynamic_debug/control ]; then
    echo 'module tenstorrent +p' > /sys/kernel/debug/dynamic_debug/control
    dmesg --clear
fi
"$TEST" 2>&1 | tee "$OUT/test-suite-output-root.txt"
rc=${PIPESTATUS[0]}
echo "exit: $rc" | tee -a "$OUT/test-suite-output-root.txt"
if [ -f /sys/kernel/debug/dynamic_debug/control ]; then
    dmesg | tee "$OUT/ftrace-or-dmesg.txt"
    echo 'module tenstorrent -p' > /sys/kernel/debug/dynamic_debug/control
fi

echo "== 5. Restore hugepages =="
echo "$HP_SAVED" > "$HP"
echo "nr_hugepages restored: $(cat "$HP")"

echo "== DONE. New artifacts: lspci-vvv-root.txt lspci-config-baseline.txt test-suite-output-root.txt ftrace-or-dmesg.txt =="
echo "NOTE: the reset step (step 5 of the plan) is intentionally NOT here."
echo "It stays skipped until tt-smi AND tt-flash + a known-good fw image exist."
