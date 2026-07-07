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

    if (argc >= 3 && wcscmp(argv[1], L"--soak") == 0) {
        soakCycles = _wtoi(argv[2]);
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
        if (soakCycles > 0) {
            CloseHandle(h);
            if (wcsstr(path, L"PCI#VEN_1E52") != NULL) {
                return RunSoak(path, soakCycles);
            }
            continue;
        }

        TestDriverInfo(h);
        TestDeviceInfo(h, &isRealDevice, &deviceId);
        TestQueryMappings(h, isRealDevice);
        TestNegative(h);
        TestM2Firmware(h, deviceId);
        TestM3Memory(h, deviceId);
        CloseHandle(h);
    }

    if (soakCycles > 0) {
        printf("soak: no PCI device found\n");
        return 2;
    }

    printf("\nttinfo: %d device(s), %d failure(s)\n", devices, g_failures);
    return g_failures == 0 ? 0 : 1;
}
