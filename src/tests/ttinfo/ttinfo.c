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
#include "ttkmd_debug.h"

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

static void TestDeviceInfo(HANDLE h, int *isRealDevice, int *deviceId)
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
    *deviceId = arg.out.device_id;
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

// M2: telemetry + ARC message round-trip against the ttsim-backed Blackhole
// (debug ioctl surface; values asserted against ttsim's emulated CMFW state,
// ttsim tile.cpp telem[] table + heartbeat test-rig patch).
static void TestM2Firmware(HANDLE h, int deviceId)
{
    struct tenstorrent_debug_read_telemetry t;
    struct tenstorrent_debug_arc_msg m;
    DWORD info;
    BOOL ok;
    uint32_t hb1, hb2;

    if (deviceId != 0xB140) {
        // Soft device: debug telemetry must report NOT_SUPPORTED (no BH HW).
        memset(&t, 0, sizeof(t));
        ok = TtIoctl(h, IOCTL_TENSTORRENT_DEBUG_READ_TELEMETRY, &t, sizeof(t),
                     sizeof(t), &info);
        CHECK(!ok && GetLastError() == ERROR_NOT_SUPPORTED,
              "soft-dev telemetry: ok=%d gle=%lu\n", ok, GetLastError());
        return;
    }

    // Board ID high (tag 1): ttsim reports P150 (0x400).
    memset(&t, 0, sizeof(t));
    t.tag_id = 1;
    ok = TtIoctl(h, IOCTL_TENSTORRENT_DEBUG_READ_TELEMETRY, &t, sizeof(t),
                 sizeof(t), &info);
    CHECK(ok, "telemetry tag1 failed, gle=%lu\n", GetLastError());
    CHECK(t.value == 0x400, "board_id_high=%#x\n", t.value);

    // AICLK (tag 14): ttsim reports 1000 MHz.
    memset(&t, 0, sizeof(t));
    t.tag_id = 14;
    ok = TtIoctl(h, IOCTL_TENSTORRENT_DEBUG_READ_TELEMETRY, &t, sizeof(t),
                 sizeof(t), &info);
    CHECK(ok, "telemetry tag14 failed, gle=%lu\n", GetLastError());
    CHECK(t.value == 1000, "aiclk=%u\n", t.value);

    // Absent tag -> STATUS_NOT_FOUND (-ENODATA parity); ttsim has no tag 5.
    memset(&t, 0, sizeof(t));
    t.tag_id = 5;
    ok = TtIoctl(h, IOCTL_TENSTORRENT_DEBUG_READ_TELEMETRY, &t, sizeof(t),
                 sizeof(t), &info);
    CHECK(!ok && GetLastError() == ERROR_NOT_FOUND,
          "absent tag: ok=%d gle=%lu\n", ok, GetLastError());

    // Out-of-range tag -> -EINVAL parity.
    memset(&t, 0, sizeof(t));
    t.tag_id = 128;
    ok = TtIoctl(h, IOCTL_TENSTORRENT_DEBUG_READ_TELEMETRY, &t, sizeof(t),
                 sizeof(t), &info);
    CHECK(!ok && GetLastError() == ERROR_INVALID_PARAMETER,
          "tag>=128: ok=%d gle=%lu\n", ok, GetLastError());

    // Heartbeat (tag 32, TELEMETRY_TIMER_HEARTBEAT): must advance while the
    // chip runs (M2 acceptance).
    memset(&t, 0, sizeof(t));
    t.tag_id = 32;
    ok = TtIoctl(h, IOCTL_TENSTORRENT_DEBUG_READ_TELEMETRY, &t, sizeof(t),
                 sizeof(t), &info);
    CHECK(ok, "heartbeat read1 failed, gle=%lu\n", GetLastError());
    hb1 = t.value;
    Sleep(1500);
    memset(&t, 0, sizeof(t));
    t.tag_id = 32;
    ok = TtIoctl(h, IOCTL_TENSTORRENT_DEBUG_READ_TELEMETRY, &t, sizeof(t),
                 sizeof(t), &info);
    CHECK(ok, "heartbeat read2 failed, gle=%lu\n", GetLastError());
    hb2 = t.value;
    CHECK(hb2 > hb1, "heartbeat not advancing: %u -> %u\n", hb1, hb2);
    printf("heartbeat: %u -> %u (advancing)\n", hb1, hb2);

    // ARC message round-trip: TEST (0x90) must complete with response
    // header == 0 (send_arc_message success contract, blackhole.c:539).
    memset(&m, 0, sizeof(m));
    m.header = 0x90;   // ARC_MSG_TYPE_TEST
    m.payload[0] = 0x12345678;
    ok = TtIoctl(h, IOCTL_TENSTORRENT_DEBUG_ARC_MSG, &m, sizeof(m),
                 sizeof(m), &info);
    CHECK(ok, "ARC_MSG ioctl failed, gle=%lu\n", GetLastError());
    CHECK(m.success == 1, "ARC TEST success=%u header=%#x\n", m.success,
          m.header);
    CHECK(m.header == 0, "ARC TEST response header=%#x\n", m.header);
    printf("arc: TEST round-trip ok (response header=0)\n");
}

