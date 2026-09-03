// SPDX-FileCopyrightText: 2026 tt-win-kmd contributors
// SPDX-License-Identifier: GPL-2.0-only
//
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
static BOOL g_simOracle;   // --sim-oracle: keep ttsim exact-equality CHECKs

// --only selectors (bitmask). See Usage() for the CLI grammar.
#define SEL_INFO      (1u << 0)   // driver + device info
#define SEL_MAPPINGS  (1u << 1)   // QUERY_MAPPINGS
#define SEL_NEGATIVE  (1u << 2)   // negative / parity cases
#define SEL_FIRMWARE  (1u << 3)   // TestM2Firmware (ARC TEST path)
#define SEL_TELEMETRY (1u << 4)   // read-only telemetry dump
#define SEL_TLB       (1u << 5)   // TLB alloc/configure/map/read
#define SEL_DMA       (1u << 6)   // DMA-buffer alloc/map/rw
#define SEL_PIN       (1u << 7)   // PIN_PAGES

// Read-only-safe default set (bare `ttinfo.exe`): no resets, no TLB/DMA/pin.
#define SEL_DEFAULT (SEL_INFO | SEL_MAPPINGS | SEL_NEGATIVE | SEL_TELEMETRY)

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

// Production liveness probe: read the firmware heartbeat via the RELEASE-safe
// IOCTL_TENSTORRENT_QUERY_TELEMETRY (never the debug DEBUG_READ_TELEMETRY, which
// is compiled out of release drivers), wait, read again. Wrap-tolerant: any u32
// change means the chip is running. Requires TT_TELEM_PRESENT_HEARTBEAT in both
// reads. waitMs==0 uses the 1500 ms default.
// Reads TIMER_HEARTBEAT via the production QUERY_TELEMETRY and returns TRUE
// once the counter is observed to change (wrap-tolerant: any change == the ARC
// is running). Poll-based rather than a single sleep so it tolerates the
// firmware-reboot outage measured on real silicon: after a reset,
// QUERY_TELEMETRY fails for ~1.64 s and the counter restarts from ~0
// (real-silicon-linux/reset-timing.txt). waitMs is the minimum observation
// span; the total budget is at least ~5 s so a post-reset check does not
// false-fail across the outage. A read failure is treated as "not yet", not
// fatal — we keep polling until the deadline.
static BOOL HeartbeatAdvances(HANDLE dev, DWORD waitMs)
{
    struct tenstorrent_query_telemetry qt;
    DWORD info;
    uint32_t first = 0;
    BOOL haveFirst = FALSE;
    DWORD span = waitMs ? waitMs : 1500;
    DWORD budget = span < 5000 ? 5000 : span;   // tolerate ~2 s reset outage
    DWORD elapsed;

    for (elapsed = 0; elapsed <= budget; elapsed += 300) {
        memset(&qt, 0, sizeof(qt));
        if (TtIoctl(dev, IOCTL_TENSTORRENT_QUERY_TELEMETRY, &qt, sizeof(qt),
                    sizeof(qt), &info) &&
            (qt.out.present & TT_TELEM_PRESENT_HEARTBEAT)) {
            if (!haveFirst) {
                first = qt.out.heartbeat;
                haveFirst = TRUE;
            } else if (qt.out.heartbeat != first && elapsed >= span) {
                return TRUE;
            }
        }
        Sleep(300);
    }
    return FALSE;
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
// The debug IOCTL surface (ttkmd_debug.h) exists only in Debug driver builds.
// A Release driver answers every debug code with ERROR_INVALID_FUNCTION; the
// sections that depend on it are then reported as skipped, not failed.
static BOOL DebugIoctlsAvailable(HANDLE h)
{
    static int cached = -1;
    struct tenstorrent_debug_read_telemetry t;
    DWORD info;
    BOOL ok;

    if (cached < 0) {
        memset(&t, 0, sizeof(t));
        t.tag_id = 1;
        ok = TtIoctl(h, IOCTL_TENSTORRENT_DEBUG_READ_TELEMETRY, &t, sizeof(t),
                     sizeof(t), &info);
        cached = (!ok && GetLastError() == ERROR_INVALID_FUNCTION) ? 0 : 1;
        if (!cached) {
            printf("debug ioctls: unavailable (Release driver); dependent checks skipped\n");
        }
    }
    return cached ? TRUE : FALSE;
}

static void TestM2Firmware(HANDLE h, int deviceId)
{
    struct tenstorrent_debug_read_telemetry t;
    struct tenstorrent_debug_arc_msg m;
    DWORD info;
    BOOL ok;
    uint32_t hb1, hb2;

    if (!DebugIoctlsAvailable(h)) {
        return;
    }

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
    if (g_simOracle) {
        CHECK(t.value == 0x400, "board_id_high=%#x\n", t.value);
    } else {
        // Silicon: decode card_type; nonzero passes. 0x40 == p150a (WARN only).
        uint32_t cardType = (t.value >> 4) & 0xFFFF;
        CHECK(cardType != 0, "board_id_high=%#x card_type=0\n", t.value);
        printf("board: id_high=%#x card_type=%#x%s\n", t.value, cardType,
               cardType == 0x40 ? " (p150a)" : "");
        if (cardType != 0x40) {
            printf("WARN: card_type %#x != 0x40 (p150a expected)\n", cardType);
        }
    }

    // AICLK (tag 14): ttsim reports 1000 MHz.
    memset(&t, 0, sizeof(t));
    t.tag_id = 14;
    ok = TtIoctl(h, IOCTL_TENSTORRENT_DEBUG_READ_TELEMETRY, &t, sizeof(t),
                 sizeof(t), &info);
    CHECK(ok, "telemetry tag14 failed, gle=%lu\n", GetLastError());
    if (g_simOracle) {
        CHECK(t.value == 1000, "aiclk=%u\n", t.value);
    } else {
        CHECK(t.value >= 100 && t.value <= 2000, "aiclk=%u (want 100..2000)\n",
              t.value);
        printf("aiclk: %u MHz\n", t.value);
    }

    // Absent tag -> STATUS_NOT_FOUND (-ENODATA parity); ttsim has no tag 5.
    memset(&t, 0, sizeof(t));
    t.tag_id = 5;
    ok = TtIoctl(h, IOCTL_TENSTORRENT_DEBUG_READ_TELEMETRY, &t, sizeof(t),
                 sizeof(t), &info);
    if (g_simOracle) {
        CHECK(!ok && GetLastError() == ERROR_NOT_FOUND,
              "absent tag: ok=%d gle=%lu\n", ok, GetLastError());
    } else {
        // Silicon: tag 5 presence is board-dependent; report, do not assert.
        printf("tag5: %s\n", ok ? "present" : "absent");
    }

    // Out-of-range tag -> -EINVAL parity (structural; always asserted).
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

// M3 (tlb): TLB windows + MAP/UNMAP (vs ttsim Blackhole). Factored out of the
// original TestM3Memory so `--only tlb` runs just this rung; code is byte-
// equivalent apart from the CSM-liveness reads, which are silicon-gated (#3/#5).
static void TestM3Tlb(HANDLE h, int deviceId)
{
    struct tenstorrent_allocate_tlb at;
    struct tenstorrent_configure_tlb ct;
    struct tenstorrent_free_tlb ft;
    struct tenstorrent_map mp;
    struct tenstorrent_unmap um;
    DWORD info;
    BOOL ok;
    uint32_t tlbId;
    uint64_t tlbUcToken;
    volatile uint32_t *va;

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
    if (g_simOracle) {
        // CSM+0x100 = ttsim telemetry table: word0 = version 1
        CHECK(va[0x100 / 4] == 1, "csm telemetry version via user map = %u\n",
              va[0x100 / 4]);
        printf("tlb window: user VA reads CSM (telemetry version %u)\n", va[0x100 / 4]);
    } else {
        // Silicon: no CSM dereference before liveness is proven — the window
        // targets a management-core NOC endpoint; prove liveness via the
        // production telemetry path instead.
        printf("tlb window: user VA mapped (CSM contents not read on silicon)\n");
        CHECK(HeartbeatAdvances(h, 1500),
              "tlb window: heartbeat health check failed\n");
    }

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
    if (g_simOracle) {
        CHECK(va[0x100 / 4] == 1, "bar0-path csm version=%u\n", va[0x100 / 4]);
    } else {
        printf("tlb bar0-path: mapped (CSM contents not read on silicon)\n");
        CHECK(HeartbeatAdvances(h, 1500),
              "tlb bar0-path: heartbeat health check failed\n");
    }
    memset(&um, 0, sizeof(um));
    um.in.user_va = mp.out.user_va;
    (void)TtIoctl(h, IOCTL_TENSTORRENT_UNMAP, &um, sizeof(um), sizeof(um), &info);
    memset(&ft, 0, sizeof(ft));
    ft.in.id = at.out.id;
    (void)TtIoctl(h, IOCTL_TENSTORRENT_FREE_TLB, &ft, sizeof(ft), sizeof(ft), &info);
}

// M3 (dma): DMA-buffer validation/alloc/map/verify + NOC_DMA. Factored from the
// original TestM3Memory (byte-equivalent) so `--only dma` runs just this rung.
static void TestM3Dma(HANDLE h, int deviceId)
{
    struct tenstorrent_allocate_dma_buf ab;
    struct tenstorrent_map mp;
    DWORD info;
    BOOL ok;
    volatile uint32_t *va;

    if (deviceId != 0xB140) {
        return;   // needs the Blackhole TLB/DMA hardware model
    }

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

    // FREE_DMA_BUF_EX (DD-16): frees a slot early; busy while a view exists.
    {
        struct tenstorrent_free_dma_buf_ex fr;
        struct tenstorrent_unmap um;

        // Index 0 is still mapped (va above) -> busy.
        memset(&fr, 0, sizeof(fr));
        fr.in.buf_index = 0;
        ok = TtIoctl(h, IOCTL_TENSTORRENT_FREE_DMA_BUF_EX, &fr, sizeof(fr), sizeof(fr), &info);
        CHECK(!ok && GetLastError() == ERROR_BUSY,
              "free_ex mapped: ok=%d gle=%lu\n", ok, GetLastError());

        // Unmap the view, then the free succeeds and the slot is reusable.
        memset(&um, 0, sizeof(um));
        um.in.user_va = mp.out.user_va;
        ok = TtIoctl(h, IOCTL_TENSTORRENT_UNMAP, &um, sizeof(um), sizeof(um), &info);
        CHECK(ok, "unmap dmabuf view gle=%lu\n", GetLastError());

        memset(&fr, 0, sizeof(fr));
        fr.in.buf_index = 0;
        ok = TtIoctl(h, IOCTL_TENSTORRENT_FREE_DMA_BUF_EX, &fr, sizeof(fr), sizeof(fr), &info);
        CHECK(ok, "free_ex index 0 gle=%lu\n", GetLastError());

        ok = TtIoctl(h, IOCTL_TENSTORRENT_FREE_DMA_BUF_EX, &fr, sizeof(fr), sizeof(fr), &info);
        CHECK(!ok && GetLastError() == ERROR_INVALID_PARAMETER,
              "free_ex twice: ok=%d gle=%lu\n", ok, GetLastError());

        memset(&ab, 0, sizeof(ab));
        ab.in.requested_size = 64 * 1024;
        ab.in.buf_index = 0;
        ok = TtIoctl(h, IOCTL_TENSTORRENT_ALLOCATE_DMA_BUF, &ab, sizeof(ab), sizeof(ab), &info);
        CHECK(ok, "dmabuf realloc index 0 gle=%lu\n", GetLastError());

        // NOC-mapped index 1: free tears the aperture down as well.
        memset(&fr, 0, sizeof(fr));
        fr.in.buf_index = 1;
        ok = TtIoctl(h, IOCTL_TENSTORRENT_FREE_DMA_BUF_EX, &fr, sizeof(fr), sizeof(fr), &info);
        CHECK(ok, "free_ex index 1 (noc) gle=%lu\n", GetLastError());

        // Reserved bits and out-of-range slots are rejected.
        memset(&fr, 0, sizeof(fr));
        fr.in.buf_index = 0;
        fr.in.reserved = 1;
        ok = TtIoctl(h, IOCTL_TENSTORRENT_FREE_DMA_BUF_EX, &fr, sizeof(fr), sizeof(fr), &info);
        CHECK(!ok && GetLastError() == ERROR_INVALID_PARAMETER,
              "free_ex reserved: ok=%d gle=%lu\n", ok, GetLastError());
        memset(&fr, 0, sizeof(fr));
        fr.in.buf_index = 256;
        ok = TtIoctl(h, IOCTL_TENSTORRENT_FREE_DMA_BUF_EX, &fr, sizeof(fr), sizeof(fr), &info);
        CHECK(!ok && GetLastError() == ERROR_INVALID_PARAMETER,
              "free_ex range: ok=%d gle=%lu\n", ok, GetLastError());
        printf("dmabuf: FREE_DMA_BUF_EX busy/free/reuse PASS\n");
    }
}

// DD-17: the per-device DMA-buffer ceiling. With the built-in default (4 GiB)
// and the 256 MiB per-buffer cap, the 17th full-size buffer must be refused
// with the quota error and earlier ones stay usable. Skipped when the driver
// runs without a limit (registry override 0) or the host cannot supply the
// contiguous memory, since neither proves anything about the ceiling.
static void TestM3DmaQuota(HANDLE h, int deviceId)
{
    struct tenstorrent_allocate_dma_buf ab;
    struct tenstorrent_free_dma_buf_ex fr;
    DWORD info;
    BOOL ok;
    int allocated = 0;
    int i;
    DWORD gle = 0;

    if (deviceId != 0xB140) {
        return;
    }

    for (i = 0; i < 17; i++) {
        memset(&ab, 0, sizeof(ab));
        ab.in.requested_size = 256u << 20;
        ab.in.buf_index = (uint8_t)(100 + i);
        ok = TtIoctl(h, IOCTL_TENSTORRENT_ALLOCATE_DMA_BUF, &ab, sizeof(ab), sizeof(ab), &info);
        if (!ok) {
            gle = GetLastError();
            break;
        }
        allocated++;
    }

    if (allocated == 17) {
        printf("dmabuf quota: 17 x 256 MiB accepted (limit disabled or raised); skipped\n");
    } else if (allocated < 16 && gle == ERROR_NO_SYSTEM_RESOURCES) {
        printf("dmabuf quota: host ran out of contiguous memory after %d buffers; skipped\n", allocated);
    } else {
        CHECK(allocated == 16 && gle == ERROR_NOT_ENOUGH_QUOTA,
              "dmabuf quota: allocated=%d gle=%lu (want 16 + ERROR_NOT_ENOUGH_QUOTA)\n",
              allocated, gle);
        printf("dmabuf quota: 16 x 256 MiB then ERROR_NOT_ENOUGH_QUOTA PASS\n");
    }

    for (i = 0; i < allocated; i++) {
        memset(&fr, 0, sizeof(fr));
        fr.in.buf_index = (uint32_t)(100 + i);
        ok = TtIoctl(h, IOCTL_TENSTORRENT_FREE_DMA_BUF_EX, &fr, sizeof(fr), sizeof(fr), &info);
        CHECK(ok, "dmabuf quota: free %d gle=%lu\n", i, GetLastError());
    }
}

// M3 (pin): PIN_PAGES positive + negatives. Factored from the original
// TestM3Memory (byte-equivalent) so `--only pin` runs just this rung.
static void TestM3Pin(HANDLE h, int deviceId)
{
    struct tenstorrent_pin_pages pp;
    struct tenstorrent_unpin_pages up;
    void *pinBuf;
    DWORD info;
    BOOL ok;

    if (deviceId != 0xB140) {
        return;   // needs the Blackhole TLB/DMA hardware model
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

    // --- DD-16: pinning a MAP view of one of this handle's DMA buffers -----
    // No pages are locked; the device address is the buffer's bus address and
    // the NOC aperture is allocated bottom-up (the tt-umd sysmem path).
    {
        struct tenstorrent_allocate_dma_buf ab;
        struct tenstorrent_map mp;
        struct tenstorrent_free_dma_buf_ex fr;
        struct tenstorrent_unmap um;
        struct {
            struct tenstorrent_pin_pages_in in;
            struct tenstorrent_pin_pages_out_extended out;
        } ppx;

        memset(&ab, 0, sizeof(ab));
        ab.in.requested_size = 1u << 20;
        ab.in.buf_index = 4;
        ok = TtIoctl(h, IOCTL_TENSTORRENT_ALLOCATE_DMA_BUF, &ab, sizeof(ab), sizeof(ab), &info);
        CHECK(ok, "backed pin: dmabuf alloc gle=%lu\n", GetLastError());

        memset(&mp, 0, sizeof(mp));
        mp.in.mmap_offset = ab.out.mapping_offset;
        mp.in.length = 1u << 20;
        ok = TtIoctl(h, IOCTL_TENSTORRENT_MAP, &mp, sizeof(mp), sizeof(mp), &info);
        CHECK(ok, "backed pin: map gle=%lu\n", GetLastError());

        // Whole view, NOC-mapped: device address == the buffer's bus address.
        memset(&ppx, 0, sizeof(ppx));
        ppx.in.output_size_bytes = sizeof(ppx.out);
        ppx.in.virtual_address = mp.out.user_va;
        ppx.in.size = 1u << 20;
        ppx.in.flags = TENSTORRENT_PIN_PAGES_CONTIGUOUS | TENSTORRENT_PIN_PAGES_NOC_DMA;
        ok = TtIoctl(h, IOCTL_TENSTORRENT_PIN_PAGES, &ppx, sizeof(ppx), sizeof(ppx), &info);
        CHECK(ok, "backed pin: pin gle=%lu\n", GetLastError());
        CHECK(ppx.out.physical_address == ab.out.physical_address,
              "backed pin: phys %llx != bus %llx\n",
              (unsigned long long)ppx.out.physical_address,
              (unsigned long long)ab.out.physical_address);
        CHECK(ppx.out.noc_address >= (4ull << 58),
              "backed pin: noc=%llx\n", (unsigned long long)ppx.out.noc_address);

        // The buffer cannot be freed while the pin references it.
        memset(&fr, 0, sizeof(fr));
        fr.in.buf_index = 4;
        ok = TtIoctl(h, IOCTL_TENSTORRENT_FREE_DMA_BUF_EX, &fr, sizeof(fr), sizeof(fr), &info);
        CHECK(!ok && GetLastError() == ERROR_BUSY,
              "backed pin: free while pinned ok=%d gle=%lu\n", ok, GetLastError());

        memset(&up, 0, sizeof(up));
        up.in.virtual_address = mp.out.user_va;
        up.in.size = 1u << 20;
        ok = TtIoctl(h, IOCTL_TENSTORRENT_UNPIN_PAGES, &up, sizeof(up), sizeof(up), &info);
        CHECK(ok, "backed pin: unpin gle=%lu\n", GetLastError());

        // Sub-range inside the view: offset carries through to the address.
        memset(&ppx, 0, sizeof(ppx));
        ppx.in.output_size_bytes = sizeof(ppx.out);
        ppx.in.virtual_address = mp.out.user_va + 4096;
        ppx.in.size = 8192;
        ok = TtIoctl(h, IOCTL_TENSTORRENT_PIN_PAGES, &ppx, sizeof(ppx), sizeof(ppx), &info);
        CHECK(ok, "backed pin: sub-range pin gle=%lu\n", GetLastError());
        CHECK(ppx.out.physical_address == ab.out.physical_address + 4096,
              "backed pin: sub-range phys %llx\n",
              (unsigned long long)ppx.out.physical_address);
        up.in.virtual_address = mp.out.user_va + 4096;
        up.in.size = 8192;
        ok = TtIoctl(h, IOCTL_TENSTORRENT_UNPIN_PAGES, &up, sizeof(up), sizeof(up), &info);
        CHECK(ok, "backed pin: sub-range unpin gle=%lu\n", GetLastError());

        // A range running past the view is not backed and falls through to
        // the direct path, which rejects the driver mapping as non-user RAM
        // or refuses in a translated domain; either way it must not succeed
        // as a backed pin.
        memset(&ppx, 0, sizeof(ppx));
        ppx.in.output_size_bytes = sizeof(ppx.out);
        ppx.in.virtual_address = mp.out.user_va + (1u << 20) - 4096;
        ppx.in.size = 8192;
        ok = TtIoctl(h, IOCTL_TENSTORRENT_PIN_PAGES, &ppx, sizeof(ppx), sizeof(ppx), &info);
        CHECK(!ok, "backed pin: overrun pin unexpectedly ok\n");

        memset(&um, 0, sizeof(um));
        um.in.user_va = mp.out.user_va;
        ok = TtIoctl(h, IOCTL_TENSTORRENT_UNMAP, &um, sizeof(um), sizeof(um), &info);
        CHECK(ok, "backed pin: unmap gle=%lu\n", GetLastError());
        memset(&fr, 0, sizeof(fr));
        fr.in.buf_index = 4;
        ok = TtIoctl(h, IOCTL_TENSTORRENT_FREE_DMA_BUF_EX, &fr, sizeof(fr), sizeof(fr), &info);
        CHECK(ok, "backed pin: free gle=%lu\n", GetLastError());
        printf("pin: DMA-buffer-backed pin/sub-range/busy PASS\n");
    }
    // dmabufs/maps intentionally left live: cleanup-on-close covers them.
}

// DD-15: MAP/UNMAP/PIN/UNPIN are only honoured from the process that opened
// the handle. Duplicate a handle into a child and let it try a MAP; the
// driver must answer ERROR_ACCESS_DENIED before looking at the request.
static void TestProcessGuard(const WCHAR *path)
{
    SECURITY_ATTRIBUTES sa;
    HANDLE inheritable;
    WCHAR exe[MAX_PATH];
    WCHAR cmdline[MAX_PATH + 64];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD exitCode = (DWORD)-1;
    BOOL ok;

    memset(&sa, 0, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    inheritable = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                              OPEN_EXISTING, 0, NULL);
    CHECK(inheritable != INVALID_HANDLE_VALUE, "guard: open gle=%lu\n", GetLastError());
    if (inheritable == INVALID_HANDLE_VALUE) {
        return;
    }

    if (GetModuleFileNameW(NULL, exe, MAX_PATH) == 0) {
        CHECK(0, "guard: GetModuleFileName gle=%lu\n", GetLastError());
        CloseHandle(inheritable);
        return;
    }
    swprintf_s(cmdline, MAX_PATH + 64, L"\"%s\" --child-map %llu", exe,
               (unsigned long long)(uintptr_t)inheritable);

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    ok = CreateProcessW(exe, cmdline, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
    CHECK(ok, "guard: CreateProcess gle=%lu\n", GetLastError());
    if (ok) {
        WaitForSingleObject(pi.hProcess, 30000);
        GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        CHECK(exitCode == 0, "guard: child exit code %lu (0 = MAP refused with ACCESS_DENIED)\n",
              exitCode);
        if (exitCode == 0) {
            printf("guard: MAP through an inherited handle refused in the child PASS\n");
        }
    }
    CloseHandle(inheritable);
}

// Child side of TestProcessGuard: the handle value was inherited from the
// parent. Exit 0 only if MAP is refused with ERROR_ACCESS_DENIED.
static int RunChildMap(unsigned long long handleValue)
{
    HANDLE h = (HANDLE)(uintptr_t)handleValue;
    struct tenstorrent_map mp;
    DWORD info;
    BOOL ok;

    memset(&mp, 0, sizeof(mp));
    mp.in.mmap_offset = 0;      // BAR0 UC base; never reached
    mp.in.length = 4096;
    ok = TtIoctl(h, IOCTL_TENSTORRENT_MAP, &mp, sizeof(mp), sizeof(mp), &info);
    if (ok) {
        printf("child-map: MAP unexpectedly succeeded\n");
        return 1;
    }
    if (GetLastError() != ERROR_ACCESS_DENIED) {
        printf("child-map: MAP failed with gle=%lu, want ERROR_ACCESS_DENIED\n", GetLastError());
        return 1;
    }
    return 0;
}

// --multiproc N: N concurrent copies of --soak M against the device, the
// multi-process regime tt-umd and tt-metal run in. Each child is a full
// ttinfo invocation so the per-handle teardown paths interleave for real.
static int RunMultiproc(int processes, int cycles)
{
    WCHAR exe[MAX_PATH];
    HANDLE *procs;
    int i;
    int failures = 0;

    if (GetModuleFileNameW(NULL, exe, MAX_PATH) == 0) {
        printf("multiproc: GetModuleFileName gle=%lu\n", GetLastError());
        return 1;
    }
    procs = (HANDLE *)calloc((size_t)processes, sizeof(HANDLE));
    if (procs == NULL) {
        return 1;
    }
    for (i = 0; i < processes; i++) {
        WCHAR cmdline[MAX_PATH + 64];
        STARTUPINFOW si;
        PROCESS_INFORMATION pi;

        swprintf_s(cmdline, MAX_PATH + 64, L"\"%s\" --soak %d", exe, cycles);
        memset(&si, 0, sizeof(si));
        si.cb = sizeof(si);
        memset(&pi, 0, sizeof(pi));
        if (!CreateProcessW(exe, cmdline, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
            printf("multiproc: CreateProcess %d gle=%lu\n", i, GetLastError());
            failures++;
            continue;
        }
        CloseHandle(pi.hThread);
        procs[i] = pi.hProcess;
    }
    for (i = 0; i < processes; i++) {
        DWORD exitCode = (DWORD)-1;

        if (procs[i] == NULL) {
            continue;
        }
        WaitForSingleObject(procs[i], INFINITE);
        GetExitCodeProcess(procs[i], &exitCode);
        CloseHandle(procs[i]);
        if (exitCode != 0) {
            printf("multiproc: child %d exit code %lu\n", i, exitCode);
            failures++;
        }
    }
    free(procs);
    printf("multiproc: %d processes x %d cycles, %d failures %s\n",
           processes, cycles, failures, failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
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
    if (g_simOracle) {
        CHECK(qt.out.aiclk_mhz == 1000, "aiclk=%u (want 1000)\n", qt.out.aiclk_mhz);
    } else {
        CHECK(qt.out.aiclk_mhz >= 100 && qt.out.aiclk_mhz <= 2000,
              "aiclk=%u (want 100..2000)\n", qt.out.aiclk_mhz);
    }
    CHECK(qt.out.present & TT_TELEM_PRESENT_SERIAL, "serial not present\n");
    if (g_simOracle) {
        // ttsim board id: high=0x400 (P150), low=0x1.
        CHECK(qt.out.serial == ((uint64_t)0x400 << 32 | 0x1),
              "serial=%llx\n", (unsigned long long)qt.out.serial);
    } else {
        CHECK(qt.out.serial != 0, "serial=0\n");
    }
    CHECK(qt.out.present & TT_TELEM_PRESENT_HEARTBEAT, "heartbeat not present\n");
    printf("telemetry: aiclk=%uMHz serial=%llx heartbeat=%u fw_bundle=%#x\n",
           qt.out.aiclk_mhz, (unsigned long long)qt.out.serial,
           qt.out.heartbeat, qt.out.fw_bundle_ver);
    if (!g_simOracle) {
        // Decode fw_bundle bytes 3..0 as maj.min.patch.ver; asic_id nonzero.
        printf("telemetry: fw_bundle %u.%u.%u.%u asic_id=%#llx\n",
               (qt.out.fw_bundle_ver >> 24) & 0xFF, (qt.out.fw_bundle_ver >> 16) & 0xFF,
               (qt.out.fw_bundle_ver >> 8) & 0xFF, qt.out.fw_bundle_ver & 0xFF,
               (unsigned long long)qt.out.asic_id);
    }

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
        // Readback needs the Debug driver's aggregate-power IOCTL.
        if (DebugIoctlsAvailable(h)) {
        memset(&agg, 0, sizeof(agg));
        ok = TtIoctl(h, IOCTL_TENSTORRENT_DEBUG_GET_AGG_POWER, &agg, sizeof(agg), sizeof(agg), &info);
        CHECK(ok && agg.valid, "agg power gle=%lu valid=%u\n", GetLastError(), agg.valid);
        CHECK((agg.power_flags & TT_POWER_FLAG_MAX_AI_CLK) &&
              (agg.power_flags & TT_POWER_FLAG_TENSIX_ENABLE),
              "agg flags=%#x (missing OR)\n", agg.power_flags);
        CHECK(agg.power_settings[0] == 250,
              "agg setting[0]=%u (want max 250)\n", agg.power_settings[0]);
        }
        printf("power: aggregated flags=%#x setting[0]=%u (OR + max) PASS\n",
               agg.power_flags, agg.power_settings[0]);
        CloseHandle(hb);
    }

    // --- SET_NOC_CLEANUP: register + validation -------------------------
    {
        struct tenstorrent_set_noc_cleanup nc;

        memset(&nc, 0, sizeof(nc));
        nc.argsz = sizeof(nc);
        nc.enabled = 1;
        nc.x = 1; nc.y = 0; nc.noc = 0;
        nc.addr = 0x1000;      // 4-byte aligned
        nc.data = 0xDEADBEEF;
        ok = TtIoctl(h, IOCTL_TENSTORRENT_SET_NOC_CLEANUP, &nc, sizeof(nc), sizeof(nc), &info);
        CHECK(ok, "SET_NOC_CLEANUP gle=%lu\n", GetLastError());

        // Negatives: unaligned addr, noc>1, bad argsz.
        nc.addr = 0x1002;
        ok = TtIoctl(h, IOCTL_TENSTORRENT_SET_NOC_CLEANUP, &nc, sizeof(nc), sizeof(nc), &info);
        CHECK(!ok && GetLastError() == ERROR_INVALID_PARAMETER,
              "noc_cleanup unaligned accepted\n");
        nc.addr = 0x1000; nc.noc = 2;
        ok = TtIoctl(h, IOCTL_TENSTORRENT_SET_NOC_CLEANUP, &nc, sizeof(nc), sizeof(nc), &info);
        CHECK(!ok && GetLastError() == ERROR_INVALID_PARAMETER,
              "noc_cleanup bad noc accepted\n");
        nc.noc = 0; nc.argsz = 8;
        ok = TtIoctl(h, IOCTL_TENSTORRENT_SET_NOC_CLEANUP, &nc, sizeof(nc), sizeof(nc), &info);
        CHECK(!ok && GetLastError() == ERROR_INVALID_PARAMETER,
              "noc_cleanup bad argsz accepted\n");

        // Clear it so the ttsim NOC write at close targets a harmless tile only
        // when re-enabled by another test; here disable.
        nc.argsz = sizeof(nc); nc.enabled = 0;
        TtIoctl(h, IOCTL_TENSTORRENT_SET_NOC_CLEANUP, &nc, sizeof(nc), sizeof(nc), &info);
        printf("noc_cleanup: register + validation PASS\n");
    }
}

// Read-only telemetry dump: issue the production QUERY_TELEMETRY and print every
// field with its present bit (`name=value (present)` / `name=--- (absent)`). No
// value assertions beyond present-mask nonzero + heartbeat liveness (#4/#5).
static void TestTelemetryDump(HANDLE h, int deviceId)
{
    struct tenstorrent_query_telemetry qt;
    DWORD info;
    BOOL ok;

    if (deviceId != 0xB140) {
        printf("telemetry: skipped (no Blackhole telemetry on device %04x)\n",
               deviceId);
        return;
    }

    memset(&qt, 0, sizeof(qt));
    ok = TtIoctl(h, IOCTL_TENSTORRENT_QUERY_TELEMETRY, &qt, sizeof(qt), sizeof(qt), &info);
    CHECK(ok, "QUERY_TELEMETRY gle=%lu\n", GetLastError());
    if (!ok) {
        return;
    }
    CHECK(qt.out.present != 0, "telemetry present-mask=0\n");

    // One line per field; type-specific formatting, gated on the present bit.
#define TELEM_I(field, bit) \
    do { if (qt.out.present & (bit)) \
             printf("  %-16s = %d (present)\n", #field, (int)qt.out.field); \
         else printf("  %-16s = --- (absent)\n", #field); } while (0)
#define TELEM_U(field, bit) \
    do { if (qt.out.present & (bit)) \
             printf("  %-16s = %u (present)\n", #field, (unsigned)qt.out.field); \
         else printf("  %-16s = --- (absent)\n", #field); } while (0)
#define TELEM_X64(field, bit) \
    do { if (qt.out.present & (bit)) \
             printf("  %-16s = %#llx (present)\n", #field, \
                    (unsigned long long)qt.out.field); \
         else printf("  %-16s = --- (absent)\n", #field); } while (0)

    printf("telemetry dump (present-mask=%#x):\n", qt.out.present);
    TELEM_I(temp_input_mc,    TT_TELEM_PRESENT_TEMP);
    TELEM_I(temp_max_mc,      TT_TELEM_PRESENT_TEMP);
    TELEM_U(vcore_input_mv,   TT_TELEM_PRESENT_VCORE);
    TELEM_U(vcore_max_mv,     TT_TELEM_PRESENT_VCORE);
    TELEM_U(curr_input_ma,    TT_TELEM_PRESENT_CURRENT);
    TELEM_U(curr_max_ma,      TT_TELEM_PRESENT_CURRENT);
    TELEM_U(power_input_uw,   TT_TELEM_PRESENT_POWER);
    TELEM_U(power_max_uw,     TT_TELEM_PRESENT_POWER);
    TELEM_U(aiclk_mhz,        TT_TELEM_PRESENT_AICLK);
    TELEM_U(axiclk_mhz,       TT_TELEM_PRESENT_AXICLK);
    TELEM_U(arcclk_mhz,       TT_TELEM_PRESENT_ARCCLK);
    TELEM_U(fan_rpm,          TT_TELEM_PRESENT_FAN);
    TELEM_U(heartbeat,        TT_TELEM_PRESENT_HEARTBEAT);
    TELEM_U(therm_trip_count, TT_TELEM_PRESENT_THERMTRIP);
    TELEM_X64(serial,         TT_TELEM_PRESENT_SERIAL);
    TELEM_X64(asic_id,        TT_TELEM_PRESENT_ASIC_ID);
    if (qt.out.present & TT_TELEM_PRESENT_FW_BUNDLE) {
        printf("  %-16s = %#x -> %u.%u.%u.%u (present)\n", "fw_bundle_ver",
               qt.out.fw_bundle_ver,
               (qt.out.fw_bundle_ver >> 24) & 0xFF, (qt.out.fw_bundle_ver >> 16) & 0xFF,
               (qt.out.fw_bundle_ver >> 8) & 0xFF, qt.out.fw_bundle_ver & 0xFF);
    } else {
        printf("  %-16s = --- (absent)\n", "fw_bundle_ver");
    }

#undef TELEM_I
#undef TELEM_U
#undef TELEM_X64

    // The only assertion beyond present-mask nonzero: the chip is alive.
    CHECK(HeartbeatAdvances(h, 1500), "telemetry: heartbeat did not advance\n");
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
    if (g_simOracle) {
        CHECK(va[0x100 / 4] == 1, "m4 pre-reset CSM read=%u\n", va[0x100 / 4]);
    } else {
        CHECK(HeartbeatAdvances(worker, 1500),
              "m4 pre-reset heartbeat health check failed\n");
    }

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
        if (g_simOracle) {
            if (*(volatile uint32_t *)(uintptr_t)(mp.out.user_va + 0x100) != 1) {
                printf("soak %d: csm readback mismatch\n", i);
                return 1;
            }
        } else if ((i + 1) % 100 == 0) {
            // Silicon: heartbeat health check once per 100 iterations instead of
            // a per-cycle CSM oracle read.
            if (!HeartbeatAdvances(h, 1500)) {
                printf("soak %d: heartbeat health check failed\n", i);
                return 1;
            }
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

// Parse a comma-separated --only list into a selector bitmask. Unknown tokens
// count as a failure (so the exit code reflects the typo) and are skipped.
static unsigned ParseSelectors(const WCHAR *list)
{
    static const struct { const WCHAR *name; unsigned bit; } tbl[] = {
        { L"info", SEL_INFO }, { L"mappings", SEL_MAPPINGS },
        { L"negative", SEL_NEGATIVE }, { L"firmware", SEL_FIRMWARE },
        { L"telemetry", SEL_TELEMETRY }, { L"tlb", SEL_TLB },
        { L"dma", SEL_DMA }, { L"pin", SEL_PIN },
    };
    unsigned mask = 0;
    const WCHAR *p = list;

    while (*p) {
        const WCHAR *start = p;
        size_t n, k;
        BOOL matched = FALSE;

        while (*p && *p != L',') {
            p++;
        }
        n = (size_t)(p - start);
        for (k = 0; k < ARRAYSIZE(tbl); k++) {
            if (wcslen(tbl[k].name) == n && wcsncmp(start, tbl[k].name, n) == 0) {
                mask |= tbl[k].bit;
                matched = TRUE;
                break;
            }
        }
        if (!matched) {
            wprintf(L"FAIL: unknown --only selector '%.*s'\n", (int)n, start);
            g_failures++;
        }
        if (*p == L',') {
            p++;
        }
    }
    return mask;
}

// Lightweight device-id probe (no CHECKs) so selector-driven runs can route on
// isRealDevice/deviceId even when the `info` rung is not selected.
static void ProbeDevice(HANDLE h, int *isRealDevice, int *deviceId)
{
    struct tenstorrent_get_device_info arg;
    DWORD info;

    memset(&arg, 0, sizeof(arg));
    arg.in.output_size_bytes = sizeof(arg.out);
    if (TtIoctl(h, IOCTL_TENSTORRENT_GET_DEVICE_INFO, &arg, sizeof(arg),
                sizeof(arg), &info)) {
        *isRealDevice = (arg.out.vendor_id != 0);
        *deviceId = arg.out.device_id;
    } else {
        *isRealDevice = 0;
        *deviceId = 0;
    }
}

// Issue one RESET_DEVICE flavor on `h`; assert the ioctl succeeded and
// out.result==0, and print out.result. Returns the ioctl success.
static BOOL IssueReset(HANDLE h, uint32_t flavor, const char *name)
{
    struct tenstorrent_reset_device rd;
    DWORD info;
    BOOL ok;

    memset(&rd, 0, sizeof(rd));
    rd.in.output_size_bytes = sizeof(rd.out);
    rd.in.flags = flavor;
    ok = TtIoctl(h, IOCTL_TENSTORRENT_RESET_DEVICE, &rd, sizeof(rd), sizeof(rd), &info);
    CHECK(ok, "reset(%s) ioctl gle=%lu\n", name, GetLastError());
    CHECK(ok && rd.out.result == 0, "reset(%s) out.result=%u\n", name, rd.out.result);
    printf("reset: %s out.result=%u\n", name, ok ? rd.out.result : (uint32_t)-1);
    return ok;
}

// DESTRUCTIVE: open a fresh handle, issue the requested RESET_DEVICE flavor,
// print out.result, then run the heartbeat health check (#2/#5) on that handle.
// `restore-then-post` issues RESTORE_STATE then POST_RESET.
static void DoResetFlavor(const WCHAR *path, const WCHAR *flavor)
{
    HANDLE h;
    BOOL alive;
    // Only flavors that leave the chip live and OUT of the reset window can
    // be health-asserted: user/asic/asic-dmc set needs_hw_init (telemetry is
    // gated NOT_READY until --reset post), config-write leaves the chip mid-
    // reset, and pcie-link tears down the device stack entirely. For those
    // the heartbeat result is informational, not a failure.
    BOOL expectLive = (wcscmp(flavor, L"restore") == 0 ||
                       wcscmp(flavor, L"post") == 0 ||
                       wcscmp(flavor, L"restore-then-post") == 0);

    h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_EXISTING, 0, NULL);
    CHECK(h != INVALID_HANDLE_VALUE, "reset open gle=%lu\n", GetLastError());
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }

    if (wcscmp(flavor, L"restore") == 0) {
        IssueReset(h, TENSTORRENT_RESET_DEVICE_RESTORE_STATE, "restore");
    } else if (wcscmp(flavor, L"pcie-link") == 0) {
        IssueReset(h, TENSTORRENT_RESET_DEVICE_RESET_PCIE_LINK, "pcie-link");
    } else if (wcscmp(flavor, L"config-write") == 0) {
        IssueReset(h, TENSTORRENT_RESET_DEVICE_CONFIG_WRITE, "config-write");
    } else if (wcscmp(flavor, L"user") == 0) {
        IssueReset(h, TENSTORRENT_RESET_DEVICE_USER_RESET, "user");
    } else if (wcscmp(flavor, L"asic") == 0) {
        IssueReset(h, TENSTORRENT_RESET_DEVICE_ASIC_RESET, "asic");
    } else if (wcscmp(flavor, L"asic-dmc") == 0) {
        IssueReset(h, TENSTORRENT_RESET_DEVICE_ASIC_DMC_RESET, "asic-dmc");
    } else if (wcscmp(flavor, L"post") == 0) {
        IssueReset(h, TENSTORRENT_RESET_DEVICE_POST_RESET, "post");
    } else if (wcscmp(flavor, L"restore-then-post") == 0) {
        IssueReset(h, TENSTORRENT_RESET_DEVICE_RESTORE_STATE, "restore");
        IssueReset(h, TENSTORRENT_RESET_DEVICE_POST_RESET, "post");
    } else {
        g_failures++;
        wprintf(L"FAIL: unknown --reset flavor '%s'\n", flavor);
        CloseHandle(h);
        return;
    }

    // Health check after the reset flavor, on the same fresh handle. Only
    // asserted for flavors that leave the device readable (see expectLive).
    alive = HeartbeatAdvances(h, 1500);
    if (expectLive) {
        CHECK(alive, "reset(%ls) post-reset heartbeat did not advance\n", flavor);
        printf("reset: post-reset health %s\n",
               alive ? "PASS (heartbeat advancing)" : "FAIL (heartbeat stalled)");
    } else {
        printf("reset: post-reset heartbeat %s (informational — this flavor "
               "opens a reset window or tears down the stack; follow with "
               "--reset post, or re-enumerate for pcie-link)\n",
               alive ? "advancing" : "not readable");
    }

    CloseHandle(h);
}

static void Usage(const WCHAR *argv0)
{
    wprintf(L"usage: %s [options]\n", argv0);
    printf(
        "  (no args)         read-only-safe: driver/device info, mappings,\n"
        "                    negative cases, telemetry dump. No resets, no\n"
        "                    TLB/DMA/pin writes. Safe on real silicon.\n"
        "  --only <list>     run only these comma-separated rungs:\n"
        "                      info mappings negative firmware telemetry\n"
        "                      tlb dma pin\n"
        "  --reset <flavor>  DESTRUCTIVE: issue one RESET_DEVICE flavor on a\n"
        "                    fresh handle, print out.result, then a heartbeat\n"
        "                    health check. Flavors:\n"
        "                      restore pcie-link config-write user asic\n"
        "                      asic-dmc post restore-then-post\n"
        "  --all-legacy      reproduce the full legacy ttsim sweep, including\n"
        "                    the DESTRUCTIVE M4 reset (implies --sim-oracle).\n"
        "  --sim-oracle      keep ttsim exact-equality checks (board id, AICLK,\n"
        "                    CSM liveness). Default off = silicon mode.\n"
        "  --soak N          open/alloc/map/pin/close soak, N cycles.\n"
        "  --multiproc P     run P concurrent --soak children (default 200\n"
        "                    cycles each, or --soak N).\n"
        "  --storm N         DESTRUCTIVE reset storm, N iterations.\n"
        "  --help            this text.\n");
}

int wmain(int argc, wchar_t **argv)
{
    WCHAR *list, *path;
    ULONG len = 0;
    int devices = 0;
    int soakCycles = 0;
    int stormIters = 0;
    int multiprocCount = 0;
    unsigned long long childMapHandle = 0;
    BOOL allLegacy = FALSE;
    BOOL haveOnly = FALSE;
    unsigned onlyMask = 0;
    unsigned mask;
    const WCHAR *resetFlavor = NULL;
    int resetDone = 0;
    int argi;
    CONFIGRET cr;

    for (argi = 1; argi < argc; argi++) {
        if (wcscmp(argv[argi], L"--sim-oracle") == 0) {
            g_simOracle = TRUE;
        } else if (wcscmp(argv[argi], L"--all-legacy") == 0) {
            allLegacy = TRUE;
            g_simOracle = TRUE;   // legacy sweep runs the ttsim oracles
        } else if (wcscmp(argv[argi], L"--only") == 0 && argi + 1 < argc) {
            haveOnly = TRUE;
            onlyMask |= ParseSelectors(argv[++argi]);
        } else if (wcscmp(argv[argi], L"--reset") == 0 && argi + 1 < argc) {
            resetFlavor = argv[++argi];
        } else if (wcscmp(argv[argi], L"--soak") == 0 && argi + 1 < argc) {
            soakCycles = _wtoi(argv[++argi]);
        } else if (wcscmp(argv[argi], L"--storm") == 0 && argi + 1 < argc) {
            stormIters = _wtoi(argv[++argi]);
        } else if (wcscmp(argv[argi], L"--multiproc") == 0 && argi + 1 < argc) {
            multiprocCount = _wtoi(argv[++argi]);
        } else if (wcscmp(argv[argi], L"--child-map") == 0 && argi + 1 < argc) {
            childMapHandle = _wcstoui64(argv[++argi], NULL, 10);
        } else if (wcscmp(argv[argi], L"--help") == 0 ||
                   wcscmp(argv[argi], L"-h") == 0 ||
                   wcscmp(argv[argi], L"/?") == 0) {
            Usage(argv[0]);
            return 0;
        } else {
            wprintf(L"FAIL: unrecognized argument '%s'\n", argv[argi]);
            Usage(argv[0]);
            return 1;
        }
    }

    if (childMapHandle != 0) {
        return RunChildMap(childMapHandle);
    }
    if (multiprocCount > 0) {
        return RunMultiproc(multiprocCount, soakCycles > 0 ? soakCycles : 200);
    }

    // Effective selector set: explicit --only, else the read-only-safe default.
    mask = haveOnly ? onlyMask : SEL_DEFAULT;

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

        if (resetFlavor != NULL) {
            // DESTRUCTIVE resets run only against the real PCI device.
            CloseHandle(h);
            if (wcsstr(path, L"PCI#VEN_1E52") != NULL) {
                DoResetFlavor(path, resetFlavor);
                resetDone = 1;
            }
            continue;
        }

        if (allLegacy) {
            // Full legacy sweep (ttsim QEMU rig regression), same order as before
            // the split; TestM3{Tlb,Dma,Pin} together == the old TestM3Memory.
            TestDriverInfo(h);
            TestDeviceInfo(h, &isRealDevice, &deviceId);
            TestQueryMappings(h, isRealDevice);
            TestNegative(h);
            TestM2Firmware(h, deviceId);
            TestM3Tlb(h, deviceId);
            TestM3Dma(h, deviceId);
            TestM3DmaQuota(h, deviceId);
            TestM3Pin(h, deviceId);
            TestProcessGuard(path);
            TestM5(path, h, deviceId);
            CloseHandle(h);
            if (isRealDevice) {
                TestM4Reset(path, deviceId);
            }
            continue;
        }

        // Selector-driven run (read-only default set, or explicit --only).
        ProbeDevice(h, &isRealDevice, &deviceId);
        if (mask & SEL_INFO) {
            TestDriverInfo(h);
            TestDeviceInfo(h, &isRealDevice, &deviceId);
        }
        if (mask & SEL_MAPPINGS) {
            TestQueryMappings(h, isRealDevice);
        }
        if (mask & SEL_NEGATIVE) {
            TestNegative(h);
        }
        if (mask & SEL_FIRMWARE) {
            TestM2Firmware(h, deviceId);
        }
        if (mask & SEL_TELEMETRY) {
            TestTelemetryDump(h, deviceId);
        }
        if (mask & SEL_TLB) {
            TestM3Tlb(h, deviceId);
        }
        if (mask & SEL_DMA) {
            TestM3Dma(h, deviceId);
            TestM3DmaQuota(h, deviceId);
        }
        if (mask & SEL_PIN) {
            TestM3Pin(h, deviceId);
            TestProcessGuard(path);
        }
        CloseHandle(h);
    }

    if (soakCycles > 0 || stormIters > 0) {
        printf("no PCI device found\n");
        return 2;
    }
    if (resetFlavor != NULL && !resetDone) {
        printf("note: --reset requested but no real PCI (VEN_1E52) device found\n");
    }

    if (!allLegacy && !haveOnly && resetFlavor == NULL) {
        printf("\nnotice: bare run is read-only-safe. Deeper rungs need --only\n"
               "        <list> (e.g. tlb,dma,pin) or --reset <flavor> (DESTRUCTIVE);\n"
               "        --all-legacy runs the full ttsim sweep. See --help.\n");
    }

    printf("\nttinfo: %d device(s), %d failure(s)\n", devices, g_failures);
    return g_failures == 0 ? 0 : 1;
}
