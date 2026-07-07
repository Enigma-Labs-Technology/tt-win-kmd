# 02. Enumeration and Lifecycle

## Scope

Assigned files (read in full):

| File | Lines |
|---|---|
| `enumerate.c` | 545 |
| `enumerate.h` | 29 |
| `interrupt.c` | 46 |
| `interrupt.h` | 14 |

Supporting files read for cross-references (probe calls into all of these): `module.c` (121), `module.h` (38), `chardev.c` (966), `device.h` (127), `pcie.c` (158), `telemetry.c` (232), `telemetry.h` (81), `wormhole.c` (1084), `wormhole.h` (32), `blackhole.c` (840), `blackhole.h` (28), `chardev_private.h` (82), `memory.c` (excerpt, `is_iommu_translated`), `udev-50-tenstorrent.rules` (3).

All paths below are relative to the tt-kmd repo root.

---

## 1. Module-level context (what exists before probe runs)

`ttdriver_init()` (module.c:76-108) runs in this order, with reverse-order unwind on failure:

1. `debugfs_create_dir("tenstorrent", NULL)` → global `tt_debugfs_root` (module.c:82). Failure is tolerated (debugfs API returns error pointers that are safely ignored downstream; chardev.c:109 checks `if (tt_debugfs_root)`).
2. `proc_mkdir("driver/tenstorrent", NULL)` → global `tt_procfs_root`; NULL → `-ENOMEM` and unwind (module.c:84-88).
3. `init_char_driver(max_devices)` (module.c:90): `alloc_chrdev_region(&tt_device_id, 0, max_devices, "tenstorrent")` then `class_create("tenstorrent")` (chardev.c:48-75). `max_devices` is a module parameter, default **32** (module.c:36-38).
4. `tenstorrent_pci_register_driver()` → `pci_register_driver(&tenstorrent_pci_driver)` (enumerate.c:537-540).

Module parameters consumed by the lifecycle paths (module.c:36-62):

| Param | Type/default | Meaning |
|---|---|---|
| `max_devices` | uint, 32 | count of chrdev minors reserved (module.c:36-38) |
| `dma_address_bits` | uint, 0 | DMA address bits, "0 for automatic" (module.c:40-42) |
| `reset_limit` | uint, 10 | max boot-time resets in WH PCIe init retry loop (module.c:44-46, pcie.c:98-128) |
| `auto_reset_timeout` | byte, 10 | seconds for M3 auto-reset / ARC watchdog (module.c:48-50) |
| `power_policy` | bool, true | low power at probe, re-aggregate on close (module.c:52-54) |
| `idle_power_down_grace_ms` | uint, 5000, mode **0644** (writable at runtime) | delay from last close to idle power-down message (module.c:56-62) |

The PCI ID table (module.c:64-72):

```c
{ PCI_DEVICE(PCI_VENDOR_ID_TENSTORRENT, PCI_DEVICE_ID_GRAYSKULL),
  .driver_data=(kernel_ulong_t)NULL}, // Deprecated
{ PCI_DEVICE(PCI_VENDOR_ID_TENSTORRENT, PCI_DEVICE_ID_WORMHOLE),
  .driver_data=(kernel_ulong_t)&wormhole_class },
{ PCI_DEVICE(PCI_VENDOR_ID_TENSTORRENT, PCI_DEVICE_ID_BLACKHOLE),
  .driver_data=(kernel_ulong_t)&blackhole_class },
```

Vendor/device IDs: vendor `0x1E52`, Grayskull `0xFACA`, Wormhole `0x401E`, Blackhole `0xB140` (enumerate.h:15-18). **Grayskull is bound by the ID table but has NULL `driver_data`, so probe rejects it with `-ENODEV`** ("Unsupported device", enumerate.c:261-264). There is no grayskull ops table anywhere in the tree; only `wormhole_class` and `blackhole_class` exist (module.h:31-32).

The pci_driver structure (enumerate.c:527-535) sets `.probe`, `.remove`, and **`.shutdown = tenstorrent_pci_remove`** — the shutdown path is literally the remove function — plus `.driver.pm = &tenstorrent_pm_ops` (SIMPLE_DEV_PM_OPS, enumerate.c:524).

> **Porting note:** `.shutdown == .remove` means a Windows port should run the identical teardown from `EvtDeviceShutdown`/`IRP_MJ_SHUTDOWN` handling as from device removal — including putting firmware into the A3 (low-power) state — not a reduced "fast shutdown" path.

---

## 2. Global device collection and ordinal allocation

The only global device collection is an allocating XArray:

```c
static DEFINE_XARRAY_ALLOC(tenstorrent_dev_xa);
```
(enumerate.c:33). Locking is the XArray's internal spinlock; the driver never iterates or looks up this collection (no `xa_load`/`xa_for_each` anywhere) — it is used purely as an ordinal (index) allocator; the stored `tt_dev` pointers are never read back. Entries are inserted in probe (enumerate.c:287-294) and erased in remove (enumerate.c:478) and on the probe failure path (enumerate.c:398).

### 2.1 Galaxy static ordinals

Galaxy systems (32 chips: 4 UBB boards × 8 chips) get deterministic ordinals derived from the PCI bus number (enumerate.c:35-76):

- `GALAXY_CHIPS_PER_UBB 8`, `GALAXY_NUM_UBBS 4` (enumerate.c:39-40).
- Subsystem IDs select the mapping table: `PCI_SUBSYSTEM_ID_GALAXY_WH 0x0035`, `PCI_SUBSYSTEM_ID_GALAXY_BH 0x0047` (enumerate.c:42-43).
- Bus-number high nibble → UBB index tables:
  ```c
  static const u8 wh_galaxy_ubb_bus_prefix[GALAXY_NUM_UBBS] = { 0xC, 0x8, 0x0, 0x4 };
  static const u8 bh_galaxy_ubb_bus_prefix[GALAXY_NUM_UBBS] = { 0x0, 0x4, 0xC, 0x8 };
  ```
  (enumerate.c:45-46).
- Low nibble is the 1-based chip index and must be in 1..8 (enumerate.c:67-68); ordinal = `ubb * 8 + (low - 1)` (enumerate.c:72). Any mismatch returns `-1` = "not a Galaxy device".

Note the mapping uses only `pdev->bus->number` — the PCI **domain (segment) is ignored** (enumerate.c:51).

### 2.2 Allocation in probe