// M3: TLB windows, MAP/UNMAP, DMA buffers, pin pages (vs ttsim Blackhole).
static void TestM3Memory(HANDLE h, int deviceId)
{
    struct tenstorrent_allocate_tlb at;
    struct tenstorrent_configure_tlb ct;
    struct tenstorrent_free_tlb ft;
    struct tenstorrent_allocate_dma_buf ab;
    struct tenstorrent_map mp;
    struct tenstorrent_unmap um;
    struct tenstorrent_pin_pages pp;
    struct tenstorrent_unpin_pages up;
    DWORD info;
    BOOL ok;
    uint32_t tlbId;
    uint64_t tlbUcToken;
    volatile uint32_t *va;
    void *pinBuf;

    if (deviceId != 0xB140) {
        return;   // needs the Blackhole TLB/DMA hardware model
    }

    // --- TLB allocate: wrong size rejected, 2M accepted -------------------
    memset(&at, 0, sizeof(at));
    at.in.size = 1 << 20;
    ok = TtIoctl(h, IOCTL_TENSTORRENT_ALLOCATE_TLB, &at, sizeof(at), sizeof(at), &info);
    CHECK(!ok && GetLastError() == ERROR_INVALID_PARAMETER,
          "tlb wrong size: ok=%d gle=%lu\n", ok, GetLastError());

    memset(&at, 0, sizeof(at));
    at.in.size = 2 << 20;
    ok = TtIoctl(h, IOCTL_TENSTORRENT_ALLOCATE_TLB, &at, sizeof(at), sizeof(at), &info);
    CHECK(ok, "tlb alloc failed, gle=%lu\n", GetLastError());
    tlbId = at.out.id;
    tlbUcToken = at.out.mmap_offset_uc;
    CHECK(tlbId < 202, "tlb id=%u\n", tlbId);
    CHECK(tlbUcToken == (6ull << 36) + (uint64_t)tlbId * (2 << 20),
          "tlb uc token=%llx\n", (unsigned long long)tlbUcToken);
    CHECK(at.out.mmap_offset_wc == (7ull << 36) + (uint64_t)tlbId * (2 << 20),
          "tlb wc token=%llx\n", (unsigned long long)at.out.mmap_offset_wc);

    // --- Configure to ARC CSM; unaligned rejected -------------------------
    memset(&ct, 0, sizeof(ct));
    ct.in.id = tlbId;
    ct.in.config.addr = 0x10000000ull;   // ARC CSM base (2M aligned)
    ct.in.config.x_end = 8;              // ARC tile (8,0)
    ct.in.config.ordering = 1;           // strict
    ok = TtIoctl(h, IOCTL_TENSTORRENT_CONFIGURE_TLB, &ct, sizeof(ct), sizeof(ct), &info);
    CHECK(ok, "tlb configure failed, gle=%lu\n", GetLastError());

    ct.in.config.addr = 0x10000000ull + 4096;   // not window-aligned
    ok = TtIoctl(h, IOCTL_TENSTORRENT_CONFIGURE_TLB, &ct, sizeof(ct), sizeof(ct), &info);
    CHECK(!ok && GetLastError() == ERROR_INVALID_PARAMETER,
          "tlb cfg unaligned: ok=%d gle=%lu\n", ok, GetLastError());

    ct.in.id = tlbId + 1;                // not ours
    ct.in.config.addr = 0x10000000ull;
    ok = TtIoctl(h, IOCTL_TENSTORRENT_CONFIGURE_TLB, &ct, sizeof(ct), sizeof(ct), &info);
    CHECK(!ok && GetLastError() == ERROR_ACCESS_DENIED,
          "tlb cfg foreign: ok=%d gle=%lu\n", ok, GetLastError());

    // --- MAP the window (UC), read simulated CSM through user VA ----------
    memset(&mp, 0, sizeof(mp));
    mp.in.mmap_offset = tlbUcToken;
    mp.in.length = 2 << 20;
    ok = TtIoctl(h, IOCTL_TENSTORRENT_MAP, &mp, sizeof(mp), sizeof(mp), &info);
    CHECK(ok, "MAP tlb failed, gle=%lu\n", GetLastError());
    va = (volatile uint32_t *)(uintptr_t)mp.out.user_va;
    CHECK(va != NULL, "MAP returned null va\n");
    // CSM+0x100 = ttsim telemetry table: word0 = version 1
    CHECK(va[0x100 / 4] == 1, "csm telemetry version via user map = %u\n",
          va[0x100 / 4]);
    printf("tlb window: user VA reads CSM (telemetry version %u)\n", va[0x100 / 4]);

    // FREE while mapped -> EBUSY; UNMAP; FREE ok
    memset(&ft, 0, sizeof(ft));
    ft.in.id = tlbId;
    ok = TtIoctl(h, IOCTL_TENSTORRENT_FREE_TLB, &ft, sizeof(ft), sizeof(ft), &info);
    CHECK(!ok && GetLastError() == ERROR_BUSY,
          "free-while-mapped: ok=%d gle=%lu\n", ok, GetLastError());

    memset(&um, 0, sizeof(um));
    um.in.user_va = mp.out.user_va;
    ok = TtIoctl(h, IOCTL_TENSTORRENT_UNMAP, &um, sizeof(um), sizeof(um), &info);
    CHECK(ok, "UNMAP failed, gle=%lu\n", GetLastError());

    ok = TtIoctl(h, IOCTL_TENSTORRENT_FREE_TLB, &ft, sizeof(ft), sizeof(ft), &info);
    CHECK(ok, "FREE_TLB failed, gle=%lu\n", GetLastError());
    ok = TtIoctl(h, IOCTL_TENSTORRENT_FREE_TLB, &ft, sizeof(ft), sizeof(ft), &info);
    CHECK(!ok && GetLastError() == ERROR_ACCESS_DENIED,
          "double free: ok=%d gle=%lu\n", ok, GetLastError());

    // --- Open-BAR pattern: map BAR0 at the window's offset directly -------
    memset(&at, 0, sizeof(at));
    at.in.size = 2 << 20;
    ok = TtIoctl(h, IOCTL_TENSTORRENT_ALLOCATE_TLB, &at, sizeof(at), sizeof(at), &info);
    CHECK(ok, "tlb re-alloc failed, gle=%lu\n", GetLastError());
    memset(&ct, 0, sizeof(ct));
    ct.in.id = at.out.id;
    ct.in.config.addr = 0x10000000ull;
    ct.in.config.x_end = 8;
    ct.in.config.ordering = 1;
    ok = TtIoctl(h, IOCTL_TENSTORRENT_CONFIGURE_TLB, &ct, sizeof(ct), sizeof(ct), &info);
    CHECK(ok, "tlb re-configure failed, gle=%lu\n", GetLastError());
    memset(&mp, 0, sizeof(mp));
    mp.in.mmap_offset = (0ull << 36) + (uint64_t)at.out.id * (2 << 20); // BAR0 UC
    mp.in.length = 2 << 20;
    ok = TtIoctl(h, IOCTL_TENSTORRENT_MAP, &mp, sizeof(mp), sizeof(mp), &info);
    CHECK(ok, "MAP bar0 failed, gle=%lu\n", GetLastError());
    va = (volatile uint32_t *)(uintptr_t)mp.out.user_va;
    CHECK(va[0x100 / 4] == 1, "bar0-path csm version=%u\n", va[0x100 / 4]);
    memset(&um, 0, sizeof(um));
    um.in.user_va = mp.out.user_va;
    (void)TtIoctl(h, IOCTL_TENSTORRENT_UNMAP, &um, sizeof(um), sizeof(um), &info);
    memset(&ft, 0, sizeof(ft));
    ft.in.id = at.out.id;
    (void)TtIoctl(h, IOCTL_TENSTORRENT_FREE_TLB, &ft, sizeof(ft), sizeof(ft), &info);

    // --- DMA buffer: validation, alloc, map, write/read, NOC_DMA ----------
    memset(&ab, 0, sizeof(ab));
    ab.in.requested_size = 4096 + 1;   // unaligned
    ab.in.buf_index = 0;
    ok = TtIoctl(h, IOCTL_TENSTORRENT_ALLOCATE_DMA_BUF, &ab, sizeof(ab), sizeof(ab), &info);
    CHECK(!ok && GetLastError() == ERROR_INVALID_PARAMETER,
          "dmabuf unaligned: ok=%d gle=%lu\n", ok, GetLastError());

    memset(&ab, 0, sizeof(ab));
    ab.in.requested_size = 64 * 1024;
    ab.in.buf_index = 0;
    ok = TtIoctl(h, IOCTL_TENSTORRENT_ALLOCATE_DMA_BUF, &ab, sizeof(ab), sizeof(ab), &info);
    CHECK(ok, "dmabuf alloc failed, gle=%lu\n", GetLastError());
    CHECK(ab.out.physical_address != 0, "dmabuf phys=0\n");
    CHECK(ab.out.mapping_offset == 0xF000000000ull, "dmabuf token=%llx\n",
          (unsigned long long)ab.out.mapping_offset);
    CHECK(ab.out.size == 64 * 1024, "dmabuf size=%u\n", ab.out.size);

    ok = TtIoctl(h, IOCTL_TENSTORRENT_ALLOCATE_DMA_BUF, &ab, sizeof(ab), sizeof(ab), &info);
    CHECK(!ok && GetLastError() == ERROR_INVALID_PARAMETER,
          "dmabuf dup index: ok=%d gle=%lu\n", ok, GetLastError());

    memset(&mp, 0, sizeof(mp));
    mp.in.mmap_offset = 0xF000000000ull;
    mp.in.length = 64 * 1024;
    ok = TtIoctl(h, IOCTL_TENSTORRENT_MAP, &mp, sizeof(mp), sizeof(mp), &info);
    CHECK(ok, "MAP dmabuf failed, gle=%lu\n", GetLastError());
    va = (volatile uint32_t *)(uintptr_t)mp.out.user_va;
    va[0] = 0xDEADBEEF;
    va[16383] = 0x12345678;
    CHECK(va[0] == 0xDEADBEEF && va[16383] == 0x12345678,
          "dmabuf rw mismatch\n");
    printf("dmabuf: 64K mapped+verified at user VA, bus=%llx\n",
           (unsigned long long)ab.out.physical_address);

    // NOC_DMA variant on another index
    memset(&ab, 0, sizeof(ab));
    ab.in.requested_size = 64 * 1024;
    ab.in.buf_index = 1;
    ab.in.flags = TENSTORRENT_ALLOCATE_DMA_BUF_NOC_DMA;
    ok = TtIoctl(h, IOCTL_TENSTORRENT_ALLOCATE_DMA_BUF, &ab, sizeof(ab), sizeof(ab), &info);
    CHECK(ok, "dmabuf noc alloc failed, gle=%lu\n", GetLastError());
    CHECK(ab.out.noc_address >= (4ull << 58), "noc_address=%llx\n",
          (unsigned long long)ab.out.noc_address);
    printf("dmabuf: NOC DMA via iATU, noc=%llx\n",
           (unsigned long long)ab.out.noc_address);

    // FREE_DMA_BUF: upstream stub -EINVAL
    {
        DWORD zero = 0;
        ok = TtIoctl(h, IOCTL_TENSTORRENT_FREE_DMA_BUF, &zero, 0, 0, &info);
        CHECK(!ok && GetLastError() == ERROR_INVALID_PARAMETER,
              "FREE_DMA_BUF: ok=%d gle=%lu\n", ok, GetLastError());
    }

    // --- PIN_PAGES: single page positive + negatives ----------------------
    pinBuf = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    CHECK(pinBuf != NULL, "VirtualAlloc failed\n");
    memset(pinBuf, 0xAB, 4096);

    memset(&pp, 0, sizeof(pp));
    pp.in.output_size_bytes = sizeof(struct tenstorrent_pin_pages_out);
    pp.in.virtual_address = (uint64_t)(uintptr_t)pinBuf;
    pp.in.size = 4096;
    ok = TtIoctl(h, IOCTL_TENSTORRENT_PIN_PAGES, &pp, sizeof(pp), sizeof(pp), &info);
    CHECK(ok, "pin failed, gle=%lu\n", GetLastError());
    CHECK(pp.out.physical_address != 0, "pin phys=0\n");
    printf("pin: 4K page at phys=%llx\n",
           (unsigned long long)pp.out.physical_address);

    ok = TtIoctl(h, IOCTL_TENSTORRENT_PIN_PAGES, &pp, sizeof(pp), sizeof(pp), &info);
    CHECK(!ok && GetLastError() == ERROR_ALREADY_EXISTS,
          "dup pin: ok=%d gle=%lu\n", ok, GetLastError());

    memset(&up, 0, sizeof(up));
    up.in.virtual_address = (uint64_t)(uintptr_t)pinBuf;
    up.in.size = 8192;   // wrong size
    ok = TtIoctl(h, IOCTL_TENSTORRENT_UNPIN_PAGES, &up, sizeof(up), sizeof(up), &info);
    CHECK(!ok && GetLastError() == ERROR_INVALID_PARAMETER,
          "unpin wrong size: ok=%d gle=%lu\n", ok, GetLastError());

    up.in.size = 4096;
    ok = TtIoctl(h, IOCTL_TENSTORRENT_UNPIN_PAGES, &up, sizeof(up), sizeof(up), &info);
    CHECK(ok, "unpin failed, gle=%lu\n", GetLastError());

    memset(&pp, 0, sizeof(pp));
    pp.in.output_size_bytes = sizeof(struct tenstorrent_pin_pages_out);
    pp.in.virtual_address = (uint64_t)(uintptr_t)pinBuf;
    pp.in.size = 4096;
    pp.in.flags = TENSTORRENT_PIN_PAGES_READ_ONLY;
    ok = TtIoctl(h, IOCTL_TENSTORRENT_PIN_PAGES, &pp, sizeof(pp), sizeof(pp), &info);
    CHECK(!ok && GetLastError() == ERROR_NOT_SUPPORTED,
          "pin RO: ok=%d gle=%lu\n", ok, GetLastError());

    pp.in.flags = 0;
    pp.in.virtual_address += 1;   // misaligned
    ok = TtIoctl(h, IOCTL_TENSTORRENT_PIN_PAGES, &pp, sizeof(pp), sizeof(pp), &info);
    CHECK(!ok && GetLastError() == ERROR_INVALID_PARAMETER,
          "pin misaligned: ok=%d gle=%lu\n", ok, GetLastError());

    VirtualFree(pinBuf, 0, MEM_RELEASE);
    // dmabufs/maps intentionally left live: cleanup-on-close covers them.
}

