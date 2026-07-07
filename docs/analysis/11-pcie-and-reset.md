# 11. PCIe Link Management and Device Reset

## Scope

Files read in full (line counts from the baseline tree, tag `ttkmd-2.10.0-rc1-1-g8c32c2b`):

| File | Lines | Coverage in this section |
|---|---|---|
| `pcie.c` | 158 | entire file |
| `pcie.h` | 18 | entire file |
| `ioctl.h` | 458 | reset flags + `tenstorrent_reset_device` structs (ioctl.h:142-166), EXPORT_TLB_DMABUF reset rationale (ioctl.h:413-448) |
| `chardev.c` | 966 | RESET_DEVICE handler, reset-gen machinery, ioctl gatekeeping, open/release interaction |
| `enumerate.c` | 545 | probe/remove/shutdown/suspend/resume, reboot notifier |
| `module.c` | 121 | `reset_limit`, `auto_reset_timeout` module params |
| `module.h` | 38 | param externs |
| `wormhole.c` | 1084 | `wormhole_reset`, FW messaging, init_hardware, save/restore reset state |
| `blackhole.c` | 840 | `blackhole_reset`, init_hardware, save/restore reset state |
| `device.h` | 127 | reset-related fields of `struct tenstorrent_device` |
| `chardev_private.h` | 82 | `open_reset_gen` |
| `memory.c` | 1742 | `tenstorrent_vma_zap`, dmabuf-export revoke/query (memory.c:1281-1326, 1677-1742), VMA ops (memory.c:1370-1439) |
| `tools/reset.c` (tt-kmd) | 295 | reference userspace consumer of ASIC_RESET/POST_RESET |
| `tt-umd/device/warm_reset.cpp` | 710 | reference userspace reset orchestration |
| `tt-umd/device/pcie/pci_device.cpp` | (relevant parts) | how UMD issues the reset ioctl |

---

## 1. Reset-related device and fd state

`struct tenstorrent_device` carries four fields that drive the whole reset model (device.h:33-36):

```c
bool detached; // No longer valid for hardware access
bool needs_hw_init;
atomic_long_t reset_gen; // Generation counter, incremented on reset
struct rw_semaphore reset_rwsem;
```

Each open fd records the generation it was opened under: `long open_reset_gen; // Reset generation at open time` (chardev_private.h:77), assigned in `tt_cdev_open` as `private_data->open_reset_gen = atomic_long_read(&tt_dev->reset_gen);` (chardev.c:812). At probe, `reset_gen` starts at 0 (`atomic_long_set(&tt_dev->reset_gen, 0);` enumerate.c:309) and `reset_rwsem` is initialized (enumerate.c:310). `needs_hw_init` is initialized true at probe start (enumerate.c:305) and finalized as `tt_dev->needs_hw_init = !device_class->init_hardware(tt_dev);` (enumerate.c:370). Telemetry is only initialized at probe when `!needs_hw_init` (enumerate.c:382-383).

### Concurrency model: `reset_rwsem`

- Every ioctl entry takes `reset_rwsem`: RESET_DEVICE takes it **exclusive** (`down_write`), all other ioctls **shared** (`down_read`) (chardev.c:596-601). Released at chardev.c:700-703.
- `mmap` takes it shared with **trylock**; failure returns `-ENODEV` (deliberate, to avoid an ABBA deadlock with `mmap_lock` held by the kernel while `tenstorrent_vma_zap()` needs `mmap_lock` under `reset_rwsem`) (chardev.c:713-718).
- `open` takes it shared across the deferred-powerdown cancel and initial power aggregation (chardev.c:838, 858).
- `release` takes it shared across all device-touching cleanup (chardev.c:929, 950).
- `tenstorrent_pci_remove` takes it exclusive to drain in-flight ioctls before unmapping BARs (enumerate.c:444-447).
- `LOCK_CTL ACQUIRE_BLOCKING` *drops* the shared hold across its wait and reacquires before returning, because holding it across `wait_event_interruptible` would deadlock the writer-fair rwsem against RESET_DEVICE (chardev.c:318-344).

### Gatekeeping at ioctl/mmap entry

Executed under the rwsem, in this order (chardev.c:603-624):

1. `if (priv->device->detached) return -ENODEV;` — fd from a removed/hotplugged device is permanently invalid (chardev.c:604-607).
2. `if (atomic_long_read(&priv->device->reset_gen) != priv->open_reset_gen) return -ENODEV;` — fd opened before a generation-bumping reset is permanently invalid (chardev.c:609-613).
3. While `needs_hw_init` is set ("reset window"), only `GET_DEVICE_INFO`, `GET_DRIVER_INFO` and `RESET_DEVICE` are allowed; everything else returns `-ENODEV` (chardev.c:616-624).

`tt_cdev_mmap` applies checks 1 and 2 but **not** the `needs_hw_init` allowlist (chardev.c:720-729).

`bump_reset_gen()` increments the device generation *and carries the caller's fd along*:

```c
static void bump_reset_gen(struct chardev_private *priv)
{
	priv->open_reset_gen = atomic_long_inc_return(&priv->device->reset_gen);
}
```
(chardev.c:195-198). Per the comment above it: "Other fds become permanently invalid, but the resetter keeps a live fd so it can complete the reset sequence without a close/reopen window." (chardev.c:192-194).

