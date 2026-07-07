# 03. Char Device and FD Lifecycle

## Scope

Primary files (read in full):

| File | Lines |
|---|---|
| `chardev.c` | 966 |
| `chardev.h` | 18 |
| `chardev_private.h` | 82 |

Supporting files read for context and cited where load-bearing: `ioctl.h` (458 lines, read in full), `memory.c` (1742 lines; mmap dispatch, query-mappings, TLB-allocate, cleanup, and vma-zap paths), `memory.h` (73), `device.h` (127), `tlb.c` (108), `module.c` / `module.h` (params), `enumerate.c` / `enumerate.h` (registration context, remove path). All paths are relative to the tt-kmd repo root at tag `ttkmd-2.10.0-rc1-1-g8c32c2b`.

---

## 1. Char device registration

### Major/minor scheme

`init_char_driver(max_devices)` allocates one dynamic major with `max_devices` contiguous minors and creates a device class, both named `"tenstorrent"`:

```c
res = alloc_chrdev_region(&tt_device_id, 0, max_devices, TENSTORRENT);   // chardev.c:55
tt_dev_class = class_create(TENSTORRENT);                                 // chardev.c:60 (>=6.4 form)
```
(chardev.c:48-75). `TENSTORRENT` is `"tenstorrent"` (enumerate.h:13). `max_devices` is a module parameter defaulting to **32** (module.c:36-38). On class-create failure the chrdev region is unregistered and `res` is returned — but `res` still holds `alloc_chrdev_region`'s successful return (0) at that point, so `init_char_driver` actually reports *success* despite the failed class creation (apparent upstream bug; chardev.c:56, 64-74). `cleanup_char_driver()` destroys the class then unregisters the region (chardev.c:77-83).

Per-device minor = base minor + device ordinal:

```c
return MKDEV(MAJOR(tt_device_id), MINOR(tt_device_id) + tt_dev->ordinal);   // chardev.c:87
```
(chardev.c:85-88). The ordinal is assigned at PCI probe time (an xarray slot; see enumerate.c — out of scope here).

### Device node naming

```c
dev_set_name(&tt_dev->dev, TENSTORRENT "/%d", tt_dev->ordinal);   // chardev.c:106
```
Because the name contains a `/`, udev creates the node as **`/dev/tenstorrent/<ordinal>`** (a directory containing per-ordinal nodes). tt-umd depends on exactly this path: `open(fmt::format("/dev/tenstorrent/{}", n).c_str(), O_RDWR | O_CLOEXEC | O_APPEND)` (tt-umd/device/pcie/pci_device.cpp:355).

### Registration sequence (`tenstorrent_register_device`, chardev.c:90-119)

1. `init_waitqueue_head` for `resource_lock_waitqueue` and `chardev_excl_waitqueue` (chardev.c:95-96).
2. `device_initialize`; sets `devt`, `class`, `parent = &tt_dev->pdev->dev`, `groups = NULL`, `release = NULL`, `id = ordinal` (chardev.c:98-105).
3. Creates per-device debugfs dir `<ordinal>` under the module root, and a procfs dir `<ordinal>` under `/proc/driver/tenstorrent` containing a read-only `pids` file (mode `0444`) backed by `pids_proc_show` (chardev.c:108-113). `pids_proc_show` walks `open_fds_list` under `chardev_mutex` and prints `pid_vnr(priv->pid)` per open fd (enumerate.c:230-241).
4. `INIT_LIST_HEAD(&tt_dev->open_fds_list)` (chardev.c:115).
5. `cdev_init(&tt_dev->chardev, &chardev_fops); return cdev_device_add(...)` (chardev.c:117-118).

`tenstorrent_unregister_device` removes debugfs recursively, removes procfs, then `cdev_device_del` (chardev.c:121-126). Note there is no forced revocation of already-open fds here; they are handled by the `detached` flag (section 7).

> **Porting note:** On Windows/KMDF the whole major/minor + udev-node scheme maps to a device interface GUID plus a per-device reference string (or `\\.\TenstorrentN` symbolic links) created in `EvtDeviceAdd`. The ordinal-stability property (ordinal ties the node name to an xarray slot allocated at probe, enumerate.c:284-294; the slot is erased unconditionally at remove — deliberately not postponed to last-close — so a re-probed device can reuse the ordinal even while stale fds still reference the old device struct, enumerate.c:477-478) must be reproduced deliberately if UMD-on-Windows enumerates by index.

---

## 2. `file_operations` table

```c
static struct file_operations chardev_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = tt_cdev_ioctl,
	.mmap = tt_cdev_mmap,
	.open = tt_cdev_open,
	.release = tt_cdev_release,
};
```
(chardev.c:40-46). There is **no** `read`, `write`, `poll`, `llseek`, or `compat_ioctl`. The absence of `.compat_ioctl` means a 32-bit process on a 64-bit kernel gets `-ENOTTY` for every ioctl — 32-bit userspace is effectively unsupported.

`get_tenstorrent_priv(struct file *f)` (chardev.c:960-966) returns `f->private_data` only if `f->f_op == &chardev_fops`, else `NULL`. This is the identity check used by `TENSTORRENT_IOCTL_MAP_PEER_BAR` to verify that a caller-supplied peer fd really is a tenstorrent chardev fd.

