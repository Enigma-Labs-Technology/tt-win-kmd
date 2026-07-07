# tt-kmd Linux Driver Analysis — Master Document (Windows Port Baseline)

| | |
|---|---|
| **Upstream baseline** | tt-kmd tag `ttkmd-2.10.0-rc1-1-g8c32c2b` (commit `8c32c2b`) |
| **Companion repos** | tt-umd (consumer ABI, section 13), tt-system-tools (section 14), tt-kmd `test/`+`tools/` (section 12) |
| **Date assembled** | 2026-07-07 |
| **Citation convention** | Every `file:line` citation in this document is relative to that tag's tree (repo-root-relative paths; tt-umd/tt-system-tools paths are prefixed) |

**Verification note.** Every citation in the 14 sections below was checked against the source at the baseline tag and corrected where it did not hold. Claims that could not be verified against the checked-out sources are marked **[UNVERIFIED]** in place (they are additionally indexed in Appendix B). Everything else in sections 01–14 should be treated as verified against `8c32c2b`.

---

## How to read this document

This is the single reference for engineers porting tt-kmd (the Tenstorrent Linux kernel-mode driver) to Windows. It is organized as:

1. **Executive summary** — the driver in one page: architecture, the 17-ioctl ABI at a glance, the device lifecycle, and the five facts most likely to surprise a Windows port engineer. Read this first.
2. **Sections 01–14** — the full analysis, inlined verbatim from the per-topic section files (headings demoted one level so each section title is an H2). Each section follows the same shape: a *Scope* block naming exactly which source files it covers, the analysis body with `file:line` citations for every claim, `> Porting note:` call-outs translating Linux mechanisms into KMDF/Windows terms, a *Key constants table*, and its own *Open questions* list.
3. **Appendix A** — all open questions raised by the section readers, deduplicated and grouped by theme (ABI, reset/lifecycle, hardware/firmware, packaging, Windows design), with section attribution preserved. Use this as the triage feed for port design decisions.
4. **Appendix B** — an index of every claim marked **[UNVERIFIED]** in the body.

Conventions used throughout:

- `file.c:123` cites the tt-kmd tree at the baseline tag; `tt-umd/...` and `tt-system-tools/...` cite those repos as checked out alongside.
- "WH" = Wormhole (PCI `0x1E52:0x401E`), "BH" = Blackhole (`0x1E52:0xB140`). Grayskull (`0xFACA`) is claimed by the ID table but rejected at probe (`-ENODEV`) — there is no Grayskull support in this baseline.
- Ioctl names drop the `TENSTORRENT_IOCTL_` prefix where unambiguous.
- Section-local "Open questions" are preserved inline; Appendix A is the consolidated, deduplicated view.

### Table of contents

- [Executive summary](#executive-summary)
- [01. Module and Packaging](#01-module-and-packaging) — module identity, init/exit order, the 6 module parameters, PCI ID table, DKMS/AKMS packaging, udev rules, CI
- [02. Enumeration and Lifecycle](#02-enumeration-and-lifecycle) — probe/remove/shutdown/suspend step by step, ordinal allocation (incl. Galaxy), interrupts, sysfs inventory
- [03. Char Device and FD Lifecycle](#03-char-device-and-fd-lifecycle) — `/dev/tenstorrent/<N>`, open/release semantics, O_APPEND/O_EXCL, ioctl+mmap gating, post-reset fd invalidation
- [04. IOCTL Catalog (ABI Contract)](#04-ioctl-catalog-abi-contract) — all 17 ioctls: structs byte-by-byte, validation order, errors, concurrency, lifetime
- [05. Memory Management](#05-memory-management) — mmap offset encoding, DMA buffers, PIN_PAGES end to end, iATU allocator, peer-BAR P2P, TLB dma-buf export
- [06. TLB Window Management](#06-tlb-window-management) — window pools per chip, allocate/configure/free, mmap routing, export pinning
- [07. Blackhole Hardware Personality](#07-blackhole-hardware-personality) — BARs, TLB register formats, iATU, ARC messaging, telemetry, reset sequences
- [08. Wormhole hardware](#08-wormhole-hardware-bars-registers-arc-messaging-telemetry-reset) — BARs/iATU, scratch-register + queue ARC protocols, telemetry, reset; [WH-only] vs [SHARED] tagging
- [09. ARC Firmware Message Queue Protocol](#09-arc-firmware-message-queue-protocol) — the CSM ring queue: layout, index arithmetic, barriers, every opcode sent
- [10. Telemetry and hwmon](#10-telemetry-and-hwmon) — FW tag table in CSM, tag IDs, hwmon channels and scaling, sysfs attributes, lifecycle
- [11. PCIe Link Management and Device Reset](#11-pcie-link-management-and-device-reset) — the seven reset flavors end to end, PCIe primitives, reset_gen machinery, boot-time retrain loop
- [12. KMD Tests and Tools](#12-kmd-tests-and-tools-test-tools-docs-contrib) — the conformance test suite as executable ABI documentation; tools/reset.c, tools/power.c; portability assessment
- [13. tt-umd Consumer ABI](#13-tt-umd-consumer-abi--the-de-facto-userspace-contract-a-windows-shim-must-satisfy) — every ioctl/mmap/sysfs touch tt-umd makes; the de facto contract a Windows shim must satisfy
- [14. Hugepages and System Tools](#14-hugepages-and-system-tools) — the 1 GB hugepages service, UMD sysmem consumption, tt-oops, packaging, Windows equivalents
- [Appendix A. Open questions raised by analysis](#appendix-a-open-questions-raised-by-analysis)
- [Appendix B. Unverified claims](#appendix-b-unverified-claims)

---

## Executive summary

**Architecture.** tt-kmd is a small (~7,000 lines of C) monolithic PCI char driver for Tenstorrent AI accelerators. It binds vendor `0x1E52`, devices Wormhole (`0x401E`) and Blackhole (`0xB140`) — Grayskull (`0xFACA`) is claimed but rejected at probe — and hides chip differences behind a single ops vtable (`struct tenstorrent_device_class`: `wormhole_class` / `blackhole_class`). Each device becomes `/dev/tenstorrent/<ordinal>` (udev mode **0666** — deliberately world-writable), where the ordinal comes from an XArray allocator (Galaxy 32-chip systems get fixed bus-derived ordinals). The driver is **entirely polling-driven**: it requests one MSI/MSI-X/INTx vector but the ISR is a stub; all firmware interaction goes through MMIO — a legacy scratch-register protocol on Wormhole and a shared 32-byte-entry ring queue in ARC CSM SRAM on both chips (Blackhole exclusively). Userspace gets work done through exactly three surfaces: 17 ioctls on the char device, `mmap` on the same fd with a 64-bit offset namespace that multiplexes BARs / TLB windows / DMA buffers, and read-only sysfs/hwmon telemetry backed by a firmware-published tag table in CSM. Host memory reaches the device either as `dma_alloc_coherent` buffers (≤256 MiB each, ≤256 per fd), or as long-term-pinned user pages (`PIN_PAGES`) that **must resolve to one contiguous bus-address range** — 1 GB hugetlbfs pages without an IOMMU, or a verified-contiguous IOVA with one. The de facto ABI is whatever tt-umd (the user-mode driver) does; section 13 catalogs that consumer contract call-site by call-site.

**The 17-ioctl ABI at a glance** (all `_IO(0xFA, nr)` — no size/direction bits; struct layouts are the real contract):

| nr | name | one-line semantic |
|---|---|---|
| 0 | GET_DEVICE_INFO | PCI identity (vendor/device/subsystem, BDF, domain, max DMA-buf log2); allowed even mid-reset |
| 1 | GET_HARVESTING | no handler — always `-EINVAL` (reserved stub) |
| 2 | QUERY_MAPPINGS | up to 6 records: BARs 0/2/4 × {UC,WC} as opaque mmap-offset tokens + sizes |
| 3 | ALLOCATE_DMA_BUF | coherent DMA buffer (page-multiple, ≤256 MiB) in caller-chosen slot 0–255; returns bus addr/IOVA + mmap token; optional NOC iATU window |
| 4 | FREE_DMA_BUF | unimplemented — always `-EINVAL`; buffers free only at fd close |
| 5 | GET_DRIVER_INFO | ioctl ABI version (=2) + driver version 2.10.1; allowed mid-reset |
| 6 | RESET_DEVICE | 7 flavors (flags 0–6) from config-restore to ASIC+DMC reset and POST_RESET completion; runs with `reset_rwsem` held exclusive |
| 7 | PIN_PAGES | FOLL_LONGTERM pin of page-aligned user memory; requires a single contiguous phys/IOVA range; optional NOC iATU; 16-byte extended-output quirk |
| 8 | LOCK_CTL | 64 advisory per-device locks: acquire / release / test / blocking-acquire (the only indefinitely blocking ioctl) |
| 9 | MAP_PEER_BAR | map another tt device's BAR into this device's DMA domain (P2P); unmapped only at fd close |
| 10 | UNPIN_PAGES | exact-match (VA, size) unpin of a prior PIN_PAGES range |
| 11 | ALLOCATE_TLB | claim a TLB window of *exactly* a supported size; returns id + UC/WC mmap tokens |
| 12 | FREE_TLB | return an owned window (`-EBUSY` while mmapped; a live dma-buf export keeps it allocated) |
| 13 | CONFIGURE_TLB | program the window's NOC routing registers (addr, x/y rect, mcast, ordering, …) |
| 14 | SET_NOC_CLEANUP | register one NOC write32 the driver fires at fd close — crash-safe device-side cleanup |
| 15 | SET_POWER_STATE | per-fd power request; kernel aggregates across all fds (OR flags / max settings) and messages firmware |
| 16 | EXPORT_TLB_DMABUF | export a TLB window's BAR aperture as a dma-buf for third-party P2P; pins window+device; makes destructive reset `-EBUSY` |

**Device lifecycle:**

```
 pci_probe                                        open(2)
   | ordinal <- XArray (Galaxy: fixed bus slots)    | kref_get(device); snapshot reset_gen
   | DMA masks (streaming 64b; coherent WH=32/BH=58)| O_APPEND => "power-aware" fd
   | init_device: map BARs (never request_regions)  | O_EXCL => wait until sole opener
   | init_hardware: FW A0, iATU, watchdog           v
   | pci_save_state + save MPS (via DBI)        [fd live] --every ioctl/mmap gate-->
   | cdev_device_add -> /dev/tenstorrent/<N>        detached? reset_gen match? needs_hw_init?
   | telemetry sysfs+hwmon (WH: deferred <=120s)    (all three fail => -ENODEV)
   | power_policy: aggregate -> low power
   v
[RUNNING] --RESET_DEVICE flags 3/4/5--> [needs_hw_init] --POST_RESET (6)--> [RUNNING']
   ^        bump reset_gen: every OTHER fd          | only GET_DEVICE_INFO,
   |        permanently -ENODEV; zap all            | GET_DRIVER_INFO, RESET_DEVICE
   |        BAR/TLB user mappings                   | allowed in this window
   |                                                v
 pci_remove (== .shutdown)                     marker check + config restore + re-init
   | detached=true (under chardev_mutex); drain deferred power work
   | cleanup_telemetry BEFORE BAR unmap
   | down_write(reset_rwsem): zap user mappings, unmap BARs
   | revoke TLB dma-bufs; wake blocked lock waiters; per-fd memory cleanup
   | cdev_device_del; xa_erase(ordinal)  [ordinal immediately reusable]
   v
 device struct freed only when the LAST open fd closes (kref)
```

**Five facts most likely to surprise a Windows port engineer:**

1. **A destructive reset permanently kills every other open handle — except the resetter's.** RESET_DEVICE flags 2–5 bump a device-wide `reset_gen`; every fd whose open-time snapshot no longer matches gets `-ENODEV` on *all* ioctls and mmaps, forever. The calling fd survives only because `bump_reset_gen` migrates its own snapshot in the same statement. Between the reset and POST_RESET (`needs_hw_init` window) exactly three ioctls work: GET_DEVICE_INFO, GET_DRIVER_INFO, RESET_DEVICE. And resource-lock bits *survive* reset — a stale fd can never release them by ioctl, only by closing. (Sections 03 §7, 04 ioctl 6, 11.)
2. **One mmap offset space multiplexes everything, with hard-coded magic constants.** BARs 0/2/4 each occupy a 2^36-byte slot in UC and WC flavors at `(0..5) << 36`; TLB windows live at `(6..7) << 36 + <byte offset of the window within its BAR>` (plus a 2^29 bias for Blackhole BAR4 windows); DMA buffers sit at `(PAGE_SIZE−256) << 32 + index·2^32` — a **page-size-dependent** constant (0xF00_0000_0000 on 4 KiB pages). The "offsets" returned by QUERY_MAPPINGS / ALLOCATE_DMA_BUF / ALLOCATE_TLB are nominally opaque tokens, and a port must preserve them as such. (Sections 03 §6, 05 §2.)
3. **Four argument-passing protocols coexist, and the output size lives *inside* the input struct.** Ioctls 0/5/6/7/8 use `output_size_bytes`: the kernel `clear_user`s the caller's *entire declared* output area (zero-fill = forward compat), then copies `min(declared, sizeof(out))` (truncate = backward compat). Ioctls 14/15/16 demand `argsz == sizeof(struct)` exactly; ioctl 2 is slot-count based; the rest are fixed-size. PIN_PAGES additionally writes up to 16 output bytes where the nominal wrapper struct reserves 8 — a buffered-I/O port that sizes output from the struct definition will corrupt or truncate. (Section 04 §3.)
4. **The security model is "open BARs".** Device nodes are world-writable (udev 0666), and *any* fd may mmap the full BARs — including every TLB data window and the TLB config register file — with no ownership check; ownership is enforced only on the dedicated TLB-window mmap path. tt-umd depends on raw BAR access for userspace register programming, so a Windows port must consciously decide whether to reproduce this open model or force everything through ioctls (and coordinate that with the UMD port). (Sections 04 §4, 06, 13.)
5. **The device cannot scatter-gather: one contiguous bus range per buffer is a hard assumption.** PIN_PAGES verifies pfn-contiguity (no IOMMU) or that `dma_map_sgtable` produced a *single contiguous IOVA* (IOMMU) and fails with `-EINVAL` otherwise; raw physical addresses/IOVAs are then handed to userspace, which programs them into NOC/iATU hardware. Linux leans on 1 GB hugepages or the dma-iommu allocator to make this true; whether any Windows DMA-remapping path can guarantee the same is a core feasibility question for the port. (Sections 05 §6/§10, 14.)

---

## 01. Module and Packaging

### Scope

Files covered (line counts from `wc -l` at baseline `ttkmd-2.10.0-rc1-1-g8c32c2b`):

| File | Lines |
|---|---|
| module.c | 121 |
| module.h | 38 |
| Makefile | 55 |
| AKMBUILD | 3 |
| dkms.conf | 9 |
| modprobe.d-tenstorrent.conf | 13 |
| udev-50-tenstorrent.rules | 3 |
| dkms-post-install | 4 |
| README.md | 65 |
| VERSION_UPDATE.md | 16 |
| tools/current-version | 28 |
| .github/workflows/ (skim, CI only) | release.yml 301; test.yml, build-debian.yml, build-rpm.yml, mass-build-test.yml, hardware-test.yml, check-padding.yml, checkws.yml |

Supporting cross-references (read only the parts where module parameters / names defined here are consumed): chardev.c, enumerate.c, enumerate.h, pcie.c, wormhole.c, blackhole.c, telemetry.c, device.h, ioctl.h, tools/build_debs.sh, tools/build_rpms.sh, tools/exclude-from-release.

---

### 1. Module identity and minimum kernel

- License/description/version: `MODULE_LICENSE("GPL")`, `MODULE_DESCRIPTION("Tenstorrent AI kernel driver")`, `MODULE_VERSION(TENSTORRENT_DRIVER_VERSION_STRING)` (module.c:29-31).
- Hard build-time floor: Linux >= 5.4:
  ```c
  #if LINUX_VERSION_CODE < KERNEL_VERSION(5, 4, 0)
  #error "tt-kmd requires Linux 5.4 or later"
  #endif
  ```
  (module.c:19-21). README states build-testing against mainline 5.4 through 6.18 (README.md:15).
- The version string is assembled from four macros in module.h:
  ```c
  #define TENSTORRENT_DRIVER_VERSION_MAJOR 2
  #define TENSTORRENT_DRIVER_VERSION_MINOR 10
  #define TENSTORRENT_DRIVER_VERSION_PATCH 1
  #define TENSTORRENT_DRIVER_VERSION_SUFFIX "-pre"
  ```
  (module.h:19-22), stringified as `MAJOR.MINOR.PATCH SUFFIX` → `"2.10.1-pre"` (module.c:23-27).
- module.h also defines an RHEL backport-detection helper `TT_RHEL_RELEASE_GE(a, b)` used to select the newer 1-arg `class_create()` API on RHEL 9.4+ (module.h:11-17, consumed at chardev.c:59-63). Pure Linux-ism; irrelevant on Windows except as a warning that the codebase keys API selection off kernel versions in several places.
- The link-time composition of the module: `tenstorrent-y := module.o chardev.o enumerate.o interrupt.o wormhole.o blackhole.o msgqueue.o pcie.o sg_helpers.o memory.o tlb.o telemetry.o` (Makefile:4-5). This is the complete list of translation units a port must account for.

### 2. Module init/exit sequence and registration order

#### Init (`ttdriver_init`, module.c:76-108), in exact order:

1. `pr_info` banner with version string (module.c:80).
2. `tt_debugfs_root = debugfs_create_dir("tenstorrent", NULL)` (module.c:82). **Not error-checked** — debugfs is optional; failure is tolerated (per-device code guards with `if (tt_debugfs_root)`, chardev.c:109-110).
3. `tt_procfs_root = proc_mkdir("driver/tenstorrent", NULL)`; on NULL → `err = -ENOMEM`, goto unwind (module.c:84-88). procfs presence IS mandatory for load.
4. `init_char_driver(max_devices)` (module.c:90-92) — allocates a char major with `max_devices` minors via `alloc_chrdev_region(&tt_device_id, 0, max_devices, TENSTORRENT)` and creates the device class `class_create(TENSTORRENT)` where `TENSTORRENT` is the string `"tenstorrent"` (chardev.c:48-75, enumerate.h:13). Error unwind inside: class failure unregisters the chrdev region (chardev.c:71-74).
5. `tenstorrent_pci_register_driver()` → `pci_register_driver(&tenstorrent_pci_driver)` (module.c:94-96, enumerate.c:537-540). **Registration order matters**: the char-device infrastructure (major number + class) must exist before PCI probe can run, because probe → `tenstorrent_register_device()` creates the per-device cdev immediately (enumerate.c:375, chardev.c:90-119).

Failure unwind (module.c:100-107): reverse order — unregister nothing past the failed step; `cleanup_char_driver()`, `proc_remove()`, `debugfs_remove()`. Returns the error to modprobe (`-ENOMEM` for procfs, whatever `init_char_driver`/`pci_register_driver` returned otherwise).

#### Exit (`ttdriver_cleanup`, module.c:110-118), in exact order:

1. `tenstorrent_pci_unregister_driver()` — triggers `.remove` for every bound device first.
2. `cleanup_char_driver()` — destroys class and releases the chrdev region (chardev.c:77-83).
3. `debugfs_remove(tt_debugfs_root)`.
4. `proc_remove(tt_procfs_root)`.

Note the exit order of debugfs/procfs is not the exact mirror of init (harmless in Linux since both are empty by then — all per-device entries were removed during `.remove`).

> **Porting note:** In KMDF the equivalent of steps 4–5 collapses into `DriverEntry`/`WdfDriverCreate` + `EvtDeviceAdd`; there is no global "char driver" to pre-register. What must be preserved is the *invariant*, not the mechanism: per-device user-visible interfaces (device interface / symbolic link, ~= `/dev/tenstorrent/N`) may only appear after per-device init succeeds, and must be torn down before the device object goes away. The `max_devices` pre-allocation (32 minors) has no Windows analogue — device interfaces are unbounded — but note anything in userspace that assumes ordinals < 32.

#### Global namespaces created at init

- debugfs: `/sys/kernel/debug/tenstorrent/` (module.c:82), per-device numeric subdirs plus a `mappings` file (chardev.c:109-110, enumerate.c:385).
- procfs: `/proc/driver/tenstorrent/` (module.c:84), per-device numeric subdirs each containing a read-only `pids` file listing PIDs with open fds (chardev.c:111-113, enumerate.c:230-241).

> **Porting note:** debugfs/procfs are diagnostics only; on Windows these map naturally to either an IOCTL-based query or WPP/ETW + a WMI provider. The `pids` file (which processes hold the device open) is used operationally by tt-system-tools style tooling; decide early whether the port exposes an equivalent query IOCTL.

### 3. Module parameters — complete list

All six parameters, in declaration order (module.c:36-62). Permission bits are the sysfs mode: `0444` = visible read-only under `/sys/module/tenstorrent/parameters/`, settable only at load time (modprobe option); `0644` = root-writable **at runtime**.

| Name | Type | Default | Perms | Declared |
|---|---|---|---|---|
| `max_devices` | uint | 32 | 0444 | module.c:36-38 |
| `dma_address_bits` | uint | 0 | 0444 | module.c:40-42 |
| `reset_limit` | uint | 10 | 0444 | module.c:44-46 |
| `auto_reset_timeout` | byte (u8, 0–255) | 10 | 0444 | module.c:48-50 |
| `power_policy` | bool | true | 0444 | module.c:52-54 |
| `idle_power_down_grace_ms` | uint | 5000 | **0644** | module.c:56-62 |

`max_devices` is `static` to module.c; the other five are exported via module.h:25-29 for use by other translation units.

#### 3.1 `max_devices` — "Maximum number of tenstorrent devices (chips) to support."
Passed once to `init_char_driver()` (module.c:90); sizes the char minor-number region: `alloc_chrdev_region(&tt_device_id, 0, max_devices, TENSTORRENT)` (chardev.c:55) and is remembered for cleanup (chardev.c:52, 82). A device whose ordinal ≥ max_devices would get a minor outside the allocated region; ordinal allocation itself (xarray, Galaxy fixed slots) is in enumerate.c (see section 02). No runtime clamping is performed in module.c.

#### 3.2 `dma_address_bits` — "DMA address bits, 0 for automatic."
Consumed in probe (enumerate.c:322-331):
```c
tt_dev->dma_capable = (dma_set_mask(&dev->dev, DMA_BIT_MASK(dma_address_bits ?: 64)) == 0);
dma_set_coherent_mask(&dev->dev, DMA_BIT_MASK(dma_address_bits ?: device_class->dma_address_bits));
```
Semantics when 0 (default): streaming DMA mask = 64-bit; coherent DMA mask = the device class default — **32 for Wormhole** (`.dma_address_bits = 32`, wormhole.c:1058) and **58 for Blackhole** (`.dma_address_bits = 58`, blackhole.c:816; field defined at device.h:90). When nonzero, the same value overrides *both* masks. The comment explains why they differ: legacy Wormhole software assumes 32-bit addresses from ALLOCATE_DMA_BUF, but a 32-bit streaming mask would cripple user pinnings under IOMMU (enumerate.c:322-329). Probe also maxes out segment size/boundary: `dma_set_max_seg_size(&dev->dev, UINT_MAX); dma_set_seg_boundary(&dev->dev, ULONG_MAX);` (enumerate.c:334-335).

> **Porting note:** This is the single most consequential parameter for a Windows DMA design. The 32-bit coherent constraint for Wormhole common buffers (DMA-buf allocations) must be honored — on Windows that means a DMA enabler / common-buffer allocation constrained below 4 GiB for Wormhole, while user pinnings may use full 64-bit logical addresses. Blackhole's 58-bit limit reflects its NOC iATU carve-out (`noc_dma_limit = (1ULL << 58) - 1`, `noc_pcie_offset = (4ULL << 58)`, blackhole.c:817-818).

#### 3.3 `reset_limit` — "Maximum number of times to reset device during boot."
Consumed only in `wormhole_complete_pcie_init()` (pcie.c:92-131). If the device has no upstream bridge or `reset_limit == 0`, the retrain loop is skipped entirely and treated as success (`return true`, pcie.c:98-99). Otherwise up to `reset_limit` iterations of: read bridge `PCI_EXP_LNKCTL2` target link speed and subsystem vendor ID, send ARC message `FW_MSG_PCIE_RETRAIN` with `target_link_speed | (last_retry << 15)` and a 200000 µs timeout (pcie.c:101-114); exit code 0 → success; otherwise `pci_save_state` + secondary-bus hot reset + restore, and retry (pcie.c:125-127). Failure of the message send or of the final retry → `false`.

#### 3.4 `auto_reset_timeout` — "Timeout duration in seconds for M3 auto reset to occur."
Type is `byte` (`unsigned char`), so its range is 0–255 seconds. Three consumers:
1. **Wormhole FW watchdog programming** at hardware init: `WH_FW_MSG_UPDATE_M3_AUTO_RESET_TIMEOUT, auto_reset_timeout` (seconds, passed raw) with 10000 µs message timeout (wormhole.c:729-731; the timeout parameter is `timeout_us`, wormhole.c:206).
2. **Blackhole FW watchdog programming** at hardware init: `msg.header = ARC_MSG_TYPE_SET_WDT_TIMEOUT; msg.payload[0] = 1000 * auto_reset_timeout;` — converted to **milliseconds** (blackhole.c:632-636); failure is a `dev_warn` only ("normal for old FW").
3. **Host-side reset wait** in `wormhole_reset()` (wormhole.c:484-505): if the chip does not answer a NOP ARC message, and `auto_reset_timeout == 0`, the reset **fails immediately** with "Watchdog is disabled and device is unresponsive, cannot reset." (wormhole.c:488-491). Otherwise the driver polls hot-reset+NOP in a loop until deadline `ktime_add_ms(ktime_get(), (auto_reset_timeout * 1000) + 500)` (wormhole.c:493), sleeping 1 s between attempts (interruptible; a signal aborts with `false`, wormhole.c:502-503).

So the value 0 means "M3/DMC watchdog disabled" and simultaneously disables the driver's own wait-for-watchdog fallback.

#### 3.5 `power_policy` — "Enable power policy: low power at probe, re-aggregate on close (default=on)."
Two consumers: at the end of probe, `if (power_policy) tenstorrent_set_aggregated_power_state(tt_dev);` puts the freshly-probed device into low power (enumerate.c:387-389); at fd release, `if (!power_policy)` skips the close-time aggregation entirely (chardev.c:910). The user-visible contract is documented in ioctl.h:380-383 (SET_POWER_STATE semantics interact with this parameter).

#### 3.6 `idle_power_down_grace_ms` — delayed idle power-down
Full description: "Delay in ms between the last fd closing a device and the idle power-down message being sent. 0 sends the message synchronously at close. Only honored by device classes that opt in via defer_idle_powerdown." (module.c:58-62). Consumed at release: `can_defer = tt_dev->dev_class->defer_idle_powerdown && (idle_power_down_grace_ms > 0);` (chardev.c:903) and, when deferring, `mod_delayed_work(system_wq, &tt_dev->power_down_work, msecs_to_jiffies(idle_power_down_grace_ms));` (chardev.c:917). Opt-in flag: `defer_idle_powerdown` (device.h:122) is set **only for Wormhole** (`.defer_idle_powerdown = true`, wormhole.c:1083); the Blackhole class initializer omits it (blackhole.c:813-840), so on Blackhole the message goes out synchronously at close regardless of this parameter. The delayed work is cancelled in suspend and remove (device.h:60-62; enumerate.c:503).

Because this parameter is `0644`, root can retune it at runtime through `/sys/module/tenstorrent/parameters/idle_power_down_grace_ms`; the value is read fresh at each fd release, so changes take effect immediately.

> **Porting note:** The natural KMDF mapping for all six parameters is registry values under the service/device `Parameters` key, read at `DriverEntry`/`EvtDeviceAdd`. `idle_power_down_grace_ms` is the only one Linux allows to change at runtime; if that capability is preserved, the Windows driver must re-read the registry (or accept a control IOCTL) rather than caching at start. The modprobe.d sample file (modprobe.d-tenstorrent.conf:1-13) shows the parameters Tenstorrent expects administrators to tune: it documents only `max_devices`, `dma_address_bits`, `reset_limit`, `auto_reset_timeout` — all commented out, i.e., defaults everywhere. An INF should likewise ship defaults and document overrides, not hard-set values.

### 4. PCI device ID table and driver structure

The ID table (module.c:64-72):
```c
const struct pci_device_id tenstorrent_ids[] = {
    { PCI_DEVICE(PCI_VENDOR_ID_TENSTORRENT, PCI_DEVICE_ID_GRAYSKULL),
      .driver_data=(kernel_ulong_t)NULL}, // Deprecated
    { PCI_DEVICE(PCI_VENDOR_ID_TENSTORRENT, PCI_DEVICE_ID_WORMHOLE),
      .driver_data=(kernel_ulong_t)&wormhole_class },
    { PCI_DEVICE(PCI_VENDOR_ID_TENSTORRENT, PCI_DEVICE_ID_BLACKHOLE),
      .driver_data=(kernel_ulong_t)&blackhole_class },
    { 0 },
};
MODULE_DEVICE_TABLE(pci, tenstorrent_ids);
```
IDs (enumerate.h:15-18):
```c
#define PCI_VENDOR_ID_TENSTORRENT 0x1E52
#define PCI_DEVICE_ID_GRAYSKULL	0xFACA
#define PCI_DEVICE_ID_WORMHOLE	0x401E
#define PCI_DEVICE_ID_BLACKHOLE	0xB140
```
`driver_data` is a pointer to the per-ASIC `struct tenstorrent_device_class` (declared module.h:31-32). **Grayskull still matches but carries a NULL class**: probe rejects it up front with `dev_warn(..., "Unsupported device\n"); return -ENODEV;` (enumerate.c:261-264). So the driver deliberately *claims* Grayskull in the match table (blocking other drivers / documenting deprecation) but refuses to bind.

`MODULE_DEVICE_TABLE(pci, ...)` (module.c:74) generates the modalias data that makes udev auto-load the module when a matching PCI function appears — this is the Linux autoload mechanism, not a runtime structure.

The `pci_driver` itself (enumerate.c:527-535):
```c
static struct pci_driver tenstorrent_pci_driver = {
    .name = TENSTORRENT,
    .id_table = tenstorrent_ids,
    .probe = tenstorrent_pci_probe,
    .remove = tenstorrent_pci_remove,
    .shutdown = tenstorrent_pci_remove,
    .driver.pm = &tenstorrent_pm_ops,
};
```
Load-bearing details:
- **`.shutdown` is aliased to `.remove`** — at system shutdown/reboot the full remove path runs (draining work, sending power messages, unregistering the chardev). A separate reboot notifier is also registered per device when the class provides a `.reboot` hook, and it fires for reboot but *not* SYS_POWER_OFF (enumerate.c:243-251, 377-380).
- PM ops are `SIMPLE_DEV_PM_OPS(tenstorrent_pm_ops, tenstorrent_suspend, tenstorrent_resume)` (enumerate.c:524): suspend cancels the deferred power-down work, revokes TLB dmabufs, and calls `cleanup_hardware`; resume re-runs `init_hardware` and re-saves PCI config state, returning `-EIO` on failure (enumerate.c:499-522).
- Probe fixes up unflashed boards that enumerate with no PCI class code: `dev->class = 0x120000; /* Processing Accelerator - vendor-specific interface */` followed by `pci_assign_unassigned_bus_resources(dev->bus)` (enumerate.c:270-275).
- Probe suppresses hotplug on Galaxy chassis, keyed by PCI subsystem device ID `0x0035` (Galaxy Wormhole) / `0x0047` (Galaxy Blackhole) (enumerate.c:42-43, 355-357).

> **Porting note:** The INF Models section should list `PCI\VEN_1E52&DEV_401E` and `PCI\VEN_1E52&DEV_B140`. Whether to also claim `DEV_FACA` (Grayskull) needs a decision: Linux claims-then-rejects (-ENODEV), which on Windows would look like a device that always fails to start — usually worse UX than not claiming it. `.shutdown == .remove` means the Windows driver should run its full quiesce path (power aggregation message, watchdog handling) from the shutdown path (`EvtDeviceD0Exit` with system-shutdown awareness / `EvtDeviceShutdown` for FDOs), not just on removal. The class-code fixup and `pci_assign_unassigned_bus_resources` have no user-mode-visible analogue and are a manufacturing-flow convenience; on Windows an unflashed board with class 0x000000 may not even get resources assigned by PnP — treat as out of scope unless manufacturing on Windows is required.

### 5. Firmware loading

There are **no** `MODULE_FIRMWARE` declarations and no `request_firmware()` calls anywhere in the driver (verified by grep across all .c/.h). The driver never loads firmware files from disk; it communicates with ARC/M3/DMC firmware that is already resident on the board (flashed), via message queues and register mailboxes. Consequently the Linux package ships no firmware payloads, and a Windows driver package needs none either.

### 6. Version string handling

Three distinct version identities exist and must not be conflated:

1. **Driver code version** — module.h macros (`2` / `10` / `1` / `"-pre"`, module.h:19-22), stringified for the load banner and `MODULE_VERSION` (module.c:23-31, 80). Reported to userspace *numerically* by `TENSTORRENT_IOCTL_GET_DRIVER_INFO`: `out.driver_version_major/minor/patch = TENSTORRENT_DRIVER_VERSION_{MAJOR,MINOR,PATCH}` (chardev.c:176-179). **The suffix ("-pre"/"-rc1") is not reported through the ioctl.**
2. **ioctl ABI version** — `#define TENSTORRENT_DRIVER_VERSION 2` (ioctl.h:10), returned as `out.driver_version` (chardev.c:176). This is the compatibility number UMD checks; it changes only on ABI breaks, independent of the package version.
3. **Package version** — `PACKAGE_VERSION="2.10.1-pre"` in dkms.conf:2 and `modver=2.10.1-pre` in AKMBUILD:2. VERSION_UPDATE.md names dkms.conf the "Primary Source of Truth" and lists AKMBUILD, debian/changelog, module.h, and ioctl.h as needing manual sync (VERSION_UPDATE.md:5-12).

Automation: `tools/current-version` prints `MAJOR.MINOR.PATCH SUFFIX` **parsed from module.h** (tools/current-version:18-28) — despite VERSION_UPDATE.md:16 claiming it extracts from dkms.conf. The release workflow rewrites dkms.conf, AKMBUILD, and module.h together from a single user-supplied semver string (release.yml:88-118), tags releases `ttkmd-<version>` (release.yml:176-179), and then bumps to the next `-pre` patch version for development (release.yml:241-267) — which is why the working tree reads `2.10.1-pre` one commit after tag `ttkmd-2.10.0-rc1`. Debian/RPM packaging converts `-` to `~` for correct prerelease ordering (build-debian.yml step "Determine version for artifact naming": `DEB_VERSION="${VERSION//-/\~}"`; build-rpm.yml does the same for `RPM_VERSION`).

> **Porting note:** For Windows: the ioctl ABI number (`2`) and the numeric major/minor/patch must round-trip through the ported GET_DRIVER_INFO exactly, since tt-umd gates features on them. The package version maps to INF `DriverVer` (which cannot carry a `-pre` suffix — DriverVer is `mm/dd/yyyy,w.x.y.z`; encode prerelease-ness in the 4th numeric field or drop it). Keep a single source of truth and generate both the INF and the driver's version header from it, mirroring release.yml's atomic multi-file update.

### 7. Device naming and udev rules

The driver creates one char device per chip, named with an embedded slash so devtmpfs materializes a directory: `dev_set_name(&tt_dev->dev, TENSTORRENT "/%d", tt_dev->ordinal)` → `/dev/tenstorrent/0`, `/dev/tenstorrent/1`, … (chardev.c:106; README.md:11). The devt is `MKDEV(MAJOR(tt_device_id), MINOR(tt_device_id) + tt_dev->ordinal)` (chardev.c:85-88). The device's class (= udev SUBSYSTEM) is `"tenstorrent"` (chardev.c:59-63, enumerate.h:13), and its sysfs parent is the PCI device (`tt_dev->dev.parent = &tt_dev->pdev->dev`, chardev.c:101) — which is what lets udev match parent PCI attributes.

The complete udev rules file (udev-50-tenstorrent.rules:1-3):
```
SUBSYSTEM=="tenstorrent", MODE="0666"
SUBSYSTEM=="tenstorrent", ATTRS{device}=="0x401e", ATTR{tt_asic_id}=="?*", SYMLINK+="tenstorrent/by-id/wormhole-%s{tt_asic_id}"
SUBSYSTEM=="tenstorrent", ATTRS{device}=="0xb140", ATTR{tt_asic_id}=="?*", SYMLINK+="tenstorrent/by-id/blackhole-%s{tt_asic_id}"
```
Semantics:
- **Mode 0666**: every tenstorrent device node is world-readable *and world-writable*. All access control beyond that lives inside the driver (e.g., per-fd exclusivity, reset gating).
- **by-id symlinks**: `/dev/tenstorrent/by-id/wormhole-<ASICID>` / `blackhole-<ASICID>`, keyed on the *parent PCI* device ID (`ATTRS{device}` = `0x401e`/`0xb140`) and the class device's own `tt_asic_id` sysfs attribute; `=="?*"` requires the attribute to be non-empty (i.e., telemetry readable). `tt_asic_id` is a read-only sysfs attribute backed by telemetry tag `TELEMETRY_ASIC_ID` (wormhole.c:382, blackhole.c:421), formatted as 16 uppercase hex digits: `scnprintf(buf, PAGE_SIZE, "%08X%08X\n", hi, lo)` (telemetry.c:49-65). So a stable-name link looks like `wormhole-0123456789ABCDEF`.
- tt-umd itself opens devices only by ordinal path `/dev/tenstorrent/<N>` (tt-umd/device/pcie/pci_device.cpp:202, 355, 1101) and scans the `/dev/tenstorrent/` directory for enumeration (tt-umd/device/pcie/pci_device.cpp:230, 1079); no by-id consumer was found in tt-umd.

> **Porting note:** The Windows equivalents: a device interface GUID (published per device) replaces both the `/dev/tenstorrent/N` node and udev; the ordinal-N contract that UMD depends on must be reproduced somehow (e.g., an interface reference string or a driver-assigned ordinal queryable via GET_DEVICE_INFO — the Windows UMD port will need a matching enumeration strategy since it cannot scan `/dev`). MODE 0666 corresponds to a permissive SDDL on the device object/interface (e.g., allowing world access) — replicate deliberately or tighten with justification, because UMD assumes unprivileged open works. The by-id scheme (asic-id-stable names) maps to interface properties or a custom device property (DEVPKEY) carrying the 16-hex-digit ASIC ID.

### 8. DKMS / AKMS / package behavior

- **dkms.conf** (dkms.conf:1-9): `PACKAGE_NAME="tenstorrent"`, `PACKAGE_VERSION="2.10.1-pre"`, `BUILT_MODULE_NAME="tenstorrent"`, `DEST_MODULE_LOCATION="/kernel/extra"`, `AUTOINSTALL="yes"` (rebuild automatically for new kernels), `POST_INSTALL="./dkms-post-install"`.
- **dkms-post-install** (dkms-post-install:1-4) is the entire post-install step:
  ```sh
  cp udev-50-tenstorrent.rules /etc/udev/rules.d/50-tenstorrent.rules
  udevadm control -R
  ```
  i.e., install udev rules (renamed to `50-tenstorrent.rules`) and reload udevd. Note the rename: DKMS installs to `/etc/udev/rules.d/50-tenstorrent.rules`, while the deb/rpm packages install the file under its source name into `/lib/udev/rules.d/` (`cp -v udev-50-tenstorrent.rules "${PACKAGE_DIR}/lib/udev/rules.d/"`, tools/build_debs.sh:73-75; `%files ... /lib/udev/rules.d/udev-50-tenstorrent.rules`, tools/build_rpms.sh:95-97, 156).
- **Makefile targets** (Makefile:35-55): `make dkms` = `dkms add . && dkms install --force tenstorrent/$(VERSION) && modprobe tenstorrent`; `make dkms-remove` unloads and removes every installed tenstorrent DKMS version; `make akms`/`akms-remove` are the Alpine (akms/doas) equivalents. `VERSION` comes from `tools/current-version` (Makefile:14).
- **AKMBUILD** (AKMBUILD:1-3): Alpine akms metadata — `modname=tenstorrent`, `modver=2.10.1-pre`, `built_modules='tenstorrent.ko'`.
- **Deb postinst** additionally runs `common.postinst`, `modprobe tenstorrent || true`, and `udevadm control --reload || true` (tools/build_debs.sh:97-104); the RPM `%post` similarly reloads udev and attempts modprobe (tools/build_rpms.sh:99-131).
- **modprobe.d-tenstorrent.conf** is a documentation-only sample (every `options` line commented out, modprobe.d-tenstorrent.conf:1-13) and is *excluded* from source release tarballs (tools/exclude-from-release:4). It lists only 4 of the 6 parameters.
- NixOS support exists via a flake overlay; the udev rules must be added to `services.udev.packages` separately from the module (README.md:38-56) — reinforcing that the rules file is a required, separately-installed companion to the module.

> **Porting note:** The DKMS design translates to a signed driver package (INF + SYS + CAT) installed by pnputil/DPInst-equivalent; there is no rebuild-per-kernel concern on Windows. The three install-time side effects the Windows installer must replicate: (1) device security/permissions (udev MODE 0666 → SDDL in INF or `EvtDeviceAdd`), (2) stable-name links (by-id → device properties/interface strings), (3) immediate driver load on install without reboot (modprobe in postinst → INF-based install triggers PnP start automatically for present devices).

### 9. CI (skim)

- `test.yml`: builds via nix, dkms, and plain `make` (with sparse `make C=2`) on ubuntu-22.04/24.04; builds test suite; builds the .deb and verifies install: dkms status, module present under `/lib/modules/$(uname -r)/{updates,extra}/tenstorrent.ko[.zst]`, `modprobe tenstorrent`, `depmod -a` (test.yml, incl. lines 56-120).
- `mass-build-test.yml`: compiles the module against every mainline kernel from v5.4 to v7.1 using the gregkh linux mirror (`test/mass-build-test . /tmp/linux v5.4 v7.1`).
- `hardware-test.yml`: runs `test/run-hardware-tests.sh` on real runners: n150, n300, n300-llmbox, p150b — each in normal and `viommu` (virtualized IOMMU) variants (hardware-test.yml matrix). This is the supported-configuration matrix: Wormhole (n150/n300) and Blackhole (p150b), with and without IOMMU.
- `check-padding.yml`: runs pahole (`test/pahole_check.sh`) to reject **implicit padding in ioctl.h structs** — the ioctl ABI is deliberately padding-free/fixed-layout. A Windows port sharing ioctl.h-derived structures must preserve exact layout (no compiler-inserted padding).
- `checkws.yml`: whitespace lint. `release.yml`: covered in section 6.
- `build-rpm.yml`/`build-debian.yml`: package builds via `tools/build_rpms.sh`/`tools/build_debs.sh` with `-`→`~` version mangling.

### Key constants table

| Name | Value | Source |
|---|---|---|
| Minimum kernel | 5.4 | module.c:19-21 |
| Driver code version (baseline tree) | 2.10.1 + `"-pre"` | module.h:19-22 |
| ioctl ABI version `TENSTORRENT_DRIVER_VERSION` | 2 | ioctl.h:10 |
| Package version (dkms/akms) | `2.10.1-pre` | dkms.conf:2, AKMBUILD:2 |
| PCI vendor ID | 0x1E52 | enumerate.h:15 |
| Grayskull device ID (deprecated, probe → -ENODEV) | 0xFACA | enumerate.h:16, enumerate.c:261-264 |
| Wormhole device ID | 0x401E | enumerate.h:17 |
| Blackhole device ID | 0xB140 | enumerate.h:18 |
| Galaxy WH subsystem ID (hotplug suppressed) | 0x0035 | enumerate.c:42 |
| Galaxy BH subsystem ID (hotplug suppressed) | 0x0047 | enumerate.c:43 |
| Class-code fixup for unflashed boards | 0x120000 | enumerate.c:273 |
| `max_devices` default | 32 | module.c:36 |
| `dma_address_bits` default | 0 (= auto) | module.c:40 |
| `reset_limit` default | 10 | module.c:44 |
| `auto_reset_timeout` default | 10 s (u8) | module.c:48 |
| `power_policy` default | true | module.c:52 |
| `idle_power_down_grace_ms` default | 5000 ms, perms 0644 | module.c:56-57 |
| Streaming DMA mask fallback | 64 bits | enumerate.c:330 |
| Wormhole coherent DMA mask default | 32 bits | wormhole.c:1058 |
| Blackhole coherent DMA mask default | 58 bits | blackhole.c:816 |
| WH unresponsive-reset deadline | `auto_reset_timeout*1000 + 500` ms | wormhole.c:493 |
| BH watchdog units | ms (`1000 * auto_reset_timeout`) | blackhole.c:634 |
| PCIe retrain ARC message timeout | 200000 µs | pcie.c:113 |
| Char device class / driver name | `"tenstorrent"` | enumerate.h:13, chardev.c:60, enumerate.c:528 |
| Device node path | `/dev/tenstorrent/%d` | chardev.c:106, README.md:11 |
| Device node mode | 0666 | udev-50-tenstorrent.rules:1 |
| by-id symlink pattern | `tenstorrent/by-id/{wormhole,blackhole}-%s{tt_asic_id}` | udev-50-tenstorrent.rules:2-3 |
| `tt_asic_id` format | `"%08X%08X\n"` (16 hex digits) | telemetry.c:64 |
| debugfs root | `/sys/kernel/debug/tenstorrent` | module.c:82 |
| procfs root | `/proc/driver/tenstorrent` | module.c:84 |
| DKMS dest | `/kernel/extra`, AUTOINSTALL=yes | dkms.conf:5-7 |
| Release tag format | `ttkmd-<version>` | .github/workflows/release.yml:178 |

### Open questions

1. **Grayskull INF policy**: Linux claims `DEV_FACA` in the match table but probe returns `-ENODEV` (module.c:65-66, enumerate.c:261-264). Should the Windows INF bind (and fail start, mirroring Linux and blocking other drivers) or not list Grayskull at all? Linux behavior is claim-and-reject; a direct mirror produces a permanently code-10-style device on Windows.
2. **`tools/current-version` vs VERSION_UPDATE.md discrepancy**: the doc says the script extracts the version from dkms.conf (VERSION_UPDATE.md:15-16), but the script parses module.h (tools/current-version:11-28). Which file wins if they diverge is therefore ambiguous in the upstream process; the port's single-source-of-truth should be defined explicitly.
3. **Runtime mutability of `idle_power_down_grace_ms`** (0644, module.c:57): is runtime tunability a hard requirement for the Windows port, or is boot/start-time configuration sufficient? No in-tree tooling was found that writes it.
4. **Security model of MODE 0666** (udev-50-tenstorrent.rules:1): world-writable device nodes expose reset/pin-pages ioctls to any local user. Is replicating this on Windows (permissive SDDL) acceptable, or should the port tighten access and require UMD to run with membership in a group?
5. **by-id symlink consumers**: no consumer found in tt-umd (grep for "by-id" empty). Unknown whether external tooling (tt-smi, orchestration) relies on `/dev/tenstorrent/by-id/*`; determines whether the Windows port needs an ASIC-ID-stable naming feature at all.
6. **modprobe.d sample staleness**: modprobe.d-tenstorrent.conf documents only 4 of 6 parameters (missing `power_policy`, `idle_power_down_grace_ms`) and is excluded from release tarballs (tools/exclude-from-release:4) — unclear if intentional (params considered internal) or stale documentation.
7. **`max_devices` > 32 systems**: Galaxy fixed-ordinal assignment plus the static 32-minor region means a >32-chip system needs `max_devices` raised at load; whether any deployed configuration does this (and thus whether the Windows port needs an equivalent knob) is unknown from the source alone.
8. **`.shutdown = .remove` scope**: running the full remove path (including chardev teardown and power messages) at shutdown is a Linux implementation convenience; exactly which subset (quiesce? watchdog reprogram? power state?) is *required* by firmware across a warm reboot is not documented in these files — needs correlation with sections covering enumerate.c/wormhole.c shutdown behavior before deciding what the Windows shutdown path must do.

---

## 02. Enumeration and Lifecycle

### Scope

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

### 1. Module-level context (what exists before probe runs)

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

### 2. Global device collection and ordinal allocation

The only global device collection is an allocating XArray:

```c
static DEFINE_XARRAY_ALLOC(tenstorrent_dev_xa);
```
(enumerate.c:33). Locking is the XArray's internal spinlock; the driver never iterates or looks up this collection (no `xa_load`/`xa_for_each` anywhere) — it is used purely as an ordinal (index) allocator; the stored `tt_dev` pointers are never read back. Entries are inserted in probe (enumerate.c:287-294) and erased in remove (enumerate.c:478) and on the probe failure path (enumerate.c:398).

#### 2.1 Galaxy static ordinals

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

#### 2.2 Allocation in probe

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

### 3. Probe sequence (`tenstorrent_pci_probe`, enumerate.c:253-402)

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

#### 3.1 Probe failure unwind

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

### 4. Char device creation ordering and naming

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

### 5. Interrupt allocation and handler (interrupt.c, all 46 lines)

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

### 6. Sysfs attribute inventory (created during probe / init_telemetry)

The class device itself has **no static attribute groups** (`tt_dev->dev.groups = NULL`, chardev.c:102). Everything is added by `init_telemetry` (probe step 18) via `device_add_group`, plus a separately registered hwmon device. Removal for all of these is `cleanup_telemetry` (wormhole.c:766-784; blackhole.c:677-695), called early in remove.

#### 6.1 Telemetry group (directly on `/sys/class/tenstorrent/tenstorrent!<N>/`)

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

#### 6.2 `pcie_perf_counters` group (subdirectory `pcie_perf_counters/` on the class device)

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

#### 6.3 hwmon device (separate `/sys/class/hwmon/hwmonX`, parent = PCI device)

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

#### 6.4 Non-sysfs per-device files

- debugfs `/sys/kernel/debug/tenstorrent/<N>/mappings`, 0444 (enumerate.c:385, behavior §4).
- procfs `/proc/driver/tenstorrent/<N>/pids`, 0444 (chardev.c:111-113).

> **Porting note:** all of §6 is diagnostics/monitoring surface with no ioctl dependency, except `tt_asic_id`, which the udev by-id symlinks (stable device identity for UMD) depend on. A Windows port should expose ASIC ID early (device property), and can map the rest to WMI/ETW or a query ioctl at leisure.

---

### 7. Remove/shutdown sequence (`tenstorrent_pci_remove`, enumerate.c:404-481)

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

### 8. Suspend/resume and reboot

`SIMPLE_DEV_PM_OPS(tenstorrent_pm_ops, tenstorrent_suspend, tenstorrent_resume)` (enumerate.c:524):

- **Suspend** (enumerate.c:499-509): `cancel_delayed_work_sync(&power_down_work)`, `tenstorrent_revoke_tlb_dmabufs`, `dev_class->cleanup_hardware` (FW → A3). Always returns 0. It does *not* zap user VMAs or set `detached`.
- **Resume** (enumerate.c:511-522): `ok = dev_class->init_hardware(tt_dev)`; on success `pci_save_state(pdev)` because "Suspend invalidates the saved state" (enumerate.c:517-519); returns `ok ? 0 : -EIO`.

Reboot: see §3 step 17 — Wormhole only, ASTATE3 on anything except `SYS_POWER_OFF` (enumerate.c:243-251).

---

### 9. PCI config-space save and hot-reset references

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

### 10. Lifecycle-relevant locking summary

| Lock | Protects | Lifecycle usage |
|---|---|---|
| `tenstorrent_dev_xa` internal spinlock | ordinal map | probe insert/alloc (enumerate.c:287-293), remove/fail erase (enumerate.c:398, 478) |
| `tt_dev->chardev_mutex` | `open_fds_list`, `chardev_excl_held`, power aggregation, `detached` transition | probe init (enumerate.c:316); remove sets `detached` under it (enumerate.c:423-425); open/close list add/del (chardev.c:755-787, 936-948); mappings/pids readers (enumerate.c:100, 235) |
| `tt_dev->reset_rwsem` | hardware access vs reset/remove | read-held by every ioctl/mmap/open-body/release; write-held by RESET_DEVICE ioctl and remove's zap+unmap window (enumerate.c:444-447; chardev.c:598-601) |
| `tt_dev->kref` | struct lifetime | probe init (enumerate.c:302), per-open get (chardev.c:810), remove put (enumerate.c:480), release frees (enumerate.c:483-493) |
| `iatu_mutex`, `dmabuf_export_lock`, per-fd locks | see memory/ioctl sections | initialized at probe (enumerate.c:317-318) |

---

### Key constants table

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

### Open questions

1. **Ordinal vs chrdev-region overflow.** `alloc_chrdev_region` reserves `max_devices` (default 32) minors (chardev.c:55), but `xa_alloc` can return ordinals up to 2^31-1 (enumerate.c:290-293) and `devt_for_device` adds the ordinal to the base minor without bounds-checking (chardev.c:85-88). Behavior when >32 devices (or Galaxy fallback ordinals ≥ 32) are probed is unverified — `cdev_device_add` would register a devt outside the reserved region. A port should pick an explicit policy.
2. **`tenstorrent_register_device` return value ignored** (enumerate.c:375 vs chardev.c:118). If `cdev_device_add` fails, probe still succeeds, leaving a device with sysfs/telemetry but no char device, and remove will call `cdev_device_del` on a never-added cdev. Intentional tolerance or oversight?
3. **`kzalloc` failure path leaves the PCI device enabled** — the `-ENOMEM` return at enumerate.c:280-282 does not call `pci_disable_device`, unlike the ordinal-failure path (enumerate.c:295-299). Presumed oversight; a port should unwind symmetrically.
4. **Stub handler claims all shared interrupts.** `irq_handler` returns `IRQ_HANDLED` unconditionally (interrupt.c:13-19) while requesting with `IRQF_SHARED`; on a shared INTx line this masks other devices' interrupt storms/diagnostics. Whether future TT interrupt sources will require reading a status register first is unknown from this code.
5. **Embedded `struct device` has `release = NULL`** (chardev.c:103) while the memory is freed via the separate driver kref (`kfree` at enumerate.c:492). This relies on the device kobject holding no references at kfree time; the Linux core normally warns on releasing a device without a release callback. Whether a warning fires (and whether refs can outlive) was not verified at runtime.
6. **Unlocked `open_fds_list` walk in remove** (enumerate.c:465-467) — no `chardev_mutex` held while iterating and running `tenstorrent_memory_cleanup` on live fds. Presumably safe because `cdev_device_del` has not yet run... but opens are still possible at that instant (device deleted only at enumerate.c:469), and a concurrent open's `list_add` (chardev.c:785) or release's `list_del` (chardev.c:938) could race the walk. Needs a deliberate answer in the port (take the equivalent lock).
7. **`CONFIG_HWMON=n` stub mismatch**: enumerate.c:78-82 stubs `devm_hwmon_device_register_with_info`, but the classes call the non-devm `hwmon_device_register_with_info` (wormhole.c:628, blackhole.c:664) — the stub appears stale; a `CONFIG_HWMON=n` build's status is unclear.
8. **Galaxy ordinal mapping ignores PCI domain** (enumerate.c:51) — on a host exposing Galaxy chips across multiple PCI segments, bus-number collisions between segments would map two chips to the same fixed ordinal (second one falls back to dynamic with a warning). Is single-domain an actual invariant of Galaxy hosts?
9. **Remove-time A3 message**: remove calls `cleanup_hardware` only if the vendor ID reads back valid (enumerate.c:432-434), but both class implementations independently skip work when `detached` is set (wormhole.c:789, blackhole.c:702), and `detached` is always true by that point in remove — so at driver unload the firmware apparently never receives the A3 message from the remove path (it does from suspend and WH reboot-notifier paths, where `detached` is false). Whether leaving FW in A0 at unload is intended (e.g. relying on the probe-time low-power policy of a future driver load) is not documented in the code.

---

## 03. Char Device and FD Lifecycle

### Scope

Primary files (read in full):

| File | Lines |
|---|---|
| `chardev.c` | 966 |
| `chardev.h` | 18 |
| `chardev_private.h` | 82 |

Supporting files read for context and cited where load-bearing: `ioctl.h` (458 lines, read in full), `memory.c` (1742 lines; mmap dispatch, query-mappings, TLB-allocate, cleanup, and vma-zap paths), `memory.h` (73), `device.h` (127), `tlb.c` (108), `module.c` / `module.h` (params), `enumerate.c` / `enumerate.h` (registration context, remove path). All paths are relative to the tt-kmd repo root at tag `ttkmd-2.10.0-rc1-1-g8c32c2b`.

---

### 1. Char device registration

#### Major/minor scheme

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

#### Device node naming

```c
dev_set_name(&tt_dev->dev, TENSTORRENT "/%d", tt_dev->ordinal);   // chardev.c:106
```
Because the name contains a `/`, udev creates the node as **`/dev/tenstorrent/<ordinal>`** (a directory containing per-ordinal nodes). tt-umd depends on exactly this path: `open(fmt::format("/dev/tenstorrent/{}", n).c_str(), O_RDWR | O_CLOEXEC | O_APPEND)` (tt-umd/device/pcie/pci_device.cpp:355).

#### Registration sequence (`tenstorrent_register_device`, chardev.c:90-119)

1. `init_waitqueue_head` for `resource_lock_waitqueue` and `chardev_excl_waitqueue` (chardev.c:95-96).
2. `device_initialize`; sets `devt`, `class`, `parent = &tt_dev->pdev->dev`, `groups = NULL`, `release = NULL`, `id = ordinal` (chardev.c:98-105).
3. Creates per-device debugfs dir `<ordinal>` under the module root, and a procfs dir `<ordinal>` under `/proc/driver/tenstorrent` containing a read-only `pids` file (mode `0444`) backed by `pids_proc_show` (chardev.c:108-113). `pids_proc_show` walks `open_fds_list` under `chardev_mutex` and prints `pid_vnr(priv->pid)` per open fd (enumerate.c:230-241).
4. `INIT_LIST_HEAD(&tt_dev->open_fds_list)` (chardev.c:115).
5. `cdev_init(&tt_dev->chardev, &chardev_fops); return cdev_device_add(...)` (chardev.c:117-118).

`tenstorrent_unregister_device` removes debugfs recursively, removes procfs, then `cdev_device_del` (chardev.c:121-126). Note there is no forced revocation of already-open fds here; they are handled by the `detached` flag (section 7).

> **Porting note:** On Windows/KMDF the whole major/minor + udev-node scheme maps to a device interface GUID plus a per-device reference string (or `\\.\TenstorrentN` symbolic links) created in `EvtDeviceAdd`. The ordinal-stability property (ordinal ties the node name to an xarray slot allocated at probe, enumerate.c:284-294; the slot is erased unconditionally at remove — deliberately not postponed to last-close — so a re-probed device can reuse the ordinal even while stale fds still reference the old device struct, enumerate.c:477-478) must be reproduced deliberately if UMD-on-Windows enumerates by index.

---

### 2. `file_operations` table

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

### 3. open(): `tt_cdev_open` (chardev.c:791-863)

#### O_APPEND power-aware-client detection

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

#### Full open sequence

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

### 4. release(): `tt_cdev_release` cleanup order (chardev.c:922-958)

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

### 5. ioctl dispatch: `tt_cdev_ioctl` (chardev.c:591-706)

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

### 6. mmap dispatch and the offset-space encoding

#### Entry: `tt_cdev_mmap` (chardev.c:708-736)

```c
if (!down_read_trylock(&tt_dev->reset_rwsem))
	return -ENODEV;                                   // chardev.c:717-718
```
Trylock (not a blocking lock) to avoid ABBA deadlock: mmap is called with `mmap_lock` held, and `tenstorrent_vma_zap()` (run under `reset_rwsem`) takes `mmap_lock` (comment chardev.c:714-716). Then the same `detached` and `reset_gen` gates as ioctl, both `-ENODEV` (chardev.c:720-729), then `tenstorrent_mmap(priv, vma)`.

#### Offset space (`tenstorrent_mmap`, memory.c:1585-1636)

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

#### How ioctl.h values relate

- **QUERY_MAPPINGS** returns, for each BAR with non-zero length, a `tenstorrent_mapping { mapping_id, mapping_base, mapping_size }` where `mapping_id` is `TENSTORRENT_MAPPING_RESOURCE{0,1,2}_{UC,WC}` (1-6, ioctl.h:33-39) and `mapping_base` is exactly the `MMAP_OFFSET_*` constant (memory.c:352-391). Up to 6 mappings; caller-supplied `output_mapping_count` extras are zero-filled (memory.c:393-406, overflow-checked at 397-398 → `-EFAULT`).
- **ALLOCATE_DMA_BUF** returns `out.mapping_offset = MMAP_OFFSET_DMA_BUF + buf_index * MMAP_SIZE_DMA_BUF` (memory.c:421-423); `buf_index` is a `u8` chosen by the caller (`[0, TENSTORRENT_MAX_DMA_BUFS)`, ioctl.h:93).
- **ALLOCATE_TLB** returns `out.mmap_offset_uc = MMAP_OFFSET_TLB_UC + encoded_id` and `out.mmap_offset_wc = MMAP_OFFSET_TLB_WC + encoded_id`, where `encoded_id = tlb_desc.bar_offset`, plus `BAR0_SIZE` (`1UL << 29`, memory.c:29) if the window lives in BAR 4 (Blackhole 4G windows) (memory.c:918-934). I.e., **the TLB offset encodes the window's byte offset within its BAR, not the TLB id**; `map_tlb_window` reverses this by searching all windows for a matching `bar_offset` (memory.c:1494-1534), then checks: window not owned by this fd → `-EPERM` (memory.c:1541-1543); `size > tlb_desc.size` → `-EINVAL`; window past BAR end → `-ENXIO`; `io_remap_pfn_range` failure → `-EAGAIN`. Successful BAR and TLB mappings are recorded as `struct tenstorrent_mmap_vma` on `priv->vma_list` under `priv->vma_lock`, with `vm_ops` hooks that keep the list correct across `fork()` (vma_open duplicates the tracking node; on allocation failure the child's mapping is zapped, memory.c:1370-1410) and unmap (vma_close unlinks and frees, memory.c:1412-1434).

The vma tracking exists so `tenstorrent_vma_zap(tt_dev)` (memory.c:1677-1729+) can, on reset/removal, walk every open fd's `vma_list` under `chardev_mutex` and zap the PTEs of all **BAR and TLB** mappings (lock-ordering dance with `mmap_lock` documented at memory.c:1683-1689, modeled on VFIO). Subsequent user access faults instead of reaching dead hardware. DMA-buffer mappings are *not* tracked and not zapped (they are host RAM and stay valid).

> **Porting note:** Windows has no `mmap` on a device handle with a 64-bit routing offset. The conventional KMDF replacement is an ioctl that maps the requested entity into the caller's address space (`MmMapLockedPagesSpecifyCache` on an MDL built with `MmBuildMdlForNonPagedPool`/`IoAllocateMdl` for BAR space, `MmCached`/`MmNonCached`/`MmWriteCombined` for the UC/WC split) and returns the user VA — the offset *encoding* can be kept as the ioctl input so UMD's bookkeeping survives. Two hard requirements to preserve: (a) per-fd ownership checks (TLB windows mappable only by the allocating fd; DMA buffers looked up in the per-fd table), and (b) revocation on reset — Windows user mappings created via MDLs cannot be "zapped" to fault; the port must either unmap them (`MmUnmapLockedPages`, requires tracking user VAs and process context) or interpose a section object it can dereference. This is one of the largest behavioral deltas of the whole port.

---

### 7. Post-reset fd invalidation: mechanism and the narrow exception

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

### 8. `struct chardev_private` field by field (chardev_private.h:55-78)

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

### 9. Interaction with device removal (for completeness)

`tenstorrent_pci_remove` (enumerate.c:404-481): sets `detached = true` under `chardev_mutex` (fencing new deferred-powerdown arms, enumerate.c:415-425), `cancel_delayed_work_sync(power_down_work)` (427), conditionally `cleanup_hardware` (429-434), drains in-flight ioctls with `down_write(&reset_rwsem)` around `tenstorrent_vma_zap` + `cleanup_device` (BAR unmap) (442-447), revokes TLB dma-buf exports (449-459), wakes blocked lock waiters (461-463), then calls `tenstorrent_memory_cleanup(priv)` for every still-open fd (465-467) — so DMA/pinned-page resources are torn down at remove even though the fds remain open; the later `tt_cdev_release` re-runs `tenstorrent_memory_cleanup` against now-empty lists (safe: lists/table are emptied as they are cleaned) and skips hardware-touching steps via the `detached` checks. The device struct itself is freed only when the last fd drops its kref (enumerate.c:483-497).

---

### Key constants table

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

### Open questions

1. **NOC cleanup write vs reset state:** `tt_cdev_release_noc_cleanup` gates only on `tt_dev->detached` (chardev.c:869) — not on `reset_gen` or `needs_hw_init`. A stale pre-reset fd closing while the device is mid-reset (`needs_hw_init == true`) still performs its `noc_write32` into hardware that may be in reset. Intentional (harmless write) or an oversight? A Windows port should decide whether to add a `needs_hw_init` guard or replicate exactly.
2. **GET_HARVESTING (ioctl nr 1)** has no handler; the case falls through with `ret` still `-EINVAL` (chardev.c:631-632). Is the code reserved for compatibility (older kmd/umd)? The port must at minimum reserve the function number; unclear whether any UMD version still probes it.
3. **32-bit userspace:** no `.compat_ioctl` (chardev.c:40-46) means 32-bit processes cannot use the driver on 64-bit kernels. Should the Windows port explicitly reject WOW64 callers, or thunk? (All ioctl structs appear layout-identical across widths — fixed-width types with explicit reserved fields — but this was not exhaustively verified for every struct.)
4. **open() during removal race:** `tt_cdev_open` never checks `detached`; an open racing `cdev_device_del` can succeed and immediately get `-ENODEV` on all operations while still triggering the power-aggregation attempt guard (`!detached` check at chardev.c:852). The exact Linux race window is closed by cdev refcounting; the KMDF equivalent behavior (create arriving during surprise removal) needs an explicit decision.
5. **`x > 64 || y > 64` NOC coordinate validation** is flagged `TODO: Implement a more robust coordinate validation scheme` (chardev.c:467-469) — bounds are not per-ASIC. Port should track upstream if this tightens.
6. **`out.value` width in LOCK_CTL TEST:** `test_bit` returns int; `(test_bit(...) << 1) | test_bit(...)` stores into a `__u8` (chardev.c:414-415, ioctl.h:242). Unambiguous today, but worth pinning in the ported ABI definition.
7. **O_APPEND semantics on Windows:** tt-umd currently opens with `O_RDWR | O_CLOEXEC | O_APPEND` (tt-umd/device/pcie/pci_device.cpp:204) but one call site notes "O_APPEND is temporarily disabled to investigate NOC1 issues" (pci_device.cpp:379). The port needs an agreed replacement signal for power-aware clients (create-time flag vs explicit ioctl), coordinated with the UMD Windows port.

---

## 04. IOCTL Catalog (ABI Contract)

### Scope

Primary files (read in full):

| File | Lines |
|---|---|
| `ioctl.h` | 458 |
| `chardev.c` | 966 |
| `memory.c` | 1743 |
| `tlb.c` | 108 |
| `chardev_private.h` | 82 |
| `memory.h` | 73 |
| `tlb.h` | 25 |
| `device.h` | 127 |

Targeted reads for end-to-end tracing: `pcie.c:1-158`, `wormhole.c:20-42,87,700-930,1058-1083`, `blackhole.c:20-42,150-268,560-760,816-840`, `enumerate.c:300-509`, `module.h:19-29`, `module.c:40-58`, plus grep of `tt-umd/device/pcie/pci_device.cpp` and `tt-umd/device/chip_helpers/silicon_sysmem_manager.cpp` for consumer usage.

All ioctls are defined in `ioctl.h`, dispatched from `tt_cdev_ioctl()` in `chardev.c:591-706`, with handlers in `chardev.c` (info/reset/lock/noc-cleanup/power) and `memory.c` (mappings/DMA/pin/peer/TLB/dmabuf), backed by `tlb.c` for the TLB allocator.

---

### 1. Command encoding

All 17 commands use `_IO()` — direction and size fields are **zero**; the argument size is *not* encoded in the command number:

```c
#define TENSTORRENT_IOCTL_MAGIC 0xFA
#define TENSTORRENT_IOCTL_GET_DEVICE_INFO	_IO(TENSTORRENT_IOCTL_MAGIC, 0)
...
#define TENSTORRENT_IOCTL_EXPORT_TLB_DMABUF		_IO(TENSTORRENT_IOCTL_MAGIC, 16)
```
(ioctl.h:12-30)

So the on-the-wire command values are `0x0000FA00 + n` for n = 0..16 (`_IO(type,nr) = (type<<8)|nr` with dir/size = 0). The kernel never uses the ioctl size bits; each handler `copy_from_user`s a fixed-size input struct and decides output size via the protocols in §3.

`TENSTORRENT_DRIVER_VERSION` (the ioctl ABI version) is **2** (ioctl.h:10); the driver package version is 2.10.1 (`module.h:19-21`).

> **Porting note:** On Windows, `CTL_CODE()` encodes function codes 0..0x7FF plus method/access; a KMDF port should define 17 `CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800+n, METHOD_BUFFERED, ...)` codes and preserve the *struct layouts* exactly (they are the real contract), not the Linux command numbers. Because Linux does not encode size in the command, a buffered-I/O port must derive the expected input length per-command from this catalog, not from the IRP's declared lengths alone — and must reproduce the truncation/zero-fill semantics of §3 rather than NT's default "copy exactly InputBufferLength" behavior.

### 2. Dispatch, gating, and reset interaction (applies to ALL ioctls)

`tt_cdev_ioctl()` (chardev.c:591-706):

1. `ret` is initialized to `-EINVAL` (chardev.c:595); unknown commands fall to `default: ret = -EINVAL` (chardev.c:694-696).
2. **Reset rwsem**: `RESET_DEVICE` takes `tt_dev->reset_rwsem` **exclusive** (`down_write`); every other ioctl takes it **shared** (`down_read`) (chardev.c:596-601). The rwsem is writer-fair, so a pending reset starves out new readers.
3. **Detached gate**: `if (priv->device->detached) return -ENODEV;` — an fd on a removed/hotplugged device is permanently invalid (chardev.c:604-607).
4. **Reset-generation gate**: `if (atomic_long_read(&priv->device->reset_gen) != priv->open_reset_gen) return -ENODEV;` — an fd opened before a generation-bumping reset is permanently invalid (chardev.c:609-613). `open_reset_gen` is latched at open (chardev.c:812); the resetting fd itself is exempted because `bump_reset_gen()` updates its own `open_reset_gen` (chardev.c:195-198).
5. **needs_hw_init gate**: between a destructive reset and its `POST_RESET`, only `GET_DEVICE_INFO`, `GET_DRIVER_INFO`, and `RESET_DEVICE` are allowed; everything else gets `-ENODEV` (chardev.c:616-624).

`mmap` has the same three gates but uses `down_read_trylock` and returns `-ENODEV` if the rwsem is contended, to avoid an ABBA deadlock with `tenstorrent_vma_zap()` which takes `mmap_lock` while holding `reset_rwsem` (chardev.c:708-736).

Device removal (`tenstorrent_pci_remove`, enumerate.c:404-481) sets `detached = true` under `chardev_mutex` (enumerate.c:423-425), drains in-flight ioctls with `down_write(&reset_rwsem)` around VMA zap and BAR unmap (enumerate.c:444-447), revokes all TLB dma-buf exports (enumerate.c:459), wakes `resource_lock_waitqueue` so `ACQUIRE_BLOCKING` waiters observe `detached` (enumerate.c:461-463), and runs `tenstorrent_memory_cleanup()` on every still-open fd (enumerate.c:465-467).

### 3. Argument-passing protocols

Four distinct conventions coexist. This is the single most important ABI subtlety.

#### 3a. `output_size_bytes` protocol (ioctls 0, 5, 6, 7, 8)

The struct is `struct { in; out; }`; `in.output_size_bytes` is the number of bytes of user buffer available at `&arg->out`. The handler:

1. `copy_from_user(&in, &arg->in, sizeof(in))` — fixed size, `-EFAULT` on failure.
2. `clear_user(&arg->out, in.output_size_bytes)` — zero-fills the **entire declared** output area, `-EFAULT` if any byte is unwritable. A caller declaring more output space than it mapped fails here.
3. `bytes_to_copy = min(in.output_size_bytes, (u32)sizeof(out)); copy_to_user(&arg->out, &out, bytes_to_copy)`.

Example (GET_DEVICE_INFO, chardev.c:151-157):
```c
if (clear_user(&arg->out, in.output_size_bytes) != 0)
	return -EFAULT;
bytes_to_copy = min(in.output_size_bytes, (u32)sizeof(out));
if (copy_to_user(&arg->out, &out, bytes_to_copy) != 0)
	return -EFAULT;
```

**Forward/backward compatibility rules this implies:**
- An *old* userspace on a *new* kernel gets the struct truncated to its declared size (fields it doesn't know are silently dropped).
- A *new* userspace on an *old* kernel gets its extra output space **zeroed** (clear_user covers the whole declared size), so new fields read as 0.
- `out.output_size_bytes` (first field of the out struct, where present) is set by the kernel to `sizeof(out)` so userspace can detect how much the kernel actually produced (chardev.c:142, 175, 292).
- `PIN_PAGES` is the outlier: its `clear_user` happens **before** input validation (memory.c:572-573), and the kernel may write up to `sizeof(tenstorrent_pin_pages_out_extended)` = 16 bytes even though the nominal combined `struct tenstorrent_pin_pages` only reserves 8 output bytes (see ioctl 7).

#### 3b. Count-based protocol (ioctl 2, QUERY_MAPPINGS)

`in.output_mapping_count` is a count of `struct tenstorrent_mapping` slots. Kernel copies `min(count, valid)` entries and zero-fills the remaining declared slots (memory.c:393-406).

#### 3c. `argsz` protocol (ioctls 14, 15, 16)

Single flat struct (no in/out split). The kernel copies the whole struct in, requires `argsz == sizeof(struct)` **exactly** (`-EINVAL` otherwise) and `flags == 0`. No truncation tolerance — a future struct growth requires a new size handshake. E.g. chardev.c:448-453, chardev.c:570-574, memory.c:1164-1169. Only EXPORT_TLB_DMABUF copies the struct back out (with `fd` filled, memory.c:1264-1269); SET_NOC_CLEANUP and SET_POWER_STATE produce no output.

#### 3d. Fixed-size, no size negotiation (ioctls 3, 4, 9, 10, 11, 12, 13)

Fixed `sizeof(in)` copied in, fixed `sizeof(out)` copied out (or nothing). Any struct growth would be a hard ABI break.

### 4. mmap offset namespace (returned by ioctls 2, 3, 11)

The single char device multiplexes all mappable entities by mmap offset (memory.c:255-274, dispatched in `tenstorrent_mmap`, memory.c:1585-1636):

```c
#define MMAP_OFFSET_RESOURCE0_UC	(U64_C(0) << 36)
#define MMAP_OFFSET_RESOURCE0_WC	(U64_C(1) << 36)
#define MMAP_OFFSET_RESOURCE1_UC	(U64_C(2) << 36)
#define MMAP_OFFSET_RESOURCE1_WC	(U64_C(3) << 36)
#define MMAP_OFFSET_RESOURCE2_UC	(U64_C(4) << 36)
#define MMAP_OFFSET_RESOURCE2_WC	(U64_C(5) << 36)
#define MMAP_OFFSET_TLB_UC		(U64_C(6) << 36)
#define MMAP_OFFSET_TLB_WC		(U64_C(7) << 36)
#define MMAP_RESOURCE_SIZE (U64_C(1) << 36)
#define MMAP_OFFSET_DMA_BUF		((u64)(PAGE_SIZE-U8_MAX-1) << 32)
#define MMAP_SIZE_DMA_BUF (U64_C(1) << 32)
```
(memory.c:258-274)

- "Resource 0/1/2" = PCI BAR 0/2/4 respectively (memory.c:352, 365, 378). Each is exposed twice: UC (`pgprot_device`) and WC (`pgprot_writecombine`) (memory.c:1596-1618).
- A mapping must be entirely contained in one entity's window; `vma_target_range()` rebases `vm_pgoff` to be entity-relative on match (memory.c:1330-1342).
- DMA buffer for `buf_index` i maps at `MMAP_OFFSET_DMA_BUF + i * MMAP_SIZE_DMA_BUF` (memory.c:421-423). With 4 KiB pages that base is `3840 << 32` = 0xF00_0000_0000, chosen so offset/PAGE_SIZE fits in 32 bits (comment, memory.c:269-272).
- TLB windows map at `MMAP_OFFSET_TLB_{UC,WC} + encoded_id` where `encoded_id = tlb_desc.bar_offset` (+ `BAR0_SIZE` = `1UL<<29` for Blackhole BAR4 4G windows) (memory.c:926-934, 29).
- BAR mmap requires nothing but the range check; **TLB-window mmap requires the fd to own the window** (`test_bit(id, priv->tlbs)` else `-EPERM`, memory.c:1541-1544) and forbids VMA splits (memory.c:1478-1492).
- Every BAR/TLB VMA is tracked on `priv->vma_list` so `tenstorrent_vma_zap()` can shoot down PTEs across all processes on reset/removal (memory.c:1370-1475, 1677-1742).

#### mapping_id constants

```c
#define TENSTORRENT_MAPPING_UNUSED		0
#define TENSTORRENT_MAPPING_RESOURCE0_UC	1
#define TENSTORRENT_MAPPING_RESOURCE0_WC	2
#define TENSTORRENT_MAPPING_RESOURCE1_UC	3
#define TENSTORRENT_MAPPING_RESOURCE1_WC	4
#define TENSTORRENT_MAPPING_RESOURCE2_UC	5
#define TENSTORRENT_MAPPING_RESOURCE2_WC	6
```
(ioctl.h:33-39) — "These are not array indices" (ioctl.h:32).

> **Porting note:** Windows has no mmap-offset multiplexing on a control device. The natural KMDF equivalent is a "map request" ioctl that MDL-maps a BAR sub-range (or common-buffer DMA allocation) into user space and returns the user VA, with cache attribute chosen via `MmMapLockedPagesSpecifyCache` (`MmNonCached` for UC, `MmWriteCombined` for WC). The port must still preserve the *offset tokens* returned by QUERY_MAPPINGS/ALLOCATE_DMA_BUF/ALLOCATE_TLB as opaque handles so tt-umd's logic maps 1:1, and must implement the reset-time zap (Linux `tenstorrent_vma_zap`) — on Windows that means tracking user mappings and unmapping/revoking them on reset (there is no PTE-zap equivalent; MDL mappings must be torn down explicitly, or the design must forbid reset while mappings exist).

### 5. Per-fd and per-device state touched by ioctls

Per-fd `struct chardev_private` (chardev_private.h:55-78): device pointer, `mutex`, `dmabufs` hashtable (16 buckets, chardev_private.h:42,58), `pinnings` list, `peer_mappings` list, `vma_list`+`vma_lock`, `pid`/`comm`, `resource_lock` bitmap (64 bits), `open_fd` list node, `tlbs` bitmap (256 bits), `noc_cleanup` struct, `power_state` struct, `open_reset_gen`.

Per-device `struct tenstorrent_device` (device.h:26-83): `detached`, `needs_hw_init`, `reset_gen`, `reset_rwsem`, `chardev_mutex`, `chardev_excl_held`+waitqueue, `resource_lock` bitmap + waitqueue, `open_fds_list`, `power_down_work`, `tlbs` bitmap + `tlb_counts[]` + `tlb_refcount[]`, `iatu_mutex` + `outbound_iatus[16]`, `dmabuf_exports` list + `dmabuf_export_lock`.

**Close (release) ordering** (chardev.c:922-958), under `reset_rwsem` shared:
1. NOC cleanup write if enabled and not detached (chardev.c:931, 865-875).
2. `tenstorrent_memory_cleanup`: free all DMA bufs (+iATU teardown), unpin all pinnings (+iATU/dma unmap/dirty unpin), unmap all peer mappings (memory.c:1638-1668).
3. Release all resource locks held by this fd; wake waiters (chardev.c:933, 877-885).
4. Free all TLB windows owned by this fd (`tenstorrent_device_free_tlb` per set bit; exports keep windows alive via refcount) (chardev.c:934, 887-892).
5. Under `chardev_mutex`: unlink from `open_fds_list`; re-aggregate/defer power (chardev.c:936-941, 894-920); clear `O_EXCL` hold and wake open waiters if list now empty (chardev.c:943-946).
6. Drop device kref, put pid, free priv (chardev.c:952-955).

---

### The 17 ioctls

### Ioctl 0 — TENSTORRENT_IOCTL_GET_DEVICE_INFO (cmd 0xFA00)

Handler: `ioctl_get_device_info` (chardev.c:128-160). Protocol §3a.

**Structs** (ioctl.h:46-65):

`tenstorrent_get_device_info_in` — 4 bytes:
| off | field | type | semantics |
|---|---|---|---|
| 0 | output_size_bytes | __u32 | bytes available at `&arg->out` |

`tenstorrent_get_device_info_out` — 20 bytes:
| off | field | type | semantics |
|---|---|---|---|
| 0 | output_size_bytes | __u32 | set by kernel = 20 |
| 4 | vendor_id | __u16 | PCI vendor (0x1E52) |
| 6 | device_id | __u16 | PCI device id |
| 8 | subsystem_vendor_id | __u16 | PCI subsystem vendor |
| 10 | subsystem_id | __u16 | PCI subsystem id |
| 12 | bus_dev_fn | __u16 | `PCI_DEVID(bus, devfn)` = bus<<8 \| dev<<3 \| fn; header comment "[0:2] function, [3:7] device, [8:15] bus" (ioctl.h:56) |
| 14 | max_dma_buf_size_log2 | __u16 | `MAX_DMA_BUF_SIZE_LOG2` = 28 (memory.h:10) |
| 16 | pci_domain | __u16 | `pci_domain_nr(pdev->bus)` (since 1.23) |
| 18 | reserved | __u16 | 0 |

Combined `tenstorrent_get_device_info`: in @0, out @4, total 24 bytes.

**Validation:** none beyond copy/clear faults. `in.output_size_bytes` is not sanity-checked; huge values just make `clear_user` fail `-EFAULT`.

**Semantics:** pure read of cached `pci_dev` identity (chardev.c:142-149). No side effects, no per-fd/device state change.

**Errors:** `-EFAULT` (copy_from_user/clear_user/copy_to_user); `-ENODEV` from entry gates. **Allowed during the `needs_hw_init` window** (chardev.c:617).

**Concurrency:** only `reset_rwsem` shared; no other locks. Non-blocking.

**Lifetime:** stateless.

Used by tt-umd (`tt-umd/device/pcie/pci_device.cpp:177`).

### Ioctl 1 — TENSTORRENT_IOCTL_GET_HARVESTING (cmd 0xFA01)

**There is no handler.** The dispatch case is an empty `break`, leaving `ret` at its initialization value:

```c
long ret = -EINVAL;                          // chardev.c:595
...
case TENSTORRENT_IOCTL_GET_HARVESTING:
	break;                               // chardev.c:631-632
```

**Semantics:** always returns `-EINVAL` (after passing the detached/reset-gen/needs_hw_init gates, which can return `-ENODEV` first). No struct is read or written. tt-umd's vendored `ioctl.h` defines it (tt-umd/device/pcie/ioctl.h:19) but no tt-umd code calls it.

> **Porting note:** implement as an immediate STATUS_INVALID_PARAMETER (the -EINVAL analog) stub; do not invent harvesting data.

### Ioctl 2 — TENSTORRENT_IOCTL_QUERY_MAPPINGS (cmd 0xFA02)

Handler: `ioctl_query_mappings` (memory.c:331-409). Protocol §3b.

**Structs** (ioctl.h:67-86):

`tenstorrent_query_mappings_in` — 8 bytes:
| off | field | type | semantics |
|---|---|---|---|
| 0 | output_mapping_count | __u32 | number of `tenstorrent_mapping` slots at `&arg->out` |
| 4 | reserved | __u32 | **not validated** |

`tenstorrent_mapping` — 24 bytes, align 8:
| off | field | type | semantics |
|---|---|---|---|
| 0 | mapping_id | __u32 | TENSTORRENT_MAPPING_* (§4) |
| 4 | reserved | __u32 | 0 |
| 8 | mapping_base | __u64 | mmap offset token (N<<36, §4) |
| 16 | mapping_size | __u64 | `pci_resource_len` of the BAR |

`tenstorrent_query_mappings_out` is a zero-length array of mappings at offset 8 of the combined struct (ioctl.h:79-81; kernel-side re-declared as a flexible array to appease UBSAN, memory.c:326-329).

**Semantics:** builds up to 6 mappings — for each of BAR0, BAR2, BAR4 with nonzero `pci_resource_len`, emits the UC and WC pair with the hard-coded bases from §4 (memory.c:352-389). Copies `min(output_mapping_count, valid)` entries, then `clear_user`s the remaining declared slots (memory.c:393-406).

**Validation / errors:** `-EFAULT` on copy faults; `-EFAULT` (not -EINVAL) if `extra_mappings_to_clear * 24` would overflow u32: `if (U32_MAX / sizeof(struct tenstorrent_mapping) < extra_mappings_to_clear) return -EFAULT;` (memory.c:397-398). Never fails for small counts — count 0 succeeds copying nothing.

**Concurrency:** no locks beyond `reset_rwsem` shared. **Side effects:** none. **Lifetime:** stateless.

Used by tt-umd (pci_device.cpp:451).

### Ioctl 3 — TENSTORRENT_IOCTL_ALLOCATE_DMA_BUF (cmd 0xFA03)

Handler: `ioctl_allocate_dma_buf` (memory.c:425-513). Protocol §3d (fixed 24-byte in, fixed 40-byte out).

**Structs** (ioctl.h:88-111):

`tenstorrent_allocate_dma_buf_in` — 24 bytes, align 8:
| off | field | type | semantics |
|---|---|---|---|
| 0 | requested_size | __u32 | bytes; multiple of PAGE_SIZE, ≤ 1<<28 |
| 4 | buf_index | __u8 | [0, 256); key for mmap offset and hashtable |
| 5 | flags | __u8 | bit 1 (`TENSTORRENT_ALLOCATE_DMA_BUF_NOC_DMA` = 2, ioctl.h:89); **other bits silently ignored** |
| 6 | reserved0[2] | __u8 | not validated |
| 8 | reserved1[2] | __u64 | not validated |

`tenstorrent_allocate_dma_buf_out` — 40 bytes:
| off | field | type | semantics |
|---|---|---|---|
| 0 | physical_address | __u64 | dma_handle from `dma_alloc_coherent` ("or IOVA", ioctl.h:100) |
| 8 | mapping_offset | __u64 | mmap token = `MMAP_OFFSET_DMA_BUF + buf_index*(1<<32)` (memory.c:495, 421-423) |
| 16 | size | __u32 | echo of requested_size |
| 20 | reserved0 | __u32 | 0 |
| 24 | noc_address | __u64 | valid iff NOC_DMA flag set: `noc_pcie_offset + iATU base` (memory.c:196) |
| 32 | reserved1 | __u64 | 0 |

Combined: in @0, out @24, total 64 bytes.

**Validation:** `!priv->device->dma_capable` → `-EINVAL` (memory.c:442-443; dma_capable = `dma_set_mask(64-bit or module param)` succeeded, enumerate.c:330). `buf_index >= TENSTORRENT_MAX_DMA_BUFS(256)` → `-EINVAL`. `requested_size % PAGE_SIZE != 0 || == 0 || > MAX_DMA_BUF_SIZE (1u<<28)` → `-EINVAL` (memory.c:445-451, 253). Duplicate `buf_index` on this fd → `-EINVAL` (memory.c:455-458).

**Semantics:** under `priv->mutex`: allocate tracking struct; `dma_alloc_coherent` of requested_size (memory.c:466-468). If NOC_DMA flag: `setup_noc_dma(top_down=true)` — under `tt_dev->iatu_mutex`, find a free address range below `dev_class->noc_dma_limit` among the 16 outbound iATU regions, program hardware via `dev_class->configure_outbound_atu`, record owner=priv (memory.c:172-200, 136-169); `out.noc_address = noc_pcie_offset + base`. NOC constants: WH `noc_dma_limit = 0xFFFDFFFF`, `noc_pcie_offset = 0x8_0000_0000` (wormhole.c:1059-1060); BH limit `(1<<58)-1`, offset `4<<58` (blackhole.c:817-818). Fill out; `copy_to_user` of full 40 bytes; **on -EFAULT the buffer and iATU region are torn down** (memory.c:498-506); on success `hash_add` into `priv->dmabufs` (memory.c:508).

**Errors:** `-EFAULT`, `-EINVAL` (above), `-ENOMEM` (kzalloc / dma_alloc_coherent / no iATU address-space gap, memory.c:189-192), `-ENOSPC` (all 16 iATU regions in use, memory.c:154-155), or `configure_outbound_atu`'s error.

**Concurrency:** `priv->mutex` for the whole body; `iatu_mutex` inside setup_noc_dma. Non-blocking.

**Side effects / lifetime:** per-fd hashtable entry + optional per-device iATU region owned by this fd. **There is no free ioctl** (see ioctl 4); the buffer, its mmap, and its iATU region live until fd close (`tenstorrent_memory_cleanup`, memory.c:1649-1654; iATU hardware is deprogrammed unless `detached`, memory.c:276-297). mmap of the buffer goes through `dma_mmap_coherent` (memory.c:1629-1632).

Used by tt-umd (pci_device.cpp:971).

### Ioctl 4 — TENSTORRENT_IOCTL_FREE_DMA_BUF (cmd 0xFA04)

Handler: `ioctl_free_dma_buf` (memory.c:515-523). **Unconditionally returns `-EINVAL`**, with the comment: "This is unsupported until I figure out how to block freeing as long as a mapping exists. Otherwise the dma buffer is freed when the struct file is destroyed" (memory.c:518-521). Input/output structs are empty (ioctl.h:113-122); nothing is copied. DMA buffers are only freed at fd close.

### Ioctl 5 — TENSTORRENT_IOCTL_GET_DRIVER_INFO (cmd 0xFA05)

Handler: `ioctl_get_driver_info` (chardev.c:162-190). Protocol §3a.

**Structs** (ioctl.h:124-140):

`tenstorrent_get_driver_info_in` — 4 bytes: `output_size_bytes` __u32 @0.

`tenstorrent_get_driver_info_out` — 12 bytes:
| off | field | type | value |
|---|---|---|---|
| 0 | output_size_bytes | __u32 | 12 |
| 4 | driver_version | __u32 | `TENSTORRENT_DRIVER_VERSION` = 2 (ioctl.h:10) — "IOCTL API version" |
| 8 | driver_version_major | __u8 | 2 (module.h:19) |
| 9 | driver_version_minor | __u8 | 10 (module.h:20) |
| 10 | driver_version_patch | __u8 | 1 (module.h:21) |
| 11 | reserved0 | __u8 | 0 |

Combined: in @0, out @4, total 16 bytes.

**Validation/errors/concurrency:** identical shape to ioctl 0: only `-EFAULT`/`-ENODEV`; no locks; allowed during `needs_hw_init` (chardev.c:618). Stateless.

**[UNVERIFIED]** tt-umd gates features on this (kmd_versions.hpp; pci_device.cpp:158). GET_DRIVER_INFO is not called in pci_device.cpp; the driver version is fetched via `tt_driver_get_attr(TT_DRIVER_API_VERSION)` (pci_device.cpp:430) through the `tt_device_t` abstraction, so the `:158` citation (a `}`) does not back this claim.

### Ioctl 6 — TENSTORRENT_IOCTL_RESET_DEVICE (cmd 0xFA06)

Handler: `ioctl_reset_device` (chardev.c:200-310). Protocol §3a. **Dispatch takes `reset_rwsem` exclusive** (chardev.c:598-599), so it runs with no other ioctl/mmap/open-body/release-body in flight.

**Structs** (ioctl.h:142-166):

Flags:
```c
#define TENSTORRENT_RESET_DEVICE_RESTORE_STATE 0    // legacy
#define TENSTORRENT_RESET_DEVICE_RESET_PCIE_LINK 1  // legacy
#define TENSTORRENT_RESET_DEVICE_CONFIG_WRITE 2     // legacy
#define TENSTORRENT_RESET_DEVICE_USER_RESET 3
#define TENSTORRENT_RESET_DEVICE_ASIC_RESET 4
#define TENSTORRENT_RESET_DEVICE_ASIC_DMC_RESET 5
#define TENSTORRENT_RESET_DEVICE_POST_RESET 6
```
(ioctl.h:143-151)

`tenstorrent_reset_device_in` — 8 bytes: `output_size_bytes` __u32 @0, `flags` __u32 @4.
`tenstorrent_reset_device_out` — 8 bytes: `output_size_bytes` __u32 @0 (=8), `result` __u32 @4 (**0 = success, 1 = failure**: `out.result = !ok`, chardev.c:293).
Combined: in @0, out @8, total 16.

**Validation:** unknown flag value → `-EINVAL` (chardev.c:288-289). Flags 1,2,3,4,5 (all destructive variants) are refused with `-EBUSY` if any TLB dma-buf export is live (`tenstorrent_has_tlb_dmabuf_exports`, chardev.c:222-233, memory.c:1299-1308) — rationale: a pin-only P2P importer can't be revoked and in-flight P2P DMA during reset can wedge the host (ioctl.h:429-433).

**Semantics, step by step** (chardev.c:236-299):
1. `cancel_delayed_work_sync(&tt_dev->power_down_work)` — drain deferred idle powerdown before touching hardware (chardev.c:238).
2. Per flag:
   - **0 RESTORE_STATE**: `safe_pci_restore_state(pdev)` (requires saved state and vendor-ID readback == 0x1E52; restores+re-saves config space, pcie.c:43-59) then `dev_class->restore_reset_state` + `init_hardware`; `ok=false` if restore fails (chardev.c:240-246). No gen bump, no zap.
   - **1 RESET_PCIE_LINK**: `tenstorrent_vma_zap` then `pcie_hot_reset_and_restore_state` — secondary-bus reset via bridge `PCI_BRIDGE_CTL_BUS_RESET`, 2 ms assert, 500 ms settle, poll link up for 10000 ms, restore state (pcie.c:61-90). No gen bump.
   - **2 CONFIG_WRITE**: `bump_reset_gen` + zap; `pcie_timer_interrupt` — config writes `INTERFACE_TIMER_TARGET_OFF (0x934) = 0x1`, `INTERFACE_TIMER_CONTROL_OFF (0x930) = INTERFACE_TIMER_EN|INTERFACE_FORCE_PENDING (0x11)` (pcie.c:16-22, 133-138).
   - **3 USER_RESET**: bump + zap; `set_reset_marker` — sets `PCI_COMMAND_PARITY` bit as a marker that firmware clears on completed reset (pcie.c:140-149); `needs_hw_init = true`.
   - **4 ASIC_RESET / 5 ASIC_DMC_RESET**: bump + zap; `dev_class->reset(tt_dev, flags)`; `needs_hw_init = true` (chardev.c:259-268).
   - **6 POST_RESET**: `ok = is_reset_marker_zero(pdev)` (PCI_COMMAND parity bit cleared, pcie.c:151-158). If `needs_hw_init`: clear it, `safe_pci_restore_state` + `restore_reset_state` + `init_hardware` + optional `probe_telemetry` (chardev.c:269-287). No gen bump — the resetter's fd stays usable.
3. `out.result = !ok`; wake `resource_lock_waitqueue` so pre-reset `ACQUIRE_BLOCKING` waiters observe the gen bump and fail `-ENODEV` (chardev.c:292-299).
4. Output per §3a.

**Errors:** `-EFAULT`, `-EINVAL` (bad flag), `-EBUSY` (live dmabuf export). Note: a *failed* reset still returns ioctl status 0 with `out.result = 1`.

**Side effects:** `bump_reset_gen` (flags 2-5) permanently invalidates **every other open fd** on the device (they get `-ENODEV` on all subsequent ioctls/mmaps); the caller's fd is migrated to the new generation (chardev.c:192-198). `tenstorrent_vma_zap` clears user PTEs of all BAR/TLB mappings in all processes (memory.c:1677-1742). `needs_hw_init` gates all but ioctls 0/5/6 until POST_RESET. Resource-lock bits are *not* cleared by reset — "Resource locks survive reset ... only close(fd) clears the bits" (chardev.c:312-316).

**Concurrency:** exclusive `reset_rwsem`; `dmabuf_export_lock` briefly; blocking on `cancel_delayed_work_sync` and (flag 1) up to ~10.5 s of sleeps in the PCIe hot reset path.

**[UNVERIFIED]** Used by tt-umd (pci_device.cpp mention at :545 and its ioctl.h). Line 545 is blank; tt-umd issues reset via `send_reset_ioctl`/`tt_device_reset` (pci_device.cpp:200-215, called at 905), not a raw ioctl at `:545`.

### Ioctl 7 — TENSTORRENT_IOCTL_PIN_PAGES (cmd 0xFA07)

Handler: `ioctl_pin_pages` (memory.c:544-744). Protocol §3a, **with the extended-output quirk**.

**Structs** (ioctl.h:168-208):

Flags:
```c
#define TENSTORRENT_PIN_PAGES_CONTIGUOUS 1   // app attests pages are physically contiguous (advisory; not read by kmd)
#define TENSTORRENT_PIN_PAGES_NOC_DMA 2      // map into NOC address space via outbound iATU
#define TENSTORRENT_PIN_PAGES_NOC_TOP_DOWN 4 // NOC DMA allocated top-down
#define TENSTORRENT_PIN_PAGES_READ_ONLY 8    // device reads only; requires IOMMU translation
```
(ioctl.h:169-172)

`tenstorrent_pin_pages_in` — 24 bytes: `output_size_bytes` __u32 @0; `flags` __u32 @4; `virtual_address` __u64 @8; `size` __u64 @16.

`tenstorrent_pin_pages_out` — 8 bytes: `physical_address` __u64 @0 ("or IOVA").
`tenstorrent_pin_pages_out_extended` — 16 bytes: `physical_address` __u64 @0; `noc_address` __u64 @8 (ioctl.h:185-188).

Combined `tenstorrent_pin_pages`: in @0, out (**8-byte, non-extended**) @24, total 32. **The kernel writes `min(output_size_bytes, 16)` bytes at `&arg->out`** (memory.c:565, 708-710) — a caller wanting `noc_address` must allocate 16 bytes at offset 24 (40 total) and pass `output_size_bytes = 16`. tt-umd does exactly this with an ad-hoc `struct { in; out_extended; }` (tt-umd/device/pcie/pci_device.cpp:651-660).

**Validation (in order):** `-EFAULT` on copy-in; `clear_user(&arg->out, in.output_size_bytes)` **before any validation** → `-EFAULT` (memory.c:572-573); unknown flag bits → `-EINVAL` (memory.c:575-576); VA or size not page-aligned, or size == 0 → `-EINVAL` (memory.c:578-579); size > 1 GiB on kernels ≤ 5.4 → `-EINVAL` (memory.c:532-542, 581); `READ_ONLY` without IOMMU translation → `-EOPNOTSUPP` (memory.c:588-589); duplicate (VA, page-count) pinning on this fd → `-EEXIST` (memory.c:596-605). Note `noc_dma = in.flags & (NOC_DMA | NOC_TOP_DOWN)` — **TOP_DOWN alone also enables NOC DMA** (memory.c:584).

**Semantics** (all under `priv->mutex`, memory.c:594-723):
1. `vzalloc` page-pointer array; `pin_user_pages_fast(FOLL_LONGTERM [| FOLL_WRITE unless read_only])` (memory.c:613-621, 202-207). Partial pin → unpin + `-EINVAL` (memory.c:628-632).
2. **IOMMU path** (memory.c:634-684): build chained sg-table, `dma_map_sgtable` (dir = `DMA_TO_DEVICE` if read_only else `DMA_BIDIRECTIONAL`); verify the mapping is a single contiguous IOVA and total length matches, else `-EINVAL`; `out.physical_address = sg_dma_address(sgl)`; optional `setup_noc_dma` targeting the IOVA.
3. **Non-IOMMU path** (memory.c:685-705): verify pfn-contiguity of all pages, else `-EINVAL`; `out.physical_address = page_to_phys(pages[0])`; optional `setup_noc_dma` targeting phys.
4. `out.noc_address`; copy out min(declared, 16); `-EFAULT` triggers full teardown (memory.c:710-713, 727-743). Success: record `pinned_page_range` {page_count, pages, sg table, VA, iatu_region, read_only} on `priv->pinnings` (memory.c:715-722, memory.h:22-34).

**Errors:** `-EFAULT`, `-EINVAL`, `-EOPNOTSUPP`, `-EEXIST`, `-ENOMEM` (allocs, sg alloc, iATU gap), `-ENOSPC` (iATU regions), or negative return of pin_user_pages.

**Concurrency:** `priv->mutex` throughout, `iatu_mutex` in setup_noc_dma. GUP can block on page faults.

**Lifetime:** pinnings released at UNPIN_PAGES or fd close (`unpin_pinned_page_range`: teardown iATU → `dma_unmap_sgtable` → free sgt → `unpin_user_pages_dirty_lock(dirty=!read_only)` → vfree/kfree, memory.c:299-314). Pinned memory is never accessible via mmap of this device.

Heavily used by tt-umd (pci_device.cpp:611, 660, 714, 753; silicon_sysmem_manager.cpp hugepage path).

### Ioctl 8 — TENSTORRENT_IOCTL_LOCK_CTL (cmd 0xFA08)

Handler: `ioctl_lock_ctl` (chardev.c:371-430) + `acquire_resource_lock_blocking` (chardev.c:323-367). Protocol §3a.

**Structs** (ioctl.h:210-249):

```c
#define TENSTORRENT_LOCK_CTL_ACQUIRE 0          // out.value: 1 = acquired, 0 = held by another
#define TENSTORRENT_LOCK_CTL_RELEASE 1          // out.value: 1 = released, 0 = not held by us
#define TENSTORRENT_LOCK_CTL_TEST 2             // out.value: bit 0 = held by us, bit 1 = held by any
#define TENSTORRENT_LOCK_CTL_ACQUIRE_BLOCKING 3 // blocks until acquired, out.value: 1
```
(ioctl.h:211-214). Conventional indices `TENSTORRENT_LOCK_INDEX_ETH00..15` = 0..15 (ioctl.h:217-232). `TENSTORRENT_RESOURCE_LOCK_COUNT` = 64 (ioctl.h:44).

`tenstorrent_lock_ctl_in` — 12 bytes: `output_size_bytes` __u32 @0; `flags` __u32 @4; `index` __u8 @8; `reserved[3]` @9 (not validated).
`tenstorrent_lock_ctl_out` — 4 bytes: `value` __u8 @0; `reserved[3]` @1.
Combined: in @0, out @12, total 16. Note: unlike ioctls 0/5/6, `out` has no `output_size_bytes` echo field.

**Validation:** `index >= 64` → `-EINVAL` (chardev.c:385-386); unknown `flags` → `-EINVAL` (chardev.c:417-418).

**Semantics.** Two bitmaps: per-device `tt_dev->resource_lock` (who holds each of the 64 locks, device.h:50) and per-fd `priv->resource_lock` (which locks *this fd* holds, chardev_private.h:68). Invariant: "acquire sets global then local; release clears local then global. This ensures the global bit is always set while the local bit is set" (chardev.c:369-370).

- **ACQUIRE** (non-blocking): `test_and_set_bit` on device bitmap; on win, set fd bit, `out.value = 1`; else `out.value = 0` — **success with value 0, not an errno** (chardev.c:389-396).
- **ACQUIRE_BLOCKING**: `acquire_resource_lock_blocking`:
  - **Drops `reset_rwsem` (shared) for the wait** — holding it would deadlock RESET_DEVICE and starve all ioctls because the rwsem is writer-fair (comment chardev.c:318-322; `up_read` at 329, `down_read` re-taken at 344).
  - Loop: `wait_event_interruptible(resource_lock_waitqueue, !test_bit(idx, device) || detached || reset_gen != open_reset_gen)` then `test_and_set_bit` to claim (chardev.c:331-342).
  - Signal during wait → `-ERESTARTSYS` (chardev.c:346-347). (Reaches userspace as `EINTR` unless the syscall is auto-restarted.)
  - If detached or a reset happened — even if the bit was just won, it is given back and waiters re-woken — → `-ENODEV`; "The caller never observes a successful acquire on a fd that is now invalid" (chardev.c:349-360). Defensive `if (!got_bit) return -ENODEV` (chardev.c:362-363).
  - Success: set fd bit, `out.value = 1` (chardev.c:365-366, 397-402).
  - Wakers: RELEASE (chardev.c:406), fd close (chardev.c:884), RESET_DEVICE (chardev.c:299), device removal (enumerate.c:463).
- **RELEASE**: only if this fd holds it (`test_and_clear_bit` on fd bitmap); clears device bit; wakes waitqueue; `out.value = 1`; else `out.value = 0` (chardev.c:403-410). One fd cannot release another fd's lock.
- **TEST**: `out.value = (device_bit << 1) | fd_bit` (chardev.c:412-416).

**Reset interaction:** locks **survive** reset; because pre-reset fds fail the gen gate, the holder can never RELEASE via ioctl after a reset — only its close() clears the bits (chardev.c:312-316).

**Errors:** `-EFAULT`, `-EINVAL`, `-ERESTARTSYS`, `-ENODEV`.

**Concurrency:** atomic bitops only; no mutex. ACQUIRE_BLOCKING is the **only indefinitely-blocking ioctl** in the driver, and it is interruptible.

**Lifetime:** close releases all held locks and wakes waiters (`tt_cdev_release_resource_locks`, chardev.c:877-885).

> **Porting note:** the wait must be alertable/cancelable in the Windows sense (IRP cancellation on CTRL+C / process kill replaces `-ERESTARTSYS`), and the "drop the reset lock while waiting" discipline must be reproduced or replaced with a design where a blocked waiter can never hold up reset/removal.

Not called by tt-umd (defined in its vendored header only).

### Ioctl 9 — TENSTORRENT_IOCTL_MAP_PEER_BAR (cmd 0xFA09)

Handler: `ioctl_map_peer_bar` (memory.c:782-891). Protocol §3d.

**Structs** (ioctl.h:251-268):

`tenstorrent_map_peer_bar_in` — 24 bytes:
| off | field | type | semantics |
|---|---|---|---|
| 0 | peer_fd | __u32 | fd of *another* tenstorrent device |
| 4 | peer_bar_index | __u32 | BAR index on the peer (raw 0..PCI_NUM_RESOURCES-1, **not** the resource-0/1/2 scheme) |
| 8 | peer_bar_offset | __u32 | byte offset into peer BAR |
| 12 | peer_bar_length | __u32 | bytes to map; != 0 |
| 16 | flags | __u32 | must be 0 |
| 20 | reserved | __u32 | not validated |

`tenstorrent_map_peer_bar_out` — 16 bytes: `dma_address` __u64 @0; `reserved` __u64 @8.
Combined: in @0, out @24, total 40.

**Validation:** `-EFAULT`; `flags != 0` → `-EINVAL`; `peer_bar_index >= PCI_NUM_RESOURCES` → `-EINVAL`; `peer_bar_length == 0` → `-EINVAL` (memory.c:802-809). `fget(peer_fd)` fails → `-EBADF` (memory.c:811-813). Peer file not a tenstorrent chardev (`get_tenstorrent_priv` checks `f->f_op == &chardev_fops`, chardev.c:960-966) → `-EINVAL`; peer is the *same* device → `-EINVAL`; different device class (WH vs BH) → `-EINVAL` (memory.c:815-829). Range check `peer_bar_offset >= resource_len || peer_bar_length > resource_len - peer_bar_offset` → `-EINVAL` (memory.c:847-851).

**Semantics:** with **both** fds' mutexes held, ordered by device pointer value to avoid ABBA ("locking in a globally-consistent order", memory.c:837-845): compute peer BAR phys addr, `dma_map_resource(this_device, phys, len, DMA_BIDIRECTIONAL)` — maps the *peer's* BAR into *this* device's DMA/IOMMU domain for P2P — return the dma address (memory.c:853-863). Track on `priv->peer_mappings`. `copy_to_user` failure unmaps and returns `-EFAULT` (memory.c:865-868, 879-880). `dma_mapping_error` → its code (memory.c:855-858). `-ENOMEM` on tracking alloc.

**Concurrency:** two per-fd mutexes; `fget`/`fput` pin the peer file only for the call duration.

**Side effects / lifetime:** the mapping lives on **this** fd; there is **no unmap ioctl**; unmapped at this fd's close (memory.c:1660-1665). No reference is kept to the peer device or fd after return — removal of the peer device while the mapping exists is not handled by this path.

Not called by tt-umd (vendored header only).

### Ioctl 10 — TENSTORRENT_IOCTL_UNPIN_PAGES (cmd 0xFA0A)

Handler: `ioctl_unpin_pages` (memory.c:746-780). Protocol §3d (out struct empty; nothing is written back).

**Structs** (ioctl.h:190-203):

`tenstorrent_unpin_pages_in` — 24 bytes: `virtual_address` __u64 @0 ("original VA used to pin, not current VA if remapped", ioctl.h:192); `size` __u64 @8; `reserved` __u64 @16 (**must be 0**). "unpinning subset of a pinned buffer is not supported" (ioctl.h:190).

**Validation:** `-EFAULT`; `reserved != 0 || size == 0 || (size >> PAGE_SHIFT) == 0` → `-EINVAL` (memory.c:757-760).

**Semantics:** under `priv->mutex`, scan `priv->pinnings` for exact `virtual_address` match; if found but `page_count != size>>PAGE_SHIFT` → `-EINVAL`; else `unpin_pinned_page_range` (iATU teardown → dma unmap → dirty-unpin → free) and return 0 (memory.c:762-776). No match → `-EINVAL` (initial value, memory.c:752).

**Concurrency:** `priv->mutex`. **Lifetime:** removes exactly one pinning; the rest die at close.

Used by tt-umd (pci_device.cpp:788).

### Ioctl 11 — TENSTORRENT_IOCTL_ALLOCATE_TLB (cmd 0xFA0B)

Handler: `ioctl_allocate_tlb` (memory.c:893-944) → `tenstorrent_device_allocate_tlb` (tlb.c:9-54). Protocol §3d.

**Structs** (ioctl.h:270-286):

`tenstorrent_allocate_tlb_in` — 16 bytes: `size` __u64 @0 (must **exactly equal** one of the device's window sizes); `reserved` __u64 @8 (not validated).

`tenstorrent_allocate_tlb_out` — 32 bytes:
| off | field | type | semantics |
|---|---|---|---|
| 0 | id | __u32 | window id, index into the global window space [0, total windows) |
| 4 | reserved0 | __u32 | 0 |
| 8 | mmap_offset_uc | __u64 | `MMAP_OFFSET_TLB_UC (6<<36) + encoded_id` |
| 16 | mmap_offset_wc | __u64 | `MMAP_OFFSET_TLB_WC (7<<36) + encoded_id` |
| 24 | reserved1 | __u64 | 0 |

`encoded_id = tlb_desc.bar_offset`, plus `BAR0_SIZE (1<<29)` if the window lives in BAR4 (memory.c:926-934). Combined: in @0, out @16, total 48.

**Window inventory** (needed to interpret `size`):
- Wormhole (all BAR0): 156 x 1 MiB, 10 x 2 MiB, 20 x 16 MiB (wormhole.c:20-33, 1061-1063); window 185 (the last 16 MiB) is reserved for the kernel at init (`set_bit(KERNEL_TLB_INDEX, tt_dev->tlbs)`, wormhole.c:87, 702).
- Blackhole: 202 x 2 MiB in BAR0 and up to 8 x 4 GiB in BAR4 — 4G count is trimmed to `bar4_len / 4GiB` at probe (blackhole.c:20-31, 580, 820-821); 2 MiB window 201 is kernel-reserved (blackhole.c:40, 609).

**Validation & allocation:** `dev_class->describe_tlb == NULL` → `-EINVAL` (memory.c:902-903). `tenstorrent_device_allocate_tlb`: no kind with `tlb_size == size` → `-EINVAL`; scan that kind's bit range in `tt_dev->tlbs` with `find_next_zero_bit` + `test_and_set_bit`; all busy → `-ENOMEM`; on repeated races, `cond_resched()` and `-ERESTARTSYS` if a signal is pending (tlb.c:20-50). On claim, `refcount_set(&tt_dev->tlb_refcount[id], 1)` (tlb.c:43). Then `describe_tlb` failure or a window in a BAR other than 0/4 frees the window and returns `-EINVAL` (memory.c:913-922). `copy_to_user` failure frees the window, `-EFAULT` (memory.c:936-939). Success: `set_bit(id, priv->tlbs)` — **no `priv->mutex` held**; the bit op itself is atomic (memory.c:941).

**Concurrency:** allocation is lock-free atomic bitmap claim on the *device* bitmap; ownership recording on the *fd* bitmap is a bare set_bit.

**Side effects / lifetime:** window id is a per-device resource owned by this fd; freed by FREE_TLB, or at close via `tt_cdev_release_tlbs` (chardev.c:887-892), except a dma-buf export keeps the underlying window allocated via `tlb_refcount` (tlb.c:71-79). The returned mmap offsets are consumed by `mmap` (`map_tlb_window`, memory.c:1494-1583; requires ownership, `-EPERM` otherwise).

**[UNVERIFIED]** Used by tt-umd (pci_device.cpp:417). Line 417 is unrelated (`if (ret_code != 0)`); TLB allocation goes through `PCIDevice::allocate_tlb` (pci_device.cpp:820) / `SiliconTlbHandle`, and the raw `TENSTORRENT_IOCTL_ALLOCATE_TLB` does not appear in pci_device.cpp.

### Ioctl 12 — TENSTORRENT_IOCTL_FREE_TLB (cmd 0xFA0C)

Handler: `ioctl_free_tlb` (memory.c:946-982) → `tenstorrent_device_free_tlb` (tlb.c:56-81). Protocol §3d (out empty).

**Struct** (ioctl.h:288-298): `tenstorrent_free_tlb_in` — 4 bytes: `id` __u32 @0.

**Validation & semantics:** `-EFAULT`; `id >= TENSTORRENT_MAX_INBOUND_TLBS (256)` → `-EINVAL` (the `in.id < 0` half of the check is dead code on a u32, memory.c:955-956). Under `priv->mutex`: not owned by this fd → `-EPERM` (memory.c:960-963); any live user VMA of this window (scan `priv->vma_list` under `vma_lock` for `TT_VMA_TLB && tlb.id == id`) → `-EBUSY` — **you must munmap before freeing** (memory.c:965-974). Then clear fd bit and `tenstorrent_device_free_tlb`: `id >= total` → `-EINVAL`; device bit not set → `-EPERM`; `refcount_dec_and_test(&tlb_refcount[id])` and only then `clear_bit` — a live dma-buf export keeps the window out of the pool (tlb.c:62-79).

**Errors:** `-EFAULT`, `-EINVAL`, `-EPERM`, `-EBUSY`. **Concurrency:** `priv->mutex` + `priv->vma_lock`. **Lifetime:** same freeing runs per set bit at close (without the VMA check — VMAs are already gone or being torn down by then, chardev.c:887-892).

**[UNVERIFIED]** Used by tt-umd (pci_device.cpp:432, 455). Neither line contains a FREE_TLB call (`log_debug(` and a comment); `TENSTORRENT_IOCTL_FREE_TLB` does not appear anywhere in the tt-umd sources checked out here (freeing is handled inside the `TlbHandle`/`tt_device_t` abstraction).

### Ioctl 13 — TENSTORRENT_IOCTL_CONFIGURE_TLB (cmd 0xFA0D)

Handler: `ioctl_configure_tlb` (memory.c:984-999) → `tenstorrent_device_configure_tlb` (tlb.c:101-108) → per-chip `configure_tlb`. Protocol §3d. **The out struct (`reserved` __u64) is never written** (handler returns directly; memory.c:998).

**Structs** (ioctl.h:300-328):

`tenstorrent_noc_tlb_config` — 32 bytes, align 8:
| off | field | type | semantics |
|---|---|---|---|
| 0 | addr | __u64 | NOC address; must be aligned to the window size |
| 8 | x_end | __u16 | NOC endpoint / multicast rectangle end X |
| 10 | y_end | __u16 | end Y |
| 12 | x_start | __u16 | multicast rectangle start X |
| 14 | y_start | __u16 | start Y |
| 16 | noc | __u8 | NOC select (0/1) |
| 17 | mcast | __u8 | multicast enable |
| 18 | ordering | __u8 | ordering mode (2 bits in hw) |
| 19 | linked | __u8 | linked flag |
| 20 | static_vc | __u8 | static virtual channel (BH: `use_static_vc`) |
| 21 | reserved0[3] | __u8 | not validated |
| 24 | reserved1[2] | __u32 | not validated |

`tenstorrent_configure_tlb_in` — 40 bytes: `id` __u32 @0; `reserved` __u32 @4 (not validated); `config` @8.
`tenstorrent_configure_tlb_out` — 8 bytes (unused). Combined: in @0, out @40, total 48.

**Validation & semantics:** `-EFAULT`; `id >= 256` → `-EINVAL`; **not owned by this fd → `-EPERM`, checked without any lock** (memory.c:992-996). Then chip dispatch (`-EINVAL` if the class has no `configure_tlb`, tlb.c:104-107):
- Wormhole (`wh_configure_tlb`, wormhole.c:873-891): window index in range; `construct_tlb_config` requires `addr` aligned to window size and `addr < 1UL << 36` (`WH_NOC_BITS` = 36, wormhole.c:37, 858-864), else `-EINVAL`; packs address + {x/y start/end (6 bits each), noc_sel, mcast, ordering(2), linked} and writes 2 x 32-bit registers at `TLB_REGS_START + tlb*8`.
- Blackhole (blackhole.c:724-736): 2M windows (id < 202) — `addr & TLB_2M_WINDOW_MASK` → `-EINVAL`; writes 3 x 32-bit regs, and **zeroes the strided-TLB register** for windows that have one ("Strided TLB configuration is unsupported by the CONFIGURE_TLB API", blackhole.c:165-197); 4G windows — `addr & TLB_4G_WINDOW_MASK` → `-EINVAL` (blackhole.c:200-226). Field truncation to hardware widths is silent (e.g. x_end u16 → 6-bit field).

**Errors:** `-EFAULT`, `-EINVAL`, `-EPERM`. **Concurrency:** none beyond `reset_rwsem` shared — the ownership test and the MMIO writes race against concurrent FREE_TLB/close by design (see Open questions). **Side effects:** reprograms the window's NOC routing registers; persistent until reconfigured or device reset. Configuration is not restored after reset.

**[UNVERIFIED]** Used by tt-umd (pci_device.cpp:492, 511). Those lines are unrelated (`LogUMD,` and `if (bar0 == MAP_FAILED)`); configuration goes through `PCIDevice::configure_tlb` (pci_device.cpp:845), and the raw `TENSTORRENT_IOCTL_CONFIGURE_TLB` does not appear in pci_device.cpp.

### Ioctl 14 — TENSTORRENT_IOCTL_SET_NOC_CLEANUP (cmd 0xFA0E)

Handler: `ioctl_set_noc_cleanup` (chardev.c:432-476). Protocol §3c (argsz; input-only).

**Struct** (ioctl.h:330-359) — `tenstorrent_set_noc_cleanup`, 32 bytes, align 8:
| off | field | type | semantics |
|---|---|---|---|
| 0 | argsz | __u32 | must == 32 |
| 4 | flags | __u32 | must == 0 |
| 8 | enabled | __u8 | 1 = register, 0 = clear; values >1 rejected |
| 9 | x | __u8 | NOC tile X (≤ 64) |
| 10 | y | __u8 | NOC tile Y (≤ 64) |
| 11 | noc | __u8 | 0 or 1 |
| 12 | reserved0 | __u32 | **not validated** |
| 16 | addr | __u64 | NOC address, 4-byte aligned |
| 24 | data | __u64 | value; upper 32 bits ignored at fire time |

**Validation:** class without `noc_write32` → `-EOPNOTSUPP` (chardev.c:439-440); `-EFAULT`; `argsz != sizeof` → `-EINVAL`; `flags != 0` → `-EINVAL`; `enabled > 1` → `-EINVAL`; `addr & 0x3` → `-EINVAL`; `noc > 1` → `-EINVAL`; `x > 64 || y > 64` → `-EINVAL` (chardev.c:442-469; coordinate check marked TODO).

**Semantics:** stores the whole struct into `priv->noc_cleanup` under `priv->mutex` (chardev.c:471-473). Registration replaces any previous one (single slot per fd). **At fd close** — including abnormal process death — the driver performs `noc_write32(x, y, addr, data & 0xFFFFFFFF, noc)` unless the device is detached or `enabled == 0` (`tt_cdev_release_noc_cleanup`, chardev.c:865-875, called first in release, chardev.c:931). Purpose per header doc: reliable device-side cleanup if the host app dies (segfault/OOM) (ioctl.h:331-337). On both chips `noc_write32` funnels through the kernel-reserved TLB window under a device mutex (blackhole.c:258-268; equivalent path in wormhole.c).

**Errors:** `-EOPNOTSUPP`, `-EFAULT`, `-EINVAL`. **Reset interaction:** the fire at close is skipped only when `detached`; a post-reset stale fd still fires its write at close (device bit-state permitting).

Present in tt-umd's vendored header; **[UNVERIFIED]** call sites exist in tt-umd (pci_device.cpp:309 area per grep). `TENSTORRENT_IOCTL_SET_NOC_CLEANUP` does not appear anywhere in the tt-umd sources checked out here, so this call-site claim is unsupported.

### Ioctl 15 — TENSTORRENT_IOCTL_SET_POWER_STATE (cmd 0xFA0F)

Handler: `ioctl_set_power_state` (chardev.c:562-589) + aggregation `tenstorrent_set_aggregated_power_state[_locked]` (chardev.c:478-545). Protocol §3c (argsz; input-only).

**Struct** (ioctl.h:361-411) — `tenstorrent_power_state`, 40 bytes, align 4:
| off | field | type | semantics |
|---|---|---|---|
| 0 | argsz | __u32 | must == 40 |
| 4 | flags | __u32 | must == 0 |
| 8 | reserved0 | __u8 | must == 0 |
| 9 | validity | __u8 | bits 0-3 = count of valid power_flags bits (0-15); bits 4-7 = count of valid power_settings entries (0-14); build with `TT_POWER_VALIDITY(f,s)` (ioctl.h:401-404) |
| 10 | power_flags | __u16 | bit 0 `TT_POWER_FLAG_MAX_AI_CLK`; bit 1 `TT_POWER_FLAG_MRISC_PHY_WAKEUP`; bit 2 `TT_POWER_FLAG_TENSIX_ENABLE`; bit 3 `TT_POWER_FLAG_L2CPU_ENABLE` (ioctl.h:406-409) |
| 12 | power_settings[14] | __u16[14] | numeric settings, aggregated by max |

**Validation:** `-EFAULT`; `argsz != 40` → `-EINVAL`; `flags != 0 || reserved0 != 0` → `-EINVAL`; `validity > TT_POWER_VALIDITY(15,14)` (= 0xEF) → `-EINVAL` (chardev.c:567-579).

**Semantics:** store into `priv->power_state` under `priv->mutex` (chardev.c:584-586), then aggregate across all fds under `tt_dev->chardev_mutex` (chardev.c:536-545): for each fd on `open_fds_list` whose `open_reset_gen` matches the device (stale fds are skipped, chardev.c:493-495): flags a client did *not* declare (bits ≥ its flags_count) default to ON — `unspecified_flags_mask = ~((1U << flags_count) - 1) & TT_POWER_FLAG_ALL` (`0x7FFF`, chardev.c:29, 509) — so old clients cannot turn off features they don't know about; effective flags are OR-ed; each declared setting is aggregated by max (chardev.c:514-523). Final message always carries validity (15 flags, max settings count) and goes to firmware via `dev_class->set_power_state` (chardev.c:529-533). The ioctl returns that function's return value (chardev.c:588).

**Interaction with open/close** (documented in ioctl.h:373-383 and implemented in chardev.c:791-863, 894-920):
- open **without** `O_APPEND` (legacy): fd starts with all flags on except MAX_AI_CLK (`validity = TT_POWER_VALIDITY(15,0)`, chardev.c:821-823) and triggers immediate aggregation (chardev.c:852-856).
- open **with** `O_APPEND` (power-aware): starts contributing nothing; expected to call this ioctl.
- close: if `power_policy` module param (default true, module.c:52-54) and the fd contributed anything: last-close on a `defer_idle_powerdown` class arms `power_down_work` after `idle_power_down_grace_ms` (default 5000, module.c:56-57); otherwise re-aggregates synchronously (chardev.c:894-920). RESET_DEVICE and open() cancel pending deferred powerdown (chardev.c:238, 848-850).

**Errors:** `-EFAULT`, `-EINVAL`, or `set_power_state`'s error (firmware message failure). **Concurrency:** `priv->mutex` then `chardev_mutex` (which nests each fd's `priv->mutex` inside during aggregation, chardev.c:497-525). **Lifetime:** contribution removed at close by aggregation-after-list_del (chardev.c:938-941).

**[UNVERIFIED]** Used by tt-umd (pci_device.cpp:525 area; version-gated per kmd_versions.hpp — introduced in KMD 2.6.0). The version gating / "introduced in KMD 2.6.0" is backed by kmd_versions.hpp:39, but the `:525` usage citation is wrong (blank line): power state is set via `PCIDevice::set_power_state`/`tt_device_set_power_state` (pci_device.cpp:1056, 1071).

### Ioctl 16 — TENSTORRENT_IOCTL_EXPORT_TLB_DMABUF (cmd 0xFA10)

Handler: `ioctl_export_tlb_dmabuf` (memory.c:1146-1279; compiled to `-EOPNOTSUPP` stub on kernels < 5.8, memory.c:1310-1315). Protocol §3c (argsz; struct copied back out with `fd` filled).

**Struct** (ioctl.h:413-456) — `tenstorrent_export_tlb_dmabuf`, 32 bytes, align 8:
| off | field | type | semantics |
|---|---|---|---|
| 0 | argsz | __u32 | must == 32 |
| 4 | flags | __u32 | must == 0 |
| 8 | tlb_id | __u32 | window id from ALLOCATE_TLB; must be owned by this fd |
| 12 | fd | __s32 | OUT: dma-buf file descriptor (O_RDWR \| O_CLOEXEC) |
| 16 | offset | __u64 | page-aligned start within the window |
| 24 | size | __u64 | page-multiple byte count; 0 = to end of window |

**Validation:** `-EFAULT`; `argsz != 32` → `-EINVAL`; `flags != 0` → `-EINVAL`; `tlb_id >= 256` → `-EINVAL`; offset or size not page-aligned → `-EINVAL`; no `describe_tlb` → `-EINVAL` (memory.c:1161-1177). Under `priv->mutex`: not owner → `-EPERM` (memory.c:1180-1184); `describe_tlb` failure → `-EINVAL`; `offset >= window size || size > window size` → `-EINVAL`; resolved `[offset, offset+region_size)` beyond window → `-EINVAL`; region beyond the BAR length → `-EINVAL` (memory.c:1186-1211).

**Semantics** (memory.c:1213-1274): compute `phys = BAR start + bar_offset + offset`; take `kref_get(&tt_dev->kref)` and `tenstorrent_tlb_export_get(tlb_id)` (refcount_inc, tlb.c:87-90) so **the export pins both the device object and the window** for the dma-buf's lifetime; `dma_buf_export` with `tt_tlb_dmabuf_ops`; link into `tt_dev->dmabuf_exports` under `dmabuf_export_lock`; allocate an fd (`get_unused_fd_flags(O_CLOEXEC)`); copy struct back with `fd`; `fd_install`. Failure after export uses `dma_buf_put`, whose release callback unwinds list/refs consistently (memory.c:1258-1269, 1118-1135).

**dma-buf ops semantics** (needed to understand device-side contract):
- `attach` refuses importers without `peer2peer` capability → `-EOPNOTSUPP` ("PCI BAR region with no backing struct page", memory.c:1023-1031).
- `map_dma_buf` — under the dmabuf reservation lock — refuses revoked exports (`-ENODEV`), `dma_map_resource`s the BAR range into the **importer's** DMA domain as one contiguous IOVA, and describes it in ≤1 GiB sg entries (`TT_TLB_DMABUF_SG_CHUNK SZ_1G`) because `sg_dma_len` is 32-bit (memory.c:1033-1090).
- `pin` refuses only already-revoked exports (memory.c:1101-1112) — pin-only importers are accepted.
- `release` unlinks the export, drops the window refcount (possibly returning the window to the pool if the owning fd already freed/closed) and the device kref (memory.c:1118-1135).
- Revocation (`tenstorrent_revoke_tlb_dmabufs`): marks each export revoked under its reservation lock and calls `dma_buf_move_notify`; runs at device removal (enumerate.c:459) and suspend (enumerate.c:504) — **not** at reset; instead reset is refused with `-EBUSY` while exports live (chardev.c:222-233; rationale ioctl.h:429-438).

**Errors:** `-EOPNOTSUPP` (old-kernel build), `-EFAULT`, `-EINVAL`, `-EPERM`, `-ENOMEM`, `PTR_ERR(dma_buf_export)`, fd-allocation errors.

**Concurrency:** `priv->mutex` for validation/creation; `dmabuf_export_lock` for the export list; dma_resv locks in ops.

**Lifetime rules (the core of this ioctl):** "An export pins its window: FREE_TLB or close() of the owning fd does not return the window to the allocation pool while the export is live, so the window cannot be reallocated and reconfigured to redirect a live importer's DMA elsewhere on the NOC. The window is freed only once the dma-buf is released." (ioctl.h:434-438; mechanism tlb.c:56-99).

Not called by tt-umd (not even in its vendored header, which predates this ioctl).

> **Porting note:** dma-buf has no direct Windows equivalent. The closest analogs are exporting via NT handles to a section/DXGK shared resource, or supporting third-party P2P through the PnP "function-to-function" DMA path. If the port omits this ioctl, it must also omit the -EBUSY reset guard, but must keep FREE_TLB/close semantics identical otherwise. Returning STATUS_NOT_SUPPORTED matches the old-kernel `-EOPNOTSUPP` behavior.

---

### open()/close() flags that shape ioctl behavior

- `O_EXCL` open: waits (or `-EAGAIN` with `O_NONBLOCK`) until **no other fd** is open, then holds device-exclusivity; non-exclusive opens wait for the O_EXCL holder to go away (`admit_chardev_open`, chardev.c:743-789). Interruptible → `-ERESTARTSYS`.
- `O_APPEND` open: marks the fd "power aware" (see ioctl 15) (chardev.c:795, 817-823).
- Every open records pid/comm for the procfs `pids` file (chardev.c:814-815, 111-113).

### tt-umd usage summary (quick grep; deep analysis is another section's job)

Called from tt-umd code: GET_DEVICE_INFO, GET_DRIVER_INFO, QUERY_MAPPINGS, PIN_PAGES (4 sites incl. extended-out and hugepage paths), UNPIN_PAGES, ALLOCATE_DMA_BUF, ALLOCATE_TLB, FREE_TLB, CONFIGURE_TLB, SET_POWER_STATE, SET_NOC_CLEANUP, RESET_DEVICE **[UNVERIFIED line map]** (tt-umd/device/pcie/pci_device.cpp:177, 158, 451, 611/660/714/753, 788, 971, 417, 432/455, 492/511, 525, 309, 545). Only GET_DEVICE_INFO:177, QUERY_MAPPINGS:451, PIN_PAGES:611/660/714/753, UNPIN_PAGES:788 and ALLOCATE_DMA_BUF:971 match the current tt-umd source. The remaining citations do not match: GET_DRIVER_INFO:158, ALLOCATE_TLB:417, FREE_TLB:432/455, CONFIGURE_TLB:492/511, SET_POWER_STATE:525, SET_NOC_CLEANUP:309, RESET_DEVICE:545 (SET_NOC_CLEANUP is absent from tt-umd entirely; the others are reached through the `tt_device_t` abstraction / `PCIDevice::allocate_tlb`@820, `configure_tlb`@845, `set_power_state`@1056, `send_reset_ioctl`@200/905, `tt_driver_get_attr`@430). Defined-but-unused by tt-umd: GET_HARVESTING, FREE_DMA_BUF, LOCK_CTL, MAP_PEER_BAR; EXPORT_TLB_DMABUF absent from its header.

### Key constants table

| Name | Value | Source |
|---|---|---|
| TENSTORRENT_DRIVER_VERSION (ioctl ABI) | 2 | ioctl.h:10 |
| Driver version major/minor/patch | 2 / 10 / 1 | module.h:19-21 |
| TENSTORRENT_IOCTL_MAGIC | 0xFA | ioctl.h:12 |
| Command values | 0xFA00 + n, n = 0..16 (`_IO`, no size bits) | ioctl.h:14-30 |
| TENSTORRENT_MAX_DMA_BUFS | 256 | ioctl.h:41 |
| TENSTORRENT_MAX_INBOUND_TLBS | 256 | ioctl.h:42 |
| TENSTORRENT_RESOURCE_LOCK_COUNT | 64 | ioctl.h:44 |
| MAX_DMA_BUF_SIZE_LOG2 / MAX_DMA_BUF_SIZE | 28 / 1u<<28 (256 MiB) | memory.h:10; memory.c:253 |
| TENSTORRENT_MAX_OUTBOUND_IATU_REGIONS | 16 | memory.h:65 |
| BAR0_SIZE | 1UL << 29 (512 MiB) | memory.c:29 |
| MMAP_OFFSET_RESOURCE{0,1,2}_{UC,WC} | (0..5) << 36 | memory.c:258-263 |
| MMAP_OFFSET_TLB_UC / TLB_WC | 6<<36 / 7<<36 | memory.c:264-265 |
| MMAP_RESOURCE_SIZE | 1 << 36 | memory.c:267 |
| MMAP_OFFSET_DMA_BUF | (PAGE_SIZE-256) << 32 (0xF00_0000_0000 @4K pages) | memory.c:272 |
| MMAP_SIZE_DMA_BUF | 1 << 32 | memory.c:274 |
| ALLOCATE_DMA_BUF_NOC_DMA flag | 2 | ioctl.h:89 |
| PIN_PAGES flags CONTIGUOUS/NOC_DMA/NOC_TOP_DOWN/READ_ONLY | 1 / 2 / 4 / 8 | ioctl.h:169-172 |
| LOCK_CTL flags ACQUIRE/RELEASE/TEST/ACQUIRE_BLOCKING | 0 / 1 / 2 / 3 | ioctl.h:211-214 |
| RESET_DEVICE flags | 0..6 (see ioctl 6) | ioctl.h:143-151 |
| TT_POWER_FLAG_ALL (kernel aggregation mask) | 0x7FFF | chardev.c:29 |
| TT_POWER_FLAG_{MAX_AI_CLK, MRISC_PHY_WAKEUP, TENSIX_ENABLE, L2CPU_ENABLE} | 1<<0 .. 1<<3 | ioctl.h:406-409 |
| Max validity value accepted | TT_POWER_VALIDITY(15,14) = 0xEF | chardev.c:578 |
| idle_power_down_grace_ms default | 5000 | module.c:56 |
| power_policy default | true | module.c:52 |
| WH TLB windows (BAR0) | 156 x 1M, 10 x 2M, 20 x 16M; id 185 kernel-reserved | wormhole.c:20-33, 87, 702, 1062-1063 |
| WH NOC address width | 36 bits (`WH_NOC_BITS`) | wormhole.c:37, 863 |
| WH noc_dma_limit / noc_pcie_offset | 0xFFFDFFFF / 0x8_0000_0000 | wormhole.c:1059-1060 |
| BH TLB windows | 202 x 2M (BAR0, id 201 kernel-reserved), ≤8 x 4G (BAR4) | blackhole.c:20-31, 40, 580, 609, 820-821 |
| BH noc_dma_limit / noc_pcie_offset | (1<<58)-1 / 4<<58 | blackhole.c:817-818 |
| TT_TLB_DMABUF_SG_CHUNK | 1 GiB | memory.c:1036 |
| PCIe hot reset timings | 2 ms assert, 500 ms settle, 10000 ms link poll | pcie.c:76-80 |
| INTERFACE_TIMER_CONTROL_OFF / TARGET_OFF | 0x930 / 0x934 | pcie.c:17-18 |
| Reset marker | PCI_COMMAND parity-error bit | pcie.c:140-158 |

### Open questions

1. **GET_HARVESTING (ioctl 1) is a stub** returning `-EINVAL` via the initialized `ret` (chardev.c:595, 631-632). Whether any legacy userspace still expects the pre-2.x harvesting payload is unknown; tt-umd does not call it. A Windows port should keep the stub, but confirm no consumer depends on it.
2. **CONFIGURE_TLB / ALLOCATE_TLB ownership races**: `ioctl_configure_tlb` checks `test_bit(in.id, priv->tlbs)` and writes MMIO with no lock (memory.c:992-998), and `ioctl_allocate_tlb` records ownership with a bare `set_bit` (memory.c:941). A concurrent FREE_TLB (or close) on the same fd can free — and another fd can then reallocate — the window between the ownership check and the register write. Is this tolerated intentionally (userspace bug = self-harm within one fd)? A port must decide whether to replicate the loose semantics or serialize under the per-fd mutex.
3. **`NOC_TOP_DOWN` alone enables NOC DMA**: `noc_dma = in.flags & (TENSTORRENT_PIN_PAGES_NOC_DMA | TENSTORRENT_PIN_PAGES_NOC_TOP_DOWN)` (memory.c:584) — flag 4 without flag 2 still allocates an iATU region. Intent unclear; ABI-visible, so the port must match it.
4. **ALLOCATE_DMA_BUF does not reject unknown flag bits** (only bit 1 is examined, memory.c:476) and `TENSTORRENT_PIN_PAGES_CONTIGUOUS` (flag 1) is accepted but never read by the kernel; contiguity is instead verified by the pfn walk in the non-IOMMU path. Ports must accept-and-ignore the same bits for compatibility.
5. **PIN_PAGES extended output overflows the nominal wrapper struct**: kernel writes up to 16 bytes at `&arg->out` where `struct tenstorrent_pin_pages` reserves 8 (ioctl.h:181-208; memory.c:708-710). The port's buffered-ioctl output size must be computed from `in.output_size_bytes`, not `sizeof(struct tenstorrent_pin_pages)`.
6. **Unvalidated reserved fields**: `query_mappings_in.reserved`, `allocate_dma_buf_in.reserved0/1`, `allocate_tlb_in.reserved`, `configure_tlb_in.reserved` + `noc_tlb_config.reserved0/1`, `lock_ctl_in.reserved`, `map_peer_bar_in.reserved`, `set_noc_cleanup.reserved0` are copied but never checked; only `unpin_pages_in.reserved` (memory.c:759) and the argsz-protocol `flags` fields are enforced as zero. Matching this exactly matters for bug-for-bug compatibility.
7. **MAP_PEER_BAR holds no reference to the peer device after return** (memory.c:875, 888): removal of the *peer* device while the mapping is live is not visibly handled in these files. Whether enumerate.c/IOMMU teardown makes this safe is outside this section's scope — flagged for the device-lifecycle section.
8. **`PCI_NUM_RESOURCES`** bound in MAP_PEER_BAR (memory.c:805) is a kernel constant (BAR0-5 + ROM + optional IOV resources); the exact numeric bound is kernel-config-dependent. A port should bound peer_bar_index to real BARs (0-5) unless peer ROM/IOV mapping is truly needed.
9. **`acquire_resource_lock_blocking`'s final `if (!got_bit) return -ENODEV`** (chardev.c:362-363) appears unreachable (detached never clears; reset_gen never reverts); treated as defensive. Port may keep an assert instead — but the `-ERESTARTSYS`-vs-`-ENODEV` precedence (signal checked before detach/reset, chardev.c:346-347) is observable and should be preserved.
10. **`tenstorrent_set_noc_cleanup.data` is __u64 but only the low 32 bits are written** at close (`data & 0xFFFFFFFF`, chardev.c:873); header says "upper 32 bits are ignored" (ioctl.h:347) — validated nowhere, so a port must also silently ignore them.
11. **RESET_DEVICE flag semantics beyond the driver** (what firmware does for ASIC_RESET vs ASIC_DMC_RESET arguments, and the exact contract of the 0x930/0x934 "interface timer" config write) are chip/firmware behavior documented in the chip-support section, not derivable from these files alone.

---

## 05. Memory Management

### Scope

Files covered (tt-kmd @ ttkmd-2.10.0-rc1-1-g8c32c2b, all read in full):

| File | Lines |
|---|---|
| `memory.c` | 1742 |
| `memory.h` | 73 |
| `sg_helpers.c` | 118 |
| `sg_helpers.h` | 41 |

Supporting definitions cited for context: `chardev_private.h` (struct dmabuf, struct chardev_private, struct tenstorrent_mmap_vma), `ioctl.h` (ioctl ABI structs/flags), `device.h`, `wormhole.c`/`blackhole.c` (per-chip NOC DMA constants), `enumerate.c` (DMA masks), `chardev.c` (call sites), `module.c` (`dma_address_bits` parameter).

Note: `ioctl_allocate_tlb` / `ioctl_free_tlb` / `ioctl_configure_tlb` (memory.c:893-999) and `map_tlb_window` also live in memory.c; they are covered here only to the extent needed for the mmap-offset scheme and EXPORT_TLB_DMABUF. The TLB allocator itself (`tlb.c`) belongs to the TLB section.

---

### 1. Per-fd bookkeeping structures

All memory objects are tracked per open file descriptor in `struct chardev_private` (chardev_private.h:55-78):

- `DECLARE_HASHTABLE(dmabufs, DMABUF_HASHTABLE_BITS)` — DMA buffers, keyed by `dmabuf.index`, `DMABUF_HASHTABLE_BITS` = 4 (chardev_private.h:42,58).
- `struct list_head pinnings` — `struct pinned_page_range.list` (chardev_private.h:59).
- `struct list_head peer_mappings` — `struct peer_resource_mapping.list` (chardev_private.h:60).
- `struct list_head vma_list` + `struct mutex vma_lock` — tracked user mappings (chardev_private.h:62-63).
- `priv->mutex` guards the dmabuf hashtable, pinnings list, and peer mappings list. The TLB ownership bitmap (`priv->tlbs`) is *checked* under `priv->mutex` in the free/export/mmap paths (memory.c:958-960, 1180-1184, 1539-1544), but `ioctl_allocate_tlb`'s `set_bit` (memory.c:941) and `ioctl_configure_tlb`'s `test_bit` (memory.c:995) run without it (atomic bitops only).

A DMA buffer record is (chardev_private.h:43-51):

```c
struct dmabuf {
	struct hlist_node hash_chain;
	void *ptr;	// kernel address for dma buffer
	dma_addr_t phys;
	u64 size;	// always a multiple of PAGE_SIZE
	u8 index;
	int outbound_iatu_region;
};
```

A pinned range is (memory.h:22-34):

```c
struct pinned_page_range {
	struct list_head list;
	unsigned long page_count;
	struct page **pages;	// vmalloc/vfree
	struct sg_table dma_mapping;	// alloc_chained_sgt_for_pages / free_chained_sgt
	u64 virtual_address;
	int outbound_iatu_region;
	bool read_only;	// IOMMU forbids device writes
};
```

All three object classes are torn down implicitly at fd release: `tenstorrent_memory_cleanup()` is called from the chardev release path (chardev.c:932) and, holding `priv->mutex`, frees every dmabuf, unpins every pinning, and unmaps every peer mapping (memory.c:1638-1668).

> **Porting note:** In KMDF the natural per-fd container is a file-object context (WdfFileObject context). Cleanup must run in the file-close/cleanup callback (EvtFileCleanup/EvtFileClose) with the same "free everything the fd owned" semantics, including iATU regions programmed on behalf of that fd.

---

### 2. The mmap offset encoding

The driver multiplexes all mappable entities through a single char-dev mmap, using the file offset as a namespace (memory.c:1585-1595 comment). Offsets are nominally dynamic (returned by QUERY_MAPPINGS / ALLOCATE_DMA_BUF / ALLOCATE_TLB) but actually hard-coded (memory.c:255-257 comment):

```c
#define MMAP_OFFSET_RESOURCE0_UC	(U64_C(0) << 36)
#define MMAP_OFFSET_RESOURCE0_WC	(U64_C(1) << 36)
#define MMAP_OFFSET_RESOURCE1_UC	(U64_C(2) << 36)
#define MMAP_OFFSET_RESOURCE1_WC	(U64_C(3) << 36)
#define MMAP_OFFSET_RESOURCE2_UC	(U64_C(4) << 36)
#define MMAP_OFFSET_RESOURCE2_WC	(U64_C(5) << 36)
#define MMAP_OFFSET_TLB_UC		(U64_C(6) << 36)
#define MMAP_OFFSET_TLB_WC		(U64_C(7) << 36)
#define MMAP_RESOURCE_SIZE (U64_C(1) << 36)
```
(memory.c:258-267)

"RESOURCE0/1/2" are PCI BARs 0, 2, 4 respectively (memory.c:1596-1618). Each entity gets a 64 GiB (2^36) slot, in an uncacheable and a write-combining flavor.

DMA buffers live in a separate range (memory.c:269-274):

```c
// tenstorrent_allocate_dma_buf_in.buf_index is u8 so that sets a limit of
// U8_MAX DMA buffers per fd. 32-bit mmap offsets are divided by PAGE_SIZE,
// so PAGE_SIZE << 32 is the largest possible offset.
#define MMAP_OFFSET_DMA_BUF		((u64)(PAGE_SIZE-U8_MAX-1) << 32)
#define MMAP_SIZE_DMA_BUF (U64_C(1) << 32)
```

With 4 KiB pages, `MMAP_OFFSET_DMA_BUF` = (4096−256)·2^32 = `0xF00_0000_0000`. Buffer *n* of an fd maps at `MMAP_OFFSET_DMA_BUF + n * MMAP_SIZE_DMA_BUF` (`dmabuf_mapping_start()`, memory.c:421-423). 256 slots × 4 GiB = 2^40 bytes, ending exactly at 2^44 = `PAGE_SIZE << 32`, the largest possible offset. The TLB_WC slot ends at `8 << 36` = 2^39 = 0x80_0000_0000, far below the DMA range start of 0xF00_0000_0000, so no overlap. Note the formula is PAGE_SIZE-dependent.

#### Dispatch (`tenstorrent_mmap`, memory.c:1585-1636)

`vma_target_range()` (memory.c:1330-1342) checks that the requested window (start `vm_pgoff << PAGE_SHIFT`, length `vm_end − vm_start`) is fully contained in an entity's slot and, on match, **rewrites `vma->vm_pgoff` to be relative to the entity start** (memory.c:1337). Dispatch order: BAR0 UC/WC, BAR1(=BAR2) UC/WC, BAR2(=BAR4) UC/WC, TLB UC/WC, else DMA buffer, else `-EINVAL` (memory.c:1596-1635). Cache attributes are applied via `pgprot_device()` (UC) or `pgprot_writecombine()` (memory.c:1597-1626).

- **BAR mappings** (`map_pci_bar`, memory.c:1441-1475): `vm_iomap_memory(vma, bar_start, bar_len)` — the (rewritten) `vm_pgoff` becomes an offset within the BAR; a `tenstorrent_mmap_vma` tracking record (type `TT_VMA_BAR`) is added to `priv->vma_list` under `vma_lock`, and `vm_ops = &bar_vma_ops` installs open/close callbacks. Errors: `-ENOMEM` on tracking alloc failure, `vm_iomap_memory`'s error otherwise.
- **TLB window mappings** (`map_tlb_window`, memory.c:1494-1583): offset inside the TLB slot encodes the window: it equals the window's BAR0 offset, except windows in Blackhole BAR4 are encoded at `bar_offset + BAR0_SIZE` where `BAR0_SIZE = 1UL << 29` (memory.c:29, 926-931, 1503, 1519-1520). The code linearly scans all windows via `describe_tlb` to find a `bar_offset`/BAR match (memory.c:1523-1531). Checks: `describe_tlb` exists and `tlb_kinds != 0` else `-EINVAL` (1510-1514); window found else `-EINVAL` (1533-1534); `size <= tlb_desc.size` else `-EINVAL` (1536-1537); caller owns the window (`test_bit(id, priv->tlbs)`) else `-EPERM` (1541-1544); window inside BAR else `-ENXIO` (1547-1550); `io_remap_pfn_range` failure gives `-EAGAIN` (1561-1565). VMA is tracked as `TT_VMA_TLB` with the window id. TLB VMAs forbid splitting: `.may_split` (or `.split` pre-5.11) returns `-EINVAL` (memory.c:1478-1492).
- **DMA buffer mappings** (`vma_dmabuf_target`, memory.c:1344-1368): index = `(vm_pgoff − MMAP_OFFSET_DMA_BUF/PAGE_SIZE) / (MMAP_SIZE_DMA_BUF/PAGE_SIZE)`; must be `< TENSTORRENT_MAX_DMA_BUFS`; buffer must exist for this fd; requested range must fit within the *allocated* size (not the 4 GiB slot). The map is performed by `dma_mmap_coherent(&pdev->dev, vma, dmabuf->ptr, dmabuf->phys, dmabuf->size)` (memory.c:1629-1632). DMA-buffer VMAs are **not** tracked in `vma_list` and get no vm_ops (so they are not zapped on reset, and there is no per-VMA bookkeeping — safety comes from the mapping refcounting the struct file, per the comment at memory.c:518-521).

#### VMA lifecycle tracking

`tenstorrent_vma_open` (fork) duplicates the tracking record for the child VMA; if allocation fails it zaps the child's PTEs and orphans the VMA (`vm_private_data = NULL`) rather than fail fork (memory.c:1370-1410). `tenstorrent_vma_close` unlinks and frees the record under `vma_lock` (memory.c:1412-1434). `tenstorrent_vma_zap()` (memory.c:1677-1743) walks every open fd of a device and zaps all tracked BAR/TLB VMAs with `zap_special_vma_range` (`zap_vma_ptes` pre-7.1, memory.c:34-37), used after reset so userspace cannot touch dead device memory. Lock ordering is critical and documented: `mmap_lock` before `vma_lock` (memory.c:1683-1689); the walk takes an `mmget_not_zero` reference per mm, drops `vma_lock`, takes `mmap_read_lock`, re-takes `vma_lock`, zaps all VMAs of that mm, using `list_del_init` so a concurrent `vma_close` is safe (memory.c:1690-1739).

> **Porting note:** Windows has no mmap-offset multiplexing and no VMA callbacks. The equivalent design is ioctl-driven mapping: map BAR/TLB apertures into user space with `MmMapIoSpaceEx` + `MmMapLockedPagesSpecifyCache`(UserMode) or a section object, with UC vs WC as the cache type (`MmNonCached`/`MmWriteCombined`). The port must reproduce: (a) per-fd ownership checks before mapping a TLB window, (b) the no-split property (Windows user mappings can't be partially unmapped, so this is free), (c) revocation on reset — hardest part; there is no `zap_vma_ptes` equivalent, so the port either tracks and force-unmaps (`MmUnmapLockedPages` requires process context) or avoids the problem by failing reset while mappings exist. (d) fork does not exist on Windows; child-inheritance logic can be dropped.

---

### 3. QUERY_MAPPINGS (memory.c:331-409)

Returns up to 6 `tenstorrent_mapping` records (BARs 0/2/4 × UC/WC), each with the hard-coded `mapping_base` offsets above and `mapping_size = pci_resource_len(...)`; BARs with zero length are omitted (memory.c:352-389). The output array is written through a local flexible-array view of the user struct to keep UBSAN quiet (memory.c:323-329). If the user asked for more mappings than exist, the excess entries are zeroed with `clear_user`; a `U32_MAX / sizeof(struct tenstorrent_mapping) < extra_mappings_to_clear` overflow check guards the multiply (memory.c:393-406). Errors: `-EFAULT` only. No locks taken. TLB window offsets are *not* reported here; they come from ALLOCATE_TLB's `mmap_offset_uc/wc` (memory.c:933-934).

---

### 4. DMA buffer allocation (ALLOCATE_DMA_BUF, memory.c:425-513)

Input struct (ioctl.h:91-97): `requested_size` (u32), `buf_index` (u8), `flags` (u8). Output (ioctl.h:99-106): `physical_address` ("or IOVA"), `mapping_offset`, `size`, `noc_address`.

Validation (in order):
1. `copy_from_user` → `-EFAULT` (memory.c:439-440).
2. `!priv->device->dma_capable` → `-EINVAL` (memory.c:442-443). `dma_capable` is set at probe by `dma_set_mask(dev, DMA_BIT_MASK(dma_address_bits ?: 64)) == 0`; the coherent mask uses the device-class width (`dma_address_bits`: Wormhole 32, Blackhole 58) unless overridden by the `dma_address_bits` module parameter (enumerate.c:322-331, wormhole.c:1058, blackhole.c:816, module.c:40-42).
3. `buf_index >= TENSTORRENT_MAX_DMA_BUFS` (= 256, ioctl.h:41) → `-EINVAL` (memory.c:445-446).
4. `requested_size % PAGE_SIZE != 0 || requested_size == 0 || requested_size > MAX_DMA_BUF_SIZE` → `-EINVAL` (memory.c:448-451), where `MAX_DMA_BUF_SIZE = 1u << MAX_DMA_BUF_SIZE_LOG2` and `MAX_DMA_BUF_SIZE_LOG2 = 28` → **256 MiB max per buffer** (memory.c:253, memory.h:10).
5. Under `priv->mutex`: duplicate `buf_index` → `-EINVAL` (memory.c:453-458).

The buffer is allocated with `dma_alloc_coherent(&pdev->dev, in.requested_size, &dma_handle, GFP_KERNEL)` (memory.c:466-468); `-ENOMEM` on failure. `dma_handle` is stored as `dmabuf->phys` and returned to userspace verbatim as `out.physical_address` — under an IOMMU this is an IOVA, not a CPU physical address (memory.c:490, 494; ioctl.h:100 comment "or IOVA"). `out.mapping_offset = dmabuf_mapping_start(buf_index)`; `out.size = requested_size` (memory.c:495-496).

**NOC_DMA flag path**: if `in.flags & TENSTORRENT_ALLOCATE_DMA_BUF_NOC_DMA` (= 2, ioctl.h:89), `setup_noc_dma(priv, top_down=true, size, dma_handle, &out.noc_address)` claims an outbound iATU region targeting the buffer's DMA address and reports the resulting NOC address (memory.c:476-486). DMA-buffer NOC mappings are always allocated **top-down** (`bool top_down = true;`, memory.c:477). Failure frees the coherent buffer and returns the error.

On `copy_to_user` failure everything (iATU region, coherent buffer, record) is unwound and `-EFAULT` returned (memory.c:498-506). Success adds the record to `priv->dmabufs` (memory.c:508). All of this happens under `priv->mutex` (memory.c:453, 511).

**FREE_DMA_BUF is unimplemented**: it unconditionally returns `-EINVAL`; buffers are only freed at fd release, which is safe because a user mapping refcounts the struct file (memory.c:515-523). At release, cleanup does `dma_free_coherent(...)` then `teardown_outbound_iatu(...)` for each buffer (memory.c:1649-1654).

> **Porting note:** `dma_alloc_coherent` maps to a WDF common buffer (`WdfCommonBufferCreate`) or `DMA_ADAPTER::AllocateCommonBuffer`; per-buffer max 256 MiB, page-multiple sizes, up to 256 buffers per handle must be preserved because UMD depends on `buf_index`-based offsets. Windows common buffers give a logical (device bus) address — the direct analogue of `dma_handle` — which is what must be reported in `physical_address`. Note the Linux cleanup order (free memory, then tear down the iATU window still targeting it, memory.c:1650-1651) leaves a brief window where the device aperture points at freed memory; a Windows port should tear down the iATU first.

---

### 5. NOC DMA and the outbound iATU allocator

The device exposes up to `TENSTORRENT_MAX_OUTBOUND_IATU_REGIONS` = 16 outbound iATU regions, tracked device-globally in `tt_dev->outbound_iatus[]`, each recording the owning fd (`priv`), `base`, `limit`, `target` (memory.h:65-71). The array is guarded by `tt_dev->iatu_mutex` (memory.c:183, 284).

`setup_noc_dma(priv, top_down, size, target, *noc_address)` (memory.c:172-200):
- `size == 0` → `-EINVAL` (memory.c:180-181).
- Under `iatu_mutex`, finds a gap in device NOC-DMA address space `[0, dev_class->noc_dma_limit]` using first-fit either **top-down** (`find_iatu_region_top_down`, memory.c:67-99) or **bottom-up** (`find_iatu_region_bottom_up`, memory.c:101-133) over the in-use regions sorted by base (insertion sort of ≤16 indices, memory.c:39-65). No gap → `-ENOMEM` (memory.c:189-192).
- `configure_outbound_iatu` finds a free slot (`-ENOSPC` if none, memory.c:154-155; `base > limit` → `-EINVAL`, memory.c:143-144), programs the hardware via `dev_class->configure_outbound_atu(tt_dev, region, base, limit, target)`, and records ownership (memory.c:136-169).
- The user-visible NOC address is `*noc_address = dev_class->noc_pcie_offset + base` (memory.c:196).

Per-chip constants:
- Wormhole: `.noc_dma_limit = (0xFFFE0000 - 1)`, `.noc_pcie_offset = 0x800000000ULL` (wormhole.c:1059-1060).
- Blackhole: `.noc_dma_limit = (1ULL << 58) - 1`, `.noc_pcie_offset = (4ULL << 58)` (blackhole.c:817-818).

`teardown_outbound_iatu(priv, region)` is a no-op for `region < 0`; otherwise, under `iatu_mutex`, it reprograms the region to `(0,0,0)` **unless `tt_dev->detached`** (device already removed — hardware gone), then clears the slot (memory.c:276-297).

> **Porting note:** The iATU allocator is pure software plus one device-class callback; it ports directly. The 16-region table is per-device state that must live in the device context, protected by a lock usable at PASSIVE_LEVEL (the programming callback does MMIO writes). Ownership by fd matters for cleanup: when a handle closes, only its regions are torn down (via dmabuf/pinning teardown).

---

### 6. PIN_PAGES end to end (memory.c:544-744)

Flags (ioctl.h:168-172):

```c
#define TENSTORRENT_PIN_PAGES_CONTIGUOUS 1	// app attests that the pages are physically contiguous
#define TENSTORRENT_PIN_PAGES_NOC_DMA 2		// app wants to use the pages for NOC DMA
#define TENSTORRENT_PIN_PAGES_NOC_TOP_DOWN 4	// NOC DMA will be allocated top-down (default is bottom-up)
#define TENSTORRENT_PIN_PAGES_READ_ONLY 8	// device will only read; IOMMU enforced, requires IOMMU translation
```

`CONTIGUOUS` is accepted in `valid_flags` (memory.c:547-548) but never otherwise read — contiguity is *always* verified, the flag is vestigial. Note that `noc_dma` is triggered by *either* NOC_DMA *or* NOC_TOP_DOWN (`noc_dma = in.flags & (TENSTORRENT_PIN_PAGES_NOC_DMA | TENSTORRENT_PIN_PAGES_NOC_TOP_DOWN)`, memory.c:584), so TOP_DOWN alone implies a NOC mapping.

#### Validation
1. `copy_from_user` → `-EFAULT` (memory.c:569-570).
2. `clear_user(&arg->out, in.output_size_bytes)` up front → `-EFAULT` (memory.c:572-573). This both validates writability and zero-fills for forward-compat sizing.
3. Unknown flags → `-EINVAL` (memory.c:575-576).
4. `!PAGE_ALIGNED(virtual_address) || !PAGE_ALIGNED(size) || size == 0` → `-EINVAL` (memory.c:578-579).
5. `is_pin_pages_size_safe`: on kernels ≤ 5.4 caps size at `1 << 30` (1 GiB) due to an IOMMU-unmap soft-lockup; on modern kernels always true (memory.c:532-542, 581-582).
6. `READ_ONLY` without IOMMU translation → `-EOPNOTSUPP` (memory.c:588-589). `is_iommu_translated()` = domain exists and is not `IOMMU_DOMAIN_IDENTITY` (memory.c:526-530). Rationale: read-only can only be *enforced* by the IOMMU; there is no way to make host RAM read-only to a bus master otherwise.
7. Under `priv->mutex`: duplicate (VA, page_count) pinning → `-EEXIST` — prevents UNPIN ambiguity over iATU teardown (memory.c:594-605).

#### Pinning
`gup_flags = read_only ? 0 : FOLL_WRITE`; DMA direction `DMA_TO_DEVICE` if read-only else `DMA_BIDIRECTIONAL` (memory.c:591-592). Page-pointer array is `vzalloc`'d (`nr_pages = PAGE_ALIGN(size) >> PAGE_SHIFT`, memory.c:613-614). Pages are pinned with `pin_user_pages_fast(start, nr_pages, gup_flags | FOLL_LONGTERM, pages)` (memory.c:202-207; ≥5.11 path — older kernels fall back to `pin_user_pages`/`get_user_pages` with a temporary vmas array, memory.c:208-242). **FOLL_LONGTERM** is essential: these are indefinite-duration pins. A negative return propagates; a *partial* pin (`pages_pinned != nr_pages`) is `-EINVAL` after unpinning what was pinned (memory.c:621-632).

#### IOMMU path (memory.c:634-684)
- Builds a chained sg_table with `alloc_chained_sgt_for_pages` (see §9); failure → `-ENOMEM`.
- `dma_map_sgtable(&pdev->dev, &dma_mapping, dir, 0)` maps it into the device's IOMMU domain (memory.c:647). Note the driver never calls `iommu_map()` directly; it relies on the DMA-IOMMU layer, then **verifies** the resulting IOVA range is contiguous and complete: iterates `for_each_sgtable_dma_sg`, requiring each entry to start where the previous ended, and total mapped length == `nr_pages * PAGE_SIZE`; any violation → `-EINVAL` with `debug_print_sgtable` diagnostics (memory.c:654-674). The comment says a discontiguous mapping "can only happen due to a misconfiguration or a bug" (memory.c:654).
- `out.physical_address = sg_dma_address(dma_mapping.sgl)` — the base **IOVA** (memory.c:676).
- READ_ONLY enforcement is implicit in `dir = DMA_TO_DEVICE`: the IOMMU PTEs are created without write permission.

#### Direct (no-IOMMU) path (memory.c:685-705)
- Requires the pinned pages to be **physically contiguous**: `page_to_pfn(pages[i]) != page_to_pfn(pages[i-1]) + 1` → `-EINVAL` (memory.c:688-694). No sg_table is built (`dma_mapping` stays zeroed).
- `out.physical_address = page_to_phys(pages[0])` — a raw **CPU physical address** handed to userspace (memory.c:696).

#### NOC DMA
If requested, `setup_noc_dma(priv, top_down, in.size, out.physical_address, &noc_address)` — top-down vs bottom-up per the flag (default bottom-up, memory.c:585) — and `out.noc_address` is returned (memory.c:678-684, 698-704, 707).

#### Output and bookkeeping
`bytes_to_copy = min(in.output_size_bytes, sizeof(out))` where `out` is `tenstorrent_pin_pages_out_extended` {physical_address, noc_address} (memory.c:565, 708-710; ioctl.h:185-188) — old callers passing the short struct only get `physical_address`. The pinning record is added to `priv->pinnings` and `priv->mutex` released (memory.c:715-723).

#### Error unwinding (memory.c:727-743)
Reverse-order labels: teardown iATU (no-op if −1) → `dma_unmap_sgtable` → `free_chained_sgt` (both no-ops on the zeroed table from the direct path) → `unpin_user_pages_dirty_lock(pages, pages_pinned, false)` (not dirtied on failure) → `vfree(pages)` → `kfree(pinning)`; `priv->mutex` released at the end.

#### UNPIN_PAGES (memory.c:746-780)
Input {virtual_address, size, reserved} (ioctl.h:191-195; "original VA used to pin, not current VA if remapped"). Validation: `reserved != 0 || size == 0 || (size >> PAGE_SHIFT) == 0` → `-EINVAL`. Under `priv->mutex`, finds a pinning with matching VA; a VA match with mismatched page count is `-EINVAL`; no match at all is `-EINVAL`; partial unpin is unsupported (ioctl.h:190). On match, `unpin_pinned_page_range()` (memory.c:299-314): tears down the iATU region, `dma_unmap_sgtable` (direction per `read_only`), frees the chained sgt, `unpin_user_pages_dirty_lock(pages, count, !read_only)` — **pages are marked dirty unless the pin was read-only** — then `vfree`/`list_del`/`kfree`. The same function runs for every leftover pinning at fd release (memory.c:1656-1658).

> **Porting note:** The Windows analogue of pin_user_pages(FOLL_LONGTERM) is `MmProbeAndLockPages` on an MDL (IoWriteAccess unless read-only), held for the object's lifetime, plus `MmGetSystemAddressForMdlSafe`-free DMA via `DMA_ADAPTER` or direct PFN use. The two Linux paths translate as: (a) no-IOMMU path = require physically contiguous locked pages (or have UMD allocate via the driver instead), exposing `MmGetPhysicalAddress` of the first page; (b) IOMMU path — Windows offers no general driver-controlled IOMMU domain for arbitrary remapping on client SKUs; DMA remapping (kernel DMA protection) is opaque to drivers. A practical port either requires contiguous memory (large pages / driver-allocated buffers) or uses `GetDmaTransferInfo`/DMA v3 with scatter-gather only if the device could scatter-gather — it cannot: the device needs one contiguous bus range per pinning (that is the whole point of the contiguity check). Expect to drop READ_ONLY (-EOPNOTSUPP equivalent: STATUS_NOT_SUPPORTED) unless running with a controllable remapping layer. The dirty-marking on unpin corresponds to `MmUnlockPages` (Windows dirties automatically for write-locked MDLs).

---

### 7. MAP_PEER_BAR (memory.c:782-891)

Purpose: map a *peer* Tenstorrent device's BAR into *this* device's DMA address space for device-to-device (P2P) transfers. Input {peer_fd, peer_bar_index, peer_bar_offset, peer_bar_length (all u32), flags} (ioctl.h:251-258); output {dma_address} (ioctl.h:260-263).

Validation and flow:
1. `flags != 0` → `-EINVAL`; `peer_bar_index >= PCI_NUM_RESOURCES` → `-EINVAL`; `peer_bar_length == 0` → `-EINVAL` (memory.c:802-809).
2. `fget(in.peer_fd)` → `-EBADF` if not a valid fd (memory.c:811-813). `get_tenstorrent_priv(peer_file)` verifies the fd is actually a tenstorrent chardev → `-EINVAL` (memory.c:815-819).
3. The peer must be a *different* device (`peer_priv->device == priv->device` → `-EINVAL`) of the *same* device class (`dev_class` mismatch → `-EINVAL`) (memory.c:821-829).
4. Both fds' mutexes are taken in a globally consistent order — by device-pointer comparison — to avoid AB/BA deadlock between concurrent MAP_PEER_BAR calls (memory.c:837-845).
5. Bounds: `peer_bar_offset >= resource_len || peer_bar_length > resource_len - peer_bar_offset` → `-EINVAL` (memory.c:847-850).
6. `phys_addr = pci_resource_start(peer_pdev, bar) + offset`; `mapping = dma_map_resource(&priv->device->pdev->dev, phys_addr, length, DMA_BIDIRECTIONAL, 0)` — mapped into the **local** device's DMA/IOMMU domain; `dma_mapping_error` result returned as-is on failure (memory.c:853-858).
7. `out.dma_address = mapping` is returned to userspace (an IOVA or bus address, memory.c:863); on `copy_to_user` failure the resource is unmapped and `-EFAULT` returned (memory.c:865-868, 879-880).
8. The mapping {mapped_address, size} is tracked on the **local** fd's `priv->peer_mappings` list (memory.c:316-321, 870).
9. `fput(peer_file)` in all paths (memory.c:875, 888) — **no lasting reference is kept on the peer file or peer device**. The mapping outlives the peer fd; there is no cross-device refcount. Cleanup is only `dma_unmap_resource` at the local fd's release (memory.c:1660-1665).

> **Porting note:** P2P BAR mapping on Windows means handing device A a bus address inside device B's BAR. With no IOMMU remapping the BAR's physical address *is* the bus address, so the port can validate the peer handle and return `pci_resource_start`-equivalent from B's resource list; with DMA remapping active this is not expressible via public KMDF APIs. The peer-fd validation (must be another tt device of the same class) and the dual-lock ordering-by-address pattern port directly (compare device object pointers). Decide explicitly whether to take a reference on the peer device object (Linux deliberately does not — see Open questions).

---

### 8. EXPORT_TLB_DMABUF (memory.c:1001-1326)

Only compiled for kernels ≥ 5.8; otherwise the ioctl returns `-EOPNOTSUPP` and the revoke/has-exports helpers are no-ops (memory.c:1310-1326). Requires `MODULE_IMPORT_NS(DMA_BUF)` (memory.c:1004-1010).

Exports a TLB window's BAR aperture (a sub-range of it) as a dma-buf fd so other drivers (e.g. an RDMA NIC) can DMA directly into the window (P2P). ABI struct: {argsz, flags, tlb_id, fd (out), offset, size} (ioctl.h:449-456).

#### Export (`ioctl_export_tlb_dmabuf`, memory.c:1146-1279)
Validation: `argsz != sizeof(in)` → `-EINVAL`; `flags != 0` → `-EINVAL`; `tlb_id >= TENSTORRENT_MAX_INBOUND_TLBS` (= 256, ioctl.h:42) → `-EINVAL`; `offset`/`size` page-aligned → `-EINVAL`; `describe_tlb` must exist (memory.c:1161-1177). Under `priv->mutex`: caller must own the window (`test_bit` in `priv->tlbs`) → `-EPERM` (memory.c:1180-1184); `offset >= tlb_desc.size || size > tlb_desc.size` → `-EINVAL`; `size == 0` means "to end of window" (memory.c:1193-1204); the region must lie within the BAR → `-EINVAL` (memory.c:1207-1211). `phys = pci_resource_start(bar) + bar_offset + offset` (memory.c:1213).

Before `dma_buf_export`, the driver takes **two references that live as long as the dma-buf**: `kref_get(&tt_dev->kref)` (device) and `tenstorrent_tlb_export_get(tt_dev, tlb_id)` (window) (memory.c:1227-1234). This is what makes the export survive FREE_TLB and close() of the owning fd — the window cannot return to the pool and be reprogrammed to redirect a live importer's DMA (ioctl.h doc, ioctl.h:434-438). The export record is added to the device-global `tt_dev->dmabuf_exports` list under `dmabuf_export_lock` (memory.c:1252-1254). fd creation: `get_unused_fd_flags(O_CLOEXEC)`, `copy_to_user` of the fd, then `fd_install` (memory.c:1258-1273); failures drop the dma-buf (whose release callback balances the two references).

#### dma_buf ops (memory.c:1137-1144)
- **attach**: rejects importers without `peer2peer` support with `-EOPNOTSUPP` — there are no backing struct pages, only a PCI BAR (memory.c:1023-1031).
- **map**: asserts `dma_resv` held; refuses if `revoked` with `-ENODEV`; maps the whole region into the **importer's** DMA domain with one `dma_map_resource(attach->dev, phys, size, dir, 0)` call, then describes the contiguous IOVA using page-less sg entries split into `TT_TLB_DMABUF_SG_CHUNK = SZ_1G` chunks because `sg_dma_len` is 32-bit (4 GiB truncates to 0) (memory.c:1033-1090). Entry 0 holds the base address for unmap.
- **unmap**: `dma_unmap_resource` on entry 0's address for the full size, free table (memory.c:1092-1099).
- **pin / unpin**: `pin` only refuses already-revoked exports (`-ENODEV`); pinned importers are otherwise accepted (memory.c:1101-1116). Both move_notify-capable and pin-only importers work (ioctl.h:425-427).
- **release**: unlinks from `dmabuf_exports`, drops the window export ref (`tenstorrent_tlb_export_put` — returns the window to the pool if the owning fd is already gone) and the device ref (memory.c:1118-1135).

#### Revocation and RESET_DEVICE interaction
`tenstorrent_revoke_tlb_dmabufs()` walks the export list, and under each buffer's `dma_resv` lock sets `revoked = true` and calls `dma_buf_invalidate_mappings` (`dma_buf_move_notify` pre-7.1) (memory.c:1281-1297, 34-37). But a **pin-only importer cannot be revoked**, so destructive resets (RESET_PCIE_LINK / CONFIG_WRITE / USER_RESET / ASIC_RESET / ASIC_DMC_RESET) are refused with `-EBUSY` while any export is live, via `tenstorrent_has_tlb_dmabuf_exports()` (memory.c:1299-1308; chardev.c:222-231, the check at chardev.c:228-229). Rationale: resetting under in-flight P2P DMA "can wedge the host hard enough to require out-of-band recovery" (ioctl.h:429-432).

> **Porting note:** dma-buf has no Windows equivalent for arbitrary cross-driver BAR sharing. Options: drop the ioctl (return STATUS_NOT_SUPPORTED, matching the pre-5.8 behavior), or design something on D3DKMT shared resources / NT handles if a concrete consumer exists. Whatever the choice, the port must preserve the *invariant* the feature encodes: a TLB window that any external agent may be DMAing into must not be freed, reprogrammed, or reset under it — the `-EBUSY`-on-reset and window-refcount semantics are the load-bearing part.

---

### 9. sg_helpers (sg_helpers.c, sg_helpers.h)

`sg_helpers.h:9-34` provides pre-5.8 backports of `dma_map_sgtable`/`dma_unmap_sgtable`/`for_each_sgtable_dma_sg` (needed only for old kernels; `dma_map_sgtable` returns 0 or `-ENOMEM`).

`alloc_chained_sgt_for_pages(table, pages, n_pages)` (sg_helpers.c:17-72) is a scalable replacement for `sg_alloc_table_from_pages`, built from single-page allocations chained together (no large allocations, "unlimited scaling"):

```c
#define SCL_PER_PAGE (PAGE_SIZE / sizeof(struct scatterlist) - 1)   // 145 on x86-64
#define MAX_PAGES_PER_SCL (UINT_MAX / PAGE_SIZE)
```
(sg_helpers.c:10,13)

- Each scatterlist page reserves its last slot for the chain entry (sg_helpers.c:6-9).
- Runs of physically contiguous pages are coalesced into single entries (`page_to_pfn(pages[-1]) + 1 != page_to_pfn(pages[0])` breaks the run), capped at `MAX_PAGES_PER_SCL` pages per entry because entry length is `unsigned int` (sg_helpers.c:49-56).
- Pages are allocated `GFP_KERNEL | __GFP_ZERO` because `sg_set_page` preserves chain/end bits in `page_link` (sg_helpers.c:29-30).
- `nents == orig_nents` = number of data entries (chain entries excluded); end mark on the last valid entry (sg_helpers.c:59-66).
- Returns false (after freeing partial work) on page-allocation failure (sg_helpers.c:33-34, 69-71).

`free_chained_sgt(table)` (sg_helpers.c:78-99) frees the scatterlist pages by walking the chain, *assuming* every page but the last holds exactly `SCL_PER_PAGE` entries (`BUG_ON(!sg_is_chain(...))` if violated); it tolerates a zero-initialized table and a partially built table with no end marker (header comment sg_helpers.h:37, sg_helpers.c:74-77).

`debug_print_sgtable` logs each DMA segment and flags discontiguities at `dev_dbg` level (sg_helpers.c:101-118).

> **Porting note:** On Windows the entire chained-sgt machinery disappears: an MDL already is the page-array representation, and there is no sg_table concept. What must survive is the *behavioral contract* it serves: (1) building the device mapping must not require a single huge contiguous kernel allocation for metadata (MDLs are fine — one allocation sized ~8 bytes/page, same as `vzalloc` of the pages array which Linux does anyway); (2) after mapping, the device-visible address range must be verified contiguous and of full length, else fail the ioctl.

---

### 10. Where physical addresses / IOVAs cross the user boundary

A Windows port must reproduce each of these exposures, since tt-umd consumes them:

| ioctl | field | value | cite |
|---|---|---|---|
| ALLOCATE_DMA_BUF | `out.physical_address` | `dma_alloc_coherent` handle (IOVA under IOMMU, else physical) | memory.c:490,494; ioctl.h:100 |
| ALLOCATE_DMA_BUF | `out.noc_address` | `noc_pcie_offset + iATU base` (NOC address space) | memory.c:196,478 |
| PIN_PAGES | `out.physical_address` | IOMMU: `sg_dma_address(sgl)` (IOVA); direct: `page_to_phys(pages[0])` (CPU physical) | memory.c:676,696 |
| PIN_PAGES | `out.noc_address` | `noc_pcie_offset + iATU base` | memory.c:679,699,707 |
| MAP_PEER_BAR | `out.dma_address` | `dma_map_resource` result (bus address / IOVA of peer BAR) | memory.c:855,863 |
| QUERY_MAPPINGS | `mapping_base` | synthetic mmap offsets only, never physical addresses | memory.c:355-388 |
| ALLOCATE_TLB | `mmap_offset_uc/wc` | synthetic offsets (`MMAP_OFFSET_TLB_* + bar_offset [+ BAR0_SIZE if BAR4]`) | memory.c:929-934 |

---

### Key constants table

| Name | Value | Source |
|---|---|---|
| `MAX_DMA_BUF_SIZE_LOG2` | 28 (→ 256 MiB max DMA buffer) | memory.h:10, memory.c:253 |
| `TENSTORRENT_MAX_DMA_BUFS` | 256 (buf_index is u8) | ioctl.h:41, ioctl.h:93 |
| `TENSTORRENT_MAX_INBOUND_TLBS` | 256 | ioctl.h:42 |
| `TENSTORRENT_MAX_OUTBOUND_IATU_REGIONS` | 16 | memory.h:65 |
| `BAR0_SIZE` | `1UL << 29` (512 MiB; BAR4 TLB mmap offsets start here) | memory.c:29 |
| `MMAP_OFFSET_RESOURCE0_UC` … `MMAP_OFFSET_TLB_WC` | `(0..7) << 36` | memory.c:258-265 |
| `MMAP_RESOURCE_SIZE` | `1 << 36` | memory.c:267 |
| `MMAP_OFFSET_DMA_BUF` | `(PAGE_SIZE-U8_MAX-1) << 32` = 0xF00_0000_0000 @ 4 KiB pages | memory.c:272 |
| `MMAP_SIZE_DMA_BUF` | `1 << 32` (4 GiB slot per buffer) | memory.c:274 |
| `TENSTORRENT_ALLOCATE_DMA_BUF_NOC_DMA` | 2 | ioctl.h:89 |
| `TENSTORRENT_PIN_PAGES_CONTIGUOUS/NOC_DMA/NOC_TOP_DOWN/READ_ONLY` | 1 / 2 / 4 / 8 | ioctl.h:169-172 |
| pin size cap (kernels ≤ 5.4 only) | `1 << 30` (1 GiB) | memory.c:537-538 |
| `TT_TLB_DMABUF_SG_CHUNK` | `SZ_1G` | memory.c:1036 |
| `SCL_PER_PAGE` | `PAGE_SIZE/sizeof(struct scatterlist) - 1` (145 on x86-64) | sg_helpers.c:10 |
| `MAX_PAGES_PER_SCL` | `UINT_MAX / PAGE_SIZE` | sg_helpers.c:13 |
| `DMABUF_HASHTABLE_BITS` | 4 | chardev_private.h:42 |
| Wormhole `noc_dma_limit` / `noc_pcie_offset` | `0xFFFE0000 - 1` / `0x800000000ULL` | wormhole.c:1059-1060 |
| Blackhole `noc_dma_limit` / `noc_pcie_offset` | `(1ULL << 58) - 1` / `4ULL << 58` | blackhole.c:817-818 |
| Wormhole / Blackhole `dma_address_bits` (coherent mask) | 32 / 58 | wormhole.c:1058, blackhole.c:816 |
| Streaming DMA mask | `DMA_BIT_MASK(dma_address_bits ?: 64)` | enumerate.c:330 |

### Open questions

1. **MAP_PEER_BAR keeps no reference on the peer device.** `fput(peer_file)` runs before the ioctl returns (memory.c:875) and nothing pins the peer `tenstorrent_device`; the `dma_map_resource` mapping into the local device's domain persists until the *local* fd closes (memory.c:1660-1665). If the peer device is unbound/removed while the mapping lives, the local device holds a bus address into a possibly reassigned BAR. It is unclear whether this is intentional (relying on both devices sharing driver lifetime) — a Windows port must decide whether to reference the peer device object.
2. **Cleanup ordering for NOC-DMA buffers**: `tenstorrent_memory_cleanup` calls `dma_free_coherent` *before* `teardown_outbound_iatu` (memory.c:1650-1651), so the outbound iATU window briefly targets freed memory. `ioctl_allocate_dma_buf`'s error path does it in the opposite (safe) order (memory.c:499-501). Probably harmless (nothing should be issuing NOC traffic at fd close), but a port should tear down the aperture first.
3. **`TENSTORRENT_PIN_PAGES_CONTIGUOUS` is dead**: accepted in `valid_flags` (memory.c:547) but never examined; contiguity is always enforced on the non-IOMMU path and IOVA-contiguity on the IOMMU path. Should the Windows ABI keep, require, or reject it?
4. **NOC_TOP_DOWN implies NOC_DMA**: `noc_dma` is true if *either* flag is set (memory.c:584). Is TOP_DOWN-without-NOC_DMA a supported combination or an accident of the mask? The port should document/normalize this.
5. **`MMAP_OFFSET_DMA_BUF` is PAGE_SIZE-dependent** (memory.c:272): the constant differs on non-4K-page kernels. The Windows port defines its own handle/offset scheme, but any ABI-compatibility shim for tt-umd must use the 4 KiB-page value (0xF00_0000_0000).
6. **DMA-buffer VMAs are untracked**: mappings created through `dma_mmap_coherent` are not added to `vma_list` and are not zapped by `tenstorrent_vma_zap()` (memory.c:1629-1632 vs 1677-1743). Presumably safe because the buffer is host RAM, not device BAR — but confirms that reset does not revoke user access to DMA buffers; the port can mirror this.
7. **IOVA contiguity is assumed, verified, and fatal if violated** (memory.c:654-674). On Linux the dma-iommu allocator happens to produce one contiguous IOVA per sgtable map. Whether an equivalent guarantee exists on any Windows remapping path is a core feasibility question for supporting non-contiguous user buffers at all.
8. **`ioctl_free_dma_buf` returns `-EINVAL` always** (memory.c:515-523). The ABI reserves the ioctl; the port must decide whether to implement real freeing (Windows can track section mappings, so the blocking problem Linux cites may be solvable) or preserve the stub for UMD compatibility.

---

## 06. TLB Window Management

### Scope

Files covered (all paths relative to the repo root of `tt-kmd` unless prefixed with `tt-umd/`):

| File | Lines | Coverage |
|---|---|---|
| `tlb.c` | 108 | fully read |
| `tlb.h` | 25 | fully read |
| `blackhole.c` | 840 | fully read; TLB-related parts documented here |
| `wormhole.c` | 1084 | fully read; TLB-related parts documented here |
| `chardev.c` | 966 | fully read; ioctl dispatch, fd-close cleanup, reset gating |
| `memory.c` | 1742 | TLB ioctls (893-999), TLB dma-buf export (1001-1326), mmap routing (1328-1741), offset constants (253-274) |
| `device.h` | 127 | device/class struct fields for TLBs |
| `chardev_private.h` | 82 | per-fd TLB state, VMA tracking |
| `ioctl.h` | 458 | uAPI structs for ALLOCATE/FREE/CONFIGURE/EXPORT TLB |
| `blackhole.h` | 28, `wormhole.h` 32 | per-arch device structs (kernel TLB fields) |
| `enumerate.c` | (relevant lines only) | per-device count init, remove/suspend hooks |
| `tt-umd/device/pcie/silicon_tlb_handle.cpp`, `tt-umd/device/tt_kmd_lib/tt_kmd_lib.c`, `tt-umd/device/pcie/pci_device.cpp` | (relevant lines only) | how userspace consumes this API |

Note: this baseline (`ttkmd-2.10.0-rc1-1-g8c32c2b`) has **no Grayskull support** — only `wormhole_class` and `blackhole_class` exist (there is no `grayskull.c` in the tree). The comment "GS/WH/BH" at memory.c:918 is historical.

---

### 1. TLB abstraction and data structures

A "TLB" here is an **inbound PCIe→NOC address translation window**: a fixed-size aperture in a PCI BAR that the chip forwards to a programmable (x,y,address) NOC destination. There is no `struct tenstorrent_tlb` object; a TLB is identified by a small integer **id** (a global index across all window kinds) and its state lives in three places:

- **Device-wide allocation bitmap + refcounts** (device.h:65-67):
  ```c
  DECLARE_BITMAP(tlbs, TENSTORRENT_MAX_INBOUND_TLBS);
  u32 tlb_counts[MAX_TLB_KINDS];	// Per-device TLB counts (may differ from dev_class defaults)
  refcount_t tlb_refcount[TENSTORRENT_MAX_INBOUND_TLBS];
  ```
  `TENSTORRENT_MAX_INBOUND_TLBS` is 256 (ioctl.h:42), `MAX_TLB_KINDS` is 4 (device.h:22).
- **Per-fd ownership bitmap** (chardev_private.h:72): `DECLARE_BITMAP(tlbs, TENSTORRENT_MAX_INBOUND_TLBS)` — which windows this fd owns.
- **Per-device-class geometry** (device.h:93-95): `u32 tlb_kinds; u32 tlb_counts[MAX_TLB_KINDS]; u64 tlb_sizes[MAX_TLB_KINDS];` plus two arch hooks (device.h:104-105): `configure_tlb()` and `describe_tlb()`.

`describe_tlb()` fills a `struct tlb_descriptor` (tlb.h:12-16):

```c
struct tlb_descriptor {
	int bar;
	unsigned long size;
	unsigned long bar_offset;
};
```

Per-device counts are initialized by copying the class defaults at PCI probe (enumerate.c:314: `memcpy(tt_dev->tlb_counts, device_class->tlb_counts, sizeof(tt_dev->tlb_counts));`) and may then be adjusted by arch init (Blackhole shrinks the 4G pool, see below). Nothing zeroes `tt_dev->tlbs` explicitly — the device struct is `kzalloc`'d, so the bitmap starts empty.

Window ids are assigned **pool-major**: id space is the concatenation of the kinds in table order, using the *per-device* counts as pool widths (tlb.c:20-30). E.g. Wormhole: ids 0-155 are 1M windows, 156-165 are 2M, 166-185 are 16M.

> **Porting note:** The id-to-window mapping is uAPI-visible (ids are returned to userspace and used in CONFIGURE_TLB/FREE_TLB/EXPORT), so a Windows port must reproduce exactly this pool-major numbering, per-arch counts, and the kernel-reserved top window (below). The bitmap + `refcount_t` pair can become an `RTL_BITMAP` + per-window `LONG` refcount guarded by a spinlock; the Linux code relies on atomic bitops instead of a lock (see §3).

---

### 2. Per-device TLB pools (exact tables)

#### Wormhole (`wormhole_class`, wormhole.c:1055-1084)

```c
.tlb_kinds = NUM_TLB_KINDS,   // 3
.tlb_counts = { TLB_1M_WINDOW_COUNT, TLB_2M_WINDOW_COUNT, TLB_16M_WINDOW_COUNT },
.tlb_sizes = { TLB_1M_WINDOW_SIZE, TLB_2M_WINDOW_SIZE, TLB_16M_WINDOW_SIZE },
```

Constants (wormhole.c:20-37):

| Kind | Count | Size | Shift | BAR | BAR offset of pool base | id range |
|---|---|---|---|---|---|---|
| 1M | 156 | 0x100000 | 20 | 0 | 0x0 | 0-155 |
| 2M | 10 | 0x200000 | 21 | 0 | 0x09C00000 (`TLB_2M_WINDOW_BASE`, wormhole.c:28) | 156-165 |
| 16M | 20 | 0x1000000 | 24 | 0 | 0x0B000000 (`TLB_16M_WINDOW_BASE`, wormhole.c:33) | 166-185 |

`TLB_WINDOW_COUNT` = 186 (wormhole.c:36). All windows live in BAR0; data apertures span BAR0 offsets 0x0-0x1F000000. `wormhole_describe_tlb()` (wormhole.c:900-915) computes `bar = 0`, `bar_offset = base[kind] + size[kind] * (tlb - first_index[kind])` from the static tables at wormhole.c:803-807.

**Kernel-reserved window:** `KERNEL_TLB_INDEX = TLB_WINDOW_COUNT - 1` = 185, the last 16M window (wormhole.c:87). Its bit is set at init (`set_bit(KERNEL_TLB_INDEX, tt_dev->tlbs)`, wormhole.c:702) so the allocator can never hand it out. Its data aperture is BAR0 offset 0x1E000000, but the kernel accesses it through **BAR4 offset 0** (`KERNEL_TLB_START (0x1E000000 - BAR4_SOC_TARGET_ADDRESS)` = 0, wormhole.c:88; used at wormhole.c:928) — BAR4 is iATU-remapped to SoC addresses 0x1E000000-0x20000000 (`BAR4_SOC_TARGET_ADDRESS 0x1E000000`, wormhole.c:74-77), which coincides with the last 16M window's aperture.

#### Blackhole (`blackhole_class`, blackhole.c:813-840)

```c
.tlb_kinds = 2,
.tlb_counts = { TLB_2M_WINDOW_COUNT, TLB_4G_WINDOW_COUNT },   // { 202, 8 }
.tlb_sizes = { TLB_2M_WINDOW_SIZE, TLB_4G_WINDOW_SIZE },       // { 1<<21, 1UL<<32 }
```

Constants (blackhole.c:20-42):

| Kind | Count | Size | Shift | BAR | BAR offset | id range |
|---|---|---|---|---|---|---|
| 2M | 202 | 0x200000 | 21 | 0 | `tlb * 2M` (0x0-0x19400000) | 0-201 |
| 4G | 8 (nominal) | 0x100000000 | 32 | 4 | `(tlb - 202) * 4G` | 202-209 |

`TLB_TOTAL_WINDOW_COUNT` = 210 (blackhole.c:31). `blackhole_describe_tlb()` (blackhole.c:738-753):

```c
desc->bar = is_2M ? 0 : 4;
desc->size = is_2M ? TLB_2M_WINDOW_SIZE : TLB_4G_WINDOW_SIZE;
desc->bar_offset = is_2M ? tlb * TLB_2M_WINDOW_SIZE
                         : (tlb - TLB_2M_WINDOW_COUNT) * TLB_4G_WINDOW_SIZE;
```

**Per-device 4G count clamp:** `blackhole_init()` shrinks the 4G pool to what BAR4 actually exposes (blackhole.c:576-580):

```c
resource_size_t bar4_len = pci_resource_len(tt_dev->pdev, 4);
// Limit 4G window count to what's available; partial windows not supported.
tt_dev->tlb_counts[1] = bar4_len / TLB_4G_WINDOW_SIZE;
```

so on hosts without large-BAR support the allocatable 4G ids are 202 .. 202 + `bar4_len/4G` - 1 (possibly zero of them). `describe_tlb` itself still accepts any id < 210; the allocator and `map_tlb_window()`'s search loop are bounded by the per-device counts.

**Kernel-reserved window:** `KERNEL_TLB_INDEX = TLB_2M_WINDOW_COUNT - 1` = 201, the last 2M window (blackhole.c:40), claimed at init (`set_bit(KERNEL_TLB_INDEX, tt_dev->tlbs)`, blackhole.c:609). The kernel maps it separately via `pci_iomap_range(pdev, 0, KERNEL_TLB_START, KERNEL_TLB_LEN)` where `KERNEL_TLB_START = 201 * 2M` (blackhole.c:41-42, 588).

> **Porting note:** On Windows, BAR4 length must be read from the translated resources at `EvtDevicePrepareHardware`; the 4G pool clamp (`bar4_len / 4G`) must be replicated, including the "0 usable 4G windows" case when BAR4 is small/absent. The comment at blackhole.c:25 warns explicitly: "Not all are guaranteed to be exposed in BAR4".

---

### 3. ALLOCATE_TLB — size validation and pool selection

uAPI (ioctl.h:270-286): input is only `__u64 size` (+reserved); output is `__u32 id`, `__u64 mmap_offset_uc`, `__u64 mmap_offset_wc`.

#### Core allocator (`tenstorrent_device_allocate_tlb`, tlb.c:9-54)

- Returns `-EINVAL` if the class has no TLB kinds (tlb.c:17-18).
- **Exact-size pool selection** (tlb.c:20-30): walks kinds in order, accumulating `offset += tlb_count` (per-device counts), and selects the kind where `size == tlb_size`. **No rounding** — the requested size must exactly equal one of the pool sizes (1M/2M/16M on WH; 2M/4G on BH) or the result is `-EINVAL` (tlb.c:32-33).
- **Lock-free claim loop** (tlb.c:36-50): `find_next_zero_bit(tt_dev->tlbs, offset + n, offset)`; if none free in the pool, `-ENOMEM`; otherwise `test_and_set_bit(id, ...)` and on success `refcount_set(&tt_dev->tlb_refcount[id], 1)` and return id. On a lost race it retries with `cond_resched()`, returning `-ERESTARTSYS` if a signal is pending (tlb.c:47-49). No mutex is held; correctness relies on atomic bitops.

#### ioctl wrapper (`ioctl_allocate_tlb`, memory.c:893-944)

Validation/flow, in order:
1. `-EINVAL` if the class has no `describe_tlb` hook (memory.c:902-903).
2. `-EFAULT` on `copy_from_user` (memory.c:905-906).
3. Allocate (errors propagated: `-EINVAL` / `-ENOMEM` / `-ERESTARTSYS`).
4. `describe_tlb(id)` failure → free the window, `-EINVAL` (memory.c:913-916).
5. `bar` must be 0 or 4 → else free, `-EINVAL` (memory.c:918-922).
6. Compute mmap offsets (see §6) and `copy_to_user`; on `-EFAULT` the window is freed again (memory.c:936-939).
7. Only after the copy succeeds: `set_bit(id, priv->tlbs)` records fd ownership (memory.c:941).

Locks: none beyond the atomic bitops (the per-fd `set_bit` is atomic; `priv->mutex` is *not* taken here). Cleanup on every failure path returns the window to the device pool.

---

### 4. CONFIGURE_TLB — register programming

uAPI config struct (ioctl.h:300-313):

```c
struct tenstorrent_noc_tlb_config {
	__u64 addr;
	__u16 x_end;  __u16 y_end;  __u16 x_start;  __u16 y_start;
	__u8 noc;  __u8 mcast;  __u8 ordering;  __u8 linked;  __u8 static_vc;
	__u8 reserved0[3];  __u32 reserved1[2];
};
```

#### ioctl path (`ioctl_configure_tlb`, memory.c:984-999)

- `-EFAULT` on copy-in; `-EINVAL` if `id >= TENSTORRENT_MAX_INBOUND_TLBS` (256); `-EPERM` if the fd does not own the window (`!test_bit(in.id, priv->tlbs)`, memory.c:995-996). No lock is held around the ownership test or the register write.
- Dispatches through `tenstorrent_device_configure_tlb()` (tlb.c:101-108), `-EINVAL` if the class lacks the hook.
- **No validation of coordinates or field ranges**: `x_end`/`y_end`/etc. are silently truncated to the hardware bit-field widths; `reserved0`/`reserved1` are not checked.

#### Wormhole programming (wormhole.c:809-891)

64-bit register per window, stride **8 bytes**: `offset = tlb * 2 * sizeof(u32)` (wormhole.c:886), written as two 32-bit `iowrite32`s into `bar4_mapping + TLB_REGS_START` where `TLB_REGS_START = 0x1FC00000 - BAR4_SOC_TARGET_ADDRESS` = BAR4 offset 0x01C00000 (wormhole.c:80).

Register layout (`construct_tlb_config`, wormhole.c:840-871): the low `36 - shift` bits hold `addr >> shift`; above them sits the packed non-address block (wormhole.c:809-823):

```c
u64 x_end: 6;  u64 y_end: 6;  u64 x_start: 6;  u64 y_start: 6;
u64 noc_sel : 1;  u64 mcast: 1;  u64 ordering: 2;  u64 linked: 1;
```

```c
*regs |= (u64)config->addr >> TLB_SHIFTS[kind];
*regs |= (u64)non_address_bits.reg << (WH_NOC_BITS - TLB_SHIFTS[kind]);   // WH_NOC_BITS = 36
```

Validation: `-EINVAL` if `tlb` out of [0,186) (wormhole.c:880-881), if `addr` is not aligned to the window size (wormhole.c:859-860), or if `addr >= 1<<36` (wormhole.c:862-864). Note **`config->static_vc` is ignored on Wormhole** (no field in the WH register struct) and `noc` is truncated to 1 bit (`noc_sel : 1`).

#### Blackhole programming (blackhole.c:112-226, 724-736)

96-bit register per window, stride **12 bytes** (`TLB_REG_SIZE 12`, same for 2M and 4G; blackhole.c:30), written as three `iowrite32`s at `bh->tlb_regs + tlb * 12`. `bh->tlb_regs` is a dedicated iomap of BAR0 [0x1FC00000, +0x1000) (`TLB_REGS_START`/`TLB_REGS_LEN`, blackhole.c:33-34, mapped at blackhole.c:587). 210 windows x 12 B + 32 strided regs x 4 B = 2648 B, fits the 4 KiB mapping.

2M register bit layout (blackhole.c:112-137; packed struct, `y_start` straddles the mid/high words):

```c
u64 address : 43;  u64 x_end : 6;  u64 y_end : 6;  u64 x_start : 6;  u64 y_start : 6;
u64 noc : 2;  u64 multicast : 1;  u64 ordering : 2;  u64 linked : 1;
u64 use_static_vc : 1;  u64 stream_header : 1;  u64 static_vc : 3;  u64 reserved : 18;
```

4G register (blackhole.c:139-163) is identical except `address : 32` and `reserved : 29`.

Field mapping from the uAPI struct (blackhole.c:175-184 and 210-219): `addr >> shift` → `address`; `x_end/y_end/x_start/y_start/noc/mcast/ordering/linked` map 1:1; **`config->static_vc` maps to the single-bit `use_static_vc`**, while the 3-bit `static_vc` and `stream_header` fields are always written 0. Validation: `-EINVAL` if `addr` not 2M-aligned (blackhole.c:172-173) / 4G-aligned (blackhole.c:207-208); no upper-bound check is needed (43+21 = 32+32 = 64 bits cover the full address space). Dispatch bounds: id in [0,202) → 2M path, [202,210) → 4G path, else `-EINVAL` (blackhole.c:724-736).

**Strided-TLB scrub:** the first 32 2M windows have an extra 4-byte "strided" register (non-rectangular multicast) at `tlb_regs + 0x9D8 + tlb*4` (`TLB_STRIDED_COUNT 32`, `TLB_STRIDED_REGS_OFFSET = 210*12 = 0x9D8`, blackhole.c:36-38). CONFIGURE_TLB cannot set it but **always zeroes it** for those windows to clear configuration set by other means (blackhole.c:190-195).

#### Kernel TLB vs. user TLBs

Each arch reserves exactly one window for kernel use (WH id 185, BH id 201, see §2). Kernel NOC access reprograms that window on every access under a per-device `kernel_tlb_mutex` (wormhole.h:12 / blackhole.h:13) with `ordering = 1 // strict` and unicast target (`wh_configure_kernel_tlb`, wormhole.c:917-929; `bh_configure_kernel_tlb`, blackhole.c:228-241), then does a single `ioread32`/`iowrite32` (`noc_read32`/`noc_write32`, wormhole.c:931-954, blackhole.c:243-268). Consumers: telemetry probe, ARC message queue, CSM read/write, reset save/restore of DBI MPS, and the per-fd NOC-cleanup write at release (chardev.c:865-875). There is no other kernel/user distinction — user windows are programmed by the same `configure_tlb` hooks.

> **Porting note (registers):** The bit layouts above are the load-bearing hardware contract. In a KMDF driver, replicate them with explicit shifts/masks rather than C bitfields (MSVC bitfield layout of the 96-bit packed struct is not guaranteed to match GCC's). Writes must be 32-bit MMIO stores in low-to-high order (`WRITE_REGISTER_ULONG`); the BH 12-byte stride means odd indices are only 4-byte aligned, so 64-bit stores are unsafe (tt-umd documents the same constraint for aarch64, tt-umd/device/pcie/pci_device.cpp:857-871).

> **Porting note (kernel window):** The Windows driver needs the same reserved-window + mutex construct for its own ARC/telemetry traffic, and must mark the reserved id allocated before any user can reach the allocator.

---

### 5. FREE_TLB

#### ioctl (`ioctl_free_tlb`, memory.c:946-982)

1. `-EFAULT` on copy-in; `-EINVAL` if `id >= 256` (memory.c:955-956; the `in.id < 0` half is dead code, `id` is `__u32`).
2. Takes `priv->mutex` (memory.c:958). `-EPERM` if the fd does not own the window (memory.c:960-963).
3. Under `priv->vma_lock`, scans `priv->vma_list`; if any live VMA is a `TT_VMA_TLB` for this id → `-EBUSY` (memory.c:965-974). **A window cannot be freed while mmapped.**
4. `clear_bit(in.id, priv->tlbs)` then `tenstorrent_device_free_tlb()` (memory.c:976-977).

#### Core free (`tenstorrent_device_free_tlb`, tlb.c:56-81)

- `-EINVAL` if no kinds or `id >= total_tlbs` (sum of per-device counts, tlb.c:62-69); `-EPERM` if the device bit is not set (tlb.c:71-72).
- Drops the owning reference: `if (refcount_dec_and_test(&tt_dev->tlb_refcount[id])) clear_bit(id, tt_dev->tlbs);` (tlb.c:77-78) — if a dma-buf export still holds a reference, the bit stays set and the window returns to the pool only when the last export is released (comment tlb.c:74-76; `tenstorrent_tlb_export_get/put`, tlb.c:87-99).

The hardware register is **not** cleared or reprogrammed on free; the next owner inherits whatever configuration was last written until it calls CONFIGURE_TLB.

---

### 6. mmap routing and `mmap_offset_uc`/`mmap_offset_wc` encoding

#### Offset namespace (memory.c:255-274)

The char device multiplexes all mappable entities through the mmap offset. The fixed 64 GiB regions (each `1<<36` bytes, memory.c:267):

```c
#define MMAP_OFFSET_RESOURCE0_UC	(U64_C(0) << 36)
#define MMAP_OFFSET_RESOURCE0_WC	(U64_C(1) << 36)
#define MMAP_OFFSET_RESOURCE1_UC	(U64_C(2) << 36)
#define MMAP_OFFSET_RESOURCE1_WC	(U64_C(3) << 36)
#define MMAP_OFFSET_RESOURCE2_UC	(U64_C(4) << 36)
#define MMAP_OFFSET_RESOURCE2_WC	(U64_C(5) << 36)
#define MMAP_OFFSET_TLB_UC		(U64_C(6) << 36)   // 0x60_0000_0000
#define MMAP_OFFSET_TLB_WC		(U64_C(7) << 36)   // 0x70_0000_0000
```

(RESOURCE0/1/2 = BAR 0/2/4; DMA buffers live higher, at `MMAP_OFFSET_DMA_BUF`, memory.c:272.) QUERY_MAPPINGS reports only the six BAR regions (memory.c:331-409); TLB mmap offsets are communicated **only** through ALLOCATE_TLB's output.

#### Encoding at allocation (memory.c:924-934)

```c
encoded_id = tlb_desc.bar_offset;
if (tlb_desc.bar == 4)
	encoded_id += BAR0_SIZE;           // BAR0_SIZE = (1UL << 29) = 512 MiB, memory.c:29
out.mmap_offset_uc = MMAP_OFFSET_TLB_UC + encoded_id;
out.mmap_offset_wc = MMAP_OFFSET_TLB_WC + encoded_id;
```

I.e. the offset within the TLB region equals the window's BAR0 byte offset, except Blackhole's BAR4 4G windows are re-based at 512 MiB ("the size of BAR0", comment memory.c:926-928). Maximum encoded value 0x8_2000_0000 fits well inside the 64 GiB region.

#### Decode at mmap (`tenstorrent_mmap` → `map_tlb_window`, memory.c:1585-1636, 1494-1583)

`tenstorrent_mmap()` routes offsets in `[6<<36, 7<<36)` to `map_tlb_window(..., BAR_MAPPING_UC)` with `pgprot_device()` and `[7<<36, 8<<36)` to the WC variant with `pgprot_writecombine()` (memory.c:1620-1626); `vma_target_range()` also rebases `vm_pgoff` to be region-relative (memory.c:1330-1342).

`map_tlb_window()`:
1. `-EINVAL` if the class lacks `describe_tlb`/kinds (memory.c:1510-1514).
2. `bar4 = offset >= BAR0_SIZE`; if so, `offset -= BAR0_SIZE` (memory.c:1503, 1519-1520).
3. Linear search over all per-device windows for `tlb_desc.bar_offset == offset && (tlb_desc.bar == 4) == bar4` (memory.c:1523-1531); `-EINVAL` if none. **The mapping must start exactly at a window base**; mapping at an interior offset is rejected.
4. `-EINVAL` if the VMA is larger than the window (memory.c:1536-1537); partial-length mappings from the base are allowed.
5. Under `priv->mutex`: `-EPERM` if the fd doesn't own the window (memory.c:1541-1544); `-ENXIO` if `bar_offset + size` exceeds the BAR length (memory.c:1547-1550).
6. `io_remap_pfn_range()` of `pci_resource_start(bar) + bar_offset` (memory.c:1558-1565; `-EAGAIN` on failure), installs `tlb_vm_ops` and records a `struct tenstorrent_mmap_vma { type = TT_VMA_TLB, tlb.id, cache_mode }` on `priv->vma_list` under `priv->vma_lock` (memory.c:1567-1578; struct at chardev_private.h:21-40).

`tlb_vm_ops` forbids VMA splitting (`may_split` returns `-EINVAL`, memory.c:1478-1492) and its open/close callbacks keep the tracking list correct across `fork()`/`munmap()` (memory.c:1370-1434; on fork, an allocation failure zaps the child's copy rather than leaving it untracked, memory.c:1387-1393).

The mmap entry point itself (chardev.c:708-736) refuses with `-ENODEV` if the reset rwsem cannot be read-acquired (trylock, to avoid an ABBA deadlock with `tenstorrent_vma_zap`), if the device is detached, or if the fd predates the current reset generation.

> **Porting note:** Windows has no mmap-offset namespace. The natural KMDF mapping is a per-window map request (e.g. an IOCTL that maps the window into the caller with `MmMapLockedPagesSpecifyCache`, or a section object per window) carrying a UC-vs-WC cache-type flag instead of the two magic offsets; `mmap_offset_uc/wc` then become opaque handles the Windows UMD layer feeds back to a MAP ioctl. What must be preserved semantically: (a) mapping requires ownership; (b) mappings start at the window base and never exceed the window; (c) FREE fails with a busy error while a mapping exists; (d) the driver can revoke all user mappings on reset (Windows: `MmUnmapLockedPages` on tracked mappings, or fail-fast the owning process — VM zap has no direct equivalent, revocation strategy needs a design decision).

#### How userspace uses this (tt-umd)

`tt_kmd_lib.c` calls `TENSTORRENT_IOCTL_ALLOCATE_TLB`, picks `out.mmap_offset_uc` or `out.mmap_offset_wc` by cache mode, and mmaps the device fd at that offset (tt-umd/device/tt_kmd_lib/tt_kmd_lib.c:417-426). Configuration goes either through the CONFIGURE_TLB ioctl (tt-umd/device/tt_kmd_lib/tt_kmd_lib.c:492, 511) or — in `SiliconTlbHandle::configure` — by writing the TLB config registers **directly from user space through a BAR0 mapping** (tt-umd/device/pcie/silicon_tlb_handle.cpp:43-55, tt-umd/device/pcie/pci_device.cpp:845-895). The direct path works on Linux because QUERY_MAPPINGS+mmap exposes all of BAR0 (including the register file at 0x1FC00000) with no ownership checks.

---

### 7. EXPORT_TLB_DMABUF (window pinning across free/close)

`ioctl_export_tlb_dmabuf` (memory.c:1146-1279) wraps a window's BAR aperture sub-range in a dma-buf for peer-to-peer DMA (e.g. RDMA NICs; rationale in ioctl.h:413-448). Validation: `-EFAULT` on copy; `-EINVAL` for `argsz != sizeof`, nonzero `flags`, `tlb_id >= 256`, non-page-aligned `offset`/`size`, missing `describe_tlb`, out-of-window range, or range exceeding the BAR; `-EPERM` if the fd doesn't own the window (memory.c:1161-1211, under `priv->mutex`).

Lifetime rules a port must honor:
- The export takes a device kref **and** a TLB refcount (`tenstorrent_tlb_export_get`, memory.c:1233-1234), so FREE_TLB or fd close does not return the window to the pool while the export lives; only `tt_tlb_dmabuf_release` drops it (memory.c:1118-1135, tlb.c:95-99). This prevents reallocation+reconfiguration from redirecting a live importer's DMA (ioctl.h:434-438).
- Destructive resets are refused with `-EBUSY` while any export is live (chardev.c:222-233, `tenstorrent_has_tlb_dmabuf_exports`, memory.c:1299-1308).
- Exports are revoked (mappings invalidated, `revoked` flag set) on device removal and suspend (`tenstorrent_revoke_tlb_dmabufs`, memory.c:1281-1297; called at enumerate.c:459 and enumerate.c:504).
- Importers must support P2P (`attach` rejects non-peer2peer with `-EOPNOTSUPP`, memory.c:1023-1031); the BAR range is mapped into the importer's IOMMU domain with `dma_map_resource` and described in <=1 GiB scatterlist chunks (memory.c:1036-1090).

> **Porting note:** dma-buf has no Windows equivalent; the closest concepts are NTB/DirectGMA-style vendor APIs. For a first port this ioctl can return `STATUS_NOT_SUPPORTED` (Linux itself returns `-EOPNOTSUPP` on kernels < 5.8, memory.c:1310-1315) — but then the `-EBUSY`-on-reset guard and the export refcount machinery drop out too. The refcount design in tlb.c is still worth keeping if any future Windows P2P mechanism arrives.

---

### 8. Lifecycle: fd close, reset, removal

**fd close** (`tt_cdev_release`, chardev.c:922-958, run under `reset_rwsem` shared): after the NOC-cleanup write and memory cleanup, `tt_cdev_release_tlbs()` walks the per-fd bitmap and calls `tenstorrent_device_free_tlb()` for every owned window (chardev.c:887-892, called at chardev.c:934). By this point all of the process's VMAs are gone (munmap at process exit ran `tenstorrent_vma_close`), so there is no `-EBUSY` interaction; windows with live dma-buf exports stay allocated per §7. Release runs even for fds invalidated by a reset — the bitmap is device state, not generation-scoped.

**Reset** (`ioctl_reset_device`, chardev.c:200-310, under `reset_rwsem` exclusive): all destructive flavors call `tenstorrent_vma_zap(tt_dev)` (chardev.c:248-267), which unmaps the PTEs of **every** tracked BAR and TLB VMA across all open fds (memory.c:1677-1741; VFIO-derived lock-ordering dance between `mmap_lock` and `vma_lock`, comment memory.c:1683-1689). Because `tlb_vm_ops` has no `.fault` handler, any subsequent user access to a zapped mapping takes SIGBUS. The reset also bumps `reset_gen`, so every other pre-reset fd gets `-ENODEV` from all subsequent ioctls/mmaps (chardev.c:604-624, 726-729). The driver does **not** clear the device TLB allocation bitmap, the per-fd ownership bitmaps, or the hardware window registers on reset — stale owners keep their ids reserved until they close, and re-init (`init_hardware`) does not touch user TLB registers. During the `needs_hw_init` window only GET_DEVICE_INFO / GET_DRIVER_INFO / RESET_DEVICE are allowed; ALLOCATE/CONFIGURE/FREE_TLB return `-ENODEV` (chardev.c:615-624).

**Device removal** (`tenstorrent_pci_remove`, enumerate.c:404-481): sets `detached` under `chardev_mutex`, drains ioctls via `down_write(&reset_rwsem)`, zaps all VMAs, unmaps BARs (`cleanup_device` → `pci_iounmap` of `tlb_regs`/`kernel_tlb` etc., blackhole.c:710-722, wormhole.c:793-801), then revokes TLB dma-buf exports (enumerate.c:444-459; ordering rationale in the comment). Open fds survive with `-ENODEV` semantics; their TLB bookkeeping is torn down at their eventual close.

**ioctl dispatch** for the four TLB calls is at chardev.c:670-680 (ALLOCATE/FREE/CONFIGURE) and chardev.c:690-692 (EXPORT), all under `reset_rwsem` shared with the detached/reset-gen/needs_hw_init gates above.

> **Porting note:** The per-fd bitmap + release-time sweep maps cleanly onto a KMDF file-object context cleaned up in `EvtFileCleanup`/`EvtFileClose`. The reset-generation gating (fd permanently invalid after reset) and the "zap all user mappings on reset" behavior are load-bearing for safety: a Windows port must be able to cut off user MMIO access before resetting (unmap tracked user mappings, or refuse reset while mappings exist).

---

### Key constants table

| Name | Value | Source |
|---|---|---|
| `TENSTORRENT_MAX_INBOUND_TLBS` | 256 | ioctl.h:42 |
| `MAX_TLB_KINDS` | 4 | device.h:22 |
| WH `TLB_1M_WINDOW_COUNT` / shift | 156 / 20 | wormhole.c:20-21 |
| WH `TLB_2M_WINDOW_COUNT` / shift / base | 10 / 21 / 0x09C00000 | wormhole.c:25-28 |
| WH `TLB_16M_WINDOW_COUNT` / shift / base | 20 / 24 / 0x0B000000 | wormhole.c:30-33 |
| WH `TLB_WINDOW_COUNT` | 186 | wormhole.c:36 |
| WH `WH_NOC_BITS` (addr width) | 36 | wormhole.c:37 |
| WH TLB register stride | 8 bytes (`tlb * 2 * sizeof(u32)`) | wormhole.c:886 |
| WH TLB regs location | BAR4 + 0x01C00000 (SoC 0x1FC00000) | wormhole.c:80 |
| WH `BAR4_SOC_TARGET_ADDRESS` | 0x1E000000 | wormhole.c:76 |
| WH `KERNEL_TLB_INDEX` / kernel aperture | 185 / BAR4 + 0x0 | wormhole.c:87-88 |
| BH `TLB_2M_WINDOW_COUNT` / shift | 202 / 21 | blackhole.c:20-21 |
| BH `TLB_4G_WINDOW_COUNT` / shift | 8 (clamped by BAR4 len) / 32 | blackhole.c:25-26, 580 |
| BH `TLB_REG_SIZE` | 12 bytes | blackhole.c:30 |
| BH `TLB_TOTAL_WINDOW_COUNT` | 210 | blackhole.c:31 |
| BH `TLB_REGS_START` / `TLB_REGS_LEN` | BAR0 0x1FC00000 / 0x1000 | blackhole.c:33-34 |
| BH `TLB_STRIDED_COUNT` / reg size / offset | 32 / 4 / 0x9D8 (=210*12) | blackhole.c:36-38 |
| BH `KERNEL_TLB_INDEX` / kernel aperture | 201 / BAR0 + 201*2M | blackhole.c:40-42 |
| BH 2M reg `address` width | 43 bits | blackhole.c:121 |
| BH 4G reg `address` width | 32 bits | blackhole.c:147 |
| `MMAP_OFFSET_TLB_UC` / `_WC` | 6<<36 (0x60_0000_0000) / 7<<36 (0x70_0000_0000) | memory.c:264-265 |
| `MMAP_RESOURCE_SIZE` (region span) | 1<<36 | memory.c:267 |
| `BAR0_SIZE` (BAR4 mmap-encoding rebase) | 1<<29 (512 MiB) | memory.c:29 |
| Kernel-window ordering value | 1 ("strict") | wormhole.c:924, blackhole.c:236 |
| `TT_TLB_DMABUF_SG_CHUNK` | 1 GiB | memory.c:1036 |
| ioctl numbers (magic 0xFA) | ALLOCATE=11, FREE=12, CONFIGURE=13, EXPORT_DMABUF=16 | ioctl.h:12, 25-30 |

Error-code summary: ALLOCATE_TLB → `-EINVAL` (bad size/no pools), `-ENOMEM` (pool exhausted), `-ERESTARTSYS`, `-EFAULT`; CONFIGURE_TLB → `-EINVAL` (id range, arch reject: misalignment, WH addr >= 2^36), `-EPERM` (not owner), `-EFAULT`; FREE_TLB → `-EINVAL`, `-EPERM`, `-EBUSY` (live mapping), `-EFAULT`; mmap → `-EINVAL`/`-EPERM`/`-ENXIO`/`-EAGAIN`/`-ENODEV`.

### Open questions

1. **Hardware TLB register state after ASIC reset.** The driver never clears or reprograms user TLB config registers on reset or free; whether the hardware resets them to a benign default after `ASIC_RESET`/`ASIC_DMC_RESET` is not observable in this code. A Windows port should not assume registers are cleared (and per blackhole.c:190-195, out-of-band strided configuration can persist and is only scrubbed by the next CONFIGURE_TLB on windows 0-31).
2. **Wormhole ignores `static_vc`.** `tenstorrent_noc_tlb_config.static_vc` is silently dropped on WH (no field in `noc_tlb_non_address_bits`, wormhole.c:809-823) but drives `use_static_vc` on BH (blackhole.c:184). It is unclear whether WH hardware has an equivalent bit that the driver chooses not to expose; the port should replicate the drop-silently behavior for compatibility.
3. **Semantics of `ordering` values.** The kernel only documents `1 = strict` (comments wormhole.c:924, blackhole.c:236); the meaning of 0/2/3 is defined by NOC hardware and by tt-umd's `tlb_data` enums, not by tt-kmd. The port should treat the field as an opaque 2-bit passthrough.
4. **BAR mappings bypass TLB ownership.** QUERY_MAPPINGS + mmap of RESOURCE0/2 (BAR0/BAR4) exposes every TLB data window *and* the TLB config register file to any fd without ownership checks (memory.c:1596-1618), and tt-umd's `SiliconTlbHandle::configure` relies on this to poke registers from user space. A Windows port must decide whether to reproduce this open-BAR model (required for unmodified UMD behavior) or force all configuration through the CONFIGURE_TLB path.
5. **`blackhole_init` BAR2 error handling.** The failure check at blackhole.c:592 tests `tlb_regs`/`kernel_tlb`/`noc2axi_cfg` but not `bar2_mapping`; a failed BAR2 iomap proceeds and later iATU writes would dereference NULL (blackhole.c:104-110). Presumably a latent bug rather than intent; a port should treat BAR2 mapping failure as fatal.
6. **Exact-size ALLOCATE contract.** `tenstorrent_device_allocate_tlb` requires `size` to exactly match a pool size (tlb.c:24) — no rounding up. This appears intentional (UMD always passes exact sizes) but is worth confirming as a frozen uAPI behavior before the Windows ioctl surface copies it.
7. **`tlb_counts` divergence between allocator and `describe_tlb` on BH.** With a small BAR4, ids 202+`tlb_counts[1]`..209 are unallocatable yet `blackhole_describe_tlb` still describes them (blackhole.c:743 uses the compile-time total). Harmless on Linux (nothing reaches describe with an unallocated id except the mmap search loop, which is bounded by per-device counts, memory.c:1516-1523), but a port restructuring this code should keep the allocator bound authoritative.

---

## 07. Blackhole Hardware Personality

### Scope

Files covered (paths relative to tt-kmd repo root, baseline `ttkmd-2.10.0-rc1-1-g8c32c2b`):

| File | Lines | Role |
|---|---|---|
| `blackhole.c` | 840 | Complete Blackhole (BH) hardware personality: BAR mapping, TLB windows, NOC/CSM access, ARC messaging, telemetry, iATU, reset |
| `blackhole.h` | 28 | `struct blackhole_device` instance state |
| `device.h` | 127 | `struct tenstorrent_device` (generic per-device state) and `struct tenstorrent_device_class` ops table |

Cross-referenced (read to verify constants used by blackhole.c; covered in depth by other sections): `msgqueue.h` (28 lines), `msgqueue.c` (133 lines), `telemetry.h` (82 lines), `telemetry.c` (card-type decode only), `pcie.h`/`pcie.c` (reset helpers), `module.c` (module params), `enumerate.c`/`enumerate.h` (lifecycle call sites, PCI IDs), `ioctl.h` (ABI structs), `memory.h`, `tlb.h`.

---

### 1. Device identity and instance state

Blackhole is matched by PCI vendor/device `0x1E52:0xB140`:

- `#define PCI_DEVICE_ID_BLACKHOLE 0xB140` — enumerate.h:18
- PCI table entry binds it to `blackhole_class` — module.c:69-70

Per-device state (blackhole.h:10-23):

```c
struct blackhole_device {
	struct tenstorrent_device tt;
	struct mutex kernel_tlb_mutex;	// Guards access to kernel_tlb
	u8 __iomem *tlb_regs;   // All TLB registers
	u8 __iomem *kernel_tlb; // Topmost 2M window, reserved for kernel
	u8 __iomem *noc2axi_cfg;
	u8 __iomem *bar2_mapping;
	u8 saved_mps;
	bool pcie_perf_group_registered;
	bool telemetry_group_registered;
};
```

The container-of macro has a quirk: the parameter is named `ttdev` but the body references `tt_dev` (blackhole.h:25-26):

```c
#define tt_dev_to_bh_dev(ttdev) \
	container_of((tt_dev), struct blackhole_device, tt)
```

It only compiles because every call site's argument variable is literally named `tt_dev`.

> **Porting note:** In the Windows port, make the equivalent accessor a proper inline function (`CONTAINING_RECORD`), fixing this accidental capture-by-name.

`instance_size = sizeof(struct blackhole_device)` (blackhole.c:815) — the common layer allocates one blob containing `tenstorrent_device` as its first member; the class-specific tail follows it.

---

### 2. BAR layout

Blackhole exposes three BARs the driver uses: **BAR0** (TLB windows + NOC2AXI/TLB config registers), **BAR2** (DWC PCIe controller registers; iATU), **BAR4** (4 GB TLB windows).

#### BAR0

| BAR0 offset | Size | Contents | Cite |
|---|---|---|---|
| `0x00000000` | 202 × 2 MB = `0x19400000` | 2 MB TLB windows 0..201, window *i* at `i * 0x200000` | blackhole.c:20-23, 746-750 |
| `0x19200000` | `0x200000` | Window 201 = **kernel-reserved TLB** (`KERNEL_TLB_INDEX = 202 - 1`, `KERNEL_TLB_START = 201 * 2MB`) | blackhole.c:40-42 |
| `0x1FC00000` | `0x1000` | TLB configuration registers (`TLB_REGS_START` / `TLB_REGS_LEN`), covers all 2M + 4G window regs and strided regs | blackhole.c:33-34 |
| `0x1FD00000` | `0x100000` | NOC2AXI config (`NOC2AXI_CFG_START` / `NOC2AXI_CFG_LEN`): NOC ID reg, NOC status counters | blackhole.c:44-45 |

```c
#define TLB_REGS_START 0x1FC00000   // BAR0
#define TLB_REGS_LEN 0x00001000     // Covers all TLB registers
...
#define NOC2AXI_CFG_START 0x1FD00000
#define NOC2AXI_CFG_LEN 0x00100000
```
(blackhole.c:33-45)

The code never asserts the total BAR0 size, but the mapped ranges require BAR0 to span at least `0x1FE00000` (≈510 MB). Only the three sub-ranges above are mapped (`pci_iomap_range`), not the whole BAR (blackhole.c:587-589).

#### BAR2

Mapped in full: `bh->bar2_mapping = pci_iomap(bh->tt.pdev, 2, 0);` (blackhole.c:590). The only documented content is the **DesignWare iATU register block at BAR2 offset `0x1000`** (`IATU_BASE`, blackhole.c:76). Nothing else in BAR2 is touched by the driver.

#### BAR4

Holds up to 8 × 4 GB TLB windows, window *j* at BAR4 offset `j * 4GB` (blackhole.c:25-28, 746-750). Not all 8 are guaranteed to be BIOS-exposed; `blackhole_init` clamps the per-device count to what BAR4 actually provides:

```c
resource_size_t bar4_len = pci_resource_len(tt_dev->pdev, 4);
// Limit 4G window count to what's available; partial windows not supported.
tt_dev->tlb_counts[1] = bar4_len / TLB_4G_WINDOW_SIZE;
```
(blackhole.c:576-580; comment at blackhole.c:25). BAR4 itself is never kernel-mapped by blackhole.c — user space mmaps windows into it via the TLB/mmap machinery (see `describe_tlb`, §4).

> **Porting note:** On Windows, BAR0 sub-range mappings become `MmMapIoSpaceEx` over portions of the CM_PARTIAL_RESOURCE_DESCRIPTOR for BAR0; the BAR4-length probe becomes reading the translated resource length for BAR4. The clamp `tlb_counts[1] = bar4_len / 4GB` must be preserved — machines with small BAR support expose fewer than 8 windows and partial windows are unsupported.

---

### 3. TLB windows

#### Geometry constants

```c
#define TLB_2M_WINDOW_COUNT 202
#define TLB_2M_SHIFT 21
#define TLB_4G_WINDOW_COUNT 8	// NB: Not all are guaranteed to be exposed in BAR4
#define TLB_4G_SHIFT 32
#define TLB_REG_SIZE 12	// Same for 2M and 4G
#define TLB_TOTAL_WINDOW_COUNT (TLB_2M_WINDOW_COUNT + TLB_4G_WINDOW_COUNT)   // 210
#define TLB_STRIDED_COUNT 32	// First 32 2M windows support non-rectangular multicast patterns
#define TLB_STRIDED_REG_SIZE 4
#define TLB_STRIDED_REGS_OFFSET (TLB_TOTAL_WINDOW_COUNT * TLB_REG_SIZE)      // 210*12 = 0x9D8
```
(blackhole.c:20-38)

TLB IDs are a single flat namespace 0..209: IDs 0..201 are 2 MB windows, 202..209 are 4 GB windows (blackhole.c:729-735). Config register for TLB *i* (either kind) is at `tlb_regs + i * 12` (blackhole.c:168, 203) — i.e. BAR0 `0x1FC00000 + i*12`.

#### 2 MB window register format (96 bits, packed)

blackhole.c:112-137:

```c
struct TLB_2M_REG { union { struct { u32 low32; u32 mid32; u32 high32; };
	struct __attribute__((packed)) {
		u64 address : 43;   // NOC address >> 21
		u64 x_end : 6;  u64 y_end : 6;  u64 x_start : 6;  u64 y_start : 6;
		u64 noc : 2;  u64 multicast : 1;  u64 ordering : 2;  u64 linked : 1;
		u64 use_static_vc : 1;  u64 stream_header : 1;  u64 static_vc : 3;
		u64 reserved : 18;
	}; }; };
static_assert(sizeof(struct TLB_2M_REG) == TLB_REG_SIZE, ...);   // 12 bytes
```

#### 4 GB window register format (96 bits, packed)

blackhole.c:139-163 — identical fields except `address : 32` (NOC address >> 32) and `reserved : 29`.

#### Programming a window (`blackhole_configure_tlb`)

Dispatch (blackhole.c:724-736): `0 <= tlb < 202` → 2M path; `202 <= tlb < 210` → 4G path; otherwise **-EINVAL**.

2M path (blackhole.c:165-198):
- Validation: `if (config->addr & TLB_2M_WINDOW_MASK) return -EINVAL;` — address must be 2 MB-aligned (blackhole.c:172-173).
- Fields copied from `struct tenstorrent_noc_tlb_config` (ioctl.h:300-313): `addr>>21`, `x_end`, `y_end`, `x_start`, `y_start`, `noc`, `mcast`, `ordering`, `linked`, `static_vc → use_static_vc` (blackhole.c:175-184). Note `stream_header` and `static_vc` (the 3-bit VC number) are never set by this path.
- Three 32-bit MMIO writes: `low32` at +0, `mid32` at +4, `high32` at +8 (blackhole.c:186-188).
- **Strided-register scrub**: for `tlb < 32`, a fourth write zeroes the per-window strided config register at `tlb_regs + 0x9D8 + tlb*4`: "Strided TLB configuration is unsupported by the CONFIGURE_TLB API. Write zero to clear any strided configuration set by alternate means." (blackhole.c:190-195).

4G path (blackhole.c:200-226): same, with 4 GB alignment check (`config->addr & TLB_4G_WINDOW_MASK → -EINVAL`, blackhole.c:207-208) and `addr>>32`.

No range validation is done on `x_end/y_end/x_start/y_start` (u16 in ABI, 6-bit in hardware), `noc` (u8 → 2 bits), `ordering` (u8 → 2 bits): values are **silently truncated** to field width by the bitfield assignment.

No lock is taken in configure_tlb; per-window exclusivity is enforced by the generic TLB allocator (window ownership bitmaps `tt_dev->tlbs`, device.h:65).

#### `blackhole_describe_tlb`

blackhole.c:738-753. Validation: `tlb < 0 || tlb >= 210 → -EINVAL`. Fills `struct tlb_descriptor {int bar; unsigned long size; unsigned long bar_offset;}` (tlb.h:12-16):

- 2M: `bar = 0`, `size = 0x200000`, `bar_offset = tlb * 0x200000`
- 4G: `bar = 4`, `size = 0x100000000`, `bar_offset = (tlb - 202) * 0x100000000`

(blackhole.c:746-750). This is what the mmap path uses to hand user space a window.

#### Kernel-reserved TLB and NOC access primitives

`blackhole_init` claims window 201 for the kernel: `set_bit(KERNEL_TLB_INDEX, tt_dev->tlbs);` and initializes `kernel_tlb_mutex` (blackhole.c:608-610).

`bh_configure_kernel_tlb(bh, x, y, addr, noc)` (blackhole.c:228-241) programs window 201 as a unicast window to `(x_end=x, y_end=y)` at `addr & ~2M_mask` with `ordering = 1; // strict` (blackhole.c:236) and returns `kernel_tlb + (addr & 2M_mask)`.

`noc_read32` / `noc_write32` (blackhole.c:243-268) take `kernel_tlb_mutex`, reprogram the kernel window, do one 32-bit MMIO read/write, and unlock. **Every** kernel-initiated NOC access on Blackhole (ARC scratch, CSM, message queue, DBI-over-NOC) funnels through this single 2 MB window under this mutex.

> **Porting note:** `kernel_tlb_mutex` is acquired in process context only and the code inside sleeps (ARC polling with `usleep_range`), so the Windows equivalent must be a PASSIVE_LEVEL synchronization object (e.g. `FAST_MUTEX`/`KGUARDED_MUTEX` won't do if you also wait with delays — a KEVENT-based mutex or ERESOURCE at PASSIVE_LEVEL is appropriate). Do not use a spinlock: `send_arc_message` holds up to 500 ms + 1000 ms of polling under repeated acquisitions of this mutex.

#### CSM accessors

ARC's Code/State Memory (CSM) window on the NOC is `0x10000000 .. 0x10080000` (base `ARC_CSM_BASE 0x10000000`, size `1 << 19` = 512 KB; telemetry.h:73-74). Range check helper (telemetry.h:75-78):

```c
static inline bool is_range_within_csm(u64 addr, size_t len)
{ return (addr >= ARC_CSM_BASE) && (addr <= (ARC_CSM_BASE + ARC_CSM_SIZE) - len); }
```

`csm_read32`/`csm_write32` (blackhole.c:270-286) validate with `is_range_within_csm(addr, 4)` (→ **-EINVAL**) then perform `noc_read32/noc_write32` to node **(x=8, y=0)** (`ARC_X 8`, `ARC_Y 0`, blackhole.c:57-58) on NOC 0. Exposed as class ops `blackhole_csm_read32`/`blackhole_csm_write32` (blackhole.c:288-296) — the generic `msgqueue.c` ring code calls back through these.

---

### 4. NOC2AXI config region: PCIe instance detection and perf counters

#### Active PCIe instance detection

Blackhole has two PCIe controller instances; only one is connected. The NOC ID register identifies which (blackhole.c:298-302):

```c
// BH has two PCIE instances, the function reads NOC ID to find out which one is active
static bool blackhole_detect_pcie_noc_x(struct blackhole_device *bh, u32 *noc_x) {
	*noc_x = ioread32(bh->noc2axi_cfg + NOC_ID_OFFSET) & 0x3F;
	return (*noc_x == 2 || *noc_x == 11);
}
```

`NOC_ID_OFFSET 0x4044` (blackhole.c:46) — i.e. BAR0 `0x1FD04044`. The active PCIe tile is at NOC0 coordinates `(x, y=0)` with `x ∈ {2, 11}`; anything else means detection failed and save/restore of reset state silently does nothing (blackhole.c:310-311, 323-324).

#### PCIe DBI over NOC

`PCIE_DBI_ADDR 0xF800000000000000ULL` — "this points to outbound NOC_TLB_62 configured by CMFW" (blackhole.c:50-51). Reads/writes to `(pcie_x, 0)` at this NOC address reach the DWC PCIe controller's DBI registers. Used only for the Device Control/Status register: `DBI_DEVICE_CONTROL_DEVICE_STATUS 0x78` (pcie.h:8).

#### PCIe performance counters (sysfs)

Counter read: `value = ioread32(noc2axi_cfg + NOC_STATUS_OFFSET + 4*counter_id + noc*NOC1_NOC2AXI_OFFSET)` with `NOC_STATUS_OFFSET 0x4200` and `NOC1_NOC2AXI_OFFSET 0x10000` (blackhole.c:47-48, 332-339). Counter IDs (blackhole.c:354-359):

```c
#define SLV_POSTED_WR_DATA_WORD_RECEIVED 0x39
#define SLV_NONPOSTED_WR_DATA_WORD_RECEIVED 0x38
#define SLV_RD_DATA_WORD_SENT 0x33
#define MST_POSTED_WR_DATA_WORD_SENT 0x9
#define MST_NONPOSTED_WR_DATA_WORD_SENT 0x8
#define MST_RD_DATA_WORD_RECEIVED 0x3
```

Each is exposed twice (suffix `0`/`1` for NOC0/NOC1) in sysfs group `pcie_perf_counters` (blackhole.c:341-389).

> **Porting note:** sysfs groups have no direct KMDF equivalent; expose these as an IOCTL, WMI, or registry-less query interface. The register arithmetic (`0x1FD04200 + 4*id + noc*0x10000` in BAR0) is the part the port must preserve.

---

### 5. iATU (outbound address translation), BAR2

DesignWare unrolled iATU register block, at BAR2 offset `0x1000`, 16 outbound regions, per-(region,direction) stride `0x100`:

```c
#define IATU_BASE 0x1000	// Relative to the start of BAR2
#define IATU_OUTBOUND 0
#define IATU_OUTBOUND_REGIONS 16
#define IATU_REGION_STRIDE 0x100
#define IATU_REGION_CTRL_1_OUTBOUND 0x00
#define IATU_REGION_CTRL_2_OUTBOUND 0x04
#define IATU_LOWER_BASE_ADDR_OUTBOUND 0x08
#define IATU_UPPER_BASE_ADDR_OUTBOUND 0x0C
#define IATU_LOWER_LIMIT_ADDR_OUTBOUND 0x10
#define IATU_LOWER_TARGET_ADDR_OUTBOUND 0x14
#define IATU_UPPER_TARGET_ADDR_OUTBOUND 0x18
#define IATU_REGION_CTRL_3_OUTBOUND 0x1C
#define IATU_UPPER_LIMIT_ADDR_OUTBOUND 0x20
#define INCREASE_REGION_SIZE (1 << 13)   // IATU_REGION_CTRL_1 field
#define REGION_EN (1 << 31)              // IATU_REGION_CTRL_2 field
```
(blackhole.c:76-94)

Register address computation (blackhole.c:104-110):

```c
u32 offset = IATU_BASE + (2 * region + direction) * IATU_REGION_STRIDE + reg;
iowrite32(value, bh_dev->bar2_mapping + offset);
```

With `direction = IATU_OUTBOUND = 0`, outbound region *r*'s block is at BAR2 `0x1000 + r*0x200` (inbound would interleave at `+0x100`, but the driver never programs inbound regions).

`blackhole_configure_outbound_atu(tt_dev, region, base, limit, target)` (blackhole.c:755-788):

- **Validation:** `size = limit - base + 1`; `size > SZ_1T (0x10000000000)` → **-EINVAL** ("iATU has a max region size of 1T"); `region >= 16` → **-EINVAL** (blackhole.c:770-775; SZ_1T at blackhole.c:96-98).
- `region_ctrl_1 = INCREASE_REGION_SIZE` always (enables the upper-limit register, i.e. >4 GB regions); `region_ctrl_2 = (limit == 0) ? 0 : REGION_EN` — so calling with `base = limit = target = 0` **disables** the region; `region_ctrl_3 = 0` (blackhole.c:760-762).
- Write order: LOWER/UPPER_BASE, LOWER/UPPER_TARGET, LOWER/UPPER_LIMIT, then CTRL_1, CTRL_2, CTRL_3 (blackhole.c:777-785). CTRL_2 (with REGION_EN) is written before CTRL_3.

Locking: the op itself takes no lock; the generic caller in memory.c holds `tt_dev->iatu_mutex` around region allocation/programming and around teardown-time disable (`configure_outbound_atu(tt_dev, iatu_region, 0, 0, 0)`) (memory.c:183-198, 284-296; mutex declared device.h:69-70).

Semantics: outbound iATU maps NOC-side addresses ("base..limit" in the chip's PCIe-outbound aperture) to host bus addresses (`target`), used by the PIN_PAGES/DMA path so NOC masters can reach host memory. The device-class parameters that pair with this are `noc_dma_limit = (1ULL << 58) - 1` and `noc_pcie_offset = (4ULL << 58)` = `0x1000000000000000` (blackhole.c:817-818): DMA bus addresses must fit under 2^58, and the NOC address a Tensix uses to reach host memory is `0x1000000000000000 + iATU-region base` (memory.c:175, 196). `dma_address_bits = 58` (blackhole.c:816) sets the coherent DMA mask (enumerate.c:330-331).

---

### 6. ARC (firmware CPU): scratch registers, boot status, message queue

The ARC management CPU lives at NOC0 node **(8, 0)** (blackhole.c:57-58). Its reset-unit scratch registers are NOC-addressable:

```c
#define RESET_SCRATCH(N) (0x80030400 + ((N) * 4))
#define ARC_TELEMETRY_PTR RESET_SCRATCH(13)   // = 0x80030434
#define ARC_TELEMETRY_DATA RESET_SCRATCH(12)  // = 0x80030430
#define ARC_MSG_QCB_PTR RESET_SCRATCH(11)     // = 0x8003042C, Message Queue Control Block
#define ARC_MSI_FIFO 0x800B0000               // Write 0 to trigger the ARC message queue processor
#define ARC_MSG_READY_MS 500
#define ARC_BOOT_STATUS RESET_SCRATCH(2)      // = 0x80030408
#define ARC_BOOT_STATUS_READY_FOR_MSG 0x1
```
(blackhole.c:59-74; "see msgqueue.c in tt-zephyr-platforms.git" comment at blackhole.c:63)

ARC message opcodes (blackhole.c:67-72):

```c
#define ARC_MSG_TYPE_ASIC_STATE0 0xA0
#define ARC_MSG_TYPE_ASIC_STATE3 0xA3
#define ARC_MSG_TYPE_SET_WDT_TIMEOUT 0xC1
#define ARC_MSG_TYPE_TRIGGER_RESET 0x56
#define ARC_MSG_TYPE_POWER_SETTING 0x21
#define ARC_MSG_TYPE_TEST 0x90
```

Message format: `struct arc_msg { u32 header; u32 payload[7]; }` — 32 bytes (msgqueue.h:11-14).

#### `send_arc_message` sequence (blackhole.c:500-540)

1. **Ready poll:** read `ARC_BOOT_STATUS` (0x80030408) via kernel TLB in a loop for up to `ARC_MSG_READY_MS` = **500 ms**. If a read returns `0xFFFFFFFF` → "NOC is hung", return false immediately (blackhole.c:509-515). If bit 0 (`READY_FOR_MSG`) never sets → false.
2. **Queue discovery:** `queue_ctrl_addr = noc_read32(ARC_MSG_QCB_PTR)` (0x8003042C); then via CSM-validated reads: `queue_base = csm[qcb+0]`, `queue_info = csm[qcb+4]`, `num_entries = queue_info & 0xFF` (blackhole.c:520-528). Because these go through `csm_read32`, a QCB pointer outside `0x10000000..0x1007FFFC` fails the send.
3. **Push** request via generic ring code `arc_msg_push` (msgqueue.c:12-70): request ring starts at `queue_base + 32` (`ARC_MSG_QUEUE_HEADER_SIZE 32`, msgqueue.h:16); pointers at `queue_base+0x00` (REQ_WPTR), `+0x04` (RES_RPTR), `+0x10` (REQ_RPTR), `+0x14` (RES_WPTR) (msgqueue.h:19-22); occupancy = `(wptr - rptr) % (2*num_entries)`; waits up to `ARC_MSG_TIMEOUT_MS 1000` (msgqueue.h:17) with `usleep_range(100, 200)` polling; slot = `wptr % num_entries`; 8 dword writes; wptr advanced mod `2*num_entries`. All-1s pointer reads abort ("device gone?", msgqueue.c:25-28).
4. **Doorbell:** `noc_write32(bh, 8, 0, ARC_MSI_FIFO /*0x800B0000*/, 0, 0)` — "Trigger ARC interrupt" (blackhole.c:533-534).
5. **Pop** response via `arc_msg_pop` (msgqueue.c:72-132): response ring at `queue_base + 32 + num_entries*32`; same 1000 ms poll for occupancy > 0; reads 8 dwords; advances RES_RPTR.
6. **Success criterion:** `return msg->header == 0;` — firmware writes 0 into the response header on success (blackhole.c:539).

The whole exchange is synchronous, polled, interrupt-free from the host side, and every CSM access re-acquires `kernel_tlb_mutex` (one lock cycle per 32-bit access).

> **Porting note:** Worst-case latency of one message ≈ 500 ms (ready) + 1000 ms (push space) + 1000 ms (response) of sleeping poll loops. On Windows this must run at PASSIVE_LEVEL (e.g. a system worker/passive-level DPC substitute or the calling IOCTL thread), never at DISPATCH_LEVEL.

---

### 7. Telemetry

#### Discovery (`telemetry_probe`, class op `.probe_telemetry`) — blackhole.c:455-498

1. Zero the per-device `telemetry_tag_cache[128]` (`TELEM_TAG_CACHE_SIZE 128`, telemetry.h:13; cache field device.h:75-79 — "BH stores a raw CSM address for NOC reads").
2. `base_addr = noc_read32(ARC_TELEMETRY_PTR /*0x80030434*/)`, `data_addr = noc_read32(ARC_TELEMETRY_DATA /*0x80030430*/)`; `tags_addr = base_addr + 8` (blackhole.c:466-468).
3. Both pointers must lie in CSM (checked with length 1) else `dev_err("Telemetry not available")` and **-ENODEV** (blackhole.c:470-473).
4. Version word at `base_addr`: `major = (v>>16)&0xFF, minor = (v>>8)&0xFF, patch = v&0xFF`; `major > 1` → **-ENOTSUPP** (blackhole.c:475-483).
5. `num_entries` at `base_addr + 4`; then a tag table scan: each entry at `tags_addr + i*4` is `{u16 tag_id = entry & 0xFFFF; u16 offset = entry >> 16}`; the cached address is `data_addr + offset*4`; only tags < 128 are cached (blackhole.c:485-495). Returns 0.

So the telemetry data layout in CSM is: `[version u32][num_entries u32][tag entries u32 × N]` at `base_addr`, values array at `data_addr` (dword-indexed by `offset`).

Note the tag-table reads use raw `noc_read32` (not `csm_read32`), so per-entry addresses are **not** re-validated against CSM during the scan; validation happens later at read time.

#### Read path (`blackhole_read_telemetry_tag`, class op `.read_telemetry_tag`) — blackhole.c:440-453

- `tag_id >= 128` → **-EINVAL**
- cache entry 0 (tag absent) → **-ENODATA**
- else `csm_read32(bh, addr, value)` (re-validates CSM range → -EINVAL if out; returns 0 on success).

#### Consumers (sysfs + hwmon)

Telemetry tag IDs used by Blackhole (telemetry.h:15-41 for values):

- sysfs attributes (blackhole.c:413-424): `tt_aiclk` (AICLK=14), `tt_axiclk` (15), `tt_arcclk` (16), `tt_serial` + `tt_card_type` (BOARD_ID=1, the latter read as u64: tag N and N+1), `tt_fw_bundle_ver` (28), `tt_m3app_fw_ver` (26), `tt_asic_id` (61), `tt_heartbeat` (32), `tt_therm_trip_count` (60).
- hwmon channels (blackhole.c:400-411): ASIC_TEMP=11→temp_input, THM_LIMIT_THROTTLE=56→temp_max, VCORE=6→in_input, VDD_LIMITS=9→in_max, CURRENT=8→curr_input, TDC_LIMIT_MAX=55→curr_max, POWER=7→power_input, TDP_LIMIT_MAX=64→power_max, FAN_RPM=41→fan_input. hwmon device registered under name `"blackhole"` (blackhole.c:664).

---

### 8. The `tenstorrent_device_class` ops table

Definition: device.h:87-123. Blackhole's instantiation (blackhole.c:813-840):

```c
struct tenstorrent_device_class blackhole_class = {
	.name = "Blackhole",
	.instance_size = sizeof(struct blackhole_device),
	.dma_address_bits = 58,
	.noc_dma_limit = (1ULL << 58) - 1,
	.noc_pcie_offset = (4ULL << 58),
	.tlb_kinds = 2,
	.tlb_counts = { TLB_2M_WINDOW_COUNT, TLB_4G_WINDOW_COUNT },   // {202, 8}
	.tlb_sizes = { TLB_2M_WINDOW_SIZE, TLB_4G_WINDOW_SIZE },      // {2MB, 4GB}
	...
};
```

| Op | BH implementation | When called (per enumerate.c) | What it does |
|---|---|---|---|
| `.init_device` | `blackhole_init` (572-618) | probe, before hardware init (enumerate.c:364) | Clamp `tlb_counts[1]` by BAR4 size; alloc telemetry attr array (devm); map BAR0 sub-ranges + BAR2; claim kernel TLB 201; init `kernel_tlb_mutex`; wire sysfs telemetry group. Returns false on mapping/alloc failure (probe fails). |
| `.init_hardware` | `blackhole_init_hardware` (620-639) | probe (enumerate.c:370), and again on resume (enumerate.c:515) | `pcie_set_readrq(pdev, 4096)` (`MAX_MRRS 4096`, blackhole.c:18); ARC msg `0xA0` (ASIC_STATE0 — go to full-power A0); ARC msg `0xC1` SET_WDT_TIMEOUT with `payload[0] = 1000 * auto_reset_timeout` ms (module default 10 s, module.c:48-50; failure only warns: "normal for old FW"). Always returns true. |
| `.init_telemetry` | `blackhole_init_telemetry` (641-675) | probe, only if `!needs_hw_init` (enumerate.c:382-383) | Register `pcie_perf_counters` sysfs group; run `telemetry_probe`; if probe ok: register telemetry sysfs group, set hwmon attr/label tables, `hwmon_device_register_with_info(..., "blackhole", ...)` — hwmon failure returns false; emit `KOBJ_CHANGE` uevent. |
| `.probe_telemetry` | `telemetry_probe` (455-498) | re-probe path (also used post-reset by common code) | Tag scan as in §7. |
| `.read_telemetry_tag` | `blackhole_read_telemetry_tag` (440-453) | sysfs/hwmon reads | §7 read path. |
| `.cleanup_telemetry` | `blackhole_cleanup_telemetry` (677-695) | device remove, before cleanup_hardware (enumerate.c:439-440) | Unregister hwmon (if set), telemetry group, perf-counter group — each guarded by its registered flag. |
| `.cleanup_hardware` | `blackhole_cleanup_hardware` (697-708) | remove (enumerate.c:434) and suspend (enumerate.c:506) | If `tt_dev->detached` do nothing; else ARC msg `0xA3` (ASIC_STATE3, low-power). |
| `.cleanup_device` | `blackhole_cleanup` (710-722) | remove, last (enumerate.c:446 "unmap BARs") | `pci_iounmap` each of the four mappings, NULL-guarded. |
| `.reset` | `blackhole_reset` (542-570) | RESET_DEVICE ioctl paths | §9. |
| `.save_reset_state` | `blackhole_save_reset_state` (304-315) | probe (enumerate.c:373) and around resets | Detect PCIe tile x∈{2,11}; read DBI `0x78` (Device Control/Status) over NOC at `0xF800000000000000 + 0x78`; save MPS field (`PCI_EXP_DEVCTL_PAYLOAD`) into `bh->saved_mps`. Silently no-ops if detection fails. |
| `.restore_reset_state` | `blackhole_restore_reset_state` (317-330) | after reset/FLR restore | Same detection; read-modify-write DBI 0x78 to restore saved MPS. |
| `.configure_tlb` | `blackhole_configure_tlb` (724-736) | CONFIGURE_TLB ioctl | §3. |
| `.describe_tlb` | `blackhole_describe_tlb` (738-753) | TLB alloc/mmap | §3. |
| `.configure_outbound_atu` | `blackhole_configure_outbound_atu` (755-788) | iATU ioctls / DMA setup / teardown | §5. |
| `.noc_write32` | `blackhole_noc_write32` (790-794) | common-code raw NOC write | Thin wrapper over `noc_write32` (kernel TLB). |
| `.csm_read32` / `.csm_write32` | blackhole.c:288-296 | msgqueue.c ring accessors, telemetry | §3 CSM accessors. |
| `.set_power_state` | `blackhole_set_power_state` (796-811) | SET_POWER_STATE ioctl | ARC msg with `header = 0x21 | (validity << 8) | (power_flags << 16)`; `payload[0..6]` = `power_settings[14]` (u16×14 = 28 bytes, exactly `sizeof(msg.payload)`, asserted by `BUILD_BUG_ON`, blackhole.c:803). Send failure → **-EINVAL**. |
| `.reboot` | **not implemented** | — | Because `.reboot` is NULL, the reboot notifier is *not registered* for Blackhole devices (enumerate.c:377-380; notifier body enumerate.c:243-251). Wormhole implements it; BH relies on ARC FW to handle host reboot. |
| `.defer_idle_powerdown` | **not set (false)** | fd release | Idle power-down message goes out synchronously from `release()` (historical behavior) rather than via `power_down_work` delayed work (device.h:116-122, 59-63). |

There is no op literally named `post_reset` in this codebase; the post-reset roles are covered by `save_reset_state`/`restore_reset_state` plus re-running `init_hardware` (`needs_hw_init` flag, device.h:34; set at enumerate.c:305, 370).

---

### 9. Reset sequences

Reset flags from the ioctl ABI: `TENSTORRENT_RESET_DEVICE_ASIC_RESET 4`, `TENSTORRENT_RESET_DEVICE_ASIC_DMC_RESET 5` (ioctl.h:149-150). `blackhole_reset` (blackhole.c:542-570) supports exactly these two; any other flag → returns false.

#### ASIC+DMC reset (flag 5) — firmware-mediated, step by step

1. Probe firmware liveness with a **TEST message** (`header = 0x90`, no payload). If `send_arc_message` fails: `dev_warn("Couldn't communicate with firmware; NOC is likely hung.")`, return false (blackhole.c:550-555).
2. `set_reset_marker(pdev)`: sets the `PCI_COMMAND_PARITY` bit in config-space PCI_COMMAND — "pci_command_parity is used as reset marker. Set to 1, check if cleared to 0 after reset" (pcie.c:140-149; checked later by `is_reset_marker_zero`, pcie.c:151-158 — a reset clears config space, so a cleared parity bit proves the device actually reset).
3. Send **TRIGGER_RESET** (`header = 0x56`, `payload[0] = 3 // Argument for ASIC + M3 reset`) (blackhole.c:549, 559-562). The response is *not* awaited meaningfully — return value of the second send is ignored; function returns `true; // Possibly a lie...` (blackhole.c:562-563).

#### ASIC-only reset (flag 4) — config-space interface timer

1. `set_reset_marker(pdev)` (blackhole.c:565).
2. `pcie_timer_interrupt(pdev)` (pcie.c:133-138): two PCI **config-space** dword writes into the DWC DBI-mapped-to-config registers:

```c
#define INTERFACE_TIMER_CONTROL_OFF 0x930
#define INTERFACE_TIMER_TARGET_OFF 0x934
#define INTERFACE_TIMER_TARGET 0x1
#define INTERFACE_TIMER_EN 0x1
#define INTERFACE_FORCE_PENDING 0x10
pci_write_config_dword(pdev, INTERFACE_TIMER_TARGET_OFF, INTERFACE_TIMER_TARGET);       // 0x934 <- 0x1
pci_write_config_dword(pdev, INTERFACE_TIMER_CONTROL_OFF, INTERFACE_TIMER_EN | INTERFACE_FORCE_PENDING); // 0x930 <- 0x11
```
(pcie.c:16-22, 133-138). This forces a pending interface-timer interrupt that the CMFW/hardware turns into an ASIC reset.

The delays / link-retrain / config-restore around a reset live in the common layer (other sections): `pcie_hot_reset_and_restore_state` does a secondary-bus reset with 2 ms assert + 500 ms settle then polls vendor ID up to 10 s (pcie.c:61-90); note that `wormhole_complete_pcie_init` retraining (pcie.c:92-131, `FW_MSG_PCIE_RETRAIN 0xB6`, `reset_limit` default 10, module.c:44) is **Wormhole-only** — there is no Blackhole link-retrain loop in the driver.

#### MPS save/restore around reset

Reset wipes the endpoint's Device Control register. `blackhole_save_reset_state` captures the negotiated Max Payload Size before reset and `blackhole_restore_reset_state` re-writes it afterward — but via **NOC access to the DBI** (`0xF800000000000000 + 0x78` at tile (2|11, 0)), not via host config space (blackhole.c:304-330). Only the MPS field (`PCI_EXP_DEVCTL_PAYLOAD`) is preserved; `saved_mps` is a u8 holding the 3-bit field value (blackhole.h:19).

> **Porting note:** On Windows, config-space writes for `set_reset_marker`/`pcie_timer_interrupt` map to `BUS_INTERFACE_STANDARD`/`IoGetDeviceProperty`-style config access (`SetBusData`/`GetBusData`). Restoring MPS through the NOC (not config space) must be kept as-is: the host-side config write would race the DWC DBI state. Also note MRRS is re-set to 4096 in `init_hardware` on every hardware (re)init (blackhole.c:626) — Windows PCI stack may clamp MRRS differently; the port must re-apply it after every reset/resume.

---

### 10. Card variants: p150a, Galaxy

There is **no p150a-specific code path in blackhole.c** — all Blackhole boards share this personality. Card differentiation is by telemetry BOARD_ID (tag 1) upper bits: `card_type = (value >> 4) & 0xFFFF` where `value` is the *upper* 32 bits of the 64-bit board ID; Blackhole types: `0x36`=p100, `0x40`=**p150a**, `0x41`=p150b, `0x42`=p150c, `0x43`=p100a, `0x44`=p300b, `0x45`=p300a, `0x46`=p300c, `0x47`=galaxy-blackhole (telemetry.c:108-123).

`PCI_SUBSYSTEM_DEVICE_GALAXY 0x0047` is defined in blackhole.c:53-54 but **never used in blackhole.c** (dead constant). Galaxy-BH handling lives in enumerate.c: subsystem ID `PCI_SUBSYSTEM_ID_GALAXY_BH 0x0047` triggers deterministic ordinal assignment from PCI bus number using UBB bus prefixes `{0x0, 0x4, 0xC, 0x8}` (enumerate.c:42-46, 57-72, 356).

---

### 11. Hardware quirks, workarounds, failure behaviors

1. **BAR2 mapping not checked in init.** `blackhole_init`'s failure check tests only `tlb_regs`, `kernel_tlb`, `noc2axi_cfg` — *not* `bar2_mapping` (blackhole.c:592: `if (!bh->tlb_regs || !bh->kernel_tlb || !bh->noc2axi_cfg)`). If BAR2 mapping fails, init succeeds and a later `configure_outbound_atu` dereferences `NULL + offset`. The error path *does* unmap `bar2_mapping` if the other three failed (blackhole.c:602-603). A Windows port should treat a BAR2 mapping failure as fatal.
2. **All-1s reads as hang detection.** `0xFFFFFFFF` from `ARC_BOOT_STATUS` (blackhole.c:511-512) or from queue pointers (msgqueue.c:25-28, 38-41, 85-88, 98-101) is treated as "NOC hung / device gone" and aborts the operation. A port must preserve these checks — they are the only defense against surprise-removal / hung-NOC deadloops.
3. **Strided-TLB scrub** on every 2M window configure for windows 0..31 (blackhole.c:190-195) — protects against stale non-rectangular multicast configs programmed "by alternate means" (e.g. previous user, firmware).
4. **`reset` returning "possibly a lie"** — after TRIGGER_RESET the device may drop off the bus before responding; the true confirmation is the reset marker (PCI_COMMAND parity bit) reading 0 after re-enumeration (blackhole.c:563; pcie.c:144, 151-158).
5. **Silent field truncation** in TLB config (§3) — no `-EINVAL` for out-of-range NOC coordinates; hardware bitfields truncate.
6. **`cleanup_hardware` skipped when `detached`** (blackhole.c:702-703; `detached` flag device.h:33) — after surprise removal or reset-detach, no A3 message is attempted against dead hardware.
7. **WDT set failure is non-fatal** ("normal for old FW", blackhole.c:636) — the port must not fail init when firmware rejects opcode 0xC1.
8. **`telemetry_probe` CSM checks use length 1** (blackhole.c:470) rather than 4 — a base pointer at `CSM_END - 1` would pass the probe check but the subsequent version read would extend past CSM (raw `noc_read32`, unvalidated).

---

### 12. Lifecycle / cleanup summary (fd close, device removal)

- Nothing in blackhole.c is per-fd; all state is per-device. Per-fd TLB/iATU/pin cleanup is in the common layers (memory.c/tlb.c), which call back into `configure_tlb` / `configure_outbound_atu(region, 0, 0, 0)` (memory.c:284-296) to disable resources at fd close.
- Device removal order (enumerate.c:423-459): set `detached` → cancel `power_down_work` → `cleanup_hardware` (A3 message, skipped if detached or device already gone) → `cleanup_telemetry` (hwmon + sysfs groups) → `cleanup_device` (unmap BARs) → revoke TLB dmabufs (enumerate.c:459, after BAR unmap). Suspend calls `cleanup_hardware` only (enumerate.c:506); resume calls `init_hardware` and re-saves PCI state (enumerate.c:515-519).
- `telemetry_attrs` array is devm-allocated (blackhole.c:582) — freed automatically at unbind; a Windows port must free it explicitly in cleanup.

---

### Key constants table

| Name | Value | Source |
|---|---|---|
| `PCI_DEVICE_ID_BLACKHOLE` | `0xB140` | enumerate.h:18 |
| `MAX_MRRS` | `4096` | blackhole.c:18 |
| `TLB_2M_WINDOW_COUNT` | `202` | blackhole.c:20 |
| `TLB_2M_SHIFT` / window size | `21` / 2 MB | blackhole.c:21-22 |
| `TLB_4G_WINDOW_COUNT` | `8` (clamped by BAR4 size) | blackhole.c:25, 580 |
| `TLB_4G_SHIFT` / window size | `32` / 4 GB | blackhole.c:26-27 |
| `TLB_REG_SIZE` | `12` bytes | blackhole.c:30 |
| `TLB_TOTAL_WINDOW_COUNT` | `210` | blackhole.c:31 |
| `TLB_REGS_START` (BAR0) | `0x1FC00000`, len `0x1000` | blackhole.c:33-34 |
| `TLB_STRIDED_COUNT` | `32` | blackhole.c:36 |
| `TLB_STRIDED_REGS_OFFSET` | `210*12 = 0x9D8` (reg stride 4) | blackhole.c:37-38 |
| `KERNEL_TLB_INDEX` / start | `201` / BAR0 `0x19200000` | blackhole.c:40-41 |
| `NOC2AXI_CFG_START` (BAR0) | `0x1FD00000`, len `0x100000` | blackhole.c:44-45 |
| `NOC_ID_OFFSET` | `0x4044` (mask `0x3F`; valid x: 2 or 11) | blackhole.c:46, 300-301 |
| `NOC_STATUS_OFFSET` | `0x4200` | blackhole.c:47 |
| `NOC1_NOC2AXI_OFFSET` | `0x10000` | blackhole.c:48 |
| `PCIE_DBI_ADDR` | `0xF800000000000000` (NOC addr, via outbound NOC_TLB_62 set by CMFW) | blackhole.c:50-51 |
| `PCI_SUBSYSTEM_DEVICE_GALAXY` | `0x0047` (unused in blackhole.c) | blackhole.c:54 |
| `ARC_X`, `ARC_Y` | `8`, `0` | blackhole.c:57-58 |
| `RESET_SCRATCH(N)` | `0x80030400 + N*4` | blackhole.c:59 |
| `ARC_TELEMETRY_PTR` | `0x80030434` (scratch 13) | blackhole.c:60 |
| `ARC_TELEMETRY_DATA` | `0x80030430` (scratch 12) | blackhole.c:61 |
| `ARC_MSG_QCB_PTR` | `0x8003042C` (scratch 11) | blackhole.c:64 |
| `ARC_MSI_FIFO` | `0x800B0000` (write 0 = doorbell) | blackhole.c:65 |
| `ARC_MSG_READY_MS` | `500` ms | blackhole.c:66 |
| `ARC_BOOT_STATUS` | `0x80030408` (scratch 2), ready bit `0x1` | blackhole.c:73-74 |
| ARC opcodes | A0=`0xA0`, A3=`0xA3`, WDT=`0xC1`, RESET=`0x56`, POWER=`0x21`, TEST=`0x90` | blackhole.c:67-72 |
| `ARC_MSG_QUEUE_HEADER_SIZE` | `32` | msgqueue.h:16 |
| `ARC_MSG_TIMEOUT_MS` | `1000` ms | msgqueue.h:17 |
| Queue ptr offsets | REQ_WPTR +0x00, RES_RPTR +0x04, REQ_RPTR +0x10, RES_WPTR +0x14 | msgqueue.h:19-22 |
| `ARC_CSM_BASE` / `ARC_CSM_SIZE` | `0x10000000` / `1<<19` (512 KB) | telemetry.h:73-74 |
| `TELEM_TAG_CACHE_SIZE` | `128` | telemetry.h:13 |
| `IATU_BASE` (BAR2) | `0x1000` | blackhole.c:76 |
| `IATU_OUTBOUND_REGIONS` | `16` | blackhole.c:78 |
| `IATU_REGION_STRIDE` | `0x100` (outbound region r at `0x1000 + r*0x200`) | blackhole.c:79, 106 |
| iATU reg offsets | CTRL_1 +0x00, CTRL_2 +0x04, LBASE +0x08, UBASE +0x0C, LLIMIT +0x10, LTARGET +0x14, UTARGET +0x18, CTRL_3 +0x1C, ULIMIT +0x20 | blackhole.c:80-88 |
| `INCREASE_REGION_SIZE` | `1 << 13` (CTRL_1) | blackhole.c:91 |
| `REGION_EN` | `1 << 31` (CTRL_2) | blackhole.c:94 |
| iATU max region size | `SZ_1T = 0x10000000000` | blackhole.c:96-97, 771 |
| PCIe perf counter IDs | 0x39, 0x38, 0x33, 0x9, 0x8, 0x3 | blackhole.c:354-359 |
| `DBI_DEVICE_CONTROL_DEVICE_STATUS` | `0x78` | pcie.h:8 |
| Interface timer config regs | control `0x930`, target `0x934`; EN `0x1`, FORCE_PENDING `0x10`, TARGET `0x1` | pcie.c:17-22 |
| `dma_address_bits` | `58` | blackhole.c:816 |
| `noc_dma_limit` | `(1<<58)-1 = 0x3FFFFFFFFFFFFFF` | blackhole.c:817 |
| `noc_pcie_offset` | `4<<58 = 0x1000000000000000` | blackhole.c:818 |
| `auto_reset_timeout` default | `10` s (→ 10000 ms WDT) | module.c:48, blackhole.c:634 |
| Reset ioctl flags | ASIC_RESET=4, ASIC_DMC_RESET=5 | ioctl.h:149-150 |
| `TENSTORRENT_MAX_OUTBOUND_IATU_REGIONS` | `16` | memory.h:65 |

### Open questions

1. **BAR0 total size** is never asserted in the driver; mapped offsets require ≥ `0x1FE00000`. Whether BAR0 is exactly 512 MB and what (if anything) lives between `0x19400000` (end of window 201) and `0x1FC00000`, and above `0x1FE00000`, is not derivable from tt-kmd.
2. **BAR2 contents beyond iATU**: BAR2 is mapped in full but only `0x1000 + iATU block` is touched. The full BAR2 register map (DWC DBI mirror? reset unit?) is unknown from this code.
3. **`stream_header`/`static_vc` TLB fields** exist in the register layout (blackhole.c:131-132, 157-158) but are never set by the driver; hardware semantics unknown here.
4. **Interface-timer reset mechanism** (`0x930/0x934` config writes) — how the DWC interface timer interrupt is converted into an ASIC reset is firmware behavior (CMFW), not visible in the driver. The exact post-reset settle time for Blackhole flag-4 reset is handled by common code, not blackhole.c.
5. **TRIGGER_RESET payload semantics**: `payload[0] = 3` is commented "ASIC + M3 reset" (blackhole.c:549); other argument values are undocumented in tt-kmd (source of truth: tt-zephyr-platforms.git per blackhole.c:63).
6. **NOC ID register format**: only the low 6 bits (x coordinate) are consumed (blackhole.c:300); rest of the register is undocumented here. Whether a y-coordinate check should also apply is unknown (code assumes y=0).
7. **`is_range_within_csm(base_addr, 1)`** in telemetry_probe (blackhole.c:470) uses length 1, allowing a base within 3 bytes of CSM end whose subsequent 4-byte reads would exceed CSM — intentional slack or oversight?
8. **BAR2 NULL-check omission** (blackhole.c:592) — bug or deliberate (BAR2 considered optional)? Port should decide to treat as fatal.
9. **Galaxy-BH (subsystem 0x0047)**: `PCI_SUBSYSTEM_DEVICE_GALAXY` is defined in blackhole.c but unused; whether Galaxy Blackhole needs personality-level differences (beyond enumerate.c ordinal assignment) is not visible in this file.
10. **p150a-specific behavior**: none exists in the kernel driver; any board-specific tuning (power limits, harvesting) is firmware/UMD territory. Confirm against tt-umd before assuming the Windows KMD needs no board-type branches.

---

## 08. Wormhole hardware (BARs, registers, ARC messaging, telemetry, reset)

### Scope

Files fully read for this section:

- `wormhole.c` — 1084 lines. The Wormhole device-class implementation (ops table `wormhole_class`).
- `wormhole.h` — 32 lines. `struct wormhole_device` and the one exported helper `wormhole_send_arc_fw_message_with_args`.
- `pcie.c` — 159 lines. Shared PCIe reset/link helpers plus the Wormhole-only `wormhole_complete_pcie_init`.
- `pcie.h` — 18 lines. `DBI_DEVICE_CONTROL_DEVICE_STATUS` and PCIe helper prototypes.
- `msgqueue.c` — 133 lines / `msgqueue.h` — 27 lines. The **shared** queue-based ARC message protocol (`arc_msg_push`/`arc_msg_pop`), used by both Wormhole (power settings only) and Blackhole (everything).

Supporting files read for cross-references (covered in depth in their own sections):
`device.h` (device-class vtable, `struct tenstorrent_device`), `telemetry.h` (`ARC_CSM_BASE`, `is_range_within_csm`, tag enum, `TELEM_TAG_CACHE_SIZE`), `tlb.h` (`struct tlb_descriptor`), `enumerate.h`/`enumerate.c` (PCI IDs, probe/remove), `module.c`/`module.h` (module params `auto_reset_timeout`, `reset_limit`, `idle_power_down_grace_ms`; PCI ID table), `ioctl.h` (`tenstorrent_noc_tlb_config`, `tenstorrent_power_state`, reset flags).

**Grayskull status:** No `grayskull.c`/`grayskull.h` remain in the tree. The only Grayskull traces are the PCI device ID `PCI_DEVICE_ID_GRAYSKULL 0xFACA` (`enumerate.h:16`) and its table entry, which is **deprecated with `NULL` driver_data** (`module.c:65-66`). At probe, a `NULL` driver_data is rejected: `if (!id->driver_data) { dev_warn(&dev->dev, "Unsupported device\n"); return -ENODEV; }` (`enumerate.c:261-264`). A Windows port does **not** need Grayskull support.

> **Porting note:** Primary port target is Blackhole. Throughout this section, mechanisms are tagged **[WH-only]** (Wormhole-specific, defer unless Wormhole support is scheduled) or **[SHARED]** (common helper reused by Blackhole; porting it benefits both). The scratch-register ARC protocol is [WH-only]; the queue-based `msgqueue.c` protocol is [SHARED].

---

### 1. Device class registration and identity

Wormhole matches PCI vendor `0x1E52` / device `0x401E` (`enumerate.h:15,17`) and binds `&wormhole_class` via `driver_data` (`module.c:67-68`). The ops table:

```c
struct tenstorrent_device_class wormhole_class = {
	.name = "Wormhole",
	.instance_size = sizeof(struct wormhole_device),
	.dma_address_bits = 32,
	.noc_dma_limit = (0xFFFE0000 - 1),
	.noc_pcie_offset = 0x800000000ULL,
	.tlb_kinds = NUM_TLB_KINDS,                    // 3
	.tlb_counts = { 156, 10, 20 },
	.tlb_sizes  = { 1M, 2M, 16M },
	...
	.defer_idle_powerdown = true,
};
```
(`wormhole.c:1055-1084`)

- `dma_address_bits = 32` — coherent DMA mask is 32-bit; legacy WH userspace assumes 32-bit DMA addresses from ALLOCATE_DMA_BUF (`enumerate.c:322-327`). Contrast Blackhole.
- `noc_dma_limit = 0xFFFE0000 - 1` (`wormhole.c:1059`).
- `noc_pcie_offset = 0x800000000ULL` (`wormhole.c:1060`) — same value as `PCIE_DBI_ADDR` (`wormhole.c:92`).
- `defer_idle_powerdown = true` (`wormhole.c:1083`) — idle power-down message is sent from delayed work armed at last-fd-close, honoring `idle_power_down_grace_ms` (default 5000, `module.c:56`).

`struct wormhole_device` (`wormhole.h:10-24`) embeds `struct tenstorrent_device tt` as first member; `tt_dev_to_wh_dev()` is `container_of` (`wormhole.h:26-27`). Fields: `kernel_tlb_mutex`, `bar2_mapping`, `bar4_mapping`, `saved_mps` (u8), `fw_ready_work` (delayed work), `telemetry_retries` (int), and two bool `*_group_registered` flags.

---

### 2. BAR layout and register map

#### BARs

| BAR | Role | Mapped in kernel? | Cite |
|-----|------|-------------------|------|
| BAR0 | TLB windows (userspace NOC apertures) | No (mmap'd by userspace) | `wormhole.c:23` (`TLB_1M_WINDOW_BASE 0 // BAR0`), `describe_tlb` sets `desc->bar = 0` (`wormhole.c:908`) |
| BAR2 | iATU registers | Yes, `bar2_mapping = pci_iomap(pdev, 2, 0)` | `wormhole.c:696-697`, `IATU_BASE 0x1200` relative to BAR2 (`wormhole.c:46`) |
| BAR4 | 32MB window onto system registers `0x1E000000..0x20000000` | Yes, `bar4_mapping = pci_iomap(pdev, 4, 0)` | `wormhole.c:699-700`, `BAR4_SOC_TARGET_ADDRESS 0x1E000000` (`wormhole.c:76`) |

BAR4 is directed to system registers by programming iATU **inbound region 1** as a BAR-match remap to SoC address `0x1E000000` (`map_bar4_to_system_registers`, `wormhole.c:449-458`). This is done in `init_hardware` (`wormhole.c:721`).

BAR0 total span (computed, not an explicit constant): 156×1M + 10×2M + 20×16M = 496 MB. TLB window count `TLB_WINDOW_COUNT = 186` (`wormhole.c:36`).

#### System-register offsets within BAR4 (all relative to `BAR4_SOC_TARGET_ADDRESS = 0x1E000000`)

| Symbol | SoC address | BAR4 offset | Cite |
|--------|-------------|-------------|------|
| `RESET_UNIT_START` | `0x1FF30000` | `0x01F30000` | `wormhole.c:78` |
| `ARC_CSM_START` | `0x1FE80000` | `0x01E80000` | `wormhole.c:79` |
| `TLB_REGS_START` | `0x1FC00000` | `0x01C00000` | `wormhole.c:80` |
| `NOC2AXI_START` | `0x1FD02000` | `0x01D02000` | `wormhole.c:81` |
| `KERNEL_TLB_START` | `0x1E000000` | `0x00000000` | `wormhole.c:88` |

Derived pointers into the reset unit:

| Symbol | Offset | Cite |
|--------|--------|------|
| `ARC_TELEMETRY_PTR` | `RESET_UNIT_START + 0x01D0` | `wormhole.c:83` |
| `ARC_TELEMETRY_DATA` | `RESET_UNIT_START + 0x01D4` | `wormhole.c:84` |
| `ARC_MSG_QCB_PTR` | `RESET_UNIT_START + 0x01D8` | `wormhole.c:119` |

`reset_unit_regs(wh_dev)` returns `bar4_mapping + RESET_UNIT_START` (`wormhole.c:460-462`).

#### Scratch registers (byte offset within the reset unit)

`SCRATCH_REG(n) = 0x60 + n*4` (`wormhole.c:90`):

| Register | Offset | Purpose | Cite |
|----------|--------|---------|------|
| SR0 = `POST_CODE_REG` | `0x60` | ARC L2 running check | `wormhole.c:101` |
| SR3 = args_reg | `0x6C` | ARC message args (arg0\|arg1<<16) | `wormhole.c:208` |
| SR5 = message_reg | `0x74` | ARC message id / completion | `wormhole.c:209` |
| SR6 = `PCIE_ARMISC_INFO_REG` | `0x78` | DBI-enable (read side) / hardware-hung sentinel | `wormhole.c:98`, `137` |
| SR7 = `PCIE_AWMISC_INFO_REG` | `0x7C` | DBI-enable (write side) | `wormhole.c:99` |

`ARC_MISC_CNTL_REG = 0x100`, IRQ0 trigger bit `ARC_MISC_CNTL_IRQ0_MASK = (1 << 16)` (`wormhole.c:105-106`).

> **Porting note:** `SCRATCH_REG(6)` (0x78) is overloaded: it is both `PCIE_ARMISC_INFO_REG` (DBI routing) and the register read by `is_hardware_hung` to detect a dead link (`== 0xFFFFFFFF`). A Windows port must preserve this dual use. All these reads/writes are `ioread32`/`iowrite32` on the BAR4 kernel mapping — on Windows, MMIO via `READ_REGISTER_ULONG`/`WRITE_REGISTER_ULONG` on the mapped BAR4 with matching little-endian semantics.

---

### 3. ARC firmware messaging — scratch-register protocol [WH-only]

This is the classic Wormhole ARC message path. **Blackhole does not use it** (Blackhole uses only the queue protocol in §4); a grep of `blackhole.c` shows no reference to `wormhole_send_arc_fw_message*`.

#### Protocol

Documented at `wormhole.c:108-112`: write `0xAA00 | message_id` into SR5, wait for `message_id` to appear in the low 16 bits. FW resets SR5 to 0 then writes back `message_id` (plus a 16-bit exit code in the high half) when done. `WH_FW_MESSAGE_PRESENT = 0xAA00` (`wormhole.c:112`).

`wormhole_send_arc_fw_message_with_args` (`wormhole.c:205-234`, **exported** via `wormhole.h:29-30`):

1. Bail if ARC L2 not running: `arc_l2_is_running()` checks `(POST_CODE & 0xFFFF0000) == 0xC0DE0000` (`wormhole.c:199-203`, constants `POST_CODE_ARC_L2 0xC0DE0000`, `POST_CODE_ARC_L2_MASK 0xFFFF0000` at `wormhole.c:102-103`). On failure logs and returns `false` (`wormhole.c:214-217`).
2. `iowrite32(arg0 | (arg1<<16), SR3)` then `iowrite32(0xAA00 | message_id, SR5)` (`wormhole.c:219-220`).
3. Trigger IRQ to ARC: read-modify-write `ARC_MISC_CNTL_REG |= (1<<16)` (`wormhole.c:222-224`).
4. If `timeout_us == 0`, return `false` immediately (fire-and-forget; note caller must not treat this as an error) (`wormhole.c:226-227`).
5. Otherwise poll via `arc_msg_poll_completion` (`wormhole.c:229-233`).

`arc_msg_poll_completion` (`wormhole.c:163-197`):
- Poll period `= max(10, timeout_us/100)` µs (~100 polls) (`wormhole.c:167`).
- Match when `(read_val & 0xffff) == msg_code`; sets `*exit_code = read_val >> 16`; returns **0** (`wormhole.c:174-178`).
- `read_val == 0xFFFFFFFF` **and** hung → return **-3** (`wormhole.c:180-183`).
- `read_val == 0xFFFFFFFF` (not hung) → unrecognized message, return **-2** (`wormhole.c:185-188`).
- Deadline exceeded → return **-1** (`wormhole.c:190-193`).
- Sleeps `usleep_range(poll_period, 2*poll_period)` between polls (`wormhole.c:195`).

`is_hardware_hung` (`wormhole.c:129-138`): returns true if config-space vendor read fails or `!= 0x1E52`, **or** `ioread32(reset_unit_regs + SCRATCH_REG(6)) == 0xFFFFFFFF`.

`wormhole_send_arc_fw_message` (`wormhole.c:236-240`) is the args-less wrapper (arg0=arg1=0).

#### Scratch-protocol message IDs

| Message | ID | Cite |
|---------|----|----|
| `WH_FW_MSG_PCIE_INDEX` | `0x51` | `wormhole.c:39` |
| `WH_FW_MSG_TRIGGER_RESET` | `0x56` | `wormhole.c:42` |
| `WH_FW_MSG_NOP` | `0x11` | `wormhole.c:43` |
| `WH_FW_MSG_ASTATE0` | `0xA0` | `wormhole.c:40,114` (defined twice, same value) |
| `WH_FW_MSG_ASTATE3` | `0xA3` | `wormhole.c:115` |
| `WH_FW_MSG_UPDATE_M3_AUTO_RESET_TIMEOUT` | `0xBC` | `wormhole.c:41` |
| `WH_FW_MSG_CURR_DATE` | `0xB7` | `wormhole.c:116` |
| `FW_MSG_PCIE_RETRAIN` | `0xB6` | `pcie.c:16` |

---

### 4. ARC firmware messaging — queue protocol [SHARED with Blackhole]

Used by Wormhole **only for power settings** (`send_arc_message`, `wormhole.c:242-291`), and by Blackhole for all messaging. Implemented in `msgqueue.c`/`msgqueue.h`.

#### Message format (shared)

```c
struct arc_msg {
	u32 header;
	u32 payload[7];
};              // 32 bytes (8 words)
```
(`msgqueue.h:11-14`) — `ARC_MSG_QUEUE_HEADER_SIZE 32`, `ARC_MSG_TIMEOUT_MS 1000` (`msgqueue.h:16-17`).

Queue-control-block (QCB) ring pointers, relative to `queue_base` (`msgqueue.h:19-22`):

| Pointer | Offset |
|---------|--------|
| `ARC_MSG_QUEUE_REQ_WPTR` | `+0x00` |
| `ARC_MSG_QUEUE_RES_RPTR` | `+0x04` |
| `ARC_MSG_QUEUE_REQ_RPTR` | `+0x10` |
| `ARC_MSG_QUEUE_RES_WPTR` | `+0x14` |

`arc_msg_push` (`msgqueue.c:12-70`): reads WPTR/RPTR through the class `csm_read32`/`csm_write32` callbacks; treats an all-`0xFFFFFFFF` read as "device gone" and fails (`msgqueue.c:25-28,38-41`). Occupancy `= (wptr - rptr) % (2*num_entries)`; waits (100–200 µs sleeps, 1000 ms timeout) until `< num_entries` (`msgqueue.c:43-53`). Writes 8 words into slot `wptr % num_entries` at `request_base = queue_base + 32` (`msgqueue.c:55-63`); advances `wptr = (wptr+1) % (2*num_entries)` (`msgqueue.c:65-67`).

`arc_msg_pop` (`msgqueue.c:72-132`): `response_base = queue_base + 32 + num_entries*32` (`msgqueue.c:75`); waits until occupancy `> 0`; reads header + 7 payload words; advances RES_RPTR.

#### Wormhole `send_arc_message` wrapper [WH-only wiring; calls SHARED core]

`send_arc_message` (`wormhole.c:242-291`, `__maybe_unused`):
1. Spin (up to `ARC_MSG_READY_MS = 500`, `wormhole.c:120`) until `arc_l2_is_running` (`wormhole.c:252-260`).
2. Read QCB pointer from `ARC_MSG_QCB_PTR` (`wormhole.c:263`). **`qcb_ptr == 0` means old FW without queue support → `dev_warn_once` and return false** (`wormhole.c:264-267`).
3. Validate `is_range_within_csm(qcb_ptr, 4)` (`wormhole.c:268-269`).
4. `csm_read32(qcb_ptr+0)` → `queue_base`, `csm_read32(qcb_ptr+4)` → `queue_info` (`wormhole.c:271-275`). `queue_base += ARC_CSM_BASE`; `num_entries = queue_info & 0xFF` (`wormhole.c:277-278`).
5. `arc_msg_push`, trigger IRQ0 (`ARC_MISC_CNTL_REG |= 1<<16`), `arc_msg_pop` (`wormhole.c:280-288`).
6. Success == `msg->header == 0` (`wormhole.c:290`).

#### CSM access primitives (used by the queue protocol) [WH-only address translation]

`wh_arc_addr_to_sysreg(arc_addr) = ARC_CSM_START + (arc_addr - ARC_CSM_BASE)` (`wormhole.c:140-143`). `ARC_CSM_BASE = 0x10000000`, `ARC_CSM_SIZE = (1<<19)` (`telemetry.h:73-74`).

`csm_read32`/`csm_write32` (`wormhole.c:145-161`): validate via `is_range_within_csm(addr, 4)`, return **-EINVAL** on failure; otherwise `ioread32`/`iowrite32` at `bar4_mapping + wh_arc_addr_to_sysreg(addr)`. Exposed as class ops `wormhole_csm_read32`/`wormhole_csm_write32` (`wormhole.c:1024-1032`).

`is_range_within_csm(addr,len)` = `addr >= 0x10000000 && addr <= (0x10000000 + 0x80000) - len` (`telemetry.h:75-78`). **[SHARED]** — same check used by Blackhole.

> **Porting note:** The queue protocol reaches the CSM via the class `csm_read32`/`csm_write32` callbacks. Wormhole translates the ARC CSM address into a BAR4 sysreg offset (`wh_arc_addr_to_sysreg`); Blackhole (per `device.h:76-79`) stores/uses **raw CSM addresses** for NOC reads. The `msgqueue.c` core is address-agnostic and portable as-is; only the per-arch `csm_read32`/`csm_write32` differ.

---

### 5. TLB windows and NOC access

#### Geometry (`wormhole.c:20-36`)

| Kind | Count | Shift | Window size | Base (in BAR0) |
|------|-------|-------|-------------|----------------|
| 1M | 156 (`TLB_1M_WINDOW_COUNT`) | 20 | `0x100000` | `0` |
| 2M | 10 (`TLB_2M_WINDOW_COUNT`) | 21 | `0x200000` | `156 * 1M` |
| 16M | 20 (`TLB_16M_WINDOW_COUNT`) | 24 | `0x1000000` | `2M base + 10*2M` |

`TLB_WINDOW_COUNT = 186`; `WH_NOC_BITS = 36` (`wormhole.c:36-37`). `NUM_TLB_KINDS = 3` (`wormhole.c:803`). Index/shift/size/base lookup arrays at `wormhole.c:804-807`.

`wormhole_tlb_kind(tlb)` maps a TLB index to kind 0/1/2 or `-EINVAL` (`wormhole.c:825-838`).

#### TLB config register encoding [WH-only]

Non-address control bits (`struct noc_tlb_non_address_bits`, `wormhole.c:809-823`):

```c
union { u32 reg; struct {
	u64 x_end: 6; u64 y_end: 6; u64 x_start: 6; u64 y_start: 6;
	u64 noc_sel : 1; u64 mcast: 1; u64 ordering: 2; u64 linked: 1;
}; };   // 29 bits used, packed into the low 32 bits (union with u32 reg)
```

`construct_tlb_config` (`wormhole.c:840-871`) validates:
- Bad kind → **-EINVAL** (`wormhole.c:855-856`).
- `config->addr & (window_size-1)` (unaligned) → **-EINVAL** (`wormhole.c:858-860`).
- `config->addr >= (1UL << 36)` → **-EINVAL** (`wormhole.c:862-864`).

Register value (`wormhole.c:866-868`):
```c
*regs  = (u64)config->addr >> TLB_SHIFTS[kind];                       // low bits = address page
*regs |= (u64)non_address_bits.reg << (WH_NOC_BITS - TLB_SHIFTS[kind]); // NOC bits above
```

`wh_configure_tlb` (`wormhole.c:873-891`): range-checks `tlb ∈ [0, 186)` (**-EINVAL**), builds regs, writes the 64-bit value as two 32-bit stores at `TLB_REGS_START + tlb*2*4` (low word first) (`wormhole.c:886-888`). Public op `wormhole_configure_tlb` (`wormhole.c:893-898`).

`wormhole_describe_tlb` (`wormhole.c:900-915`): fills `desc->bar = 0`, `desc->size = window_size`, `desc->bar_offset = base + size*(tlb - kind_first_index)`; **-EINVAL** on bad kind. TLB windows are **BAR0-relative** for userspace mmap.

> **Porting note:** The `u64`-typed bitfields inside a `union` with `u32 reg` are compiler-layout-dependent. On MSVC/Windows the port must reproduce the exact bit packing (x_end at bit 0, …, linked at bit 28) rather than rely on struct-bitfield ABI. Prefer explicit shift/mask when porting.

#### Kernel TLB and NOC read/write [WH-only]

`KERNEL_TLB_INDEX = TLB_WINDOW_COUNT - 1 = 185` — the **last 16M window**, reserved by `set_bit(KERNEL_TLB_INDEX, tt_dev->tlbs)` in `wormhole_init` (`wormhole.c:702`) so userspace cannot allocate it. Its data aperture is `bar4_mapping + KERNEL_TLB_START (=0) + offset` (`wormhole.c:88, 928`).

`wh_configure_kernel_tlb(wh, x, y, addr, noc)` (`wormhole.c:917-929`): splits `addr` at the 16M mask (`TLB_16M_WINDOW_MASK = 16M-1`, `wormhole.c:34`); programs index 185 with `x_end=x, y_end=y, ordering=1 (strict), noc=noc`; returns the window pointer at `offset = addr & mask`.

`noc_read32` / `noc_write32` (`wormhole.c:931-954`): take `kernel_tlb_mutex`, configure the kernel TLB, do a single `ioread32`/`iowrite32`, release the mutex. `wormhole_noc_write32` is the class op (`wormhole.c:1018-1022`); **no `noc_read32` class op is exported** (used internally only, e.g. save/restore reset state).

> **Porting note:** `kernel_tlb_mutex` serializes *all* kernel NOC access because there is one shared kernel TLB (index 185). Every `noc_read32`/`noc_write32`, and thus `save_reset_state`/`restore_reset_state`, contends on it. On Windows use a fast mutex / passive-level lock; these paths run at PASSIVE_LEVEL (they sleep in the ARC helpers).

---

### 6. iATU (inbound BAR remap + outbound DMA) [WH-only register layout]

iATU register block at `IATU_BASE = 0x1200` relative to BAR2 (`wormhole.c:46`). Region stride `0x100`, 16 outbound regions (`wormhole.c:49-50`). Per-direction field offsets at `wormhole.c:51-61`. `write_iatu_reg` computes `offset = IATU_BASE + (2*region + direction)*0x100 + reg` and writes to `bar2_mapping` (`wormhole.c:362-368`); `WRITE_IATU_REG` macro at `wormhole.c:123-125`.

Inbound control-2 fields (`wormhole.c:63-67`): `REGION_EN (1<<31)`, `BAR_MATCH_MODE (1<<30)`, `FUZZY_TYPE_MATCH (1<<27)`, `BAR_NUM(n) ((n)<<8)`.
Outbound control-2 fields (`wormhole.c:69-72`): `DMA_BYPASS (1<<27)`, `TLP_BYPASS (1<<21)`, `FUNC_BYPASS (1<<19)`.

`map_bar4_to_system_registers` (`wormhole.c:449-458`): inbound region 1, target `0x1E000000`, ctrl2 = `REGION_EN | BAR_MATCH_MODE | FUZZY_TYPE_MATCH | BAR_NUM(4)`.

`wormhole_configure_outbound_atu(region, base, limit, target)` (`wormhole.c:990-1016`), class op `.configure_outbound_atu`:
- `region >= 16` → **-EINVAL** (`wormhole.c:1001-1002`).
- `limit > U32_MAX` → **-EINVAL** (`wormhole.c:1004-1005`).
- ctrl1 = 0 (MEM type); ctrl2 = 0 when `limit==0` (disable) else `REGION_EN | DMA_BYPASS | TLP_BYPASS | FUNC_BYPASS` (`wormhole.c:994-995`).
- Writes lower/upper base, lower/upper target, limit, ctrl1, ctrl2 in that order (`wormhole.c:1007-1013`).

---

### 7. Telemetry [tag table WH-specific; cache/tags SHARED semantics]

#### Discovery

`ARC_TELEMETRY_PTR` holds a CSM address of the telemetry table base; `ARC_TELEMETRY_DATA` holds the CSM address of the data block (`wormhole.c:83-84`).

`telemetry_probe` (`wormhole.c:536-584`):
1. Zero the tag cache (`wormhole.c:545`).
2. Read `base_addr` and `data_addr` (raw `ioread32` of the two pointer regs) (`wormhole.c:547-548`); `tags_addr = base_addr + 8`.
3. If either is **not** `is_range_within_csm` → log "Telemetry not available", return **-ENODEV** (`wormhole.c:551-554`).
4. Read version at `base_addr` (via `wh_arc_addr_to_sysreg`): `major=(v>>16)&0xFF, minor=(v>>8)&0xFF, patch=v&0xFF`. `major > 1` → **-ENOTSUPP** (`wormhole.c:556-564`).
5. `num_entries = ioread32(base_addr + 4)` (`wormhole.c:566`).
6. For each entry at `tags_addr + i*4`: `tag_id = entry & 0xFFFF`, `offset = (entry>>16)&0xFFFF`, `addr = data_addr + offset*4`. Skip (with error log) if `addr` not in CSM. If `tag_id < 128` cache `telemetry_tag_cache[tag_id] = wh_arc_addr_to_sysreg(addr)` — i.e. **a BAR4 offset** (`wormhole.c:568-581`).

`wormhole_read_telemetry_tag(tag_id, *value)` (`wormhole.c:520-534`), class op `.read_telemetry_tag`:
- `tag_id >= TELEM_TAG_CACHE_SIZE (128)` → **-EINVAL** (`wormhole.c:525-526`).
- cached offset `== 0` → **-ENODATA** (`wormhole.c:529-530`).
- else `*value = ioread32(bar4_mapping + offset)` (`wormhole.c:532`).

> **Porting note:** WH stores a **BAR4 sysreg offset** in `telemetry_tag_cache` and reads telemetry via a direct MMIO load (`bar4_mapping + offset`). Blackhole stores a **raw CSM address** and reads via NOC (`device.h:76-79`). The tag enum (`telemetry.h:15-41`) and `TELEM_TAG_CACHE_SIZE = 128` are shared, but the read mechanism is per-arch.

#### Deferred FW-ready polling [WH-only]

Because ARC FW may not be ready when the driver probes/resets, WH defers telemetry setup to a delayed work item:

`is_fw_ready_for_telemetry` (`wormhole.c:640-647`): both telemetry pointers must be in CSM.

`fw_ready_work_func` (`wormhole.c:654-681`): if `tt_dev->detached` return; if FW not ready, reschedule after 1000 ms while `telemetry_retries-- > 0`, else log timeout; once ready, `telemetry_probe`, then first-time register the sysfs telemetry group and hwmon (guarded by `telemetry_group_registered` / `hwmon_dev`).

`wormhole_probe_telemetry` (`wormhole.c:739-746`), class op `.probe_telemetry`: sets `telemetry_retries = 120` and schedules the work immediately (delay 0). Returns 0. (120 retries × 1 s ⇒ ~2 min budget.)

`wormhole_init_telemetry` (`wormhole.c:748-764`), class op `.init_telemetry`: registers the PCIe perf-counters sysfs group (sets `pcie_perf_group_registered`), then calls `wormhole_probe_telemetry`.

`wormhole_cleanup_telemetry` (`wormhole.c:766-784`), class op `.cleanup_telemetry`: unregister hwmon, telemetry group, and PCIe perf group (each guarded by its flag).

> **Porting note:** The remove path (`enumerate.c:410-413`) special-cases Wormhole to `cancel_delayed_work_sync(&wh->fw_ready_work)` **before** the generic teardown. A Windows port must have an equivalent cancel-and-flush of the FW-ready polling timer/DPC before unmapping BAR4, or a late poll will touch freed MMIO.

#### sysfs telemetry attributes

`wh_sysfs_attributes[]` (`wormhole.c:370-384`) maps 13 tags (aiclk, axiclk, arcclk, board serial, card type, fw bundle ver, m3app/m3bl/arc/eth fw ver, ttflash ver, asic id, heartbeat) to sysfs files via shared show callbacks (`tt_sysfs_show_*`, `telemetry.h:52-56`).

#### hwmon

`wormhole_hwmon_init` (`wormhole.c:619-638`) registers an hwmon device named `"wormhole"` with `wh_hwmon_chip_info` (`wormhole.c:614-617`) and emits a `KOBJ_CHANGE` uevent. Tag→sensor mapping in `wh_hwmon_attrs` (`wormhole.c:586-596`): ASIC_TEMP→temp_input, THM_LIMIT_THROTTLE→temp_max, VCORE→in_input, VDD_LIMITS→in_max, CURRENT→curr_input, TDC_LIMIT_MAX→curr_max, POWER→power_input, TDP_LIMIT_MAX→power_max. Labels at `wormhole.c:598-604`.

#### PCIe performance counters [WH-only]

NIU counters at `NIU_COUNTERS_START = NOC2AXI_START + 0x200` (`wormhole.c:386`); NOC1 bank offset `NIU_NOC1_OFFSET = 0x8000` (`wormhole.c:387`). `wh_show_pcie_single_counter` reads `ioread32(bar4 + NIU_COUNTERS_START + 4*counter_offset + noc*0x8000)` (`wormhole.c:389-397`). Counter type IDs (`wormhole.c:412-417`): `SLV_POSTED_WR_DATA_WORD_RECEIVED 0x39`, `SLV_NONPOSTED_WR_DATA_WORD_RECEIVED 0x38`, `SLV_RD_DATA_WORD_SENT 0x33`, `MST_POSTED_WR_DATA_WORD_SENT 0x9`, `MST_NONPOSTED_WR_DATA_WORD_SENT 0x8`, `MST_RD_DATA_WORD_RECEIVED 0x3`. Exposed per-NOC (suffix 0/1) in sysfs group `pcie_perf_counters` (`wormhole.c:400-447`).

---

### 8. Init / cleanup lifecycle

`wormhole_init` (`wormhole.c:683-716`), class op `.init_device`:
- `INIT_DELAYED_WORK(&fw_ready_work, fw_ready_work_func)`.
- Alloc `telemetry_attrs` array (`+1` for NULL terminator); return false (fail) if NULL.
- `pci_iomap` BAR2 then BAR4; failure unwinds (`fail_bar4` iounmaps BAR2, `fail_bar2` returns false) (`wormhole.c:696-715`).
- `set_bit(KERNEL_TLB_INDEX, tt_dev->tlbs)`; `mutex_init(&kernel_tlb_mutex)`.
- Build telemetry attr group with visibility `tt_sysfs_telemetry_is_visible`.

`wormhole_init_hardware` (`wormhole.c:718-735`), class op `.init_hardware`:
1. `map_bar4_to_system_registers` (always).
2. **Only if `arc_l2_is_running`:** send `WH_FW_MSG_CURR_DATE` (via `wormhole_send_curr_date`), `WH_FW_MSG_ASTATE0` (10 ms), `update_device_index`, `wormhole_complete_pcie_init`, and `WH_FW_MSG_UPDATE_M3_AUTO_RESET_TIMEOUT` with `auto_reset_timeout` (`wormhole.c:723-732`).

`update_device_index` (`wormhole.c:464-471`): sends `WH_FW_MSG_PCIE_INDEX` with `arg0 = ordinal | 0x80` (`INDEX_VALID`), 10 ms timeout (`10*1000` µs, `wormhole.c:470`).

`wormhole_send_curr_date` (`wormhole.c:321-360`) + `month_lookup` (`wormhole.c:303-319`): packs current UTC date/time (seconds since 2020-01-01, leap handling) into two 16-bit args and sends `WH_FW_MSG_CURR_DATE` (1 ms timeout). Purely informational to FW.

`wormhole_cleanup_hardware` (`wormhole.c:786-791`), class op `.cleanup_hardware` **and** `.reboot`: if `!detached`, call `wormhole_shutdown_firmware`.
`wormhole_shutdown_firmware` (`wormhole.c:293-301`): if hung, return false; else send `WH_FW_MSG_ASTATE3` (10 ms) — puts ARC into A3 low-power state.

`wormhole_cleanup` (`wormhole.c:793-801`), class op `.cleanup_device`: `pci_iounmap` BAR2 and BAR4 (each guarded non-NULL).

Removal order (`enumerate.c:404-446`): cancel `fw_ready_work` (WH-only) → set `detached` under `chardev_mutex` → cancel `power_down_work` → if vendor id readable, `cleanup_hardware` (A3) → `cleanup_telemetry` → `cleanup_device` (unmap BARs).

---

### 9. Reset

`wormhole_reset(tt_dev, reset_flag)` (`wormhole.c:473-518`), class op `.reset`:
- `reset_arg = 3` iff `reset_flag == TENSTORRENT_RESET_DEVICE_ASIC_DMC_RESET (5)` else `0` (`wormhole.c:477`; flag values `ioctl.h:143-151`).
- Probe responsiveness with `WH_FW_MSG_NOP` (1 ms; `1000` µs) (`wormhole.c:481`).
- If unresponsive: if `auto_reset_timeout == 0` (watchdog disabled), log and **return false** (`wormhole.c:488-491`). Else loop for `auto_reset_timeout*1000 + 500` ms, each iteration `pcie_hot_reset_and_restore_state` then retry NOP; between tries `msleep_interruptible(1000)` (signal → return false) (`wormhole.c:493-505`).
- If responsive: `set_reset_marker(pdev)`, then `WH_FW_MSG_TRIGGER_RESET` with `arg0=reset_arg` and **`timeout_us=0`** (fire-and-forget), return **true** (assumes success) (`wormhole.c:508-513`).
- Still unresponsive → log, return false (`wormhole.c:516-517`).

`auto_reset_timeout` default 10 s (`module.c:48`).

#### Save/restore reset state (MPS preservation) [WH-only mechanism]

`.save_reset_state` = `wormhole_save_reset_state` (`wormhole.c:968-976`), `.restore_reset_state` = `wormhole_restore_reset_state` (`wormhole.c:978-988`):
- `open_dbi` writes `DBI_ENABLE = 0x00200000` to SR6 and SR7 (routes all outbound NOC traffic to DBI) (`wormhole.c:958-961`); `close_dbi` writes 0 back (`wormhole.c:963-966`).
- Read PCIe DEVICE_CONTROL via `noc_read32(PCIE_NOC_X=0, PCIE_NOC_Y=3, PCIE_DBI_ADDR + DBI_DEVICE_CONTROL_DEVICE_STATUS, noc=0)`; `PCIE_DBI_ADDR = 0x800000000`, `DBI_DEVICE_CONTROL_DEVICE_STATUS = 0x78` (`wormhole.c:92`, `pcie.h:8`).
- Save: extract `PCI_EXP_DEVCTL_PAYLOAD` field into `wh->saved_mps` (`wormhole.c:973-974`).
- Restore: clear the payload field and write back `saved_mps` via `noc_write32` (`wormhole.c:983-986`).

> **Porting note:** `open_dbi` warns (`wormhole.c:956-957`) that DBI routing disrupts normal NOC DMA — it must only be invoked when there is no outbound traffic. The Windows port must gate these around quiesced-DMA points (reset), and note the `open_dbi`/`noc_read32`/`close_dbi` sequence holds `kernel_tlb_mutex` transitively via `noc_read32`.

#### Shared PCIe reset helpers [SHARED]

In `pcie.c` (generic, not WH-named except `wormhole_complete_pcie_init`):
- `safe_pci_restore_state` (`pcie.c:43-59`): test-reads vendor id first (guards a long capability walk), then `pci_restore_state` + `pci_save_state`.
- `pcie_hot_reset_and_restore_state` (`pcie.c:61-90`): asserts `PCI_BRIDGE_CTL_BUS_RESET` on the upstream bridge, `msleep(2)`, deassert, `msleep(500)`, then `poll_pcie_link_up(10000 ms)` + `safe_pci_restore_state`. Manages `ignore_hotplug`.
- `poll_pcie_link_up` (`pcie.c:24-41`): polls vendor id == `0x1E52` every 100 ms until timeout.
- `set_reset_marker` / `is_reset_marker_zero` (`pcie.c:140-158`): use the `PCI_COMMAND_PARITY` bit in config-space `PCI_COMMAND` as a reset marker (set before reset, expected cleared to 0 after).
- `pcie_timer_interrupt` (`pcie.c:133-138`): config-space writes to `INTERFACE_TIMER_TARGET_OFF 0x934` / `INTERFACE_TIMER_CONTROL_OFF 0x930`.

#### `wormhole_complete_pcie_init` [WH-only]

`pcie.c:92-131` (declared in `pcie.h:11`). Called from `wormhole_init_hardware`. Loops up to `reset_limit` (default 10, `module.c:44`) times:
- Reads bridge target link speed (`PCI_EXP_LNKCTL2 & PCI_EXP_LNKCTL2_TLS`) and subsystem vendor id.
- Sends `FW_MSG_PCIE_RETRAIN (0xB6)` with `arg0 = target_link_speed | (last_retry<<15)`, `arg1 = subsys_vendor_id`, 200 ms timeout, capturing `exit_code`.
- `exit_code == 0` → success. Otherwise (unless last retry) `pci_save_state` + `pcie_hot_reset_and_restore_state` and try again. Uses `FW_MSG_PCIE_RETRAIN` (`pcie.c:16`); the interface-timer/force-pending constants (`pcie.c:17-22`) belong to `pcie_timer_interrupt`, not this function.

---

### 10. Power state [WH wrapper; queue core SHARED]

`wormhole_set_power_state(tt_dev, power_state)` (`wormhole.c:1034-1053`), class op `.set_power_state`:
- `msg.header = ARC_MSG_TYPE_POWER_SETTING (0xC0) | (validity << 8) | (power_flags << 16)` (`wormhole.c:1040`; constant `wormhole.c:121`).
- `BUILD_BUG_ON(sizeof(power_settings) != sizeof(msg.payload))` — the 14× `u16` `power_settings` array (`ioctl.h:410`) exactly fills the 7× `u32` payload (28 bytes) (`wormhole.c:1041-1042`).
- `send_arc_message` (queue protocol, §4); failure → **-EINVAL** (`wormhole.c:1047-1052`).

`validity` bitfield semantics and `TT_POWER_FLAG_*` bits at `ioctl.h:388-410`.

---

### Key constants table

| Name | Value | Source cite |
|------|-------|-------------|
| PCI vendor / Wormhole device | `0x1E52` / `0x401E` | `enumerate.h:15,17` |
| PCI Grayskull device (deprecated, NULL class) | `0xFACA` | `enumerate.h:16`, `module.c:65-66` |
| `dma_address_bits` | `32` | `wormhole.c:1058` |
| `noc_dma_limit` | `0xFFFE0000 - 1` | `wormhole.c:1059` |
| `noc_pcie_offset` / `PCIE_DBI_ADDR` | `0x800000000` | `wormhole.c:1060, 92` |
| `BAR4_SOC_TARGET_ADDRESS` | `0x1E000000` | `wormhole.c:76` |
| `RESET_UNIT_START` (BAR4 off) | `0x01F30000` | `wormhole.c:78` |
| `ARC_CSM_START` (BAR4 off) | `0x01E80000` | `wormhole.c:79` |
| `TLB_REGS_START` (BAR4 off) | `0x01C00000` | `wormhole.c:80` |
| `NOC2AXI_START` (BAR4 off) | `0x01D02000` | `wormhole.c:81` |
| `KERNEL_TLB_START` (BAR4 off) | `0x0` | `wormhole.c:88` |
| `ARC_TELEMETRY_PTR/DATA/QCB` | RESET_UNIT + `0x1D0/0x1D4/0x1D8` | `wormhole.c:83,84,119` |
| `SCRATCH_REG(n)` | `0x60 + 4n` | `wormhole.c:90` |
| `ARC_MISC_CNTL_REG` / IRQ0 bit | `0x100` / `(1<<16)` | `wormhole.c:105-106` |
| `POST_CODE_ARC_L2` / mask | `0xC0DE0000` / `0xFFFF0000` | `wormhole.c:102-103` |
| `WH_FW_MESSAGE_PRESENT` | `0xAA00` | `wormhole.c:112` |
| `DBI_ENABLE` | `0x00200000` | `wormhole.c:97` |
| `DBI_DEVICE_CONTROL_DEVICE_STATUS` | `0x78` | `pcie.h:8` |
| `PCIE_NOC_X / Y` | `0` / `3` | `wormhole.c:93-94` |
| `ARC_CSM_BASE` / `ARC_CSM_SIZE` | `0x10000000` / `(1<<19)` | `telemetry.h:73-74` |
| TLB kinds counts | `156 / 10 / 20` | `wormhole.c:20,25,30` |
| TLB shifts | `20 / 21 / 24` | `wormhole.c:21,26,31` |
| `TLB_WINDOW_COUNT` / `KERNEL_TLB_INDEX` | `186` / `185` | `wormhole.c:36, 87` |
| `WH_NOC_BITS` | `36` | `wormhole.c:37` |
| `IATU_BASE` (BAR2 rel) / stride / regions | `0x1200` / `0x100` / `16` | `wormhole.c:46,50,49` |
| `ARC_MSG_QUEUE_HEADER_SIZE` | `32` | `msgqueue.h:16` |
| `ARC_MSG_TIMEOUT_MS` | `1000` | `msgqueue.h:17` |
| `ARC_MSG_READY_MS` | `500` | `wormhole.c:120` |
| `ARC_MSG_TYPE_POWER_SETTING` | `0xC0` | `wormhole.c:121` |
| Scratch msg IDs | NOP `0x11`, PCIE_INDEX `0x51`, TRIGGER_RESET `0x56`, ASTATE0 `0xA0`, ASTATE3 `0xA3`, PCIE_RETRAIN `0xB6`, CURR_DATE `0xB7`, M3_TIMEOUT `0xBC` | `wormhole.c:39-43,114-116`; `pcie.c:16` |
| `TELEM_TAG_CACHE_SIZE` | `128` | `telemetry.h:13` |
| `auto_reset_timeout` default | `10` (s) | `module.c:48` |
| `reset_limit` default | `10` | `module.c:44` |
| `idle_power_down_grace_ms` default | `5000` | `module.c:56` |
| poll_completion codes | 0 ok, -3 hung, -2 unrecognized, -1 timeout | `wormhole.c:177,182,187,192` |

---

### Open questions

1. **Kernel-TLB BAR4 aperture geometry.** `KERNEL_TLB_START` computes to BAR4 offset `0` (`wormhole.c:88`), and the kernel accesses window index 185 through `bar4_mapping + 0 + offset` while userspace sees the same window as BAR0-relative (`describe_tlb` `bar=0`). The exact hardware relationship between the BAR4 system-register aperture (`0x1E000000`) and the BAR0 kernel TLB window is not spelled out in the code; I documented only the code-visible addressing. A Windows port that reuses index 185 for kernel NOC access must confirm this aperture with hardware docs.
2. **`noc_tlb_non_address_bits` bitfield ABI.** The struct uses `u64`-typed bitfields inside a `union` with a `u32 reg` (`wormhole.c:809-823`). The intended packing (29 bits into the low 32) works under GCC but is not guaranteed identical under MSVC. Exact bit positions should be validated against hardware before relying on struct layout in the port.
3. **`WH_FW_MSG_ASTATE0` duplicate define.** Defined twice with identical value `0xA0` (`wormhole.c:40` and `114`); harmless but worth noting so a port does not treat them as distinct.
4. **`timeout_us == 0` return convention.** `wormhole_send_arc_fw_message_with_args` returns `false` for fire-and-forget sends (`wormhole.c:226-227`), and `wormhole_reset` deliberately uses this for `TRIGGER_RESET` yet returns `true` (`wormhole.c:510-512`). A port must not treat the `false` from a zero-timeout send as an error.
5. **`send_arc_message` is `__maybe_unused` on Wormhole (`wormhole.c:242`)** — it is reachable only through `wormhole_set_power_state`. If the Windows port omits power-state support initially, the entire queue path (and `msgqueue.c`) is dormant for Wormhole but still required for Blackhole.

---

## 09. ARC Firmware Message Queue Protocol

### Scope

Files covered (full reads unless noted):

- `msgqueue.c` (132 lines) — the ring-buffer push/pop engine.
- `msgqueue.h` (27 lines) — struct + header-offset macros + timeout constant.
- `blackhole.c` — every msgqueue call site: constants (lines 56-74), CSM accessors (270-296), `telemetry_probe` QCB-neighbor discovery (455-498), `send_arc_message` (500-540), `blackhole_reset` (542-570), `blackhole_init_hardware` (620-639), `blackhole_cleanup_hardware` (697-708), `blackhole_set_power_state` (796-811), class table (813-840). Supporting NOC/TLB helpers (200-268).
- `wormhole.c` — constants (74-121), CSM accessors + `wh_arc_addr_to_sysreg` (140-161), the *separate* scratch-register protocol `arc_msg_poll_completion` / `wormhole_send_arc_fw_message[_with_args]` (163-240), the queue-based `send_arc_message` (242-291), `wormhole_set_power_state` (1034-1053), class table (1055-1084).
- `telemetry.c` (232 lines) — **read in full; contains NO msgqueue call sites.** It reads telemetry via `read_telemetry_tag` (a CSM read on the tag cache), never through the ARC message queue. Included here only to record that fact.
- `telemetry.h:73-78` — `ARC_CSM_BASE`/`ARC_CSM_SIZE`/`is_range_within_csm` (the address gate shared by both arch CSM accessors).
- `device.h:110-111` — the `csm_read32`/`csm_write32` function-pointer contract.
- `ioctl.h:396-411` — `struct tenstorrent_power_state` (payload source for the POWER_SETTING message).

`msgqueue.c` is architecture-agnostic: it never touches hardware directly. All device I/O goes through the `csm_read32`/`csm_write32` ops installed by the arch class (`blackhole.c:837-838`, `wormhole.c:1080-1081`).

---

### 1. Data structures and in-memory layout

#### 1.1 The message struct — 32 bytes, fixed

```c
struct arc_msg {
	u32 header;
	u32 payload[7];
};
```
(`msgqueue.h:11-14`) → exactly 8×4 = **32 bytes**. This is both the request-entry size and the response-entry size; the code hardcodes `sizeof(struct arc_msg)` everywhere for slot stride (`msgqueue.c:56,75,116`).

#### 1.2 Queue header — 32 bytes, four pointer words

`ARC_MSG_QUEUE_HEADER_SIZE 32` (`msgqueue.h:16`). The four ring pointers live in this 32-byte header at these byte offsets relative to `queue_base` (`msgqueue.h:19-22`):

```c
#define ARC_MSG_QUEUE_REQ_WPTR(base) ((base) + 0x00)   // request  write ptr  — DRIVER writes
#define ARC_MSG_QUEUE_RES_RPTR(base) ((base) + 0x04)   // response read  ptr  — DRIVER writes
#define ARC_MSG_QUEUE_REQ_RPTR(base) ((base) + 0x10)   // request  read  ptr  — FW writes
#define ARC_MSG_QUEUE_RES_WPTR(base) ((base) + 0x14)   // response write ptr  — FW writes
```

Ownership split (who advances each pointer):
- **Driver owns** `REQ_WPTR` (0x00) and `RES_RPTR` (0x04) — the two "host side" indices, packed into the first 8 bytes.
- **Firmware owns** `REQ_RPTR` (0x10) and `RES_WPTR` (0x14) — the two "ARC side" indices, packed 16 bytes away.

Offsets 0x08, 0x0C, 0x18, 0x1C are inside the 32-byte header but are **never read or written by the driver** (reserved / FW-private). The 8-byte gap between the host pair (0x00/0x04) and the FW pair (starting 16 bytes from base at 0x10/0x14) reads as deliberate separation between the host and the ARC core.

#### 1.3 Full queue image in CSM

Given `queue_base` and `num_entries` (call it N):

```
queue_base + 0x00 .. +0x1F         : 32-byte header (4 live pointers + reserved)
request_base  = queue_base + 32                              (msgqueue.c:15)
  request ring : N entries × 32 bytes
response_base = queue_base + 32 + N*32                       (msgqueue.c:75)
  response ring: N entries × 32 bytes
```
Total footprint = `32 + 2*N*32` bytes. Request and response rings are **separate** rings that share the same slot count N and the same 32-byte stride.

> **Porting note:** The entire header is memory in device CSM (chip SRAM), not a driver-side struct. On Windows the equivalent is MMIO/NOC-window register reads at these byte offsets. Do NOT model the header as a C struct with natural alignment — offset 0x08/0x0C are holes. Honor the exact offsets above.

---

### 2. Ring index arithmetic and wraparound

Pointers are kept in the range **[0, 2N)** — i.e. counters run to *twice* the slot count, not N. This is the "extra range" trick that lets full and empty be distinguished without a separate flag.

- Slot index into a ring: `slot = ptr % num_entries` (`msgqueue.c:55,115`).
- Advance: `ptr = (ptr + 1) % (2 * num_entries)` (`msgqueue.c:65,127`).
- Occupancy: `num_occupied = (wptr - rptr) % (2 * num_entries)` (`msgqueue.c:43,103`).

Push waits until there is room:
```c
num_occupied = (wptr - rptr) % (2 * num_entries);
if (num_occupied < num_entries)      // not full → space available
	break;
```
(`msgqueue.c:43-45`)

Pop waits until something is present:
```c
num_occupied = (wptr - rptr) % (2 * num_entries);
if (num_occupied > 0)                // at least one response queued
	break;
```
(`msgqueue.c:103-105`)

The subtraction `(wptr - rptr)` is unsigned `u32`; when `wptr < rptr` (post-wrap) it wraps to `2^32 - (rptr - wptr)` and is then reduced `% (2*num_entries)`. This yields the correct occupancy **only when `2*num_entries` divides `2^32`, i.e. when `num_entries` is a power of two** (so that `2^32 mod 2N == 0`). See Open Questions — the code does not validate this.

> **Porting note:** Replicate this in unsigned 32-bit arithmetic exactly (wrap then modulo). Do not "simplify" to `if (wptr >= rptr) ... else ...` unless you have confirmed `num_entries` is always a power of two; the two are only equivalent under that assumption.

---

### 3. `arc_msg_push` — enqueue a request (`msgqueue.c:12-70`)

Signature: `bool arc_msg_push(tt_dev, const struct arc_msg *msg, u32 queue_base, u32 num_entries)`.

Step by step:

1. `request_base = queue_base + ARC_MSG_QUEUE_HEADER_SIZE` (`:15`).
2. Read `REQ_WPTR` via `cls->csm_read32(...ARC_MSG_QUEUE_REQ_WPTR(queue_base)...)`; **return false on any csm error** (`:22-23`).
3. **Device-gone guard:** `if (wptr == U32_MAX)` → `dev_err("ARC queue WPTR read returned all-1s; device gone?")`, return false (`:25-28`).
4. Arm timeout: `timeout = jiffies + msecs_to_jiffies(ARC_MSG_TIMEOUT_MS)` = now + **1000 ms** (`:30`).
5. Space-wait loop (`:31-53`):
   - Read `REQ_RPTR`; return false on csm error (`:35-36`).
   - **Device-gone guard** on rptr all-1s (`:38-41`).
   - Compute occupancy; break when `< num_entries` (`:43-45`).
   - `if (time_after(jiffies, timeout))` → `dev_err("Timeout waiting for space in ARC message queue")`, return false (`:47-50`).
   - `usleep_range(100, 200)` — sleep **100–200 µs** between polls (`:52`).
6. `slot = wptr % num_entries`; `req_offset = slot * sizeof(struct arc_msg)` (`:55-56`).
7. Write all 8 words of the entry (`:57-63`):
   ```c
   for (i = 0; i < 8; ++i) {
       u32 addr = request_base + req_offset + (i * sizeof(u32));
       u32 value = (i == 0) ? msg->header : msg->payload[i - 1];
       if (cls->csm_write32(tt_dev, addr, value) != 0) return false;
   }
   ```
   → word 0 = `header`, words 1..7 = `payload[0..6]`. Any csm write failure aborts immediately (no rollback of already-written words).
8. Publish: `wptr = (wptr + 1) % (2 * num_entries)`; write it back to `REQ_WPTR` (`:65-67`). csm error → false.
9. Return true (`:69`).

Note: the entry words are written **before** the WPTR update, so the WPTR write is the publish point. No memory barrier separates them (see §6).

---

### 4. `arc_msg_pop` — dequeue a response (`msgqueue.c:72-132`)

Signature: `bool arc_msg_pop(tt_dev, struct arc_msg *msg, u32 queue_base, u32 num_entries)`.

1. `response_base = queue_base + ARC_MSG_QUEUE_HEADER_SIZE + (num_entries * sizeof(struct arc_msg))` (`:75`).
2. Read `RES_RPTR`; false on error; **all-1s device-gone guard** (`:82-88`).
3. Arm the same **1000 ms** timeout (`:90`).
4. Response-wait loop (`:91-113`):
   - Read `RES_WPTR`; false on error; **all-1s device-gone guard** (`:95-101`).
   - Occupancy `> 0` → break (`:103-105`).
   - Timeout → `dev_err("Timeout waiting for ARC response")`, false (`:107-110`).
   - `usleep_range(100, 200)` (`:112`).
5. `slot = rptr % num_entries`; `response_offset = slot * sizeof(struct arc_msg)` (`:115-116`).
6. Read header from `response_base + response_offset` into `msg->header` (`:117-118`), then 7 payload words (`:120-125`):
   ```c
   for (i = 0; i < 7; ++i) {
       u32 addr = response_base + response_offset + ((i + 1) * sizeof(u32));
       if (cls->csm_read32(tt_dev, addr, &msg->payload[i]) != 0) return false;
   }
   ```
   Offsets read: header at +0, payload at +4,+8,…,+28.
7. Advance consumer: `rptr = (rptr + 1) % (2 * num_entries)`; write to `RES_RPTR` (`:127-129`).
8. Return true (`:131`).

---

### 5. Request/response pairing and the transaction wrapper

`msgqueue.c` has **no pairing/matching logic** — it is a pure FIFO. Pairing is enforced by the arch `send_arc_message` doing push → interrupt → pop as one synchronous transaction, sending exactly one request and reading exactly one response. There is no request ID or tag in the protocol; correlation is purely positional/FIFO.

#### 5.1 Blackhole `send_arc_message` (`blackhole.c:500-540`)

```c
timeout = jiffies + msecs_to_jiffies(ARC_MSG_READY_MS);   // 500 ms
do {
    boot_status = noc_read32(bh, ARC_X, ARC_Y, ARC_BOOT_STATUS, 0);
    if (boot_status == 0xFFFFFFFFu) return false;          // NOC is hung
    if (boot_status & ARC_BOOT_STATUS_READY_FOR_MSG) break;
} while (time_before(jiffies, timeout));
if (!(boot_status & ARC_BOOT_STATUS_READY_FOR_MSG)) return false;

queue_ctrl_addr = noc_read32(bh, ARC_X, ARC_Y, ARC_MSG_QCB_PTR, 0);
if (csm_read32(bh, queue_ctrl_addr + 0, &queue_base) != 0) return false;
if (csm_read32(bh, queue_ctrl_addr + 4, &queue_info) != 0) return false;
num_entries = queue_info & 0xFF;

if (!arc_msg_push(&bh->tt, msg, queue_base, num_entries)) return false;
noc_write32(bh, ARC_X, ARC_Y, ARC_MSI_FIFO, 0, 0);         // trigger ARC
if (!arc_msg_pop(&bh->tt, msg, queue_base, num_entries))  return false;
return msg->header == 0;                                   // header 0 == success
```

Key facts:
- Readiness gate: poll `ARC_BOOT_STATUS` (= `RESET_SCRATCH(2)` = 0x80030408) for up to **500 ms** until bit `ARC_BOOT_STATUS_READY_FOR_MSG` (0x1) is set (`blackhole.c:509-518`). `0xFFFFFFFF` here means the NOC is hung → abort (`:511-512`).
- Interrupt/doorbell: a single `noc_write32(...ARC_MSI_FIFO, 0...)` where `ARC_MSI_FIFO = 0x800B0000`; the comment says "Write 0 to trigger the ARC message queue processor" (`blackhole.c:65,534`).
- **Success = the popped response `header == 0`** (`:539`). A non-zero response header is treated as failure by the wrapper even though `arc_msg_pop` itself returned true.
- **No lock** is taken across the push/interrupt/pop transaction. Serialization is the caller's responsibility (see §8).

#### 5.2 Wormhole `send_arc_message` (`wormhole.c:242-291`)

```c
timeout = jiffies + msecs_to_jiffies(ARC_MSG_READY_MS);   // 500 ms
do { if (arc_l2_is_running(regs)) break; } while (time_before(jiffies, timeout));
if (!arc_l2_is_running(regs)) return false;

qcb_ptr = ioread32(wh->bar4_mapping + ARC_MSG_QCB_PTR);
if (qcb_ptr == 0) { dev_warn_once("ARC message queue not available (normal for old FW)"); return false; }
if (!is_range_within_csm(qcb_ptr, sizeof(u32))) return false;

if (csm_read32(wh, qcb_ptr + 0, &queue_base) != 0) return false;
if (csm_read32(wh, qcb_ptr + 4, &queue_info) != 0) return false;
queue_base += ARC_CSM_BASE;                                // <-- WH-only fixup
num_entries = queue_info & 0xFF;

if (!arc_msg_push(&wh->tt, msg, queue_base, num_entries)) return false;
arc_misc_cntl = ioread32(regs + ARC_MISC_CNTL_REG);
iowrite32(arc_misc_cntl | ARC_MISC_CNTL_IRQ0_MASK, regs + ARC_MISC_CNTL_REG);  // trigger IRQ0
if (!arc_msg_pop(&wh->tt, msg, queue_base, num_entries))  return false;
return msg->header == 0;
```

Differences vs Blackhole:
- Readiness gate is `arc_l2_is_running` = POST-code check: `(ioread32(POST_CODE_REG) & 0xFFFF0000) == 0xC0DE0000` (`wormhole.c:199-203,101-103`).
- QCB pointer is read from a BAR4 MMIO register `ARC_MSG_QCB_PTR = RESET_UNIT_START + 0x01D8` (= BAR4 offset **0x1F301D8**), not a NOC read (`wormhole.c:119,263`). A value of **0 means "old FW, no queue"** → return false (`:264-266`).
- **`queue_base += ARC_CSM_BASE`** (`wormhole.c:277`). The WH QCB stores `queue_base` as a **CSM-relative offset**; the driver adds `ARC_CSM_BASE` (0x10000000) so the value matches what `csm_read32/write32` expect. **Blackhole does NOT do this** — its QCB stores an absolute CSM address. This asymmetry must be preserved.
- Doorbell is a read-modify-write of `ARC_MISC_CNTL_REG` (0x100) setting `ARC_MISC_CNTL_IRQ0_MASK` = `1<<16` (`wormhole.c:105-106,283-285`).
- The function is tagged `__maybe_unused` (`:242`) but is live via `wormhole_set_power_state` (`:1047`).

#### 5.3 QCB (Queue Control Block) discovery, both arches

The QCB is a small 2-word structure in CSM. Word 0 = `queue_base`, word 1 = `queue_info`; `num_entries = queue_info & 0xFF` (upper 24 bits ignored by the driver). Pointer to the QCB:
- BH: NOC read of `ARC_MSG_QCB_PTR = RESET_SCRATCH(11) = 0x8003042C` (`blackhole.c:59,64,520`).
- WH: MMIO read of `ARC_MSG_QCB_PTR = 0x1F301D8` (`wormhole.c:119,263`).

---

### 6. Memory barriers / ordering

`msgqueue.c` contains **no explicit memory barriers** — no `mb()`, `wmb()`, `rmb()`, `smp_*`, `dma_*`, or `READ_ONCE`/`WRITE_ONCE`. All ordering is inherited from the CSM accessors:

- **Wormhole** CSM access is `ioread32`/`iowrite32` on BAR4 (`wormhole.c:150,159`). On x86 these are strongly ordered, so entry-writes → WPTR-write → IRQ-trigger stay in program order.
- **Blackhole** CSM access is a NOC read/write through the kernel TLB window, each guarded by `mutex_lock(&bh->kernel_tlb_mutex)` around the `iowrite32`/`ioread32` (`blackhole.c:243-268`). The mutex serializes each individual 32-bit access but is dropped between accesses, so it is not a transaction lock.

The correctness of "publish the entry, then bump WPTR, then ring the doorbell" therefore rests entirely on MMIO accessor ordering, not on explicit barriers.

> **Porting note:** On Windows, `READ_REGISTER_*`/`WRITE_REGISTER_*` (or `MmMapIoSpace` + volatile access) do not guarantee the same ordering on all architectures. To match Linux x86 behavior you must ensure: (a) all 8 entry words are committed to the device before the WPTR store, and (b) the WPTR store is committed before the doorbell write. Insert explicit `KeMemoryBarrier()`/`_mm_sfence`/`MemoryBarrier()` between those phases if the target CPU or bus is weakly ordered. On the read side, ensure the RES_WPTR load is ordered before the response-entry loads.

---

### 7. NOC-hang early-exit detection (exact conditions)

The message-wait loops abort *early* — before the 1000 ms timeout expires — on an all-ones pointer read. These are the exact conditions:

| Location | Condition | Action |
|---|---|---|
| `msgqueue.c:25` | `wptr == U32_MAX` after reading `REQ_WPTR` | `dev_err("...WPTR...device gone?")`, return false |
| `msgqueue.c:38` | `rptr == U32_MAX` inside push wait loop | `dev_err("...RPTR...device gone?")`, return false |
| `msgqueue.c:85` | `rptr == U32_MAX` after reading `RES_RPTR` | `dev_err("...RPTR...device gone?")`, return false |
| `msgqueue.c:98` | `wptr == U32_MAX` inside pop wait loop | `dev_err("...WPTR...device gone?")`, return false |

`U32_MAX` = `0xFFFFFFFF`. These checks sit **before** the `time_after(jiffies, timeout)` check inside each loop, so a device that has fallen off the bus (reads return all-ones) aborts on the very next poll instead of spinning for a full second. Only the **pointer** reads are all-ones-checked; the entry header/payload reads are not.

Two additional (non-queue) hang gates exist in the wrappers:
- BH readiness poll: `boot_status == 0xFFFFFFFFu` → return false ("NOC is hung"), `blackhole.c:511-512`.
- WH scratch-register protocol `arc_msg_poll_completion`: `read_val == 0xFFFFFFFFu && is_hardware_hung(...)` → return `-3` (`wormhole.c:180-183`). `is_hardware_hung` checks the PCI vendor ID and `SCRATCH_REG(6) == 0xFFFFFFFF` (`wormhole.c:129-138`). This is the *legacy* SR5 protocol, not the CSM queue.

---

### 8. Error propagation, locking, cleanup

#### 8.1 Return-value contract
`arc_msg_push`/`arc_msg_pop` return `bool` only. Every distinct failure — csm range error (`-EINVAL` from `is_range_within_csm`), device-gone (all-ones), and timeout — collapses to `false`. The specific errno from `csm_read32`/`csm_write32` is **discarded** (`msgqueue.c:22-23,35-36,61-62,...`). The `dev_err` log line is the only way to tell timeout from device-gone.

`csm_read32`/`csm_write32` themselves return `-EINVAL` when the address is outside `[ARC_CSM_BASE, ARC_CSM_BASE+ARC_CSM_SIZE - len]` i.e. `[0x10000000, 0x10080000)` (`telemetry.h:73-78`, `blackhole.c:272-273,281-282`, `wormhole.c:147-148,156-157`).

#### 8.2 Caller-side propagation
- `blackhole_set_power_state` / `wormhole_set_power_state`: `send_arc_message` false → **return `-EINVAL`** (`blackhole.c:806-808`, `wormhole.c:1049-1050`).
- `blackhole_init_hardware`: ASTATE0 failure → `dev_err` but continues; WDT failure → `dev_warn` "normal for old FW", still returns true (`blackhole.c:628-638`).
- `blackhole_cleanup_hardware`: guarded by `if (tt_dev->detached) return;`, then ASTATE3 best-effort with `dev_err` on failure (`blackhole.c:700-708`).
- `blackhole_reset` (DMC path): `ARC_MSG_TYPE_TEST` failure → `dev_warn("...NOC is likely hung")` + return false; the subsequent `ARC_MSG_TYPE_TRIGGER_RESET` send's result is **ignored** and the function returns true ("// Possibly a lie...") (`blackhole.c:547-563`).

#### 8.3 Locking around transactions
`send_arc_message` takes no lock spanning the push/doorbell/pop. Serialization comes from higher layers:
- All aggregated power-setting sends funnel through `tenstorrent_set_aggregated_power_state[_locked]` under `tt_dev->chardev_mutex` (`chardev.c:540-542`; `lockdep_assert_held` at `:484`). The ioctl, open, and release callers additionally hold `reset_rwsem` **shared** for the duration (`chardev.c:601`, `:838`, `:929`), so they are mutually excluded from the reset ioctl.
- Reset / `init_hardware` paths run under `reset_rwsem` held **exclusive** in the reset ioctl (`chardev.c:598-599`; comment at `:236-238`; the reset block at `:240-290`). `cleanup_hardware` is **not** called from the reset ioctl: it runs from the suspend path (`enumerate.c:506`) and the PCI-remove path (`enumerate.c:434`).
- The deferred `power_down_work` handler (`chardev.c:553-560`) sends power-setting messages under `chardev_mutex` but **without** `reset_rwsem`; the reset ioctl drains it with `cancel_delayed_work_sync` before disturbing the device (`chardev.c:238`), and re-arming happens only while `reset_rwsem` is held shared (`chardev.c:917` inside the release path), so it cannot overlap the reset ioctl either.

The suspend-path `cleanup_hardware` send (`enumerate.c:506`) is the ARC transaction with no lock against a concurrent power-setting send (see Open Questions).

#### 8.4 Cleanup at fd close / removal
The queue itself holds no per-fd state — it is stateless device memory. At fd close the aggregated power state may be recomputed and a fresh POWER_SETTING message sent (`chardev.c` release path); `blackhole_cleanup_hardware` sends ASTATE3 from the suspend path (`enumerate.c:506`). On the PCI-remove path it is also invoked (`enumerate.c:434`), but `detached` is set earlier in remove (`enumerate.c:424`), so its `if (tt_dev->detached) return;` guard skips the ASTATE3 send there. There is no queue teardown to perform.

---

### 9. Every message ID / opcode the driver sends

The message opcode occupies the **low byte** of `arc_msg.header`. For POWER_SETTING the header additionally packs `validity` into bits 8-15 and `power_flags` into bits 16-31.

#### 9.1 Blackhole — CSM queue messages (`blackhole.c:67-72`)

| Constant | Value | Meaning / where sent | Payload |
|---|---|---|---|
| `ARC_MSG_TYPE_ASIC_STATE0` | `0xA0` | Enter A0 (active) power state; `blackhole_init_hardware` (`:628-630`) | none |
| `ARC_MSG_TYPE_ASIC_STATE3` | `0xA3` | Enter A3 (low) power state; `blackhole_cleanup_hardware` (`:705-707`) | none |
| `ARC_MSG_TYPE_SET_WDT_TIMEOUT` | `0xC1` | Set ARC watchdog timeout; `blackhole_init_hardware` (`:633-636`) | `payload[0] = 1000 * auto_reset_timeout` (ms; module param default 10 s → 10000) |
| `ARC_MSG_TYPE_TRIGGER_RESET` | `0x56` | Trigger ASIC+M3 reset; `blackhole_reset` DMC path (`:560-562`) | `payload[0] = 3` (ASIC + M3 reset arg) |
| `ARC_MSG_TYPE_POWER_SETTING` | `0x21` | Apply aggregated power state; `blackhole_set_power_state` (`:802-804`) | header bits 8-15 = validity, 16-31 = power_flags; `payload[0..6]` = `power_settings[0..13]` |
| `ARC_MSG_TYPE_TEST` | `0x90` | Ping/liveness probe before reset; `blackhole_reset` (`:551-552`) | none |

#### 9.2 Wormhole — CSM queue message (`wormhole.c:121`)

| Constant | Value | Meaning / where sent | Payload |
|---|---|---|---|
| `ARC_MSG_TYPE_POWER_SETTING` | `0xC0` | Apply aggregated power state; `wormhole_set_power_state` (`:1040-1042`) | same packing as BH |

**Note the value differs from Blackhole's POWER_SETTING (0x21 vs 0xC0).** The opcode is arch-specific.

#### 9.3 Wormhole — legacy SR5 scratch-register protocol (NOT the CSM queue) (`wormhole.c:112-116`)

These travel through Scratch Register 5, not `msgqueue.c`. Included for completeness because they are an alternate ARC-messaging path on WH.

| Constant | Value | Meaning |
|---|---|---|
| `WH_FW_MESSAGE_PRESENT` | `0xAA00` | OR'd with `message_id`, written to SR5 (`SCRATCH_REG(5)` = reset-unit offset 0x74, i.e. BAR4 off 0x1F30074) to post a message (`:112,220`) |
| `WH_FW_MSG_ASTATE0` | `0xA0` | A0 state; sent by `wormhole_init_hardware` with 10 000 µs timeout (`:725`) |
| `WH_FW_MSG_ASTATE3` | `0xA3` | A3 state; sent by `wormhole_shutdown_firmware` with 10 000 µs timeout (`:298`) |
| `WH_FW_MSG_CURR_DATE` | `0xB7` | Current date/time to FW; sent by `wormhole_send_curr_date` (`:358`) |

This table is not the complete SR5 traffic: the driver also sends `WH_FW_MSG_NOP` `0x11` (`:43`; liveness probes in `wormhole_reset`, `:481,497`), `WH_FW_MSG_TRIGGER_RESET` `0x56` (`:42`; `wormhole_reset`, `:510`), `WH_FW_MSG_PCIE_INDEX` `0x51` (`:39`; `update_device_index`, `:467`), `WH_FW_MSG_UPDATE_M3_AUTO_RESET_TIMEOUT` `0xBC` (`:41`; `wormhole_init_hardware`, `:729`), and `FW_MSG_PCIE_RETRAIN` `0xB6` (`pcie.c:16,112`) — all over SR5, none over the CSM queue.

Protocol (`wormhole.c:108-112, 205-234`): write args to `SCRATCH_REG(3)` (off 0x6C), write `0xAA00 | id` to `SCRATCH_REG(5)`, pulse IRQ0 via `ARC_MISC_CNTL_REG`, then `arc_msg_poll_completion` waits for the low 16 bits of SR5 to equal `message_id`; the high 16 bits become `*exit_code`.

#### 9.4 POWER_SETTING header/payload packing (both arches)
```c
msg.header = ARC_MSG_TYPE_POWER_SETTING | (validity << 8) | (power_flags << 16);
BUILD_BUG_ON(sizeof(power_state->power_settings) != sizeof(msg.payload));   // 14*u16 == 7*u32 == 28
memcpy(msg.payload, power_state->power_settings, sizeof(msg.payload));
```
(`blackhole.c:802-804`, `wormhole.c:1040-1042`.) `power_settings` is `__u16[14]` (`ioctl.h:410`); `validity` is a `__u8` split into "valid flags count" (bits 0-3) and "valid settings count" (bits 4-7) per `TT_POWER_VALIDITY` (`ioctl.h:400-404`).

---

### 10. What telemetry.c does NOT do

`telemetry.c` reads sensor tags through `tt_dev->dev_class->read_telemetry_tag` (`telemetry.c:28`), which for BH is `blackhole_read_telemetry_tag` → a direct `csm_read32` against a per-tag cached CSM address (`blackhole.c:440-453`). This **bypasses the ARC message queue entirely** — telemetry is a plain CSM memory read, gated only by `reset_rwsem` (read), `detached`, and `needs_hw_init` checks (`telemetry.c:16-28`). The tag→address cache is populated by `telemetry_probe` (`blackhole.c:455-498`), which is likewise pure NOC/CSM reads, not queue traffic.

---

### Key constants table

| Name | Value | Source cite |
|---|---|---|
| `ARC_MSG_QUEUE_HEADER_SIZE` | 32 bytes | `msgqueue.h:16` |
| `ARC_MSG_TIMEOUT_MS` | 1000 ms | `msgqueue.h:17` |
| `ARC_MSG_QUEUE_REQ_WPTR` offset | base + 0x00 (driver writes) | `msgqueue.h:19` |
| `ARC_MSG_QUEUE_RES_RPTR` offset | base + 0x04 (driver writes) | `msgqueue.h:20` |
| `ARC_MSG_QUEUE_REQ_RPTR` offset | base + 0x10 (FW writes) | `msgqueue.h:21` |
| `ARC_MSG_QUEUE_RES_WPTR` offset | base + 0x14 (FW writes) | `msgqueue.h:22` |
| `sizeof(struct arc_msg)` | 32 bytes (1 header + 7 payload u32) | `msgqueue.h:11-14` |
| poll sleep between waits | `usleep_range(100, 200)` (100–200 µs) | `msgqueue.c:52,112` |
| device-gone sentinel | `U32_MAX` = 0xFFFFFFFF | `msgqueue.c:25,38,85,98` |
| pointer counter range | `[0, 2*num_entries)` | `msgqueue.c:65,127` |
| `num_entries` extraction | `queue_info & 0xFF` | `blackhole.c:528`, `wormhole.c:278` |
| `ARC_CSM_BASE` | 0x10000000 | `telemetry.h:73` |
| `ARC_CSM_SIZE` | 0x80000 (1<<19) | `telemetry.h:74` |
| CSM valid range | `[0x10000000, 0x10080000)` | `telemetry.h:75-78` |
| `ARC_MSG_READY_MS` (both arches) | 500 ms | `blackhole.c:66`, `wormhole.c:120` |
| BH `ARC_MSG_QCB_PTR` | RESET_SCRATCH(11) = 0x8003042C (NOC) | `blackhole.c:59,64` |
| BH `ARC_MSI_FIFO` doorbell | 0x800B0000, write 0 | `blackhole.c:65,534` |
| BH `ARC_BOOT_STATUS` | RESET_SCRATCH(2) = 0x80030408 | `blackhole.c:59,73` |
| BH `ARC_BOOT_STATUS_READY_FOR_MSG` | 0x1 | `blackhole.c:74` |
| BH `ARC_X, ARC_Y` | 8, 0 | `blackhole.c:57-58` |
| WH `ARC_MSG_QCB_PTR` | RESET_UNIT_START+0x1D8 = 0x1F301D8 (BAR4) | `wormhole.c:119` |
| WH queue_base fixup | `+= ARC_CSM_BASE` (BH does not) | `wormhole.c:277` |
| WH `ARC_MISC_CNTL_REG` / IRQ0 mask | 0x100 / (1<<16) | `wormhole.c:105-106` |
| WH `arc_l2_is_running` POST code | `(POST_CODE & 0xFFFF0000)==0xC0DE0000` | `wormhole.c:101-103,199-203` |
| WH SR5 present marker | `WH_FW_MESSAGE_PRESENT` = 0xAA00 | `wormhole.c:112` |
| BH msg opcodes | A0=0xA0, A3=0xA3, WDT=0xC1, RESET=0x56, POWER=0x21, TEST=0x90 | `blackhole.c:67-72` |
| WH queue POWER opcode | 0xC0 | `wormhole.c:121` |
| success criterion | popped `msg->header == 0` | `blackhole.c:539`, `wormhole.c:290` |
| `auto_reset_timeout` (WDT payload) | default 10 (seconds) → ×1000 ms | `module.c:48`, `blackhole.c:634` |

---

### Open questions

1. **Power-of-two `num_entries` requirement.** The occupancy math `(wptr - rptr) % (2*num_entries)` on unsigned `u32` is only correct across wrap when `2*num_entries` divides `2^32`, i.e. `num_entries` is a power of two. The driver reads `num_entries = queue_info & 0xFF` from the QCB and never validates it. Is the FW contract that `num_entries` is always a power of two? If not, occupancy is miscomputed after a wptr wrap and the queue can wedge or corrupt. A Windows port must either preserve the identical wrap-then-modulo behavior or add validation.

2. **Transaction serialization across locks.** `send_arc_message` (push → doorbell → pop) holds no lock spanning the transaction. Power-setting sends are serialized among themselves by `chardev_mutex` and against the reset ioctl by `reset_rwsem` (shared vs exclusive — see §8.3), so the ioctl/open/release/deferred-work power paths cannot interleave with reset/init. The remaining exposure is the suspend-path `cleanup_hardware` ASTATE3 send (`enumerate.c:506`), which holds neither `chardev_mutex` nor `reset_rwsem`; it presumably relies on the PM core freezing userspace before suspend callbacks run. Confirm that invariant, or give the port an explicit per-device "ARC message" mutex.

3. **Meaning of the upper 24 bits of `queue_info`.** Only bits 0-7 (`num_entries`) are consumed. Whether the remaining bits encode entry size, version, or flags is unknown; the driver hardcodes a 32-byte entry stride. If a future FW changes entry size via those bits, the port would silently mis-stride.

4. **Response header semantics beyond zero.** Both wrappers treat `header == 0` as success and any non-zero value as failure, but the meaning of a non-zero response header (error code? echoed opcode? status bitfield?) is not documented in these files. Callers cannot distinguish error causes.

5. **Barrier requirements on non-x86 Windows targets.** `msgqueue.c` relies entirely on `ioread32`/`iowrite32` (WH) and mutex-guarded NOC accesses (BH) for ordering, with no explicit barriers. On a weakly-ordered target the entry-write → WPTR-publish → doorbell ordering (and the RES_WPTR → entry-read ordering) is not guaranteed. The exact barrier placement a Windows KMDF port needs is unverified here.

6. **`ARC_MSI_FIFO` (BH) vs `ARC_MISC_CNTL` IRQ0 (WH) doorbell semantics.** The BH doorbell writes 0 to a FIFO at 0x800B0000; the WH doorbell RMW-sets bit 16 of a control register. Whether the doorbell must strictly follow the WPTR store (and whether it auto-clears) is inferred from code comments, not confirmed against hardware docs.

---

## 10. Telemetry and hwmon

### Scope

Files covered (paths relative to tt-kmd repo root unless prefixed):

| File | Lines | Coverage |
|---|---|---|
| `telemetry.c` | 232 | entire file |
| `telemetry.h` | 81 | entire file |
| `wormhole.c` | 1084 | entire file; telemetry glue at 74–84, 370–447, 520–784 |
| `blackhole.c` | 840 | entire file; telemetry glue at 56–74, 332–498, 641–695 |
| `wormhole.h` | 32 | entire file |
| `blackhole.h` | 28 | entire file |
| `device.h` | 127 | entire file (telemetry-related fields) |
| `enumerate.c` | 545 | entire file (init/cleanup call sites, hwmon stub) |
| `chardev.c` | (partial) | 40–130 (device registration), 185–310 (reset re-probe path) |
| `tt-umd/device/api/umd/device/types/telemetry.hpp` | 80 | entire file (cross-reference for the full tag ID space) |

---

### 1. Telemetry data model: firmware-published tag table in ARC CSM

The ARC (management) firmware publishes a telemetry table inside the ARC CSM SRAM window. CSM occupies ARC addresses `0x10000000 .. 0x10080000`:

```c
#define ARC_CSM_BASE 0x10000000
#define ARC_CSM_SIZE (1 << 19)
static inline bool is_range_within_csm(u64 addr, size_t len)
{
	return (addr >= ARC_CSM_BASE) && (addr <= (ARC_CSM_BASE + ARC_CSM_SIZE) - len);
}
```
(telemetry.h:73-78)

Two firmware-owned scratch registers point at the table:

- **Wormhole** — read through the BAR4 mapping of the RESET_UNIT block. BAR4 is iATU-remapped so that it covers SoC addresses starting at `0x1E000000` (wormhole.c:74-76, 449-458). The pointers are `ARC_TELEMETRY_PTR = RESET_UNIT_START + 0x01D0` and `ARC_TELEMETRY_DATA = RESET_UNIT_START + 0x01D4`, with `RESET_UNIT_START = 0x1FF30000 - 0x1E000000` (wormhole.c:78, 83-84).
- **Blackhole** — read over the NOC from the ARC node at `x=8, y=0` (blackhole.c:57-58). `RESET_SCRATCH(N) = 0x80030400 + N*4`; `ARC_TELEMETRY_PTR = RESET_SCRATCH(13)`, `ARC_TELEMETRY_DATA = RESET_SCRATCH(12)` (blackhole.c:59-61). NOC reads go through the kernel-reserved 2M TLB window under `kernel_tlb_mutex` (blackhole.c:243-256).

#### Table layout (identical on both architectures)

`base_addr` (the value of `ARC_TELEMETRY_PTR`) points into CSM at:

| Offset from base | Contents |
|---|---|
| +0 | `u32 version`; `major = (v>>16)&0xFF`, `minor = (v>>8)&0xFF`, `patch = v&0xFF` (wormhole.c:556-559, blackhole.c:475-478) |
| +4 | `u32 num_entries` (wormhole.c:566, blackhole.c:485) |
| +8 | array of `num_entries` × `u32` tag entries: `tag_id = entry & 0xFFFF`, `offset = (entry >> 16) & 0xFFFF` (wormhole.c:549, 568-572; blackhole.c:468, 487-491) |

The value for a tag lives at `data_addr + offset * 4` where `data_addr` is the value of `ARC_TELEMETRY_DATA` (wormhole.c:572, blackhole.c:491).

#### Probe-time validation (`telemetry_probe`, one per arch)

Both implementations (`wormhole.c:536-584`, `blackhole.c:455-498`):

1. `memset` the tag cache to zero (wormhole.c:545, blackhole.c:464).
2. Reject if `base_addr` or `data_addr` are outside CSM → `dev_err("Telemetry not available")`, return `-ENODEV` (wormhole.c:551-554, blackhole.c:470-473). Note the WH check uses `is_range_within_csm(addr, sizeof(u32))` while BH uses length 1 (blackhole.c:470).
3. Reject `major_ver > 1` → `dev_err("Unsupported telemetry version %u.%u.%u")`, return `-ENOTSUPP` (wormhole.c:561-564, blackhole.c:480-483).
4. Iterate entries, caching addresses for `tag_id < TELEM_TAG_CACHE_SIZE` (128) (wormhole.c:579-580, blackhole.c:493-494).
   - **WH only**: each per-tag address is bounds-checked against CSM; invalid entries log `"Telemetry tag %u has invalid address 0x%08X"` and are skipped (wormhole.c:574-577).
   - **BH**: no per-tag validation at probe; validation is deferred to read time via `csm_read32` (blackhole.c:270-277).

#### The tag cache is an *address* cache, not a value cache

```c
// Per-device tag-to-address cache, indexed by tag ID.
// Populated by telemetry_probe(); zero means tag not available.
// The stored value is arch-specific: WH stores a BAR4 sysreg offset,
// BH stores a raw CSM address for NOC reads.
u64 telemetry_tag_cache[TELEM_TAG_CACHE_SIZE];
```
(device.h:75-79; `TELEM_TAG_CACHE_SIZE` = 128, telemetry.h:13)

WH stores `wh_arc_addr_to_sysreg(addr) = ARC_CSM_START + (addr - ARC_CSM_BASE)` — a BAR4 offset (wormhole.c:140-143, 580). BH stores the raw CSM address (blackhole.c:491-494). A cached value of **0 means "tag not present"**; this drives both hwmon and sysfs attribute visibility.

**There is no value caching and no periodic refresh in the driver**: every sysfs/hwmon read performs a live MMIO (WH) or NOC (BH) read of the firmware-maintained value. The firmware updates values on its own schedule (tag 5 `UPDATE_TELEM_SPEED` exists in the tag space, tt-umd/device/api/umd/device/types/telemetry.hpp:16, but the KMD does not read it).

---

### 2. Tag IDs — full enumeration

#### Tags the KMD knows about (telemetry.h:15-41)

| KMD name | ID | Used for |
|---|---|---|
| `TELEMETRY_BOARD_ID` | 1 | `tt_serial` (u64: tag 1 = high, tag 2 = low), `tt_card_type` |
| `TELEMETRY_VCORE` | 6 | hwmon `in*_input` (mV) |
| `TELEMETRY_POWER` | 7 | hwmon `power1_input` (W → µW) |
| `TELEMETRY_CURRENT` | 8 | hwmon `curr1_input` (A → mA) |
| `TELEMETRY_VDD_LIMITS` | 9 | hwmon `in*_max` (max in upper 16 bits, mV) |
| `TELEMETRY_THM_LIMIT_SHUTDOWN` | 10 | defined, **unused** |
| `TELEMETRY_ASIC_TEMP` | 11 | hwmon `temp1_input` (16.16 fixed → m°C) |
| `TELEMETRY_AICLK` | 14 | sysfs `tt_aiclk` |
| `TELEMETRY_AXICLK` | 15 | sysfs `tt_axiclk` |
| `TELEMETRY_ARCCLK` | 16 | sysfs `tt_arcclk` |
| `TELEMETRY_ETH_FW_VERSION` | 24 | sysfs `tt_eth_fw_ver` (WH only; special packing) |
| `TELEMETRY_BM_APP_FW_VERSION` | 26 | sysfs `tt_m3app_fw_ver` |
| `TELEMETRY_BM_BL_FW_VERSION` | 27 | sysfs `tt_m3bl_fw_ver` (WH only) |
| `TELEMETRY_FLASH_BUNDLE_VERSION` | 28 | sysfs `tt_fw_bundle_ver` |
| `TELEMETRY_CM_FW_VERSION` | 29 | sysfs `tt_arc_fw_ver` (WH only) |
| `TELEMETRY_FAN_SPEED` | 31 | defined, **unused** |
| `TELEMETRY_TIMER_HEARTBEAT` | 32 | sysfs `tt_heartbeat` |
| `TELEMETRY_FAN_RPM` | 41 | hwmon `fan1_input` (BH only) |
| `TELEMETRY_TDC_LIMIT_MAX` | 55 | hwmon `curr1_max` |
| `TELEMETRY_THM_LIMIT_THROTTLE` | 56 | hwmon `temp1_max` |
| `TELEMETRY_TT_FLASH_VERSION` | 58 | sysfs `tt_ttflash_ver` (WH only) |
| `TELEMETRY_THERM_TRIP_COUNT` | 60 | sysfs `tt_therm_trip_count` (BH only) |
| `TELEMETRY_ASIC_ID` | 61 | sysfs `tt_asic_id` (u64: tag 61 = high, tag 62 = low) |
| `TELEMETRY_AICLK_LIMIT_MAX` | 63 | defined, **unused** |
| `TELEMETRY_TDP_LIMIT_MAX` | 64 | hwmon `power1_max` |

The `u64_hex` show routine implicitly consumes `tag_id + 1` as the low word (telemetry.c:56-64), so tags 2 (`BOARD_ID_LOW`) and 62 (`ASIC_ID_LOW`) are consumed even though the KMD has no named constant for them.

#### Full tag ID space (cross-reference, tt-umd/device/api/umd/device/types/telemetry.hpp:11-78)

`BOARD_ID_HIGH=1, BOARD_ID_LOW=2, ASIC_ID=3, HARVESTING_STATE=4, UPDATE_TELEM_SPEED=5, VCORE=6, TDP=7, TDC=8, VDD_LIMITS=9, THM_LIMIT_SHUTDOWN=10, ASIC_TEMPERATURE=11, VREG_TEMPERATURE=12, BOARD_TEMPERATURE=13, AICLK=14, AXICLK=15, ARCCLK=16, L2CPUCLK0..3=17..20, ETH_LIVE_STATUS=21, GDDR_STATUS=22, GDDR_SPEED=23, ETH_FW_VERSION=24, GDDR_FW_VERSION=25, DM_APP_FW_VERSION=26, DM_BL_FW_VERSION=27, FLASH_BUNDLE_VERSION=28, CM_FW_VERSION=29, L2CPU_FW_VERSION=30, FAN_SPEED=31, TIMER_HEARTBEAT=32, TELEMETRY_ENUM_COUNT=33, ENABLED_TENSIX_COL=34, ENABLED_ETH=35, ENABLED_GDDR=36, ENABLED_L2CPU=37, PCIE_USAGE=38, NOC_TRANSLATION=40, FAN_RPM=41, GDDR_0_1_TEMP=42, GDDR_2_3_TEMP=43, GDDR_4_5_TEMP=44, GDDR_6_7_TEMP=45, GDDR_0_1_CORR_ERRS=46, GDDR_2_3_CORR_ERRS=47, GDDR_4_5_CORR_ERRS=48, GDDR_6_7_CORR_ERRS=49, GDDR_UNCORR_ERRS=50, MAX_GDDR_TEMP=51, ASIC_LOCATION=52, BOARD_POWER_LIMIT=53, INPUT_POWER=54, TDC_LIMIT_MAX=55, THM_LIMIT_THROTTLE=56, TT_FLASH_VERSION=58, THERM_TRIP_COUNT=60, ASIC_ID_HIGH=61, ASIC_ID_LOW=62, AICLK_LIMIT_MAX=63, TDP_LIMIT_MAX=64, AICLK_ARB_MIN=65, AICLK_ARB_MAX=66, ENABLED_MIN_ARB=67, ENABLED_MAX_ARB=68, NUMBER_OF_TAGS=69`

Note the UMD's `ASIC_ID=3` vs `ASIC_ID_HIGH/LOW=61/62` — the KMD uses 61/62 for `tt_asic_id`. Tag IDs 39, 57, 59 are absent from both enumerations.

---

### 3. The core read path: `tt_telemetry_read32`

```c
int tt_telemetry_read32(struct tenstorrent_device *tt_dev, u16 tag_id, u32 *value)
{
	if (tag_id >= TELEM_TAG_CACHE_SIZE)
		return -EINVAL;
	down_read(&tt_dev->reset_rwsem);
	if (tt_dev->detached) { r = -ENODEV; goto out; }
	if (tt_dev->needs_hw_init) { r = -ENODATA; goto out; }
	r = tt_dev->dev_class->read_telemetry_tag(tt_dev, tag_id, value);
out:
	up_read(&tt_dev->reset_rwsem);
	return r;
}
```
(telemetry.c:9-33)

- **Validation**: `tag_id >= 128` → `-EINVAL`.
- **Locks**: `reset_rwsem` held for read across the hardware access; this is the same lock `tenstorrent_pci_remove` takes for write before unmapping BARs (enumerate.c:444-447), so an in-flight telemetry read cannot race BAR teardown.
- **Device state**: `detached` → `-ENODEV`; `needs_hw_init` (set between an ASIC reset and POST_RESET) → `-ENODATA` (telemetry.c:18-26).

Arch backends:

- **Wormhole** `wormhole_read_telemetry_tag` (wormhole.c:520-534): `tag_id >= 128` → `-EINVAL`; cache entry 0 → `-ENODATA`; else `*value = ioread32(wh->bar4_mapping + offset)`, returns 0.
- **Blackhole** `blackhole_read_telemetry_tag` (blackhole.c:440-453): same `-EINVAL`/`-ENODATA` checks; then `csm_read32` which re-validates CSM bounds (`-EINVAL` if outside, blackhole.c:270-277) and does a NOC read via the kernel TLB window while holding `bh->kernel_tlb_mutex` (blackhole.c:243-256). **Each BH telemetry read reprograms the kernel TLB window** (blackhole.c:228-241).

Any error from this path propagates as the return value of the sysfs/hwmon `read`/`show` callback, i.e. userspace `read(2)` on the attribute fails with that errno.

---

### 4. hwmon exposure

#### Registration

- **WH**: `hwmon_device_register_with_info(dev, "wormhole", tt_dev, &wh_hwmon_chip_info, NULL)` with `dev = &tt_dev->pdev->dev` (parent is the **PCI device**) (wormhole.c:619-638). Registration is deferred to `fw_ready_work` (see §7). On failure only a `dev_warn` is emitted; the driver continues without hwmon (wormhole.c:629-632).
- **BH**: `hwmon_device_register_with_info(dev, "blackhole", tt_dev, &bh_hwmon_chip_info, NULL)`, synchronous in `blackhole_init_telemetry`, only if `telemetry_probe` succeeded; failure makes `init_telemetry` return false (blackhole.c:652-672).
- After successful registration both archs emit `kobject_uevent(&tt_dev->dev.kobj, KOBJ_CHANGE)` to tell udev the attributes are ready (wormhole.c:637, blackhole.c:671).
- Shared ops table: `tt_hwmon_ops = { .is_visible, .read, .read_string }` (telemetry.c:228-232).
- enumerate.c:78-82 contains a `!IS_ENABLED(CONFIG_HWMON)` stub for `devm_hwmon_device_register_with_info` — a function the driver **no longer calls** (both archs use the non-devm `hwmon_device_register_with_info`), so a `CONFIG_HWMON=n` build would fail to link; the stub appears stale.

#### Channels (tag → hwmon attribute mapping)

Wormhole (`wh_hwmon_attrs`, wormhole.c:586-596) and Blackhole (`bh_hwmon_attrs`, blackhole.c:400-411) share eight entries; BH adds one fan entry:

| Tag | hwmon type | hwmon attr | sysfs file (hwmon ABI) | arch |
|---|---|---|---|---|
| `TELEMETRY_ASIC_TEMP` (11) | `hwmon_temp` | `temp_input` | `temp1_input` | both |
| `TELEMETRY_THM_LIMIT_THROTTLE` (56) | `hwmon_temp` | `temp_max` | `temp1_max` | both |
| `TELEMETRY_VCORE` (6) | `hwmon_in` | `in_input` | `in0_input` | both |
| `TELEMETRY_VDD_LIMITS` (9) | `hwmon_in` | `in_max` | `in0_max` | both |
| `TELEMETRY_CURRENT` (8) | `hwmon_curr` | `curr_input` | `curr1_input` | both |
| `TELEMETRY_TDC_LIMIT_MAX` (55) | `hwmon_curr` | `curr_max` | `curr1_max` | both |
| `TELEMETRY_POWER` (7) | `hwmon_power` | `power_input` | `power1_input` | both |
| `TELEMETRY_TDP_LIMIT_MAX` (64) | `hwmon_power` | `power_max` | `power1_max` | both |
| `TELEMETRY_FAN_RPM` (41) | `hwmon_fan` | `fan_input` | `fan1_input` | **BH only** |

Channel declarations: WH `HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT | HWMON_T_LABEL | HWMON_T_MAX)`, and likewise for in/curr/power (wormhole.c:606-612); BH adds `HWMON_CHANNEL_INFO(fan, HWMON_F_INPUT | HWMON_F_LABEL)` (blackhole.c:426-433). One channel per sensor type. (The concrete file names in the table follow the standard Linux hwmon sysfs ABI numbering — voltage channels start at 0, all others at 1 — given channel count 1 per type in these tables.)

#### Labels — **the names differ between architectures**

WH (`wh_hwmon_labels`, wormhole.c:598-604): `"asic1_temp"`, `"vcore1"`, `"current1"`, `"power1"`.
BH (`bh_hwmon_labels`, blackhole.c:391-398): `"asic_temp"`, `"vcore"`, `"current"`, `"power"`, `"fan_rpm"`.

Labels are served by `tt_hwmon_read_string` (telemetry.c:212-226, `-EOPNOTSUPP` if no match) and are **always visible** regardless of tag availability (telemetry.c:155-158).

#### Scaling arithmetic (`tt_hwmon_read`, telemetry.c:170-210)

```c
if (type == hwmon_temp) {
	if (attr == hwmon_temp_input) {
		// ASIC_TEMPERATURE is 16.16 fixed-point
		u32 int_part = raw >> 16;
		u32 frac_part = raw & 0xFFFF;
		*val = (int_part * 1000) + ((frac_part * 1000) / 0x10000);
	} else {
		// Limit tags are plain degrees C
		*val = raw * 1000;
	}
} else if (type == hwmon_curr) {
	*val = raw * 1000;	// Convert A to mA
} else if (type == hwmon_power) {
	*val = raw * 1000000;	// Convert W to uW
} else if (type == hwmon_in) {
	if (attr == hwmon_in_max)
		raw = (raw >> 16) & 0xFFFF;  // VDD_LIMITS: max in upper 16
	*val = raw;		// Reported in mV
} else if (type == hwmon_fan) {
	*val = raw;		// Reported in RPM
}
```

Firmware raw units → hwmon ABI units:

| Channel | FW raw format | Driver output (hwmon ABI) |
|---|---|---|
| `temp1_input` | 16.16 fixed-point °C | milli-°C |
| `temp1_max` | integer °C | milli-°C (`*1000`) |
| `curr1_input`/`curr1_max` | integer A | mA (`*1000`) |
| `power1_input`/`power1_max` | integer W | µW (`*1000000`) |
| `in0_input` | integer mV | mV (as-is) |
| `in0_max` | packed; max in bits 31:16, mV | mV (`(raw>>16)&0xFFFF`) |
| `fan1_input` | RPM | RPM (as-is) |

Unmatched type/attr → `-EOPNOTSUPP` (telemetry.c:209).

#### Visibility

`tt_hwmon_is_visible` (telemetry.c:149-168): label attributes → always `S_IRUGO` (0444); value attributes → `S_IRUGO` only if `telemetry_tag_cache[tag_id] != 0`, else hidden (mode 0). Visibility is evaluated by the hwmon core at registration time, so a tag that appears only after a firmware update becomes visible after the next re-registration (device removal/re-probe), not merely after a tag-cache refresh.

---

### 5. Non-hwmon sysfs attributes (`telemetry_group` on the class device)

Attached via `device_add_group(&tt_dev->dev, &tt_dev->telemetry_group)` — i.e. on the **tenstorrent class device** (`/sys/class/tenstorrent/tenstorrent!<N>`, name set by `dev_set_name(&tt_dev->dev, TENSTORRENT "/%d", ...)`, chardev.c:106; `TENSTORRENT` = `"tenstorrent"`, enumerate.h:13), not on the PCI device. Group construction: `telemetry_attrs` array built in `init_device`, with `telemetry_group.is_visible = tt_sysfs_telemetry_is_visible` (wormhole.c:705-708, blackhole.c:612-615). All attributes are mode `S_IRUGO` (0444). Attributes whose tag is absent from the tag cache are hidden entirely (telemetry.c:130-143, mode 0).

#### Wormhole attribute list (`wh_sysfs_attributes`, wormhole.c:370-384)

| Name | Tag | Show fn | Format |
|---|---|---|---|
| `tt_aiclk` | 14 | `tt_sysfs_show_u32_dec` | `"%u\n"` raw FW value (no scaling) |
| `tt_axiclk` | 15 | u32_dec | same |
| `tt_arcclk` | 16 | u32_dec | same |
| `tt_serial` | 1 (+2) | `tt_sysfs_show_u64_hex` | `"%08X%08X\n"` (hi=tag, lo=tag+1) |
| `tt_card_type` | 1 | `tt_sysfs_show_card_type` | decoded string, see below |
| `tt_fw_bundle_ver` | 28 | `tt_sysfs_show_u32_ver` | `"%u.%u.%u.%u\n"` |
| `tt_m3app_fw_ver` | 26 | u32_ver | same |
| `tt_ttflash_ver` | 58 | u32_ver | same |
| `tt_m3bl_fw_ver` | 27 | u32_ver | same |
| `tt_arc_fw_ver` | 29 | u32_ver | same |
| `tt_eth_fw_ver` | 24 | u32_ver | `"%u.%u.%u\n"` (ETH packing) |
| `tt_asic_id` | 61 (+62) | u64_hex | `"%08X%08X\n"` |
| `tt_heartbeat` | 32 | u32_dec | `"%u\n"` |

#### Blackhole attribute list (`bh_sysfs_attributes`, blackhole.c:413-424)

| Name | Tag | Show fn |
|---|---|---|
| `tt_aiclk` | 14 | u32_dec |
| `tt_axiclk` | 15 | u32_dec |
| `tt_arcclk` | 16 | u32_dec |
| `tt_serial` | 1 (+2) | u64_hex |
| `tt_card_type` | 1 | card_type |
| `tt_fw_bundle_ver` | 28 | u32_ver |
| `tt_m3app_fw_ver` | 26 | u32_ver |
| `tt_asic_id` | 61 (+62) | u64_hex |
| `tt_heartbeat` | 32 | u32_dec |
| `tt_therm_trip_count` | 60 | u32_dec |

Differences: BH **lacks** `tt_ttflash_ver`, `tt_m3bl_fw_ver`, `tt_arc_fw_ver`, `tt_eth_fw_ver`; BH **adds** `tt_therm_trip_count`.

#### Formatting details

- `tt_sysfs_show_u32_dec`: `scnprintf(buf, PAGE_SIZE, "%u\n", value)` (telemetry.c:35-47).
- `tt_sysfs_show_u64_hex`: reads `tag_id` (high word) and `tag_id + 1` (low word), prints `"%08X%08X\n"` — 16 uppercase hex digits, zero-padded, no `0x` prefix (telemetry.c:49-65).
- `tt_sysfs_show_u32_ver`: default packing `major=(v>>24)&0xFF, minor=(v>>16)&0xFF, patch=(v>>8)&0xFF, ver=v&0xFF` → `"%u.%u.%u.%u\n"`. Exception when `tag_id == TELEMETRY_ETH_FW_VERSION`: `major=(v>>16)&0xFF, minor=(v>>12)&0xF, patch=v&0xFFF` → `"%u.%u.%u\n"` (telemetry.c:67-93).
- `tt_sysfs_show_card_type`: `card_type = (value >> 4) & 0xFFFF` from `TELEMETRY_BOARD_ID` high word, decoded (telemetry.c:95-128):

```c
case 0x14: card_name = "n300"; break;
case 0x18: card_name = "n150"; break;
case 0x35: card_name = "galaxy-wormhole"; break;
case 0x36: card_name = "p100"; break;
case 0x40: card_name = "p150a"; break;
case 0x41: card_name = "p150b"; break;
case 0x42: card_name = "p150c"; break;
case 0x43: card_name = "p100a"; break;
case 0x44: card_name = "p300b"; break;
case 0x45: card_name = "p300a"; break;
case 0x46: card_name = "p300c"; break;
case 0x47: card_name = "galaxy-blackhole"; break;
default: card_name = "unknown"; break;
```
(telemetry.c:111-124)

#### tt_heartbeat and thermal-trip counter

- `tt_heartbeat` exposes `TELEMETRY_TIMER_HEARTBEAT` (tag 32) as a raw decimal `u32`. The driver applies no interpretation; it is a firmware-incremented counter that tools poll to detect a hung ARC. Present on both archs (wormhole.c:383, blackhole.c:422).
- `tt_therm_trip_count` exposes `TELEMETRY_THERM_TRIP_COUNT` (tag 60) as raw decimal, **Blackhole only** (blackhole.c:423). The thermal *shutdown* limit tag (10) is defined but never exposed (telemetry.h:21, no users).

---

### 6. PCIe perf counter sysfs group (non-telemetry but same lifecycle)

Both archs register an `attribute_group` named `"pcie_perf_counters"` on the class device inside `init_telemetry` (wormhole.c:753-757, blackhole.c:646-650); registration failure is only logged (`"PCIe perf counters unavailable: %d"`).

Twelve read-only attributes per arch — six counters × two NOCs, names suffixed `0`/`1` (wormhole.c:419-447, blackhole.c:361-389):
`slv_posted_wr_data_word_received{0,1}`, `slv_nonposted_wr_data_word_received{0,1}`, `slv_rd_data_word_sent{0,1}`, `mst_posted_wr_data_word_sent{0,1}`, `mst_nonposted_wr_data_word_sent{0,1}`, `mst_rd_data_word_received{0,1}` — each printed `"%u\n"`.

Counter register indices (identical on both archs, wormhole.c:412-417, blackhole.c:354-359):

```c
#define SLV_POSTED_WR_DATA_WORD_RECEIVED 0x39
#define SLV_NONPOSTED_WR_DATA_WORD_RECEIVED 0x38
#define SLV_RD_DATA_WORD_SENT 0x33
#define MST_POSTED_WR_DATA_WORD_SENT 0x9
#define MST_NONPOSTED_WR_DATA_WORD_SENT 0x8
#define MST_RD_DATA_WORD_RECEIVED 0x3
```

Address computation:

- WH: `ioread32(bar4 + NIU_COUNTERS_START + 4*counter + noc*0x8000)` where `NIU_COUNTERS_START = NOC2AXI_START + 0x200` and `NOC2AXI_START = 0x1FD02000 - 0x1E000000` (wormhole.c:81, 386-396).
- BH: `ioread32(noc2axi_cfg + 0x4200 + 4*counter + noc*0x10000)` (`NOC_STATUS_OFFSET` 0x4200, `NOC1_NOC2AXI_OFFSET` 0x10000, blackhole.c:47-48, 332-339). `noc2axi_cfg` is a BAR0 range mapping at `0x1FD00000` length `0x100000` (blackhole.c:44-45, 589).

**These show functions perform raw MMIO with no `reset_rwsem`, no `detached` check, and no tag gating.** Safety at removal relies solely on ordering: `cleanup_telemetry` removes the groups (which waits for in-flight sysfs callbacks) *before* `cleanup_device` unmaps BARs (enumerate.c:436-447).

---

### 7. Lifecycle, deferral, and update intervals

#### First registration

`tenstorrent_pci_probe` calls `device_class->init_telemetry(tt_dev)` only when `init_hardware` succeeded (`!needs_hw_init`) (enumerate.c:370, 382-383).

- **Blackhole** (`blackhole_init_telemetry`, blackhole.c:641-675): synchronous — register perf-counter group, run `telemetry_probe`, and on success add the telemetry group, set `hwmon_attributes`/`hwmon_labels`, register hwmon, emit `KOBJ_CHANGE`. If `telemetry_probe` fails, the telemetry group and hwmon are simply never registered (function still returns true).
- **Wormhole** (`wormhole_init_telemetry`, wormhole.c:748-764): registers the perf-counter group, then defers the rest to a delayed work item because "On some WH systems, ARC firmware may not have finished initializing by the time the PCI driver probes" (wormhole.c:649-653). `wormhole_probe_telemetry` sets `wh->telemetry_retries = 120` and schedules `fw_ready_work` immediately (wormhole.c:739-746). `fw_ready_work_func` (wormhole.c:654-681):
  - bails if `tt_dev->detached`;
  - checks readiness by validating both telemetry pointers against CSM (`is_fw_ready_for_telemetry`, wormhole.c:640-647); if not ready, reschedules itself every **1000 ms**, up to **120 retries** (~2 minutes), then logs `"Timed out waiting for FW telemetry"`;
  - once ready: `telemetry_probe`, then first-time-only `device_add_group` (guarded by `wh->telemetry_group_registered`, wormhole.h:23) and hwmon registration (guarded by `tt_dev->hwmon_dev`).

#### Re-probe after reset

`TENSTORRENT_RESET_DEVICE_POST_RESET` re-runs `init_hardware` and then `probe_telemetry` "in case firmware was updated before this reset" (chardev.c:274-283). WH's `probe_telemetry` restarts the polling work (wormhole.c:739-746); BH's is the synchronous `telemetry_probe` (blackhole.c:828). sysfs groups and the hwmon device stay registered across resets; only the address cache is refreshed. While `needs_hw_init` is true (between ASIC reset and POST_RESET), all telemetry reads return `-ENODATA` (telemetry.c:23-26).

#### Teardown (device removal / driver shutdown)

Order in `tenstorrent_pci_remove` (also used as the PCI `shutdown` callback, enumerate.c:532):

1. WH only: `cancel_delayed_work_sync(&wh->fw_ready_work)` (enumerate.c:410-413).
2. `detached = true` under `chardev_mutex` (enumerate.c:423-425).
3. `cleanup_hardware` (A3/ASTATE3) if the device still answers config reads (enumerate.c:432-434).
4. `cleanup_telemetry` — "Tear down hwmon/sysfs interfaces before unmapping BARs. This removes the sysfs files and waits for any in-flight callbacks to complete" (enumerate.c:436-440). Both arch implementations unregister hwmon, then remove the telemetry group, then the perf-counter group, all guarded by their registered flags (wormhole.c:766-784, blackhole.c:677-695).
5. BAR unmap under `reset_rwsem` write (enumerate.c:444-447).

#### Update intervals — summary

- Driver-side: **none for values**. Every read is live. The only timing in the telemetry subsystem is the WH firmware-readiness poll (1 s period × 120 retries, wormhole.c:665, 743).
- Firmware-side value refresh cadence is not visible in the KMD (see open questions; tag 5 `UPDATE_TELEM_SPEED` exists in the tag space but is unread).

---

### Porting notes

> **Porting note (interface surface):** Linux exposes two distinct read-only surfaces: (a) hwmon channels with fixed ABI names/units (`temp1_input` in m°C, `in0_input`/`in0_max` in mV, `curr1_input`/`curr1_max` in mA, `power1_input`/`power1_max` in µW, `fan1_input` in RPM, plus `*_label` strings), consumed by generic tools like `lm-sensors`; and (b) the `tt_*` device attributes consumed by tt-smi/tt-flash. Windows has no hwmon; the natural mapping is a device IOCTL or WMI provider. Whatever transport is chosen, the **names, formats, and units above must be reproduced exactly** (including the WH/BH label differences `asic1_temp` vs `asic_temp`, the `%08X%08X` 16-digit uppercase hex serial format, the 4-part vs 3-part version strings, and the m°C/mV/mA/µW/RPM scaling) so that ported tooling behaves identically.

> **Porting note (address cache & visibility):** The tag→address cache and the "hidden when tag absent" semantics should be preserved: a Windows port should return distinct errors mirroring `-EINVAL` (bad tag ≥128), `-ENODEV` (surprise-removed), `-ENODATA` (mid-reset or tag not published) — e.g. `STATUS_INVALID_PARAMETER` / `STATUS_DEVICE_DOES_NOT_EXIST` / `STATUS_DEVICE_NOT_READY` — rather than collapsing them.

> **Porting note (synchronization):** `reset_rwsem` (shared for reads, exclusive during remove/reset) maps to a KMDF-style remove-lock or ERESOURCE; the BH `kernel_tlb_mutex` serializing the shared 2M NOC window is a plain mutex/fast mutex. The WH deferred probe (`fw_ready_work`, 1 s × 120) maps to a KMDF timer or system worker thread; it must be cancelled synchronously before teardown, exactly as enumerate.c:410-413 does.

> **Porting note (teardown ordering):** The invariant "remove externally-visible telemetry interfaces and wait for in-flight readers *before* unmapping BARs" (enumerate.c:436-447) is load-bearing — the PCIe perf-counter reads have no other protection against use-after-unmap.

> **Porting note (uevent):** `KOBJ_CHANGE` after late attribute registration (wormhole.c:637, blackhole.c:671) exists so udev rules re-evaluate. A Windows equivalent (if needed by tooling) would be a custom device interface arrival notification or WMI event.

---

### Key constants table

| Name | Value | Source |
|---|---|---|
| `TELEM_TAG_CACHE_SIZE` | 128 | telemetry.h:13 |
| `ARC_CSM_BASE` | `0x10000000` | telemetry.h:73 |
| `ARC_CSM_SIZE` | `1 << 19` (512 KiB) | telemetry.h:74 |
| WH `BAR4_SOC_TARGET_ADDRESS` | `0x1E000000` | wormhole.c:76 |
| WH `RESET_UNIT_START` | `0x1FF30000 - 0x1E000000` | wormhole.c:78 |
| WH `ARC_CSM_START` | `0x1FE80000 - 0x1E000000` | wormhole.c:79 |
| WH `ARC_TELEMETRY_PTR` | `RESET_UNIT_START + 0x01D0` | wormhole.c:83 |
| WH `ARC_TELEMETRY_DATA` | `RESET_UNIT_START + 0x01D4` | wormhole.c:84 |
| BH `ARC_X`, `ARC_Y` | 8, 0 | blackhole.c:57-58 |
| BH `RESET_SCRATCH(N)` | `0x80030400 + N*4` | blackhole.c:59 |
| BH `ARC_TELEMETRY_PTR` | `RESET_SCRATCH(13)` | blackhole.c:60 |
| BH `ARC_TELEMETRY_DATA` | `RESET_SCRATCH(12)` | blackhole.c:61 |
| Telemetry version field | `major=(v>>16)&0xFF`, reject `major > 1` | wormhole.c:556-563, blackhole.c:475-482 |
| Tag entry packing | `tag=lo16`, `offset=hi16`, value at `data + offset*4` | wormhole.c:570-572, blackhole.c:489-491 |
| temp input fixed point | 16.16 → m°C | telemetry.c:186-189 |
| VDD_LIMITS max | `(raw>>16)&0xFFFF` mV | telemetry.c:199-201 |
| hwmon device names | `"wormhole"` / `"blackhole"` | wormhole.c:628, blackhole.c:664 |
| WH labels | `asic1_temp, vcore1, current1, power1` | wormhole.c:598-604 |
| BH labels | `asic_temp, vcore, current, power, fan_rpm` | blackhole.c:391-398 |
| sysfs group name (PCIe) | `"pcie_perf_counters"` | wormhole.c:444-447, blackhole.c:386-389 |
| PCIe counter IDs | 0x39/0x38/0x33/0x9/0x8/0x3 | wormhole.c:412-417, blackhole.c:354-359 |
| WH NIU counter base | `NOC2AXI_START + 0x200`, NOC1 stride 0x8000 | wormhole.c:386-387 |
| BH NIU counter base | `0x4200`, NOC1 stride 0x10000 | blackhole.c:47-48, 336 |
| WH FW-ready poll | 1000 ms period, 120 retries | wormhole.c:665, 743 |
| ETH FW version packing | `maj=(v>>16)&0xFF, min=(v>>12)&0xF, patch=v&0xFFF` | telemetry.c:80-84 |
| Default version packing | `maj.min.patch.ver` from bytes 3..0 | telemetry.c:87-92 |
| Card-type field | `(BOARD_ID_HIGH >> 4) & 0xFFFF` | telemetry.c:108 |
| Class/device name | `"tenstorrent"`, `"tenstorrent/%d"` | enumerate.h:13, chardev.c:106 |
| Attribute mode | `S_IRUGO` (0444) all telemetry attrs | wormhole.c:370-384, blackhole.c:413-424, telemetry.c:157-163 |

### Open questions

1. **Firmware value refresh cadence**: the KMD never reads tag 5 (`UPDATE_TELEM_SPEED`, tt-umd/device/api/umd/device/types/telemetry.hpp:16) and contains no statement of how often the ARC firmware updates the CSM values or increments `TIMER_HEARTBEAT`. A Windows port that adds any caching layer would need this from firmware documentation, not from this driver.
2. **Clock attribute units**: `tt_aiclk`/`tt_axiclk`/`tt_arcclk` print the raw `u32` with no unit conversion (telemetry.c:35-47). The units are whatever firmware publishes (conventionally MHz), but the KMD neither documents nor enforces this.
3. **BH probe-time address validation gap**: BH `telemetry_probe` caches tag addresses without CSM bounds-checking (blackhole.c:487-495), relying on `csm_read32`'s `-EINVAL` at read time, while WH validates and skips at probe time (wormhole.c:574-577). Whether a port should normalize to the stricter WH behavior (attribute hidden) or keep BH's behavior (attribute visible but read fails) is a policy choice; they are user-visible differences for a malformed FW table.
4. **Stale `CONFIG_HWMON=n` stub**: enumerate.c:78-82 stubs `devm_hwmon_device_register_with_info`, but the driver calls the non-devm `hwmon_device_register_with_info` (wormhole.c:628, blackhole.c:664); a hwmon-less Linux build looks broken. Irrelevant on Windows but indicates the stub is not a behavior to emulate.
5. **PCIe perf counter reads bypass `reset_rwsem`/`detached`** (wormhole.c:389-397, blackhole.c:332-339): safe on Linux only because of teardown ordering (enumerate.c:436-447). It is ambiguous whether concurrent reads during an ASIC reset (`needs_hw_init` true, BARs still mapped) are meaningful; the driver allows them and returns whatever the hardware yields.
6. **`tt_card_type` unknown IDs**: values other than the twelve enumerated (telemetry.c:111-123) print `"unknown"`; new board IDs will need table updates in lockstep with the Linux driver to keep tool behavior identical.
7. **hwmon visibility is registration-time only**: a tag that appears after a POST_RESET re-probe updates the tag cache, but hwmon `is_visible` results were latched at registration. Whether tools depend on late-appearing channels is unknown; the WH deferred-registration path mitigates the common case (slow ARC boot).

---

## 11. PCIe Link Management and Device Reset

### Scope

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
| `tools/reset.c` (tt-kmd) | 294 | reference userspace consumer of ASIC_RESET/POST_RESET |
| `tt-umd/device/warm_reset.cpp` | 710 | reference userspace reset orchestration |
| `tt-umd/device/pcie/pci_device.cpp` | (relevant parts) | how UMD issues the reset ioctl |

---

### 1. Reset-related device and fd state

`struct tenstorrent_device` carries four fields that drive the whole reset model (device.h:33-36):

```c
bool detached; // No longer valid for hardware access
bool needs_hw_init;
atomic_long_t reset_gen; // Generation counter, incremented on reset
struct rw_semaphore reset_rwsem;
```

Each open fd records the generation it was opened under: `long open_reset_gen; // Reset generation at open time` (chardev_private.h:77), assigned in `tt_cdev_open` as `private_data->open_reset_gen = atomic_long_read(&tt_dev->reset_gen);` (chardev.c:812). At probe, `reset_gen` starts at 0 (`atomic_long_set(&tt_dev->reset_gen, 0);` enumerate.c:309) and `reset_rwsem` is initialized (enumerate.c:310). `needs_hw_init` is initialized true at probe start (enumerate.c:305) and finalized as `tt_dev->needs_hw_init = !device_class->init_hardware(tt_dev);` (enumerate.c:370). Telemetry is only initialized at probe when `!needs_hw_init` (enumerate.c:382-383).

#### Concurrency model: `reset_rwsem`

- Every ioctl entry takes `reset_rwsem`: RESET_DEVICE takes it **exclusive** (`down_write`), all other ioctls **shared** (`down_read`) (chardev.c:596-601). Released at chardev.c:700-703.
- `mmap` takes it shared with **trylock**; failure returns `-ENODEV` (deliberate, to avoid an ABBA deadlock with `mmap_lock` held by the kernel while `tenstorrent_vma_zap()` needs `mmap_lock` under `reset_rwsem`) (chardev.c:713-718).
- `open` takes it shared across the deferred-powerdown cancel and initial power aggregation (chardev.c:838, 858).
- `release` takes it shared across all device-touching cleanup (chardev.c:929, 950).
- `tenstorrent_pci_remove` takes it exclusive to drain in-flight ioctls before unmapping BARs (enumerate.c:444-447).
- `LOCK_CTL ACQUIRE_BLOCKING` *drops* the shared hold across its wait and reacquires before returning, because holding it across `wait_event_interruptible` would deadlock the writer-fair rwsem against RESET_DEVICE (chardev.c:318-344).

#### Gatekeeping at ioctl/mmap entry

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

### 2. The ioctl surface: flags and struct

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

#### Handler prologue and epilogue (common to all flavors)

`ioctl_reset_device` (chardev.c:200-310), running with `reset_rwsem` held exclusive:

1. `copy_from_user(&in, ...)` → `-EFAULT` on fault (chardev.c:213-214).
2. **dmabuf -EBUSY gate**: for the destructive flags — `RESET_PCIE_LINK`, `CONFIG_WRITE`, `USER_RESET`, `ASIC_RESET`, `ASIC_DMC_RESET` — refuse with `-EBUSY` if `tenstorrent_has_tlb_dmabuf_exports(tt_dev)` (chardev.c:222-233). `RESTORE_STATE` and `POST_RESET` are exempt ("the re-init halves of a reset sequence, not destructive", chardev.c:219-221). Rationale (ioctl.h:429-433): "Resetting the device under in-flight P2P DMA can wedge the host hard enough to require out-of-band recovery, and a pin-only importer cannot be revoked... RESET_DEVICE is therefore refused with -EBUSY while any export is live." The predicate is just a locked `!list_empty(&tt_dev->dmabuf_exports)` (memory.c:1299-1308). On kernels < 5.8 the export ioctl is `-EOPNOTSUPP` and the predicate is hardwired `false` (memory.c:1312-1324).
3. `cancel_delayed_work_sync(&tt_dev->power_down_work)` — drains any deferred idle powerdown before disturbing the device; safe because open()/release() take the rwsem shared and reset holds it exclusive (chardev.c:235-238).
4. Flavor dispatch (below), producing `bool ok`.
5. `out.result = !ok;` — **result 0 = success, 1 = failure. A failed reset still returns 0 from the ioctl**; only validation errors produce negative errno (chardev.c:292-293).
6. `wake_up_interruptible(&tt_dev->resource_lock_waitqueue)` so `ACQUIRE_BLOCKING` waiters re-check `reset_gen` and return `-ENODEV` instead of waiting forever (chardev.c:295-299; waiter side chardev.c:331-363).
7. `clear_user(&arg->out, in.output_size_bytes)` then `copy_to_user` of `min(in.output_size_bytes, sizeof(out))` bytes → `-EFAULT` on fault (chardev.c:301-307). Note `output_size_bytes` is caller-controlled and un-capped; a huge value that fits in the user mapping just zeroes that much of the caller's buffer.

---

### 3. Low-level PCIe primitives (pcie.c)

#### 3.1 `poll_pcie_link_up(pdev, timeout_ms)` (pcie.c:24-41)

Polls config-space `PCI_VENDOR_ID` until it reads `PCI_VENDOR_ID_TENSTORRENT` (`0x1E52`, enumerate.h:15), sleeping `msleep(100)` between reads, bounded by `timeout_ms` via `ktime`. Returns false on timeout.

#### 3.2 `safe_pci_restore_state(pdev)` (pcie.c:43-59)

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

#### 3.3 `pcie_hot_reset_and_restore_state(pdev)` — Secondary Bus Reset (pcie.c:61-90)

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

#### 3.4 `pcie_timer_interrupt(pdev)` — config-write chip reset trigger (pcie.c:133-138)

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
(pcie.c:17-22, 133-138). Two dword writes into the device's own extended config space (Synopsys DWC PCIe controller "interface timer" registers): write `0x1` to offset 0x934, then `0x11` to offset 0x930. This forces a pending timer interrupt inside the chip, which firmware services as a reset request. Always returns true. Used by legacy `CONFIG_WRITE` (chardev.c:253) and by Blackhole `ASIC_RESET` (blackhole.c:566).

#### 3.5 Reset marker (pcie.c:140-158)

```c
// pci_command_parity is used as reset marker. Set to 1, check if cleared to 0 after reset
pci_read_config_word(pdev, PCI_COMMAND, &pci_command);
pci_write_config_word(pdev, PCI_COMMAND, pci_command | PCI_COMMAND_PARITY);
```
`set_reset_marker` sets the Parity Error Response bit (bit 6, 0x40) of `PCI_COMMAND` (offset 0x04). `is_reset_marker_zero` reads it back and returns `(pci_command & PCI_COMMAND_PARITY) == 0` (pcie.c:151-158). The scheme relies on a real chip reset clearing config space to defaults (bit 6 resets to 0); a cleared bit therefore proves the reset actually happened. `tools/reset.c` polls exactly this bit from sysfs config space (`(cmd_reg >> 6) & 1) == 0`, tools/reset.c:241-243).

#### 3.6 Dead declaration

`void pcie_retrain_link_to_max_speed(struct pci_dev *pdev);` is declared (pcie.h:16) but **defined nowhere and called nowhere** in the tree — do not port it.

> **Porting note:** On Windows, a KMDF function driver cannot legally touch the upstream bridge's config space, so the SBR in 3.3 cannot be re-implemented literally. The equivalent is the PCI bus driver's reset interface (`PCI_DEVICE_RESET_INTERFACE_STANDARD`, `DeviceReset(PLDR/FLR)` / `GUID_DEVICE_RESET_INTERFACE_STANDARD`) requesting a **bus/PLDR reset**; the 2 ms/500 ms/10 s timings above are what the hardware has been validated against and should be preserved where the port controls timing. Config save/restore (`pci_save_state`/`pci_restore_state`) must be re-implemented manually via `BUS_INTERFACE_STANDARD.GetBusData/SetBusData` (the Windows PCI driver restores config on power transitions, not on app-triggered SBR). `pcie_timer_interrupt` and the reset marker are plain config accesses to the device's *own* config space and port directly via GetBusData/SetBusData.

---

### 4. The seven flavors, end to end

All flavors run under exclusive `reset_rwsem`, after the -EBUSY gate and powerdown drain described in section 2.

#### 4.1 `RESTORE_STATE` (0, legacy) — chardev.c:240-246

No gen bump, no VMA zap, exempt from the dmabuf gate. Sequence:
1. `safe_pci_restore_state(pdev)` (restore + re-save config space). If it fails (no saved state / device absent), `ok = false`.
2. `dev_class->restore_reset_state(priv->device)` — restore Max Payload Size (section 6).
3. `ok = dev_class->init_hardware(priv->device)` — re-run device init (section 5/6).

Used by legacy UMD flows as the second half after an externally executed reset (tt-umd/device/warm_reset.cpp:290, 347).

#### 4.2 `RESET_PCIE_LINK` (1, legacy) — chardev.c:247-249

1. `tenstorrent_vma_zap(tt_dev)` — unmap all user BAR/TLB mappings device-wide (section 7).
2. `ok = pcie_hot_reset_and_restore_state(pdev)` — the SBR sequence of 3.3, including config restore.

**Does not bump `reset_gen` and does not set `needs_hw_init`** — pre-existing fds stay valid; only their mappings are gone. Legacy WH flow: `RESET_PCIE_LINK` first, then UMD talks to ARC FW itself (A3 + TRIGGER_RESET), then `RESTORE_STATE` (tt-umd/device/warm_reset.cpp:294-369). In the current arch-agnostic UMD flow it is only sent when the caller explicitly requests a secondary bus reset (tt-umd/device/warm_reset.cpp:207-209).

#### 4.3 `CONFIG_WRITE` (2, legacy) — chardev.c:250-253

1. `bump_reset_gen(priv)` — all other fds invalidated; caller's fd carried forward.
2. `tenstorrent_vma_zap(tt_dev)`.
3. `ok = pcie_timer_interrupt(pdev)` (3.4) — fires the chip-internal reset; always true.

**Does not set `needs_hw_init`.** Legacy BH flow: `CONFIG_WRITE`, poll config command byte until reset observed (up to `BH_WARM_RESET_TIMEOUT`), then `RESTORE_STATE` (tt-umd/device/warm_reset.cpp:241-292).

#### 4.4 `USER_RESET` (3) — chardev.c:254-258

1. `bump_reset_gen(priv)`; 2. `tenstorrent_vma_zap(tt_dev)`; 3. `ok = set_reset_marker(pdev)` (always true); 4. `needs_hw_init = true`.

No hardware reset is performed by the driver — the *user* performs the reset by other means (e.g. tt-smi driving FW directly); the driver only arms the marker and enters the reset window. Completion is via `POST_RESET`.

#### 4.5 `ASIC_RESET` (4) — chardev.c:259-263

1. `bump_reset_gen(priv)`; 2. `tenstorrent_vma_zap(tt_dev)`; 3. `ok = dev_class->reset(priv->device, in.flags)`; 4. `needs_hw_init = true`.

**Wormhole** (`wormhole_reset`, wormhole.c:473-518):
- Probe responsiveness with FW message `WH_FW_MSG_NOP` (0x11), 1000 µs timeout (wormhole.c:481).
- If unresponsive: if `auto_reset_timeout == 0`, fail immediately with "Watchdog is disabled and device is unresponsive, cannot reset." (wormhole.c:488-491). Otherwise loop until `auto_reset_timeout*1000 + 500` ms elapse: `pcie_hot_reset_and_restore_state(pdev)`, retry NOP (1000 µs), `msleep_interruptible(1000)` between attempts (a signal aborts with `ok=false`) (wormhole.c:493-504). This rides the on-board M3 watchdog auto-reset: keep hot-resetting the link until FW comes back.
- If responsive: `set_reset_marker(pdev)`, then send `WH_FW_MSG_TRIGGER_RESET` (0x56) with `arg0 = 0` for ASIC_RESET (3 for DMC, see 4.6) and **`timeout_us = 0`, i.e. fire-and-forget**; return true — "Assumes the reset was successful." (wormhole.c:508-512). (With `timeout_us == 0` the send helper returns false right after raising the ARC IRQ, wormhole.c:226-227; the return value is deliberately ignored.)
- FW message transport: write `arg0|arg1<<16` to reset-unit `SCRATCH_REG(3)`, write `0xAA00 | msg_id` to `SCRATCH_REG(5)`, set bit 16 of `ARC_MISC_CNTL_REG` (0x100) to raise IRQ0, then poll SCRATCH_REG(5) for the msg id in the low 16 bits with exit code in the high 16 (wormhole.c:108-112, 205-234, 163-197). Skipped entirely (returns false) if ARC L2 post code (`SCRATCH_REG(0)` masked with 0xFFFF0000) != `0xC0DE0000` (wormhole.c:101-103, 214-217).

**Blackhole** (`blackhole_reset`, blackhole.c:542-570): `ASIC_RESET` is `set_reset_marker(pdev)` + `pcie_timer_interrupt(pdev)` (blackhole.c:564-567) — the config-write trigger, no FW handshake.

#### 4.6 `ASIC_DMC_RESET` (5) — chardev.c:264-268

Same wrapper as 4.5 (bump gen, zap, `dev_class->reset`, `needs_hw_init = true`).

**Wormhole**: identical to 4.5 except `reset_arg = 3` passed to `WH_FW_MSG_TRIGGER_RESET` (wormhole.c:477, 510-511); the "ASIC + M3 reset" meaning of argument 3 is documented in blackhole.c:549 for the same firmware argument.

**Blackhole** (blackhole.c:547-563): via the ARC message queue (not scratch registers):
1. Send `ARC_MSG_TYPE_TEST` (0x90) to confirm FW/NOC alive; on failure log "Couldn't communicate with firmware; NOC is likely hung." and return false (blackhole.c:551-555).
2. `set_reset_marker(pdev)` (blackhole.c:557).
3. Send `ARC_MSG_TYPE_TRIGGER_RESET` (0x56) with `payload[0] = 3` ("Argument for ASIC + M3 reset") and return true — "Possibly a lie..." (blackhole.c:559-563). Queue transport: wait up to `ARC_MSG_READY_MS` (500 ms) for `ARC_BOOT_STATUS` bit 0, read QCB pointer from `RESET_SCRATCH(11)`, push, ring doorbell by writing 0 to `ARC_MSI_FIFO` (0x800B0000), pop reply (blackhole.c:500-540).

#### 4.7 `POST_RESET` (6) — the completion path — chardev.c:269-287

This is the narrow "post-reset fd" exception. Exact semantics:

**Who may call it, on which fd.** Any fd of the device that passes the standard gate: device not `detached`, and `open_reset_gen == reset_gen` (chardev.c:604-613). Because the flavors that enter the reset window (USER_RESET/ASIC_RESET/ASIC_DMC_RESET, and legacy CONFIG_WRITE) bump the generation, that means exactly two kinds of fd: (a) **the resetter's own fd** (carried forward by `bump_reset_gen`), or (b) **any fd opened after the reset ioctl returned**. It is reachable during the reset window because `RESET_DEVICE` is on the `needs_hw_init` allowlist (chardev.c:616-624), and it is exempt from the dmabuf `-EBUSY` gate (chardev.c:231-232). No capability/ownership check beyond that — any process that can open the node can complete a reset. In practice both reference consumers open a **fresh fd** (UMD: `tt_device_open(..., O_RDWR|O_CLOEXEC|O_APPEND)` per reset call, tt-umd/device/pcie/pci_device.cpp:200-215; tools/reset.c re-finds the device by BDF after it re-enumerates and opens the new node, tools/reset.c:260-289).

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

#### End-to-end reference flow (current, arch-agnostic)

From tt-umd/device/warm_reset.cpp:182-239 and tools/reset.c:196-289:

1. (optional) `RESET_PCIE_LINK` if a secondary bus reset was requested (warm_reset.cpp:207-209).
2. `ASIC_DMC_RESET` if M3/DMC reset requested, else `ASIC_RESET` — each on a freshly opened fd, immediately closed (warm_reset.cpp:211-215; pci_device.cpp:200-215).
3. Wait: UMD sleeps `max(2.0, 0.4 * ndevices)` seconds (or the M3 delay) then polls sysfs for the BDF to reappear (warm_reset.cpp:217-235); tools/reset.c polls the marker bit / device disappearance for 5 s (ASIC) or 10 s (DMC), 100 ms period, with a 500 ms pre-delay on WH (tools/reset.c:218-255).
4. `POST_RESET` on a newly opened fd (warm_reset.cpp:237; tools/reset.c:275-288), checking `out.result == 0`.

---

### 5. Effects on other open fds, mmaps, locks, power

- **Other fds:** after a gen bump, every ioctl and mmap on a pre-reset fd returns `-ENODEV` forever (chardev.c:609-613, 726-729). Blocking lock waiters are woken and return `-ENODEV` (chardev.c:295-299, 353-360). `close()` still works and performs full cleanup (`tt_cdev_release`, chardev.c:922-958) — NOC cleanup write is skipped only when `detached` (chardev.c:869), power re-aggregation is skipped when `detached || needs_hw_init` (chardev.c:907-908), and aggregation itself skips stale-generation fds (chardev.c:493-495).
- **Mappings:** `tenstorrent_vma_zap` walks every open fd's `vma_list` under `chardev_mutex` + per-fd `vma_lock`, takes `mmget_not_zero`/`mmap_read_lock` in the VFIO-derived ordering, and `zap_special_vma_range()`s each BAR/TLB VMA (memory.c:1677-1742). The BAR/TLB vm_ops have **no `.fault` handler** (memory.c:1436-1439, 1484-1492), so a post-zap access faults fatally (SIGBUS) rather than re-populating. DMA-buffer mmaps (`dma_mmap_coherent`) are not on `vma_list` and are not zapped.
- **Resource locks:** survive reset — device-global bits are *not* cleared by reset. A pre-reset holder can no longer release via ioctl (its fd is invalid); only `close(fd)` clears its bits (comment chardev.c:312-316, cleanup chardev.c:877-885).
- **Power:** the deferred idle-powerdown work is drained before the reset (chardev.c:238); new arms cannot race in because open/release hold the rwsem shared (comment chardev.c:236-237).

> **Porting note:** `tenstorrent_vma_zap` is the hardest piece to port. Windows has no zap-PTEs primitive for user mappings of device memory created by a driver. Options: (a) map BARs to user space via `MmMapLockedPagesSpecifyCache`+MDL and use `MmUnmapLockedPages` on reset (requires tracking every mapping and works only if the port controls unmap), or (b) rotate to a section-object design where reset marks the section invalid — in either case revocation-on-reset must be designed in from the start, or the port must instead *refuse* destructive resets while any user mapping exists (stricter than Linux, which forcibly revokes).

---

### 6. `save_reset_state` / `restore_reset_state` — what config restore does *not* cover

The chip's Max Payload Size lives in the endpoint's Device Control register but is negotiated by FW and is not recoverable from the host-side `pci_save_state` snapshot alone after a FW-level reset, so the driver snapshots it out-of-band via the chip's own DBI view of its config space:

- **Wormhole** (wormhole.c:968-988): `open_dbi()` writes `DBI_ENABLE` (0x00200000) into reset-unit scratch 6/7 (`PCIE_ARMISC_INFO_REG`/`PCIE_AWMISC_INFO_REG`), then a NOC read/write at node (0,3), address `PCIE_DBI_ADDR = 0x800000000ULL` + `DBI_DEVICE_CONTROL_DEVICE_STATUS` (0x78, pcie.h:8) captures/restores the `PCI_EXP_DEVCTL_PAYLOAD` field into/from `wh->saved_mps`; `close_dbi()` writes 0 back. The comment warns DBI mode disrupts all outbound NOC traffic, so this only happens when quiescent (wormhole.c:956-957).
- **Blackhole** (blackhole.c:304-330): same idea; PCIe NOC x-coordinate detected via `NOC_ID_OFFSET` read (must be 2 or 11, blackhole.c:299-302), DBI at `PCIE_DBI_ADDR = 0xF800000000000000ULL` (an outbound NOC TLB set up by FW, blackhole.c:50-51).

`save_reset_state` is called once at probe (enumerate.c:373); `restore_reset_state` in `RESTORE_STATE` and `POST_RESET` (chardev.c:242, 277).

`init_hardware`, re-run by `RESTORE_STATE`/`POST_RESET`/resume:
- **Wormhole** (wormhole.c:718-735): remap BAR4 through iATU inbound region 1 to `BAR4_SOC_TARGET_ADDRESS` (0x1E000000); if ARC L2 running: send current date (0xB7), `ASTATE0` (0xA0, 10 000 µs), device index (0x51), then `wormhole_complete_pcie_init` (section 8), then `UPDATE_M3_AUTO_RESET_TIMEOUT` (0xBC) with `auto_reset_timeout` seconds (10 000 µs timeout) (wormhole.c:729-731). Always returns true.
- **Blackhole** (blackhole.c:620-639): `pcie_set_readrq(pdev, 4096)` (MAX_MRRS, blackhole.c:18), ARC message `ASIC_STATE0` (0xA0), then `SET_WDT_TIMEOUT` (0xC1) with `1000 * auto_reset_timeout` ms (failure tolerated for old FW). Always returns true.

---

### 7. Module parameters

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

### 8. Boot-time PCIe link training loop — `wormhole_complete_pcie_init` (pcie.c:92-131)

Wormhole-only (called from `wormhole_init_hardware`, wormhole.c:728; Blackhole has no equivalent). Skipped when there is no upstream bridge or `reset_limit == 0`. For up to `reset_limit` iterations:

1. Read the **bridge's** `PCI_EXP_LNKCTL2` and mask `PCI_EXP_LNKCTL2_TLS` (Target Link Speed, low 4 bits) (pcie.c:107-108).
2. Read the **bridge's** config word at `PCI_SUBSYSTEM_VENDOR_ID` (0x2C) (pcie.c:110) — see Open questions.
3. Send WH FW message `FW_MSG_PCIE_RETRAIN` (0xB6, pcie.c:16) with `arg0 = target_link_speed | (last_retry << 15)`, `arg1 = subsys_vendor_id`, timeout **200 000 µs**, collecting a 16-bit exit code (pcie.c:112-113). The FW performs the actual link retraining from the endpoint side.
4. `exit_code == 0` → success. Otherwise, unless this was the last retry: `pci_save_state(pdev)` then `pcie_hot_reset_and_restore_state(pdev)` (full SBR of 3.3) and loop (pcie.c:116-127).

Failure of the message send or exhaustion of retries returns false — but note `wormhole_init_hardware` ignores the return value and still returns true (wormhole.c:728, 734).

---

### 9. Reboot notifier, shutdown, suspend/resume, remove

- **Reboot notifier** (enumerate.c:243-251): registered at probe only when the class has a `.reboot` op (enumerate.c:377-380) — Wormhole only (`.reboot = wormhole_cleanup_hardware`, wormhole.c:1073; Blackhole registers none, blackhole.c:813-840). Handler: `if (action != SYS_POWER_OFF) tt_dev->dev_class->reboot(tt_dev);` → on restart/halt (but *not* power-off) send WH FW `ASTATE3` (0xA3) with 10 000 µs timeout, skipped if hardware hung (`wormhole_shutdown_firmware`, wormhole.c:293-301) and skipped entirely when `detached` (`wormhole_cleanup_hardware` guard, wormhole.c:789-790). Unregistered at final kref release (enumerate.c:487-488).
- **`.shutdown = tenstorrent_pci_remove`** (enumerate.c:532): system shutdown runs the full remove path.
- **Remove** (enumerate.c:404-481), reset-relevant ordering: cancel WH fw_ready work; set `detached = true` under `chardev_mutex`; drain `power_down_work`; probe vendor ID and call `cleanup_hardware` (FW → A3) only if it reads != 0xFFFF (hotplug-gone check, enumerate.c:432-434); `cleanup_telemetry`; **`down_write(&reset_rwsem)`; `tenstorrent_vma_zap`; `cleanup_device` (unmap BARs); `up_write`** (enumerate.c:444-447); `tenstorrent_revoke_tlb_dmabufs` (must stay after the drain — long ordering comment enumerate.c:449-458); wake lock waiters; per-fd `tenstorrent_memory_cleanup`; unregister cdev; disable interrupts/device.
- **Suspend** (enumerate.c:499-509): drain powerdown work, `tenstorrent_revoke_tlb_dmabufs`, `cleanup_hardware` (FW → A3). **Resume** (enumerate.c:511-522): `init_hardware`; on success `pci_save_state(pdev)` ("Suspend invalidates the saved state"); returns `-EIO` on failure. Resume does **not** bump `reset_gen` — fds survive suspend/resume.

> **Porting note:** reboot notifier ≈ `EvtDeviceShutdown`/`IRP_MJ_SHUTDOWN` (note Linux deliberately *skips* the A3 message on power-off; Windows shutdown callbacks can distinguish shutdown vs. restart only coarsely). Suspend/resume ≈ D0Exit/D0Entry; the "re-save config after resume" step is unnecessary on Windows only if config restore is delegated to the PCI driver, but the DBI-based MPS restore and `init_hardware` re-run are still required.

---

### Key constants table

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

### Open questions

1. **`pcie_retrain_link_to_max_speed`** is declared (pcie.h:16) but never defined or called — presumed dead; confirm before dropping from the port.
2. **Bridge "subsystem vendor ID" read** in `wormhole_complete_pcie_init` (pcie.c:110): `PCI_SUBSYSTEM_VENDOR_ID` (0x2C) on a type-1 (bridge) header is not a Subsystem Vendor ID field (it falls in the prefetchable-limit-upper area). The value is forwarded verbatim to FW as `arg1`; whether FW expects this specific bridge register or the code is a copy-paste of an endpoint offset is unknown. A port must read *bridge config offset 0x2C* to be bit-faithful, but the intent is unverified.
3. **`CONFIG_WRITE` bumps `reset_gen` but does not set `needs_hw_init`**, and **`RESET_PCIE_LINK` zaps VMAs but bumps nothing** (chardev.c:247-253). Both look like frozen legacy-compat behavior (matching UMD's legacy flows that follow with `RESTORE_STATE`); whether a Windows port must reproduce this exact asymmetry or may unify on the flag 3-6 model depends on whether legacy UMD flows must be supported.
4. **`POST_RESET` clears `needs_hw_init` unconditionally** before attempting re-init (chardev.c:274-275): a *failed* POST_RESET (marker still set or restore/init failure) leaves the device out of the reset window with possibly uninitialized hardware, and subsequent ioctls are permitted. It is unclear whether this is intentional ("caller saw result=1, must retry a full reset") or an accepted gap.
5. **`mmap` does not check `needs_hw_init`** (chardev.c:708-736): a post-reset-generation fd can map BARs during the reset window before POST_RESET re-initializes the chip. Probably benign (BARs are restored config-wise), but the asymmetry with the ioctl allowlist is undocumented.
6. **Semantics of config registers 0x930/0x934** ("interface timer") are not documented in-tree; the driver treats "write 0x1 to 0x934 then 0x11 to 0x930" as an opaque reset trigger serviced by chip firmware. The port must reproduce the writes exactly; no independent description of the hardware behavior is available in these repos.
7. **WH `TRIGGER_RESET` is fire-and-forget** (`timeout_us = 0`, return value ignored, wormhole.c:510-512): the ioctl reports success without confirmation the FW accepted the reset ("Assumes the reset was successful", "Possibly a lie..." on BH, blackhole.c:563). Windows error reporting should preserve this optimism rather than invent stricter checking, since callers (UMD/tools) compensate by polling the marker/BDF afterwards.
8. **UMD legacy BH poll** checks command-register bit 1 (memory-space enable) rising (tt-umd/device/warm_reset.cpp:258-259) rather than the parity marker used by the current flow — the mechanism that sets that bit post-reset (FW?) is not visible from these repos. Only relevant if legacy flows must be supported against the Windows KMD.
9. **`reset_limit`/`auto_reset_timeout` are module-wide**, not per-device. If the Windows port makes them per-device registry values, multi-device systems could diverge from Linux behavior — probably fine, but note the difference.

---

## 12. KMD Tests and Tools (test/, tools/, docs/, contrib/)

### Scope

Files covered (all read in full; line counts from `wc -l`):

Test suite (`test/`):
- test/main.cpp (77), test/Makefile (37), test/.gitignore (2)
- test/enumeration.h (28), test/enumeration.cpp (167)
- test/devfd.h (20), test/devfd.cpp (30)
- test/util.h (72), test/util.cpp (210)
- test/test_failure.h (27), test/test_failure.cpp (14)
- test/aligned_allocator.h (38)
- test/get_driver_info.cpp (71), test/get_device_info.cpp (66)
- test/config_space.cpp (165), test/query_mappings.cpp (262)
- test/dma_buf.cpp (175), test/pin_pages.cpp (447)
- test/lock.cpp (490), test/hwmon.cpp (93)
- test/ioctl_overrun.cpp (212), test/ioctl_zeroing.cpp (107)
- test/map_peer_bar.cpp (167), test/tlbs.h (117), test/tlbs.cpp (634)
- test/dmabuf_export.cpp (350), test/release.cpp (126)
- test/mappings_debugfs.cpp (318), test/procfs_pids.cpp (164)
- test/excl.cpp (332)
- test/checkws (38), test/checkws-tests (8), test/pahole_check.sh (125)
- test/run-hardware-tests.sh (152), test/mass-build-test (163)

Tools (`tools/`):
- tools/reset.c (294), tools/power.c (283)
- tools/rdma/nic_bh_p2p_dma.c (657), tools/rdma/Makefile (15)
- tools/fix-tt-hotplug-bars (111)
- tools/current-version (28), tools/exclude-from-release (8)
- tools/installer-header.sh (35), tools/make-installer (60), tools/make-source-release (67)
- tools/build_debs.sh (154), tools/build_rpms.sh (192)

Docs (`docs/`):
- docs/sysfs-attributes.md (150)

Contrib (`contrib/`):
- contrib/packaging/MAINTAINERS.md (5), contrib/packaging/README.md (9)
- contrib/packaging/nix/ci.sh (11), contrib/packaging/nix/overlay.nix (32)

The test suite (`ttkmd_test`) is the closest thing tt-kmd has to executable ABI documentation. Each test below is enumerated with the exact semantics it asserts, including error codes, so a Windows port can treat these as the conformance contract.

---

### 1. Test harness structure

#### 1.1 Build and entry point

`ttkmd_test` is a single C++17 userspace binary built with `-O2 -Wall -Wno-narrowing` (test/Makefile:19-20). It includes the driver's `ioctl.h` directly (e.g. test/get_driver_info.cpp:9), so it is compiled against the exact uAPI header of the driver under test.

`main()` (test/main.cpp:30-77):
- Accepts one optional argument `--skip-aer`, which disables the AER check because "When running inside a VM aer seems to be disabled" (test/main.cpp:34-37).
- Enumerates devices and runs, per device, in order: `TestGetDriverInfo, TestGetDeviceInfo, TestConfigSpace, TestQueryMappings, TestDmaBuf, TestNocDmaBuf, TestPinPages, TestLock, TestHwmon, TestIoctlOverrun, TestIoctlZeroing, TestTlbs, TestTlbExport, TestMappingsDebugfs, TestProcfsPids, TestExcl, TestDeviceRelease` (test/main.cpp:44-60).
- Then runs `TestMapPeerBar(devs[i], devs[j])` over the full cartesian product of devices, *including i==j* (test/main.cpp:65-71).
- Fails if no device was found: `THROW_TEST_FAILURE("No devices found.")` (test/main.cpp:73-74).

Failures are reported by throwing `test_failure` (a `std::runtime_error` carrying file/line/function; test/test_failure.h:9-27) — the first failure aborts the whole run (no catch in main).

#### 1.2 Device enumeration (itself a test)

`EnumerateDevices()` (test/enumeration.cpp:142-167) cross-checks the driver's userspace surface:
- Every character-device entry in `/dev/tenstorrent/` must have a sysfs `subsystem` link resolving to `tenstorrent` (test/enumeration.cpp:30-36, 46-59); non-char-device entries are silently skipped (test/enumeration.cpp:53-54). A char device there that is *not* bound to the tenstorrent driver is a failure (test/enumeration.cpp:56-57).
- Every PCI device with vendor ID `0x1E52` (test/enumeration.cpp:82) must have exactly one `tenstorrent/` class-device subdirectory containing a `dev` file with `MAJOR:MINOR` (test/enumeration.cpp:93-109). Zero nodes (test/enumeration.cpp:94-95) or more than one (test/enumeration.cpp:97-98) is a failure.
- The set of dev_t from `/dev/tenstorrent` must exactly equal the set derived from PCI sysfs: "PCI devices and driver-reported devices do not match." (test/enumeration.cpp:155-156).
- Device type is determined by PCI device ID: `0x401e → Wormhole`, `0xb140 → Blackhole`; any other DID is a hard error (test/enumeration.cpp:133-139).
- IOMMU translation state is detected by reading `/sys/bus/pci/devices/<bdf>/iommu_group/type` and checking for a `"DMA"` prefix (test/enumeration.cpp:115-126); failure to read means "not translated". This flag changes expected PIN_PAGES semantics (section 2.7).

> **Porting note:** On Windows the enumeration layer maps to SetupDi/CM_* interface enumeration by device interface GUID plus querying bus/device/function via `DEVPKEY_Device_BusNumber`/`Address`. The invariant to preserve: every TT PCI function has exactly one user-visible device object, and device type is derived from PCI DID (0x401e/0xb140). The IOMMU-translated bit maps to whether DMA remapping (IOMMU/DMA guard) is active for the device — the port must expose or internally know the equivalent because PIN_PAGES contiguity rules depend on it.

#### 1.3 Support utilities

- `DevFd` opens the node `O_RDWR | O_CLOEXEC` and closes on destruction (test/devfd.cpp:13-24).
- `util.cpp` provides sysfs helpers, `page_size()` via `sysconf(_SC_PAGE_SIZE)` (test/util.cpp:140-143), an unlinked temp file (test/util.cpp:156-178), and an anonymous POSIX shm object (test/util.cpp:194-210, used to construct physically-discontiguous mappings).
- `AlignedAllocator` wraps `std::aligned_alloc` for placing ioctl structs at controlled alignment (test/aligned_allocator.h:10-38).

---

### 2. Per-test ABI semantics (executable ABI documentation)

#### 2.1 TestGetDriverInfo (test/get_driver_info.cpp)

Asserts, for `TENSTORRENT_IOCTL_GET_DRIVER_INFO` with `in.output_size_bytes = sizeof(out)`:
- ioctl returns 0 (test/get_driver_info.cpp:42-43).
- `out.output_size_bytes >= offsetof(out, driver_version) + sizeof(driver_version)` — the driver reports at least up through the legacy version field (test/get_driver_info.cpp:45-50).
- `out.output_size_bytes <= sizeof(out)` — the driver never claims more output than the current header defines (test/get_driver_info.cpp:52-53).
- `out.driver_version == TENSTORRENT_DRIVER_VERSION` (interface version 2; ioctl.h:10) (test/get_driver_info.cpp:55-56).
- `driver_version_major/minor/patch` must equal the semver parsed from `/sys/module/tenstorrent/version` (test/get_driver_info.cpp:58-70); parsing uses the full semver.org regex (test/get_driver_info.cpp:21).

> **Porting note:** The cross-check source (`/sys/module/tenstorrent/version`) is Linux-specific; a Windows conformance test would compare against the driver file version resource or a registry value. The ioctl-side assertions port directly.

#### 2.2 TestGetDeviceInfo (test/get_device_info.cpp)

For `TENSTORRENT_IOCTL_GET_DEVICE_INFO`:
- ioctl returns 0; `out.output_size_bytes >= offsetof(out, pci_domain)+sizeof(pci_domain)` ("pci_domain has been present since 1.23", test/get_device_info.cpp:26-32).
- `vendor_id`, `device_id`, `subsystem_vendor_id`, `subsystem_id` must match PCI config space (read via sysfs) (test/get_device_info.cpp:34-50).
- BDF encoding contract (test/get_device_info.cpp:52-59):
  ```c
  unsigned bus = (get_device_info.out.bus_dev_fn >> 8) & 0xFF;
  unsigned device = (get_device_info.out.bus_dev_fn >> 3) & 0x1F;
  unsigned function = get_device_info.out.bus_dev_fn & 0x7;
  unsigned domain = get_device_info.out.pci_domain;
  ```
- `12 <= max_dma_buf_size_log2 <= 63` (test/get_device_info.cpp:61-65).

#### 2.3 TestConfigSpace (test/config_space.cpp)

Verifies driver-programmed PCI config state by reading `/sys/bus/pci/devices/<bdf>/config`:
- `Command.MemorySpaceEnable` (bit 1) and `Command.BusMasterEnable` (bit 2) at config offset 4 must both be set (test/config_space.cpp:22-24, 88-98).
- MSI capability (cap ID 5) must exist, be enabled (`message_control & 1`), and have a nonzero message address (accounting for 64-bit addressing via `message_control & 0x80`) (test/config_space.cpp:31-36, 100-118).
- PCIe capability (cap ID 0x10): at Device Control (offset 8 into the cap), at least one of correctable/non-fatal/fatal/unsupported error-reporting-enable bits (0x1|0x2|0x4|0x8) must be set — "AER is disabled." otherwise; skipped with `--skip-aer` (test/config_space.cpp:38-43, 120-138).
- If the kernel rejects config reads beyond 64 bytes for non-root, the MSI/AER checks are skipped with a console message (test/config_space.cpp:148-164).

> **Porting note:** These are assertions about what the *driver* must program (memory space, bus mastering, MSI, AER), not about the ioctl surface. On Windows most of this is done by the PCI bus driver/KMDF (`WdfDeviceQueryInterruptProperty`, MSI configured via INF/interrupt resources); the conformance equivalent is verifying the device is receiving interrupts and BME is on.

#### 2.4 TestQueryMappings (test/query_mappings.cpp)

The test's own header comment enumerates the contract (test/query_mappings.cpp:4-10). Query procedure: caller passes `in.output_mapping_count`; the buffer is `sizeof(tenstorrent_query_mappings) + count*sizeof(tenstorrent_mapping)` (test/query_mappings.cpp:182-199); the test doubles count starting at 16 until the last returned entry is `TENSTORRENT_MAPPING_UNUSED` (test/query_mappings.cpp:201-213). Asserted invariants:
- Only known mapping IDs appear: `UNUSED`, `RESOURCE{0,1,2}_UC`, `RESOURCE{0,1,2}_WC` (test/query_mappings.cpp:35-53).
- All `UNUSED` entries are at the end of the output array (test/query_mappings.cpp:55-63).
- No non-UNUSED mapping ID appears more than once (test/query_mappings.cpp:65-73).
- `RESOURCE0_UC` is always present (test/query_mappings.cpp:75-79).
- If `RESOURCEi_WC` is present, `RESOURCEi_UC` must also be present (test/query_mappings.cpp:81-101).
- Mappings must not overlap and must not wrap around 2^64 (test/query_mappings.cpp:104-129).
- Every non-UNUSED mapping: `mapping_size > 0`, `mapping_size % page_size == 0`, `mapping_base % page_size == 0` (test/query_mappings.cpp:133-147).
- `mapping_base + mapping_size < (1ULL << 44)` — "32 + log(PAGE_SIZE)" so a 32-bit `mmap` offset (in pages) can address every mapping (test/query_mappings.cpp:153-157).
- Prefix property: querying with `output_mapping_count = i` for every i up to the full count must return exactly the first i entries of the full result (id, base, and size identical) (test/query_mappings.cpp:215-229).
- Every non-UNUSED mapping must be mmap-able `PROT_READ|PROT_WRITE, MAP_SHARED` at `offset = mapping_base`, `length = mapping_size`, and munmap-able (test/query_mappings.cpp:231-245).

Note: `VerifyNoOverlap` builds a base-sorted copy (`base_sorted_mappings`) but then iterates the *unsorted* `mappings` vector in the overlap loop (test/query_mappings.cpp:114-128) — apparently a test bug; the intended invariant is clearly "no overlap among non-UNUSED mappings".

> **Porting note:** This is the core mmap-offset contract. A Windows port that keeps `QUERY_MAPPINGS` must keep all these invariants, with `mmap` replaced by whatever mapping call the port defines (e.g. an ioctl that returns a user VA, or a section object). The 2^44 limit exists for 32-bit Linux mmap offsets; a Windows port should decide whether to preserve it (harmless) or document divergence.

#### 2.5 TestDmaBuf / TestNocDmaBuf (test/dma_buf.cpp)

For `TENSTORRENT_IOCTL_ALLOCATE_DMA_BUF`:
- Allocation of index 0 at up to `1 << max_dma_buf_size_log2` bytes must succeed; on `ENOMEM` the test halves the size and retries down to one page (test/dma_buf.cpp:31-77) — i.e. `ENOMEM` is the expected error for allocation failure.
- Re-allocating an already-used `buf_index` fails with exactly `EINVAL` (test/dma_buf.cpp:144-150).
- One page can be allocated at *every* index `1..TENSTORRENT_MAX_DMA_BUFS-1` (256 buffers total; ioctl.h:41) (test/dma_buf.cpp:152-164).
- `buf_index == TENSTORRENT_MAX_DMA_BUFS` fails with exactly `EINVAL` (test/dma_buf.cpp:79-89).
- Every allocated buffer is mmap-able at `out.mapping_offset` for `out.size` bytes, `PROT_READ|PROT_WRITE, MAP_SHARED`; memory is writable and retains data across per-buffer mappings (test/dma_buf.cpp:91-116).
- `TestNocDmaBuf`: two buffers with `flags = TENSTORRENT_ALLOCATE_DMA_BUF_NOC_DMA` (=2, ioctl.h:89) at indices 0 and 1 must both succeed on a fresh fd (test/dma_buf.cpp:118-127, 171-175) — i.e. multiple NOC-visible (iATU-mapped) buffers must coexist.

#### 2.6 TestIoctlOverrun (test/ioctl_overrun.cpp)

Technique: place the ioctl input at the very end of a page followed by a `PROT_NONE` guard page (test/ioctl_overrun.cpp:31-86). Contract asserted (header comment, test/ioctl_overrun.cpp:1-9):
- For ioctls with an `output_size_bytes` field, passing `output_size_bytes = 0` with only the *in* struct mapped must produce **no output written and no error** — any `EFAULT` means the driver read or wrote beyond what userspace provided (test/ioctl_overrun.cpp:92-106).
- Applied with expected success (errno 0): `GET_DEVICE_INFO`, `QUERY_MAPPINGS` (with `output_mapping_count = 0`), `ALLOCATE_DMA_BUF` (full struct at page end), `GET_DRIVER_INFO`, `RESET_DEVICE` (flags `TENSTORRENT_RESET_DEVICE_RESTORE_STATE`), `PIN_PAGES`, `LOCK_CTL` (TEST op) (test/ioctl_overrun.cpp:108-178).
- `FREE_DMA_BUF` is expected to fail with `EINVAL` but must not `EFAULT` (test/ioctl_overrun.cpp:134-139).
- `MAP_PEER_BAR` with `peer_fd = <same fd>` is expected to fail with `EINVAL` but must not `EFAULT` (test/ioctl_overrun.cpp:180-194).
- `GET_HARVESTING` is skipped: "simply fails" (test/ioctl_overrun.cpp:203).

> **Porting note:** This maps naturally to METHOD_BUFFERED vs METHOD_NEITHER decisions. The Linux driver copies exactly `output_size_bytes` out; a KMDF port using METHOD_BUFFERED gets kernel-managed buffering, but the *semantic* contract — output truncated to caller-specified `output_size_bytes`, never touching memory beyond it — must be preserved because UMD passes these sizes. The guard-page test technique ports directly via `VirtualAlloc` + `PAGE_NOACCESS`.

#### 2.7 TestIoctlZeroing (test/ioctl_zeroing.cpp)

Contract (test/ioctl_zeroing.cpp:1-2): "When the actual output data is smaller than output_size_bytes, the remainder must be zero-filled." Technique: buffer of `offsetof(IoctlData, out) + page_size()` filled with `0xFF`; after the ioctl, every byte past `sizeof(IoctlData)` must be zero (test/ioctl_zeroing.cpp:28-42). Applied to: `GET_DEVICE_INFO`, `GET_DRIVER_INFO`, `RESET_DEVICE` (RESTORE_STATE), `PIN_PAGES`, `LOCK_CTL(TEST)` with `output_size_bytes = page_size()` (test/ioctl_zeroing.cpp:44-89). Explicit exclusions documented in the test: GET_HARVESTING "simply fails"; QUERY_MAPPINGS "complicated, has its own test"; ALLOCATE_DMA_BUF, FREE_DMA_BUF, MAP_PEER_BAR "does not zero" (test/ioctl_zeroing.cpp:98-106).

#### 2.8 TestPinPages (test/pin_pages.cpp)

Contract summary in header comment (test/pin_pages.cpp:4-11). For `TENSTORRENT_IOCTL_PIN_PAGES`:
- flags 0 and `TENSTORRENT_PIN_PAGES_CONTIGUOUS` (=1, ioctl.h:169) each succeed pinning a single page (test/pin_pages.cpp:49-73).
- `flags = ~TENSTORRENT_PIN_PAGES_CONTIGUOUS` must fail (returns -1; errno not asserted) (test/pin_pages.cpp:75-97).
- `size == 0` and `size == page_size/2` must fail (test/pin_pages.cpp:99-135) — size must be a nonzero page multiple.
- Pinning an unmapped VA range, or a range that is only partially mapped, must fail (test/pin_pages.cpp:137-179).
- 1024 separate single-page pins on one fd must all succeed (`max_pinned_ranges = 1024`, test/pin_pages.cpp:181-206).
- A hugepage (any size found under `/sys/kernel/mm/hugepages`) pins successfully with `CONTIGUOUS` (test/pin_pages.cpp:208-258); skipped with a message if no hugepages can be allocated.
- Discontiguity check (test/pin_pages.cpp:260-352): two shm pages are mapped twice, second time in swapped order, so at most one of the two orderings can be physically contiguous. Flags used: `0` when IOMMU-translated, `CONTIGUOUS` otherwise (test/pin_pages.cpp:301).
  - IOMMU on: **both** pins must succeed — discontiguous pinning is allowed (test/pin_pages.cpp:334-345).
  - IOMMU off: **at most one** may succeed; both may fail (test/pin_pages.cpp:347-351).
- `TENSTORRENT_IOCTL_UNPIN_PAGES`: unpinning with exact `virtual_address`/`size` of a prior pin succeeds (test/pin_pages.cpp:356-384); `size = 0`, `size = page_size/2` and `size = page_size*2` (superset of the pinned range) must all fail (test/pin_pages.cpp:386-434) — unpin must match the pinned region exactly.

Note test/pin_pages.cpp:270: "6.8 fails to pin temporary files but works with shared memory objects" — the pinnability of file-backed pages varies by kernel; the ABI contract is only asserted for anonymous/shm memory.

#### 2.9 TestLock (test/lock.cpp)

For `TENSTORRENT_IOCTL_LOCK_CTL` (ops: ACQUIRE=0, RELEASE=1, TEST=2, ACQUIRE_BLOCKING=3; ioctl.h:211-214). Result convention: `out.value != 0` = acquired/released; TEST returns `bit0 = held by this fd (LOCK_LOCAL=0b01)`, `bit1 = held by anyone (LOCK_GLOBAL=0b10)` (test/lock.cpp:27-28). Assertions (test/lock.cpp:97-208):
1. Acquire then release works (out.value=1 both times).
2. Releasing an unheld lock returns out.value=0 (not an ioctl error).
3. An fd cannot release a lock held by a different fd (returns 0).
4. Locks are **not re-entrant**: second acquire on the same fd returns 0 (test/lock.cpp:120-126).
5. Locks are exclusive across fds.
6. TEST: holder sees `LOCK_LOCAL|LOCK_GLOBAL` (=3); non-holder sees `LOCK_GLOBAL` (=2) (test/lock.cpp:136-149).
7. Lock indices are independent.
8. Closing the fd auto-releases its locks; the lock is then free (`TEST == 0`) and acquirable by others (test/lock.cpp:161-184).
- Bounds: `index == TENSTORRENT_RESOURCE_LOCK_COUNT` (64; ioctl.h:44) fails with ioctl error and `errno == EINVAL`; index 63 works (test/lock.cpp:187-208).
- One fd can hold all 64 locks simultaneously (test/lock.cpp:210-232).
- `ACQUIRE_BLOCKING` blocks while another fd holds the lock and wakes when it is released (verified with a thread and a 50ms probe; test/lock.cpp:234-278). The semantics satisfy C++ `BasicLockable`/`Lockable` (test/lock.cpp:280-356).
- Process exit (child `_exit` without release) frees the lock (test/lock.cpp:358-385).
- A blocking acquire in one process **wakes when the holding process exits** — fd cleanup must wake waiters (test/lock.cpp:387-424; comment: "This tests that wake_up_interruptible is called during fd cleanup", test/lock.cpp:388).
- SA_RESTART semantics: a blocking acquire interrupted by a signal whose handler has `SA_RESTART` is transparently restarted; the kernel path is `-ERESTARTSYS` internally (documented in the test comment, test/lock.cpp:457-464). The handler releases the lock; the restarted ioctl then acquires and returns success (test/lock.cpp:428-476).

> **Porting note:** The lock array is pure driver state — 64 per-device flags with per-handle ownership, blocking waits, and wake-on-handle-cleanup. On Windows: KEVENT/wait queue keyed per lock index; `EvtFileCleanup` must release all locks held by the file object *and* wake waiters. The SA_RESTART/ERESTARTSYS test is Linux-signal-specific; the Windows analogue is alertable waits + `CancelSynchronousIo`/IRP cancellation — the blocking LOCK_CTL IRP must be cancelable, and cancellation must not leak lock state.

#### 2.10 TestHwmon (test/hwmon.cpp)

Skipped when `/sys/bus/pci/devices/<bdf>/hwmon` doesn't exist (test/hwmon.cpp:78-79). Otherwise, in the `hwmonX` subdirectory:
- Label files must match: `curr1_label` ~ `current[0-9]*`, `in0_label` ~ `vcore[0-9]*`, `temp1_label` ~ `asic[0-9]*_temp`, `power1_label` ~ `power[0-9]*` (test/hwmon.cpp:14-31).
- On Wormhole only (`dev.type < Blackhole`, test/hwmon.cpp:89-90): each of `in0_input`, `curr1_input`, `temp1_input`, `power1_input` must parse as an integer strictly less than the corresponding `*_max` (test/hwmon.cpp:34-68).

> **Porting note:** hwmon is a Linux subsystem; a Windows port would surface telemetry via WMI/IOCTL instead. The load-bearing fact is the sensor set (voltage, current, ASIC temperature, power) and that per-sensor max values exist on Wormhole.

#### 2.11 TestMapPeerBar (test/map_peer_bar.cpp)

For `TENSTORRENT_IOCTL_MAP_PEER_BAR` run over every ordered device pair:
- Same device (two fds to the same node): must fail (returns -1; errno not asserted) (test/map_peer_bar.cpp:110-125, dispatch test/map_peer_bar.cpp:153-156).
- Different chip generations (different PCI DIDs): must fail (test/map_peer_bar.cpp:127-142, 157-160).
- Same-type distinct devices: mapping each *memory* BAR of the peer with `peer_bar_offset = 0` and `peer_bar_length = min(bar_size, 0xFFFFF000)` must succeed ("Cap to the largest page-aligned size the u32 ABI field can hold", test/map_peer_bar.cpp:96-97) (test/map_peer_bar.cpp:83-108). BAR geometry is read from the undocumented sysfs `resource` file, with flags decoded per include/linux/ioport.h values 0x100 (IO), 0x200 (memory), 0x2000 (prefetchable) (test/map_peer_bar.cpp:37-81).
- Overrun variant asserts `EINVAL` when `peer_fd` refers to the same device (test/ioctl_overrun.cpp:180-194).

> **Porting note:** `peer_fd` is a file descriptor identifying the peer device's open handle. A Windows port needs a handle-based equivalent (e.g. passing a HANDLE and resolving via `ObReferenceObjectByHandle` to confirm it is a tenstorrent file object) — the tests demand the driver be able to identify "same underlying device" and "different chip type" from that handle.

#### 2.12 TestTlbs (test/tlbs.cpp, test/tlbs.h)

TLB window helper (test/tlbs.h:19-117): `ALLOCATE_TLB(size)` → `{id, mmap_offset_uc, mmap_offset_wc}`; `CONFIGURE_TLB{id, config{addr, x_end, y_end, ...}}`; mmap of the UC offset; `FREE_TLB{id}` on destruction. Window sizes: `ONE_MEG=1<<20`, `TWO_MEG=1<<21`, `SIXTEEN_MEG=1<<24`, `FOUR_GIG=1ULL<<32` (test/tlbs.h:109-112).

Wormhole assertions:
- Window inventory: "Wormhole has 156x 1M, 10x 2M, and 20x 16M windows; all but the last 16M window should be available" (test/tlbs.cpp:98-99). The test allocates 156×1M, 10×2M, 19×16M successfully; the 20th 16M allocation must fail ("The last 16M window should be off-limits to userspace", test/tlbs.cpp:135-142); then frees all.
- Node-ID readback through windows of all three sizes: ARC tile at (x=0,y=10) via NOC address `0xFFFB2002C`, DDR at (0,11) via `0x10009002C`; node id register encodes `x = bits[5:0]`, `y = bits[11:6]` (test/tlbs.cpp:88-96, 167-186).
- 184 simultaneously-configured windows (156+10+18) all pointing at the same DRAM address read back identical random data written through a 185th window (test/tlbs.cpp:188-233).
- CONFIGURE_TLB rejections: `addr` not aligned to the window size (`size/2`) must fail; `addr = 1ULL << 36` ("Addresses must fit in 36 bits") must fail (test/tlbs.cpp:235-291). Errno is not asserted, only nonzero return.

Blackhole assertions:
- "Blackhole has 202x 2M and up to 8x 4G windows" (test/tlbs.cpp:293-295): 201×2M allocations succeed, the 202nd must fail (last 2M window kernel-reserved, test/tlbs.cpp:312-319); number of 4G windows = `st_size(resource4)/4GiB` (test/tlbs.cpp:70-85) and exactly that many 4G allocations succeed.
- NOC translation detection: BAR0 is mmapped (512MiB, `BAR0_SIZE = 1<<29`) and `NIU_CFG` is read at BAR0 offset `0x1FD04100`; bit 14 = translation enabled (test/tlbs.cpp:22-45).
- Tensix node-id sweep over a 17×12 grid (`x in [1,7]∪[10,16], y in [2,11]`) at NOC register `0xffb20148`, via 2M and (if present) 4G windows (test/tlbs.cpp:358-409).
- PCI tile node-id readback at `0xFFFFFFFFFF000148`: tile (19,24) when translated, (2,0) when not; ARC tile is (8,0) at register `0x0000000080050044` regardless of translation (test/tlbs.cpp:411-441).
- 200 simultaneous 2M windows aimed at DRAM ((17,12) translated / (0,0) untranslated) read back a pattern written through a 201st (test/tlbs.cpp:443-475).
- CONFIGURE_TLB misaligned-address rejection as on Wormhole (no 36-bit check on Blackhole) (test/tlbs.cpp:477-521).

Both device types:
- **Partial unmap of a TLB window must fail**: after mmapping a 2M window, `munmap` of each individual 4K page must fail (the Linux driver forbids VMA splits via a `.may_split`/`.split` vm_ops hook that returns `-EINVAL`, memory.c:1478-1490; the test asserts each per-page `munmap` returns nonzero) (test/tlbs.cpp:523-548). If `mremap` of a page succeeds (kernel-version dependent: "fails on 5.15.0 (fine), succeeds on 5.4.0"), the remapped page's reference must prevent `FREE_TLB` until it is unmapped (test/tlbs.cpp:550-577).
- **A window that is mmapped cannot be freed**: `FREE_TLB` fails while a mapping exists, succeeds after `munmap` (test/tlbs.cpp:581-605).

> **Porting note:** The reference counting contract — outstanding user mappings pin a TLB window; FREE_TLB fails (nonzero) while mapped — must hold on Windows via MDL/section lifetime tracking. "Partial unmap must fail" is enforced by the driver's `.may_split` rejection (memory.c:1478-1490); on Windows, mapping the window as a single section view naturally gives all-or-nothing unmap.

#### 2.13 TestTlbExport (test/dmabuf_export.cpp)

For `TENSTORRENT_IOCTL_EXPORT_TLB_DMABUF` (struct: `{argsz, flags, tlb_id, fd, offset, size}`):
- Probe: export of `{offset=0, size=0}` on a fresh window; `EOPNOTSUPP` means kernel < 5.8 support absent and the whole test is skipped (test/dmabuf_export.cpp:56-73, 335-343).
- Basic export of a whole window succeeds and yields a valid dma-buf fd (test/dmabuf_export.cpp:76-91). (`size=0` in the basic path exports successfully, implying size 0 = whole window.)
- `EINVAL` cases, each asserted exactly (test/dmabuf_export.cpp:94-154):
  - `argsz != sizeof(struct)` (test uses `sizeof+1`)
  - `flags != 0`
  - out-of-range `tlb_id` (0xFFFFFFFF)
  - `offset` not page-aligned (0x1)
  - `size` not page-aligned (0x1)
  - `offset >= window size` (offset = TWO_MEG on a 2M window)
- Ownership: exporting a window allocated on another fd fails with exactly `EPERM` (test/dmabuf_export.cpp:157-178).
- Lifetime, part 1 (test/dmabuf_export.cpp:185-259): with the 2M pool exhausted, `FREE_TLB` on an exported window *succeeds* (refcount model), but the window must **not** become allocatable until the dma-buf fd is closed; closing the dma-buf returns it to the pool.
- Lifetime, part 2 (test/dmabuf_export.cpp:266-331): closing the owning chardev fd (without FREE_TLB) while the export is live must not tear down the window; the dma-buf keeps it allocated; only closing the dma-buf frees it (observed from a second fd via pool exhaustion).

> **Porting note:** dma-buf is Linux-only. If the Windows port needs the equivalent (peer-to-peer DMA into a TLB window by another driver, cf. tools/rdma), the closest analogues are NtCreateSection-based shared mappings or a bus-interface contract; the *lifetime rules* (export holds a reference independent of the owning handle) are the part to preserve.

#### 2.14 TestDeviceRelease (test/release.cpp)

For `TENSTORRENT_IOCTL_SET_NOC_CLEANUP` (struct `{argsz, flags, enabled, x, y, noc, reserved0, addr, data}`; ioctl.h:349-359):
- After arming `{enabled=true, data=0xDEADBEEF, x, y, addr}` and closing the fd, a fresh fd reading `(x,y,addr)` through a 2M TLB window must observe `0xDEADBEEF` — the driver performs the 32-bit NOC write during release (test/release.cpp:18-53).
- Arming then disarming (`enabled=false`) must leave the prior memory content (0x0DDBA115) untouched after close (test/release.cpp:55-91).
- Target tiles: Wormhole DRAM (0,0) addr 0 (test/release.cpp:93-98); Blackhole DRAM (17,12) translated / (0,0) untranslated (test/release.cpp:100-109).

> **Porting note:** This is a per-handle cleanup action; on Windows it belongs in `EvtFileCleanup` (guaranteed to run at last handle close, including process termination).

#### 2.15 TestMappingsDebugfs (test/mappings_debugfs.cpp)

Reads `/sys/kernel/debug/tenstorrent/<ordinal>/mappings` (path built from the /dev node name, test/mappings_debugfs.cpp:34-44); skipped if not readable (e.g. non-root or debugfs unmounted, test/mappings_debugfs.cpp:303-307). Asserts textual content:
- Header contains "WARNING: This file is for diagnostic purposes only" and "not stable"; columns "PID", "Comm", "Type", "Mapping Details" (test/mappings_debugfs.cpp:60-83).
- Entry type strings, each triggered by the corresponding operation: `OPEN_FD` (open fd + caller PID visible), `PIN_PAGES`, `PIN_PAGES+IATU` (pin with `TENSTORRENT_PIN_PAGES_NOC_DMA` flag =2), `DMA_BUF` (+"ID: 0"), `DMA_BUF+IATU` (NOC_DMA alloc, +"ID: 2"), `BAR` (after mmap of BAR0 UC at offset 0), `TLB` (after ALLOCATE_TLB) (test/mappings_debugfs.cpp:85-249).
- Multiple resource types coexist in one read (test/mappings_debugfs.cpp:251-295).

#### 2.16 TestProcfsPids (test/procfs_pids.cpp)

Reads `/proc/driver/tenstorrent/<ordinal>/pids` (test/procfs_pids.cpp:24-34); skipped if absent. Asserts:
- Format: one PID per line, strictly numeric, positive (test/procfs_pids.cpp:48-82).
- The caller's PID appears while it holds an open fd and disappears after close (test/procfs_pids.cpp:84-121).
- One entry **per open fd**: 3 fds ⇒ PID listed exactly 3 times (test/procfs_pids.cpp:123-147).

> **Porting note:** debugfs/procfs diagnostics have no direct Windows equivalent; candidates are a diagnostic IOCTL, WMI, or `!ttkmd` debugger extension. The functional requirement is: the driver tracks, per open handle, the owning PID/process name and every live resource (pins, DMA bufs, BAR mappings, TLBs, iATU regions) — needed anyway for cleanup.

#### 2.17 TestExcl (test/excl.cpp)

Open-time reader/writer lock semantics, documented in the test header (test/excl.cpp:4-10): "O_EXCL is the writer, plain opens are readers, and O_NONBLOCK selects trylock behavior... Blocking waiters wake when open_fds_list becomes empty (i.e. on the last release)." Assertions:
- Two plain opens coexist (test/excl.cpp:53-67).
- O_EXCL on an idle device succeeds (test/excl.cpp:70-76).
- While O_EXCL is held: plain `O_NONBLOCK` and `O_EXCL|O_NONBLOCK` both fail immediately with exactly `EAGAIN` (test/excl.cpp:80-92).
- While a plain fd is held: `O_EXCL|O_NONBLOCK` fails with `EAGAIN` (test/excl.cpp:95-105).
- Closing the O_EXCL fd restores normal opens (test/excl.cpp:108-119).
- Blocking O_EXCL waits while a plain fd is open, and completes when it closes (test/excl.cpp:123-160). Symmetrically, a plain open blocks while O_EXCL is held (test/excl.cpp:164-201).
- A blocking O_EXCL wakes when the holder *process exits* (kernel fd cleanup), with a pipe handshake to avoid racing (test/excl.cpp:205-260).
- A signal without SA_RESTART interrupts a blocking O_EXCL open with exactly `EINTR` (test/excl.cpp:266-317).

> **Porting note:** O_EXCL/O_NONBLOCK are open(2) flags — the CreateFile equivalent must be designed (e.g. map exclusive-open to `FILE_SHARE_*=0` create semantics evaluated in `EvtDeviceFileCreate`, or an explicit flag in an ECP/first-ioctl). Windows CreateFile has no "block until available" mode, so either the create IRP is pended (cancelable) or the blocking mode is dropped and documented. EINTR-on-signal maps to IRP cancellation.

---

### 3. Non-runtime test tooling

- **test/checkws** (Python) — whitespace lint: rejects whitespace-only lines, trailing whitespace, tab-after-space in indentation, and 8+ consecutive leading spaces (test/checkws:10-13). `test/checkws-tests` is its fixture file.
- **test/pahole_check.sh** — compiles a dummy TU including `ioctl.h` and runs `pahole` on every struct; exits 1 if any struct has implicit padding: "Implicit padding in kernel-userspace ABI structures can lead to portability and security issues. All padding should be made explicit using reserved fields." (test/pahole_check.sh:7-9, 89-95).
  > **Porting note:** This is a strong statement that every `ioctl.h` struct is packed-by-construction with explicit reserved fields — MSVC will lay them out identically without pragma pack, but the port should replicate this check (e.g. static_asserts on sizeof/offsetof) to guarantee the shared ABI header stays hole-free.
- **test/run-hardware-tests.sh** — root-only CI driver: unloads existing module/DKMS versions, builds, `insmod`, verifies `/dev/tenstorrent/` exists, dumps `/sys/class/tenstorrent/tenstorrent!*/tt_*` telemetry, ensures ≥2 2MiB hugepages (`/sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages`, test/run-hardware-tests.sh:135-143), then runs `ttkmd_test` under a **120 s timeout** with dmesg/lscpu diagnostics on hang (test/run-hardware-tests.sh:16-47, 148).
- **test/mass-build-test** (Python) — builds the module against every kernel tag in a range (e.g. v4.18..v6.3) using `defconfig` + `modules_prepare`, with `KBUILD_MODPOST_WARN=1` (test/mass-build-test:40-44). Pure Linux build-matrix tool; the Windows analogue is building against multiple WDK/OS targets.

---

### 4. Tools

#### 4.1 tools/reset.c — reset sequencing reference

Standalone C tool ("Iteratively AI-written with manual adjustments", tools/reset.c:6) that documents the full user-visible reset flow. It carries its own copies of the ioctl definitions: magic `0xFA`, `GET_DEVICE_INFO = _IO(0xFA, 0)`, `RESET_DEVICE = _IO(0xFA, 6)`; flags `ASIC_RESET 4`, `ASIC_DMC_RESET 5`, `POST_RESET 6` (tools/reset.c:38-45). Flow:
1. Open `/dev/tenstorrent/<id>` with `O_RDWR | O_APPEND` (power-aware open; tools/reset.c:196) and issue `RESET_DEVICE` with flags 4 (ASIC) or 5 (ASIC+DMC); a nonzero `out.result` is a failure distinct from an ioctl error (tools/reset.c:199-209).
2. Wormhole requires a settle delay before polling: "tt-smi uses 2 seconds... On my system, 20ms isn't long enough but 40ms is" — the tool sleeps 500 ms (tools/reset.c:218-223).
3. Poll for completion with timeout 5 s (ASIC) / 10 s (DMC) (tools/reset.c:225): either the PCI device disappears from `/sys/bus/pci/devices/<bdf>` and reappears, or an **in-place reset marker clears**: 1 byte read of config offset 4 (Command register), completion when `((cmd_reg >> 6) & 1) == 0` (tools/reset.c:236-246) — i.e. the driver uses Command register bit 6 (Parity Error Response) as a reset-in-progress marker.
4. Re-find the device by BDF (the /dev node index may change after reset; 10 s timeout, tools/reset.c:261-270) and issue `RESET_DEVICE` with `POST_RESET` (flags 6) on the new node, checking `out.result == 0` (tools/reset.c:273-289).

> **Porting note:** The disappearance/reappearance leg corresponds to a PCIe-link-level (hot-reset-like) path — on Windows this is surprise-removal + re-enumeration, and the tool's "re-find by BDF, node index may change" logic maps to re-opening by device interface after `CM_NOTIFY` arrival. The Command-register-bit-6 marker is a cross-cutting hardware/driver contract any port of RESET_DEVICE must reproduce or replace.

#### 4.2 tools/power.c — SET_POWER_STATE reference client

Documents `TENSTORRENT_IOCTL_SET_POWER_STATE = _IO(0xFA, 15)` (tools/power.c:35) and the struct (tools/power.c:37-51):
```c
struct tenstorrent_power_state {
    __u32 argsz; __u32 flags;
    __u8 reserved0;
    __u8 validity;   // low nibble = # valid flag bits, high nibble = # valid settings
    __u16 power_flags;
    __u16 power_settings[14];
};
```
Flag bits: bit0 `MAX_AI_CLK` (1=max, 0=min), bit1 `MRISC_PHY_WAKEUP`, bit2 `TENSIX_ENABLE`, bit3 `L2CPU_ENABLE` (tools/power.c:46-49). Documented driver behavior (usage text, tools/power.c:160-176):
- The driver **aggregates** power requests from all clients; unspecified flag bits are treated as ON for that client (backward compatibility) — turning a feature off requires explicitly passing a 0 bit.
- Unknown settings are silently accepted for forward compatibility ("the driver will not return an error for unknown settings").
- Opening with `O_APPEND` marks the client power-aware (tools/power.c:266-267); the client's contribution is removed when the fd closes (tools/power.c:277-279). This matches driver behavior: `bool power_aware = file->f_flags & O_APPEND;` (chardev.c:795) and ioctl.h:374-376 (with O_APPEND initial state is all-off; without, legacy high-power).

> **Porting note:** `O_APPEND` as a "power-aware client" signal is a pure Linux open-flag hack; the Windows port needs an explicit mechanism (create option, ECP, or an ioctl handshake). The aggregation-with-remove-on-close semantics belong in file-object context + `EvtFileCleanup`.

#### 4.3 tools/rdma/nic_bh_p2p_dma.c — dma-buf P2P demo

Two-node RDMA demo: the Blackhole node allocates a 4 GiB TLB window aimed at a GDDR tile (x=17, y=12, addr 0, ordering 0 = Relaxed; tools/rdma/nic_bh_p2p_dma.c:127-132, 504-513), exports 256 MiB of it as a dma-buf via `EXPORT_TLB_DMABUF = _IO(0xFA, 16)` (tools/rdma/nic_bh_p2p_dma.c:59, 515-520), and registers the dma-buf as an RDMA MR with `ibv_reg_dmabuf_mr` (tools/rdma/nic_bh_p2p_dma.c:529-533). The peer node then RDMA-WRITEs a random pattern into GDDR and RDMA-READs it back for verification, with the BH host CPU never touching the data (tools/rdma/nic_bh_p2p_dma.c:594-611). Before use it raises power via SET_POWER_STATE with all four flags (tools/rdma/nic_bh_p2p_dma.c:460-469) and validates the device is Blackhole (DID 0xb140, tools/rdma/nic_bh_p2p_dma.c:471-487). Local ioctl struct copies here match ioctl.h (tools/rdma/nic_bh_p2p_dma.c:63-123) and independently document the ioctl numbers 0, 11, 12, 13, 15, 16.

#### 4.4 tools/fix-tt-hotplug-bars

Bash workaround for Thunderbolt-attached devices whose BARs get `<unassigned>`: requires booting with `pci=realloc`, then removes the Thunderbolt host bridge from sysfs and rescans so the kernel resizes bridge windows (tools/fix-tt-hotplug-bars:5-15, 88-101). Detects TT devices by vendor `0x1e52` (tools/fix-tt-hotplug-bars:21). Linux-only PCI plumbing; documents that the device has large BARs which can fail bridge allocation on hot-plug paths.

#### 4.5 Packaging/release scripts

- **tools/current-version** parses `TENSTORRENT_DRIVER_VERSION_{MAJOR,MINOR,PATCH,SUFFIX}` out of `module.h` and prints e.g. `2.10.1-pre` (tools/current-version:18-28; module.h:19-22 currently 2/10/1/"-pre"). `module.h` is the single version source; build_debs.sh:22-30 and build_rpms.sh:20-28 hard-fail if `dkms.conf`'s `PACKAGE_VERSION` disagrees.
- **tools/make-source-release** creates a DKMS source tarball, excluding paths in **tools/exclude-from-release** (`tools`, `test`, `ttkmd_*.tar`, `modprobe.d-tenstorrent.conf`, `.git*`, `.cache`, `*.deb`, `*.rpm`; tools/exclude-from-release:1-8), and refuses to ship if any `.o` files leak in (tools/make-source-release:52-61).
- **tools/make-installer** + **tools/installer-header.sh** build a self-extracting shell installer (gzip'd DKMS tarball appended after a `__CUT_HERE__` marker) that removes old DKMS versions, `dkms ldtarball`/`install`, and reloads the module (tools/installer-header.sh:8-33).
- **tools/build_debs.sh** / **tools/build_rpms.sh** produce `tenstorrent-dkms` .deb/.rpm packages; both install `udev-50-tenstorrent.rules` into `/lib/udev/rules.d` (build_debs.sh:74-75; build_rpms.sh:95-97), convert `-` to `~` in versions for pre-release ordering (build_debs.sh:32-33; build_rpms.sh:31-35), and modprobe the module on install.

All of section 4.5 is Linux distribution machinery with no ABI content; the Windows analogue is the INF/CAT/MSI signing pipeline.

---

### 5. docs/ — sysfs ABI documentation

`docs/sysfs-attributes.md` is the only file in docs/ and documents two sysfs surfaces:

1. **Telemetry attributes** at `/sys/class/tenstorrent/tenstorrent!<N>/tt_*` (the `!` is literal; docs/sysfs-attributes.md:13-29). Attributes appear **only if the firmware reports the corresponding telemetry tag** (docs/sysfs-attributes.md:45-49). Table (docs/sysfs-attributes.md:54-69):
   - BH+WH: `tt_aiclk`, `tt_axiclk`, `tt_arcclk` (MHz), `tt_serial` (hex), `tt_card_type` (e.g. "p150a", "n150"), `tt_asic_id` (hex), `tt_fw_bundle_ver`, `tt_m3app_fw_ver`, `tt_heartbeat` ("changing = alive")
   - WH only: `tt_m3bl_fw_ver`, `tt_arc_fw_ver`, `tt_eth_fw_ver`, `tt_ttflash_ver`
   - BH only: `tt_therm_trip_count` (ASIC shutdowns due to critical temperature since power cycle)

2. **PCIe performance counters** at `.../pcie_perf_counters/` (docs/sysfs-attributes.md:88-109): 12 read-only counters (6 names × NOC0/NOC1 suffix 0/1): `mst_rd_data_word_received{0,1}`, `mst_nonposted_wr_data_word_sent{0,1}`, `mst_posted_wr_data_word_sent{0,1}`, `slv_nonposted_wr_data_word_received{0,1}`, `slv_posted_wr_data_word_received{0,1}`, `slv_rd_data_word_sent{0,1}` (docs/sysfs-attributes.md:129-142). Semantics: unsigned 32-bit, cumulative since last hardware reset, wrap around, **no reset mechanism**, counted in units of **32-byte flits**, read directly from a memory-mapped BAR segment (docs/sysfs-attributes.md:111-127).

> **Porting note:** These sysfs surfaces are consumed by tt-smi and monitoring tools. The Windows port should expose equivalents (WMI provider or query IOCTLs). Key facts to preserve: attribute presence is firmware-tag-conditional, heartbeat is the liveness signal, counters are 32-bit wrap-around flit counts with no reset.

---

### 6. contrib/

`contrib/packaging/` holds third-party (Nix) packaging: `overlay.nix` overlays `tt-kmd` into every nixpkgs `linuxKernel.packages` set, marking it broken for kernels older than 6.10 (`broken = linuxPackages.kernel.kernelOlder "6.10"` — note this reflects the *overlay dev build*, not the driver's actual minimum) (contrib/packaging/nix/overlay.nix:9-23). `ci.sh` builds all flake packages (contrib/packaging/nix/ci.sh:3-11). MAINTAINERS.md names the external nix maintainer. No ABI content.

---

### 7. Windows conformance-test portability assessment

| Test | Portable as Windows conformance test? | What it needs |
|---|---|---|
| Enumeration (enumeration.cpp) | Rewrite | SetupDi/CM_* by interface GUID; DID→type map (0x401e/0xb140) unchanged |
| GetDriverInfo | Yes (core) | ioctl → DeviceIoControl; replace sysfs version cross-check with file-version resource |
| GetDeviceInfo | Yes (core) | replace sysfs cross-check with CM_ bus/address properties; BDF-encoding and max_dma_buf_size_log2 range checks port verbatim |
| ConfigSpace | Partial | config space via SetupDi/ioctl instead of sysfs; MSI/BME assertions become "interrupts work / BME set" checks |
| QueryMappings | Yes | mmap → the port's mapping call; all invariants (2.4) unchanged |
| DmaBuf / NocDmaBuf | Yes | mmap → mapping call; EINVAL/ENOMEM mapped to NTSTATUS/Win32 equivalents |
| IoctlOverrun | Yes | VirtualAlloc + PAGE_NOACCESS guard page; asserts no out-of-bounds access + output truncation |
| IoctlZeroing | Yes | direct port; asserts zero-fill of `output_size_bytes` remainder |
| PinPages | Mostly | aligned_alloc→VirtualAlloc; hugepages→large pages (SeLockMemoryPrivilege); discontiguous double-map trick via CreateFileMapping/MapViewOfFile twice; IOMMU-conditional expectations need a Windows-side "is DMA remapped" probe |
| Lock | Mostly | threads/fds port directly; fork-based exit tests → spawn child process; SA_RESTART test replaced by IRP-cancellation test |
| Hwmon | No | replace with telemetry-IOCTL/WMI test |
| MapPeerBar | Yes (if ioctl kept) | peer_fd → peer HANDLE; BAR geometry from CM_ resource lists instead of sysfs `resource` |
| Tlbs | Yes | mmap → mapping call; sysfs resource4 size → BAR4 length from resources; partial-unmap semantics re-specified for section views |
| TlbExport (dmabuf) | No (mechanism) / Yes (lifetime) | dma-buf has no Windows equivalent; if a sharing mechanism exists, port the EINVAL/EPERM/lifetime matrix |
| DeviceRelease (NOC cleanup) | Yes | close fd → CloseHandle; also test process-kill path |
| MappingsDebugfs | No | replace with diagnostic IOCTL/WMI equivalent if implemented |
| ProcfsPids | No | same |
| Excl | Design-dependent | depends on the chosen Windows exclusive-open design; EAGAIN/EINTR map to sharing-violation / cancellation |

---

### Key constants table

| Name | Value | Source cite |
|---|---|---|
| TT PCI vendor ID | 0x1E52 | test/enumeration.cpp:82 |
| Wormhole PCI device ID | 0x401e | test/enumeration.cpp:135; tools/reset.c:51 |
| Blackhole PCI device ID | 0xb140 | test/enumeration.cpp:136; tools/reset.c:50 |
| TENSTORRENT_DRIVER_VERSION (interface) | 2 | ioctl.h:10 (asserted test/get_driver_info.cpp:55) |
| TENSTORRENT_MAX_DMA_BUFS | 256 | ioctl.h:41 (exercised test/dma_buf.cpp:156, 82) |
| TENSTORRENT_RESOURCE_LOCK_COUNT | 64 | ioctl.h:44 (exercised test/lock.cpp:195, 215) |
| max_dma_buf_size_log2 valid range | [12, 63] | test/get_device_info.cpp:61-65 |
| bus_dev_fn encoding | (bus<<8)\|(dev<<3)\|fn | test/get_device_info.cpp:52-54 |
| Mapping base+size limit | < 2^44 ("32 + log(PAGE_SIZE)") | test/query_mappings.cpp:153 |
| MAP_PEER_BAR max length used | 0xFFFFF000 (u32, page-aligned cap) | test/map_peer_bar.cpp:97 |
| Lock TEST result bits | LOCK_LOCAL=0b01, LOCK_GLOBAL=0b10 | test/lock.cpp:27-28 |
| Wormhole TLB inventory | 156×1M, 10×2M, 20×16M (last 16M reserved) | test/tlbs.cpp:98-99, 135-142 |
| Blackhole TLB inventory | 202×2M (last reserved) + BAR4_size/4G ×4G | test/tlbs.cpp:293-295, 312-319 |
| TLB window sizes | 1M=1<<20, 2M=1<<21, 16M=1<<24, 4G=1<<32 | test/tlbs.h:109-112 |
| WH CONFIGURE_TLB addr width | must fit 36 bits (1<<36 rejected) | test/tlbs.cpp:269-281 |
| BH NIU_CFG offset (BAR0) / translation bit | 0x1FD04100, bit 14 | test/tlbs.cpp:26, 40 |
| BH BAR0 mmap size in test | 1<<29 (512 MiB) | test/tlbs.cpp:25 |
| WH ARC node-id addr @(0,10) | 0xFFFB2002C | test/tlbs.cpp:169-172 |
| WH DDR node-id addr @(0,11) | 0x10009002C | test/tlbs.cpp:170-173 |
| BH tensix node-id reg | 0xffb20148 (grid 17×12; x∈[1,7]∪[10,16], y∈[2,11]) | test/tlbs.cpp:360-371 |
| BH PCI tile node-id reg | 0xFFFFFFFFFF000148; tile (19,24) translated, (2,0) not | test/tlbs.cpp:413, 421-429 |
| BH ARC tile / reg | (8,0), 0x0000000080050044 | test/tlbs.cpp:436-437 |
| node-id decode | x=bits[5:0], y=bits[11:6] | test/tlbs.cpp:92-93 |
| ioctl magic / RESET_DEVICE nr | 0xFA / 6 | tools/reset.c:38-40 |
| RESET flags ASIC / ASIC_DMC / POST_RESET | 4 / 5 / 6 | tools/reset.c:43-45; ioctl.h:149-151 |
| Reset poll timeouts | 5 s ASIC, 10 s DMC, 10 s re-find; 500 ms WH settle | tools/reset.c:222-225, 262 |
| Reset in-progress marker | PCI Command reg (offset 4) bit 6 | tools/reset.c:241 |
| SET_POWER_STATE nr / flag bits | 15; bit0 MAX_AI_CLK, bit1 MRISC_PHY_WAKEUP, bit2 TENSIX_ENABLE, bit3 L2CPU_ENABLE | tools/power.c:35, 46-49 |
| Power validity nibbles | low=count of flags, high=count of settings | tools/power.c:42-44 |
| power_settings capacity | 14 × u16 | tools/power.c:50 |
| Power-aware open marker | O_APPEND | tools/power.c:266-267; chardev.c:795 |
| EXPORT_TLB_DMABUF nr | 16 | tools/rdma/nic_bh_p2p_dma.c:59 |
| PIN_PAGES flags | CONTIGUOUS=1, NOC_DMA=2 | ioctl.h:169-170 (used test/pin_pages.cpp:58; test/mappings_debugfs.cpp:149) |
| max simultaneous pins tested | 1024 | test/pin_pages.cpp:183 |
| hwmon sensor set | curr1/in0/temp1/power1 + labels + maxes (WH) | test/hwmon.cpp:14-20, 37-42 |
| Hardware test timeout | 120 s for full ttkmd_test | test/run-hardware-tests.sh:148 |
| debugfs mappings path | /sys/kernel/debug/tenstorrent/<N>/mappings | test/mappings_debugfs.cpp:43 |
| procfs pids path | /proc/driver/tenstorrent/<N>/pids | test/procfs_pids.cpp:33 |
| PCIe perf counter unit | 32-byte flits, u32 wrap-around, no reset | docs/sysfs-attributes.md:111-127 |

### Open questions

1. **Errno for several rejection paths is unasserted.** PIN_PAGES bad-flags/bad-size/unmapped-range, UNPIN_PAGES bad-size, MAP_PEER_BAR same-device/different-chip (non-overrun variant), and CONFIGURE_TLB misaligned/oversized-address failures only assert `ioctl(...) == -1` / `!= 0` without checking errno (e.g. test/pin_pages.cpp:94, 118, 162; test/map_peer_bar.cpp:123, 140; test/tlbs.cpp:264, 280). The precise error codes must be taken from the driver sources (chardev/tlb sections of this analysis), not from the tests.
2. **`VerifyNoOverlap` iterates the unsorted mapping list** (test/query_mappings.cpp:114-128) despite building a sorted copy — likely a test bug. Whether QUERY_MAPPINGS results are guaranteed base-sorted (making the check correct as written) is not established by the test; check the driver's emit order before relying on it.
3. **EXPORT_TLB_DMABUF `size == 0`**: the basic-export and lifetime tests pass `offset=0, size=0` and expect success (test/dmabuf_export.cpp:63, 84, 217), implying size 0 = "whole window", but no test asserts what the exported length actually is. Confirm against the driver implementation.
4. **Reset marker semantics**: tools/reset.c:241 polls PCI Command register bit 6 ("marker cleared") to detect in-place reset completion. Whether the driver or firmware sets/clears this bit, and on which device generations the in-place (non-disappearing) path applies, is not documented in the tool — must be cross-checked with the reset section of the driver analysis.
5. **hwmon max-comparison excluded on Blackhole** (`dev.type < Blackhole`, test/hwmon.cpp:89): the tests don't say whether BH lacks the `*_max` files or merely has unreliable values. Affects what a Windows telemetry surface should expose per generation.
6. **TLB partial-unmap behavior is kernel-version dependent in the mremap leg** (test/tlbs.cpp:550-553: "fails on 5.15.0 (fine), succeeds on 5.4.0"); the ABI-stable requirement is only that any surviving user mapping blocks FREE_TLB. A Windows port should define its own all-or-nothing mapping semantics explicitly.
7. **`VerifyPinPagesMultipleRanges` uses 1024 pins** but calls the count `max_pinned_ranges` (test/pin_pages.cpp:183) — whether 1024 is an actual driver limit or an arbitrary test number is not established here; check the pin-pages section of the driver analysis.
8. **AER expectations in virtualized environments**: `--skip-aer` exists because AER "seems to be disabled" in VMs (test/main.cpp:34-36). For a Windows port, the equivalent expectation (AER handled by OS/platform, driver-visible or not) needs its own decision.
9. **nix overlay marks kernels < 6.10 broken** (contrib/packaging/nix/overlay.nix:19) while mass-build-test targets v4.18+ (test/mass-build-test:9) — the true minimum supported kernel is ambiguous from these directories alone (irrelevant to Windows except as a hint about which kernel-API fallbacks exist in the driver).

---

## 13. tt-umd Consumer ABI — the de facto userspace contract a Windows shim must satisfy

All paths in this section are relative to the **tt-umd** repo (`/home/alex/tt-windowsport/tt-umd`) unless prefixed otherwise. This section catalogs every OS-touching call site in tt-umd's device-access layer: which ioctls it issues (and in what order), how it discovers devices, every mmap it performs, hugepage usage, sysfs/procfs reads, fd/signal lifecycle assumptions, and threading assumptions. Together these define the ABI surface a Windows KMDF driver + user-mode shim must reproduce.

### Scope

Files read in full:

| File | Lines |
|---|---|
| device/pcie/pci_device.cpp | 1119 |
| device/pcie/ioctl.h (copy of KMD header) | 374 |
| device/api/umd/device/pcie/pci_device.hpp | 413 |
| device/tt_kmd_lib/tt_kmd_lib.c | 550 |
| device/api/umd/device/tt_kmd_lib/tt_kmd_lib.h | 429 |
| device/hugepage.cpp / hugepage.hpp | 222 / 37 |
| device/chip_helpers/silicon_sysmem_manager.cpp (+ hpp) | 473 (+51) |
| device/chip_helpers/sysmem_manager.cpp (+ hpp) | 100 (+76) |
| device/chip_helpers/sysmem_buffer.cpp | 232 |
| device/chip_helpers/tlb_manager.cpp | 118 |
| device/pcie/silicon_tlb_handle.cpp | 63 |
| device/pcie/silicon_tlb_window.cpp | 345 |
| device/utils/robust_mutex.cpp (+ hpp) | 498 (+121) |
| device/utils/lock_manager.cpp (+ hpp) | 87 (+98) |
| device/api/umd/device/utils/kmd_versions.hpp | 44 |
| device/warm_reset.cpp | 710 |
| device/chip/local_chip.cpp | 562 |
| device/noc_access.cpp | 24 |

Targeted reads (call-site verification): device/tt_device/protocol/pcie_protocol.cpp:120-349 (of 349), device/tt_device/tt_device.cpp:700-830 (of 869), device/cpuset_lib.cpp:80-180 (of 644), device/api/umd/device/arch/{wormhole,blackhole}_implementation.hpp (constants only), device/api/umd/device/pcie/pci_ids.h, device/api/umd/device/types/tlb.hpp.

---

### 1. Device discovery and open

#### 1.1 Enumeration: readdir of `/dev/tenstorrent/`

Enumeration is a directory scan of `/dev/tenstorrent/`, keeping entries whose filename is an integer:

- `PCIDevice::get_all_device_ids()` iterates `std::filesystem::directory_iterator("/dev/tenstorrent/")` and does `std::stoi(filename)` on integer-named entries (pci_device.cpp:1077-1095). If the directory does not exist, an empty list is returned (pci_device.cpp:1081-1083, and again at 232-234).
- `PCIDevice::enumerate_devices()` (pci_device.cpp:227-326) then applies the **`TT_VISIBLE_DEVICES`** environment variable (read at pci_device.cpp:236): a comma-separated list where each token is either a device index (validated 0..N-1, else throw, pci_device.cpp:287-308) or a BDF substring pattern matched against the device's BDF string (pci_device.cpp:258-285, throws if a BDF pattern matches nothing). No filter → all devices.
- Results are **sorted by PCI BDF order** (`sort_ids_based_on_bdf`, pci_device.cpp:328-350); IDs that can't be mapped to a BDF are appended in input order.
- `enumerate_devices_info()` opens each node with `open(path, O_RDWR | O_CLOEXEC | O_APPEND)` (pci_device.cpp:355), issues `TENSTORRENT_IOCTL_GET_DEVICE_INFO`, and closes the fd (pci_device.cpp:352-368). Failures to open are silently skipped (`continue`), failures of the ioctl are swallowed by `catch (...)`.
- `get_bdf_to_device_id_map()` does the same open/ioctl/close per device (pci_device.cpp:1097-1117), again with `O_APPEND`.

There is **no** udev/by-id lookup; the N in `/dev/tenstorrent/N` is the identity used everywhere ("`N in /dev/tenstorrent/N`", pci_device.hpp:123).

A secondary discovery path exists in `cpuset_lib.cpp`: hwloc PCI enumeration filtered on vendor `0x1e52` (cpuset_lib.hpp:59), then a scan of `/sys/bus/pci/devices/<bdf>/tenstorrent/` for an entry matching regex `tenstorrent!([0-9]+)` to recover the char-dev minor for a given PCI function (cpuset_lib.cpp:108-146). This is used only for hugepage-channel counting and NUMA binding.

#### 1.2 Open flags — the O_APPEND power-mode quirk

The **main device handle** is opened deliberately *without* `O_APPEND`:

```c
// pci_device.cpp:378-384
static int open_pci_device(const std::string &device_path) {
    // O_APPEND is temporarily disabled to investigate NOC1 issues. See
    // https://github.com/tenstorrent/tt-umd/issues/2531.
    int flags = O_RDWR | O_CLOEXEC;
    ...
    return open(device_path.c_str(), flags);
}
```

`O_APPEND` on the tenstorrent chardev is repurposed by KMD ≥ 2.6 as an *opt-out of legacy power mode*: "Opening the device with O_APPEND opts out of legacy mode, allowing the device to enter low-power idle states when no client holds power flags" (ioctl.h:347-348; also tt_kmd_lib.h:407-408, kmd_versions.hpp:40-41). Enumeration handles (pci_device.cpp:355, 1101) and reset handles (pci_device.cpp:204) *do* pass `O_APPEND`; the long-lived I/O handles currently do not (legacy power semantics).

All opens use `O_CLOEXEC` (pci_device.cpp:355, 381, 1101; tt_kmd_lib.c:71).

#### 1.3 Two fds per PCIDevice

`PCIDevice`'s constructor opens the chardev **twice**:

1. `pci_device_file_desc = open_pci_device(device_path)` (pci_device.cpp:389) — used for `GET_DEVICE_INFO`, `QUERY_MAPPINGS`, `PIN_PAGES`/`UNPIN_PAGES`, `ALLOCATE_DMA_BUF`, and the BAR/DMA-buffer mmaps.
2. `tt_device_open(device_path.c_str(), &tt_device_handle, /*extra_flags=*/0)` (pci_device.cpp:414-415) — a second fd inside the `tt_kmd_lib` C library (tt_kmd_lib.c:64-81), used for `ALLOCATE_TLB`/`FREE_TLB`/`CONFIGURE_TLB`, `SET_POWER_STATE`, `GET_DRIVER_INFO`, and TLB-window mmaps.

Both fds stay open for the object's lifetime; the destructor closes the tt_kmd_lib fd first, then the raw fd, and **only afterwards** unmaps BAR0, the TLB-config page, BAR2, and the DMA buffer (pci_device.cpp:572-601).

> **Porting note:** A Windows shim must allow (a) multiple concurrent handles to the same device from one process, (b) per-handle resource accounting (TLB windows allocated on handle 2, pinned pages on handle 1), and (c) mapped views remaining valid after the owning handle is closed (Linux mmaps survive `close(fd)`; the destructor relies on this ordering). With KMDF this argues for section-based mappings whose lifetime is tied to process unmap or file-object cleanup, not handle close ordering.

#### 1.4 KMD version gating (sysfs)

`PCIDevice::read_kmd_version()` reads a version string from **`/sys/module/tenstorrent/version`** (pci_device.cpp:805-818), returning `0.0.0` with a warning if unreadable. Version gates (kmd_versions.hpp):

| Gate | Version | Meaning | Cite |
|---|---|---|---|
| `KMD_IOMMU` | 1.29.0 | IOMMU support; UMD throws if IOMMU is on and KMD older (pci_device.cpp:397-401) | kmd_versions.hpp:15 |
| `KMD_TLBS` | 1.34.0 | **Hard minimum**: "Running UMD requires KMD version 1.34.0 or newer" (pci_device.cpp:402-405) | kmd_versions.hpp:36 |
| `KMD_MAP_TO_NOC` | 2.0.0 | `PIN_PAGES` with `NOC_DMA` flag; below this UMD programs iATU registers itself (`init_pcie_iatus`, local_chip.cpp:149-152, 385-405) | kmd_versions.hpp:23 |
| `KMD_ARCH_AGNOSTIC_RESET` | 2.4.1 | `RESET_DEVICE` flags 3-6 usable | kmd_versions.hpp:29 |
| `KMD_POWER_STATE` | 2.6.0 | `SET_POWER_STATE` ioctl | kmd_versions.hpp:43 |

Also consulted per device from sysfs: `numa_node` (default −1, pci_device.cpp:391), `revision` (mandatory — throws if unreadable, pci_device.cpp:392, 79-92), `iommu_group/type` (IOMMU detection: enabled iff the value starts with `"DMA"`, i.e. `DMA` or `DMA-FQ`; absent ⇒ disabled, pci_device.cpp:100-106).

> **Porting note:** A Windows port needs replacements for: module version string (e.g. a GET_DRIVER_INFO-only path — the ioctl already returns `driver_version_major/minor/patch`, ioctl.h:130-137), `numa_node`, `revision`, and IOMMU detection (Windows: DMA remapping status per device). These sysfs reads are open-coded in UMD, so they will be shim/port work regardless of what the KMD exposes.

---

### 2. Ioctl inventory

All ioctls are defined with `_IO(0xFA, nr)` — magic `0xFA` (ioctl.h:16), **no size/direction encoded in the command word** (ioctl.h:18-33). `TENSTORRENT_DRIVER_VERSION 2` (ioctl.h:14) is the IOCTL API version tt-umd was built against. Argument structs are in/out pairs copied through the pointer arg; most "in" structs begin with `output_size_bytes` which UMD sets to `sizeof(out)` (e.g. pci_device.cpp:175, tt_kmd_lib.c:95).

#### 2.1 Ioctls tt-umd issues

| Ioctl (nr) | Call sites | Arguments as issued | Error handling |
|---|---|---|---|
| `GET_DEVICE_INFO` (0) | pci_device.cpp:177; tt_kmd_lib.c:97 | `in.output_size_bytes = sizeof(out)` | `<0` ⇒ throw RuntimeError / return `-errno` |
| `QUERY_MAPPINGS` (2) | pci_device.cpp:451 | `in.output_mapping_count = 8`, zeroed 8-entry `tenstorrent_mapping` array appended (pci_device.cpp:443-449) | `== -1` ⇒ throw |
| `ALLOCATE_DMA_BUF` (3) | pci_device.cpp:971 | `in.requested_size = dma_buf_size + page` , `in.buf_index = 0`, flags 0 (pci_device.cpp:963-969) | nonzero ⇒ log + retry smaller |
| `GET_DRIVER_INFO` (5) | tt_kmd_lib.c:158 | `in.output_size_bytes = sizeof(out)` | nonzero ⇒ `-errno` |
| `RESET_DEVICE` (6) | tt_kmd_lib.c:545 (via `tt_device_reset`, called from pci_device.cpp:209) | `in.flags` = one of 0/1/2 (legacy) or 3/4/5/6 (2.4.1+) (ioctl.h:145-153); `in.output_size_bytes = sizeof(out)` | nonzero ⇒ `-errno`; **on success the fd is closed** (tt_kmd_lib.c:549) |
| `PIN_PAGES` (7) | pci_device.cpp:611, 660, 714, 753; tt_kmd_lib.c:356 | see §4 | `== -1` ⇒ throw (except `map_for_hugepage`, which logs and returns 0, pci_device.cpp:611-620) |
| `UNPIN_PAGES` (10) | pci_device.cpp:788; tt_kmd_lib.c:382 | `in.virtual_address` = original VA, `in.size` | `<0` ⇒ throw / `-errno` |
| `ALLOCATE_TLB` (11) | tt_kmd_lib.c:417 | `in.size` = window size in bytes | nonzero ⇒ `-errno` |
| `FREE_TLB` (12) | tt_kmd_lib.c:432, 455 | `in.id` | nonzero ⇒ `-errno` (and "Leaked TLB" stderr message on the mmap-failure path, tt_kmd_lib.c:433) |
| `CONFIGURE_TLB` (13) | tt_kmd_lib.c:492, 511 | `in.id` + `tenstorrent_noc_tlb_config` (addr, x/y start/end, noc, mcast, ordering, static_vc; `linked` never set) | nonzero ⇒ `-errno` |
| `SET_POWER_STATE` (15) | tt_kmd_lib.c:525 | `argsz = sizeof(struct)`, `flags = 0`, `validity = TT_POWER_VALIDITY(4,0)`, `power_flags` (tt_kmd_lib.c:519-523) | nonzero ⇒ `-errno`; caller only logs a warning (pci_device.cpp:1071-1074) |

**Ioctls defined but never issued by tt-umd 2.x:** `GET_HARVESTING` (1), `FREE_DMA_BUF` (4), `LOCK_CTL` (8), `MAP_PEER_BAR` (9), `SET_NOC_CLEANUP` (14) (definitions ioctl.h:19, 22, 26-27, 32; no call sites anywhere in `device/`, `src/`, `tools/` — verified by grep). DMA buffers are never explicitly freed; UMD relies on **fd-close cleanup** ("we can't deallocate it. That only happens when we close the fd", pci_device.cpp:984-985).

#### 2.2 Ioctl order during device bring-up

`PCIDevice::PCIDevice(int n)` (pci_device.cpp:386-570) performs, in order:

1. `open("/dev/tenstorrent/N", O_RDWR|O_CLOEXEC)` (pci_device.cpp:389, 378-384).
2. `GET_DEVICE_INFO` on that fd (pci_device.cpp:390 → 173-198). Decodes `bus_dev_fn` as `bus = >>8`, `dev = (>>3)&0x1F`, `fn = &0x07` (pci_device.cpp:181-183).
3. sysfs reads: `numa_node`, `revision`, `iommu_group/type`, `/sys/module/tenstorrent/version` (pci_device.cpp:391-395); version gate checks (397-412).
4. `tt_device_open()` — second fd (pci_device.cpp:414-427); on failure the handle is closed and RuntimeError thrown.
5. `GET_DRIVER_INFO` via `tt_driver_get_attr(TT_DRIVER_API_VERSION)` (pci_device.cpp:429-430; note this attr actually returns the compile-time constant 2, tt_kmd_lib.c:164-166).
6. Assert Wormhole revision `0x01` (pci_device.cpp:440-441).
7. `QUERY_MAPPINGS` with room for 8 entries (pci_device.cpp:443-453); scans for the six mapping IDs (467-489; IDs ioctl.h:36-42). Requires `RESOURCE0_UC` (pci_device.cpp:499-501); Wormhole additionally requires `RESOURCE2_UC` (532-535), Blackhole requires `RESOURCE1_UC` (549-552).
8. Four/five mmaps (§3).
9. DMA-buffer allocation loop (§6).

Note the post-ioctl loop bound re-reads `mappings.query_mappings.in.output_mapping_count` (pci_device.cpp:466) — UMD assumes the driver rewrites this field in place to the number of entries returned (or leaves it at 8 with unused slots having `mapping_id == 0`).

#### 2.3 Reset flow

`send_reset_ioctl(device_id, flags)` opens a *fresh* fd with `O_RDWR|O_CLOEXEC|O_APPEND` (pci_device.cpp:200-207), issues `RESET_DEVICE` with the given flags, and the helper closes the fd on success (tt_kmd_lib.c:532-550). The arch-agnostic warm reset sends `RESET_PCIE_LINK`(1) if secondary-bus reset requested, then `ASIC_DMC_RESET`(5) or `ASIC_RESET`(4), sleeps max(2 s, 0.4 s × ndevices) (or the caller-provided M3 timeout when `reset_m3` is set, warm_reset.cpp:219-221), polls for the device to reappear by glob-matching **`/sys/bus/pci/devices/<bdf>/tenstorrent/tenstorrent!*`** and checking `/dev/tenstorrent/<id>` exists (warm_reset.cpp:131-180, 182-239, glob pattern at 142), then sends `POST_RESET`(6) (warm_reset.cpp:237). Reappearance timeout is a named constant `WARM_RESET_DEVICES_REAPPEAR_TIMEOUT` (warm_reset.cpp:132). Legacy Blackhole reset uses `CONFIG_WRITE`(2), polls config-space **command byte bit 1** (read via `/sys/bus/pci/devices/<bdf>/config` offset 4, pci_device.cpp:108-123, 915-935) every 10 ms, then `RESTORE_STATE`(0) (warm_reset.cpp:241-292).

Cross-process pre/post-reset notification uses Unix-domain sockets `client_<PID>.sock` in a well-known listener directory (warm_reset.cpp:484-708).

> **Porting note:** the reset dance assumes PnP-style surprise removal/rescan semantics (device node disappears and reappears, possibly with a different minor). On Windows this maps naturally onto PnP stop/start or a bus-level reset with interface arrival notifications; the shim's "wait for reappear" must be reimplemented on device-interface notifications rather than sysfs glob.

---

### 3. mmap map — every mapping UMD creates on the device fd

`QUERY_MAPPINGS` returns `(mapping_id, mapping_base, mapping_size)` tuples where `mapping_base` is a **pseudo-offset into the fd's mmap offset space** (ioctl.h:74-79). UMD composes `mmap(..., fd, mapping_base + byte_offset_into_BAR)` — i.e. the shim must support byte-granular (page-aligned) offsets *within* a resource, not just whole-resource mapping. Resource↔BAR: "Resource 0 → BAR0, Resource 1 → BAR2, Resource 2 → BAR4" (pci_device.cpp:455-458).

| # | What | fd | offset | size | prot/flags | caching | Cite |
|---|---|---|---|---|---|---|---|
| 1 | BAR0 ARC/NOC2AXI window | dev fd | `RESOURCE0_UC.base + 509 MiB` | 3 MiB | RW, `MAP_SHARED` | UC | pci_device.cpp:503-513; offsets pci_device.hpp:406 (`509*(1<<20)`), size pci_device.hpp:354 (`3*(1<<20)`) |
| 2 | TLB config registers | dev fd | `RESOURCE0_UC.base + 0x1fc00000` | 4 KiB | RW, `MAP_SHARED` | UC | pci_device.cpp:518-530; addr wormhole_implementation.hpp:199 / blackhole_implementation.hpp:174 (`STATIC_TLB_CFG_ADDR = 0x1fc00000` both), size pci_device.hpp:356 |
| 3 | BAR2 UC (`bar2_uc`) | dev fd | `RESOURCE1_UC.base` | `RESOURCE1_UC.size` | RW, `MAP_SHARED` | UC | WH: pci_device.cpp:537-547; BH: 555-566 |
| 4 | TLB window (per allocation) | tt_kmd_lib fd | `alloc_tlb.out.mmap_offset_uc` **or** `mmap_offset_wc` | window size (1M/2M/16M/4G) | RW, `MAP_SHARED` | UC or WC, chosen per allocation | tt_kmd_lib.c:423-426 |
| 5 | KMD DMA buffer (no-IOMMU) | dev fd | `dma_buf.out.mapping_offset` | `dma_buf_size + page` | RW, `MAP_SHARED` | (kernel-chosen) | pci_device.cpp:975-981 |
| 6 | Hugepage sysmem channel | hugetlbfs file fd | 0 | 1 GiB | RW, `MAP_SHARED\|MAP_POPULATE` | cached | silicon_sysmem_manager.cpp:235-236 |
| 7 | IOMMU sysmem / DMA bounce (anon) | −1 | 0 | various | RW, `MAP_PRIVATE\|MAP_ANONYMOUS\|MAP_POPULATE` (+`MAP_HUGETLB\|MAP_HUGE_{1GB,512MB,2MB}` tries) | cached | silicon_sysmem_manager.cpp:53-97; pci_device.cpp:941-942 |
| 8 | Robust-mutex shm | shm fd | 0 | `sizeof(pthread_mutex_wrapper)` | RW, `MAP_SHARED` | cached | robust_mutex.cpp:318 |

UC vs WC policy: register access paths use UC windows (`TT_MMIO_CACHE_MODE_UC` "use for register accesses", tt_kmd_lib.h:90), bulk memory paths use WC ("Write Combined; use for memory accesses", tt_kmd_lib.h:91). Concretely: `tt_noc_read32/write32` allocate a 2 MiB UC window per call (tt_kmd_lib.c:189, 215); `tt_noc_read/write` block helpers use WC (tt_kmd_lib.c:244, 289); LocalChip keeps one cached WC window for data and one cached UC window for registers (local_chip.cpp:532-552); the DMA path's window is WC (pcie_protocol.cpp:254-257); sysmem-buffer cached window is WC (sysmem_buffer.cpp:223-230); TLBManager static windows are WC (tlb_manager.cpp:46).

BAR0 access arithmetic: `bar_read32/bar_write32` subtract `BAR0_OFFSET = 0x1FD00000` (= 509 MiB) from the AXI address before indexing the 3 MiB `bar0` view, throwing on addresses below it (pcie_protocol.cpp:154-168, pcie_protocol.hpp:92).

Unmap behavior: `~PCIDevice` munmaps `bar0`, `tlb_config_space`, `bar2_uc`, and the DMA buffer (`size + pagesize`) **after** closing both fds (pci_device.cpp:572-601). TLB windows are munmapped **before** `FREE_TLB` — "Unmap the userspace view of the TLB. This is required by the driver." (tt_kmd_lib.c:449-455).

> **Porting note:** the single-fd + magic-offset mmap model has no direct Win32 equivalent. The natural mapping is an ioctl that returns a user VA (kernel `MmMapLockedPagesSpecifyCache`/section objects) per mapping-id + offset + size + cache-attribute tuple. The shim must preserve: (a) sub-BAR offsets (509 MiB, 0x1fc00000), (b) UC vs WC selection per TLB window (two distinct "offsets" for the same window), (c) unmap-before-free ordering for TLB windows, and (d) view validity after handle close.

---

### 4. PIN_PAGES / UNPIN_PAGES — DMA & NOC mapping of host memory

Four variants, all on `pci_device_file_desc`:

1. **`map_for_hugepage(buffer, size)`** — flags `TENSTORRENT_PIN_PAGES_CONTIGUOUS` (=1), classic 8-byte `out.physical_address`. Failure is *soft*: logs a warning and returns 0 (pci_device.cpp:603-630). Used on the legacy (pre-2.0 KMD) hugepage path (silicon_sysmem_manager.cpp:312).
2. **`map_buffer_to_noc(buffer, size)`** — flags `TENSTORRENT_PIN_PAGES_NOC_DMA` (=2) with the **extended** out struct `{physical_address, noc_address}` and `in.output_size_bytes = sizeof(out) = 16` (pci_device.cpp:634-681). Preconditions: KMD ≥ 2.0.0 (throw otherwise, 635-637), VA and size page-aligned (throw, 642-644), and `size > page ⇒ IOMMU required` (throw, 646-648).
3. **`map_hugepage_to_noc(hugepage, size)`** — flags `CONTIGUOUS|NOC_DMA` (=3) extended out (pci_device.cpp:683-735). Rejects `size > 1 GiB` ("Not a hugepage", 691-693) and non-page-aligned VA/size (695-697).
4. **`map_for_dma(buffer, size)`** — flags `is_iommu_enabled() ? 0 : CONTIGUOUS` (pci_device.cpp:737-773, flag choice at 741).

`unmap_for_dma(buffer, size)` issues `UNPIN_PAGES` with the **original** VA/size (pci_device.cpp:775-803; "original VA used to pin, not current VA if remapped", ioctl.h:193; "unpinning subset of a pinned buffer is not supported", ioctl.h:191).

`tt_dma_map` (public C API) additionally exposes `TENSTORRENT_PIN_PAGES_NOC_TOP_DOWN` (=4) via `TT_DMA_FLAG_NOC_TOP_DOWN` (tt_kmd_lib.c:348-354), returns `-EINVAL` for zero/unaligned len or null/unaligned addr (tt_kmd_lib.c:326-329), and uses `~0ULL` as "no NOC address" sentinel (tt_kmd_lib.c:369, 397-399). Documented aperture constraints the driver enforces (tt_kmd_lib.h:264-271): WH per-buffer `0x1000 ≤ len ≤ 0xFFFE_0000`, cumulative `0xFFFE_0000`, max 16 simultaneous NOC mappings; BH per-buffer `0x1000 ≤ len ≤ 0xFFFF_F000`, max 16.

#### Sysmem NOC address expectations (hard-coded)

Sysmem must land at a specific NOC address; device-side firmware assumes it:

```cpp
// sysmem_manager.cpp:79-88
uint64_t SysmemManager::get_pcie_base_for_arch(tt::ARCH arch) {
    case tt::ARCH::WORMHOLE_B0: return 0x800000000;   // 32 GiB
    case tt::ARCH::BLACKHOLE:   return 4ULL << 58;
```

- Hugepage path: expected `noc_address == pcie_base + ch * 1GiB`; mismatch ⇒ loud warning about stale processes holding the aperture (silicon_sysmem_manager.cpp:296-310).
- IOMMU path: `noc_address != pcie_base` ⇒ **throw** (silicon_sysmem_manager.cpp:404-429).
- Wormhole channel 3 is truncated to `HUGEPAGE_CHANNEL_3_SIZE_LIMIT = 768 * (1 << 20)` (silicon_sysmem_manager.hpp:20) to avoid colliding with PCIe registers in the WH NOC aperture (pin: silicon_sysmem_manager.cpp:290-292; unpin: 158-163; IOMMU sizing carves out 256 MiB when 4 channels: 351-359; legacy iATU: local_chip.cpp:396-402).

`SysmemBuffer` aligns arbitrary user VAs down to page boundaries and rounds the size up before pinning, tracking `offset_from_aligned_addr_` (sysmem_buffer.cpp:198-204); its destructor calls `unmap_for_dma` and merely warns on failure (sysmem_buffer.cpp:181-196).

---

### 5. Hugepage usage (hugetlbfs) and the IOMMU alternative

Two mutually exclusive sysmem strategies, chosen by `pci_device_->is_iommu_enabled()` (silicon_sysmem_manager.cpp:135-142):

#### 5.1 hugetlbfs path (no IOMMU)

- Page size is fixed at **1 GiB per channel**: `HUGEPAGE_REGION_SIZE = 1ULL << 30` (hugepage.hpp:17), max `MAX_HOST_MEM_CHANNELS = 4` (hugepage.hpp:19; assert at silicon_sysmem_manager.cpp:116-119).
- Channel count available = `min(target, max(1, total_hugepages / num_tt_devices_of_this_arch))` (hugepage.cpp:70-71), where `total_hugepages` comes from **`/sys/kernel/mm/hugepages/hugepages-1048576kB/nr_hugepages`** (hugepage.cpp:32, throws if unreadable) and device counts come from hwloc/sysfs (hugepage.cpp:53-55).
- Mount discovery: scans **`/proc/mounts`** for a hugetlbfs mount at exactly **`/dev/hugepages-1G`** — regex `^(nodev|hugetlbfs) (/dev/hugepages-1G) hugetlbfs ([^ ]+) 0 0$` — and verifies the `pagesize=` option equals 1 GiB (hugepage.cpp:29, 114-160). No mount ⇒ empty string ⇒ `init_hugepages` returns false (silicon_sysmem_manager.cpp:201-208).
- Per-channel file naming inside the mount (hugepage.cpp:162-195): device 0 channel 0 uses the shared legacy name `tenstorrent`; otherwise `device_<N>_` prefix and, for channel≠0, `channel_<C>_`, i.e. `device_2_channel_1_tenstorrent`.
- File open: `umask(0)` around `open(path, O_RDWR|O_CREAT|O_CLOEXEC, 0666)`; on `EACCES` the file is **unlinked and re-created** (hugepage.cpp:197-209). Failure returns −1 (soft).
- `fstat` to sanity-check size (silicon_sysmem_manager.cpp:230-233), then `mmap(nullptr, 1GiB, RW, MAP_SHARED|MAP_POPULATE, hugepage_fd, 0)` and immediate `close(hugepage_fd)` (silicon_sysmem_manager.cpp:235-238). On mmap failure, diagnostics print `/proc/cmdline` and the nr_hugepages sysfs file (254-256).
- The mapping is NUMA-bound to the device's node via hwloc (`bind_area_to_memory_nodeset`, silicon_sysmem_manager.cpp:263, cpuset_lib.hpp:36-39); failure only warns (perf issue #893).
- Pinning happens later in `start_device`: KMD ≥ 2.0 → `map_hugepage_to_noc`; older → `map_for_hugepage` + userspace iATU programming (silicon_sysmem_manager.cpp:281-343; local_chip.cpp:148-152). Pin failure prints `/sys/module/tenstorrent/version`, `/proc/meminfo`, `/proc/buddyinfo` (silicon_sysmem_manager.cpp:324-326).

#### 5.2 IOMMU path

One big anonymous allocation of `num_channels × 1 GiB` (full size mmapped, silicon_sysmem_manager.cpp:352, 366), allocated by `mmap_with_hugepage_fallback`: try `MAP_HUGETLB|MAP_HUGE_1GB`, then `MAP_HUGE_512MB`, then `MAP_HUGE_2MB` (each only if size is a multiple), finally plain pages with a perf warning — all `MAP_PRIVATE|MAP_ANONYMOUS|MAP_POPULATE` (silicon_sysmem_manager.cpp:45-109). The region is pinned via one `SysmemBuffer` (`map_buffer_to_noc` or `map_for_dma`) using `iommu_mapping_size` — for WH with 4 channels this is the full size minus the 256 MiB carve-out (silicon_sysmem_manager.cpp:351-359, 400) — and per-channel `physical_address = iova + ch * 1GiB` (silicon_sysmem_manager.cpp:392-443). "Fake" channel mappings point into this buffer (378-387).

> **Porting note:** Windows has no hugetlbfs; the equivalent is `VirtualAlloc(MEM_LARGE_PAGES)` (2 MiB, and 1 GiB where supported with `SeLockMemoryPrivilege`) or, more robustly, letting the KMD allocate contiguous/remapped common buffers. The *contract* the port must keep is: (1) each channel is 1 GiB of host memory visible to the device at `pcie_base + ch*1GiB` NOC address, (2) WH channel 3 is limited to 768 MiB, (3) cross-process sharing of channel 0's legacy file name is a Linux-ism that likely need not be preserved. The `nr_hugepages`-based channel count computation must be replaced with a policy suited to the Windows allocator.

---

### 6. The PCIe DMA bounce buffer

`allocate_pcie_dma_buffer()` (pci_device.cpp:1004-1036) runs at the end of the PCIDevice ctor for WH/BH only. It tries `dma_buf_size = 512 KiB` ("empirical sweet spot", pci_device.cpp:1010-1011, 1018) and **halves on failure** down to one page. Every attempt allocates `dma_buf_size + page` — the extra page is a **completion flag page** polled by the host because "we'll need to poll a completion page to know when the DMA is done instead of receiving an interrupt" (pci_device.cpp:1013-1016; layout: `completion = buffer + dma_buf_size`, `completion_pa = buffer_pa + dma_buf_size`, pci_device.cpp:947-951, 991-995).

- **IOMMU:** anonymous `mmap` + `map_for_dma` (PIN_PAGES); on pin failure the mapping is munmapped and a smaller size tried (pci_device.cpp:937-960).
- **No IOMMU:** `ALLOCATE_DMA_BUF` with `buf_index = 0` then `mmap(fd, out.mapping_offset)` (pci_device.cpp:962-1002). If the mmap fails the buffer is *leaked until fd close* (984-986). The `NOC_DMA` allocate-flag (ioctl.h:91) is never used.

DMA transfers chunk through this bounce buffer under an in-process `dma_mutex_` (pcie_protocol.cpp:210-252) *and* a cross-process `PCIE_DMA` robust mutex (tt_device.cpp:755-756, 778-779, 801-802); the actual engine is programmed through `bar2_uc` MMIO (pcie_protocol.cpp:303-347), with 4-byte alignment/size validation throwing RuntimeError (311-317, 334-340). DMA is a best-effort fast path: if the buffer is absent the code falls back to TLB-window I/O (tt_device.cpp:758-766).

---

### 7. TLB windows: allocation, configuration, I/O

#### 7.1 Allocation (ioctl) and counts

`tt_tlb_alloc` (tt_kmd_lib.c:405-444): `ALLOCATE_TLB{size}` → `{id, mmap_offset_uc, mmap_offset_wc}` → mmap of the chosen cache mode. On mmap failure it frees the TLB and restores `errno` (428-439). `tt_tlb_free` munmaps then `FREE_TLB{id}` (446-462). Hardware inventory hard-coded in tt_kmd_lib.c:26-45 and tt_kmd_lib.h:321-329:

- Wormhole: 156× 1 MiB, 10× 2 MiB, 20× 16 MiB.
- Blackhole: 202× 2 MiB, 8× 4 GiB.
- "The driver may reserve one or more TLB windows for internal use." (tt_kmd_lib.h:331)

`PCIDevice::allocate_tlb` wraps failures; for KMD ≥ 2.6 the error message directs users to **`/sys/kernel/debug/tenstorrent/N/mappings`** and **`/proc/driver/tenstorrent/N/pids`** (pci_device.cpp:820-843).

#### 7.2 Configuration — two parallel mechanisms

1. **Ioctl path:** `CONFIGURE_TLB{id, config}` (tt_kmd_lib.c:474-516) — used by the convenience `tt_noc_*` helpers; `-EINVAL` if `addr` is not window-size aligned (477-479, 502-504).
2. **Direct MMIO path (the hot path):** `SiliconTlbHandle::configure` divides `local_offset` by the window size and calls `PCIDevice::configure_tlb`, which writes the packed config **directly into the mmapped TLB-config page** — *not* through the ioctl (silicon_tlb_handle.cpp:43-54; pci_device.cpp:845-894). Registers are `index × 8` bytes (WH) or `index × 12` bytes (BH) from `STATIC_TLB_CFG_ADDR` (wormhole_implementation.hpp:518, blackhole_implementation.hpp:483), written strictly as 32-bit stores because 64-bit stores to non-8-byte-aligned Device/UC memory SIGBUS on aarch64 (pci_device.cpp:857-873).

This means the KMD's `ALLOCATE_TLB` id namespace and the hardware TLB-config register indices are **the same namespace**: UMD takes the `id` returned by the ioctl and pokes `tlb_config_space + id * reg_size` itself. A Windows KMD must preserve that property (or the port must switch fully to the CONFIGURE_TLB ioctl).

#### 7.3 I/O through windows

Reads/writes are volatile loads/stores plus arch-tuned `memcpy_to/from_device` on the mapped pointer (silicon_tlb_window.cpp:97-271). Register accesses are 32-bit loops with 4-byte-multiple validation done by callers (local_chip.cpp:306-313, 336-343).

---

### 8. sysfs / procfs read inventory (complete)

| Path | Purpose | Cite |
|---|---|---|
| `/dev/tenstorrent/` (readdir) | device enumeration | pci_device.cpp:230-234, 1077-1095 |
| `/sys/module/tenstorrent/version` | KMD semver | pci_device.cpp:805-818; silicon_sysmem_manager.cpp:324 |
| `/sys/bus/pci/devices/<bdf>/<attr>` (`numa_node`, `revision`, `iommu_group/type`) | device attributes | pci_device.cpp:46-106, 391-395 |
| `/sys/bus/pci/devices/<bdf>/config` (binary, offset 4) | command byte for BH legacy reset | pci_device.cpp:108-123, 915-935 |
| `/sys/bus/pci/slots/*/address` | physical slot number | pci_device.cpp:130-171 |
| `/sys/bus/pci/devices/<bdf>/tenstorrent/` (readdir, `tenstorrent!N`) | BDF→minor mapping (cpuset lib); reset reappearance glob | cpuset_lib.cpp:108-146; warm_reset.cpp:142 |
| `/sys/kernel/mm/hugepages/hugepages-1048576kB/nr_hugepages` | hugepage count | hugepage.cpp:32; silicon_sysmem_manager.cpp:256 |
| `/proc/mounts` | hugetlbfs mount discovery | hugepage.cpp:119 |
| `/proc/cmdline`, `/proc/meminfo`, `/proc/buddyinfo` | diagnostics on failure only | silicon_sysmem_manager.cpp:254, 325-326 |
| `/sys/kernel/debug/tenstorrent/N/mappings`, `/proc/driver/tenstorrent/N/pids` | referenced in error text only (never read programmatically) | pci_device.cpp:837-838 |

Environment variables read in the device layer: `TT_VISIBLE_DEVICES` (pci_device.cpp:236; also cluster_descriptor.cpp:279), `TT_BACKEND_CPUSET_ALLOCATOR_*` (cpuset_lib.cpp:37-40, 232).

---

### 9. Cross-process and threading model

#### 9.1 Cross-process locks live in userspace, not the KMD

tt-umd does **not** use the KMD's `LOCK_CTL` resource locks (defined ioctl.h:26, 211-229 with `TENSTORRENT_RESOURCE_LOCK_COUNT 64` at ioctl.h:47 — zero call sites). Instead it builds robust cross-process mutexes from POSIX shm:

- `shm_open("TT_UMD_LOCK.<name>", O_RDWR|O_CREAT|O_EXCL, 0666)` with EEXIST fallback to plain open, under `umask(0)` (robust_mutex.cpp:250-274; prefix robust_mutex.hpp:31; perms robust_mutex.cpp:44).
- `ftruncate` to `sizeof(pthread_mutex_wrapper)` = pthread_mutex + `initialized` flag `0x5454554d444d5458` ("TTUMDMTX", robust_mutex.cpp:46) + owner tid/pid (robust_mutex.hpp:71-81); mmap `MAP_SHARED` (robust_mutex.cpp:318); the shm fd is closed after mapping (robust_mutex.cpp:242-245).
- pthread mutex initialized `PTHREAD_PROCESS_SHARED` + `PTHREAD_MUTEX_ROBUST` (robust_mutex.cpp:326-360); `EOWNERDEAD` is recovered with `pthread_mutex_consistent` (robust_mutex.cpp:441-451, 480-487).
- First-use initialization is serialized by `flock(shm_fd, LOCK_EX)` plus a process-local static mutex (robust_mutex.cpp:58-94, 204-237).

Lock names (lock_manager.cpp:20-23): `<TYPE>_<device_id>_<devtype>`, e.g. `TT_UMD_LOCK.PCIE_DMA_0_PCIe`. Types: `ARC_MSG`, `REMOTE_ARC_MSG`, `NON_MMIO`, `MEM_BARRIER`, `CREATE_ETH_MAP`, `CHIP_IN_USE`, `PCIE_DMA` (lock_manager.hpp:18-33, 43-51). `CHIP_IN_USE` is held for the whole `start_device`…`close_device` interval (local_chip.cpp:146, 171); `MEM_BARRIER` makes host↔device barriers atomic across processes (local_chip.cpp:438-443); `PCIE_DMA` serializes bounce-buffer DMA across processes (tt_device.cpp:755-756).

> **Porting note:** all of this maps cleanly to Windows named mutexes (`Global\` namespace), which are inherently robust (`WAIT_ABANDONED` ≈ `EOWNERDEAD`). What must be preserved is the *naming discipline keyed on the OS-level device id* so independent processes agree, and the CHIP_IN_USE / PCIE_DMA / MEM_BARRIER semantics. The KMD itself needs no lock support (LOCK_CTL can be left unimplemented for tt-umd's sake).

#### 9.2 Threading assumptions on the fd

- Both fds are shared by all threads of the process without any userspace lock around plain ioctls — e.g. concurrent `tt_tlb_alloc/free/map` from multiple threads (tt_kmd_lib.c has no locking). **The driver must support concurrent ioctls on one open file.**
- In-process serialization exists only above the fd: `std::mutex io_lock_` per protocol object (pcie_protocol.cpp:126, 144), `wc_tlb_lock`/`uc_tlb_lock` for cached windows (local_chip.cpp:262, 288, 320, 352), `dma_mutex_` (pcie_protocol.cpp:211).
- NOC selection is thread-local (`thread_local NocId tls_noc_id`, noc_access.cpp:12-18) — no fd-level state involved.
- `sysconf(_SC_PAGESIZE)` is queried repeatedly and cached in function-local statics (pci_device.cpp:639, 688, 738, 776; sysmem_buffer.cpp:199); all pinning alignment logic is expressed in the host page size.

#### 9.3 Signals, fork/exec, crash cleanup

- **SIGBUS:** UMD optionally installs a process-wide `SIGBUS` handler with `sigsetjmp/siglongjmp` recovery so that MMIO to a dead/hot-reset device throws `SigbusError` instead of killing the process; without an active jump point the handler `_exit(sig)`s (silicon_tlb_window.cpp:29-67, 273-283). This is the mechanism behind the `safe_*` I/O API.
- **exec:** every fd is `O_CLOEXEC`; nothing is intended to be inherited.
- **fork:** no special handling; correctness across fork relies on KMD per-fd semantics plus the shm robust mutexes.
- **Crash cleanup relied upon from the KMD:** on fd close/process exit the driver must release: pinned pages, TLB windows, DMA buffers ("That only happens when we close the fd", pci_device.cpp:984-985), power-flag contributions ("When the file descriptor is closed, its contribution is removed", ioctl.h:345), and NOC-aperture reservations (the "stale or crashed process holds sysmem NOC address space" recovery advice presumes the aperture frees when the stale process dies — silicon_sysmem_manager.cpp:303-310, 413-424). `SET_NOC_CLEANUP` (fd-close NOC write, ioctl.h:308-337) exists for device-side cleanup but tt-umd never registers one.

> **Porting note:** SIGBUS-on-MMIO-failure has no direct analogue; on Windows, accesses to a mapped BAR of a removed device typically produce an access violation once the mapping is torn down, catchable via SEH (`__try/__except`) — the `safe_*` API should be ported onto SEH, and the KMD should invalidate user mappings on surprise removal rather than letting reads return all-FFs silently (UMD's hang detectors *also* rely on all-FF reads: `HANG_READ_VALUE = 0xFFFFFFFF`, architecture_implementation.hpp:29, compared in hang_detector.cpp:38-46; e.g. blackhole_tt_device.cpp:340 masks `bar_read32(...) & 0x3F`).

---

### 10. tt_kmd_lib public C API (secondary ABI consumers)

`tt_kmd_lib.h` is shipped as a stable C API over the same ioctls (used by tools and by PCIDevice itself). Its semantics constrain the shim identically: `tt_device_open` returns `-errno` conventions (tt_kmd_lib.c:64-90); `tt_device_get_attr` derives arch from PCI device id `0x401e` (WH) / `0xb140` (BH) (tt_kmd_lib.c:102-106; pci_ids.h:13-14); `tt_noc_read/write{,32}` validate 4-byte alignment (`-EINVAL`) and allocate/configure/free a 2 MiB window per call (tt_kmd_lib.c:183-323); `tt_device_reset` invalidates the handle by closing it on success (tt_kmd_lib.c:549).

---

### Key constants table

| Name | Value | Source |
|---|---|---|
| Device node path pattern | `/dev/tenstorrent/<N>` | pci_device.cpp:202, 230, 355, 387 |
| Main open flags | `O_RDWR \| O_CLOEXEC` (no `O_APPEND`) | pci_device.cpp:381 |
| Enumeration/reset open flags | `O_RDWR \| O_CLOEXEC \| O_APPEND` | pci_device.cpp:355, 204, 1101 |
| `TENSTORRENT_IOCTL_MAGIC` | `0xFA`, commands `_IO(0xFA, 0..15)` | ioctl.h:16, 18-33 |
| `TENSTORRENT_DRIVER_VERSION` (API) | 2 | ioctl.h:14 |
| Minimum KMD version | 1.34.0 (`KMD_TLBS`) | kmd_versions.hpp:36; pci_device.cpp:402-405 |
| `KMD_IOMMU` / `KMD_MAP_TO_NOC` / `KMD_ARCH_AGNOSTIC_RESET` / `KMD_POWER_STATE` | 1.29.0 / 2.0.0 / 2.4.1 / 2.6.0 | kmd_versions.hpp:15, 23, 29, 43 |
| Mapping IDs RESOURCE0/1/2 UC/WC | 1/2, 3/4, 5/6 (0 = unused) | ioctl.h:36-42 |
| `bar0_size` (mapped portion of BAR0) | 3 MiB | pci_device.hpp:354 |
| `bar0_mapping_offset` / `BAR0_OFFSET` | 509 MiB = `0x1FD00000` | pci_device.hpp:406; pcie_protocol.hpp:92 |
| `STATIC_TLB_CFG_ADDR` (BAR0 offset of TLB cfg regs) | `0x1fc00000` (WH and BH) | wormhole_implementation.hpp:199; blackhole_implementation.hpp:174 |
| `tlb_config_space_size` | 4 KiB | pci_device.hpp:356 |
| TLB cfg register stride | WH 8 B, BH 12 B (32-bit stores only) | wormhole_implementation.hpp:518; blackhole_implementation.hpp:483; pci_device.cpp:857-873 |
| TLB window sizes | 1M/2M/16M (WH), 2M/4G (BH) | tt_kmd_lib.h:107-110 |
| TLB window counts | WH 156/10/20; BH 202(2M)/8(4G) | tt_kmd_lib.c:26-45 |
| PIN_PAGES flags | CONTIGUOUS=1, NOC_DMA=2, NOC_TOP_DOWN=4 | ioctl.h:171-173 |
| NOC-mapping limits | WH: len ∈ [0x1000, 0xFFFE0000], cum 0xFFFE0000, 16 max; BH: len ∈ [0x1000, 0xFFFFF000], 16 max | tt_kmd_lib.h:264-271 |
| RESET_DEVICE flags | 0 RESTORE_STATE, 1 RESET_PCIE_LINK, 2 CONFIG_WRITE, 3 USER_RESET, 4 ASIC_RESET, 5 ASIC_DMC_RESET, 6 POST_RESET | ioctl.h:145-153 |
| Power flags | MAX_AI_CLK=1, MRISC_PHY_WAKEUP=2, TENSIX_ENABLE=4, L2CPU_ENABLE=8; "busy" = 2\|4\|8 | ioctl.h:366-369; pci_device.cpp:1066-1068 |
| `HUGEPAGE_REGION_SIZE` | 1 GiB | hugepage.hpp:17 |
| `MAX_HOST_MEM_CHANNELS` | 4 | hugepage.hpp:19 |
| `HUGEPAGE_CHANNEL_3_SIZE_LIMIT` (WH) | 768 MiB | silicon_sysmem_manager.hpp:20 |
| Sysmem NOC base (`pcie_base`) | WH `0x8_0000_0000`; BH `4ULL << 58` | sysmem_manager.cpp:79-88 |
| Hugetlbfs mount point expected | `/dev/hugepages-1G` | hugepage.cpp:29 |
| DMA bounce buffer | 512 KiB initial, halved to ≥1 page; +1 completion page | pci_device.cpp:1018-1035, 939, 966 |
| `TENSTORRENT_MAX_DMA_BUFS` / `MAX_INBOUND_TLBS` | 256 / 256 | ioctl.h:44-45 |
| PCI IDs | vendor 0x1e52; WH device 0x401e; BH 0xb140 | cpuset_lib.hpp:59; pci_ids.h:13-14 |
| Shm mutex prefix / init flag | `TT_UMD_LOCK.` / `0x5454554d444d5458` | robust_mutex.hpp:31; robust_mutex.cpp:46 |
| WH B0 required PCI revision | 0x01 (assert) | pci_device.cpp:440-441 |

### Open questions

1. **`QUERY_MAPPINGS` out-count semantics.** UMD sets `in.output_mapping_count = 8` and, after the ioctl, iterates `in.output_mapping_count` entries (pci_device.cpp:449, 466). Whether the KMD rewrites this field to the number of valid mappings, or leaves it and pads with `mapping_id == 0`, must be confirmed against the tt-kmd source (section covering `ioctl.c`); the shim must reproduce whichever it is.
2. **Wormhole BAR check vs. mapping mismatch.** For WH the ctor *checks* that `RESOURCE2_UC` (BAR4) exists but then mmaps `bar2_uc_mapping` (`RESOURCE1`, BAR2) (pci_device.cpp:532-547). It is unclear whether this is intentional (BAR2 presence implied by BAR4?) or a latent bug; a Windows port should verify which BAR WH actually needs for `bar2_uc` consumers (pcie_protocol.cpp:305, 328).
3. **In-place mmap-offset arithmetic.** UMD adds byte offsets to `mapping_base` (509 MiB, `0x1fc00000`) assuming the KMD's mmap offset space is linear within a resource. Confirm from the KMD mmap handler that arbitrary page-aligned offsets within a mapping are legal (vs. only whole-resource maps), since the shim must match.
4. **Behavior of `map_for_hugepage` returning physical address 0.** UMD treats `physical_address == 0` as a failure sentinel (silicon_sysmem_manager.cpp:315). If a legitimate pin could ever produce IOVA/PA 0, this path misbehaves; worth pinning down for the shim's address allocation policy.
5. **Ioctls unused by tt-umd (`GET_HARVESTING`, `LOCK_CTL`, `MAP_PEER_BAR`, `FREE_DMA_BUF`, `SET_NOC_CLEANUP`).** Out-of-repo consumers (tt-smi, luwen-based tools, tt-metal debug tooling) may still use them; this section only proves tt-umd does not. Scope for the Windows shim's ioctl coverage needs a decision informed by the other analysis sections.
6. **`O_APPEND` policy going forward.** The main handle's `O_APPEND` is "temporarily disabled" pending tt-umd issue #2531 (pci_device.cpp:379-380). The Windows equivalent (a create-option or first-ioctl flag selecting legacy vs. modern power semantics) should support both modes since UMD may flip this back.
7. **Completion-page DMA protocol.** The bounce-buffer completion page is polled by userspace and the whole mechanism is called "a temporary hack until it's implemented in the driver" (pci_device.cpp:1013-1016). The exact producer of the completion write is in the DMA strategy code (`std::visit(... d2h_transfer ...)`, pcie_protocol.cpp:323, 346) which lives outside the files read here; the Windows design should decide whether to reproduce the hack or implement interrupt-driven DMA in the KMD from the start.
8. **hwloc dependency on Windows.** `cpuset_lib`'s device counting and NUMA binding (used to size hugepage channels and bind sysmem) is hwloc + sysfs; whether the Windows port keeps hwloc (it supports Windows) or replaces it with `GetNumaProcessorNode`-family APIs is open.

---

## 14. Hugepages and System Tools

### Scope

This section covers the host-side provisioning tooling that ships in the
`tt-system-tools` repository — the 1GB-hugepages service that reserves and
mounts host memory for Tenstorrent ASICs, the `tt-oops` diagnostic collector,
and the packaging metadata (deb/rpm) that installs them — plus a trace of how
`tt-umd` userspace *consumes* the hugepage configuration and how that ties back
to the kernel driver's page-pinning path.

Files read in full:

- `tt-system-tools/hugepages-setup/hugepages-setup.sh` (83 lines)
- `tt-system-tools/hugepages-setup/tenstorrent-hugepages.service` (15 lines)
- `tt-system-tools/hugepages-setup/dev-hugepages\x2d1G.mount` (15 lines)
- `tt-system-tools/hugepages-setup/README.md` (32 lines)
- `tt-system-tools/tt-oops/tt-oops.sh` (1292 lines)
- `tt-system-tools/tt-oops/DESIGN.md` (225 lines)
- `tt-system-tools/tt-oops/Makefile` (70 lines)
- `tt-system-tools/README.md` (28 lines)
- `tt-system-tools/tenstorrent-tools.spec` (78 lines)
- `tt-system-tools/debian/control` (15 lines), `debian/rules` (12 lines),
  `debian/changelog` (71 lines), `debian/tenstorrent-tools.install` (6 lines),
  `debian/tenstorrent-tools.postinst` (37 lines),
  `debian/tenstorrent-tools.lintian-overrides` (3 lines)
- `tt-system-tools/dev-scripts/build-rpm.sh` (19 lines)
- `tt-system-tools/.gitlab-ci.yml` (21 lines)

Cross-referenced (consumers of hugepages; not the primary subject of this
section):

- `tt-umd/device/hugepage.hpp` / `hugepage.cpp`
- `tt-umd/device/chip_helpers/silicon_sysmem_manager.cpp`
- `tt-umd/device/api/umd/device/chip_helpers/silicon_sysmem_manager.hpp`
- `tt-umd/device/chip/local_chip.cpp`
- `tt-kmd/memory.c` (pin-pages path), `tt-kmd/enumerate.h` (PCI IDs)

---

### 14.1 What the hugepages service configures

#### Page size: always 1GB

The whole scheme is built on **1GB hugepages exclusively**. The setup script
only ever writes to the 1GB hugepage sysfs directory:

```sh
HUGEPAGE_DIR="${NODEDIR}/hugepages/hugepages-1048576kB"
```
(`hugepages-setup.sh:67`) — `1048576kB` = 1 GiB. The mount unit also pins the
page size to 1G:

```
Options=pagesize=1G,mode=0777,nosuid,nodev
```
(`dev-hugepages\x2d1G.mount:12`). The `tt-umd` side agrees:
`HUGEPAGE_REGION_SIZE = 1ULL << 30; // 1GB` (`tt-umd/device/hugepage.hpp:17`),
and the comment right above it is load-bearing: *"Hugepages must be 1GB in
size"* (`hugepage.hpp:15-16`).

#### Count per device (default policy)

The script detects each ASIC by PCI vendor/device ID and allocates a fixed
number of 1GB pages per device (`hugepages-setup.sh:11-14, 53-56`):

```sh
TT_VID=1e52      # Tenstorrent PCI vendor ID
GS_PID=faca      # Grayskull
WH_PID=401e      # Wormhole
BH_PID=b140      # Blackhole
...
get_node_pages "${TT_VID}:${BH_PID}" 4   # Blackhole: 4 pages
get_node_pages "${TT_VID}:${WH_PID}" 4   # Wormhole:  4 pages
get_node_pages "${TT_VID}:${GS_PID}" 1   # Grayskull: 1 page
```

So the **defaults are 4×1GB per Wormhole, 4×1GB per Blackhole, 1×1GB per
Grayskull** (also stated in `hugepages-setup.sh:32-34` and
`hugepages-setup/README.md:17-20`). These PCI IDs match the kernel driver
exactly: `PCI_VENDOR_ID_TENSTORRENT 0x1E52`, `PCI_DEVICE_ID_GRAYSKULL 0xFACA`,
`PCI_DEVICE_ID_WORMHOLE 0x401E`, `PCI_DEVICE_ID_BLACKHOLE 0xB140`
(`tt-kmd/enumerate.h:15-18`).

The "4 pages per Wormhole" default is what backs the userspace maximum of **4
host-memory channels per device** (`MAX_HOST_MEM_CHANNELS = 4`,
`tt-umd/device/hugepage.hpp:19`; asserted again in
`silicon_sysmem_manager.cpp:116-119`: *"Only 4 host memory channels are
supported per device"*). One 1GB hugepage backs one host-memory channel.

#### Override mechanism

An operator can override the total via a file
`/opt/tenstorrent/bin/hugepages-override.txt` containing a single integer =
*total* pages across all devices (`hugepages-setup.sh:35-48`):

```sh
HP_OVERRIDE=$(<"$file_path")
TT_COUNT=$(lspci -d "${TT_VID}": | wc -l)
HP_COUNT=$((HP_OVERRIDE / TT_COUNT))
```

The override total is divided evenly (integer division) by the count of TT
devices, and each detected device gets `HP_COUNT` pages regardless of arch
(`hugepages-setup.sh:44-48`, `README.md:24-25`). Note integer division can
under-allocate when the total is not a multiple of the device count.

#### NUMA placement

Placement is **per-NUMA-node**, keyed off each device's NUMA node. The script
parses `lspci -vmm` for the `NUMANode:` field and defaults to node 0 when a
device does not advertise one (`hugepages-setup.sh:18-24`):

```sh
lspci -d "${VIDPID}" -vmm | awk "BEGIN {n=0} /NUMANode:/ {n=\$2} /^$/ {print n \" ${MULT}\"}"
```

Pages needed on the same node are summed into an associative array `nodes[]`
(`hugepages-setup.sh:42-43, 50-51`), then written to the per-node sysfs knob:

```sh
NODEDIR="/sys/devices/system/node/node${n}"
HUGEPAGE_DIR="${NODEDIR}/hugepages/hugepages-1048576kB"
echo "${nodes[$n]}" > "${HUGEPAGE_DIR}/nr_hugepages"
```
(`hugepages-setup.sh:64-73`). So a 2-device box with one device on node 0 and
one on node 1 gets 4 pages reserved on *each* node, not 8 on node 0. This is
deliberate: the pages must be physically local to the NUMA node of the device
that will DMA to them.

#### Error handling / verification

- Missing NUMA node directory or missing 1GB hugepage sysfs dir →
  `error_out` (prints to stderr, `exit 1`) (`hugepages-setup.sh:66,68`).
- Write to `nr_hugepages` failing → `error_out` (`hugepages-setup.sh:73`).
- After writing, it **re-reads** `nr_hugepages` and fails if the kernel could
  not satisfy the request (memory too fragmented to assemble contiguous 1GB
  pages): *"Failed to get requested N hugepages, only got M"*
  (`hugepages-setup.sh:74-78`). This is the classic failure mode — 1GB pages
  usually must be reserved at/near boot before memory fragments.
- `set -eo pipefail` at the top makes any un-caught command failure fatal
  (`hugepages-setup.sh:9`).

#### Mount point

The reserved pages are exposed to userspace as a hugetlbfs mount at
`/dev/hugepages-1G`:

```
What=hugetlbfs
Where=/dev/hugepages-1G
Type=hugetlbfs
Options=pagesize=1G,mode=0777,nosuid,nodev
```
(`dev-hugepages\x2d1G.mount:9-12`). `mode=0777` makes the mount world
writable so any (non-root) process can create backing files there.
`ConditionPathExists=/sys/kernel/mm/hugepages/hugepages-1048576kB` guards the
mount so it only runs on kernels that expose 1GB hugepages, and
`ConditionCapability=CAP_SYS_ADMIN` guards for the mount privilege
(`dev-hugepages\x2d1G.mount:5-6`).

#### systemd ordering and lifecycle

- `tenstorrent-hugepages.service` is a `Type=oneshot` unit run as
  `User=root`, ordered `Before=sysinit.target` with
  `DefaultDependencies=no`, i.e. it runs very early in boot before most of
  the system is up (so 1GB pages can still be assembled)
  (`tenstorrent-hugepages.service:2-9, 15`). `SuccessExitStatus=0`,
  `Restart=no`, `TimeoutStopSec=10s` (`:10-12`).
- The `.mount` unit is likewise `Before=sysinit.target`,
  `DefaultDependencies=no`, `WantedBy=sysinit.target`
  (`dev-hugepages\x2d1G.mount:3-4,14-15`).
- The service reserves the pages (writes `nr_hugepages`); the mount unit
  exposes them at `/dev/hugepages-1G`. They are independent units — the
  service does not itself mount, and the mount does not itself reserve.

#### Kernel command-line / IOMMU expectation

The Debian `postinst` adds a kernel parameter `iommu=pt` (passthrough) to
GRUB (`debian/tenstorrent-tools.postinst:4-24`):

```sh
CUSTOM_KERNEL_PARAMETERS="iommu=pt"
```

`iommu=pt` puts the IOMMU into pass-through (identity) mode. This matters for
the driver: with an identity IOMMU domain the driver treats DMA addresses as
physical addresses; hugepages give large physically-contiguous regions so the
device sees one contiguous window. The `postinst` handles missing GRUB
gracefully (CI/minimal environments) (`postinst:6-35`).

> **Porting note (IOMMU model):** On Linux the choice is binary at the UMD
> level: either the IOMMU is in identity/passthrough mode and UMD uses
> hugepages (`pin_or_map_hugepages`), or it is translating and UMD uses an
> anonymous `mmap` mapped through the IOMMU (`pin_or_map_iommu`) — see
> `silicon_sysmem_manager.cpp:124-131`. Windows has no `iommu=pt` boot flag;
> the equivalent decision is made by whether the device is behind DMA
> remapping (the Windows DMA remapping / kernel DMA protection state). A
> Windows KMDF port must decide which model it emulates and cannot rely on a
> GRUB edit to select it.

---

### 14.2 How userspace (tt-umd) consumes the hugepages

The service side only *reserves and mounts*. UMD is the consumer and encodes
the contract:

1. **Discover the mount.** `find_hugepage_dir(pagesize)` scans `/proc/mounts`
   for a hugetlbfs line whose mount path is exactly `/dev/hugepages-1G`
   (`hugepage.cpp:29, 114-160`). It parses the `pagesize=` mount option,
   converts the K/M/G/T suffix to bytes (`hugepage.cpp:117, 125-145`), and
   only returns the directory if the mount's page size equals the requested
   size (1GB) (`hugepage.cpp:146-148`). If no match, it warns and returns an
   empty string (`hugepage.cpp:153-159`), and sysmem init bails out
   (`silicon_sysmem_manager.cpp:201-208`).

2. **Read how many pages exist.** `get_num_hugepages()` reads
   `/sys/kernel/mm/hugepages/hugepages-1048576kB/nr_hugepages`
   (`hugepage.cpp:31-48`) — the global 1GB counter that aggregates the
   per-node `nr_hugepages` values the setup script wrote.
   If the file cannot be opened it throws `RuntimeError` (`hugepage.cpp:42-45`).

3. **Compute channels per device.**
   `get_available_num_host_mem_channels()` divides total hugepages by the
   number of TT MMIO devices of that arch, clamped into
   `[1, num_channels_per_device_target]` and asserted `<= MAX_HOST_MEM_CHANNELS`
   (`hugepage.cpp:50-112`, especially `:70-71`):

   ```cpp
   num_channels_per_device_available =
       std::min(num_channels_per_device_target,
                std::max((uint32_t)1, total_hugepages / num_tt_mmio_devices_for_arch));
   ```

   It emits warnings for the common misconfigurations: fewer hugepages than
   devices (`hugepage.cpp:82-91`), fewer available channels than requested
   (`:93-101`), and mixed-arch "hybrid" systems (`:75-80`).

4. **Open one backing file per (device, channel).**
   `open_hugepage_file()` builds a filename under `/dev/hugepages-1G`.
   Device 0 channel 0 uses the bare name `tenstorrent`; other
   device/channel combinations prefix `device_<id>_` and `channel_<ch>_`
   (`hugepage.cpp:162-195`). It temporarily sets `umask(0)` and opens
   `O_RDWR|O_CREAT|O_CLOEXEC` with mode `0666`
   (`S_IWUSR|S_IRUSR|S_IWGRP|S_IRGRP|S_IWOTH|S_IROTH`)
   (`hugepage.cpp:197-200`). On `EACCES` it unlinks and retries once
   (`hugepage.cpp:201-209`) — this handles a stale file left with wrong
   ownership by a previous run/user.

5. **mmap and pin.** `init_hugepages()` mmaps each 1GB file
   `MAP_SHARED|MAP_POPULATE` (`silicon_sysmem_manager.cpp:235-236`), migrates
   the pages to the device's NUMA node via
   `cpuset_allocator::bind_area_to_memory_nodeset` (a second NUMA-affinity
   safety net on top of the setup script's placement)
   (`silicon_sysmem_manager.cpp:263-272`), then `pin_or_map_hugepages()`
   pins each mapping to the device through the KMD
   (`silicon_sysmem_manager.cpp:281-343`). The pin ioctl is
   `TENSTORRENT_IOCTL_PIN_PAGES` (`tt-kmd/ioctl.h:21`), handled by
   `ioctl_pin_pages` (`tt-kmd/memory.c:544`).

#### The Wormhole 4th-channel carveout (768MB)

There is a hardware-driven quirk: on Wormhole B0, channel 3's mapping to the
NOC is limited to **768MB**, not the full 1GB, to avoid overlapping the PCIe
register space in the device address map:

```cpp
static constexpr size_t HUGEPAGE_CHANNEL_3_SIZE_LIMIT = 768 * (1 << 20);
```
(`tt-umd/device/api/umd/device/chip_helpers/silicon_sysmem_manager.hpp:20`).
It appears anywhere a per-channel NOC mapping size is computed:
`silicon_sysmem_manager.cpp:160-162, 290-292, 383-385` and
`local_chip.cpp:396-401`. The full 1GB is still mmapped
(`silicon_sysmem_manager.cpp:235-236`), but the KMD pin / NOC mapping for
channel 3 is issued with the truncated 768MB `actual_size`
(`silicon_sysmem_manager.cpp:290-292, 297, 312`). The IOMMU (no-hugepage) path
mirrors the same carveout: `carveout_size = HUGEPAGE_REGION_SIZE -
HUGEPAGE_CHANNEL_3_SIZE_LIMIT; // 1GB - 768MB = 256MB`
(`silicon_sysmem_manager.cpp:351`).

#### Kernel-side size ceiling

The driver caps a single pin at 1GB on old kernels (`<= 5.4`) because a 2GB
pinning could soft-lock on teardown (`tt-kmd/memory.c:532-542`):

```c
#if LINUX_VERSION_CODE <= KERNEL_VERSION(5, 4, 0)
	return size <= 1 << 30;
#else
	return true;
#endif
```

This is consistent with one 1GB hugepage per pin. `ioctl_pin_pages` requires
page-aligned VA and size and non-zero size (`-EINVAL` otherwise,
`memory.c:578-579`).

> **Porting note (consumption contract):** The Linux consumer contract is
> encoded as three filesystem touchpoints — a hugetlbfs mount at
> `/dev/hugepages-1G`, a sysfs count at
> `.../hugepages-1048576kB/nr_hugepages`, and per-device backing files named
> `tenstorrent` / `device_<n>_channel_<c>_tenstorrent`. None of these exist on
> Windows. A Windows port must provide an equivalent way for UMD (or its
> Windows analog) to (a) learn how much contiguous 1GB-granular host memory is
> reserved, (b) allocate up to 4 such 1GB regions per device, and (c) share
> them by name across cooperating processes. The naming convention encodes the
> "one pipeline per system, one hugepage per device/channel" assumption
> (`hugepage.hpp:31-35`) — a Windows shared-section/named-object scheme would
> have to reproduce that cross-process aliasing so multiple processes attach to
> the same physical buffer.

---

### 14.3 Host-memory expectations summarized

- **Granularity:** 1GB, hard requirement (mount, sysfs path, and
  `HUGEPAGE_REGION_SIZE` all fixed at 1 GiB).
- **Per-device counts (default):** Wormhole 4, Blackhole 4, Grayskull 1.
- **Max channels/device:** 4 (`MAX_HOST_MEM_CHANNELS`).
- **Physical contiguity:** each channel is one physically-contiguous 1GB
  region (that is the point of a hugepage; the driver's DMA path and the
  `TENSTORRENT_PIN_PAGES_CONTIGUOUS` flag, `tt-kmd/ioctl.h:169`, exist to let
  the app attest contiguity).
- **NUMA locality:** pages reserved on the device's own NUMA node (script) and
  re-bound to it at map time (UMD).
- **Wormhole channel-3 NOC window:** 768MB, not 1GB.
- **IOMMU:** either `iommu=pt` (hugepage path) or translating IOMMU (anonymous
  mmap path); the two paths are mutually exclusive per device.

---

### 14.4 tt-oops (diagnostic collector) and its interaction with the driver

`tt-oops.sh` is a **pure userspace, read-only diagnostic bundler**. It shells
out to standard tools and copies their output into a timestamped directory
that it then tars up (`tt-oops.sh:12, 1080-1097`). It has **no ioctl or
special interaction with the tenstorrent driver** — it only observes the
driver from the outside:

- Runs `tt-smi --snapshot` and `tt-info` if present (`tt-oops.sh:386-403`).
- `lsmod | grep tenstorrent` — checks the module is loaded
  (`tt-oops.sh:407`).
- `dmesg | grep -i tenstorrent` — pulls kernel log lines
  (`tt-oops.sh:408`).
- `lspci | grep -i tenstorrent` and `lspci -vv` detail (`tt-oops.sh:413-414`).
- `pip list | grep -i tenstorrent`, and copies `/var/log/tenstorrent/*.log`
  and `/etc/tenstorrent/*` if present (`tt-oops.sh:419-433`).

Everything else it collects is generic host telemetry: CPU/mem/disk/network
inventory (`collect_hardware_info`, `:146-250`), OS/packages/services
(`collect_software_info`, `:253-307`), config incl. `sysctl -a`
(`collect_detailed_system_info`, `:1052`), optional performance sampling with
`vmstat`/`iostat`/`mpstat`/`sar`/`pidstat` (`:468-550`), and logs from
`journalctl`/`dmesg`/`/var/log` (`collect_logs`, `:654-773`).

Notably for this section, tt-oops captures the exact state that a hugepage
failure would need: `cat /proc/meminfo` (`:158, 597`), the full `sysctl -a`
dump (which includes `vm.nr_hugepages*`) (`:1052`), and `dmesg`
(`:701-704`). This mirrors what UMD prints on hugepage failures:
`/proc/cmdline` and the 1GB `nr_hugepages` file on an mmap failure
(`silicon_sysmem_manager.cpp:254-256`), and `/proc/meminfo` /
`/proc/buddyinfo` on a pin failure (`silicon_sysmem_manager.cpp:324-326`).

Input validation / behavior of note:
- Collection level must be `basic|detailed|debug`; output format
  `text|json`; log level and `--logs` values are validated, else `exit 1`
  (`tt-oops.sh:1140-1204`).
- With no args it just prints usage and exits 0 (`tt-oops.sh:1228-1231`).
- Missing *required* deps (`lscpu free df lsblk uname ip grep pstree`) →
  `exit 1` (`tt-oops.sh:97-101, 124-129, 1240`); missing optional tools only
  warn (`:104-122`).
- Root is not required; detailed/debug levels warn that some data needs root
  (`tt-oops.sh:77-88`). Environment dump filters out `key|token|password|secret`
  (`:299`).

> **Porting note (tt-oops):** Nothing in tt-oops needs to be ported into the
> KMDF driver — it is an ops/support script. A Windows equivalent would be a
> separate PowerShell/CLI collector (Get-PnpDevice, `Get-CimInstance
> Win32_PhysicalMemory`, Event Log export, `tt-smi` if available). The one
> driver-relevant contract it documents is *where* diagnostics live on Linux
> (`/var/log/tenstorrent`, `/etc/tenstorrent`, module version at
> `/sys/module/tenstorrent/version`, referenced by UMD at
> `silicon_sysmem_manager.cpp:324`). A Windows port should decide on
> equivalent locations (e.g. Event Log source, a versioned registry key) so a
> Windows collector has something to read.

---

### 14.5 Packaging / install (how these land on a host)

- **Install paths** (`debian/tenstorrent-tools.install:1-5`,
  `tenstorrent-tools.spec:32-52`): scripts to `/opt/tenstorrent/bin/`,
  systemd units to `/lib/systemd/system/` (deb) or `%{_unitdir}` (rpm).
- **Backslash filename hack:** the mount unit filename literally contains
  `\x2d` (systemd's escape for `-` in `/dev/hugepages-1G`). rpmbuild cannot
  handle the backslash, so it is symlinked to `tt-hugepages-mount` for
  packaging (`tenstorrent-tools.spec:14-16, 40`; `dev-scripts/build-rpm.sh:15-16`).
- **Enablement on install:** rpm `%post` runs `systemctl daemon-reload` then
  `systemctl enable --now` for both `tenstorrent-hugepages.service` and the
  `.mount` unit (`tenstorrent-tools.spec:46-55`); deb relies on
  `dh_installsystemd` (`debian/rules:11-12`) plus the changelog note
  *"Automatically start services on install"* (`debian/changelog:5-8`).
- **Dependency:** `pciutils` (for `lspci`) is a hard package dependency
  (`debian/control:10`, `tenstorrent-tools.spec:10`).
- **Package name/version:** `tenstorrent-tools`, latest changelog entry
  `1.4.1` (`debian/changelog:1-4`).

> **Porting note (packaging):** The systemd oneshot + mount + `%post enable`
> model has no direct Windows analog. On Windows the hugepage reservation
> equivalent (large-page pool) would be provisioned either by the driver's own
> `INF`/coinstaller at install time or by a Windows service configured to run
> at boot, and the "mount" step disappears (there is no hugetlbfs — see
> §14.6). The `pciutils`/`lspci` device enumeration must be replaced with
> SetupAPI / the driver's own device enumeration.

---

### 14.6 What a Windows equivalent must meet (Porting notes)

> **Porting note (host-memory provisioning — the core requirement):** A
> Windows KMDF port must reserve, per Tenstorrent device, up to **four 1GB
> physically-contiguous host-memory regions** (Wormhole/Blackhole default 4,
> Grayskull default 1), placed on the **NUMA node local to the device**. On
> Linux this is 1GB hugetlbfs pages reserved early at boot; on Windows the
> equivalent is contiguous physical memory obtained via
> `MmAllocateContiguousMemorySpecifyCacheNode` /
> `MmAllocateContiguousNodeMemory` (NUMA-aware) or the large-page pool
> (`MEM_LARGE_PAGES` with `SeLockMemoryPrivilege`). The 1GB granularity is not
> arbitrary — UMD hard-codes `HUGEPAGE_REGION_SIZE = 1GB` and the driver's iATU
> region programming and the Wormhole channel-3 768MB carveout are all keyed to
> 1GB regions. A Windows allocator that hands back non-1GB-granular or
> non-contiguous memory would break the NOC address-map assumptions.

> **Porting note (counts and the channel↔hugepage identity):** The count is
> not decorative: `num_channels = total_hugepages / num_devices` (clamped 1..4)
> at `hugepage.cpp:70-71`. A Windows port must expose the number of reserved
> 1GB regions per device to whatever computes host-memory channels, and honor
> the ceiling of 4 (`MAX_HOST_MEM_CHANNELS`,
> `silicon_sysmem_manager.cpp:116-119`). Reserving fewer regions than devices
> silently degrades to 1 channel/device with warnings on Linux; a Windows port
> should decide whether to replicate that lenient behavior or fail hard.

> **Porting note (NUMA placement):** Both the setup script
> (`hugepages-setup.sh:18-24, 64-73`) and UMD
> (`silicon_sysmem_manager.cpp:263-272`) place/rebind host memory on the
> device's NUMA node, and UMD explicitly warns that getting this wrong
> "decreased Device->Host perf (Issue #893)". A Windows port must query the
> device's NUMA node (`IoGetDeviceNumaNode` / the PCI device's proximity
> domain) and allocate the contiguous regions on that node. This is a
> performance requirement, not merely a correctness one, but it is load-bearing
> enough to be documented in both layers.

> **Porting note (Wormhole channel-3 carveout):** For Wormhole B0 the 4th
> channel's device-visible (NOC/iATU) window must be **768MB**
> (`HUGEPAGE_CHANNEL_3_SIZE_LIMIT = 768 * (1 << 20)`,
> `silicon_sysmem_manager.hpp:20`), even though the full 1GB is allocated. If
> the Windows port takes over iATU programming from UMD (a TODO at
> `local_chip.cpp:387` says "KMD knows how to do this at page pinning time",
> and `wormhole_tt_device.cpp:152` flags the related channel-3 region hack), it
> must reproduce this truncation to avoid the region overlapping the PCIe
> register space.

> **Porting note (no mount / no hugetlbfs):** The `/dev/hugepages-1G` mount and
> the per-device backing files (`open_hugepage_file`, `hugepage.cpp:162-220`)
> are a Linux hugetlbfs artifact. Windows has no filesystem-backed hugepage
> namespace. The cross-process sharing that the file names provide (device 0
> channel 0 = `tenstorrent`, others = `device_<n>_channel_<c>_tenstorrent`) must
> be reimplemented with named shared sections or a driver-owned handle
> namespace if multi-process attach to the same physical buffer is required.
> The `mode=0777` mount and `0666` file mode (`hugepage.cpp:200`) mean the
> Linux design allows unprivileged processes to attach; a Windows port must
> pick an equivalent ACL policy on its shared objects.

> **Porting note (early-boot reservation):** The service runs
> `Before=sysinit.target` (`tenstorrent-hugepages.service:3`) precisely because
> 1GB contiguous pages usually can only be assembled before memory fragments.
> On Windows, large-contiguous allocations similarly get harder after boot; a
> port should reserve the device's host memory as early as practical (at driver
> load / device start, `EvtDevicePrepareHardware`) rather than lazily on first
> use, and surface a clear failure if the contiguous reservation cannot be
> satisfied (mirroring `hugepages-setup.sh:76-78`).

---

### Key constants table

| Name | Value | Source cite |
|------|-------|-------------|
| Hugepage size | 1 GiB (`1048576kB`) | `hugepages-setup.sh:67`; `dev-hugepages\x2d1G.mount:12`; `tt-umd/device/hugepage.hpp:17` |
| `HUGEPAGE_REGION_SIZE` | `1ULL << 30` (1GB) | `tt-umd/device/hugepage.hpp:17` |
| `MAX_HOST_MEM_CHANNELS` | 4 | `tt-umd/device/hugepage.hpp:19` |
| Default pages: Wormhole | 4 | `hugepages-setup.sh:54` |
| Default pages: Blackhole | 4 | `hugepages-setup.sh:53` |
| Default pages: Grayskull | 1 | `hugepages-setup.sh:55` |
| TT PCI vendor ID | `1e52` | `hugepages-setup.sh:11`; `tt-kmd/enumerate.h:15` |
| Grayskull PCI device ID | `faca` | `hugepages-setup.sh:12`; `tt-kmd/enumerate.h:16` |
| Wormhole PCI device ID | `401e` | `hugepages-setup.sh:13`; `tt-kmd/enumerate.h:17` |
| Blackhole PCI device ID | `b140` | `hugepages-setup.sh:14`; `tt-kmd/enumerate.h:18` |
| Hugepage mount point | `/dev/hugepages-1G` | `dev-hugepages\x2d1G.mount:10`; `tt-umd/device/hugepage.cpp:29` |
| sysfs count path | `/sys/.../hugepages/hugepages-1048576kB/nr_hugepages` | `hugepages-setup.sh:67-73`; `tt-umd/device/hugepage.cpp:32` |
| Mount options | `pagesize=1G,mode=0777,nosuid,nodev` | `dev-hugepages\x2d1G.mount:12` |
| Backing file mode | `0666` (`S_IWUSR..S_IROTH`) | `tt-umd/device/hugepage.cpp:200` |
| Base backing filename | `tenstorrent` (dev0/ch0); else `device_<n>_channel_<c>_tenstorrent` | `tt-umd/device/hugepage.cpp:164,173-183` |
| Wormhole ch-3 NOC window | 768 MB (`768 * (1<<20)`) | `tt-umd/device/api/.../silicon_sysmem_manager.hpp:20` |
| IOMMU carveout (WH, 4ch) | 256 MB (1GB − 768MB) | `tt-umd/device/chip_helpers/silicon_sysmem_manager.cpp:351` |
| Kernel param added | `iommu=pt` | `debian/tenstorrent-tools.postinst:4` |
| Override file | `/opt/tenstorrent/bin/hugepages-override.txt` (total pages) | `hugepages-setup.sh:35`; `README.md:24-25` |
| Pin size ceiling (kernel ≤5.4) | `1 << 30` (1GB) | `tt-kmd/memory.c:537-538` |
| Package name / version | `tenstorrent-tools` / `1.4.1` | `debian/changelog:1` |
| tt-oops version | `0.1.0` | `tt-oops/tt-oops.sh:10` |

---

### Open questions

1. **Blackhole channel count / carveout.** The setup script and UMD default
   Blackhole to 4×1GB, but the 768MB channel-3 carveout is guarded
   specifically by `ARCH::WORMHOLE_B0` (`silicon_sysmem_manager.cpp:160, 290,
   383`; `local_chip.cpp:396`). Whether Blackhole has an analogous
   device-address-map constraint on its 4th channel is not documented in these
   files — I did not find a Blackhole equivalent and cannot infer one.

2. **Override + NUMA interaction.** With `hugepages-override.txt`, every
   detected device gets `HP_COUNT = total/dev_count` pages regardless of arch
   (`hugepages-setup.sh:40, 44-48`). It is not stated what the intended
   behavior is when the override total is not evenly divisible by the device
   count (integer division drops the remainder, under-allocating). Treated here
   as a documented quirk, not a spec.

3. **Grayskull single page vs. 4 channels.** The comment at `hugepage.cpp:68`
   says "GS will use P2P + 1 channel"; Grayskull is EOL and largely absent from
   the newer UMD sysmem code. Whether a Windows port needs to support Grayskull
   host-memory at all is a scoping decision outside these files.

4. **Exact `iommu=pt` dependency.** The `postinst` adds `iommu=pt`
   unconditionally, but UMD supports both a translating-IOMMU path (anonymous
   mmap, `pin_or_map_iommu`) and the passthrough hugepage path. Which mode a
   given deployment actually runs in depends on platform/BIOS/IOMMU state that
   these repos do not pin down; the Windows DMA-remapping equivalent (§14.1
   porting note) needs a decision that this source does not make for us.

5. **hugetlbfs vs. anonymous MAP_HUGETLB.** Two distinct hugepage codepaths
   exist in UMD: the hugetlbfs-file path (`open_hugepage_file` +
   `find_hugepage_dir`) consumed here, and an anonymous `MAP_HUGETLB`
   fallback (`mmap_with_hugepage_fallback`, `silicon_sysmem_manager.cpp:45-109`)
   used on the IOMMU path. The system-tools service only provisions the former
   (hugetlbfs). Whether a Windows port needs to emulate both, or can collapse
   to a single contiguous-allocation primitive, is an architecture decision not
   settled by these files.

---

## Appendix A. Open questions raised by analysis

Consolidated from the per-section readers' raw feed (`docs/analysis/open-questions-raw.md`), deduplicated (three items were raised independently by two sections each and are merged below with dual attribution), and grouped by theme. Section attribution is preserved in brackets. Triage state lives separately in `docs/open-questions.md`.

### A.1 ABI and uAPI semantics

- **[03]** GET_HARVESTING (ioctl nr 1) has no handler and returns `-EINVAL` via fallthrough (chardev.c:631-632) — is the number reserved for legacy UMD probing, and must the Windows port reserve it?
- **[03]** No `compat_ioctl` means 32-bit userspace is unsupported on Linux; should the Windows port reject or thunk WOW64 callers (struct layouts appear width-invariant but were not exhaustively verified)?
- **[06]** Whether the exact-size ALLOCATE_TLB contract (no round-up, tlb.c:24) is frozen uAPI behavior to copy verbatim on Windows.
- **[06]** Semantics of `ordering` values other than 1 (documented only as "strict") are defined by NOC hardware / tt-umd enums, not by tt-kmd; treat as opaque 2-bit passthrough.
- **[12]** Exact errno values for PIN_PAGES bad-flags/bad-size/unmapped-range, UNPIN_PAGES bad-size, MAP_PEER_BAR same-device/different-chip (non-overrun path), and CONFIGURE_TLB misaligned/oversized-address rejections are not asserted by the tests (only failure is) — must be taken from driver sources.
- **[12]** VerifyNoOverlap in test/query_mappings.cpp:114-128 checks the unsorted mapping list (likely test bug); whether QUERY_MAPPINGS output is guaranteed base-sorted needs confirmation from the driver.
- **[12]** EXPORT_TLB_DMABUF with size==0 succeeds in tests and appears to mean "whole window", but the exported length is never asserted — confirm against driver implementation.
- **[12]** Whether 1024 in VerifyPinPagesMultipleRanges (test/pin_pages.cpp:183) reflects an actual driver limit on concurrent pinned ranges or is arbitrary.
- **[10]** Whether tt-smi/tt-flash parse exact formats (e.g. 16-hex-digit `tt_serial`, 4-part vs 3-part version strings) could not be confirmed from the provided repos (tt-umd and tt-system-tools contain no references to the tt_* sysfs names); treat formats as ABI regardless.
- **[10]** `tt_aiclk`/`tt_axiclk`/`tt_arcclk` units: driver prints the raw u32 with no conversion; conventionally MHz but neither documented nor enforced in the KMD (telemetry.c:35-47).

### A.2 Reset and device lifecycle

- **[11]** `pcie_retrain_link_to_max_speed` is declared in pcie.h:16 but never defined or called — presumed dead code, confirm before omitting.
- **[11]** `wormhole_complete_pcie_init` reads PCI_SUBSYSTEM_VENDOR_ID (0x2C) from the upstream bridge, which on a type-1 header is not a subsystem vendor ID field; the value is passed verbatim to FW and the intent is unverified — a bit-faithful port must read bridge config offset 0x2C anyway.
- **[11]** CONFIG_WRITE bumps reset_gen but does not set needs_hw_init, and RESET_PCIE_LINK zaps VMAs without bumping reset_gen — unclear whether a Windows port must reproduce this legacy asymmetry or only the flag 3-6 model (depends on whether legacy UMD flows are supported).
- **[11]** POST_RESET clears needs_hw_init unconditionally even when the marker check or re-init fails, leaving a possibly uninitialized device open for all ioctls — intentional retry contract or accepted gap?
- **[11]** mmap does not check needs_hw_init, so a post-reset-generation fd can map BARs during the reset window before POST_RESET re-init — undocumented asymmetry with the ioctl allowlist.
- **[11]** Config-space registers 0x930/0x934 (interface timer) have no in-tree documentation; the exact hardware/firmware behavior behind the reset trigger is unknown — port must reproduce the writes byte-for-byte.
- **[11]** Wormhole/Blackhole TRIGGER_RESET is fire-and-forget and the ioctl reports success without FW confirmation — should the Windows port preserve this optimistic reporting (recommended, since callers poll the marker) or add verification?
- **[11]** UMD legacy Blackhole flow polls command-register bit 1 (memory space enable) rising after CONFIG_WRITE; what sets that bit post-reset is not visible in these repos — only matters if legacy flows must work against the Windows KMD.
- **[11]** `reset_limit` and `auto_reset_timeout` are module-wide on Linux; deciding per-device vs global registry parameters on Windows changes multi-device behavior slightly.
- **[12]** Who sets/clears the PCI Command-register bit-6 reset-in-progress marker polled by tools/reset.c:241 (driver vs firmware), and which device generations use the in-place vs disappear/reappear reset path.
- **[03]** `tt_cdev_release_noc_cleanup` gates only on `detached`, not reset_gen/needs_hw_init: a stale pre-reset fd closing mid-reset still performs its NOC write into possibly-in-reset hardware (chardev.c:869) — intentional or oversight?
- **[06]** Hardware TLB register state after ASIC reset is unobservable in the driver: registers are never cleared/reprogrammed on reset or free, so a port must not assume a benign default.
- **[10]** PCIe perf counter reads during an in-place reset (needs_hw_init true, BARs mapped) are permitted and return whatever hardware yields; whether that is meaningful is unspecified.
- **[10]** hwmon channel visibility latches at registration time; a tag first published after a POST_RESET re-probe updates the cache but not hwmon visibility until re-registration — unclear if any tooling depends on late-appearing channels.
- **[02]** open() never checks `detached`; a create racing device removal succeeds and then fails all operations — the equivalent KMDF create-during-surprise-removal behavior needs an explicit decision.
- **[02]** remove iterates `open_fds_list` and runs per-fd memory cleanup without `chardev_mutex` while opens are still possible (cdev not yet deleted) (enumerate.c:465-467) — potential race the port must resolve explicitly.
- **[02]** `tenstorrent_register_device` / `cdev_device_add` return value is ignored by probe (enumerate.c:375) — failure leaves a device with sysfs but no char device and remove calls `cdev_device_del` on a never-added cdev; intentional?
- **[02]** kzalloc-failure path returns `-ENOMEM` without `pci_disable_device`, unlike the ordinal-failure path (enumerate.c:280-282 vs 295-299) — presumed leak/oversight.
- **[02]** chrdev region reserves only max_devices (32) minors but XArray ordinals can reach 2^31-1 with no clamp in `devt_for_device` — behavior with ordinal >= max_devices unverified (chardev.c:55, 85-88; enumerate.c:290-293).
- **[02]** Embedded `struct device` has `release=NULL` while memory is freed via a separate kref (chardev.c:103, enumerate.c:492) — relies on no outstanding kobject refs at kfree; runtime warning behavior unverified.
- **[02]** At remove/unload time `cleanup_hardware` is effectively skipped because both classes no-op when detached (already true) — firmware apparently stays in A0 after driver unload; intended?
- **[01]** `.shutdown` aliased to `.remove` runs the full teardown at system shutdown — which subset (quiesce, watchdog reprogram, power state) firmware actually requires across warm reboot is not documented here and needs correlation with the enumerate/wormhole shutdown analysis.

### A.3 Hardware and firmware behavior

- **[07]** BAR0 total size is never asserted; mapped offsets require >= 0x1FE00000 but what lies between 0x19400000 and 0x1FC00000 and above 0x1FE00000 is unknown from tt-kmd.
- **[07]** BAR2 is mapped in full but only the iATU block at 0x1000 is touched; the rest of the BAR2 register map is undocumented in the driver.
- **[07]** TLB fields `stream_header` and `static_vc` exist in the register layout but are never programmed; hardware semantics unknown.
- **[07]** How the DWC interface-timer interrupt (config 0x930/0x934) is converted into an ASIC reset is CMFW behavior invisible to the driver, as is the required post-reset settle time for flag-4 resets on Blackhole.
- **[07]** TRIGGER_RESET (0x56) payload values other than 3 (ASIC+M3) are undocumented in tt-kmd; source of truth is tt-zephyr-platforms.git.
- **[07]** NOC_ID register at 0x1FD04044: only low 6 bits (x) are consumed and y=0 is assumed; full register format unknown.
- **[07]** `telemetry_probe` validates base/data pointers with `is_range_within_csm(addr, 1)` rather than 4, allowing a base within 3 bytes of CSM end whose reads would exceed CSM — intentional slack or oversight?
- **[07] + [06]** Is the missing `bar2_mapping` NULL check in `blackhole_init` (blackhole.c:592) a bug or is BAR2 considered optional? Later iATU writes could deref NULL; presumed latent bug — the port must decide whether to treat BAR2 map failure as fatal (recommended: fatal).
- **[07]** Whether Galaxy Blackhole (subsystem 0x0047) needs personality-level differences beyond enumerate.c ordinal assignment is not visible in blackhole.c.
- **[07]** Whether any board-type (p100/p150/p300) tuning matters to the KMD should be confirmed against tt-umd; the kernel driver itself has no board branches.
- **[09]** Occupancy math `(wptr-rptr)%(2*num_entries)` in unsigned u32 is only correct across a pointer wrap when 2*num_entries divides 2^32, i.e. num_entries is a power of two; the driver reads num_entries from the QCB and never validates it. Is num_entries guaranteed by FW to be a power of two? If not, occupancy is miscomputed after wrap.
- **[09]** `send_arc_message` takes no lock spanning the push/doorbell/pop transaction; the power-setting path is serialized by `chardev_mutex` while reset/init/cleanup are serialized by `reset_rwsem` (a different lock). Can a POWER_SETTING transaction interleave with a reset/init ARC transaction on the same device, breaking the single-outstanding-message assumption?
- **[09]** Only bits 0-7 of `queue_info` (num_entries) are used; the meaning of the upper 24 bits (entry size? version? flags?) is unknown, and the driver hardcodes a 32-byte entry stride.
- **[09]** Both wrappers treat response header==0 as success and any non-zero as failure, but the semantics of a non-zero response header (error code vs echoed opcode vs status bitfield) are not documented in these files, so callers cannot distinguish error causes.
- **[09]** msgqueue.c relies solely on MMIO accessor ordering with no explicit barriers; the exact barrier placement a Windows KMDF port needs on non-x86/weakly-ordered targets (entry-writes before WPTR publish, WPTR publish before doorbell, RES_WPTR read before entry reads) is unverified.
- **[09]** Doorbell semantics are inferred from comments only: whether the BH ARC_MSI_FIFO write and WH ARC_MISC_CNTL IRQ0 bit must strictly follow the WPTR store, and whether the WH IRQ0 bit auto-clears, is not confirmed against hardware documentation.
- **[06]** Wormhole silently ignores `tenstorrent_noc_tlb_config.static_vc` (no register field) while Blackhole maps it to `use_static_vc`; unclear if WH hardware has an equivalent bit intentionally left unexposed.
- **[06]** On BH with a small BAR4, `describe_tlb` still describes unallocatable 4G ids (uses compile-time total 210 while the allocator uses per-device counts); harmless today but an invariant to preserve when restructuring.
- **[10]** Firmware value refresh cadence is invisible to the KMD: tag 5 UPDATE_TELEM_SPEED exists in the tag space (tt-umd telemetry.hpp:16) but is never read; the rate at which FW updates CSM values or increments TIMER_HEARTBEAT must come from firmware docs before adding any Windows-side caching.
- **[10]** BH vs WH probe-time validation asymmetry: BH caches unvalidated tag addresses (read later fails `-EINVAL`), WH skips invalid entries at probe (attribute hidden); which behavior a port should normalize to is a policy choice with user-visible differences for malformed FW tables (blackhole.c:487-495 vs wormhole.c:574-577).
- **[10] + [02]** Stale `CONFIG_HWMON=n` stub: enumerate.c:78-82 stubs `devm_hwmon_device_register_with_info` but the driver calls the non-devm variant (wormhole.c:628 / blackhole.c:664) — apparent dead/broken code, not behavior to emulate; hwmon-less build status unclear.
- **[12]** Whether Blackhole lacks hwmon `*_max` attributes or merely has unreliable values (max comparison skipped for BH in test/hwmon.cpp:89) — affects Windows telemetry surface design.
- **[02]** Stub IRQ handler returns IRQ_HANDLED unconditionally under IRQF_SHARED — on a shared INTx line it claims other devices' interrupts; unclear if future interrupt sources require status-register gating.
- **[02]** `galaxy_bdf_to_ordinal` ignores the PCI domain/segment (enumerate.c:51) — multi-segment Galaxy hosts could collide on fixed ordinals; is single-domain a real platform invariant?
- **[14]** Whether Blackhole has a channel-3 (or any 4th-channel) device-address-map carveout analogous to Wormhole's 768MB limit; the carveout is guarded specifically by ARCH::WORMHOLE_B0 and no Blackhole equivalent appears in these files.

### A.4 Packaging, tooling, and deployment

- **[01]** tools/current-version parses module.h while VERSION_UPDATE.md claims it extracts from dkms.conf — which file is authoritative when they diverge; the port needs an explicit single source of truth.
- **[01]** Is runtime mutability of `idle_power_down_grace_ms` (sysfs 0644) a hard requirement, or is start-time registry configuration sufficient? No in-tree writer was found.
- **[01]** No consumer of `/dev/tenstorrent/by-id/*` symlinks was found in tt-umd — unknown whether external tooling (tt-smi etc.) relies on ASIC-ID-stable names, which decides if the Windows port needs an equivalent.
- **[01]** modprobe.d-tenstorrent.conf documents only 4 of 6 parameters (missing power_policy, idle_power_down_grace_ms) and is excluded from release tarballs — intentional (internal-only params) or stale?
- **[01]** >32-device systems require raising max_devices at load; unknown whether any deployed configuration does this, i.e., whether the Windows port needs the knob at all.
- **[12]** True minimum supported Linux kernel is ambiguous (nix overlay says <6.10 broken, mass-build-test targets v4.18+) — only relevant as a hint about which fallback code paths exist in the driver.
- **[14]** Intended behavior of hugepages-override.txt when the total is not evenly divisible by device count (integer division silently drops the remainder and under-allocates).

### A.5 Windows port design decisions

- **[02]** WH probe-time PCIe retrain loop depends on driver-controlled secondary bus reset with custom short delays (pcie.c:61-131), which a Windows function driver cannot normally perform — needs PLDR/ACPI strategy decision.
- **[06]** BAR mmap (RESOURCE0/2) exposes all TLB data windows and the TLB config register file without ownership checks, and tt-umd depends on it for userspace register writes; a Windows port must decide whether to reproduce this open-BAR model or force configuration through the ioctl.
- **[03] + [12]** O_APPEND as the power-aware signal has no clean Windows CreateFile equivalent (tt-umd notes O_APPEND "temporarily disabled to investigate NOC1 issues" at one call site, tt-umd/device/pcie/pci_device.cpp:379) — the replacement signal (create option, ECP, or ioctl handshake) must be coordinated with the UMD Windows port.
- **[12]** How Windows should express the O_EXCL/O_NONBLOCK exclusive-open contract and the blocking-open semantics (pended cancelable create IRP vs sharing-mode mapping) — Linux open-flag semantics have no direct CreateFile equivalent.
- **[01]** udev MODE 0666 makes device nodes world-writable including reset and pin-pages ioctls — should the Windows SDDL replicate this permissive model or tighten it (UMD assumes unprivileged open works)?
- **[01]** Grayskull INF policy: Linux claims PCI DEV 0xFACA but probe returns -ENODEV; should the Windows INF bind-and-fail-start (mirroring Linux, blocking other drivers) or omit Grayskull entirely?
- **[03]** NOC cleanup coordinate bounds x,y <= 64 are marked TODO "more robust validation" (chardev.c:467-469); may tighten upstream.
- **[14]** Whether a Windows port must support Grayskull host memory at all (GS is "P2P + 1 channel", EOL, and largely absent from newer UMD sysmem code).
- **[14]** Which IOMMU mode a given deployment actually runs in (passthrough hugepage path vs translating anonymous-mmap path) is not pinned down by these repos; the Windows DMA-remapping equivalent decision is not made for us.
- **[14]** Whether the Windows port must emulate both UMD hugepage codepaths (hugetlbfs-file path provisioned by the service, and anonymous MAP_HUGETLB fallback used on the IOMMU path) or can collapse to a single contiguous-allocation primitive.

---

## Appendix B. Unverified claims

Every claim marked **[UNVERIFIED]** in the body is indexed here. The fact-check pass found that all such markers occur in **section 04 (IOCTL Catalog)** and all concern the same class of claim: *which tt-umd source line issues a given ioctl*. In each case the ioctl usage itself is not in doubt for the ioctls tt-umd does issue (section 13 re-derives the real call sites through the `tt_device_t` / `tt_kmd_lib` abstraction); what failed verification is the specific `pci_device.cpp` line-number citation. No other section contains [UNVERIFIED] markers.

| # | Location (section file) | Claim | Why unverified / correction |
|---|---|---|---|
| 1 | 04, ioctl 5 (04-ioctl-catalog.md:302) | tt-umd gates features on GET_DRIVER_INFO (pci_device.cpp:158) | GET_DRIVER_INFO is not called in pci_device.cpp; version is fetched via `tt_driver_get_attr(TT_DRIVER_API_VERSION)` (pci_device.cpp:430) through `tt_device_t`; the `:158` citation is a `}` |
| 2 | 04, ioctl 6 (04-ioctl-catalog.md:346) | RESET_DEVICE used by tt-umd at pci_device.cpp:545 | Line 545 is blank; reset is issued via `send_reset_ioctl`/`tt_device_reset` (pci_device.cpp:200-215, called at 905) |
| 3 | 04, ioctl 11 (04-ioctl-catalog.md:505) | ALLOCATE_TLB used by tt-umd at pci_device.cpp:417 | Line 417 is unrelated; allocation goes through `PCIDevice::allocate_tlb` (pci_device.cpp:820) / `SiliconTlbHandle`; the raw ioctl does not appear in pci_device.cpp |
| 4 | 04, ioctl 12 (04-ioctl-catalog.md:517) | FREE_TLB used by tt-umd at pci_device.cpp:432, 455 | Neither line contains a FREE_TLB call; `TENSTORRENT_IOCTL_FREE_TLB` does not appear in the tt-umd sources checked out here (freeing handled inside the `TlbHandle`/`tt_device_t` abstraction) |
| 5 | 04, ioctl 13 (04-ioctl-catalog.md:550) | CONFIGURE_TLB used by tt-umd at pci_device.cpp:492, 511 | Those lines are unrelated; configuration goes through `PCIDevice::configure_tlb` (pci_device.cpp:845); the raw ioctl does not appear in pci_device.cpp |
| 6 | 04, ioctl 14 (04-ioctl-catalog.md:575) | SET_NOC_CLEANUP call sites exist in tt-umd (pci_device.cpp:309 area) | `TENSTORRENT_IOCTL_SET_NOC_CLEANUP` does not appear anywhere in the tt-umd sources checked out here; the call-site claim is unsupported |
| 7 | 04, ioctl 15 (04-ioctl-catalog.md:602) | SET_POWER_STATE used by tt-umd at pci_device.cpp:525 (version-gated, introduced in KMD 2.6.0) | The 2.6.0 gating is backed by kmd_versions.hpp:39, but `:525` is a blank line; power state is set via `PCIDevice::set_power_state`/`tt_device_set_power_state` (pci_device.cpp:1056, 1071) |

(Section 04's "tt-umd usage summary" additionally carries an **[UNVERIFIED line map]** marker covering the same seven citations in aggregate; section 13 §2.1 contains the verified call-site table and should be treated as authoritative for tt-umd usage.)

No further unverified claims are tracked beyond the items above.
