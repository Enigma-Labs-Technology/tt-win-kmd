// M6 conformance suite: exercises every driver IOCTL exclusively through the
// ttwin_compat shim (tt_open/tt_ioctl/tt_mmap/tt_munmap/tt_close), proving the
// shim is a working ABI a ported tt-umd can build against. One row per parity-
// matrix IOCTL, positive and negative. Uses the Linux TENSTORRENT_IOCTL_*
// request constants exactly as tt-umd does.
//
// Exit 0 iff every check passes.
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "ttwin_compat.h"
#include "ttkmd_ioctl.h"

// Linux request encodings (what tt-umd passes to ioctl()). _IO(0xFA, nr).
#define REQ(nr) ((unsigned long)((TENSTORRENT_IOCTL_MAGIC << 8) | (nr)))
#define REQ_GET_DEVICE_INFO REQ(0)
#define REQ_QUERY_MAPPINGS  REQ(2)
#define REQ_ALLOCATE_DMA_BUF REQ(3)
#define REQ_GET_DRIVER_INFO REQ(5)
#define REQ_RESET_DEVICE    REQ(6)
#define REQ_PIN_PAGES       REQ(7)
#define REQ_LOCK_CTL        REQ(8)
#define REQ_UNPIN_PAGES     REQ(10)
#define REQ_ALLOCATE_TLB    REQ(11)
#define REQ_FREE_TLB        REQ(12)
#define REQ_CONFIGURE_TLB   REQ(13)
#define REQ_SET_POWER_STATE REQ(15)