// Background blocking-acquire: blocks in the driver until the lock frees.
struct BlockingLockArg {
    HANDLE h;
    BYTE index;
    volatile LONG completed;
    BYTE value;
    DWORD gle;
};

static DWORD WINAPI BlockingLockThread(LPVOID param)
{
    struct BlockingLockArg *a = (struct BlockingLockArg *)param;
    struct tenstorrent_lock_ctl lc;
    DWORD info;
    BOOL ok;

    memset(&lc, 0, sizeof(lc));
    lc.in.flags = TENSTORRENT_LOCK_CTL_ACQUIRE_BLOCKING;
    lc.in.index = a->index;
    ok = DeviceIoControl(a->h, IOCTL_TENSTORRENT_LOCK_CTL, &lc, sizeof(lc),
                         &lc, sizeof(lc), &info, NULL);
    a->value = lc.out.value;
    a->gle = ok ? 0 : GetLastError();
    InterlockedExchange(&a->completed, 1);
    return 0;
}

// M5: telemetry query, power aggregation, lock control (incl. blocking).
static void TestM5(const WCHAR *path, HANDLE h, int deviceId)
{
    struct tenstorrent_query_telemetry qt;
    struct tenstorrent_lock_ctl lc;
    struct tenstorrent_power_state ps;
    struct tenstorrent_debug_agg_power agg;
    DWORD info;
    BOOL ok;

    if (deviceId != 0xB140) {
        return;
    }

    // --- Telemetry query (hwmon-equivalent scaling) ----------------------
    memset(&qt, 0, sizeof(qt));
    ok = TtIoctl(h, IOCTL_TENSTORRENT_QUERY_TELEMETRY, &qt, sizeof(qt), sizeof(qt), &info);
    CHECK(ok, "QUERY_TELEMETRY gle=%lu\n", GetLastError());
    CHECK(qt.out.present & TT_TELEM_PRESENT_AICLK, "aiclk not present\n");
    CHECK(qt.out.aiclk_mhz == 1000, "aiclk=%u (want 1000)\n", qt.out.aiclk_mhz);
    CHECK(qt.out.present & TT_TELEM_PRESENT_SERIAL, "serial not present\n");
    // ttsim board id: high=0x400 (P150), low=0x1.
    CHECK(qt.out.serial == ((uint64_t)0x400 << 32 | 0x1),
          "serial=%llx\n", (unsigned long long)qt.out.serial);
    CHECK(qt.out.present & TT_TELEM_PRESENT_HEARTBEAT, "heartbeat not present\n");
    printf("telemetry: aiclk=%uMHz serial=%llx heartbeat=%u fw_bundle=%#x\n",
           qt.out.aiclk_mhz, (unsigned long long)qt.out.serial,
           qt.out.heartbeat, qt.out.fw_bundle_ver);

    // --- LOCK_CTL: acquire / test / release / re-acquire -----------------
    memset(&lc, 0, sizeof(lc));
    lc.in.flags = TENSTORRENT_LOCK_CTL_ACQUIRE;
    lc.in.index = 5;
    ok = TtIoctl(h, IOCTL_TENSTORRENT_LOCK_CTL, &lc, sizeof(lc), sizeof(lc), &info);
    CHECK(ok && lc.out.value == 1, "lock5 acquire value=%u\n", lc.out.value);

    lc.in.flags = TENSTORRENT_LOCK_CTL_TEST;
    ok = TtIoctl(h, IOCTL_TENSTORRENT_LOCK_CTL, &lc, sizeof(lc), sizeof(lc), &info);
    CHECK(ok && lc.out.value == 3, "lock5 test after acquire value=%u\n", lc.out.value);

    lc.in.index = 6;   // free
    ok = TtIoctl(h, IOCTL_TENSTORRENT_LOCK_CTL, &lc, sizeof(lc), sizeof(lc), &info);
    CHECK(ok && lc.out.value == 0, "lock6 test free value=%u\n", lc.out.value);

    lc.in.flags = TENSTORRENT_LOCK_CTL_RELEASE;
    lc.in.index = 5;
    ok = TtIoctl(h, IOCTL_TENSTORRENT_LOCK_CTL, &lc, sizeof(lc), sizeof(lc), &info);
    CHECK(ok && lc.out.value == 1, "lock5 release value=%u\n", lc.out.value);

    lc.in.flags = TENSTORRENT_LOCK_CTL_RELEASE;   // double release -> 0
    ok = TtIoctl(h, IOCTL_TENSTORRENT_LOCK_CTL, &lc, sizeof(lc), sizeof(lc), &info);
    CHECK(ok && lc.out.value == 0, "lock5 double release value=%u\n", lc.out.value);

    // Cross-handle contention + blocking acquire wakeup + cancellation.
    {
        HANDLE h2 = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                OPEN_EXISTING, 0, NULL);
        struct tenstorrent_lock_ctl lc2;
        struct BlockingLockArg ba;
        HANDLE thread;

        // h2 holds lock 7.
        memset(&lc2, 0, sizeof(lc2));
        lc2.in.flags = TENSTORRENT_LOCK_CTL_ACQUIRE;
        lc2.in.index = 7;
        ok = TtIoctl(h2, IOCTL_TENSTORRENT_LOCK_CTL, &lc2, sizeof(lc2), sizeof(lc2), &info);
        CHECK(ok && lc2.out.value == 1, "h2 lock7 acquire value=%u\n", lc2.out.value);

        // h's non-blocking acquire loses.
        memset(&lc, 0, sizeof(lc));
        lc.in.flags = TENSTORRENT_LOCK_CTL_ACQUIRE;
        lc.in.index = 7;
        ok = TtIoctl(h, IOCTL_TENSTORRENT_LOCK_CTL, &lc, sizeof(lc), sizeof(lc), &info);
        CHECK(ok && lc.out.value == 0, "h lock7 contended value=%u\n", lc.out.value);

        // h's BLOCKING acquire pends in the driver; it must not return yet.
        ba.h = h; ba.index = 7; ba.completed = 0; ba.value = 0; ba.gle = 0;
        thread = CreateThread(NULL, 0, BlockingLockThread, &ba, 0, NULL);
        CHECK(thread != NULL, "CreateThread failed\n");
        Sleep(500);
        CHECK(ba.completed == 0, "blocking acquire returned early (still held)\n");

        // h2 releases -> the pended waiter wakes and acquires (value 1).
        lc2.in.flags = TENSTORRENT_LOCK_CTL_RELEASE;
        lc2.in.index = 7;
        ok = TtIoctl(h2, IOCTL_TENSTORRENT_LOCK_CTL, &lc2, sizeof(lc2), sizeof(lc2), &info);
        CHECK(ok && lc2.out.value == 1, "h2 lock7 release value=%u\n", lc2.out.value);
        WaitForSingleObject(thread, 5000);
        CHECK(ba.completed == 1 && ba.value == 1,
              "blocking acquire not woken: completed=%ld value=%u gle=%lu\n",
              ba.completed, ba.value, ba.gle);
        CloseHandle(thread);
        printf("locks: blocking acquire woke on release (value=%u) PASS\n", ba.value);

        // Cancellation: the blocking waiter (ba) that just won holds lock 7 on
        // h. hc issues a blocking acquire on 7 (blocks on h's hold), then we
        // cancel its in-flight synchronous ioctl -> STATUS_CANCELLED.
        {
            HANDLE hc = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                    OPEN_EXISTING, 0, NULL);
            struct BlockingLockArg bc;

            bc.h = hc; bc.index = 7; bc.completed = 0; bc.value = 0; bc.gle = 0;
            thread = CreateThread(NULL, 0, BlockingLockThread, &bc, 0, NULL);
            CHECK(thread != NULL, "cancel CreateThread failed\n");
            Sleep(500);
            CHECK(bc.completed == 0, "cancel-case acquire returned early\n");
            CancelSynchronousIo(thread);   // cancel the pended blocking request
            WaitForSingleObject(thread, 5000);
            CHECK(bc.completed == 1 && bc.value != 1,
                  "cancel did not unblock: completed=%ld value=%u gle=%lu\n",
                  bc.completed, bc.value, bc.gle);
            CloseHandle(thread);
            CloseHandle(hc);
            printf("locks: blocking acquire cancelled (gle=%lu) PASS\n", bc.gle);
        }

        // h releases lock 7 (won via the blocking acquire above).
        lc.in.flags = TENSTORRENT_LOCK_CTL_RELEASE;
        lc.in.index = 7;
        TtIoctl(h, IOCTL_TENSTORRENT_LOCK_CTL, &lc, sizeof(lc), sizeof(lc), &info);
        CloseHandle(h2);
        printf("locks: acquire/test/release/contention PASS\n");
    }

    // --- Power aggregation: two handles OR flags, max settings -----------
    {
        HANDLE hb = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                OPEN_EXISTING, 0, NULL);
        // h: power-aware, requests MAX_AI_CLK + setting[0]=100.
        memset(&ps, 0, sizeof(ps));
        ps.argsz = sizeof(ps);
        ps.validity = (uint8_t)TT_POWER_VALIDITY(4, 1);
        ps.power_flags = TT_POWER_FLAG_MAX_AI_CLK;
        ps.power_settings[0] = 100;
        ok = TtIoctl(h, IOCTL_TENSTORRENT_SET_POWER_STATE, &ps, sizeof(ps), sizeof(ps), &info);
        CHECK(ok, "h SET_POWER_STATE gle=%lu\n", GetLastError());

        // hb: requests TENSIX_ENABLE + setting[0]=250.
        memset(&ps, 0, sizeof(ps));
        ps.argsz = sizeof(ps);
        ps.validity = (uint8_t)TT_POWER_VALIDITY(4, 1);
        ps.power_flags = TT_POWER_FLAG_TENSIX_ENABLE;
        ps.power_settings[0] = 250;
        ok = TtIoctl(hb, IOCTL_TENSTORRENT_SET_POWER_STATE, &ps, sizeof(ps), sizeof(ps), &info);
        CHECK(ok, "hb SET_POWER_STATE gle=%lu\n", GetLastError());

        // Aggregate: flags OR'd (MAX_AI_CLK | TENSIX_ENABLE plus any legacy
        // default contributors' bits), setting[0] = max(100,250) = 250.
        memset(&agg, 0, sizeof(agg));
        ok = TtIoctl(h, IOCTL_TENSTORRENT_DEBUG_GET_AGG_POWER, &agg, sizeof(agg), sizeof(agg), &info);
        CHECK(ok && agg.valid, "agg power gle=%lu valid=%u\n", GetLastError(), agg.valid);
        CHECK((agg.power_flags & TT_POWER_FLAG_MAX_AI_CLK) &&
              (agg.power_flags & TT_POWER_FLAG_TENSIX_ENABLE),
              "agg flags=%#x (missing OR)\n", agg.power_flags);
        CHECK(agg.power_settings[0] == 250,
              "agg setting[0]=%u (want max 250)\n", agg.power_settings[0]);
        printf("power: aggregated flags=%#x setting[0]=%u (OR + max) PASS\n",
               agg.power_flags, agg.power_settings[0]);
        CloseHandle(hb);
    }
}