> **Porting note:** `reset_rwsem` maps naturally to a Windows `ERESOURCE` (shared/exclusive). `reset_gen`/`open_reset_gen` is a plain generation counter per device vs. per-file-object context and ports directly. `detached` corresponds to surprise-removal state (WDF `EvtDeviceSurpriseRemoval` / `WdfObjectAcquireLock` patterns). The `-ENODEV` mapping is typically `STATUS_DEVICE_REMOVED`/`STATUS_NO_SUCH_DEVICE`.

---

## 2. The ioctl surface: flags and struct

`TENSTORRENT_IOCTL_RESET_DEVICE` is `_IO(0xFA, 6)` (ioctl.h:12, 20). Payload (ioctl.h:153-166):

```c
struct tenstorrent_reset_device_in {
	__u32 output_size_bytes;
	__u32 flags;
};
struct tenstorrent_reset_device_out {
	__u32 output_size_bytes;
	__u32 result;
};
```

The flag values, with the explicit legacy/current split from the header (ioctl.h:142-151):

```c
// legacy tenstorrent_reset_device_in.flags
#define TENSTORRENT_RESET_DEVICE_RESTORE_STATE 0
#define TENSTORRENT_RESET_DEVICE_RESET_PCIE_LINK 1
#define TENSTORRENT_RESET_DEVICE_CONFIG_WRITE 2

// tenstorrent_reset_device_in.flags
#define TENSTORRENT_RESET_DEVICE_USER_RESET 3
#define TENSTORRENT_RESET_DEVICE_ASIC_RESET 4
#define TENSTORRENT_RESET_DEVICE_ASIC_DMC_RESET 5
#define TENSTORRENT_RESET_DEVICE_POST_RESET 6
```

`flags` is a single enum-style value, not a bitmask — the handler dispatches with `if (in.flags == ...)` chains and returns `-EINVAL` for anything else (chardev.c:240-290). UMD gates use of flags 3-6 on KMD version >= 2.4.1 (`KMD_ARCH_AGNOSTIC_RESET = SemVer{2, 4, 1}`, tt-umd/device/api/umd/device/utils/kmd_versions.hpp:29; tt-umd/device/pcie/pci_device.cpp:1054).

### Handler prologue and epilogue (common to all flavors)

`ioctl_reset_device` (chardev.c:200-310), running with `reset_rwsem` held exclusive:

1. `copy_from_user(&in, ...)` → `-EFAULT` on fault (chardev.c:213-214).
2. **dmabuf -EBUSY gate**: for the destructive flags — `RESET_PCIE_LINK`, `CONFIG_WRITE`, `USER_RESET`, `ASIC_RESET`, `ASIC_DMC_RESET` — refuse with `-EBUSY` if `tenstorrent_has_tlb_dmabuf_exports(tt_dev)` (chardev.c:222-233). `RESTORE_STATE` and `POST_RESET` are exempt ("the re-init halves of a reset sequence, not destructive", chardev.c:219-221). Rationale (ioctl.h:429-433): "Resetting the device under in-flight P2P DMA can wedge the host hard enough to require out-of-band recovery, and a pin-only importer cannot be revoked... RESET_DEVICE is therefore refused with -EBUSY while any export is live." The predicate is just a locked `!list_empty(&tt_dev->dmabuf_exports)` (memory.c:1299-1308). On kernels < 5.8 the export ioctl is `-EOPNOTSUPP` and the predicate is hardwired `false` (memory.c:1312-1324).
3. `cancel_delayed_work_sync(&tt_dev->power_down_work)` — drains any deferred idle powerdown before disturbing the device; safe because open()/release() take the rwsem shared and reset holds it exclusive (chardev.c:235-238).
4. Flavor dispatch (below), producing `bool ok`.
5. `out.result = !ok;` — **result 0 = success, 1 = failure. A failed reset still returns 0 from the ioctl**; only validation errors produce negative errno (chardev.c:292-293).
6. `wake_up_interruptible(&tt_dev->resource_lock_waitqueue)` so `ACQUIRE_BLOCKING` waiters re-check `reset_gen` and return `-ENODEV` instead of waiting forever (chardev.c:295-299; waiter side chardev.c:331-363).
7. `clear_user(&arg->out, in.output_size_bytes)` then `copy_to_user` of `min(in.output_size_bytes, sizeof(out))` bytes → `-EFAULT` on fault (chardev.c:301-307). Note `output_size_bytes` is caller-controlled and un-capped; a huge value that fits in the user mapping just zeroes that much of the caller's buffer.

---

## 3. Low-level PCIe primitives (pcie.c)

### 3.1 `poll_pcie_link_up(pdev, timeout_ms)` (pcie.c:24-41)

Polls config-space `PCI_VENDOR_ID` until it reads `PCI_VENDOR_ID_TENSTORRENT` (`0x1E52`, enumerate.h:15), sleeping `msleep(100)` between reads, bounded by `timeout_ms` via `ktime`. Returns false on timeout.

### 3.2 `safe_pci_restore_state(pdev)` (pcie.c:43-59)

```c
if (!pdev->state_saved)
	return false;
// Start with a test read. pci_restore_state calls pci_find_next_ext_capability which has
// a bounded loop that is still long enough to trigger a soft lockup warning if hardware
// is extremely misbehaving.
if (pci_read_config_word(pdev, PCI_VENDOR_ID, &vendor_id) != PCIBIOS_SUCCESSFUL
    || vendor_id != PCI_VENDOR_ID_TENSTORRENT)
	return false;
pci_restore_state(pdev);
pci_save_state(pdev);
```

