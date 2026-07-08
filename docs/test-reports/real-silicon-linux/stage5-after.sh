#!/bin/bash
# Stage-5 completion: full config-space AFTER-reset capture + delta.
# The reset itself already ran (tt-smi -r 0 at 09:35:57, successful).
# Run as:  sudo bash reference/real-silicon-linux/stage5-after.sh
# Strictly read-only.
set -u
BDF=0000:c1:00.0
OUT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
[ "$EUID" -eq 0 ] || { echo "run with sudo"; exit 1; }

echo "== Full config space AFTER reset =="
lspci -xxxx -s "$BDF" > "$OUT/lspci-config-after-reset.txt"
[ -s "$OUT/lspci-config-after-reset.txt" ] || { echo "ERROR: after-capture is empty, aborting"; exit 1; }
cat "$OUT/lspci-config-after-reset.txt"

echo "== Full lspci -vvvnn AFTER reset (DevCtl MPS/MRRS, LnkSta, AER status) =="
lspci -vvvnn -s "$BDF" | tee "$OUT/lspci-vvv-root-after-reset.txt"

echo "== Delta vs before-reset (config hex lines only) =="
TMP=$(mktemp -d)
grep -E '^[0-9a-f]+0: ' "$OUT/lspci-config-before-reset.txt" > "$TMP/before.txt"
grep -E '^[0-9a-f]+0: ' "$OUT/lspci-config-after-reset.txt"  > "$TMP/after.txt"
diff "$TMP/before.txt" "$TMP/after.txt" > "$OUT/config-reset-delta.txt"
rc=$?
case $rc in
  0) echo "(no differences — full 4 KiB config space byte-identical across reset)" \
       | tee "$OUT/config-reset-delta.txt" ;;
  1) echo "DIFFERENCES FOUND:"; cat "$OUT/config-reset-delta.txt" ;;
  *) echo "ERROR: diff failed (exit $rc)"; rm -rf "$TMP"; exit 1 ;;
esac
rm -rf "$TMP"
echo "== DONE: lspci-config-after-reset.txt, lspci-vvv-root-after-reset.txt, config-reset-delta.txt =="
