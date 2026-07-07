/*
 * ttsim-bh: virtual Tenstorrent Blackhole PCIe endpoint backed by libttsim.so
 *
 * Maps to: DD-5 in tt-win-kmd/docs/design-decisions.md. QEMU owns PCI config
 * space (libttsim config writes are fatal, ttsim/docs/libttsim_api.md); the
 * library provides BAR contents, device-initiated DMA, and simulated time.
 *
 * BAR layout (ttsim/src/libttsim.cpp:12-22, TT_ARCH_VERSION==1 / Blackhole):
 *   BAR0 512 MiB @ internal 0x1_0000_0000 (TLB windows + NOC2AXI + ARC CSM/APB)
 *   BAR2   1 MiB @ internal 0x1_2000_0000 (DWC PCIe ctrl; iATU at +0x1000)
 *   BAR4  32 GiB @ internal 0x8_0000_0000 (4 GiB TLB windows)
 * BAR4 is advertised to the guest as `bar4-size` (default 4 GiB = one window;
 * tt-kmd clamps tlb_counts[1] = bar4_len / 4G, analysis §07) to stay inside
 * OVMF's 64-bit MMIO aperture. Sizes are verified against the library's BAR
 * registers at realize.
 *
 * The library is single-threaded and non-reentrant: every call here runs under
 * the BQL (MMIO handlers and QEMU_CLOCK_VIRTUAL timers both hold it).
 *
 * No interrupts: tt-kmd is 100% polling-driven (analysis §02 — the ISR is a
 * stub), so no MSI capability is exposed.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/timer.h"
#include "qemu/units.h"
#include "hw/pci/pci_device.h"
#include "hw/qdev-properties.h"
#include <dlfcn.h>

#define TYPE_TTSIM_BH "ttsim-bh"
OBJECT_DECLARE_SIMPLE_TYPE(TtsimDev, TTSIM_BH)

#define TT_VENDOR_ID 0x1e52
#define TT_DEVICE_ID_BLACKHOLE 0xb140

#define TTSIM_BAR0_SIZE (512 * MiB)
#define TTSIM_BAR2_SIZE (1 * MiB)
#define TTSIM_BAR4_BACKING (32ull * GiB)

typedef struct TtsimBarWindow {
    struct TtsimDev *dev;
    MemoryRegion region;
    uint64_t internal_base;
} TtsimBarWindow;

struct TtsimDev {
    PCIDevice parent_obj;

    char *lib_path;
    uint64_t bar4_size;
    uint32_t clocks_per_tick;
    uint32_t tick_ms;

    void *dl;
    void (*fn_init)(void);
    void (*fn_exit)(void);
    void (*fn_set_dma_cb)(void (*rd)(uint64_t, void *, uint32_t),
                          void (*wr)(uint64_t, const void *, uint32_t));
    uint32_t (*fn_cfg_rd32)(uint32_t bdf, uint32_t offset);
    void (*fn_mem_rd)(uint64_t paddr, void *dst, uint32_t size);
    void (*fn_mem_wr)(uint64_t paddr, const void *src, uint32_t size);
    void (*fn_clock)(uint32_t n);

    TtsimBarWindow bar[3]; /* BAR0, BAR2, BAR4 */
    QEMUTimer *tick;
};

/* libttsim takes bare function pointers and is a process singleton; route DMA
 * through the singleton instance. */
static TtsimDev *ttsim_singleton;

static void ttsim_dma_rd(uint64_t paddr, void *dst, uint32_t size)
{
    pci_dma_read(PCI_DEVICE(ttsim_singleton), paddr, dst, size);
}

static void ttsim_dma_wr(uint64_t paddr, const void *src, uint32_t size)
{
    pci_dma_write(PCI_DEVICE(ttsim_singleton), paddr, (void *)src, size);
}

static uint64_t ttsim_bar_read(void *opaque, hwaddr addr, unsigned size)
{
    TtsimBarWindow *w = opaque;
    uint64_t val = 0;

    w->dev->fn_mem_rd(w->internal_base + addr, &val, size);
    return val;
}

static void ttsim_bar_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    TtsimBarWindow *w = opaque;

    w->dev->fn_mem_wr(w->internal_base + addr, &val, size);
}

static const MemoryRegionOps ttsim_bar_ops = {
    .read = ttsim_bar_read,
    .write = ttsim_bar_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 8 },
};

static void ttsim_tick(void *opaque)
{
    TtsimDev *d = opaque;

    d->fn_clock(d->clocks_per_tick);
    timer_mod(d->tick,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + d->tick_ms);
}

static void *ttsim_sym(TtsimDev *d, const char *name, Error **errp)
{
    void *p = dlsym(d->dl, name);

    if (!p) {
        error_setg(errp, "ttsim-bh: %s not found in %s", name, d->lib_path);
    }
    return p;
}

static uint64_t ttsim_cfg_bar64(TtsimDev *d, uint32_t off)
{
    uint64_t lo = d->fn_cfg_rd32(0, off);
    uint64_t hi = d->fn_cfg_rd32(0, off + 4);

    return (hi << 32) | (lo & ~0xfull);
}

