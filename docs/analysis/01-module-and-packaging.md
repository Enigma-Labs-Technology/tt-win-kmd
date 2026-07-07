# 01. Module and Packaging

## Scope

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
| .github/workflows/ (skim, CI only) | release.yml 302; test.yml, build-debian.yml, build-rpm.yml, mass-build-test.yml, hardware-test.yml, check-padding.yml, checkws.yml |

Supporting cross-references (read only the parts where module parameters / names defined here are consumed): chardev.c, enumerate.c, enumerate.h, pcie.c, wormhole.c, blackhole.c, telemetry.c, device.h, ioctl.h, tools/build_debs.sh, tools/build_rpms.sh, tools/exclude-from-release.

---

## 1. Module identity and minimum kernel

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

## 2. Module init/exit sequence and registration order

### Init (`ttdriver_init`, module.c:76-108), in exact order:

1. `pr_info` banner with version string (module.c:80).
2. `tt_debugfs_root = debugfs_create_dir("tenstorrent", NULL)` (module.c:82). **Not error-checked** — debugfs is optional; failure is tolerated (per-device code guards with `if (tt_debugfs_root)`, chardev.c:109-110).
3. `tt_procfs_root = proc_mkdir("driver/tenstorrent", NULL)`; on NULL → `err = -ENOMEM`, goto unwind (module.c:84-88). procfs presence IS mandatory for load.
4. `init_char_driver(max_devices)` (module.c:90-92) — allocates a char major with `max_devices` minors via `alloc_chrdev_region(&tt_device_id, 0, max_devices, TENSTORRENT)` and creates the device class `class_create(TENSTORRENT)` where `TENSTORRENT` is the string `"tenstorrent"` (chardev.c:48-75, enumerate.h:13). Error unwind inside: class failure unregisters the chrdev region (chardev.c:71-74).
5. `tenstorrent_pci_register_driver()` → `pci_register_driver(&tenstorrent_pci_driver)` (module.c:94-96, enumerate.c:537-540). **Registration order matters**: the char-device infrastructure (major number + class) must exist before PCI probe can run, because probe → `tenstorrent_register_device()` creates the per-device cdev immediately (enumerate.c:375, chardev.c:90-119).

Failure unwind (module.c:100-107): reverse order — unregister nothing past the failed step; `cleanup_char_driver()`, `proc_remove()`, `debugfs_remove()`. Returns the error to modprobe (`-ENOMEM` for procfs, whatever `init_char_driver`/`pci_register_driver` returned otherwise).

### Exit (`ttdriver_cleanup`, module.c:110-118), in exact order:

1. `tenstorrent_pci_unregister_driver()` — triggers `.remove` for every bound device first.
2. `cleanup_char_driver()` — destroys class and releases the chrdev region (chardev.c:77-83).
3. `debugfs_remove(tt_debugfs_root)`.
4. `proc_remove(tt_procfs_root)`.

Note the exit order of debugfs/procfs is not the exact mirror of init (harmless in Linux since both are empty by then — all per-device entries were removed during `.remove`).

> **Porting note:** In KMDF the equivalent of steps 4–5 collapses into `DriverEntry`/`WdfDriverCreate` + `EvtDeviceAdd`; there is no global "char driver" to pre-register. What must be preserved is the *invariant*, not the mechanism: per-device user-visible interfaces (device interface / symbolic link, ~= `/dev/tenstorrent/N`) may only appear after per-device init succeeds, and must be torn down before the device object goes away. The `max_devices` pre-allocation (32 minors) has no Windows analogue — device interfaces are unbounded — but note anything in userspace that assumes ordinals < 32.

### Global namespaces created at init

- debugfs: `/sys/kernel/debug/tenstorrent/` (module.c:82), per-device numeric subdirs plus a `mappings` file (chardev.c:109-110, enumerate.c:385).
- procfs: `/proc/driver/tenstorrent/` (module.c:84), per-device numeric subdirs each containing a read-only `pids` file listing PIDs with open fds (chardev.c:111-113, enumerate.c:230-241).

> **Porting note:** debugfs/procfs are diagnostics only; on Windows these map naturally to either an IOCTL-based query or WPP/ETW + a WMI provider. The `pids` file (which processes hold the device open) is used operationally by tt-system-tools style tooling; decide early whether the port exposes an equivalent query IOCTL.