// M4: reset flavors, fd-invalidation, reset-under-mapping. Needs a second
// handle (the "worker") plus a fresh handle per reset (tt-umd/tools pattern).
static void TestM4Reset(const WCHAR *path, int deviceId)
{
    HANDLE worker, resetter;
    struct tenstorrent_reset_device rd;
    struct tenstorrent_get_device_info di;
    struct tenstorrent_allocate_tlb at;
    struct tenstorrent_configure_tlb ct;
    struct tenstorrent_map mp;
    volatile uint32_t *va;
    DWORD info;
    BOOL ok;

    if (deviceId != 0xB140) {
        return;
    }

    // Worker handle maps a TLB window (the "active mapping").
    worker = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                         OPEN_EXISTING, 0, NULL);
    CHECK(worker != INVALID_HANDLE_VALUE, "worker open gle=%lu\n", GetLastError());

    memset(&at, 0, sizeof(at));
    at.in.size = 2 << 20;
    ok = TtIoctl(worker, IOCTL_TENSTORRENT_ALLOCATE_TLB, &at, sizeof(at), sizeof(at), &info);
    CHECK(ok, "m4 tlb alloc gle=%lu\n", GetLastError());
    memset(&ct, 0, sizeof(ct));
    ct.in.id = at.out.id;
    ct.in.config.addr = 0x10000000ull;
    ct.in.config.x_end = 8;
    ct.in.config.ordering = 1;
    ok = TtIoctl(worker, IOCTL_TENSTORRENT_CONFIGURE_TLB, &ct, sizeof(ct), sizeof(ct), &info);
    CHECK(ok, "m4 tlb cfg gle=%lu\n", GetLastError());
    memset(&mp, 0, sizeof(mp));
    mp.in.mmap_offset = at.out.mmap_offset_uc;
    mp.in.length = 2 << 20;
    ok = TtIoctl(worker, IOCTL_TENSTORRENT_MAP, &mp, sizeof(mp), sizeof(mp), &info);
    CHECK(ok, "m4 map gle=%lu\n", GetLastError());
    va = (volatile uint32_t *)(uintptr_t)mp.out.user_va;
    CHECK(va[0x100 / 4] == 1, "m4 pre-reset CSM read=%u\n", va[0x100 / 4]);

    // Invalid reset flag -> INVALID_PARAMETER.
    memset(&rd, 0, sizeof(rd));
    rd.in.output_size_bytes = sizeof(rd.out);
    rd.in.flags = 99;
    ok = TtIoctl(worker, IOCTL_TENSTORRENT_RESET_DEVICE, &rd, sizeof(rd), sizeof(rd), &info);
    CHECK(!ok && GetLastError() == ERROR_INVALID_PARAMETER,
          "m4 bad flag: ok=%d gle=%lu\n", ok, GetLastError());

    // Destructive reset from a FRESH handle (mappings live on the worker) —
    // this is the tt-umd/tools/reset.c pattern.
    resetter = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, 0, NULL);
    CHECK(resetter != INVALID_HANDLE_VALUE, "resetter open gle=%lu\n", GetLastError());

    memset(&rd, 0, sizeof(rd));
    rd.in.output_size_bytes = sizeof(rd.out);
    rd.in.flags = TENSTORRENT_RESET_DEVICE_ASIC_RESET;   // 4
    ok = TtIoctl(resetter, IOCTL_TENSTORRENT_RESET_DEVICE, &rd, sizeof(rd), sizeof(rd), &info);
    CHECK(ok, "m4 ASIC_RESET ioctl gle=%lu\n", GetLastError());
    CHECK(rd.out.result == 0, "m4 ASIC_RESET result=%u\n", rd.out.result);
    printf("reset: ASIC_RESET result=%u (success)\n", rd.out.result);

    // The worker handle was opened before the gen bump -> now permanently
    // invalid: every ioctl returns STATUS_DEVICE_REMOVED (ERROR_NO_SUCH_DEVICE).
    memset(&di, 0, sizeof(di));
    di.in.output_size_bytes = sizeof(di.out);
    ok = TtIoctl(worker, IOCTL_TENSTORRENT_GET_DEVICE_INFO, &di, sizeof(di), sizeof(di), &info);
    CHECK(!ok && (GetLastError() == ERROR_NO_SUCH_DEVICE ||
                  GetLastError() == ERROR_DEVICE_REMOVED),
          "m4 stale worker not invalidated: ok=%d gle=%lu\n", ok, GetLastError());
    printf("reset: stale worker handle -> gle=%lu (device removed)\n", GetLastError());

    // The worker's mapping was zapped; touching it now faults. Verify via SEH.
    {
        BOOL faulted = FALSE;
        __try {
            va[0] = 0xFFFFFFFF;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            faulted = TRUE;
        }
        CHECK(faulted, "m4 zapped mapping still accessible\n");
        printf("reset: stale mapping access faulted (zapped)\n");
    }

    // The resetter (carried forward) is still valid and in the reset window:
    // GET_DEVICE_INFO allowed; a non-allowlisted ioctl (QUERY_MAPPINGS) -> removed.
    memset(&di, 0, sizeof(di));
    di.in.output_size_bytes = sizeof(di.out);
    ok = TtIoctl(resetter, IOCTL_TENSTORRENT_GET_DEVICE_INFO, &di, sizeof(di), sizeof(di), &info);
    CHECK(ok, "m4 resetter GET_DEVICE_INFO in window gle=%lu\n", GetLastError());
    {
        struct tenstorrent_query_mappings_in qm;
        memset(&qm, 0, sizeof(qm));
        ok = TtIoctl(resetter, IOCTL_TENSTORRENT_QUERY_MAPPINGS, &qm, sizeof(qm), sizeof(qm), &info);
        CHECK(!ok, "m4 non-allowlisted ioctl allowed in reset window\n");
    }

    // POST_RESET completes the sequence (marker cleared by the glue).
    memset(&rd, 0, sizeof(rd));
    rd.in.output_size_bytes = sizeof(rd.out);
    rd.in.flags = TENSTORRENT_RESET_DEVICE_POST_RESET;   // 6
    ok = TtIoctl(resetter, IOCTL_TENSTORRENT_RESET_DEVICE, &rd, sizeof(rd), sizeof(rd), &info);
    CHECK(ok, "m4 POST_RESET ioctl gle=%lu\n", GetLastError());
    CHECK(rd.out.result == 0, "m4 POST_RESET result=%u (marker not cleared?)\n", rd.out.result);
    printf("reset: POST_RESET result=%u (window closed)\n", rd.out.result);

    // After POST_RESET the resetter is fully usable again.
    {
        struct tenstorrent_query_mappings_in qm;
        memset(&qm, 0, sizeof(qm));
        ok = TtIoctl(resetter, IOCTL_TENSTORRENT_QUERY_MAPPINGS, &qm, sizeof(qm), sizeof(qm), &info);
        CHECK(ok, "m4 post-window QUERY_MAPPINGS gle=%lu\n", GetLastError());
    }

    CloseHandle(worker);
    CloseHandle(resetter);
    printf("reset: lifecycle sequence PASS\n");
}