```c
galaxy_ord = galaxy_bdf_to_ordinal(dev);
if (galaxy_ord >= 0) {
        ordinal = galaxy_ord;
        err = xa_insert(&tenstorrent_dev_xa, ordinal, tt_dev, GFP_KERNEL);
        if (err == -EBUSY) { ... err = xa_alloc(..., xa_limit_31b, GFP_KERNEL); }
} else {
        err = xa_alloc(&tenstorrent_dev_xa, &ordinal, tt_dev, xa_limit_31b, GFP_KERNEL);
}
```
(enumerate.c:284-294). So: Galaxy chips try their fixed ordinal first and fall back to dynamic allocation with a warning if it is taken (enumerate.c:288-291); everything else takes the lowest free index in `[0, INT_MAX]` (`xa_limit_31b`). Remove erases the entry with the comment "If this is postponed, a subsequent probe is forced to use a different ordinal" (enumerate.c:477-478) — i.e. ordinals are reused as soon as the old device is gone.

The ordinal determines:
- the char device minor: `MKDEV(MAJOR(tt_device_id), MINOR(tt_device_id) + tt_dev->ordinal)` (chardev.c:85-88);
- the device node name `/dev/tenstorrent/<ordinal>` via `dev_set_name(&tt_dev->dev, TENSTORRENT "/%d", ...)` (chardev.c:106);
- debugfs/procfs directory names (chardev.c:108-113);
- the "PCIe index" the wormhole driver reports to firmware (`WH_FW_MSG_PCIE_INDEX` with `ordinal | 0x80`, wormhole.c:464-471).

> **Porting note:** UMD discovers devices by scanning `/dev/tenstorrent/*` names; the ordinal is a stable userspace contract. A KMDF port must expose an equivalent stable per-device ordinal (e.g. as a device-interface reference string or queryable property), keep the Galaxy bus-nibble mapping for multi-board boxes, and reuse freed ordinals.

Note a latent mismatch: the chrdev region reserves only `max_devices` (default 32) minors (chardev.c:55), but `xa_alloc` may produce ordinals ≥ `max_devices` (limit is 2^31-1), and `devt_for_device` adds the ordinal to the base minor unconditionally (chardev.c:85-88). Nothing clamps the ordinal to `max_devices`. See Open questions.

---

## 3. Probe sequence (`tenstorrent_pci_probe`, enumerate.c:253-402)

Ordered, with validation/error behavior per step. `tt_dev` is a `struct tenstorrent_device` embedded at the head of a per-class instance struct (`wormhole_device` / `blackhole_device`; wormhole.h:10-24, blackhole.h:10-23).

1. **Class dispatch check.** `id->driver_data == NULL` → `dev_warn "Unsupported device"`, return `-ENODEV` (enumerate.c:261-264). Otherwise `device_class = (const struct tenstorrent_device_class *)id->driver_data` (enumerate.c:266) and an info log "Found a Tenstorrent %s device" (enumerate.c:268).

2. **Class-code fix-up for unflashed boards.** If `dev->class >> 8 == PCI_CLASS_NOT_DEFINED`, the driver writes `dev->class = 0x120000` ("Processing Accelerator - vendor-specific interface") and calls `pci_assign_unassigned_bus_resources(dev->bus)` to redo resource assignment (enumerate.c:270-275). This exists because pre-production boards without flashed class codes trip up `__dev_sort_resources`.

3. **`pci_enable_device(dev)`** — failure returns `-EIO` (enumerate.c:277-278).