## 3. Module parameters — complete list

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

### 3.1 `max_devices` — "Maximum number of tenstorrent devices (chips) to support."
Passed once to `init_char_driver()` (module.c:90); sizes the char minor-number region: `alloc_chrdev_region(&tt_device_id, 0, max_devices, TENSTORRENT)` (chardev.c:55) and is remembered for cleanup (chardev.c:52, 82). A device whose ordinal ≥ max_devices would get a minor outside the allocated region; ordinal allocation itself (xarray, Galaxy fixed slots) is in enumerate.c (see section 02). No runtime clamping is performed in module.c.

### 3.2 `dma_address_bits` — "DMA address bits, 0 for automatic."
Consumed in probe (enumerate.c:322-331):
```c
tt_dev->dma_capable = (dma_set_mask(&dev->dev, DMA_BIT_MASK(dma_address_bits ?: 64)) == 0);
dma_set_coherent_mask(&dev->dev, DMA_BIT_MASK(dma_address_bits ?: device_class->dma_address_bits));
```
Semantics when 0 (default): streaming DMA mask = 64-bit; coherent DMA mask = the device class default — **32 for Wormhole** (`.dma_address_bits = 32`, wormhole.c:1058) and **58 for Blackhole** (`.dma_address_bits = 58`, blackhole.c:816; field defined at device.h:90). When nonzero, the same value overrides *both* masks. The comment explains why they differ: legacy Wormhole software assumes 32-bit addresses from ALLOCATE_DMA_BUF, but a 32-bit streaming mask would cripple user pinnings under IOMMU (enumerate.c:322-329). Probe also maxes out segment size/boundary: `dma_set_max_seg_size(&dev->dev, UINT_MAX); dma_set_seg_boundary(&dev->dev, ULONG_MAX);` (enumerate.c:334-335).

> **Porting note:** This is the single most consequential parameter for a Windows DMA design. The 32-bit coherent constraint for Wormhole common buffers (DMA-buf allocations) must be honored — on Windows that means a DMA enabler / common-buffer allocation constrained below 4 GiB for Wormhole, while user pinnings may use full 64-bit logical addresses. Blackhole's 58-bit limit reflects its NOC iATU carve-out (`noc_dma_limit = (1ULL << 58) - 1`, `noc_pcie_offset = (4ULL << 58)`, blackhole.c:817-818).

### 3.3 `reset_limit` — "Maximum number of times to reset device during boot."
Consumed only in `wormhole_complete_pcie_init()` (pcie.c:92-131). If the device has no upstream bridge or `reset_limit == 0`, the retrain loop is skipped entirely and treated as success (`return true`, pcie.c:98-99). Otherwise up to `reset_limit` iterations of: read bridge `PCI_EXP_LNKCTL2` target link speed and subsystem vendor ID, send ARC message `FW_MSG_PCIE_RETRAIN` with `target_link_speed | (last_retry << 15)` and a 200000 µs timeout (pcie.c:101-114); exit code 0 → success; otherwise `pci_save_state` + secondary-bus hot reset + restore, and retry (pcie.c:125-127). Failure of the message send or of the final retry → `false`.

### 3.4 `auto_reset_timeout` — "Timeout duration in seconds for M3 auto reset to occur."
Type is `byte` (`unsigned char`), so its range is 0–255 seconds. Three consumers:
1. **Wormhole FW watchdog programming** at hardware init: `WH_FW_MSG_UPDATE_M3_AUTO_RESET_TIMEOUT, auto_reset_timeout` (seconds, passed raw) with 10000 ms timeout (wormhole.c:729-731).
2. **Blackhole FW watchdog programming** at hardware init: `msg.header = ARC_MSG_TYPE_SET_WDT_TIMEOUT; msg.payload[0] = 1000 * auto_reset_timeout;` — converted to **milliseconds** (blackhole.c:632-636); failure is a `dev_warn` only ("normal for old FW").
3. **Host-side reset wait** in `wormhole_reset()` (wormhole.c:484-505): if the chip does not answer a NOP ARC message, and `auto_reset_timeout == 0`, the reset **fails immediately** with "Watchdog is disabled and device is unresponsive, cannot reset." (wormhole.c:488-491). Otherwise the driver polls hot-reset+NOP in a loop until deadline `ktime_add_ms(ktime_get(), (auto_reset_timeout * 1000) + 500)` (wormhole.c:493), sleeping 1 s between attempts (interruptible; a signal aborts with `false`, wormhole.c:502-503).

