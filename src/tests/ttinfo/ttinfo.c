// M1 conformance test for tt-win-kmd: enumerates GUID_DEVINTERFACE_TENSTORRENT
// instances and exercises GET_DRIVER_INFO / GET_DEVICE_INFO / QUERY_MAPPINGS,
// positive and negative cases, asserting the Linux-parity semantics from
// docs/linux-driver-analysis.md §04.3 (zero-fill + truncation + -EFAULT maps).
//
// Exit code 0 iff every check passes. Intended to run inside the test VM
// against both the ttsim-backed Blackhole and the ROOT\TTKMD_SOFT device.

#include <windows.h>
#include <winioctl.h>
#include <cfgmgr32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <initguid.h>
#include "ttkmd_ioctl.h"
#include "ttkmd_abi_check.h"

static int g_failures;

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        g_failures++; \
        printf("FAIL: " __VA_ARGS__); \
        printf("  [%s]\n", #cond); \
    } \
} while (0)

static BOOL TtIoctl(HANDLE h, DWORD code, void *buf, DWORD inLen, DWORD outLen,
                    DWORD *info)
{
    *info = 0;
    return DeviceIoControl(h, code, buf, inLen, buf, outLen, info, NULL);
}

static void TestDriverInfo(HANDLE h)
{
    struct tenstorrent_get_driver_info arg;
    DWORD info;
    BOOL ok;

    memset(&arg, 0, sizeof(arg));
    arg.in.output_size_bytes = sizeof(arg.out);
    ok = TtIoctl(h, IOCTL_TENSTORRENT_GET_DRIVER_INFO, &arg, sizeof(arg),
                 sizeof(arg), &info);
    CHECK(ok, "GET_DRIVER_INFO failed, gle=%lu\n", GetLastError());
    CHECK(info == sizeof(arg.in) + sizeof(arg.out),
          "GET_DRIVER_INFO info=%lu\n", info);
    CHECK(arg.out.driver_version == TENSTORRENT_DRIVER_VERSION,
          "driver_version=%u\n", arg.out.driver_version);
    CHECK(arg.out.output_size_bytes == sizeof(arg.out),
          "out.output_size_bytes=%u\n", arg.out.output_size_bytes);
    printf("driver: ioctl-abi=%u version=%u.%u.%u\n", arg.out.driver_version,
           arg.out.driver_version_major, arg.out.driver_version_minor,
           arg.out.driver_version_patch);
}

static void TestDeviceInfo(HANDLE h, int *isRealDevice)
{
    struct tenstorrent_get_device_info arg;
    DWORD info;
    BOOL ok;

    memset(&arg, 0, sizeof(arg));
    arg.in.output_size_bytes = sizeof(arg.out);
    ok = TtIoctl(h, IOCTL_TENSTORRENT_GET_DEVICE_INFO, &arg, sizeof(arg),
                 sizeof(arg), &info);
    CHECK(ok, "GET_DEVICE_INFO failed, gle=%lu\n", GetLastError());
    CHECK(arg.out.output_size_bytes == sizeof(arg.out),
          "out.output_size_bytes=%u\n", arg.out.output_size_bytes);

    *isRealDevice = (arg.out.vendor_id != 0);
    if (*isRealDevice) {
        CHECK(arg.out.vendor_id == 0x1E52, "vendor=%04x\n", arg.out.vendor_id);
        CHECK(arg.out.device_id == 0xB140 || arg.out.device_id == 0x401E,
              "device=%04x\n", arg.out.device_id);
        CHECK(arg.out.max_dma_buf_size_log2 == 28,
              "max_dma_buf_size_log2=%u\n", arg.out.max_dma_buf_size_log2);
    }
    printf("device: %04x:%04x subsys=%04x:%04x bdf=%02x:%02x.%x domain=%u\n",
           arg.out.vendor_id, arg.out.device_id, arg.out.subsystem_vendor_id,
           arg.out.subsystem_id, arg.out.bus_dev_fn >> 8,
           (arg.out.bus_dev_fn >> 3) & 0x1F, arg.out.bus_dev_fn & 7,
           arg.out.pci_domain);

    // Truncation semantics: declared=4 -> only out.output_size_bytes copied,
    // Information = offsetof(out)+4 = 8, and the field reports the kernel's
    // full struct size (20), chardev.c:142/151-157.
    memset(&arg, 0xAA, sizeof(arg));
    arg.in.output_size_bytes = 4;
    ok = TtIoctl(h, IOCTL_TENSTORRENT_GET_DEVICE_INFO, &arg, sizeof(arg),
                 sizeof(arg), &info);
    CHECK(ok, "GET_DEVICE_INFO(trunc) failed, gle=%lu\n", GetLastError());
    CHECK(info == 8, "trunc info=%lu\n", info);
    CHECK(arg.out.output_size_bytes == 20,
          "trunc out.output_size_bytes=%u\n", arg.out.output_size_bytes);

    // -EFAULT parity: declared area exceeds the output buffer.
    memset(&arg, 0, sizeof(arg));
    arg.in.output_size_bytes = sizeof(arg.out) + 64;
    ok = TtIoctl(h, IOCTL_TENSTORRENT_GET_DEVICE_INFO, &arg, sizeof(arg),
                 sizeof(arg), &info);
    CHECK(!ok && GetLastError() == ERROR_NOACCESS,
          "oversize-declare: ok=%d gle=%lu\n", ok, GetLastError());
}