4. **Allocate device struct**: `kzalloc(device_class->instance_size, GFP_KERNEL)`; NULL → return `-ENOMEM` (enumerate.c:280-282). `instance_size` is `sizeof(struct wormhole_device)` / `sizeof(struct blackhole_device)` (wormhole.c:1057, blackhole.c:815). **This failure path does not call `pci_disable_device`** — the device is left enabled (contrast with step 5's unwind at enumerate.c:297).

5. **Ordinal allocation** as described in §2.2. On error: `kfree(tt_dev); pci_disable_device(dev); return err;` (enumerate.c:295-299).

6. **Core field initialization** (enumerate.c:301-320):
   - `kref_init(&tt_dev->kref)` — "The refcount created here persists until remove" (enumerate.c:301-302).
   - `detached = false`, `needs_hw_init = true`, `dev_class`, `pdev = pci_dev_get(dev)` (takes a pdev reference), `ordinal` (enumerate.c:304-308).
   - `atomic_long_set(&tt_dev->reset_gen, 0)`; `init_rwsem(&tt_dev->reset_rwsem)` (enumerate.c:309-310).
   - `memcpy(tt_dev->tlb_counts, device_class->tlb_counts, ...)` — per-device TLB counts seeded from class defaults; device init may adjust (enumerate.c:312-314; Blackhole trims 4G-window count to BAR4 size at blackhole.c:579-580).
   - `mutex_init` on `chardev_mutex`, `iatu_mutex`, `dmabuf_export_lock`; `INIT_LIST_HEAD(&tt_dev->dmabuf_exports)`; `INIT_DELAYED_WORK(&tt_dev->power_down_work, tenstorrent_power_down_work_func)` (enumerate.c:316-320).

7. **DMA mask negotiation** (enumerate.c:322-335):
   ```c
   tt_dev->dma_capable = (dma_set_mask(&dev->dev, DMA_BIT_MASK(dma_address_bits ?: 64)) == 0);
   dma_set_coherent_mask(&dev->dev, DMA_BIT_MASK(dma_address_bits ?: device_class->dma_address_bits));
   ```
   - Streaming mask: `dma_address_bits` if the module param is nonzero, else **64 bits**. Failure is **not fatal**; it just clears `dma_capable` (which gates DMA-buffer/pinning ioctls elsewhere).
   - Coherent mask: `dma_address_bits` if nonzero, else the **class default**: Wormhole `dma_address_bits = 32` (wormhole.c:1058), Blackhole `58` (blackhole.c:816). The comment explains why they differ: legacy WH software assumes 32-bit addresses from `ALLOCATE_DMA_BUF`, but a 32-bit streaming mask is too small for user pinnings under IOMMU (enumerate.c:322-329); per dma-mapping.h the coherent mask may always be ≤ the streaming mask, so only `dma_set_mask`'s return value is checked (enumerate.c:327-329).
   - `dma_set_max_seg_size(&dev->dev, UINT_MAX)` and `dma_set_seg_boundary(&dev->dev, ULONG_MAX)`: "Max these to ensure the IOVA allocator will not split large pinned regions" (enumerate.c:334-335).

8. **Thunderbolt/untrusted handling** (kernels ≥ 5.9; enumerate.c:342-350): if `dev->untrusted` and `is_iommu_translated(&dev->dev)` and the root port is `external_facing`, clear `dev->untrusted = 0` to avoid forced SWIOTLB bounce buffering; the IOMMU provides isolation.
   `is_iommu_translated()` is the driver's IOMMU detection primitive (memory.c:526-530):
   ```c
   struct iommu_domain *domain = iommu_get_domain_for_dev(dev);
   return domain && domain->type != IOMMU_DOMAIN_IDENTITY;
   ```
   i.e. "an IOMMU domain exists and is not pass-through".

9. **`pci_set_master(dev)`** then `pci_enable_pcie_error_reporting(dev)` (enumerate.c:352-353). On kernels ≥ 6.0 (or RHEL ≥ 9.4) the AER call is compiled to a no-op because the core owns AER (enumerate.c:26-31).

10. **Galaxy hotplug suppression**: `pci_ignore_hotplug(dev)` when subsystem ID is `0x0035`/`0x0047` (enumerate.c:355-357, explicitly labeled "HACK").

11. **Drvdata**: `pci_set_drvdata(dev, tt_dev)` and `dev_set_drvdata(&tt_dev->dev, tt_dev)` (enumerate.c:359-360) — the latter is what the sysfs show callbacks use (`dev_get_drvdata`, telemetry.c:37).

12. **Interrupts**: `tt_dev->interrupt_enabled = tenstorrent_enable_interrupts(tt_dev)` (enumerate.c:362). **Failure is non-fatal**; probe continues without interrupts. See §5.

13. **Per-chip `init_device` dispatch** (enumerate.c:364-368): `device_class->init_device(tt_dev)`; on `false`, log "Device initialization failed", set `err = -EIO`, and unwind (see §3.1). This is where BAR mapping happens — **the driver never calls `pci_request_regions`/`request_mem_region` anywhere** (verified by grep across the tree); it maps BARs unclaimed:
    - Wormhole `wormhole_init` (wormhole.c:683-716): `devm_kcalloc` of the telemetry attribute array (devm-tied to the *PCI* device, wormhole.c:691), `pci_iomap(pdev, 2, 0)` → `bar2_mapping`, `pci_iomap(pdev, 4, 0)` → `bar4_mapping` (wormhole.c:696-700); on BAR4 failure it unmaps BAR2 and returns false (wormhole.c:712-715). Reserves the kernel TLB (last 16 MB window) via `set_bit(KERNEL_TLB_INDEX, tt_dev->tlbs)` (wormhole.c:702) and fills `telemetry_group.attrs`/`.is_visible` (wormhole.c:705-708). Also `INIT_DELAYED_WORK(&wh_dev->fw_ready_work, fw_ready_work_func)` (wormhole.c:689).
    - Blackhole `blackhole_init` (blackhole.c:572-618): trims `tt_dev->tlb_counts[1]` to `pci_resource_len(pdev, 4) / 4G` (blackhole.c:576-580), then `pci_iomap_range(pdev, 0, TLB_REGS_START=0x1FC00000, 0x1000)`, `pci_iomap_range(pdev, 0, KERNEL_TLB_START, 2M)`, `pci_iomap_range(pdev, 0, NOC2AXI_CFG_START=0x1FD00000, 0x100000)`, `pci_iomap(pdev, 2, 0)` (blackhole.c:587-590). Failure of any of the first three unmaps whatever succeeded and returns false; **a NULL `bar2_mapping` alone is tolerated** (blackhole.c:592-606). Claims kernel TLB (last 2M window) and fills telemetry group (blackhole.c:608-615).

14. **`tt_dev->needs_hw_init = !device_class->init_hardware(tt_dev)`** (enumerate.c:370). Hardware init failure does *not* fail probe; it leaves the device in the "needs init" state in which only GET_DEVICE_INFO/GET_DRIVER_INFO/RESET_DEVICE ioctls are allowed (chardev.c:616-624). Wormhole `init_hardware` programs the iATU so BAR4 hits system registers (`BAR4_SOC_TARGET_ADDRESS 0x1E000000`, wormhole.c:74-76, 450-458, 721) and, if ARC L2 firmware is running, sends the current date, ASTATE0, PCIe index, runs the PCIe link training retry loop, and sets the M3 auto-reset timeout (wormhole.c:723-732); it always returns true (wormhole.c:734). Blackhole `init_hardware` sets MRRS to 4096 (`pcie_set_readrq(pdev, MAX_MRRS)`, blackhole.c:18, 626), sends ARC "ASIC_STATE0" and watchdog-timeout messages (errors only logged), and always returns true (blackhole.c:620-639).

15. **PCI config-space save**: `pci_save_state(dev)` then `device_class->save_reset_state(tt_dev)` (enumerate.c:372-373). The latter saves the Max_Payload_Size field of Device Control read through the chip's own DBI (via NOC), for restoration after hot reset (wormhole.c:968-976; blackhole.c:304-315).

16. **Char device registration**: `tenstorrent_register_device(tt_dev)` (enumerate.c:375). **The int return value is ignored** — if `cdev_device_add` fails, probe still returns 0. Details in §4.

17. **Reboot notifier**: only if `device_class->reboot` is non-NULL (Wormhole only; `wormhole_class.reboot = wormhole_cleanup_hardware`, wormhole.c:1073; blackhole_class has no `.reboot`, blackhole.c:813-840) — `tt_dev->reboot_notifier.notifier_call = tenstorrent_reboot_notifier; register_reboot_notifier(...)` (enumerate.c:377-380). The callback fires on every action **except `SYS_POWER_OFF`** and calls `dev_class->reboot(tt_dev)` (enumerate.c:243-251), i.e. on reboot/halt Wormhole firmware is sent ASTATE3, but on power-off it is not.

18. **Telemetry init**: `if (!tt_dev->needs_hw_init) device_class->init_telemetry(tt_dev)` (enumerate.c:382-383). This creates all sysfs groups and the hwmon device — see §6.

19. **Debugfs**: `debugfs_create_file("mappings", 0444, tt_dev->debugfs_root, tt_dev, &mappings_fops)` (enumerate.c:385).

20. **Initial power state**: `if (power_policy) tenstorrent_set_aggregated_power_state(tt_dev)` (enumerate.c:387-389). With zero open fds the aggregate is "everything off" flags with full validity, sent to firmware (chardev.c:478-545) — i.e. the device is put in low power at probe.

21. Return 0.

### 3.1 Probe failure unwind

Only step 13 (`init_device`) reaches the labeled unwind (enumerate.c:393-401):

```c
fail_init_device:
        tenstorrent_disable_interrupts(tt_dev);
        pci_disable_pcie_error_reporting(dev);
        pci_set_drvdata(dev, NULL);
        pci_dev_put(dev);
        xa_erase(&tenstorrent_dev_xa, ordinal);
        kfree(tt_dev);
        pci_disable_device(dev);
        return err;
```

Bus mastering is *not* cleared on this path (no `pci_clear_master`), and as noted, the `kzalloc` failure path skips `pci_disable_device` entirely.

---

## 4. Char device creation ordering and naming

`tenstorrent_register_device` (chardev.c:90-119), called from probe step 16 — i.e. **after** hardware init and PCI state save, **before** telemetry sysfs and initial power state:

1. `init_waitqueue_head` on `resource_lock_waitqueue` and `chardev_excl_waitqueue` (chardev.c:95-96).
2. `device_initialize(&tt_dev->dev)`; `devt = MKDEV(major, minor_base + ordinal)`; `class = tt_dev_class` ("tenstorrent"); `parent = &tt_dev->pdev->dev`; `groups = NULL`; **`release = NULL`** (chardev.c:98-103).
3. `dev.id = ordinal`; `dev_set_name(&tt_dev->dev, TENSTORRENT "/%d", ordinal)` (chardev.c:105-106) — the `/` in the name makes udev create **`/dev/tenstorrent/<N>`** (sysfs shows it as `tenstorrent!<N>`).
4. debugfs per-device dir `"<ordinal>"` under `tenstorrent/` (created only if `tt_debugfs_root` exists, chardev.c:108-110); procfs dir `/proc/driver/tenstorrent/<ordinal>` with a `pids` file, mode 0444, single-show `pids_proc_show` (chardev.c:111-113).
5. `INIT_LIST_HEAD(&tt_dev->open_fds_list)` (chardev.c:115).
6. `cdev_init(&tt_dev->chardev, &chardev_fops); return cdev_device_add(...)` (chardev.c:117-118). From this instant the device is openable — note `open_fds_list` and the waitqueues were initialized just before, and everything hardware-side (BARs, firmware A0) is already up, so an immediate open is safe by construction.

`pids_proc_show` lists the tgid of every open fd holder, under `chardev_mutex` (enumerate.c:230-241). The `mappings` debugfs file dumps per-fd pinnings, DMA buffers, TLB allocations, and VMAs; it takes `chardev_mutex`, then per-fd trylocks (`priv->mutex`, `iatu_mutex`, `vma_lock` — skipping detail sections it cannot lock), and hides addresses unless the reader has `CAP_SYS_ADMIN` (enumerate.c:84-215, `sensitive` at :91).

Device node permissions are a udev policy, not driver code: `SUBSYSTEM=="tenstorrent", MODE="0666"` plus by-id symlinks `tenstorrent/by-id/wormhole-<tt_asic_id>` / `blackhole-<tt_asic_id>` built from the `tt_asic_id` sysfs attribute (udev-50-tenstorrent.rules:1-3). The `KOBJ_CHANGE` uevent after hwmon/telemetry registration (wormhole.c:637, blackhole.c:671) exists to re-trigger those udev rules once `tt_asic_id` becomes readable.

> **Porting note:** the natural KMDF mapping is one device interface class for the "tenstorrent" class, world-openable (MODE 0666 equivalent — note this is deliberately unprivileged access), with the ordinal and the ASIC-ID-based stable identity exposed as interface reference string / device properties. The open-ordering guarantee (hardware fully initialized before the interface is enabled) maps to enabling the device interface only at the end of `EvtDevicePrepareHardware`/`EvtDeviceD0Entry` completion.

---

## 5. Interrupt allocation and handler (interrupt.c, all 46 lines)

```c
bool tenstorrent_enable_interrupts(struct tenstorrent_device *tt_dev)
{
        if (pci_alloc_irq_vectors(tt_dev->pdev, 1, 1, PCI_IRQ_ALL_TYPES) <= 0)
                goto out_pci_alloc_irq_vectors_failed;

        if (request_irq(pci_irq_vector(tt_dev->pdev, 0), irq_handler,
                        IRQF_SHARED, TENSTORRENT, tt_dev) != 0)
                goto out_request_irq_failed;

        tt_dev->interrupt_enabled = true;
        return true;
        ...
```
(interrupt.c:21-37)

Facts:

- **Exactly one vector** is requested (`min_vecs = 1, max_vecs = 1`).
- `PCI_IRQ_ALL_TYPES` = MSI-X, MSI, or legacy INTx; the kernel tries MSI-X first, then MSI, then legacy. The driver does not care which it got.
- `request_irq` uses `IRQF_SHARED` with name `"tenstorrent"` and `tt_dev` as cookie.
- The handler is a stub: it ignores the device (`(void)tt_dev; // to be used later`) and **unconditionally returns `IRQ_HANDLED`** (interrupt.c:13-19). Nothing in the driver is interrupt-driven; all firmware interaction is by polling.
- On `request_irq` failure the vectors are freed (interrupt.c:33-34); either failure returns `false` and probe continues (`interrupt_enabled` records the outcome — it is set both inside `tenstorrent_enable_interrupts` at interrupt.c:30 and by the assignment at enumerate.c:362).
- `tenstorrent_disable_interrupts` (interrupt.c:39-46) is guarded by the `interrupt_enabled` flag: `free_irq(pci_irq_vector(pdev, 0), tt_dev); pci_free_irq_vectors(pdev); interrupt_enabled = false;`.
- Interrupts are enabled at enumerate.c:362, **before** BARs are mapped by `init_device` — harmless only because the handler touches no hardware.

> **Porting note:** a KMDF port needs one message-signaled interrupt (or a shared line-based fallback) via `WdfInterruptCreate`; since the ISR does nothing, the port can defer creating an interrupt object entirely until a functional need appears — but must keep interrupt setup failure non-fatal to device start, matching Linux. Beware the Linux stub's `IRQ_HANDLED`-always behavior: do **not** replicate "claim every interrupt" on a shared line in Windows (return FALSE from the ISR if the device did not interrupt).

---

## 6. Sysfs attribute inventory (created during probe / init_telemetry)

The class device itself has **no static attribute groups** (`tt_dev->dev.groups = NULL`, chardev.c:102). Everything is added by `init_telemetry` (probe step 18) via `device_add_group`, plus a separately registered hwmon device. Removal for all of these is `cleanup_telemetry` (wormhole.c:766-784; blackhole.c:677-695), called early in remove.

### 6.1 Telemetry group (directly on `/sys/class/tenstorrent/tenstorrent!<N>/`)

All are mode `S_IRUGO` (0444), read-only. Visibility is dynamic: `tt_sysfs_telemetry_is_visible` hides any attribute whose telemetry tag was not found during tag-table probe (`telemetry_tag_cache[tag_id] == 0` → mode 0) (telemetry.c:130-143). Reads go through `tt_telemetry_read32`, which takes `reset_rwsem` shared and returns `-ENODEV` when detached, `-ENODATA` during `needs_hw_init`, `-EINVAL` for out-of-range tags (telemetry.c:9-33).

Wormhole set (wormhole.c:370-384), 13 attributes:

| Name | Telemetry tag | Format/semantics |
|---|---|---|
| `tt_aiclk` | AICLK (14) | decimal u32, MHz clock (`tt_sysfs_show_u32_dec`, telemetry.c:35-47) |
| `tt_axiclk` | AXICLK (15) | decimal u32 |
| `tt_arcclk` | ARCCLK (16) | decimal u32 |
| `tt_serial` | BOARD_ID (1) | `%08X%08X` of tags N,N+1 (`tt_sysfs_show_u64_hex`, telemetry.c:49-65) |
| `tt_card_type` | BOARD_ID (1) | decoded name: n300/n150/galaxy-wormhole/p100…p300c/galaxy-blackhole/unknown (`tt_sysfs_show_card_type`, telemetry.c:95-128) |
| `tt_fw_bundle_ver` | FLASH_BUNDLE_VERSION (28) | `maj.min.patch.ver` from packed u32 (`tt_sysfs_show_u32_ver`, telemetry.c:67-93) |
| `tt_m3app_fw_ver` | BM_APP_FW_VERSION (26) | version quad |
| `tt_ttflash_ver` | TT_FLASH_VERSION (58) | version quad |
| `tt_m3bl_fw_ver` | BM_BL_FW_VERSION (27) | version quad |
| `tt_arc_fw_ver` | CM_FW_VERSION (29) | version quad |
| `tt_eth_fw_ver` | ETH_FW_VERSION (24) | version, ETH packing `maj.min.patch` (telemetry.c:80-85) |
| `tt_asic_id` | ASIC_ID (61) | 64-bit hex (basis of udev by-id symlink) |
| `tt_heartbeat` | TIMER_HEARTBEAT (32) | decimal u32, increments while FW alive |

Blackhole set (blackhole.c:413-424), 10 attributes: `tt_aiclk`, `tt_axiclk`, `tt_arcclk`, `tt_serial`, `tt_card_type`, `tt_fw_bundle_ver`, `tt_m3app_fw_ver`, `tt_asic_id`, `tt_heartbeat`, plus `tt_therm_trip_count` (THERM_TRIP_COUNT tag 60, decimal). BH lacks WH's `tt_ttflash_ver`, `tt_m3bl_fw_ver`, `tt_arc_fw_ver`, `tt_eth_fw_ver`.

**Registration timing differs by chip.** Blackhole registers the telemetry group + hwmon synchronously inside `init_telemetry` (after a synchronous `telemetry_probe`; failure of the probe just skips group/hwmon, blackhole.c:641-675). Wormhole defers: `init_telemetry` schedules `fw_ready_work`, which polls firmware readiness every 1 s up to **120 retries** (`telemetry_retries = 120`, wormhole.c:743) and only then scans the tag table and registers the telemetry group and hwmon (wormhole.c:654-681, 748-764). So on WH the telemetry attributes may appear tens of seconds after `/dev/tenstorrent/<N>`.

### 6.2 `pcie_perf_counters` group (subdirectory `pcie_perf_counters/` on the class device)

Registered synchronously in both `init_telemetry` implementations; failure only logs "PCIe perf counters unavailable" (wormhole.c:753-757; blackhole.c:646-650). Twelve read-only (`DEVICE_ATTR_RO` → 0444) attributes — six NIU counters, each exposed for NOC0 (`…0`) and NOC1 (`…1`) (wormhole.c:400-447; blackhole.c:342-389):

| Name (suffix 0/1 = NOC0/NOC1) | Counter offset |
|---|---|
| `slv_posted_wr_data_word_received{0,1}` | 0x39 |
| `slv_nonposted_wr_data_word_received{0,1}` | 0x38 |
| `slv_rd_data_word_sent{0,1}` | 0x33 |
| `mst_posted_wr_data_word_sent{0,1}` | 0x9 |
| `mst_nonposted_wr_data_word_sent{0,1}` | 0x8 |
| `mst_rd_data_word_received{0,1}` | 0x3 |

(constants identical on both chips: wormhole.c:412-417, blackhole.c:354-359). Reads are raw `ioread32` of `4*offset (+ noc*stride)` from the NIU status block — WH: BAR4 `NOC2AXI_START+0x200`, NOC1 stride 0x8000 (wormhole.c:386-397); BH: `noc2axi_cfg + 0x4200`, NOC1 stride 0x10000 (blackhole.c:44-48, 332-339). **These show callbacks read hardware with no `detached`/reset guard** — safe only because `cleanup_telemetry` removes the files before BAR unmap (see §7 step 4).

### 6.3 hwmon device (separate `/sys/class/hwmon/hwmonX`, parent = PCI device)

Registered with `hwmon_device_register_with_info(dev, "wormhole"/"blackhole", tt_dev, &chip_info, NULL)` — note parent is `&pdev->dev`, not the class device (wormhole.c:619-638; blackhole.c:652-672). Channels (all read-only, visibility gated on tag presence via `tt_hwmon_is_visible`, telemetry.c:149-168):

| hwmon file | Tag | Conversion (telemetry.c:170-210) |
|---|---|---|
| `temp1_input` | ASIC_TEMP (11) | 16.16 fixed-point → millidegrees C |
| `temp1_max` | THM_LIMIT_THROTTLE (56) | °C × 1000 |
| `temp1_label` | — | "asic1_temp" (WH) / "asic_temp" (BH) |
| `in1_input` | VCORE (6) | mV as-is |
| `in1_max` | VDD_LIMITS (9) | upper 16 bits, mV |
| `in1_label` | — | "vcore1" / "vcore" |
| `curr1_input` | CURRENT (8) | A → mA (×1000) |
| `curr1_max` | TDC_LIMIT_MAX (55) | A → mA |
| `curr1_label` | — | "current1" / "current" |
| `power1_input` | POWER (7) | W → µW (×1000000) |
| `power1_max` | TDP_LIMIT_MAX (64) | W → µW |
| `power1_label` | — | "power1" / "power" |
| `fan1_input` (BH only) | FAN_RPM (41) | RPM as-is (blackhole.c:409, 431) |
| `fan1_label` (BH only) | — | "fan_rpm" |

(WH tables: wormhole.c:586-612; BH tables: blackhole.c:391-433.) If `CONFIG_HWMON` is disabled, enumerate.c provides a NULL-returning stub — but only for `devm_hwmon_device_register_with_info` (enumerate.c:78-82), while the code actually calls the non-devm variant; see Open questions.

### 6.4 Non-sysfs per-device files

- debugfs `/sys/kernel/debug/tenstorrent/<N>/mappings`, 0444 (enumerate.c:385, behavior §4).
- procfs `/proc/driver/tenstorrent/<N>/pids`, 0444 (chardev.c:111-113).

> **Porting note:** all of §6 is diagnostics/monitoring surface with no ioctl dependency, except `tt_asic_id`, which the udev by-id symlinks (stable device identity for UMD) depend on. A Windows port should expose ASIC ID early (device property), and can map the rest to WMI/ETW or a query ioctl at leisure.

---

## 7. Remove/shutdown sequence (`tenstorrent_pci_remove`, enumerate.c:404-481)

Also the `.shutdown` callback (enumerate.c:532). Ordered:

1. **Wormhole only**: `cancel_delayed_work_sync(&wh->fw_ready_work)` so deferred telemetry init can't race teardown (enumerate.c:410-413).
2. **Mark detached under `chardev_mutex`**: `tt_dev->detached = true` (enumerate.c:423-425). The long comment (enumerate.c:415-422) documents the invariant: `tt_cdev_release` arms `power_down_work` under `chardev_mutex` only when `!detached`, so this write partitions concurrent releases — any arm that happened is drained by the next step; later releases observe `detached` and skip arming.
3. `cancel_delayed_work_sync(&tt_dev->power_down_work)` (enumerate.c:427).
4. **Surprise-removal check**: read `PCI_VENDOR_ID`; if it reads `0xFFFF` (`U16_MAX`) the device is gone and `cleanup_hardware` (which sends the FW A3/ASTATE3 message) is skipped; otherwise call it (enumerate.c:429-434). (Both classes also internally no-op `cleanup_hardware` when `detached` — wormhole.c:786-791, blackhole.c:697-708 — but remove calls it exactly because the PCI link may still be alive even though `detached` was just set; the class guards protect the *suspend* and *reboot* callers.)

   *Correction:* `wormhole_cleanup_hardware` checks `!tt_dev->detached` (wormhole.c:789) and `detached` is already true here, so on WH the A3 message at remove time is actually skipped via the class-level guard; on BH likewise (blackhole.c:702). The vendor-ID check protects the *config read itself* plus older behavior. What is guaranteed: **no MMIO to a surprise-removed device.**
5. **Telemetry/sysfs teardown before BAR unmap**: `cleanup_telemetry` unregisters hwmon and removes both sysfs groups, waiting out in-flight show callbacks — this prevents use-after-unmap, since the perf-counter attrs read BARs unguarded (enumerate.c:436-440; wormhole.c:766-784; blackhole.c:677-695).
6. **Drain ioctls and unmap BARs under the write side of `reset_rwsem`**:
   ```c
   down_write(&tt_dev->reset_rwsem);
   tenstorrent_vma_zap(tt_dev);
   tt_dev->dev_class->cleanup_device(tt_dev); // unmap BARs
   up_write(&tt_dev->reset_rwsem);
   ```
   (enumerate.c:442-447). Every ioctl/mmap holds `reset_rwsem` shared and checks `detached` at entry (chardev.c:598-613, 717-729); release holds it shared too (chardev.c:929) but checks `detached` inside its hardware-touching steps rather than at entry (chardev.c:869, 907; iATU teardown at memory.c:288). So after `up_write` no fd can touch hardware: ioctls/mmaps observe `detached` and get `-ENODEV`; release skips its hardware writes. `tenstorrent_vma_zap` unmaps every user mapping (declared memory.h:60); `cleanup_device` is `pci_iounmap` of all mapped BAR regions (wormhole.c:793-801; blackhole.c:710-722).
7. **`tenstorrent_revoke_tlb_dmabufs(tt_dev)`** (enumerate.c:449-459) — must stay *after* the write-side drain; the comment explains the ordering proof (any in-flight export completed its `list_add` before the drain finished; later exporters see `detached` first).
8. **`wake_up_interruptible(&tt_dev->resource_lock_waitqueue)`** so blocked `LOCK_CTL ACQUIRE_BLOCKING` waiters observe `detached` and return `-ENODEV` (enumerate.c:461-463; waiter logic chardev.c:323-367).
9. **Per-fd memory cleanup**: `list_for_each_entry_safe(priv, tmp, &tt_dev->open_fds_list, open_fd) tenstorrent_memory_cleanup(priv);` (enumerate.c:465-467) — frees pinnings/DMA buffers of still-open fds. Note this walk takes **no `chardev_mutex`**.
10. **`tenstorrent_unregister_device`**: `debugfs_remove_recursive`, `proc_remove`, `cdev_device_del` (enumerate.c:469; chardev.c:121-126). Only now does `/dev/tenstorrent/<N>` disappear; already-open fds remain valid file objects whose operations all fail with `-ENODEV`.
11. **`tenstorrent_disable_interrupts`** (enumerate.c:470; §5).
12. `pci_disable_pcie_error_reporting`, `pci_disable_device`, `pci_set_drvdata(dev, NULL)` (enumerate.c:472-475).
13. **`xa_erase(&tenstorrent_dev_xa, tt_dev->ordinal)`** — freeing the ordinal for reuse (enumerate.c:477-478).
14. **`tenstorrent_device_put(tt_dev)`** — drops the probe-time kref (enumerate.c:480). Each open fd holds its own kref (`kref_get` at chardev.c:810, put at chardev.c:952), so the struct outlives remove until the last fd closes. Final release `tt_dev_release` unregisters the reboot notifier (if any), `pci_dev_put(pdev)`, `kfree(tt_dev)` (enumerate.c:483-493).

> **Porting note:** the load-bearing teardown invariants for Windows are: (a) mark the device unusable *before* draining I/O, under the same lock open/close uses; (b) remove all user-visible monitoring surfaces before unmapping BARs; (c) drain all in-flight ioctls (rundown/remove-lock, the analogue of the `reset_rwsem` write acquisition) before unmapping; (d) revoke shared mappings (sections/MDLs) of BAR/TLB space — `tenstorrent_vma_zap` maps to reflecting mappings through a mechanism that can be revoked; (e) keep per-open state alive past device removal until handle close (KMDF file objects naturally do this).

---

## 8. Suspend/resume and reboot

`SIMPLE_DEV_PM_OPS(tenstorrent_pm_ops, tenstorrent_suspend, tenstorrent_resume)` (enumerate.c:524):

- **Suspend** (enumerate.c:499-509): `cancel_delayed_work_sync(&power_down_work)`, `tenstorrent_revoke_tlb_dmabufs`, `dev_class->cleanup_hardware` (FW → A3). Always returns 0. It does *not* zap user VMAs or set `detached`.
- **Resume** (enumerate.c:511-522): `ok = dev_class->init_hardware(tt_dev)`; on success `pci_save_state(pdev)` because "Suspend invalidates the saved state" (enumerate.c:517-519); returns `ok ? 0 : -EIO`.

Reboot: see §3 step 17 — Wormhole only, ASTATE3 on anything except `SYS_POWER_OFF` (enumerate.c:243-251).

---

## 9. PCI config-space save and hot-reset references

Config-state save points:
- probe: `pci_save_state(dev)` + class `save_reset_state` (MPS via DBI) (enumerate.c:372-373);
- resume after successful `init_hardware` (enumerate.c:519);
- WH PCIe-retrain loop before each hot reset (pcie.c:125);
- `safe_pci_restore_state` re-saves immediately after every restore (`pci_restore_state(pdev); pci_save_state(pdev);`, pcie.c:56-58) so the saved copy is never consumed.

`safe_pci_restore_state` (pcie.c:43-59) refuses to restore unless `pdev->state_saved` and a vendor-ID config read returns `0x1E52` — guarding against the soft-lockup-length capability walk on dead hardware.

Hot reset (`pcie_hot_reset_and_restore_state`, pcie.c:61-90) is a **secondary bus reset** on the upstream bridge, not FLR: set `PCI_BRIDGE_CTL_BUS_RESET` in the bridge's `PCI_BRIDGE_CONTROL`, `msleep(2)`, clear it, `msleep(500)`, then poll vendor ID every 100 ms for up to **10 000 ms** (`poll_pcie_link_up`, pcie.c:24-41, timeout passed at pcie.c:80), then `safe_pci_restore_state`. Hotplug is suppressed around the reset via `pci_ignore_hotplug`, and the flags are cleared manually afterwards (pcie.c:70, 82-87). **There is no FLR anywhere in the driver** (no `pcie_flr`/`pci_reset_function`; verified by grep). The other reset flavors reachable from the RESET_DEVICE ioctl (config-write timer interrupt `pcie_timer_interrupt` at pcie.c:133-138 using config offsets 0x930/0x934, and the reset marker in `PCI_COMMAND` parity bit, pcie.c:140-158) belong to the ioctl section but are triggered on the same `pdev`.

Probe-time hot-reset use: `wormhole_complete_pcie_init` (called from WH `init_hardware`, wormhole.c:728) loops up to `reset_limit` times sending FW message `0xB6` (PCIE_RETRAIN, 200 ms timeout) and performing save + hot reset between attempts (pcie.c:92-131).

> **Porting note:** secondary-bus reset from a function driver is not generally available on Windows; the port must either rely on the OS-supplied reset interfaces (`GUID_DEVICE_RESET_INTERFACE_STANDARD` PLDR/FLDR) or negotiate ACPI/platform support. Since the hardware has no FLR path in this driver, PLDR (which Windows implements as SBR when possible) is the closest match — but the WH probe-time retrain loop depends on *driver-controlled* SBR with custom (shorter) delays; that is an open design problem for the port.

---

## 10. Lifecycle-relevant locking summary

| Lock | Protects | Lifecycle usage |
|---|---|---|
| `tenstorrent_dev_xa` internal spinlock | ordinal map | probe insert/alloc (enumerate.c:287-293), remove/fail erase (enumerate.c:398, 478) |
| `tt_dev->chardev_mutex` | `open_fds_list`, `chardev_excl_held`, power aggregation, `detached` transition | probe init (enumerate.c:316); remove sets `detached` under it (enumerate.c:423-425); open/close list add/del (chardev.c:755-787, 936-948); mappings/pids readers (enumerate.c:100, 235) |
| `tt_dev->reset_rwsem` | hardware access vs reset/remove | read-held by every ioctl/mmap/open-body/release; write-held by RESET_DEVICE ioctl and remove's zap+unmap window (enumerate.c:444-447; chardev.c:598-601) |
| `tt_dev->kref` | struct lifetime | probe init (enumerate.c:302), per-open get (chardev.c:810), remove put (enumerate.c:480), release frees (enumerate.c:483-493) |
| `iatu_mutex`, `dmabuf_export_lock`, per-fd locks | see memory/ioctl sections | initialized at probe (enumerate.c:317-318) |

---

## Key constants table

| Name | Value | Source |
|---|---|---|
| `PCI_VENDOR_ID_TENSTORRENT` | `0x1E52` | enumerate.h:15 |
| `PCI_DEVICE_ID_GRAYSKULL` | `0xFACA` (NULL driver_data → -ENODEV) | enumerate.h:16; module.c:65-66 |
| `PCI_DEVICE_ID_WORMHOLE` | `0x401E` | enumerate.h:17 |
| `PCI_DEVICE_ID_BLACKHOLE` | `0xB140` | enumerate.h:18 |
| `PCI_SUBSYSTEM_ID_GALAXY_WH` | `0x0035` | enumerate.c:42 |
| `PCI_SUBSYSTEM_ID_GALAXY_BH` | `0x0047` | enumerate.c:43 |
| `GALAXY_CHIPS_PER_UBB` / `GALAXY_NUM_UBBS` | 8 / 4 | enumerate.c:39-40 |
| WH galaxy bus-prefix table | `{0xC, 0x8, 0x0, 0x4}` | enumerate.c:45 |
| BH galaxy bus-prefix table | `{0x0, 0x4, 0xC, 0x8}` | enumerate.c:46 |
| Fixed-up PCI class for unflashed boards | `0x120000` | enumerate.c:273 |
| Ordinal allocation limit | `xa_limit_31b` (0..2^31-1) | enumerate.c:290, 293 |
| `max_devices` default (chrdev minors) | 32 | module.c:36 |
| Streaming DMA mask default | 64 bits (`dma_address_bits ?: 64`) | enumerate.c:330 |
| Coherent DMA mask, Wormhole | 32 bits | wormhole.c:1058 |
| Coherent DMA mask, Blackhole | 58 bits | blackhole.c:816 |
| DMA max seg size / seg boundary | `UINT_MAX` / `ULONG_MAX` | enumerate.c:334-335 |
| IRQ vectors requested | min 1, max 1, `PCI_IRQ_ALL_TYPES` | interrupt.c:23 |
| IRQ flags / name | `IRQF_SHARED`, `"tenstorrent"` | interrupt.c:26-27 |
| Surprise-removal sentinel | vendor ID == `U16_MAX` | enumerate.c:432-433 |
| Hot reset timings | 2 ms assert, 500 ms settle, 10 s link poll @100 ms | pcie.c:76-80, 36 |
| WH telemetry-ready poll | 120 retries × 1000 ms | wormhole.c:743, 665 |
| `idle_power_down_grace_ms` default | 5000 | module.c:56 |
| `power_policy` default | true (low power at probe) | module.c:52; enumerate.c:387-389 |
| WH BAR mappings | BAR2 (iATU@0x1200), BAR4 (sysregs via iATU → 0x1E000000) | wormhole.c:46, 74-76, 696-700 |
| BH BAR0 iomap ranges | TLB regs 0x1FC00000+0x1000; NOC2AXI 0x1FD00000+0x100000 | blackhole.c:33-34, 44-45, 587-589 |
| BH MRRS | 4096 | blackhole.c:18, 626 |
| Device node udev mode | 0666 | udev-50-tenstorrent.rules:1 |
| debugfs `mappings` / procfs `pids` mode | 0444 / 0444 | enumerate.c:385; chardev.c:113 |
| Driver version | 2.10.1-pre | module.h:19-22 |

---

## Open questions

1. **Ordinal vs chrdev-region overflow.** `alloc_chrdev_region` reserves `max_devices` (default 32) minors (chardev.c:55), but `xa_alloc` can return ordinals up to 2^31-1 (enumerate.c:290-293) and `devt_for_device` adds the ordinal to the base minor without bounds-checking (chardev.c:85-88). Behavior when >32 devices (or Galaxy fallback ordinals ≥ 32) are probed is unverified — `cdev_device_add` would register a devt outside the reserved region. A port should pick an explicit policy.
2. **`tenstorrent_register_device` return value ignored** (enumerate.c:375 vs chardev.c:118). If `cdev_device_add` fails, probe still succeeds, leaving a device with sysfs/telemetry but no char device, and remove will call `cdev_device_del` on a never-added cdev. Intentional tolerance or oversight?
3. **`kzalloc` failure path leaves the PCI device enabled** — the `-ENOMEM` return at enumerate.c:280-282 does not call `pci_disable_device`, unlike the ordinal-failure path (enumerate.c:295-299). Presumed oversight; a port should unwind symmetrically.
4. **Stub handler claims all shared interrupts.** `irq_handler` returns `IRQ_HANDLED` unconditionally (interrupt.c:13-19) while requesting with `IRQF_SHARED`; on a shared INTx line this masks other devices' interrupt storms/diagnostics. Whether future TT interrupt sources will require reading a status register first is unknown from this code.
5. **Embedded `struct device` has `release = NULL`** (chardev.c:103) while the memory is freed via the separate driver kref (`kfree` at enumerate.c:492). This relies on the device kobject holding no references at kfree time; the Linux core normally warns on releasing a device without a release callback. Whether a warning fires (and whether refs can outlive) was not verified at runtime.
6. **Unlocked `open_fds_list` walk in remove** (enumerate.c:465-467) — no `chardev_mutex` held while iterating and running `tenstorrent_memory_cleanup` on live fds. Presumably safe because `cdev_device_del` has not yet run... but opens are still possible at that instant (device deleted only at enumerate.c:469), and a concurrent open's `list_add` (chardev.c:785) or release's `list_del` (chardev.c:938) could race the walk. Needs a deliberate answer in the port (take the equivalent lock).
7. **`CONFIG_HWMON=n` stub mismatch**: enumerate.c:78-82 stubs `devm_hwmon_device_register_with_info`, but the classes call the non-devm `hwmon_device_register_with_info` (wormhole.c:628, blackhole.c:664) — the stub appears stale; a `CONFIG_HWMON=n` build's status is unclear.
8. **Galaxy ordinal mapping ignores PCI domain** (enumerate.c:51) — on a host exposing Galaxy chips across multiple PCI segments, bus-number collisions between segments would map two chips to the same fixed ordinal (second one falls back to dynamic with a warning). Is single-domain an actual invariant of Galaxy hosts?
9. **Remove-time A3 message**: remove calls `cleanup_hardware` only if the vendor ID reads back valid (enumerate.c:432-434), but both class implementations independently skip work when `detached` is set (wormhole.c:789, blackhole.c:702), and `detached` is always true by that point in remove — so at driver unload the firmware apparently never receives the A3 message from the remove path (it does from suspend and WH reboot-notifier paths, where `detached` is false). Whether leaving FW in A0 at unload is intended (e.g. relying on the probe-time low-power policy of a future driver load) is not documented in the code.