So the value 0 means "M3/DMC watchdog disabled" and simultaneously disables the driver's own wait-for-watchdog fallback.

### 3.5 `power_policy` — "Enable power policy: low power at probe, re-aggregate on close (default=on)."
Two consumers: at the end of probe, `if (power_policy) tenstorrent_set_aggregated_power_state(tt_dev);` puts the freshly-probed device into low power (enumerate.c:387-389); at fd release, `if (!power_policy)` skips the close-time aggregation entirely (chardev.c:910). The user-visible contract is documented in ioctl.h:380-383 (SET_POWER_STATE semantics interact with this parameter).

### 3.6 `idle_power_down_grace_ms` — delayed idle power-down
Full description: "Delay in ms between the last fd closing a device and the idle power-down message being sent. 0 sends the message synchronously at close. Only honored by device classes that opt in via defer_idle_powerdown." (module.c:58-62). Consumed at release: `can_defer = tt_dev->dev_class->defer_idle_powerdown && (idle_power_down_grace_ms > 0);` (chardev.c:903) and, when deferring, `mod_delayed_work(system_wq, &tt_dev->power_down_work, msecs_to_jiffies(idle_power_down_grace_ms));` (chardev.c:917). Opt-in flag: `defer_idle_powerdown` (device.h:122) is set **only for Wormhole** (`.defer_idle_powerdown = true`, wormhole.c:1083); the Blackhole class initializer omits it (blackhole.c:813-840), so on Blackhole the message goes out synchronously at close regardless of this parameter. The delayed work is cancelled in suspend and remove (device.h:60-62; enumerate.c:503).

Because this parameter is `0644`, root can retune it at runtime through `/sys/module/tenstorrent/parameters/idle_power_down_grace_ms`; the value is read fresh at each fd release, so changes take effect immediately.

> **Porting note:** The natural KMDF mapping for all six parameters is registry values under the service/device `Parameters` key, read at `DriverEntry`/`EvtDeviceAdd`. `idle_power_down_grace_ms` is the only one Linux allows to change at runtime; if that capability is preserved, the Windows driver must re-read the registry (or accept a control IOCTL) rather than caching at start. The modprobe.d sample file (modprobe.d-tenstorrent.conf:1-14) shows the parameters Tenstorrent expects administrators to tune: it documents only `max_devices`, `dma_address_bits`, `reset_limit`, `auto_reset_timeout` — all commented out, i.e., defaults everywhere. An INF should likewise ship defaults and document overrides, not hard-set values.

## 4. PCI device ID table and driver structure

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
- PM ops are `SIMPLE_DEV_PM_OPS(tenstorrent_pm_ops, tenstorrent_suspend, tenstorrent_resume)` (enumerate.c:524): suspend cancels the deferred power-down work, revokes TLB dmabufs, and calls `cleanup_hardware`; resume re-runs `init_hardware` and re-saves PCI config state, returning `-EIO` on failure (enumerate.c:498-522).
- Probe fixes up unflashed boards that enumerate with no PCI class code: `dev->class = 0x120000; /* Processing Accelerator - vendor-specific interface */` followed by `pci_assign_unassigned_bus_resources(dev->bus)` (enumerate.c:270-275).
- Probe suppresses hotplug on Galaxy chassis, keyed by PCI subsystem device ID `0x0035` (Galaxy Wormhole) / `0x0047` (Galaxy Blackhole) (enumerate.c:42-43, 355-357).