static void TestQueryMappings(HANDLE h, int isRealDevice)
{
    // Slots 0, 2, 6, 16: copy min(count, valid), zero-fill remainder
    // (memory.c:393-406).
    static const unsigned counts[] = { 0, 2, 6, 16 };
    unsigned ci;

    for (ci = 0; ci < ARRAYSIZE(counts); ci++) {
        unsigned n = counts[ci];
        size_t bytes = sizeof(struct tenstorrent_query_mappings_in) +
                       (size_t)n * sizeof(struct tenstorrent_mapping);
        struct tenstorrent_query_mappings_in *in = calloc(1, bytes);
        struct tenstorrent_mapping *maps = (struct tenstorrent_mapping *)(in + 1);
        unsigned expectValid = isRealDevice ? 6 : 0;
        unsigned expect = min(n, expectValid);
        DWORD info;
        unsigned i;
        BOOL ok;

        in->output_mapping_count = n;
        ok = TtIoctl(h, IOCTL_TENSTORRENT_QUERY_MAPPINGS, in, (DWORD)bytes,
                     (DWORD)bytes, &info);
        CHECK(ok, "QUERY_MAPPINGS(%u) failed, gle=%lu\n", n, GetLastError());
        CHECK(info == bytes, "QUERY_MAPPINGS(%u) info=%lu want=%zu\n", n, info,
              bytes);

        for (i = 0; i < expect; i++) {
            CHECK(maps[i].mapping_id == i + 1,
                  "map[%u].id=%u\n", i, maps[i].mapping_id);
            CHECK(maps[i].mapping_base == ((uint64_t)i) << 36,
                  "map[%u].base=%llx\n", i,
                  (unsigned long long)maps[i].mapping_base);
            CHECK(maps[i].mapping_size > 0, "map[%u].size=0\n", i);
        }
        for (; i < n; i++) {
            CHECK(maps[i].mapping_id == 0 && maps[i].mapping_base == 0 &&
                  maps[i].mapping_size == 0,
                  "map[%u] not zero-filled\n", i);
        }
        if (n >= 6 && isRealDevice) {
            printf("mappings: BAR0=%lluM BAR2=%lluK BAR4=%lluM\n",
                   (unsigned long long)maps[0].mapping_size >> 20,
                   (unsigned long long)maps[2].mapping_size >> 10,
                   (unsigned long long)maps[4].mapping_size >> 20);
        }
        free(in);
    }
}

static void TestNegative(HANDLE h)
{
    struct tenstorrent_get_device_info arg;
    DWORD info;
    BOOL ok;

    // Unported/unhandled nr falls to Linux's default -EINVAL
    // (chardev.c:694-696): GET_HARVESTING (nr 1).
    memset(&arg, 0, sizeof(arg));
    ok = TtIoctl(h, IOCTL_TENSTORRENT_GET_HARVESTING, &arg, sizeof(arg),
                 sizeof(arg), &info);
    CHECK(!ok && GetLastError() == ERROR_INVALID_PARAMETER,
          "GET_HARVESTING: ok=%d gle=%lu\n", ok, GetLastError());

    // Input buffer shorter than sizeof(in): -EFAULT parity.
    ok = TtIoctl(h, IOCTL_TENSTORRENT_GET_DEVICE_INFO, &arg, 2, sizeof(arg),
                 &info);
    CHECK(!ok && GetLastError() == ERROR_NOACCESS,
          "short-input: ok=%d gle=%lu\n", ok, GetLastError());
}

int wmain(void)
{
    WCHAR *list, *path;
    ULONG len = 0;
    int devices = 0;
    CONFIGRET cr;

    cr = CM_Get_Device_Interface_List_SizeW(
        &len, (LPGUID)&GUID_DEVINTERFACE_TENSTORRENT, NULL,
        CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
    if (cr != CR_SUCCESS || len <= 1) {
        printf("FAIL: no tenstorrent device interfaces present (cr=%u len=%lu)\n",
               cr, len);
        return 2;
    }
    list = calloc(len, sizeof(WCHAR));
    cr = CM_Get_Device_Interface_ListW(
        (LPGUID)&GUID_DEVINTERFACE_TENSTORRENT, NULL, list, len,
        CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
    if (cr != CR_SUCCESS) {
        printf("FAIL: CM_Get_Device_Interface_ListW cr=%u\n", cr);
        return 2;
    }

    for (path = list; *path; path += wcslen(path) + 1) {
        HANDLE h;
        int isRealDevice = 0;

        devices++;
        wprintf(L"\n== %s\n", path);
        h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                        OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            g_failures++;
            printf("FAIL: CreateFile gle=%lu\n", GetLastError());
            continue;
        }
        TestDriverInfo(h);
        TestDeviceInfo(h, &isRealDevice);
        TestQueryMappings(h, isRealDevice);
        TestNegative(h);
        CloseHandle(h);
    }

    printf("\nttinfo: %d device(s), %d failure(s)\n", devices, g_failures);
    return g_failures == 0 ? 0 : 1;
}