// M4 reset storm: hammer resets from fresh handles while a worker holds live
// mappings and issues ioctls. Verifies no crash and correct invalidation.
static int RunResetStorm(const WCHAR *path, int iterations)
{
    int i;

    for (i = 0; i < iterations; i++) {
        HANDLE worker, resetter;
        struct tenstorrent_reset_device rd;
        struct tenstorrent_allocate_tlb at;
        struct tenstorrent_configure_tlb ct;
        struct tenstorrent_map mp;
        DWORD info;

        worker = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                             OPEN_EXISTING, 0, NULL);
        if (worker == INVALID_HANDLE_VALUE) {
            printf("storm %d: worker open gle=%lu\n", i, GetLastError());
            return 1;
        }
        memset(&at, 0, sizeof(at));
        at.in.size = 2 << 20;
        if (TtIoctl(worker, IOCTL_TENSTORRENT_ALLOCATE_TLB, &at, sizeof(at), sizeof(at), &info)) {
            memset(&ct, 0, sizeof(ct));
            ct.in.id = at.out.id;
            ct.in.config.addr = 0x10000000ull;
            ct.in.config.x_end = 8;
            ct.in.config.ordering = 1;
            TtIoctl(worker, IOCTL_TENSTORRENT_CONFIGURE_TLB, &ct, sizeof(ct), sizeof(ct), &info);
            memset(&mp, 0, sizeof(mp));
            mp.in.mmap_offset = at.out.mmap_offset_uc;
            mp.in.length = 2 << 20;
            TtIoctl(worker, IOCTL_TENSTORRENT_MAP, &mp, sizeof(mp), sizeof(mp), &info);
        }

        resetter = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                               OPEN_EXISTING, 0, NULL);
        if (resetter == INVALID_HANDLE_VALUE) {
            printf("storm %d: resetter open gle=%lu\n", i, GetLastError());
            return 1;
        }
        memset(&rd, 0, sizeof(rd));
        rd.in.output_size_bytes = sizeof(rd.out);
        rd.in.flags = (i & 1) ? TENSTORRENT_RESET_DEVICE_ASIC_RESET
                              : TENSTORRENT_RESET_DEVICE_CONFIG_WRITE;
        if (!TtIoctl(resetter, IOCTL_TENSTORRENT_RESET_DEVICE, &rd, sizeof(rd), sizeof(rd), &info)) {
            printf("storm %d: reset gle=%lu\n", i, GetLastError());
            return 1;
        }
        // Worker is now stale; a stray ioctl must be safely rejected, not crash.
        {
            struct tenstorrent_get_device_info di;
            memset(&di, 0, sizeof(di));
            di.in.output_size_bytes = sizeof(di.out);
            TtIoctl(worker, IOCTL_TENSTORRENT_GET_DEVICE_INFO, &di, sizeof(di), sizeof(di), &info);
        }
        // Complete so the device is usable next iteration.
        memset(&rd, 0, sizeof(rd));
        rd.in.output_size_bytes = sizeof(rd.out);
        rd.in.flags = TENSTORRENT_RESET_DEVICE_POST_RESET;
        TtIoctl(resetter, IOCTL_TENSTORRENT_RESET_DEVICE, &rd, sizeof(rd), sizeof(rd), &info);

        CloseHandle(worker);
        CloseHandle(resetter);
        if ((i + 1) % 200 == 0) {
            printf("storm: %d resets\n", i + 1);
        }
    }
    printf("storm: %d resets PASS\n", iterations);
    return 0;
}