> **Porting note:** The INF Models section should list `PCI\VEN_1E52&DEV_401E` and `PCI\VEN_1E52&DEV_B140`. Whether to also claim `DEV_FACA` (Grayskull) needs a decision: Linux claims-then-rejects (-ENODEV), which on Windows would look like a device that always fails to start — usually worse UX than not claiming it. `.shutdown == .remove` means the Windows driver should run its full quiesce path (power aggregation message, watchdog handling) from the shutdown path (`EvtDeviceD0Exit` with system-shutdown awareness / `EvtDeviceShutdown` for FDOs), not just on removal. The class-code fixup and `pci_assign_unassigned_bus_resources` have no user-mode-visible analogue and are a manufacturing-flow convenience; on Windows an unflashed board with class 0x000000 may not even get resources assigned by PnP — treat as out of scope unless manufacturing on Windows is required.

## 5. Firmware loading

There are **no** `MODULE_FIRMWARE` declarations and no `request_firmware()` calls anywhere in the driver (verified by grep across all .c/.h). The driver never loads firmware files from disk; it communicates with ARC/M3/DMC firmware that is already resident on the board (flashed), via message queues and register mailboxes. Consequently the Linux package ships no firmware payloads, and a Windows driver package needs none either.

## 6. Version string handling

Three distinct version identities exist and must not be conflated:

1. **Driver code version** — module.h macros (`2` / `10` / `1` / `"-pre"`, module.h:19-22), stringified for the load banner and `MODULE_VERSION` (module.c:23-31, 80). Reported to userspace *numerically* by `TENSTORRENT_IOCTL_GET_DRIVER_INFO`: `out.driver_version_major/minor/patch = TENSTORRENT_DRIVER_VERSION_{MAJOR,MINOR,PATCH}` (chardev.c:176-179). **The suffix ("-pre"/"-rc1") is not reported through the ioctl.**
2. **ioctl ABI version** — `#define TENSTORRENT_DRIVER_VERSION 2` (ioctl.h:10), returned as `out.driver_version` (chardev.c:176). This is the compatibility number UMD checks; it changes only on ABI breaks, independent of the package version.
3. **Package version** — `PACKAGE_VERSION="2.10.1-pre"` in dkms.conf:2 and `modver=2.10.1-pre` in AKMBUILD:2. VERSION_UPDATE.md names dkms.conf the "Primary Source of Truth" and lists AKMBUILD, debian/changelog, module.h, and ioctl.h as needing manual sync (VERSION_UPDATE.md:5-12).

Automation: `tools/current-version` prints `MAJOR.MINOR.PATCH SUFFIX` **parsed from module.h** (tools/current-version:18-28) — despite VERSION_UPDATE.md:16 claiming it extracts from dkms.conf. The release workflow rewrites dkms.conf, AKMBUILD, and module.h together from a single user-supplied semver string (release.yml:88-118), tags releases `ttkmd-<version>` (release.yml:176-179), and then bumps to the next `-pre` patch version for development (release.yml:241-267) — which is why the working tree reads `2.10.1-pre` one commit after tag `ttkmd-2.10.0-rc1`. Debian/RPM packaging converts `-` to `~` for correct prerelease ordering (build-debian.yml step "Determine version": `DEB_VERSION="${VERSION//-/\~}"`).

> **Porting note:** For Windows: the ioctl ABI number (`2`) and the numeric major/minor/patch must round-trip through the ported GET_DRIVER_INFO exactly, since tt-umd gates features on them. The package version maps to INF `DriverVer` (which cannot carry a `-pre` suffix — DriverVer is `mm/dd/yyyy,w.x.y.z`; encode prerelease-ness in the 4th numeric field or drop it). Keep a single source of truth and generate both the INF and the driver's version header from it, mirroring release.yml's atomic multi-file update.

## 7. Device naming and udev rules

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

## 8. DKMS / AKMS / package behavior

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
- **modprobe.d-tenstorrent.conf** is a documentation-only sample (every `options` line commented out, modprobe.d-tenstorrent.conf:1-14) and is *excluded* from source release tarballs (tools/exclude-from-release:4). It lists only 4 of the 6 parameters.
- NixOS support exists via a flake overlay; the udev rules must be added to `services.udev.packages` separately from the module (README.md:38-56) — reinforcing that the rules file is a required, separately-installed companion to the module.