> **Porting note:** KMDF equivalent is `EvtDeviceFileCreate` / `EvtFileCleanup` / `EvtFileClose` plus an `IRP_MJ_DEVICE_CONTROL` queue. The `f_op` identity check for MAP_PEER_BAR needs a Windows analogue (e.g., verifying a referenced `FILE_OBJECT`'s `DeviceObject` belongs to this driver via `ObReferenceObjectByHandle` + driver-object comparison).

---

## 3. open(): `tt_cdev_open` (chardev.c:791-863)

### O_APPEND power-aware-client detection

```c
bool power_aware = file->f_flags & O_APPEND;   // chardev.c:795
```

A client that opens with `O_APPEND` declares itself "power-aware": it gets a zero initial power contribution and is expected to call `SET_POWER_STATE` explicitly. A legacy client (no `O_APPEND`) gets an eagerly-applied high-power default:

```c
private_data->power_state.validity = TT_POWER_VALIDITY(15, 0);            // chardev.c:821
if (!power_aware)
	private_data->power_state.power_flags = TT_POWER_FLAG_ALL & ~TT_POWER_FLAG_MAX_AI_CLK;  // chardev.c:822-823
```

`TT_POWER_FLAG_ALL` is `0x7FFF` (chardev.c:29) and `TT_POWER_FLAG_MAX_AI_CLK` is bit 0 (ioctl.h:406), so the legacy default is `0x7FFE`: everything on except max AI clock ("AICLK=Low, everything else enabled", chardev.c:817). This contract is documented in ioctl.h:373-378 and relied on by tt-umd, which opens with `O_RDWR | O_CLOEXEC | O_APPEND` (tt-umd/device/pcie/pci_device.cpp:204, 355, 1101).

### Full open sequence

1. `kzalloc` the `struct chardev_private`; `-ENOMEM` on failure (chardev.c:798-800).
2. Init per-fd state: `mutex_init(&priv->mutex)`, `hash_init(dmabufs)`, `INIT_LIST_HEAD` on `pinnings`, `peer_mappings`, `vma_list`, `mutex_init(&priv->vma_lock)` (chardev.c:802-808).
3. **Device reference count:** `kref_get(&tt_dev->kref)` (chardev.c:810). Every open fd holds a kref on the device struct; the struct outlives PCI removal until the last fd closes (release path drops it, chardev.c:952; `tenstorrent_device_put` → `kref_put(..., tt_dev_release)` enumerate.c:495-497).
4. Snapshot reset generation: `priv->open_reset_gen = atomic_long_read(&tt_dev->reset_gen)` (chardev.c:812).
5. Record identity for diagnostics: `priv->pid = get_pid(task_pid(current->group_leader)); get_task_comm(priv->comm, current)` (chardev.c:814-815).
6. Initialize `power_state` (above) *before* the fd becomes visible on `open_fds_list` (comment chardev.c:819-820).
7. **Admission control** `admit_chardev_open(tt_dev, priv, file->f_flags)` (chardev.c:825, 749-789): an open()-time reader/writer arbitration where `O_EXCL` is the writer and plain opens are readers (comment chardev.c:743-748).
   - `O_EXCL`: loop while `open_fds_list` is non-empty; with `O_NONBLOCK` return `-EAGAIN`, else `wait_event_interruptible_exclusive` on `chardev_excl_waitqueue` (signal → error return, typically `-ERESTARTSYS`) (chardev.c:757-768). On success sets `WRITE_ONCE(tt_dev->chardev_excl_held, true)` (chardev.c:769).
   - Non-exclusive: loop while `chardev_excl_held`; `O_NONBLOCK` → `-EAGAIN`, else `wait_event_interruptible` (non-exclusive registration so all readers wake together) (chardev.c:771-783).
   - On success: `list_add(&priv->open_fd, &tt_dev->open_fds_list)` under `chardev_mutex` (chardev.c:785).
   - While an O_EXCL fd is held it is the only fd on the list; its release (`list_empty` transition) wakes both waiter kinds (chardev.c:746-748, 943-946).
8. On admission failure: `put_pid`, `kfree(priv)`, `tenstorrent_device_put(tt_dev)`, return error (chardev.c:826-831).
9. `down_read(&tt_dev->reset_rwsem)` around the remaining body so `RESET_DEVICE` (exclusive holder) cannot interleave (chardev.c:833-838).
10. If the device class defers idle powerdown, `cancel_delayed_work_sync(&tt_dev->power_down_work)` — eagerly drains a powerdown armed by a previous last-close so an open in the grace window does not ship a spurious powerdown/powerup pair (chardev.c:840-850; correctness argument in the comment: even without the cancel, the work handler re-aggregates and would see this fd on the list).
11. If legacy client and `!tt_dev->detached && !tt_dev->needs_hw_init`: `tenstorrent_set_aggregated_power_state(tt_dev)`; failure only logs a warning — **open still succeeds** (chardev.c:852-856).
12. `up_read`; `file->private_data = priv`; return 0 (chardev.c:858-862).

Note: `open()` never checks `detached` or `reset_gen` as a failure condition — a fd opened against a resetting/removed device opens fine and then gets `-ENODEV` from every ioctl/mmap.

> **Porting note:** Windows `CreateFile` has no `O_APPEND` equivalent that reaches a KMDF `EvtDeviceFileCreate` cleanly (`FILE_APPEND_DATA` desired-access is the closest signal, visible via the create parameters). The port needs an explicit contract with UMD — e.g., an "I am power-aware" ioctl issued immediately after open, or an EA/reference-string on create. `O_EXCL`/`O_NONBLOCK` admission maps naturally onto share-mode arbitration done manually in `EvtDeviceFileCreate` (Windows share modes alone cannot express "exclusive waits for idle"); the blocking-with-signal semantics (`-ERESTARTSYS`) become an alertable wait returning `STATUS_CANCELLED`. Beware: blocking in `EvtDeviceFileCreate` blocks the requesting thread's create IRP — mark the create pending or use a manual queue.

---

## 4. release(): `tt_cdev_release` cleanup order (chardev.c:922-958)

Linux calls `.release` once, when the last reference to the `struct file` goes away (all dups closed, all mmaps unmapped — each vma holds a file reference, so `priv->vma_list` is empty by the time release runs; entries are removed by `tenstorrent_vma_close` at unmap, memory.c:1412-1434).

The order is load-bearing:

1. **`down_read(&tt_dev->reset_rwsem)`** — the whole device-touching body is under the shared reset lock so RESET_DEVICE cannot interleave (chardev.c:927-929).
2. **NOC cleanup write** — `tt_cdev_release_noc_cleanup(priv)` (chardev.c:931, 865-875): if `!tt_dev->detached && priv->noc_cleanup.enabled`, perform the registered write:
   ```c
   tt_dev->dev_class->noc_write32(tt_dev, priv->noc_cleanup.x, priv->noc_cleanup.y,
                                  priv->noc_cleanup.addr, priv->noc_cleanup.data & 0xFFFFFFFF,
                                  priv->noc_cleanup.noc);   // chardev.c:872-874
   ```
   This is the crash-safe device-side notification mechanism documented at ioctl.h:330-348. It happens **first**, before any resource teardown. Note it checks only `detached` — not `reset_gen` or `needs_hw_init` (see Open questions).
3. **Memory cleanup** — `tenstorrent_memory_cleanup(priv)` (chardev.c:932, memory.c:1638-1668), under `priv->mutex`, in this internal order:
   a. **DMA buffers**: for each hashtable entry: `dma_free_coherent`, `teardown_outbound_iatu` (disables the outbound iATU region via `configure_outbound_atu(...,0,0,0)` unless detached, under `iatu_mutex`, memory.c:276-297), `hash_del`, `kfree` (memory.c:1649-1654).
   b. **Pinned pages**: for each `pinned_page_range`: `teardown_outbound_iatu`, `dma_unmap_sgtable` (direction `DMA_TO_DEVICE` if read-only else `DMA_BIDIRECTIONAL`), `free_chained_sgt`, `unpin_user_pages_dirty_lock(pages, page_count, !read_only)` (dirties pages unless read-only), `vfree(pages)`, `list_del`, `kfree` (memory.c:1656-1658, 299-314).
   c. **Peer BAR mappings**: `dma_unmap_resource(..., DMA_BIDIRECTIONAL, 0)`, `list_del`, `kfree` (memory.c:1660-1665).
4. **Resource locks** — `tt_cdev_release_resource_locks(priv)` (chardev.c:933, 877-885): for each of the 64 bits, `test_and_clear_bit` on the per-fd bitmap and, if set, `clear_bit` on the device bitmap (local-then-global order, the inverse of acquire's global-then-local invariant, chardev.c:369-370); then one `wake_up_interruptible(&resource_lock_waitqueue)`.
5. **TLB windows** — `tt_cdev_release_tlbs(priv)` (chardev.c:934, 887-892): `for_each_set_bit` in `priv->tlbs` (256 bits) call `tenstorrent_device_free_tlb(tt_dev, bit)`. That drops the owning-fd refcount; the device-level bit is cleared only when the refcount hits zero — a live TLB dma-buf export keeps the window allocated past close (tlb.c:74-78, ioctl.h:434-438).
6. **`mutex_lock(&tt_dev->chardev_mutex)`; `list_del(&priv->open_fd)`** (chardev.c:936-938).
7. **Power recomputation** — `tt_cdev_release_power(priv)` — must follow the `list_del` because it reads `open_fds_list` (comment chardev.c:940, code 894-920):
   - `can_defer = dev_class->defer_idle_powerdown && (idle_power_down_grace_ms > 0)` (chardev.c:903; grace default **5000 ms**, module.c:56-58; `0` forces the synchronous path, device.h:116-122).
   - `no_power_contrib = (validity == TT_POWER_VALIDITY(15,0) && power_flags == 0)` (chardev.c:904) — a power-aware fd that never requested anything.
   - Early-outs, in order: `detached || needs_hw_init` → return; `!power_policy` (module param, default `true`, module.c:52-54) → return; `no_power_contrib` → return (chardev.c:907-914).
   - If `can_defer && last_close`: `mod_delayed_work(system_wq, &tt_dev->power_down_work, msecs_to_jiffies(idle_power_down_grace_ms))` — powerdown deferred by the grace period (chardev.c:916-917). Otherwise synchronous `tenstorrent_set_aggregated_power_state_locked(tt_dev)` (chardev.c:919).
8. **Exclusivity release / waiter wake**: if `open_fds_list` is now empty, `WRITE_ONCE(chardev_excl_held, false)` and `wake_up_interruptible(&chardev_excl_waitqueue)` (chardev.c:943-946), then unlock `chardev_mutex`.
9. **`up_read(&reset_rwsem)`** (chardev.c:950).
10. **Final frees**: `tenstorrent_device_put(tt_dev)` (drops the open-time kref — may free the device struct if the PCI device was already removed), `put_pid(priv->pid)`, `kfree(priv)`, `file->private_data = NULL`; returns 0 unconditionally (chardev.c:952-957).

Release deliberately does **not** check `reset_gen`: a stale pre-reset fd still cleans up its locks, TLB bits, and memory at close — that is the *only* way a pre-reset fd's resource-lock bits get released (comment chardev.c:312-316).

Aggregated power computation (`tenstorrent_set_aggregated_power_state_locked`, chardev.c:478-534, called under `chardev_mutex`): iterates `open_fds_list`, **skips fds whose `open_reset_gen` differs from the device `reset_gen`** (chardev.c:493-495), and under each `priv->mutex` decodes `validity` (bits 0-3 flags-count, bits 4-7 settings-count, chardev.c:499-503), ORs each fd's `power_flags` with the mask of flags it did not specify (`~((1U << flags_count) - 1) & 0x7FFF` — unspecified flags default ON for backward compatibility, chardev.c:506-517), and takes the per-index max over `power_settings` (chardev.c:519-523). The final message uses `validity = TT_POWER_VALIDITY(15, max_settings_count)` (chardev.c:528-531) and goes to firmware via `dev_class->set_power_state`. The delayed-work handler `tenstorrent_power_down_work_func` just calls the mutex-taking wrapper (chardev.c:553-560); its safety against remove/reset relies on `tenstorrent_pci_remove` setting `detached = true` under `chardev_mutex` *before* `cancel_delayed_work_sync` (chardev.c:547-552, enumerate.c:415-427), and on RESET_DEVICE cancelling it while holding `reset_rwsem` exclusive (chardev.c:235-238).

> **Porting note:** On Windows the analogue of `.release` teardown belongs in `EvtFileCleanup` (delivered when the last user handle closes, while the process address space is still alive), not `EvtFileClose` (which may arrive arbitrarily late, after all mapped views are gone). Windows complicates the "vma_list is empty at release" invariant: section views can outlive the handle (`IRP_MJ_CLEANUP` arrives before views are unmapped). A port that maps device memory into user space must revoke or track mappings explicitly at cleanup. The teardown order — device-notification write → DMA/IOMMU teardown → lock release → TLB free → power recompute — must be preserved; in particular the NOC cleanup write must precede freeing the TLB windows and DMA buffers that in-flight device work might still reference. The 5 s deferred powerdown maps to a WDFTIMER or `IoQueueWorkItem`, with the same cancel-before-teardown ordering versus remove/reset.

---

## 5. ioctl dispatch: `tt_cdev_ioctl` (chardev.c:591-706)

All ioctl codes are `_IO(0xFA, n)` — magic `0xFA`, sequence 0-16, **no size/direction encoded in the number** (ioctl.h:12-30).

Locking: `TENSTORRENT_IOCTL_RESET_DEVICE` takes `reset_rwsem` **exclusive**; every other ioctl takes it **shared** (chardev.c:596-601). Then three gates, all returning `-ENODEV`:

```c
if (priv->device->detached) { ret = -ENODEV; goto out; }                                  // chardev.c:604-607
if (atomic_long_read(&priv->device->reset_gen) != priv->open_reset_gen) { ret = -ENODEV; } // chardev.c:610-613
if (priv->device->needs_hw_init) {
	bool allowed = (cmd == TENSTORRENT_IOCTL_GET_DEVICE_INFO ||
	                cmd == TENSTORRENT_IOCTL_GET_DRIVER_INFO ||
	                cmd == TENSTORRENT_IOCTL_RESET_DEVICE);
	if (!allowed) { ret = -ENODEV; }                                                       // chardev.c:616-624
}
```

Dispatch table (chardev.c:626-697):

| nr | ioctl | handler | defined at |
|---|---|---|---|
| 0 | GET_DEVICE_INFO | `ioctl_get_device_info` | chardev.c:128-160 |
| 1 | GET_HARVESTING | *(none — `break` with `ret` still `-EINVAL`)* | chardev.c:631-632 |
| 2 | QUERY_MAPPINGS | `ioctl_query_mappings` | memory.c:331-409 |
| 3 | ALLOCATE_DMA_BUF | `ioctl_allocate_dma_buf` | memory.c:425 |
| 4 | FREE_DMA_BUF | `ioctl_free_dma_buf` | memory.c |
| 5 | GET_DRIVER_INFO | `ioctl_get_driver_info` | chardev.c:162-190 |
| 6 | RESET_DEVICE | `ioctl_reset_device` | chardev.c:200-310 |
| 7 | PIN_PAGES | `ioctl_pin_pages` | memory.c |
| 8 | LOCK_CTL | `ioctl_lock_ctl` | chardev.c:371-430 |
| 9 | MAP_PEER_BAR | `ioctl_map_peer_bar` | memory.c |
| 10 | UNPIN_PAGES | `ioctl_unpin_pages` | memory.c |
| 11 | ALLOCATE_TLB | `ioctl_allocate_tlb` | memory.c:893-944 |
| 12 | FREE_TLB | `ioctl_free_tlb` | memory.c:946 |
| 13 | CONFIGURE_TLB | `ioctl_configure_tlb` | memory.c |
| 14 | SET_NOC_CLEANUP | `ioctl_set_noc_cleanup` | chardev.c:432-476 |
| 15 | SET_POWER_STATE | `ioctl_set_power_state` | chardev.c:562-589 |
| 16 | EXPORT_TLB_DMABUF | `ioctl_export_tlb_dmabuf` | memory.c |
| — | default | `-EINVAL` | chardev.c:694-696 |

Handlers implemented in chardev.c, with their validation/error behavior:

- **GET_DEVICE_INFO** (chardev.c:128-160): copies `in.output_size_bytes`; fills vendor/device/subsystem IDs, `bus_dev_fn = PCI_DEVID(bus->number, devfn)` (bit layout ioctl.h:56), `max_dma_buf_size_log2 = MAX_DMA_BUF_SIZE_LOG2` = **28** (memory.h:10), `pci_domain`. Output protocol used by most fixed-size ioctls: `clear_user(&arg->out, in.output_size_bytes)` then `copy_to_user` of `min(in.output_size_bytes, sizeof(out))` — the caller controls how many output bytes are written, extra bytes are zeroed. All copy failures → `-EFAULT`.
- **GET_DRIVER_INFO** (chardev.c:162-190): same protocol; returns `driver_version = 2` (ioctl.h:10) and major/minor/patch = 2/10/1 (module.h:19-21).
- **RESET_DEVICE** (chardev.c:200-310) — runs with `reset_rwsem` held exclusive:
  - Destructive flags (`RESET_PCIE_LINK`=1, `CONFIG_WRITE`=2, `USER_RESET`=3, `ASIC_RESET`=4, `ASIC_DMC_RESET`=5) are refused with `-EBUSY` while any TLB dma-buf export is live (`tenstorrent_has_tlb_dmabuf_exports`, chardev.c:222-233; rationale ioctl.h:429-433).
  - `cancel_delayed_work_sync(&tt_dev->power_down_work)` before touching hardware (chardev.c:238).
  - Flag semantics (all constants ioctl.h:143-151): `RESTORE_STATE`(0): `safe_pci_restore_state` + `restore_reset_state` + `init_hardware` (no gen bump, no zap) (chardev.c:240-246). `RESET_PCIE_LINK`(1): `tenstorrent_vma_zap` + `pcie_hot_reset_and_restore_state` (no gen bump) (chardev.c:247-249). `CONFIG_WRITE`(2): `bump_reset_gen` + zap + `pcie_timer_interrupt` (chardev.c:250-253). `USER_RESET`(3): bump + zap + `set_reset_marker` + `needs_hw_init = true` (chardev.c:254-258). `ASIC_RESET`(4) / `ASIC_DMC_RESET`(5): bump + zap + `dev_class->reset(...)` + `needs_hw_init = true` (chardev.c:259-268). `POST_RESET`(6): `ok = is_reset_marker_zero(pdev)`; if `needs_hw_init`, clear it and `safe_pci_restore_state` + `restore_reset_state` + `init_hardware` + optional `probe_telemetry` (chardev.c:269-287). Any other flag → `-EINVAL` (chardev.c:288-290).
  - `out.result = !ok` — **0 means success** (chardev.c:293). Wakes `resource_lock_waitqueue` so blocked ACQUIRE_BLOCKING waiters re-check validity (chardev.c:295-299). Output copy uses the standard protocol.
- **LOCK_CTL** (chardev.c:371-430): `in.index >= TENSTORRENT_RESOURCE_LOCK_COUNT` (64, ioctl.h:44) → `-EINVAL`. `ACQUIRE`(0): `test_and_set_bit` on the device bitmap; on success also set the per-fd bit (global-then-local invariant, chardev.c:369-370); `out.value` 1/0. `ACQUIRE_BLOCKING`(3): `acquire_resource_lock_blocking` (chardev.c:323-367) — **drops `reset_rwsem` across the wait** (deadlock/starvation rationale in comment chardev.c:318-322), `wait_event_interruptible` until the bit frees, the device detaches, or `reset_gen` changes; signal → `-ERESTARTSYS`; detach/reset (including a race after winning the bit, which is given back) → `-ENODEV` (chardev.c:346-363). `RELEASE`(1): only if the per-fd bit was set (local-then-global clear, wake waiters); `out.value` 1/0. `TEST`(2): `out.value` bit 0 = held by this fd, bit 1 = held by any fd (chardev.c:412-416). Unknown flags → `-EINVAL`.
- **SET_NOC_CLEANUP** (chardev.c:432-476): `-EOPNOTSUPP` if `!dev_class->noc_write32`; `argsz != sizeof` → `-EINVAL`; `flags != 0` → `-EINVAL`; `enabled > 1` → `-EINVAL`; `addr & 0x3` → `-EINVAL`; `noc > 1` → `-EINVAL`; `x > 64 || y > 64` → `-EINVAL` (coordinate check flagged as TODO, chardev.c:467). Stores the whole struct into `priv->noc_cleanup` under `priv->mutex`.
- **SET_POWER_STATE** (chardev.c:562-589): `argsz` exact match; `flags != 0 || reserved0 != 0` → `-EINVAL`; `validity > TT_POWER_VALIDITY(15, 14)` → `-EINVAL`; stores into `priv->power_state` under `priv->mutex`; then re-aggregates (return value of `set_power_state` propagates to the caller).

> **Porting note:** The `_IO(0xFA, n)` codes become `CTL_CODE(FILE_DEVICE_..., function, METHOD_BUFFERED/NEITHER, FILE_ANY_ACCESS)` values; the "caller-specified `output_size_bytes`, zero-fill, truncate-copy" protocol maps naturally onto METHOD_BUFFERED with `OutputBufferLength`, but note the Linux protocol takes the output size from a *field inside the input struct*, not from the syscall — a compatible UMD port must keep that field working or translate. `reset_rwsem` (writer-fair rwsem, shared for normal ioctls, exclusive for reset) maps to an `ERESOURCE`. `-ERESTARTSYS` (transparent restart after signal) has no Windows equivalent; blocking acquires should be alertable/cancellable and return `STATUS_CANCELLED`.

---

## 6. mmap dispatch and the offset-space encoding

### Entry: `tt_cdev_mmap` (chardev.c:708-736)

```c
if (!down_read_trylock(&tt_dev->reset_rwsem))
	return -ENODEV;                                   // chardev.c:717-718
```
Trylock (not a blocking lock) to avoid ABBA deadlock: mmap is called with `mmap_lock` held, and `tenstorrent_vma_zap()` (run under `reset_rwsem`) takes `mmap_lock` (comment chardev.c:714-716). Then the same `detached` and `reset_gen` gates as ioctl, both `-ENODEV` (chardev.c:720-729), then `tenstorrent_mmap(priv, vma)`.

### Offset space (`tenstorrent_mmap`, memory.c:1585-1636)

One 64-bit offset space multiplexes every mappable entity; each mapping must sit entirely inside one entity (comment memory.c:1589-1594). The constants are nominally dynamic (userspace learns them from QUERY_MAPPINGS / ALLOCATE_DMA_BUF / ALLOCATE_TLB) but actually hard-coded (comment memory.c:255-257):

```c
#define MMAP_OFFSET_RESOURCE0_UC	(U64_C(0) << 36)   // memory.c:258
#define MMAP_OFFSET_RESOURCE0_WC	(U64_C(1) << 36)
#define MMAP_OFFSET_RESOURCE1_UC	(U64_C(2) << 36)
#define MMAP_OFFSET_RESOURCE1_WC	(U64_C(3) << 36)
#define MMAP_OFFSET_RESOURCE2_UC	(U64_C(4) << 36)
#define MMAP_OFFSET_RESOURCE2_WC	(U64_C(5) << 36)
#define MMAP_OFFSET_TLB_UC		(U64_C(6) << 36)
#define MMAP_OFFSET_TLB_WC		(U64_C(7) << 36)   // memory.c:265
#define MMAP_RESOURCE_SIZE (U64_C(1) << 36)         // memory.c:267
#define MMAP_OFFSET_DMA_BUF		((u64)(PAGE_SIZE-U8_MAX-1) << 32)   // memory.c:272 = 0xF00 << 32 for 4K pages
#define MMAP_SIZE_DMA_BUF (U64_C(1) << 32)          // memory.c:274
```

Dispatch order (first match wins; `vma_target_range` checks that `[vm_pgoff, vm_pgoff+len)` is contained in the region and **rebases `vm_pgoff` to region-relative**, memory.c:1330-1342):

1. `RESOURCE0_UC/WC` → PCI **BAR 0** (`pci_resource_len(pdev, 0)` bounds), `pgprot_device` (UC) or `pgprot_writecombine` (WC), `map_pci_bar(priv, vma, 0, ...)` (memory.c:1596-1602).
2. `RESOURCE1_UC/WC` → PCI **BAR 2** (memory.c:1604-1610).
3. `RESOURCE2_UC/WC` → PCI **BAR 4** (memory.c:1612-1618).
4. `TLB_UC/WC` (full `1<<36` window) → `map_tlb_window(priv, vma, UC/WC)` (memory.c:1620-1626).
5. Otherwise, DMA-buffer space: `vma_dmabuf_target` (memory.c:1344-1368) requires `vm_pgoff >= MMAP_OFFSET_DMA_BUF >> PAGE_SHIFT`, computes `dmabuf_index = (vm_pgoff - base_pg) / (MMAP_SIZE_DMA_BUF >> PAGE_SHIFT)`, rejects `index >= TENSTORRENT_MAX_DMA_BUFS` (256, ioctl.h:41), looks the buffer up in **this fd's** hashtable, and checks containment; success → `dma_mmap_coherent(&pdev->dev, vma, dmabuf->ptr, dmabuf->phys, dmabuf->size)` (memory.c:1629-1632). No match anywhere → `-EINVAL` (memory.c:1634).

Note the mapping ids: "RESOURCE0/1/2" are BAR indices 0/2/4 (the three 64-bit BARs), not 0/1/2.

### How ioctl.h values relate

- **QUERY_MAPPINGS** returns, for each BAR with non-zero length, a `tenstorrent_mapping { mapping_id, mapping_base, mapping_size }` where `mapping_id` is `TENSTORRENT_MAPPING_RESOURCE{0,1,2}_{UC,WC}` (1-6, ioctl.h:33-39) and `mapping_base` is exactly the `MMAP_OFFSET_*` constant (memory.c:352-391). Up to 6 mappings; caller-supplied `output_mapping_count` extras are zero-filled (memory.c:393-406, overflow-checked at 397-398 → `-EFAULT`).
- **ALLOCATE_DMA_BUF** returns `out.mapping_offset = MMAP_OFFSET_DMA_BUF + buf_index * MMAP_SIZE_DMA_BUF` (memory.c:421-423); `buf_index` is a `u8` chosen by the caller (`[0, TENSTORRENT_MAX_DMA_BUFS)`, ioctl.h:93).
- **ALLOCATE_TLB** returns `out.mmap_offset_uc = MMAP_OFFSET_TLB_UC + encoded_id` and `out.mmap_offset_wc = MMAP_OFFSET_TLB_WC + encoded_id`, where `encoded_id = tlb_desc.bar_offset`, plus `BAR0_SIZE` (`1UL << 29`, memory.c:29) if the window lives in BAR 4 (Blackhole 4G windows) (memory.c:918-934). I.e., **the TLB offset encodes the window's byte offset within its BAR, not the TLB id**; `map_tlb_window` reverses this by searching all windows for a matching `bar_offset` (memory.c:1494-1534), then checks: window not owned by this fd → `-EPERM` (memory.c:1541-1543); `size > tlb_desc.size` → `-EINVAL`; window past BAR end → `-ENXIO`; `io_remap_pfn_range` failure → `-EAGAIN`. Successful BAR and TLB mappings are recorded as `struct tenstorrent_mmap_vma` on `priv->vma_list` under `priv->vma_lock`, with `vm_ops` hooks that keep the list correct across `fork()` (vma_open duplicates the tracking node; on allocation failure the child's mapping is zapped, memory.c:1370-1410) and unmap (vma_close unlinks and frees, memory.c:1412-1434).

The vma tracking exists so `tenstorrent_vma_zap(tt_dev)` (memory.c:1677-1729+) can, on reset/removal, walk every open fd's `vma_list` under `chardev_mutex` and zap the PTEs of all **BAR and TLB** mappings (lock-ordering dance with `mmap_lock` documented at memory.c:1683-1689, modeled on VFIO). Subsequent user access faults instead of reaching dead hardware. DMA-buffer mappings are *not* tracked and not zapped (they are host RAM and stay valid).

> **Porting note:** Windows has no `mmap` on a device handle with a 64-bit routing offset. The conventional KMDF replacement is an ioctl that maps the requested entity into the caller's address space (`MmMapLockedPagesSpecifyCache` on an MDL built with `MmBuildMdlForNonPagedPool`/`IoAllocateMdl` for BAR space, `MmCached`/`MmNonCached`/`MmWriteCombined` for the UC/WC split) and returns the user VA — the offset *encoding* can be kept as the ioctl input so UMD's bookkeeping survives. Two hard requirements to preserve: (a) per-fd ownership checks (TLB windows mappable only by the allocating fd; DMA buffers looked up in the per-fd table), and (b) revocation on reset — Windows user mappings created via MDLs cannot be "zapped" to fault; the port must either unmap them (`MmUnmapLockedPages`, requires tracking user VAs and process context) or interpose a section object it can dereference. This is one of the largest behavioral deltas of the whole port.

---

## 7. Post-reset fd invalidation: mechanism and the narrow exception

Two independent flags plus a generation counter, all on `struct tenstorrent_device`:

- `bool detached` — set `true` under `chardev_mutex` in `tenstorrent_pci_remove` (enumerate.c:423-425); "No longer valid for hardware access" (device.h:33). Permanent: every ioctl (chardev.c:604-607) and mmap (chardev.c:720-723) returns `-ENODEV`; there are **no exemptions** for detached.
- `atomic_long_t reset_gen` (device.h:35) with per-fd snapshot `priv->open_reset_gen` taken at open (chardev.c:812). `bump_reset_gen` (chardev.c:195-198):
  ```c
  priv->open_reset_gen = atomic_long_inc_return(&priv->device->reset_gen);
  ```
  increments the device generation **and updates the calling fd's snapshot in the same statement** — so after CONFIG_WRITE / USER_RESET / ASIC_RESET / ASIC_DMC_RESET, *every fd except the resetter's* becomes permanently invalid (`-ENODEV` from all ioctls, chardev.c:609-613, and mmap, chardev.c:725-729), while the resetter "keeps a live fd so it can complete the reset sequence without a close/reopen window" (comment chardev.c:192-194). `RESTORE_STATE` and `RESET_PCIE_LINK` do **not** bump the generation. There is no exemption from the generation gate — the "surviving" fd survives because its snapshot was moved, not because any ioctl bypasses the check.
- `bool needs_hw_init` — set by USER_RESET / ASIC_RESET / ASIC_DMC_RESET (chardev.c:258, 263, 268), cleared by POST_RESET (chardev.c:274-275) (also set at probe if initial `init_hardware` fails, enumerate.c:370). While set, the dispatcher rejects everything with `-ENODEV` **except exactly three ioctls**: `GET_DEVICE_INFO`, `GET_DRIVER_INFO`, and `RESET_DEVICE` (chardev.c:615-624). This is the narrow post-reset exception: between the destructive reset and the `POST_RESET` re-init call, the surviving fd can only query info and drive the reset state machine.

Stale fds are also excluded from power aggregation (chardev.c:493-495), and blocked `ACQUIRE_BLOCKING` waiters are kicked awake to observe the change (`wake_up_interruptible` at chardev.c:299 for reset, enumerate.c:461-463 for removal) and fail with `-ENODEV` (chardev.c:353-363). Resource-lock **bits survive reset** — a stale fd cannot release them by ioctl (it gets `-ENODEV` first); only `close()` clears them (comment chardev.c:312-316, release path chardev.c:877-885).

Expected kernel-supported flow: fd A issues a destructive reset (gen bumps, all other fds die, vmas zapped, `needs_hw_init` set) → external reset actions → fd A issues POST_RESET (marker checked, hardware re-inited) → fd A is fully usable; everyone else must reopen. Note that tt-umd's warm reset does *not* actually hold one fd across the sequence: it issues ASIC_RESET/ASIC_DMC_RESET and later POST_RESET (tt-umd/device/warm_reset.cpp:212-214, 237) via `send_reset_ioctl`, which opens a fresh fd per ioctl (tt-umd/device/pcie/pci_device.cpp:200-216). A fresh fd works because its `open_reset_gen` snapshot is taken after the bump and RESET_DEVICE is on the `needs_hw_init` allowlist. USER_RESET is defined in UMD (pci_device.hpp:95) but not issued by the warm-reset path.

> **Porting note:** The port must reproduce all three predicates with the same precedence (detached → generation → needs_hw_init) and the exact three-ioctl allowlist, and must keep "resetter's handle survives" semantics — the kernel contract explicitly supports driving RESET_DEVICE and POST_RESET from one handle (chardev.c:192-194), though current tt-umd opens a fresh handle per reset ioctl (tt-umd/device/pcie/pci_device.cpp:200-216). `-ENODEV` maps naturally to `STATUS_DEVICE_REMOVED` / `STATUS_DEVICE_NOT_CONNECTED`.

---

## 8. `struct chardev_private` field by field (chardev_private.h:55-78)

| Field | Type / decl | Purpose | Cite |
|---|---|---|---|
| `device` | `struct tenstorrent_device *` | Owning device; kref'd at open, put at release | chardev_private.h:56, chardev.c:810-811, 952 |
| `mutex` | `struct mutex` | Per-fd lock guarding dmabufs table, pinnings, peer_mappings, `noc_cleanup`, `power_state`, TLB ownership checks | chardev_private.h:57; e.g. chardev.c:471-473, 584-586; memory.c:1539, 1647 |
| `dmabufs` | `DECLARE_HASHTABLE(dmabufs, 4)` | Per-fd DMA buffers keyed by `dmabuf.index` (u8), chained on `dmabuf.hash_chain`; 16 buckets | chardev_private.h:42, 58; memory.c:411-419 |
| `pinnings` | `struct list_head` | `struct pinned_page_range.list` — user pages pinned via PIN_PAGES (struct at memory.h:22-34) | chardev_private.h:59 |
| `peer_mappings` | `struct list_head` | `struct peer_resource_mapping.list` — peer-BAR DMA mappings from MAP_PEER_BAR (struct at memory.c:316-321) | chardev_private.h:60 |
| `vma_list` | `struct list_head` | Tracked BAR/TLB `struct tenstorrent_mmap_vma` mappings (struct at chardev_private.h:21-40: `type` TT_VMA_BAR/TT_VMA_TLB, `cache_mode` UC/WC, union of `{bar_index, offset, size}` or `{tlb id}`) | chardev_private.h:62 |
| `vma_lock` | `struct mutex` | Protects `vma_list`; ordering: `mmap_lock` before `vma_lock` | chardev_private.h:63; memory.c:1683-1689 |
| `pid` | `struct pid *` | Group-leader pid, refcounted (`get_pid`/`put_pid`); shown in procfs `pids` | chardev_private.h:65; chardev.c:814, 953; enumerate.c:236-237 |
| `comm` | `char comm[TASK_COMM_LEN]` | Process name at open, diagnostics | chardev_private.h:66; chardev.c:815 |
| `resource_lock` | `DECLARE_BITMAP(..., 64)` | Bits of the device-wide advisory locks this fd holds (device copy at device.h:50) | chardev_private.h:68 |
| `open_fd` | `struct list_head` | Node in `tt_dev->open_fds_list`, guarded by `tt_dev->chardev_mutex` | chardev_private.h:70; device.h:57 |
| `tlbs` | `DECLARE_BITMAP(..., 256)` | Inbound TLB windows owned by this fd (`TENSTORRENT_MAX_INBOUND_TLBS` = 256, ioctl.h:42); set on ALLOCATE_TLB success (memory.c:941) | chardev_private.h:72 |
| `noc_cleanup` | `struct tenstorrent_set_noc_cleanup` | Registered close-time NOC write (ioctl.h:349-359) | chardev_private.h:74 |
| `power_state` | `struct tenstorrent_power_state` | This fd's power request, input to aggregation (ioctl.h:396-411) | chardev_private.h:75 |
| `open_reset_gen` | `long` | `reset_gen` snapshot at open; mismatch → `-ENODEV` | chardev_private.h:77; chardev.c:812 |

> **Porting note:** This struct is the natural content of a WDF file-object context (`WdfObjectGetTypedContext` on the `WDFFILEOBJECT`). The per-device analogues (`open_fds_list`, `chardev_mutex`, `chardev_excl_held`, `resource_lock` bitmap + waitqueue, `reset_gen`, `reset_rwsem`, `power_down_work`, `kref`) live in the device context. The kref pattern — device context must outlive PnP removal until the last file handle closes — is handled differently in KMDF (framework keeps the WDFDEVICE and its context alive until all file objects are closed), but the *hardware-access* cutoff (`detached`) still needs an explicit flag set in `EvtDeviceReleaseHardware`/surprise-removal paths.

---

## 9. Interaction with device removal (for completeness)

`tenstorrent_pci_remove` (enumerate.c:404-481): sets `detached = true` under `chardev_mutex` (fencing new deferred-powerdown arms, enumerate.c:415-425), `cancel_delayed_work_sync(power_down_work)` (427), conditionally `cleanup_hardware` (429-434), drains in-flight ioctls with `down_write(&reset_rwsem)` around `tenstorrent_vma_zap` + `cleanup_device` (BAR unmap) (442-447), revokes TLB dma-buf exports (449-459), wakes blocked lock waiters (461-463), then calls `tenstorrent_memory_cleanup(priv)` for every still-open fd (465-467) — so DMA/pinned-page resources are torn down at remove even though the fds remain open; the later `tt_cdev_release` re-runs `tenstorrent_memory_cleanup` against now-empty lists (safe: lists/table are emptied as they are cleaned) and skips hardware-touching steps via the `detached` checks. The device struct itself is freed only when the last fd drops its kref (enumerate.c:483-497).

---

## Key constants table

| Name | Value | Source |
|---|---|---|
| `TENSTORRENT` (class/region/node name) | `"tenstorrent"` | enumerate.h:13 |
| Device node path pattern | `/dev/tenstorrent/<ordinal>` | chardev.c:106; tt-umd/device/pcie/pci_device.cpp:355 |
| `max_devices` (module param) | 32 (default) | module.c:36-38 |
| `TENSTORRENT_IOCTL_MAGIC` | `0xFA` | ioctl.h:12 |
| Ioctl sequence numbers | 0-16, `_IO()` (no size encoding) | ioctl.h:14-30 |
| `TENSTORRENT_DRIVER_VERSION` (API) | 2 | ioctl.h:10 |
| Driver version (major.minor.patch) | 2.10.1 (`"-pre"` suffix) | module.h:19-22 |
| `TT_POWER_FLAG_ALL` | `0x7FFF` | chardev.c:29 |
| `TT_POWER_FLAG_MAX_AI_CLK` | `1U << 0` | ioctl.h:406 |
| Legacy-open default `power_flags` | `0x7FFE` (`ALL & ~MAX_AI_CLK`) | chardev.c:822-823 |
| Legacy/initial `validity` | `TT_POWER_VALIDITY(15, 0)` = `0x0F` | chardev.c:821; ioctl.h:401-404 |
| `power_policy` (module param) | `true` (default) | module.c:52-54 |
| `idle_power_down_grace_ms` (module param) | 5000 (default; 0 = synchronous powerdown) | module.c:56-58; device.h:116-122 |
| `TENSTORRENT_RESOURCE_LOCK_COUNT` | 64 | ioctl.h:44 |
| `TENSTORRENT_MAX_DMA_BUFS` | 256 | ioctl.h:41 |
| `TENSTORRENT_MAX_INBOUND_TLBS` | 256 | ioctl.h:42 |
| `DMABUF_HASHTABLE_BITS` | 4 (16 buckets) | chardev_private.h:42 |
| `MAX_DMA_BUF_SIZE_LOG2` | 28 (256 MiB max per DMA buf) | memory.h:10 |
| `MMAP_OFFSET_RESOURCE0_UC` … `RESOURCE2_WC` | `(0…5) << 36` (BARs 0/2/4, UC/WC pairs) | memory.c:258-263 |
| `MMAP_OFFSET_TLB_UC` / `_WC` | `6 << 36` / `7 << 36` | memory.c:264-265 |
| `MMAP_RESOURCE_SIZE` | `1 << 36` | memory.c:267 |
| `MMAP_OFFSET_DMA_BUF` | `(PAGE_SIZE - 256) << 32` = `0xF00_0000_0000` (4K pages) | memory.c:272 |
| `MMAP_SIZE_DMA_BUF` | `1 << 32` per buffer slot | memory.c:274 |
| `BAR0_SIZE` (TLB-offset BAR4 bias) | `1UL << 29` (512 MiB) | memory.c:29, 929-931 |
| `TENSTORRENT_MAPPING_RESOURCE0_UC` … | ids 1-6 (`UNUSED` = 0) | ioctl.h:33-39 |
| Reset flags | RESTORE_STATE=0, RESET_PCIE_LINK=1, CONFIG_WRITE=2, USER_RESET=3, ASIC_RESET=4, ASIC_DMC_RESET=5, POST_RESET=6 | ioctl.h:143-151 |
| Post-reset (`needs_hw_init`) ioctl allowlist | GET_DEVICE_INFO, GET_DRIVER_INFO, RESET_DEVICE | chardev.c:616-624 |
| RESET_DEVICE `out.result` | `!ok` — 0 = success | chardev.c:293 |
| NOC cleanup validation bounds | `addr` 4-byte aligned, `noc ≤ 1`, `x,y ≤ 64`, `enabled ≤ 1` | chardev.c:455-469 |

## Open questions

1. **NOC cleanup write vs reset state:** `tt_cdev_release_noc_cleanup` gates only on `tt_dev->detached` (chardev.c:869) — not on `reset_gen` or `needs_hw_init`. A stale pre-reset fd closing while the device is mid-reset (`needs_hw_init == true`) still performs its `noc_write32` into hardware that may be in reset. Intentional (harmless write) or an oversight? A Windows port should decide whether to add a `needs_hw_init` guard or replicate exactly.
2. **GET_HARVESTING (ioctl nr 1)** has no handler; the case falls through with `ret` still `-EINVAL` (chardev.c:631-632). Is the code reserved for compatibility (older kmd/umd)? The port must at minimum reserve the function number; unclear whether any UMD version still probes it.
3. **32-bit userspace:** no `.compat_ioctl` (chardev.c:40-46) means 32-bit processes cannot use the driver on 64-bit kernels. Should the Windows port explicitly reject WOW64 callers, or thunk? (All ioctl structs appear layout-identical across widths — fixed-width types with explicit reserved fields — but this was not exhaustively verified for every struct.)
4. **open() during removal race:** `tt_cdev_open` never checks `detached`; an open racing `cdev_device_del` can succeed and immediately get `-ENODEV` on all operations while still triggering the power-aggregation attempt guard (`!detached` check at chardev.c:852). The exact Linux race window is closed by cdev refcounting; the KMDF equivalent behavior (create arriving during surprise removal) needs an explicit decision.
5. **`x > 64 || y > 64` NOC coordinate validation** is flagged `TODO: Implement a more robust coordinate validation scheme` (chardev.c:467-469) — bounds are not per-ASIC. Port should track upstream if this tightens.
6. **`out.value` width in LOCK_CTL TEST:** `test_bit` returns int; `(test_bit(...) << 1) | test_bit(...)` stores into a `__u8` (chardev.c:414-415, ioctl.h:242). Unambiguous today, but worth pinning in the ported ABI definition.
7. **O_APPEND semantics on Windows:** tt-umd currently opens with `O_RDWR | O_CLOEXEC | O_APPEND` (tt-umd/device/pcie/pci_device.cpp:204) but one call site notes "O_APPEND is temporarily disabled to investigate NOC1 issues" (pci_device.cpp:379). The port needs an agreed replacement signal for power-aware clients (create-time flag vs explicit ioctl), coordinated with the UMD Windows port.
