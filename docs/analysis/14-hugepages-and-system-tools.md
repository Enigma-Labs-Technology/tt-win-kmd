# 14. Hugepages and System Tools

## Scope

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
- `tt-system-tools/tt-oops/tt-oops.sh` (1293 lines)
- `tt-system-tools/tt-oops/DESIGN.md` (225 lines)
- `tt-system-tools/tt-oops/Makefile` (70 lines)
- `tt-system-tools/README.md` (29 lines)
- `tt-system-tools/tenstorrent-tools.spec` (79 lines)
- `tt-system-tools/debian/control` (16 lines), `debian/rules` (13 lines),
  `debian/changelog` (72 lines), `debian/tenstorrent-tools.install` (6 lines),
  `debian/tenstorrent-tools.postinst` (38 lines),
  `debian/tenstorrent-tools.lintian-overrides` (4 lines)
- `tt-system-tools/dev-scripts/build-rpm.sh` (20 lines)
- `tt-system-tools/.gitlab-ci.yml` (22 lines)

Cross-referenced (consumers of hugepages; not the primary subject of this
section):

- `tt-umd/device/hugepage.hpp` / `hugepage.cpp`
- `tt-umd/device/chip_helpers/silicon_sysmem_manager.cpp`
- `tt-umd/device/api/umd/device/chip_helpers/silicon_sysmem_manager.hpp`
- `tt-umd/device/chip/local_chip.cpp`
- `tt-kmd/memory.c` (pin-pages path), `tt-kmd/enumerate.h` (PCI IDs)

---

## 14.1 What the hugepages service configures

### Page size: always 1GB

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
(`dev-hugepages\x2d1G.mount:11`). The `tt-umd` side agrees:
`HUGEPAGE_REGION_SIZE = 1ULL << 30; // 1GB` (`tt-umd/device/hugepage.hpp:17`),
and the comment right above it is load-bearing: *"Hugepages must be 1GB in
size"* (`hugepage.hpp:15-16`).

### Count per device (default policy)

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

### Override mechanism

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

### NUMA placement

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

### Error handling / verification

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

### Mount point

The reserved pages are exposed to userspace as a hugetlbfs mount at
`/dev/hugepages-1G`:

```
What=hugetlbfs
Where=/dev/hugepages-1G
Type=hugetlbfs
Options=pagesize=1G,mode=0777,nosuid,nodev
```
(`dev-hugepages\x2d1G.mount:9-11`). `mode=0777` makes the mount world
writable so any (non-root) process can create backing files there.
`ConditionPathExists=/sys/kernel/mm/hugepages/hugepages-1048576kB` guards the
mount so it only runs on kernels that expose 1GB hugepages, and
`ConditionCapability=CAP_SYS_ADMIN` guards for the mount privilege
(`dev-hugepages\x2d1G.mount:5-6`).

### systemd ordering and lifecycle

- `tenstorrent-hugepages.service` is a `Type=oneshot` unit run as
  `User=root`, ordered `Before=sysinit.target` with
  `DefaultDependencies=no`, i.e. it runs very early in boot before most of
  the system is up (so 1GB pages can still be assembled)
  (`tenstorrent-hugepages.service:2-8, 15`). `SuccessExitStatus=0`,
  `Restart=no`, `TimeoutStopSec=10s` (`:11-13`).
- The `.mount` unit is likewise `Before=sysinit.target`,
  `DefaultDependencies=no`, `WantedBy=sysinit.target`
  (`dev-hugepages\x2d1G.mount:3-4,14-15`).
- The service reserves the pages (writes `nr_hugepages`); the mount unit
  exposes them at `/dev/hugepages-1G`. They are independent units — the
  service does not itself mount, and the mount does not itself reserve.

### Kernel command-line / IOMMU expectation

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

## 14.2 How userspace (tt-umd) consumes the hugepages

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
   (`hugepage.cpp:31-48`) — the same 1GB sysfs knob the setup script wrote.
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

### The Wormhole 4th-channel carveout (768MB)

There is a hardware-driven quirk: on Wormhole B0, channel 3's mapping to the
NOC is limited to **768MB**, not the full 1GB, to avoid overlapping the PCIe
register space in the device address map:

```cpp
static constexpr size_t HUGEPAGE_CHANNEL_3_SIZE_LIMIT = 768 * (1 << 20);
```
(`tt-umd/device/api/umd/device/chip_helpers/silicon_sysmem_manager.hpp:20`).
It appears anywhere a per-channel NOC mapping size is computed:
`silicon_sysmem_manager.cpp:160-162, 290-292, 383-385` and
`local_chip.cpp:396-401`. The full 1GB is still mmapped and pinned; only the
NOC-visible window of channel 3 is truncated. The IOMMU (no-hugepage) path
mirrors the same carveout: `carveout_size = HUGEPAGE_REGION_SIZE -
HUGEPAGE_CHANNEL_3_SIZE_LIMIT; // 1GB - 768MB = 256MB`
(`silicon_sysmem_manager.cpp:351`).

### Kernel-side size ceiling

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

## 14.3 Host-memory expectations summarized

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

## 14.4 tt-oops (diagnostic collector) and its interaction with the driver

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
(`:701-704`). This mirrors what UMD prints on a hugepage-mmap failure
(`/proc/cmdline`, the 1GB `nr_hugepages` file, `/proc/meminfo`,
`/proc/buddyinfo`) at `silicon_sysmem_manager.cpp:254-256, 324-326`.

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

## 14.5 Packaging / install (how these land on a host)

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

## 14.6 What a Windows equivalent must meet (Porting notes)

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
> the Windows port takes over iATU programming from UMD (there are TODOs in
> `local_chip.cpp:387` and `wormhole_tt_device.cpp:152` about moving this into
> the KMD), it must reproduce this truncation to avoid the region overlapping
> the PCIe register space.

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

## Key constants table

| Name | Value | Source cite |
|------|-------|-------------|
| Hugepage size | 1 GiB (`1048576kB`) | `hugepages-setup.sh:67`; `dev-hugepages\x2d1G.mount:11`; `tt-umd/device/hugepage.hpp:17` |
| `HUGEPAGE_REGION_SIZE` | `1ULL << 30` (1GB) | `tt-umd/device/hugepage.hpp:17` |
| `MAX_HOST_MEM_CHANNELS` | 4 | `tt-umd/device/hugepage.hpp:19` |
| Default pages: Wormhole | 4 | `hugepages-setup.sh:54` |
| Default pages: Blackhole | 4 | `hugepages-setup.sh:53` |
| Default pages: Grayskull | 1 | `hugepages-setup.sh:55` |
| TT PCI vendor ID | `1e52` | `hugepages-setup.sh:11`; `tt-kmd/enumerate.h:15` |
| Grayskull PCI device ID | `faca` | `hugepages-setup.sh:12`; `tt-kmd/enumerate.h:16` |
| Wormhole PCI device ID | `401e` | `hugepages-setup.sh:13`; `tt-kmd/enumerate.h:17` |
| Blackhole PCI device ID | `b140` | `hugepages-setup.sh:14`; `tt-kmd/enumerate.h:18` |
| Hugepage mount point | `/dev/hugepages-1G` | `dev-hugepages\x2d1G.mount:9`; `tt-umd/device/hugepage.cpp:29` |
| sysfs count path | `/sys/.../hugepages/hugepages-1048576kB/nr_hugepages` | `hugepages-setup.sh:67-73`; `tt-umd/device/hugepage.cpp:32` |
| Mount options | `pagesize=1G,mode=0777,nosuid,nodev` | `dev-hugepages\x2d1G.mount:11` |
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

## Open questions

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