> **Porting note:** The DKMS design translates to a signed driver package (INF + SYS + CAT) installed by pnputil/DPInst-equivalent; there is no rebuild-per-kernel concern on Windows. The three install-time side effects the Windows installer must replicate: (1) device security/permissions (udev MODE 0666 → SDDL in INF or `EvtDeviceAdd`), (2) stable-name links (by-id → device properties/interface strings), (3) immediate driver load on install without reboot (modprobe in postinst → INF-based install triggers PnP start automatically for present devices).

## 9. CI (skim)

- `test.yml`: builds via nix, dkms, and plain `make` (with sparse `make C=2`) on ubuntu-22.04/24.04; builds test suite; builds the .deb and verifies install: dkms status, module present under `/lib/modules/$(uname -r)/{updates,extra}/tenstorrent.ko[.zst]`, `modprobe tenstorrent`, `depmod -a` (test.yml, incl. lines 56-120).
- `mass-build-test.yml`: compiles the module against every mainline kernel from v5.4 to v7.1 using the gregkh linux mirror (`test/mass-build-test . /tmp/linux v5.4 v7.1`).
- `hardware-test.yml`: runs `test/run-hardware-tests.sh` on real runners: n150, n300, n300-llmbox, p150b — each in normal and `viommu` (virtualized IOMMU) variants (hardware-test.yml matrix). This is the supported-configuration matrix: Wormhole (n150/n300) and Blackhole (p150b), with and without IOMMU.
- `check-padding.yml`: runs pahole (`test/pahole_check.sh`) to reject **implicit padding in ioctl.h structs** — the ioctl ABI is deliberately padding-free/fixed-layout. A Windows port sharing ioctl.h-derived structures must preserve exact layout (no compiler-inserted padding).
- `checkws.yml`: whitespace lint. `release.yml`: covered in section 6.
- `build-rpm.yml`/`build-debian.yml`: package builds via `tools/build_rpms.sh`/`tools/build_debs.sh` with `-`→`~` version mangling.

## Key constants table

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

## Open questions

1. **Grayskull INF policy**: Linux claims `DEV_FACA` in the match table but probe returns `-ENODEV` (module.c:65-66, enumerate.c:261-264). Should the Windows INF bind (and fail start, mirroring Linux and blocking other drivers) or not list Grayskull at all? Linux behavior is claim-and-reject; a direct mirror produces a permanently code-10-style device on Windows.
2. **`tools/current-version` vs VERSION_UPDATE.md discrepancy**: the doc says the script extracts the version from dkms.conf (VERSION_UPDATE.md:15-16), but the script parses module.h (tools/current-version:11-28). Which file wins if they diverge is therefore ambiguous in the upstream process; the port's single-source-of-truth should be defined explicitly.
3. **Runtime mutability of `idle_power_down_grace_ms`** (0644, module.c:57): is runtime tunability a hard requirement for the Windows port, or is boot/start-time configuration sufficient? No in-tree tooling was found that writes it.
4. **Security model of MODE 0666** (udev-50-tenstorrent.rules:1): world-writable device nodes expose reset/pin-pages ioctls to any local user. Is replicating this on Windows (permissive SDDL) acceptable, or should the port tighten access and require UMD to run with membership in a group?
5. **by-id symlink consumers**: no consumer found in tt-umd (grep for "by-id" empty). Unknown whether external tooling (tt-smi, orchestration) relies on `/dev/tenstorrent/by-id/*`; determines whether the Windows port needs an ASIC-ID-stable naming feature at all.
6. **modprobe.d sample staleness**: modprobe.d-tenstorrent.conf documents only 4 of 6 parameters (missing `power_policy`, `idle_power_down_grace_ms`) and is excluded from release tarballs (tools/exclude-from-release:4) — unclear if intentional (params considered internal) or stale documentation.
7. **`max_devices` > 32 systems**: Galaxy fixed-ordinal assignment plus the static 32-minor region means a >32-chip system needs `max_devices` raised at load; whether any deployed configuration does this (and thus whether the Windows port needs an equivalent knob) is unknown from the source alone.
8. **`.shutdown = .remove` scope**: running the full remove path (including chardev teardown and power messages) at shutdown is a Linux implementation convenience; exactly which subset (quiesce? watchdog reprogram? power state?) is *required* by firmware across a warm reboot is not documented in these files — needs correlation with sections covering enumerate.c/wormhole.c shutdown behavior before deciding what the Windows shutdown path must do.