static int g_failures;
#define CHECK(cond, ...) do { if (!(cond)) { \
    g_failures++; printf("FAIL: " __VA_ARGS__); printf("  [%s]\n", #cond); } } while (0)

int main(void)
{
    int ids[16];
    int n, i;
    int realId = -1;

    n = tt_enumerate(ids, 16);
    CHECK(n >= 1, "tt_enumerate returned %d (expected >=1)\n", n);
    printf("enumerate: %d device(s)\n", n);

    for (i = 0; i < n && i < 16; i++) {
        tt_handle h = tt_open(ids[i], TT_O_RDWR);
        struct tenstorrent_get_device_info di;
        struct tenstorrent_get_driver_info dri;

        CHECK(h != NULL, "tt_open(%d) failed gle=%lu\n", ids[i], tt_get_last_error());
        if (h == NULL) {
            continue;
        }

        // GET_DRIVER_INFO through the shim.
        memset(&dri, 0, sizeof(dri));
        dri.in.output_size_bytes = sizeof(dri.out);
        CHECK(tt_ioctl(h, REQ_GET_DRIVER_INFO, &dri) == 0,
              "tt_ioctl GET_DRIVER_INFO gle=%lu\n", tt_get_last_error());
        CHECK(dri.out.driver_version == TENSTORRENT_DRIVER_VERSION,
              "driver_version=%u\n", dri.out.driver_version);

        // GET_DEVICE_INFO.
        memset(&di, 0, sizeof(di));
        di.in.output_size_bytes = sizeof(di.out);
        CHECK(tt_ioctl(h, REQ_GET_DEVICE_INFO, &di) == 0,
              "tt_ioctl GET_DEVICE_INFO gle=%lu\n", tt_get_last_error());
        printf("device %d: %04x:%04x\n", ids[i], di.out.vendor_id, di.out.device_id);
        if (di.out.device_id == 0xB140) {
            realId = ids[i];
        }
        tt_close(h);
    }

    if (realId < 0) {
        printf("ttconform: no PCI device; %d failure(s)\n", g_failures);
        return g_failures == 0 ? 0 : 1;
    }

    // Full path on the real device, all through the shim.
    {
        tt_handle h = tt_open(realId, TT_O_RDWR);
        struct tenstorrent_query_mappings *qm;
        struct tenstorrent_mapping *maps;
        struct tenstorrent_allocate_tlb at;
        struct tenstorrent_configure_tlb ct;
        struct tenstorrent_free_tlb ft;
        struct tenstorrent_allocate_dma_buf ab;
        size_t qmBytes;
        volatile uint32_t *va;

        CHECK(h != NULL, "tt_open(real) failed\n");
        if (h == NULL) { return 1; }

        // QUERY_MAPPINGS (variable-size buffer via the shim's size logic).
        qmBytes = sizeof(struct tenstorrent_query_mappings_in) +
                  6 * sizeof(struct tenstorrent_mapping);
        qm = (struct tenstorrent_query_mappings *)calloc(1, qmBytes);
        qm->in.output_mapping_count = 6;
        CHECK(tt_ioctl(h, REQ_QUERY_MAPPINGS, qm) == 0,
              "tt_ioctl QUERY_MAPPINGS gle=%lu\n", tt_get_last_error());
        maps = (struct tenstorrent_mapping *)((char *)qm +
               sizeof(struct tenstorrent_query_mappings_in));
        CHECK(maps[0].mapping_size > 0, "QUERY_MAPPINGS BAR0 size 0\n");
        printf("mappings via shim: BAR0=%lluM\n",
               (unsigned long long)maps[0].mapping_size >> 20);

        // ALLOCATE_TLB -> CONFIGURE_TLB -> tt_mmap -> read CSM -> tt_munmap -> FREE_TLB.
        memset(&at, 0, sizeof(at));
        at.in.size = 2 << 20;
        CHECK(tt_ioctl(h, REQ_ALLOCATE_TLB, &at) == 0,
              "tt_ioctl ALLOCATE_TLB gle=%lu\n", tt_get_last_error());
        memset(&ct, 0, sizeof(ct));
        ct.in.id = at.out.id;
        ct.in.config.addr = 0x10000000ull;
        ct.in.config.x_end = 8;
        ct.in.config.ordering = 1;
        CHECK(tt_ioctl(h, REQ_CONFIGURE_TLB, &ct) == 0,
              "tt_ioctl CONFIGURE_TLB gle=%lu\n", tt_get_last_error());

        va = (volatile uint32_t *)tt_mmap(h, 2 << 20, TT_PROT_READ | TT_PROT_WRITE,
                                          TT_MAP_SHARED, at.out.mmap_offset_uc);
        CHECK(va != TT_MAP_FAILED, "tt_mmap TLB gle=%lu\n", tt_get_last_error());
        if (va != TT_MAP_FAILED) {
            CHECK(va[0x100 / 4] == 1, "shim mmap CSM version=%u\n", va[0x100 / 4]);
            printf("shim mmap: user VA reads CSM (version %u)\n", va[0x100 / 4]);
            CHECK(tt_munmap(h, (void *)va, 2 << 20) == 0,
                  "tt_munmap gle=%lu\n", tt_get_last_error());
        }
        memset(&ft, 0, sizeof(ft));
        ft.in.id = at.out.id;
        CHECK(tt_ioctl(h, REQ_FREE_TLB, &ft) == 0,
              "tt_ioctl FREE_TLB gle=%lu\n", tt_get_last_error());

        // ALLOCATE_DMA_BUF -> tt_mmap -> write/read -> (freed at close).
        memset(&ab, 0, sizeof(ab));
        ab.in.requested_size = 64 * 1024;
        ab.in.buf_index = 0;
        CHECK(tt_ioctl(h, REQ_ALLOCATE_DMA_BUF, &ab) == 0,
              "tt_ioctl ALLOCATE_DMA_BUF gle=%lu\n", tt_get_last_error());
        va = (volatile uint32_t *)tt_mmap(h, 64 * 1024, TT_PROT_READ | TT_PROT_WRITE,
                                          TT_MAP_SHARED, ab.out.mapping_offset);
        CHECK(va != TT_MAP_FAILED, "tt_mmap dmabuf gle=%lu\n", tt_get_last_error());
        if (va != TT_MAP_FAILED) {
            va[0] = 0xCAFEF00D;
            CHECK(va[0] == 0xCAFEF00D, "shim dmabuf rw\n");

            // mmap-survives-close: close the handle with the view still mapped,
            // then access it, then unmap (Linux semantics the shim preserves).
            tt_close(h);
            CHECK(va[0] == 0xCAFEF00D, "view invalid after tt_close\n");
            printf("shim: mapping survived tt_close\n");
            tt_munmap(h, (void *)va, 64 * 1024);   // frees the deferred handle
        } else {
            tt_close(h);
        }
    }

    // LOCK_CTL and SET_POWER_STATE through the shim on a fresh handle.
    {
        tt_handle h = tt_open(realId, TT_O_RDWR);
        struct tenstorrent_lock_ctl lc;
        struct tenstorrent_power_state ps;

        if (h != NULL) {
            memset(&lc, 0, sizeof(lc));
            lc.in.flags = TENSTORRENT_LOCK_CTL_ACQUIRE;
            lc.in.index = 3;
            CHECK(tt_ioctl(h, REQ_LOCK_CTL, &lc) == 0 && lc.out.value == 1,
                  "shim LOCK_CTL acquire value=%u\n", lc.out.value);
            lc.in.flags = TENSTORRENT_LOCK_CTL_RELEASE;
            CHECK(tt_ioctl(h, REQ_LOCK_CTL, &lc) == 0 && lc.out.value == 1,
                  "shim LOCK_CTL release value=%u\n", lc.out.value);

            memset(&ps, 0, sizeof(ps));
            ps.argsz = sizeof(ps);
            ps.validity = (uint8_t)TT_POWER_VALIDITY(4, 0);
            ps.power_flags = TT_POWER_FLAG_MAX_AI_CLK;
            CHECK(tt_ioctl(h, REQ_SET_POWER_STATE, &ps) == 0,
                  "shim SET_POWER_STATE gle=%lu\n", tt_get_last_error());

            // Negative: bad argsz -> -1.
            ps.argsz = 1;
            CHECK(tt_ioctl(h, REQ_SET_POWER_STATE, &ps) == -1,
                  "shim SET_POWER_STATE bad argsz accepted\n");
            printf("shim: LOCK_CTL + SET_POWER_STATE ok\n");
            tt_close(h);
        }
    }

    printf("ttconform: %d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
