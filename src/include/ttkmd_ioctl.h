// Maps to: tt-kmd/ioctl.h (Windows ABI surface — M0 subset: GUIDs + CTL_CODE scheme)
//
// Shared between the driver and user mode. The full ioctl set lands milestone by
// milestone; every struct added here must carry static_asserts against
// docs/abi-ground-truth.txt (generated from the Linux header by tools/gen_abi_truth.sh).
//
// Kernel-mode consumers: include after ntddk.h/wdm.h.
// User-mode consumers: include after windows.h + winioctl.h.
// Exactly one translation unit per binary must include <initguid.h> beforehand to
// instantiate the GUIDs.

#ifndef TTKMD_IOCTL_H_INCLUDED
#define TTKMD_IOCTL_H_INCLUDED

#include <guiddef.h>

// Device interface for Tenstorrent accelerators (docs/design-decisions.md DD-2).
// Replaces /dev/tenstorrent/N discovery on Linux.
// {e2e020f2-998c-4b6f-b98e-b259372c7986}
DEFINE_GUID(GUID_DEVINTERFACE_TENSTORRENT,
    0xe2e020f2, 0x998c, 0x4b6f, 0xb9, 0x8e, 0xb2, 0x59, 0x37, 0x2c, 0x79, 0x86);

// CTL_CODE scheme (DD-4): custom device type echoing the Linux ioctl magic 0xFA
// (tt-kmd/ioctl.h:12); function code = 0x800 + Linux ioctl nr, METHOD_BUFFERED,
// FILE_ANY_ACCESS. See docs/ioctl-parity-matrix.md for the full table.
#define TT_DEVICE_TYPE 0x80FAu
#define TT_CTL(nr) CTL_CODE(TT_DEVICE_TYPE, 0x800u + (nr), METHOD_BUFFERED, FILE_ANY_ACCESS)

// IOCTL API version; parity with TENSTORRENT_DRIVER_VERSION (tt-kmd/ioctl.h:10).
#define TENSTORRENT_DRIVER_VERSION 2

#endif // TTKMD_IOCTL_H_INCLUDED