Restores the config space snapshot taken with `pci_save_state()` and immediately re-saves it (Linux clears `state_saved` on restore; re-saving keeps the snapshot valid for the next reset). The snapshot is first taken at probe (`pci_save_state(dev);` enumerate.c:372) and re-taken after resume (enumerate.c:519) and before each retry inside `wormhole_complete_pcie_init` (pcie.c:125).

### 3.3 `pcie_hot_reset_and_restore_state(pdev)` — Secondary Bus Reset (pcie.c:61-90)

The only in-driver link reset. Exact sequence:

1. `bridge_dev = pci_upstream_bridge(pdev)`; if none, return false (pcie.c:62, 67-68).
2. `pci_ignore_hotplug(pdev)` so the SBR-induced link-down does not cause the hotplug driver to remove the device (pcie.c:70). The prior value is saved and, if it was clear, both `pdev->ignore_hotplug` and `bridge_dev->ignore_hotplug` are cleared by direct field write afterwards (pcie.c:65, 82-87).
3. Secondary bus reset via the bridge's Bridge Control register — "like pci_reset_secondary_bus, but we don't want the full 1s delay" (pcie.c:72):

```c
pci_read_config_word(bridge_dev, PCI_BRIDGE_CONTROL, &bridge_ctrl);
pci_write_config_word(bridge_dev, PCI_BRIDGE_CONTROL, bridge_ctrl | PCI_BRIDGE_CTL_BUS_RESET);
msleep(2);
pci_write_config_word(bridge_dev, PCI_BRIDGE_CONTROL, bridge_ctrl);
msleep(500);
```
(pcie.c:73-78). `PCI_BRIDGE_CONTROL` is config offset 0x3E, `PCI_BRIDGE_CTL_BUS_RESET` is bit 6 (0x40) — Linux `<uapi/linux/pci_regs.h>` constants. So: **assert SBR 2 ms, deassert, wait 500 ms.**
4. `poll_pcie_link_up(pdev, 10000)` — up to **10 s** for the vendor ID to read back — then `safe_pci_restore_state(pdev)` (pcie.c:80).

There is **no FLR anywhere in the driver** (no `pcie_flr`/`pci_reset_function` calls); SBR + firmware-driven chip resets are the only mechanisms.

### 3.4 `pcie_timer_interrupt(pdev)` — config-write chip reset trigger (pcie.c:133-138)

```c
#define INTERFACE_TIMER_CONTROL_OFF 0x930
#define INTERFACE_TIMER_TARGET_OFF 0x934
#define INTERFACE_TIMER_TARGET 0x1
#define INTERFACE_TIMER_EN 0x1
#define INTERFACE_FORCE_PENDING 0x10
...
pci_write_config_dword(pdev, INTERFACE_TIMER_TARGET_OFF, INTERFACE_TIMER_TARGET);
pci_write_config_dword(pdev, INTERFACE_TIMER_CONTROL_OFF, INTERFACE_TIMER_EN | INTERFACE_FORCE_PENDING);
```
(pcie.c:16-22, 133-138). Two dword writes into the device's own extended config space (Synopsys DWC PCIe controller "interface timer" registers): write `0x1` to offset 0x934, then `0x11` to offset 0x930. This forces a pending timer interrupt inside the chip, which firmware services as a reset request. Always returns true. Used by legacy `CONFIG_WRITE` (chardev.c:253) and by Blackhole `ASIC_RESET` (blackhole.c:566).

### 3.5 Reset marker (pcie.c:140-158)

```c
// pci_command_parity is used as reset marker. Set to 1, check if cleared to 0 after reset
pci_read_config_word(pdev, PCI_COMMAND, &pci_command);
pci_write_config_word(pdev, PCI_COMMAND, pci_command | PCI_COMMAND_PARITY);
```
`set_reset_marker` sets the Parity Error Response bit (bit 6, 0x40) of `PCI_COMMAND` (offset 0x04). `is_reset_marker_zero` reads it back and returns `(pci_command & PCI_COMMAND_PARITY) == 0` (pcie.c:151-158). The scheme relies on a real chip reset clearing config space to defaults (bit 6 resets to 0); a cleared bit therefore proves the reset actually happened. `tools/reset.c` polls exactly this bit from sysfs config space (`(cmd_reg >> 6) & 1) == 0`, tools/reset.c:241-243).

### 3.6 Dead declaration

`void pcie_retrain_link_to_max_speed(struct pci_dev *pdev);` is declared (pcie.h:16) but **defined nowhere and called nowhere** in the tree — do not port it.

> **Porting note:** On Windows, a KMDF function driver cannot legally touch the upstream bridge's config space, so the SBR in 3.3 cannot be re-implemented literally. The equivalent is the PCI bus driver's reset interface (`PCI_DEVICE_RESET_INTERFACE_STANDARD`, `DeviceReset(PLDR/FLR)` / `GUID_DEVICE_RESET_INTERFACE_STANDARD`) requesting a **bus/PLDR reset**; the 2 ms/500 ms/10 s timings above are what the hardware has been validated against and should be preserved where the port controls timing. Config save/restore (`pci_save_state`/`pci_restore_state`) must be re-implemented manually via `BUS_INTERFACE_STANDARD.GetBusData/SetBusData` (the Windows PCI driver restores config on power transitions, not on app-triggered SBR). `pcie_timer_interrupt` and the reset marker are plain config accesses to the device's *own* config space and port directly via GetBusData/SetBusData.

---

## 4. The seven flavors, end to end

All flavors run under exclusive `reset_rwsem`, after the -EBUSY gate and powerdown drain described in section 2.

