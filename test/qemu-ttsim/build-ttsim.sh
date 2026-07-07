#!/usr/bin/env bash
# Builds libttsim (Blackhole) from a pristine copy of ../../../ttsim with the
# test-rig heartbeat patch applied (apply-heartbeat-patch.py). The user's ttsim
# checkout is never modified. Output: ttsim-work/src/_out/release_bh/libttsim.so
set -euo pipefail
cd "$(dirname "$0")"

SRC=../../../ttsim
WORK=ttsim-work

rsync -a --delete --exclude '.git' --exclude 'src/_out' "$SRC/" "$WORK/"
python3 apply-heartbeat-patch.py "$WORK/src/tile.cpp"
(cd "$WORK" && ./make.py :build)
ls -la "$WORK/src/_out/release_bh/libttsim.so"
echo "ttsim (heartbeat-patched): $(pwd)/$WORK/src/_out/release_bh/libttsim.so"
