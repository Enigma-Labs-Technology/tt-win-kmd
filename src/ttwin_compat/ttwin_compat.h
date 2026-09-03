// SPDX-FileCopyrightText: 2026 tt-win-kmd contributors
// SPDX-License-Identifier: GPL-2.0-only
//
// ttwin_compat — POSIX-shaped compatibility layer over the Windows ttkmd driver.
//
// Purpose: let tt-umd's device-access layer (device/pcie/pci_device.cpp and
// device/tt_kmd_lib/tt_kmd_lib.c) be ported to Windows by mechanically swapping
// its OS calls for the tt_* equivalents below — the ioctl request constants and
// struct layouts are unchanged (they come from the shared src/include header,
// byte-identical to tt-kmd/ioctl.h). See docs/tt-umd-porting-notes.md.
//
//   open(path, flags)              -> tt_open(device_id, flags)
//   ioctl(fd, TENSTORRENT_IOCTL_*) -> tt_ioctl(h, TENSTORRENT_IOCTL_*, &arg)
//   mmap(NULL, len, ..., fd, off)  -> tt_mmap(h, len, prot, flags, off)
//   munmap(addr, len)              -> tt_munmap(h, addr, len)
//   close(fd)                      -> tt_close(h)
//   readdir("/dev/tenstorrent/")   -> tt_enumerate(ids, max)
//
// mmap views survive tt_close (Linux mmap-after-close semantics): the shim keeps
// the driver handle alive until the last view for it is tt_munmap'd.

#ifndef TTWIN_COMPAT_H
#define TTWIN_COMPAT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tt_device *tt_handle;   // opaque; NULL on failure

// Linux open() flag equivalents the shim honors.
#define TT_O_RDWR     0x0002
#define TT_O_CLOEXEC  0x80000   // no-op on Windows (handles aren't inherited by default)
#define TT_O_APPEND   0x0400    // power-aware-client opt-out of legacy power mode

// mmap prot/flags (accepted for source compatibility; the driver decides
// cacheability from the mmap-offset token, so PROT/flags are advisory).
#define TT_PROT_READ  0x1
#define TT_PROT_WRITE 0x2
#define TT_MAP_SHARED 0x01

// Enumerate present tenstorrent device ids (replaces readdir of
// /dev/tenstorrent/). Fills up to max_ids ascending ids; returns the count
// (which may exceed max_ids — call again with a larger buffer), or -1 on error.
int tt_enumerate(int *ids, int max_ids);

// Open device_id (the N in /dev/tenstorrent/N). Returns NULL on failure
// (tt_get_last_error()).
tt_handle tt_open(int device_id, int flags);

// ioctl: request is a TENSTORRENT_IOCTL_* value (_IO(0xFA, nr)); arg points at
// the matching struct (used as both input and output). Returns 0 on success,
// -1 on error (tt_get_last_error()).
int tt_ioctl(tt_handle h, unsigned long request, void *arg);

// mmap a region by its mmap-offset token (QUERY_MAPPINGS.mapping_base /
// ALLOCATE_DMA_BUF.mapping_offset / ALLOCATE_TLB.mmap_offset_uc|wc, plus any
// byte offset within it). Returns the mapped address or TT_MAP_FAILED.
void *tt_mmap(tt_handle h, size_t length, int prot, int flags, uint64_t offset);
#define TT_MAP_FAILED ((void *)-1)

// Unmap a region returned by tt_mmap. Returns 0 or -1.
int tt_munmap(tt_handle h, void *addr, size_t length);

// Close a handle. Mapped views stay valid until their tt_munmap.
void tt_close(tt_handle h);

// Windows error code (GetLastError) of the last failed tt_* call.
unsigned long tt_get_last_error(void);

#ifdef __cplusplus
}
#endif

#endif // TTWIN_COMPAT_H