### 4.1 `RESTORE_STATE` (0, legacy) — chardev.c:240-246

No gen bump, no VMA zap, exempt from the dmabuf gate. Sequence:
1. `safe_pci_restore_state(pdev)` (restore + re-save config space). If it fails (no saved state / device absent), `ok = false`.
2. `dev_class->restore_reset_state(priv->device)` — restore Max Payload Size (section 6).
3. `ok = dev_class->init_hardware(priv->device)` — re-run device init (section 5/6).

Used by legacy UMD flows as the second half after an externally executed reset (tt-umd/device/warm_reset.cpp:290, 347).

### 4.2 `RESET_PCIE_LINK` (1, legacy) — chardev.c:247-249

1. `tenstorrent_vma_zap(tt_dev)` — unmap all user BAR/TLB mappings device-wide (section 7).
2. `ok = pcie_hot_reset_and_restore_state(pdev)` — the SBR sequence of 3.3, including config restore.

**Does not bump `reset_gen` and does not set `needs_hw_init`** — pre-existing fds stay valid; only their mappings are gone. Legacy WH flow: `RESET_PCIE_LINK` first, then UMD talks to ARC FW itself (A3 + TRIGGER_RESET), then `RESTORE_STATE` (tt-umd/device/warm_reset.cpp:294-369). In the current arch-agnostic UMD flow it is only sent when the caller explicitly requests a secondary bus reset (tt-umd/device/warm_reset.cpp:207-209).

### 4.3 `CONFIG_WRITE` (2, legacy) — chardev.c:250-253

1. `bump_reset_gen(priv)` — all other fds invalidated; caller's fd carried forward.
2. `tenstorrent_vma_zap(tt_dev)`.
3. `ok = pcie_timer_interrupt(pdev)` (3.4) — fires the chip-internal reset; always true.

**Does not set `needs_hw_init`.** Legacy BH flow: `CONFIG_WRITE`, poll config command byte until reset observed (up to `BH_WARM_RESET_TIMEOUT`), then `RESTORE_STATE` (tt-umd/device/warm_reset.cpp:241-292).

### 4.4 `USER_RESET` (3) — chardev.c:254-258

1. `bump_reset_gen(priv)`; 2. `tenstorrent_vma_zap(tt_dev)`; 3. `ok = set_reset_marker(pdev)` (always true); 4. `needs_hw_init = true`.

No hardware reset is performed by the driver — the *user* performs the reset by other means (e.g. tt-smi driving FW directly); the driver only arms the marker and enters the reset window. Completion is via `POST_RESET`.

### 4.5 `ASIC_RESET` (4) — chardev.c:259-263

1. `bump_reset_gen(priv)`; 2. `tenstorrent_vma_zap(tt_dev)`; 3. `ok = dev_class->reset(priv->device, in.flags)`; 4. `needs_hw_init = true`.

**Wormhole** (`wormhole_reset`, wormhole.c:473-518):
- Probe responsiveness with FW message `WH_FW_MSG_NOP` (0x11), 1000 µs timeout (wormhole.c:481).
- If unresponsive: if `auto_reset_timeout == 0`, fail immediately with "Watchdog is disabled and device is unresponsive, cannot reset." (wormhole.c:488-491). Otherwise loop until `auto_reset_timeout*1000 + 500` ms elapse: `pcie_hot_reset_and_restore_state(pdev)`, retry NOP (1000 µs), `msleep_interruptible(1000)` between attempts (a signal aborts with `ok=false`) (wormhole.c:493-504). This rides the on-board M3 watchdog auto-reset: keep hot-resetting the link until FW comes back.
- If responsive: `set_reset_marker(pdev)`, then send `WH_FW_MSG_TRIGGER_RESET` (0x56) with `arg0 = 0` for ASIC_RESET (3 for DMC, see 4.6) and **`timeout_us = 0`, i.e. fire-and-forget**; return true — "Assumes the reset was successful." (wormhole.c:508-512). (With `timeout_us == 0` the send helper returns false right after raising the ARC IRQ, wormhole.c:226-227; the return value is deliberately ignored.)
- FW message transport: write `arg0|arg1<<16` to reset-unit `SCRATCH_REG(3)`, write `0xAA00 | msg_id` to `SCRATCH_REG(5)`, set bit 16 of `ARC_MISC_CNTL_REG` (0x100) to raise IRQ0, then poll SCRATCH_REG(5) for the msg id in the low 16 bits with exit code in the high 16 (wormhole.c:108-112, 205-234, 163-197). Skipped entirely (returns false) if ARC L2 post code (`SCRATCH_REG(0)` masked with 0xFFFF0000) != `0xC0DE0000` (wormhole.c:101-103, 214-217).

**Blackhole** (`blackhole_reset`, blackhole.c:542-570): `ASIC_RESET` is `set_reset_marker(pdev)` + `pcie_timer_interrupt(pdev)` (blackhole.c:564-567) — the config-write trigger, no FW handshake.

### 4.6 `ASIC_DMC_RESET` (5) — chardev.c:264-268

Same wrapper as 4.5 (bump gen, zap, `dev_class->reset`, `needs_hw_init = true`).

**Wormhole**: identical to 4.5 except `reset_arg = 3` ("ASIC + M3 reset") passed to `WH_FW_MSG_TRIGGER_RESET` (wormhole.c:477, 510-511).