// 10,000-cycle open/alloc/map/pin/close soak (M3 acceptance: no leaks).
static int RunSoak(const WCHAR *path, int cycles)
{
    int i;

    for (i = 0; i < cycles; i++) {
        HANDLE h;
        struct tenstorrent_allocate_tlb at;
        struct tenstorrent_configure_tlb ct;
        struct tenstorrent_allocate_dma_buf ab;
        struct tenstorrent_map mp;
        struct tenstorrent_pin_pages pp;
        void *pinBuf;
        DWORD info;

        h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                        OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            printf("soak %d: open gle=%lu\n", i, GetLastError());
            return 1;
        }

        memset(&at, 0, sizeof(at));
        at.in.size = 2 << 20;
        if (!TtIoctl(h, IOCTL_TENSTORRENT_ALLOCATE_TLB, &at, sizeof(at), sizeof(at), &info)) {
            printf("soak %d: tlb gle=%lu\n", i, GetLastError());
            return 1;
        }
        memset(&ct, 0, sizeof(ct));
        ct.in.id = at.out.id;
        ct.in.config.addr = 0x10000000ull;
        ct.in.config.x_end = 8;
        ct.in.config.ordering = 1;
        if (!TtIoctl(h, IOCTL_TENSTORRENT_CONFIGURE_TLB, &ct, sizeof(ct), sizeof(ct), &info)) {
            printf("soak %d: cfg gle=%lu\n", i, GetLastError());
            return 1;
        }
        memset(&mp, 0, sizeof(mp));
        mp.in.mmap_offset = at.out.mmap_offset_uc;
        mp.in.length = 2 << 20;
        if (!TtIoctl(h, IOCTL_TENSTORRENT_MAP, &mp, sizeof(mp), sizeof(mp), &info)) {
            printf("soak %d: map gle=%lu\n", i, GetLastError());
            return 1;
        }
        if (*(volatile uint32_t *)(uintptr_t)(mp.out.user_va + 0x100) != 1) {
            printf("soak %d: csm readback mismatch\n", i);
            return 1;
        }
        memset(&ab, 0, sizeof(ab));
        ab.in.requested_size = 64 * 1024;
        ab.in.buf_index = 3;
        ab.in.flags = TENSTORRENT_ALLOCATE_DMA_BUF_NOC_DMA;
        if (!TtIoctl(h, IOCTL_TENSTORRENT_ALLOCATE_DMA_BUF, &ab, sizeof(ab), sizeof(ab), &info)) {
            printf("soak %d: dmabuf gle=%lu\n", i, GetLastError());
            return 1;
        }
        pinBuf = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        memset(pinBuf, 1, 4096);
        memset(&pp, 0, sizeof(pp));
        pp.in.output_size_bytes = sizeof(struct tenstorrent_pin_pages_out);
        pp.in.virtual_address = (uint64_t)(uintptr_t)pinBuf;
        pp.in.size = 4096;
        if (!TtIoctl(h, IOCTL_TENSTORRENT_PIN_PAGES, &pp, sizeof(pp), sizeof(pp), &info)) {
            printf("soak %d: pin gle=%lu\n", i, GetLastError());
            return 1;
        }

        CloseHandle(h);   // cleanup-on-close tears down everything
        VirtualFree(pinBuf, 0, MEM_RELEASE);

        if ((i + 1) % 1000 == 0) {
            printf("soak: %d cycles\n", i + 1);
        }
    }
    printf("soak: %d cycles PASS\n", cycles);
    return 0;
}