static void ttsim_realize(PCIDevice *pci_dev, Error **errp)
{
    TtsimDev *d = TTSIM_BH(pci_dev);
    static const uint32_t bar_cfg_off[3] = { 0x10, 0x18, 0x20 };
    static const int bar_num[3] = { 0, 2, 4 };
    uint64_t bar_size[3];
    uint32_t id;
    int i;

    if (ttsim_singleton) {
        error_setg(errp, "ttsim-bh: libttsim is a process singleton; "
                   "only one device instance is supported");
        return;
    }
    if (!d->lib_path) {
        error_setg(errp, "ttsim-bh: 'ttsim-lib' property is required");
        return;
    }
    if (d->bar4_size < 4ull * GiB || d->bar4_size > TTSIM_BAR4_BACKING ||
        !is_power_of_2(d->bar4_size)) {
        error_setg(errp, "ttsim-bh: bar4-size must be a power of two in "
                   "[4GiB, 32GiB]");
        return;
    }

    d->dl = dlopen(d->lib_path, RTLD_NOW | RTLD_LOCAL);
    if (!d->dl) {
        error_setg(errp, "ttsim-bh: dlopen(%s): %s", d->lib_path, dlerror());
        return;
    }

    if (!(d->fn_init = ttsim_sym(d, "libttsim_init", errp)) ||
        !(d->fn_exit = ttsim_sym(d, "libttsim_exit", errp)) ||
        !(d->fn_set_dma_cb = ttsim_sym(d, "libttsim_set_pci_dma_mem_callbacks", errp)) ||
        !(d->fn_cfg_rd32 = ttsim_sym(d, "libttsim_pci_config_rd32", errp)) ||
        !(d->fn_mem_rd = ttsim_sym(d, "libttsim_pci_mem_rd_bytes", errp)) ||
        !(d->fn_mem_wr = ttsim_sym(d, "libttsim_pci_mem_wr_bytes", errp)) ||
        !(d->fn_clock = ttsim_sym(d, "libttsim_clock", errp))) {
        goto fail_dl;
    }

    ttsim_singleton = d;
    d->fn_set_dma_cb(ttsim_dma_rd, ttsim_dma_wr); /* before init, per API doc */
    d->fn_init();

    /* No fabrication: identity comes from the library, not this file. */
    id = d->fn_cfg_rd32(0, 0);
    if ((id & 0xffff) != TT_VENDOR_ID ||
        (id >> 16) != TT_DEVICE_ID_BLACKHOLE) {
        error_setg(errp, "ttsim-bh: library reports %04x:%04x, expected "
                   "%04x:%04x (wrong libttsim build?)", id & 0xffff, id >> 16,
                   TT_VENDOR_ID, TT_DEVICE_ID_BLACKHOLE);
        goto fail_lib;
    }

    /* Mirror revision/class and subsystem IDs from the library. */
    pci_set_long(pci_dev->config + PCI_REVISION_ID,
                 d->fn_cfg_rd32(0, PCI_REVISION_ID));
    pci_set_long(pci_dev->config + PCI_SUBSYSTEM_VENDOR_ID,
                 d->fn_cfg_rd32(0, PCI_SUBSYSTEM_VENDOR_ID));

    bar_size[0] = TTSIM_BAR0_SIZE;
    bar_size[1] = TTSIM_BAR2_SIZE;
    bar_size[2] = d->bar4_size;

    for (i = 0; i < 3; i++) {
        TtsimBarWindow *w = &d->bar[i];
        g_autofree char *name = g_strdup_printf("ttsim-bar%d", bar_num[i]);

        w->dev = d;
        w->internal_base = ttsim_cfg_bar64(d, bar_cfg_off[i]);
        if (w->internal_base == 0 || w->internal_base == ~0xfull) {
            error_setg(errp, "ttsim-bh: implausible internal BAR%d base "
                       "0x%" PRIx64, bar_num[i], w->internal_base);
            goto fail_lib;
        }
        memory_region_init_io(&w->region, OBJECT(d), &ttsim_bar_ops, w,
                              name, bar_size[i]);
        pci_register_bar(pci_dev, bar_num[i],
                         PCI_BASE_ADDRESS_SPACE_MEMORY |
                         PCI_BASE_ADDRESS_MEM_TYPE_64 |
                         (bar_num[i] == 2 ? 0 : PCI_BASE_ADDRESS_MEM_PREFETCH),
                         &w->region);
    }

    d->tick = timer_new_ms(QEMU_CLOCK_VIRTUAL, ttsim_tick, d);
    timer_mod(d->tick, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + d->tick_ms);
    return;

fail_lib:
    d->fn_exit();
    ttsim_singleton = NULL;
fail_dl:
    dlclose(d->dl);
    d->dl = NULL;
}

static void ttsim_exit_fn(PCIDevice *pci_dev)
{
    TtsimDev *d = TTSIM_BH(pci_dev);

    if (d->tick) {
        timer_free(d->tick);
        d->tick = NULL;
    }
    if (d->dl) {
        d->fn_exit();
        dlclose(d->dl);
        d->dl = NULL;
    }
    if (ttsim_singleton == d) {
        ttsim_singleton = NULL;
    }
}

static Property ttsim_props[] = {
    DEFINE_PROP_STRING("ttsim-lib", TtsimDev, lib_path),
    DEFINE_PROP_SIZE("bar4-size", TtsimDev, bar4_size, 4ull * GiB),
    DEFINE_PROP_UINT32("clocks-per-tick", TtsimDev, clocks_per_tick, 100000),
    DEFINE_PROP_UINT32("tick-ms", TtsimDev, tick_ms, 1),
    DEFINE_PROP_END_OF_LIST(),
};

static void ttsim_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = ttsim_realize;
    k->exit = ttsim_exit_fn;
    k->vendor_id = TT_VENDOR_ID;
    k->device_id = TT_DEVICE_ID_BLACKHOLE;
    k->class_id = PCI_CLASS_PROCESSOR_CO;
    dc->desc = "Tenstorrent Blackhole (ttsim-backed)";
    device_class_set_props(dc, ttsim_props);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo ttsim_info = {
    .name = TYPE_TTSIM_BH,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(TtsimDev),
    .class_init = ttsim_class_init,
    .interfaces = (InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void ttsim_register_types(void)
{
    type_register_static(&ttsim_info);
}

type_init(ttsim_register_types);
