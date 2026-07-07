#!/usr/bin/env bash
# Builds a local qemu-system-x86_64 with the ttsim-bh device compiled in.
# QEMU has no stable out-of-tree device plugin ABI, so we build from the exact
# source tarball pinned here (matches the distro major used elsewhere).
# Prereqs: sudo apt-get install -y ninja-build meson libglib2.0-dev \
#          libpixman-1-dev libslirp-dev flex bison python3-venv
set -euo pipefail
cd "$(dirname "$0")"

QEMU_VER=8.2.2
TARBALL=qemu-${QEMU_VER}.tar.xz
URL=https://download.qemu.org/${TARBALL}

if [ ! -d qemu ]; then
    [ -f "$TARBALL" ] || curl -fLO "$URL"
    tar xf "$TARBALL"
    mv "qemu-${QEMU_VER}" qemu
fi

# Drop the device into hw/misc and register it with meson (idempotent).
cp ttsim-dev.c qemu/hw/misc/ttsim-dev.c
if ! grep -q ttsim-dev qemu/hw/misc/meson.build; then
    echo "system_ss.add(when: 'CONFIG_PCI', if_true: files('ttsim-dev.c'))" \
        >> qemu/hw/misc/meson.build
fi

cd qemu
if [ ! -f build/build.ninja ]; then
    ./configure --target-list=x86_64-softmmu --enable-kvm --enable-slirp \
                --disable-docs --disable-user
fi
ninja -C build qemu-system-x86_64
echo
echo "Built: $(pwd)/build/qemu-system-x86_64"
./build/qemu-system-x86_64 -device ttsim-bh,help | head -8