int wmain(int argc, wchar_t **argv)
{
    WCHAR *list, *path;
    ULONG len = 0;
    int devices = 0;
    int soakCycles = 0;
    CONFIGRET cr;

    int stormIters = 0;

    if (argc >= 3 && wcscmp(argv[1], L"--soak") == 0) {
        soakCycles = _wtoi(argv[2]);
    }
    if (argc >= 3 && wcscmp(argv[1], L"--storm") == 0) {
        stormIters = _wtoi(argv[2]);
    }

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
        int deviceId = 0;

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
        if (soakCycles > 0 || stormIters > 0) {
            CloseHandle(h);
            if (wcsstr(path, L"PCI#VEN_1E52") != NULL) {
                return soakCycles > 0 ? RunSoak(path, soakCycles)
                                      : RunResetStorm(path, stormIters);
            }
            continue;
        }

        TestDriverInfo(h);
        TestDeviceInfo(h, &isRealDevice, &deviceId);
        TestQueryMappings(h, isRealDevice);
        TestNegative(h);
        TestM2Firmware(h, deviceId);
        TestM3Memory(h, deviceId);
        TestM5(path, h, deviceId);
        CloseHandle(h);

        if (isRealDevice) {
            TestM4Reset(path, deviceId);
        }
    }

    if (soakCycles > 0 || stormIters > 0) {
        printf("no PCI device found\n");
        return 2;
    }

    printf("\nttinfo: %d device(s), %d failure(s)\n", devices, g_failures);
    return g_failures == 0 ? 0 : 1;
}
