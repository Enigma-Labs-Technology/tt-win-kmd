#!/usr/bin/env bash
# WSL wrapper for build.ps1: stages the repo to a Windows-side directory
# (MSBuild + driver tooling are unreliable on \\wsl.localhost UNC paths),
# builds there, and copies the artifacts back to out/.
#
#   ./build.sh [Debug|Release] [--test]
set -euo pipefail

CONFIG=Release
EXTRA=()
for arg in "$@"; do
    case "$arg" in
        Debug|Release) CONFIG="$arg" ;;
        --test) EXTRA+=('-Test') ;;
        *) echo "usage: $0 [Debug|Release] [--test]" >&2; exit 2 ;;
    esac
done

REPO_DIR="$(cd "$(dirname "$0")" && pwd)"
LOCALAPPDATA_WIN=$(cmd.exe /c "echo %LOCALAPPDATA%" 2>/dev/null | tr -d '\r')
STAGE_WSL="$(wslpath "$LOCALAPPDATA_WIN")/tt-win-kmd-build"
STAGE_WIN="$LOCALAPPDATA_WIN\\tt-win-kmd-build"

mkdir -p "$STAGE_WSL"
rsync -a --delete \
    --exclude '.git' \
    --exclude 'out' \
    --exclude 'test/vm/*.qcow2' \
    --exclude 'test/vm/*.img' \
    --exclude 'test/vm/*.iso' \
    --exclude 'test/vm/OVMF_VARS.fd' \
    --exclude 'test/vm/qmp.sock' \
    --exclude 'test/qemu-ttsim/qemu' \
    --exclude 'test/qemu-ttsim/qemu-*.tar.xz' \
    "$REPO_DIR/" "$STAGE_WSL/"

PS_ARGS=(-NoProfile -ExecutionPolicy Bypass -File "$STAGE_WIN\\build.ps1" -Configuration "$CONFIG")
if [ ${#EXTRA[@]} -gt 0 ]; then PS_ARGS+=("${EXTRA[@]}"); fi
powershell.exe "${PS_ARGS[@]}"

# Copy artifacts back
OUT="$REPO_DIR/out/$CONFIG"
mkdir -p "$OUT"
SRC="$STAGE_WSL/src/driver/x64/$CONFIG"
find "$SRC" \( -name 'ttkmd.sys' -o -name '*.inf' -o -name '*.cat' -o -name 'ttkmd.pdb' -o -name 'tt-test.cer' \) \
    -exec cp -v {} "$OUT/" \;
cp -v "$STAGE_WSL/src/tests/ttinfo/x64/$CONFIG/ttinfo.exe" "$OUT/" 2>/dev/null || true
cp -v "$STAGE_WSL/src/tests/ttconform/x64/$CONFIG/ttconform.exe" "$OUT/" 2>/dev/null || true
echo "Artifacts in $OUT"
