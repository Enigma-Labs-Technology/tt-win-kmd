// SPDX-FileCopyrightText: 2026 tt-win-kmd contributors
// SPDX-License-Identifier: GPL-2.0-only
//
// ttwin_compat implementation. Maps POSIX-shaped device access onto the ttkmd
// driver's device interface + IOCTLs. See ttwin_compat.h and
// docs/tt-umd-porting-notes.md.
#include "ttwin_compat.h"

#include <windows.h>
#include <winioctl.h>
#include <cfgmgr32.h>
#include <stdlib.h>
#include <string.h>

#include <initguid.h>
#include "ttkmd_ioctl.h"

// One live mmap view: kept on a per-handle list so the driver handle can be
// held open past tt_close until every view is unmapped (Linux mmap-after-close).
typedef struct tt_view {
    struct tt_view *next;
    void *addr;
    size_t length;
} tt_view;

struct tt_device {
    HANDLE file;
    int device_id;
    int closed;          // tt_close called but views outstanding
    tt_view *views;
};

static DWORD g_last_error;

unsigned long tt_get_last_error(void)
{
    return g_last_error;
}

// Interface-instance path for device N: the ttkmd device interface uses the
// per-device reference string "TT<ordinal>" (DD-2). Enumeration returns the
// present instances; the caller-visible id is the ordinal parsed from the ref
// string, so "device N" == the interface whose reference string is "TTN".
static WCHAR *tt_interface_list(ULONG *outLen)
{
    ULONG len = 0;
    WCHAR *list;
    CONFIGRET cr;

    cr = CM_Get_Device_Interface_List_SizeW(
        &len, (LPGUID)&GUID_DEVINTERFACE_TENSTORRENT, NULL,
        CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
    if (cr != CR_SUCCESS || len <= 1) {
        g_last_error = ERROR_NO_MORE_ITEMS;
        return NULL;
    }
    list = (WCHAR *)calloc(len, sizeof(WCHAR));
    if (list == NULL) {
        g_last_error = ERROR_NOT_ENOUGH_MEMORY;
        return NULL;
    }
    cr = CM_Get_Device_Interface_ListW(
        (LPGUID)&GUID_DEVINTERFACE_TENSTORRENT, NULL, list, len,
        CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
    if (cr != CR_SUCCESS) {
        free(list);
        g_last_error = ERROR_NO_MORE_ITEMS;
        return NULL;
    }
    *outLen = len;
    return list;
}

// Parse the ordinal from an interface path ending in "...\TT<ordinal>".
static int tt_ordinal_from_path(const WCHAR *path, int *ordinal)
{
    const WCHAR *tt = wcsstr(path, L"\\TT");

    if (tt == NULL) {
        return -1;
    }
    tt += 3;
    if (*tt < L'0' || *tt > L'9') {
        return -1;
    }
    *ordinal = _wtoi(tt);
    return 0;
}

int tt_enumerate(int *ids, int max_ids)
{
    ULONG len;
    WCHAR *list = tt_interface_list(&len);
    WCHAR *path;
    int count = 0;

    if (list == NULL) {
        // No devices present is not an error for enumeration (empty set).
        return 0;
    }
    for (path = list; *path; path += wcslen(path) + 1) {
        int ordinal;

        if (tt_ordinal_from_path(path, &ordinal) == 0) {
            if (ids != NULL && count < max_ids) {
                ids[count] = ordinal;
            }
            count++;
        }
    }
    free(list);
    return count;
}

tt_handle tt_open(int device_id, int flags)
{
    ULONG len;
    WCHAR *list = tt_interface_list(&len);
    WCHAR *path;
    HANDLE file = INVALID_HANDLE_VALUE;
    struct tt_device *dev;

    (void)flags;   // TT_O_APPEND handled via SET_CLIENT_FLAGS in a later step

    if (list == NULL) {
        return NULL;
    }
    for (path = list; *path; path += wcslen(path) + 1) {
        int ordinal;

        if (tt_ordinal_from_path(path, &ordinal) == 0 && ordinal == device_id) {
            file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                               OPEN_EXISTING, 0, NULL);
            break;
        }
    }
    free(list);

    if (file == INVALID_HANDLE_VALUE) {
        g_last_error = GetLastError() ? GetLastError() : ERROR_FILE_NOT_FOUND;
        return NULL;
    }

    dev = (struct tt_device *)calloc(1, sizeof(*dev));
    if (dev == NULL) {
        CloseHandle(file);
        g_last_error = ERROR_NOT_ENOUGH_MEMORY;
        return NULL;
    }
    dev->file = file;
    dev->device_id = device_id;
    return dev;
}

// Buffer size for a given ioctl nr. For METHOD_BUFFERED the same buffer is used
// for input and output; sizes come from the ABI ground truth. QUERY_MAPPINGS is
// variable: its size depends on in.output_mapping_count (first u32 of arg).
static DWORD tt_ioctl_size(unsigned nr, const void *arg)
{
    switch (nr) {
    case 0:  return 24;                                  // get_device_info
    case 2: {                                            // query_mappings
        uint32_t count = arg ? *(const uint32_t *)arg : 0;
        return (DWORD)(8u + count * 24u);
    }
    case 3:  return 64;                                  // allocate_dma_buf
    case 5:  return 16;                                  // get_driver_info
    case 6:  return 16;                                  // reset_device
    case 7:  return 32;                                  // pin_pages
    case 8:  return 16;                                  // lock_ctl
    case 10: return 24;                                  // unpin_pages
    case 11: return 48;                                  // allocate_tlb
    case 12: return 4;                                   // free_tlb
    case 13: return 48;                                  // configure_tlb
    case 14: return 32;                                  // set_noc_cleanup
    case 15: return 40;                                  // set_power_state
    default: return 0;                                   // unsupported by driver
    }
}

int tt_ioctl(tt_handle h, unsigned long request, void *arg)
{
    unsigned nr;
    DWORD code, size, returned = 0;
    BOOL ok;

    if (h == NULL) {
        g_last_error = ERROR_INVALID_HANDLE;
        return -1;
    }
    // Linux _IO(0xFA, nr) low byte is nr; verify the magic.
    if (((request >> 8) & 0xFF) != (unsigned long)TENSTORRENT_IOCTL_MAGIC) {
        g_last_error = ERROR_INVALID_PARAMETER;
        return -1;
    }
    nr = (unsigned)(request & 0xFF);
    size = tt_ioctl_size(nr, arg);
    code = TT_CTL(nr);

    ok = DeviceIoControl(h->file, code, arg, size, arg, size, &returned, NULL);
    if (!ok) {
        g_last_error = GetLastError();
        return -1;
    }
    return 0;
}

void *tt_mmap(tt_handle h, size_t length, int prot, int flags, uint64_t offset)
{
    struct tenstorrent_map map;
    tt_view *view;
    DWORD returned = 0;

    (void)prot;
    (void)flags;

    if (h == NULL) {
        g_last_error = ERROR_INVALID_HANDLE;
        return TT_MAP_FAILED;
    }

    memset(&map, 0, sizeof(map));
    map.in.mmap_offset = offset;
    map.in.length = length;
    if (!DeviceIoControl(h->file, IOCTL_TENSTORRENT_MAP, &map, sizeof(map),
                         &map, sizeof(map), &returned, NULL)) {
        g_last_error = GetLastError();
        return TT_MAP_FAILED;
    }

    view = (tt_view *)calloc(1, sizeof(*view));
    if (view == NULL) {
        // Best-effort unmap; report OOM.
        struct tenstorrent_unmap um;
        memset(&um, 0, sizeof(um));
        um.in.user_va = map.out.user_va;
        DeviceIoControl(h->file, IOCTL_TENSTORRENT_UNMAP, &um, sizeof(um),
                        &um, sizeof(um), &returned, NULL);
        g_last_error = ERROR_NOT_ENOUGH_MEMORY;
        return TT_MAP_FAILED;
    }
    view->addr = (void *)(uintptr_t)map.out.user_va;
    view->length = length;
    view->next = h->views;
    h->views = view;
    return view->addr;
}

// Actually close the driver handle and free the device once closed and no
// views remain.
static void tt_maybe_free(struct tt_device *h)
{
    if (h->closed && h->views == NULL) {
        CloseHandle(h->file);
        free(h);
    }
}

int tt_munmap(tt_handle h, void *addr, size_t length)
{
    struct tenstorrent_unmap um;
    tt_view **pp;
    DWORD returned = 0;
    BOOL ok;

    (void)length;
    if (h == NULL) {
        g_last_error = ERROR_INVALID_HANDLE;
        return -1;
    }

    for (pp = &h->views; *pp != NULL; pp = &(*pp)->next) {
        if ((*pp)->addr == addr) {
            tt_view *view = *pp;
            *pp = view->next;
            free(view);
            break;
        }
    }

    memset(&um, 0, sizeof(um));
    um.in.user_va = (uint64_t)(uintptr_t)addr;
    ok = DeviceIoControl(h->file, IOCTL_TENSTORRENT_UNMAP, &um, sizeof(um),
                         &um, sizeof(um), &returned, NULL);
    if (!ok) {
        g_last_error = GetLastError();
    }
    tt_maybe_free(h);
    return ok ? 0 : -1;
}

void tt_close(tt_handle h)
{
    if (h == NULL) {
        return;
    }
    h->closed = 1;
    // If views remain, keep the driver handle open (mmap survives close). The
    // driver would otherwise tear the mappings down at handle cleanup.
    tt_maybe_free(h);
}