**Blackhole** (blackhole.c:547-563): via the ARC message queue (not scratch registers):
1. Send `ARC_MSG_TYPE_TEST` (0x90) to confirm FW/NOC alive; on failure log "Couldn't communicate with firmware; NOC is likely hung." and return false (blackhole.c:551-555).
2. `set_reset_marker(pdev)` (blackhole.c:557).
3. Send `ARC_MSG_TYPE_TRIGGER_RESET` (0x56) with `payload[0] = 3` ("Argument for ASIC + M3 reset") and return true — "Possibly a lie..." (blackhole.c:559-563). Queue transport: wait up to `ARC_MSG_READY_MS` (500 ms) for `ARC_BOOT_STATUS` bit 0, read QCB pointer from `RESET_SCRATCH(11)`, push, ring doorbell by writing 0 to `ARC_MSI_FIFO` (0x800B0000), pop reply (blackhole.c:500-540).

### 4.7 `POST_RESET` (6) — the completion path — chardev.c:269-287

This is the narrow "post-reset fd" exception. Exact semantics:

**Who may call it, on which fd.** Any fd of the device that passes the standard gate: device not `detached`, and `open_reset_gen == reset_gen` (chardev.c:604-613). Because the destructive flavors bump the generation, that means exactly two kinds of fd: (a) **the resetter's own fd** (carried forward by `bump_reset_gen`), or (b) **any fd opened after the reset ioctl returned**. It is reachable during the reset window because `RESET_DEVICE` is on the `needs_hw_init` allowlist (chardev.c:616-624), and it is exempt from the dmabuf `-EBUSY` gate (chardev.c:231-232). No capability/ownership check beyond that — any process that can open the node can complete a reset. In practice both reference consumers open a **fresh fd** (UMD: `tt_device_open(..., O_RDWR|O_CLOEXEC|O_APPEND)` per reset call, tt-umd/device/pcie/pci_device.cpp:200-215; tools/reset.c re-finds the device by BDF after it re-enumerates and opens the new node, tools/reset.c:260-289).

**What it does** (chardev.c:269-287):

```c
ok = is_reset_marker_zero(pdev);
// In the hotplug case, needs_hw_init is false and there is nothing to
// do here. Otherwise this was an in-place reset, so re-initialize now.
if (priv->device->needs_hw_init) {
	priv->device->needs_hw_init = false;
	if (ok && safe_pci_restore_state(pdev)) {
		priv->device->dev_class->restore_reset_state(priv->device);
		ok = priv->device->dev_class->init_hardware(priv->device);
		// Re-probe telemetry tag addresses in case
		// firmware was updated before this reset.
		if (ok && priv->device->dev_class->probe_telemetry)
			priv->device->dev_class->probe_telemetry(priv->device);
	} else {
		ok = false;
	}
}
```

1. Verify the marker: `PCI_COMMAND` bit 6 must read 0 (proof the chip's config space was actually reset). If the marker is still set, `ok = false` — and if `needs_hw_init` was set, no re-init is attempted, but `needs_hw_init` is still cleared.
2. If `needs_hw_init` (in-place reset case): clear it **unconditionally**, then restore config space (`safe_pci_restore_state`), restore MPS (`restore_reset_state`), re-run `init_hardware`, and re-probe telemetry (`probe_telemetry`, both classes implement it — wormhole.c:1070, blackhole.c:828).
3. If `!needs_hw_init` (hotplug case — the device disappeared, got re-probed as a fresh instance whose probe already ran `init_hardware`): only the marker check runs.

**What it does not do:** no gen bump, no VMA zap, no marker set. `out.result` reports `!ok` like every flavor.

### End-to-end reference flow (current, arch-agnostic)

From tt-umd/device/warm_reset.cpp:182-239 and tools/reset.c:196-289:

1. (optional) `RESET_PCIE_LINK` if a secondary bus reset was requested (warm_reset.cpp:207-209).
2. `ASIC_DMC_RESET` if M3/DMC reset requested, else `ASIC_RESET` — each on a freshly opened fd, immediately closed (warm_reset.cpp:211-215; pci_device.cpp:200-215).
3. Wait: UMD sleeps `max(2.0, 0.4 * ndevices)` seconds (or the M3 delay) then polls sysfs for the BDF to reappear (warm_reset.cpp:217-235); tools/reset.c polls the marker bit / device disappearance for 5 s (ASIC) or 10 s (DMC), 100 ms period, with a 500 ms pre-delay on WH (tools/reset.c:218-255).
4. `POST_RESET` on a newly opened fd (warm_reset.cpp:237; tools/reset.c:275-288), checking `out.result == 0`.

---

## 5. Effects on other open fds, mmaps, locks, power

- **Other fds:** after a gen bump, every ioctl and mmap on a pre-reset fd returns `-ENODEV` forever (chardev.c:609-613, 726-729). Blocking lock waiters are woken and return `-ENODEV` (chardev.c:295-299, 353-360). `close()` still works and performs full cleanup (`tt_cdev_release`, chardev.c:922-958) — NOC cleanup write is skipped only when `detached` (chardev.c:869), power re-aggregation is skipped when `detached || needs_hw_init` (chardev.c:907-908), and aggregation itself skips stale-generation fds (chardev.c:493-495).
- **Mappings:** `tenstorrent_vma_zap` walks every open fd's `vma_list` under `chardev_mutex` + per-fd `vma_lock`, takes `mmget_not_zero`/`mmap_read_lock` in the VFIO-derived ordering, and `zap_special_vma_range()`s each BAR/TLB VMA (memory.c:1677-1742). The BAR/TLB vm_ops have **no `.fault` handler** (memory.c:1436-1439, 1484-1486), so a post-zap access faults fatally (SIGBUS) rather than re-populating. DMA-buffer mmaps (`dma_mmap_coherent`) are not on `vma_list` and are not zapped.
- **Resource locks:** survive reset — device-global bits are *not* cleared by reset. A pre-reset holder can no longer release via ioctl (its fd is invalid); only `close(fd)` clears its bits (comment chardev.c:312-316, cleanup chardev.c:877-885).
- **Power:** the deferred idle-powerdown work is drained before the reset (chardev.c:238); new arms cannot race in because open/release hold the rwsem shared (comment chardev.c:236-237).

> **Porting note:** `tenstorrent_vma_zap` is the hardest piece to port. Windows has no zap-PTEs primitive for user mappings of device memory created by a driver. Options: (a) map BARs to user space via `MmMapLockedPagesSpecifyCache`+MDL and use `MmUnmapLockedPages` on reset (requires tracking every mapping and works only if the port controls unmap), or (b) rotate to a section-object design where reset marks the section invalid — in either case revocation-on-reset must be designed in from the start, or the port must instead *refuse* destructive resets while any user mapping exists (stricter than Linux, which forcibly revokes).

---

## 6. `save_reset_state` / `restore_reset_state` — what config restore does *not* cover

The chip's Max Payload Size lives in the endpoint's Device Control register but is negotiated by FW and is not recoverable from the host-side `pci_save_state` snapshot alone after a FW-level reset, so the driver snapshots it out-of-band via the chip's own DBI view of its config space:

- **Wormhole** (wormhole.c:968-988): `open_dbi()` writes `DBI_ENABLE` (0x00200000) into reset-unit scratch 6/7 (`PCIE_ARMISC_INFO_REG`/`PCIE_AWMISC_INFO_REG`), then a NOC read/write at node (0,3), address `PCIE_DBI_ADDR = 0x800000000ULL` + `DBI_DEVICE_CONTROL_DEVICE_STATUS` (0x78, pcie.h:8) captures/restores the `PCI_EXP_DEVCTL_PAYLOAD` field into/from `wh->saved_mps`; `close_dbi()` writes 0 back. The comment warns DBI mode disrupts all outbound NOC traffic, so this only happens when quiescent (wormhole.c:956-957).
- **Blackhole** (blackhole.c:304-330): same idea; PCIe NOC x-coordinate detected via `NOC_ID_OFFSET` read (must be 2 or 11, blackhole.c:299-302), DBI at `PCIE_DBI_ADDR = 0xF800000000000000ULL` (an outbound NOC TLB set up by FW, blackhole.c:50-51).

`save_reset_state` is called once at probe (enumerate.c:373); `restore_reset_state` in `RESTORE_STATE` and `POST_RESET` (chardev.c:242, 277).

`init_hardware`, re-run by `RESTORE_STATE`/`POST_RESET`/resume:
- **Wormhole** (wormhole.c:718-735): remap BAR4 through iATU inbound region 1 to `BAR4_SOC_TARGET_ADDRESS` (0x1E000000); if ARC L2 running: send current date (0xB7), `ASTATE0` (0xA0, 10 000 µs), device index (0x51), then `wormhole_complete_pcie_init` (section 8), then `UPDATE_M3_AUTO_RESET_TIMEOUT` (0xBC) with `auto_reset_timeout` seconds (10 000 µs timeout) (wormhole.c:729-731). Always returns true.
- **Blackhole** (blackhole.c:620-639): `pcie_set_readrq(pdev, 4096)` (MAX_MRRS, blackhole.c:18), ARC message `ASIC_STATE0` (0xA0), then `SET_WDT_TIMEOUT` (0xC1) with `1000 * auto_reset_timeout` ms (failure tolerated for old FW). Always returns true.

---

## 7. Module parameters

```c
uint reset_limit = 10;
module_param(reset_limit, uint, 0444);
MODULE_PARM_DESC(reset_limit, "Maximum number of times to reset device during boot.");

unsigned char auto_reset_timeout = 10;
module_param(auto_reset_timeout, byte, 0444);
MODULE_PARM_DESC(auto_reset_timeout, "Timeout duration in seconds for M3 auto reset to occur.");
```
(module.c:44-50, externs module.h:26-27.)

- `reset_limit` is used only by `wormhole_complete_pcie_init` as the retry bound; **0 disables the whole retrain/retry loop** (`if (!bridge_dev || reset_limit == 0) return true;`, pcie.c:98-99).
- `auto_reset_timeout` (u8, seconds) is used three ways: (1) WH unresponsive-device wait budget `auto_reset_timeout*1000 + 500` ms in `wormhole_reset` — 0 means "watchdog disabled, give up immediately" (wormhole.c:488-493); (2) sent to WH FW as message 0xBC arg at every `init_hardware` (wormhole.c:729-731); (3) sent to BH FW as watchdog timeout `1000 * auto_reset_timeout` ms at every `init_hardware` (blackhole.c:633-634).

> **Porting note:** these become registry parameters (e.g. under the device/service Parameters key) read at DriverEntry/AddDevice. Both are read-only after load on Linux (0444), so a load-time read is faithful.

---

## 8. Boot-time PCIe link training loop — `wormhole_complete_pcie_init` (pcie.c:92-131)

Wormhole-only (called from `wormhole_init_hardware`, wormhole.c:728; Blackhole has no equivalent). Skipped when there is no upstream bridge or `reset_limit == 0`. For up to `reset_limit` iterations:

1. Read the **bridge's** `PCI_EXP_LNKCTL2` and mask `PCI_EXP_LNKCTL2_TLS` (Target Link Speed, low 4 bits) (pcie.c:107-108).
2. Read the **bridge's** config word at `PCI_SUBSYSTEM_VENDOR_ID` (0x2C) (pcie.c:110) — see Open questions.
3. Send WH FW message `FW_MSG_PCIE_RETRAIN` (0xB6, pcie.c:16) with `arg0 = target_link_speed | (last_retry << 15)`, `arg1 = subsys_vendor_id`, timeout **200 000 µs**, collecting a 16-bit exit code (pcie.c:112-113). The FW performs the actual link retraining from the endpoint side.
4. `exit_code == 0` → success. Otherwise, unless this was the last retry: `pci_save_state(pdev)` then `pcie_hot_reset_and_restore_state(pdev)` (full SBR of 3.3) and loop (pcie.c:116-127).

Failure of the message send or exhaustion of retries returns false — but note `wormhole_init_hardware` ignores the return value and still returns true (wormhole.c:728, 734).

---

## 9. Reboot notifier, shutdown, suspend/resume, remove

- **Reboot notifier** (enumerate.c:243-251): registered at probe only when the class has a `.reboot` op (enumerate.c:377-380) — Wormhole only (`.reboot = wormhole_cleanup_hardware`, wormhole.c:1073; Blackhole registers none, blackhole.c:813-840). Handler: `if (action != SYS_POWER_OFF) tt_dev->dev_class->reboot(tt_dev);` → on restart/halt (but *not* power-off) send WH FW `ASTATE3` (0xA3) with 10 000 µs timeout, skipped if hardware hung (`wormhole_shutdown_firmware`, wormhole.c:293-301). Unregistered at final kref release (enumerate.c:487-488).
- **`.shutdown = tenstorrent_pci_remove`** (enumerate.c:532): system shutdown runs the full remove path.
- **Remove** (enumerate.c:404-481), reset-relevant ordering: cancel WH fw_ready work; set `detached = true` under `chardev_mutex`; drain `power_down_work`; probe vendor ID and call `cleanup_hardware` (FW → A3) only if it reads != 0xFFFF (hotplug-gone check, enumerate.c:432-434); `cleanup_telemetry`; **`down_write(&reset_rwsem)`; `tenstorrent_vma_zap`; `cleanup_device` (unmap BARs); `up_write`** (enumerate.c:444-447); `tenstorrent_revoke_tlb_dmabufs` (must stay after the drain — long ordering comment enumerate.c:449-458); wake lock waiters; per-fd `tenstorrent_memory_cleanup`; unregister cdev; disable interrupts/device.
- **Suspend** (enumerate.c:499-509): drain powerdown work, `tenstorrent_revoke_tlb_dmabufs`, `cleanup_hardware` (FW → A3). **Resume** (enumerate.c:511-522): `init_hardware`; on success `pci_save_state(pdev)` ("Suspend invalidates the saved state"); returns `-EIO` on failure. Resume does **not** bump `reset_gen` — fds survive suspend/resume.

> **Porting note:** reboot notifier ≈ `EvtDeviceShutdown`/`IRP_MJ_SHUTDOWN` (note Linux deliberately *skips* the A3 message on power-off; Windows shutdown callbacks can distinguish shutdown vs. restart only coarsely). Suspend/resume ≈ D0Exit/D0Entry; the "re-save config after resume" step is unnecessary on Windows only if config restore is delegated to the PCI driver, but the DBI-based MPS restore and `init_hardware` re-run are still required.

---

## Key constants table

| Name | Value | Source |
|---|---|---|
| `TENSTORRENT_IOCTL_RESET_DEVICE` | `_IO(0xFA, 6)` | ioctl.h:12, 20 |
| `TENSTORRENT_RESET_DEVICE_RESTORE_STATE` | 0 (legacy) | ioctl.h:143 |
| `TENSTORRENT_RESET_DEVICE_RESET_PCIE_LINK` | 1 (legacy) | ioctl.h:144 |
| `TENSTORRENT_RESET_DEVICE_CONFIG_WRITE` | 2 (legacy) | ioctl.h:145 |
| `TENSTORRENT_RESET_DEVICE_USER_RESET` | 3 | ioctl.h:148 |
| `TENSTORRENT_RESET_DEVICE_ASIC_RESET` | 4 | ioctl.h:149 |
| `TENSTORRENT_RESET_DEVICE_ASIC_DMC_RESET` | 5 | ioctl.h:150 |
| `TENSTORRENT_RESET_DEVICE_POST_RESET` | 6 | ioctl.h:151 |
| `PCI_VENDOR_ID_TENSTORRENT` | 0x1E52 | enumerate.h:15 |
| SBR assert time | `msleep(2)` (2 ms) | pcie.c:76 |
| SBR post-deassert settle | `msleep(500)` (500 ms) | pcie.c:78 |
| Link-up poll timeout / period | 10 000 ms / 100 ms | pcie.c:80, 36 |
| Reset marker bit | `PCI_COMMAND_PARITY` (bit 6, 0x40, cfg offset 0x04) | pcie.c:144-146 |
| `INTERFACE_TIMER_CONTROL_OFF` | 0x930 (cfg space) | pcie.c:17 |
| `INTERFACE_TIMER_TARGET_OFF` | 0x934 (cfg space) | pcie.c:18 |
| `INTERFACE_TIMER_TARGET` / `_EN` / `FORCE_PENDING` | 0x1 / 0x1 / 0x10 | pcie.c:20-22 |
| `FW_MSG_PCIE_RETRAIN` | 0xB6, timeout 200 000 µs | pcie.c:16, 113 |
| `DBI_DEVICE_CONTROL_DEVICE_STATUS` | 0x78 (DBI offset for DevCtl/MPS) | pcie.h:8 |
| `reset_limit` default | 10 (0 disables retrain loop) | module.c:44; pcie.c:98 |
| `auto_reset_timeout` default | 10 s (u8; 0 = watchdog disabled) | module.c:48; wormhole.c:488 |
| WH unresponsive wait budget | `auto_reset_timeout*1000 + 500` ms, 1 s between attempts | wormhole.c:493, 502 |
| `WH_FW_MSG_NOP` / probe timeout | 0x11 / 1000 µs | wormhole.c:43, 481 |
| `WH_FW_MSG_TRIGGER_RESET` | 0x56, arg 0 = ASIC, 3 = ASIC+DMC(M3) | wormhole.c:42, 477, 510-511 |
| `WH_FW_MSG_UPDATE_M3_AUTO_RESET_TIMEOUT` | 0xBC | wormhole.c:41 |
| `WH_FW_MESSAGE_PRESENT` | 0xAA00 (scratch-5 protocol) | wormhole.c:112 |
| `POST_CODE_ARC_L2` / mask | 0xC0DE0000 / 0xFFFF0000 | wormhole.c:102-103 |
| `ARC_MISC_CNTL_REG` / IRQ0 bit | 0x100 / bit 16 | wormhole.c:105-106 |
| BH `ARC_MSG_TYPE_TRIGGER_RESET` | 0x56, payload[0] = 3 for DMC | blackhole.c:70, 549-561 |
| BH `ARC_MSG_TYPE_SET_WDT_TIMEOUT` | 0xC1, payload = ms | blackhole.c:69, 633-634 |
| BH `ARC_MSG_TYPE_TEST` | 0x90 | blackhole.c:72 |
| BH `ARC_MSG_READY_MS` | 500 ms | blackhole.c:66 |
| BH `MAX_MRRS` (readrq after reset) | 4096 | blackhole.c:18, 626 |
| `out.result` encoding | 0 = success, 1 = failure (`!ok`) | chardev.c:293 |
| dmabuf gate errno | `-EBUSY` on flags 1-5 | chardev.c:222-233 |
| UMD min KMD for flags 3-6 | 2.4.1 | tt-umd/device/api/umd/device/utils/kmd_versions.hpp:29 |

## Open questions

1. **`pcie_retrain_link_to_max_speed`** is declared (pcie.h:16) but never defined or called — presumed dead; confirm before dropping from the port.
2. **Bridge "subsystem vendor ID" read** in `wormhole_complete_pcie_init` (pcie.c:110): `PCI_SUBSYSTEM_VENDOR_ID` (0x2C) on a type-1 (bridge) header is not a Subsystem Vendor ID field (it falls in the prefetchable-limit-upper area). The value is forwarded verbatim to FW as `arg1`; whether FW expects this specific bridge register or the code is a copy-paste of an endpoint offset is unknown. A port must read *bridge config offset 0x2C* to be bit-faithful, but the intent is unverified.
3. **`CONFIG_WRITE` bumps `reset_gen` but does not set `needs_hw_init`**, and **`RESET_PCIE_LINK` zaps VMAs but bumps nothing** (chardev.c:247-253). Both look like frozen legacy-compat behavior (matching UMD's legacy flows that follow with `RESTORE_STATE`); whether a Windows port must reproduce this exact asymmetry or may unify on the flag 3-6 model depends on whether legacy UMD flows must be supported.
4. **`POST_RESET` clears `needs_hw_init` unconditionally** before attempting re-init (chardev.c:274-275): a *failed* POST_RESET (marker still set or restore/init failure) leaves the device out of the reset window with possibly uninitialized hardware, and subsequent ioctls are permitted. It is unclear whether this is intentional ("caller saw result=1, must retry a full reset") or an accepted gap.
5. **`mmap` does not check `needs_hw_init`** (chardev.c:708-736): a post-reset-generation fd can map BARs during the reset window before POST_RESET re-initializes the chip. Probably benign (BARs are restored config-wise), but the asymmetry with the ioctl allowlist is undocumented.
6. **Semantics of config registers 0x930/0x934** ("interface timer") are not documented in-tree; the driver treats "write 0x1 to 0x934 then 0x11 to 0x930" as an opaque reset trigger serviced by chip firmware. The port must reproduce the writes exactly; no independent description of the hardware behavior is available in these repos.
7. **WH `TRIGGER_RESET` is fire-and-forget** (`timeout_us = 0`, return value ignored, wormhole.c:510-512): the ioctl reports success without confirmation the FW accepted the reset ("Assumes the reset was successful", "Possibly a lie..." on BH, blackhole.c:563). Windows error reporting should preserve this optimism rather than invent stricter checking, since callers (UMD/tools) compensate by polling the marker/BDF afterwards.
8. **UMD legacy BH poll** checks command-register bit 1 (memory-space enable) rising (tt-umd/device/warm_reset.cpp:258-259) rather than the parity marker used by the current flow — the mechanism that sets that bit post-reset (FW?) is not visible from these repos. Only relevant if legacy flows must be supported against the Windows KMD.
9. **`reset_limit`/`auto_reset_timeout` are module-wide**, not per-device. If the Windows port makes them per-device registry values, multi-device systems could diverge from Linux behavior — probably fine, but note the difference.
