# 13. tt-umd Consumer ABI — the de facto userspace contract a Windows shim must satisfy

All paths in this section are relative to the **tt-umd** repo (`~/tt-windowsport/tt-umd`) unless prefixed otherwise. This section catalogs every OS-touching call site in tt-umd's device-access layer: which ioctls it issues (and in what order), how it discovers devices, every mmap it performs, hugepage usage, sysfs/procfs reads, fd/signal lifecycle assumptions, and threading assumptions. Together these define the ABI surface a Windows KMDF driver + user-mode shim must reproduce.

## Scope

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

## 1. Device discovery and open

### 1.1 Enumeration: readdir of `/dev/tenstorrent/`

Enumeration is a directory scan of `/dev/tenstorrent/`, keeping entries whose filename is an integer:

- `PCIDevice::get_all_device_ids()` iterates `std::filesystem::directory_iterator("/dev/tenstorrent/")` and does `std::stoi(filename)` on integer-named entries (pci_device.cpp:1077-1095). If the directory does not exist, an empty list is returned (pci_device.cpp:1081-1083, and again at 232-234).
- `PCIDevice::enumerate_devices()` (pci_device.cpp:227-326) then applies the **`TT_VISIBLE_DEVICES`** environment variable (read at pci_device.cpp:236): a comma-separated list where each token is either a device index (validated 0..N-1, else throw, pci_device.cpp:287-308) or a BDF substring pattern matched against the device's BDF string (pci_device.cpp:258-285, throws if a BDF pattern matches nothing). No filter → all devices.
- Results are **sorted by PCI BDF order** (`sort_ids_based_on_bdf`, pci_device.cpp:328-350); IDs that can't be mapped to a BDF are appended in input order.
- `enumerate_devices_info()` opens each node with `open(path, O_RDWR | O_CLOEXEC | O_APPEND)` (pci_device.cpp:355), issues `TENSTORRENT_IOCTL_GET_DEVICE_INFO`, and closes the fd (pci_device.cpp:352-368). Failures to open are silently skipped (`continue`), failures of the ioctl are swallowed by `catch (...)`.
- `get_bdf_to_device_id_map()` does the same open/ioctl/close per device (pci_device.cpp:1097-1117), again with `O_APPEND`.

There is **no** udev/by-id lookup; the N in `/dev/tenstorrent/N` is the identity used everywhere ("`N in /dev/tenstorrent/N`", pci_device.hpp:123).

A secondary discovery path exists in `cpuset_lib.cpp`: hwloc PCI enumeration filtered on vendor `0x1e52` (cpuset_lib.hpp:59), then a scan of `/sys/bus/pci/devices/<bdf>/tenstorrent/` for an entry matching regex `tenstorrent!([0-9]+)` to recover the char-dev minor for a given PCI function (cpuset_lib.cpp:108-146). This is used only for hugepage-channel counting and NUMA binding.

### 1.2 Open flags — the O_APPEND power-mode quirk

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

### 1.3 Two fds per PCIDevice

`PCIDevice`'s constructor opens the chardev **twice**:

1. `pci_device_file_desc = open_pci_device(device_path)` (pci_device.cpp:389) — used for `GET_DEVICE_INFO`, `QUERY_MAPPINGS`, `PIN_PAGES`/`UNPIN_PAGES`, `ALLOCATE_DMA_BUF`, and the BAR/DMA-buffer mmaps.
2. `tt_device_open(device_path.c_str(), &tt_device_handle, /*extra_flags=*/0)` (pci_device.cpp:414-415) — a second fd inside the `tt_kmd_lib` C library (tt_kmd_lib.c:64-81), used for `ALLOCATE_TLB`/`FREE_TLB`/`CONFIGURE_TLB`, `SET_POWER_STATE`, `GET_DRIVER_INFO`, and TLB-window mmaps.

Both fds stay open for the object's lifetime; the destructor closes the tt_kmd_lib fd first, then the raw fd, and **only afterwards** unmaps BAR0, the TLB-config page, BAR2, and the DMA buffer (pci_device.cpp:572-601).

> **Porting note:** A Windows shim must allow (a) multiple concurrent handles to the same device from one process, (b) per-handle resource accounting (TLB windows allocated on handle 2, pinned pages on handle 1), and (c) mapped views remaining valid after the owning handle is closed (Linux mmaps survive `close(fd)`; the destructor relies on this ordering). With KMDF this argues for section-based mappings whose lifetime is tied to process unmap or file-object cleanup, not handle close ordering.

### 1.4 KMD version gating (sysfs)

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

## 2. Ioctl inventory

All ioctls are defined with `_IO(0xFA, nr)` — magic `0xFA` (ioctl.h:16), **no size/direction encoded in the command word** (ioctl.h:18-33). `TENSTORRENT_DRIVER_VERSION 2` (ioctl.h:14) is the IOCTL API version tt-umd was built against. Argument structs are in/out pairs copied through the pointer arg; most "in" structs begin with `output_size_bytes` which UMD sets to `sizeof(out)` (e.g. pci_device.cpp:175, tt_kmd_lib.c:95).

### 2.1 Ioctls tt-umd issues

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

### 2.2 Ioctl order during device bring-up

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

### 2.3 Reset flow

`send_reset_ioctl(device_id, flags)` opens a *fresh* fd with `O_RDWR|O_CLOEXEC|O_APPEND` (pci_device.cpp:200-207), issues `RESET_DEVICE` with the given flags, and the helper closes the fd on success (tt_kmd_lib.c:532-550). The arch-agnostic warm reset sends `RESET_PCIE_LINK`(1) if secondary-bus reset requested, then `ASIC_DMC_RESET`(5) or `ASIC_RESET`(4), sleeps max(2 s, 0.4 s × ndevices) (or the caller-provided M3 timeout when `reset_m3` is set, warm_reset.cpp:219-221), polls for the device to reappear by glob-matching **`/sys/bus/pci/devices/<bdf>/tenstorrent/tenstorrent!*`** and checking `/dev/tenstorrent/<id>` exists (warm_reset.cpp:131-180, 182-239, glob pattern at 142), then sends `POST_RESET`(6) (warm_reset.cpp:237). Reappearance timeout is a named constant `WARM_RESET_DEVICES_REAPPEAR_TIMEOUT` (warm_reset.cpp:132). Legacy Blackhole reset uses `CONFIG_WRITE`(2), polls config-space **command byte bit 1** (read via `/sys/bus/pci/devices/<bdf>/config` offset 4, pci_device.cpp:108-123, 915-935) every 10 ms, then `RESTORE_STATE`(0) (warm_reset.cpp:241-292).

Cross-process pre/post-reset notification uses Unix-domain sockets `client_<PID>.sock` in a well-known listener directory (warm_reset.cpp:484-708).

> **Porting note:** the reset dance assumes PnP-style surprise removal/rescan semantics (device node disappears and reappears, possibly with a different minor). On Windows this maps naturally onto PnP stop/start or a bus-level reset with interface arrival notifications; the shim's "wait for reappear" must be reimplemented on device-interface notifications rather than sysfs glob.

---

## 3. mmap map — every mapping UMD creates on the device fd

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

## 4. PIN_PAGES / UNPIN_PAGES — DMA & NOC mapping of host memory

Four variants, all on `pci_device_file_desc`:

1. **`map_for_hugepage(buffer, size)`** — flags `TENSTORRENT_PIN_PAGES_CONTIGUOUS` (=1), classic 8-byte `out.physical_address`. Failure is *soft*: logs a warning and returns 0 (pci_device.cpp:603-630). Used on the legacy (pre-2.0 KMD) hugepage path (silicon_sysmem_manager.cpp:312).
2. **`map_buffer_to_noc(buffer, size)`** — flags `TENSTORRENT_PIN_PAGES_NOC_DMA` (=2) with the **extended** out struct `{physical_address, noc_address}` and `in.output_size_bytes = sizeof(out) = 16` (pci_device.cpp:634-681). Preconditions: KMD ≥ 2.0.0 (throw otherwise, 635-637), VA and size page-aligned (throw, 642-644), and `size > page ⇒ IOMMU required` (throw, 646-648).
3. **`map_hugepage_to_noc(hugepage, size)`** — flags `CONTIGUOUS|NOC_DMA` (=3) extended out (pci_device.cpp:683-735). Rejects `size > 1 GiB` ("Not a hugepage", 691-693) and non-page-aligned VA/size (695-697).
4. **`map_for_dma(buffer, size)`** — flags `is_iommu_enabled() ? 0 : CONTIGUOUS` (pci_device.cpp:737-773, flag choice at 741).

`unmap_for_dma(buffer, size)` issues `UNPIN_PAGES` with the **original** VA/size (pci_device.cpp:775-803; "original VA used to pin, not current VA if remapped", ioctl.h:193; "unpinning subset of a pinned buffer is not supported", ioctl.h:191).

`tt_dma_map` (public C API) additionally exposes `TENSTORRENT_PIN_PAGES_NOC_TOP_DOWN` (=4) via `TT_DMA_FLAG_NOC_TOP_DOWN` (tt_kmd_lib.c:348-354), returns `-EINVAL` for zero/unaligned len or null/unaligned addr (tt_kmd_lib.c:326-329), and uses `~0ULL` as "no NOC address" sentinel (tt_kmd_lib.c:369, 397-399). Documented aperture constraints the driver enforces (tt_kmd_lib.h:264-271): WH per-buffer `0x1000 ≤ len ≤ 0xFFFE_0000`, cumulative `0xFFFE_0000`, max 16 simultaneous NOC mappings; BH per-buffer `0x1000 ≤ len ≤ 0xFFFF_F000`, max 16.

### Sysmem NOC address expectations (hard-coded)

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

## 5. Hugepage usage (hugetlbfs) and the IOMMU alternative

Two mutually exclusive sysmem strategies, chosen by `pci_device_->is_iommu_enabled()` (silicon_sysmem_manager.cpp:135-142):

### 5.1 hugetlbfs path (no IOMMU)

- Page size is fixed at **1 GiB per channel**: `HUGEPAGE_REGION_SIZE = 1ULL << 30` (hugepage.hpp:17), max `MAX_HOST_MEM_CHANNELS = 4` (hugepage.hpp:19; assert at silicon_sysmem_manager.cpp:116-119).
- Channel count available = `min(target, max(1, total_hugepages / num_tt_devices_of_this_arch))` (hugepage.cpp:70-71), where `total_hugepages` comes from **`/sys/kernel/mm/hugepages/hugepages-1048576kB/nr_hugepages`** (hugepage.cpp:32, throws if unreadable) and device counts come from hwloc/sysfs (hugepage.cpp:53-55).
- Mount discovery: scans **`/proc/mounts`** for a hugetlbfs mount at exactly **`/dev/hugepages-1G`** — regex `^(nodev|hugetlbfs) (/dev/hugepages-1G) hugetlbfs ([^ ]+) 0 0$` — and verifies the `pagesize=` option equals 1 GiB (hugepage.cpp:29, 114-160). No mount ⇒ empty string ⇒ `init_hugepages` returns false (silicon_sysmem_manager.cpp:201-208).
- Per-channel file naming inside the mount (hugepage.cpp:162-195): device 0 channel 0 uses the shared legacy name `tenstorrent`; otherwise `device_<N>_` prefix and, for channel≠0, `channel_<C>_`, i.e. `device_2_channel_1_tenstorrent`.
- File open: `umask(0)` around `open(path, O_RDWR|O_CREAT|O_CLOEXEC, 0666)`; on `EACCES` the file is **unlinked and re-created** (hugepage.cpp:197-209). Failure returns −1 (soft).
- `fstat` to sanity-check size (silicon_sysmem_manager.cpp:230-233), then `mmap(nullptr, 1GiB, RW, MAP_SHARED|MAP_POPULATE, hugepage_fd, 0)` and immediate `close(hugepage_fd)` (silicon_sysmem_manager.cpp:235-238). On mmap failure, diagnostics print `/proc/cmdline` and the nr_hugepages sysfs file (254-256).
- The mapping is NUMA-bound to the device's node via hwloc (`bind_area_to_memory_nodeset`, silicon_sysmem_manager.cpp:263, cpuset_lib.hpp:36-39); failure only warns (perf issue #893).
- Pinning happens later in `start_device`: KMD ≥ 2.0 → `map_hugepage_to_noc`; older → `map_for_hugepage` + userspace iATU programming (silicon_sysmem_manager.cpp:281-343; local_chip.cpp:148-152). Pin failure prints `/sys/module/tenstorrent/version`, `/proc/meminfo`, `/proc/buddyinfo` (silicon_sysmem_manager.cpp:324-326).

### 5.2 IOMMU path

One big anonymous allocation of `num_channels × 1 GiB` (full size mmapped, silicon_sysmem_manager.cpp:352, 366), allocated by `mmap_with_hugepage_fallback`: try `MAP_HUGETLB|MAP_HUGE_1GB`, then `MAP_HUGE_512MB`, then `MAP_HUGE_2MB` (each only if size is a multiple), finally plain pages with a perf warning — all `MAP_PRIVATE|MAP_ANONYMOUS|MAP_POPULATE` (silicon_sysmem_manager.cpp:45-109). The region is pinned via one `SysmemBuffer` (`map_buffer_to_noc` or `map_for_dma`) using `iommu_mapping_size` — for WH with 4 channels this is the full size minus the 256 MiB carve-out (silicon_sysmem_manager.cpp:351-359, 400) — and per-channel `physical_address = iova + ch * 1GiB` (silicon_sysmem_manager.cpp:392-443). "Fake" channel mappings point into this buffer (378-387).

> **Porting note:** Windows has no hugetlbfs; the equivalent is `VirtualAlloc(MEM_LARGE_PAGES)` (2 MiB, and 1 GiB where supported with `SeLockMemoryPrivilege`) or, more robustly, letting the KMD allocate contiguous/remapped common buffers. The *contract* the port must keep is: (1) each channel is 1 GiB of host memory visible to the device at `pcie_base + ch*1GiB` NOC address, (2) WH channel 3 is limited to 768 MiB, (3) cross-process sharing of channel 0's legacy file name is a Linux-ism that likely need not be preserved. The `nr_hugepages`-based channel count computation must be replaced with a policy suited to the Windows allocator.

---

## 6. The PCIe DMA bounce buffer

`allocate_pcie_dma_buffer()` (pci_device.cpp:1004-1036) runs at the end of the PCIDevice ctor for WH/BH only. It tries `dma_buf_size = 512 KiB` ("empirical sweet spot", pci_device.cpp:1010-1011, 1018) and **halves on failure** down to one page. Every attempt allocates `dma_buf_size + page` — the extra page is a **completion flag page** polled by the host because "we'll need to poll a completion page to know when the DMA is done instead of receiving an interrupt" (pci_device.cpp:1013-1016; layout: `completion = buffer + dma_buf_size`, `completion_pa = buffer_pa + dma_buf_size`, pci_device.cpp:947-951, 991-995).

- **IOMMU:** anonymous `mmap` + `map_for_dma` (PIN_PAGES); on pin failure the mapping is munmapped and a smaller size tried (pci_device.cpp:937-960).
- **No IOMMU:** `ALLOCATE_DMA_BUF` with `buf_index = 0` then `mmap(fd, out.mapping_offset)` (pci_device.cpp:962-1002). If the mmap fails the buffer is *leaked until fd close* (984-986). The `NOC_DMA` allocate-flag (ioctl.h:91) is never used.

DMA transfers chunk through this bounce buffer under an in-process `dma_mutex_` (pcie_protocol.cpp:210-252) *and* a cross-process `PCIE_DMA` robust mutex (tt_device.cpp:755-756, 778-779, 801-802); the actual engine is programmed through `bar2_uc` MMIO (pcie_protocol.cpp:303-347), with 4-byte alignment/size validation throwing RuntimeError (311-317, 334-340). DMA is a best-effort fast path: if the buffer is absent the code falls back to TLB-window I/O (tt_device.cpp:758-766).

---

## 7. TLB windows: allocation, configuration, I/O

### 7.1 Allocation (ioctl) and counts

`tt_tlb_alloc` (tt_kmd_lib.c:405-444): `ALLOCATE_TLB{size}` → `{id, mmap_offset_uc, mmap_offset_wc}` → mmap of the chosen cache mode. On mmap failure it frees the TLB and restores `errno` (428-439). `tt_tlb_free` munmaps then `FREE_TLB{id}` (446-462). Hardware inventory hard-coded in tt_kmd_lib.c:26-45 and tt_kmd_lib.h:321-329:

- Wormhole: 156× 1 MiB, 10× 2 MiB, 20× 16 MiB.
- Blackhole: 202× 2 MiB, 8× 4 GiB.
- "The driver may reserve one or more TLB windows for internal use." (tt_kmd_lib.h:331)

`PCIDevice::allocate_tlb` wraps failures; for KMD ≥ 2.6 the error message directs users to **`/sys/kernel/debug/tenstorrent/N/mappings`** and **`/proc/driver/tenstorrent/N/pids`** (pci_device.cpp:820-843).

### 7.2 Configuration — two parallel mechanisms

1. **Ioctl path:** `CONFIGURE_TLB{id, config}` (tt_kmd_lib.c:474-516) — used by the convenience `tt_noc_*` helpers; `-EINVAL` if `addr` is not window-size aligned (477-479, 502-504).
2. **Direct MMIO path (the hot path):** `SiliconTlbHandle::configure` divides `local_offset` by the window size and calls `PCIDevice::configure_tlb`, which writes the packed config **directly into the mmapped TLB-config page** — *not* through the ioctl (silicon_tlb_handle.cpp:43-54; pci_device.cpp:845-894). Registers are `index × 8` bytes (WH) or `index × 12` bytes (BH) from `STATIC_TLB_CFG_ADDR` (wormhole_implementation.hpp:518, blackhole_implementation.hpp:483), written strictly as 32-bit stores because 64-bit stores to non-8-byte-aligned Device/UC memory SIGBUS on aarch64 (pci_device.cpp:857-873).

This means the KMD's `ALLOCATE_TLB` id namespace and the hardware TLB-config register indices are **the same namespace**: UMD takes the `id` returned by the ioctl and pokes `tlb_config_space + id * reg_size` itself. A Windows KMD must preserve that property (or the port must switch fully to the CONFIGURE_TLB ioctl).

### 7.3 I/O through windows

Reads/writes are volatile loads/stores plus arch-tuned `memcpy_to/from_device` on the mapped pointer (silicon_tlb_window.cpp:97-271). Register accesses are 32-bit loops with 4-byte-multiple validation done by callers (local_chip.cpp:306-313, 336-343).

---

## 8. sysfs / procfs read inventory (complete)

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

## 9. Cross-process and threading model

### 9.1 Cross-process locks live in userspace, not the KMD

tt-umd does **not** use the KMD's `LOCK_CTL` resource locks (defined ioctl.h:26, 211-229 with `TENSTORRENT_RESOURCE_LOCK_COUNT 64` at ioctl.h:47 — zero call sites). Instead it builds robust cross-process mutexes from POSIX shm:

- `shm_open("TT_UMD_LOCK.<name>", O_RDWR|O_CREAT|O_EXCL, 0666)` with EEXIST fallback to plain open, under `umask(0)` (robust_mutex.cpp:250-274; prefix robust_mutex.hpp:31; perms robust_mutex.cpp:44).
- `ftruncate` to `sizeof(pthread_mutex_wrapper)` = pthread_mutex + `initialized` flag `0x5454554d444d5458` ("TTUMDMTX", robust_mutex.cpp:46) + owner tid/pid (robust_mutex.hpp:71-81); mmap `MAP_SHARED` (robust_mutex.cpp:318); the shm fd is closed after mapping (robust_mutex.cpp:242-245).
- pthread mutex initialized `PTHREAD_PROCESS_SHARED` + `PTHREAD_MUTEX_ROBUST` (robust_mutex.cpp:326-360); `EOWNERDEAD` is recovered with `pthread_mutex_consistent` (robust_mutex.cpp:441-451, 480-487).
- First-use initialization is serialized by `flock(shm_fd, LOCK_EX)` plus a process-local static mutex (robust_mutex.cpp:58-94, 204-237).

Lock names (lock_manager.cpp:20-23): `<TYPE>_<device_id>_<devtype>`, e.g. `TT_UMD_LOCK.PCIE_DMA_0_PCIe`. Types: `ARC_MSG`, `REMOTE_ARC_MSG`, `NON_MMIO`, `MEM_BARRIER`, `CREATE_ETH_MAP`, `CHIP_IN_USE`, `PCIE_DMA` (lock_manager.hpp:18-33, 43-51). `CHIP_IN_USE` is held for the whole `start_device`…`close_device` interval (local_chip.cpp:146, 171); `MEM_BARRIER` makes host↔device barriers atomic across processes (local_chip.cpp:438-443); `PCIE_DMA` serializes bounce-buffer DMA across processes (tt_device.cpp:755-756).

> **Porting note:** all of this maps cleanly to Windows named mutexes (`Global\` namespace), which are inherently robust (`WAIT_ABANDONED` ≈ `EOWNERDEAD`). What must be preserved is the *naming discipline keyed on the OS-level device id* so independent processes agree, and the CHIP_IN_USE / PCIE_DMA / MEM_BARRIER semantics. The KMD itself needs no lock support (LOCK_CTL can be left unimplemented for tt-umd's sake).

### 9.2 Threading assumptions on the fd

- Both fds are shared by all threads of the process without any userspace lock around plain ioctls — e.g. concurrent `tt_tlb_alloc/free/map` from multiple threads (tt_kmd_lib.c has no locking). **The driver must support concurrent ioctls on one open file.**
- In-process serialization exists only above the fd: `std::mutex io_lock_` per protocol object (pcie_protocol.cpp:126, 144), `wc_tlb_lock`/`uc_tlb_lock` for cached windows (local_chip.cpp:262, 288, 320, 352), `dma_mutex_` (pcie_protocol.cpp:211).
- NOC selection is thread-local (`thread_local NocId tls_noc_id`, noc_access.cpp:12-18) — no fd-level state involved.
- `sysconf(_SC_PAGESIZE)` is queried repeatedly and cached in function-local statics (pci_device.cpp:639, 688, 738, 776; sysmem_buffer.cpp:199); all pinning alignment logic is expressed in the host page size.

### 9.3 Signals, fork/exec, crash cleanup

- **SIGBUS:** UMD optionally installs a process-wide `SIGBUS` handler with `sigsetjmp/siglongjmp` recovery so that MMIO to a dead/hot-reset device throws `SigbusError` instead of killing the process; without an active jump point the handler `_exit(sig)`s (silicon_tlb_window.cpp:29-67, 273-283). This is the mechanism behind the `safe_*` I/O API.
- **exec:** every fd is `O_CLOEXEC`; nothing is intended to be inherited.
- **fork:** no special handling; correctness across fork relies on KMD per-fd semantics plus the shm robust mutexes.
- **Crash cleanup relied upon from the KMD:** on fd close/process exit the driver must release: pinned pages, TLB windows, DMA buffers ("That only happens when we close the fd", pci_device.cpp:984-985), power-flag contributions ("When the file descriptor is closed, its contribution is removed", ioctl.h:345), and NOC-aperture reservations (the "stale or crashed process holds sysmem NOC address space" recovery advice presumes the aperture frees when the stale process dies — silicon_sysmem_manager.cpp:303-310, 413-424). `SET_NOC_CLEANUP` (fd-close NOC write, ioctl.h:308-337) exists for device-side cleanup but tt-umd never registers one.

> **Porting note:** SIGBUS-on-MMIO-failure has no direct analogue; on Windows, accesses to a mapped BAR of a removed device typically produce an access violation once the mapping is torn down, catchable via SEH (`__try/__except`) — the `safe_*` API should be ported onto SEH, and the KMD should invalidate user mappings on surprise removal rather than letting reads return all-FFs silently (UMD's hang detectors *also* rely on all-FF reads: `HANG_READ_VALUE = 0xFFFFFFFF`, architecture_implementation.hpp:29, compared in hang_detector.cpp:38-46; e.g. blackhole_tt_device.cpp:340 masks `bar_read32(...) & 0x3F`).

---

## 10. tt_kmd_lib public C API (secondary ABI consumers)

`tt_kmd_lib.h` is shipped as a stable C API over the same ioctls (used by tools and by PCIDevice itself). Its semantics constrain the shim identically: `tt_device_open` returns `-errno` conventions (tt_kmd_lib.c:64-90); `tt_device_get_attr` derives arch from PCI device id `0x401e` (WH) / `0xb140` (BH) (tt_kmd_lib.c:102-106; pci_ids.h:13-14); `tt_noc_read/write{,32}` validate 4-byte alignment (`-EINVAL`) and allocate/configure/free a 2 MiB window per call (tt_kmd_lib.c:183-323); `tt_device_reset` invalidates the handle by closing it on success (tt_kmd_lib.c:549).

---

## Key constants table

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

## Open questions

1. **`QUERY_MAPPINGS` out-count semantics.** UMD sets `in.output_mapping_count = 8` and, after the ioctl, iterates `in.output_mapping_count` entries (pci_device.cpp:449, 466). Whether the KMD rewrites this field to the number of valid mappings, or leaves it and pads with `mapping_id == 0`, must be confirmed against the tt-kmd source (section covering `ioctl.c`); the shim must reproduce whichever it is.
2. **Wormhole BAR check vs. mapping mismatch.** For WH the ctor *checks* that `RESOURCE2_UC` (BAR4) exists but then mmaps `bar2_uc_mapping` (`RESOURCE1`, BAR2) (pci_device.cpp:532-547). It is unclear whether this is intentional (BAR2 presence implied by BAR4?) or a latent bug; a Windows port should verify which BAR WH actually needs for `bar2_uc` consumers (pcie_protocol.cpp:305, 328).
3. **In-place mmap-offset arithmetic.** UMD adds byte offsets to `mapping_base` (509 MiB, `0x1fc00000`) assuming the KMD's mmap offset space is linear within a resource. Confirm from the KMD mmap handler that arbitrary page-aligned offsets within a mapping are legal (vs. only whole-resource maps), since the shim must match.
4. **Behavior of `map_for_hugepage` returning physical address 0.** UMD treats `physical_address == 0` as a failure sentinel (silicon_sysmem_manager.cpp:315). If a legitimate pin could ever produce IOVA/PA 0, this path misbehaves; worth pinning down for the shim's address allocation policy.
5. **Ioctls unused by tt-umd (`GET_HARVESTING`, `LOCK_CTL`, `MAP_PEER_BAR`, `FREE_DMA_BUF`, `SET_NOC_CLEANUP`).** Out-of-repo consumers (tt-smi, luwen-based tools, tt-metal debug tooling) may still use them; this section only proves tt-umd does not. Scope for the Windows shim's ioctl coverage needs a decision informed by the other analysis sections.
6. **`O_APPEND` policy going forward.** The main handle's `O_APPEND` is "temporarily disabled" pending tt-umd issue #2531 (pci_device.cpp:379-380). The Windows equivalent (a create-option or first-ioctl flag selecting legacy vs. modern power semantics) should support both modes since UMD may flip this back.
7. **Completion-page DMA protocol.** The bounce-buffer completion page is polled by userspace and the whole mechanism is called "a temporary hack until it's implemented in the driver" (pci_device.cpp:1013-1016). The exact producer of the completion write is in the DMA strategy code (`std::visit(... d2h_transfer ...)`, pcie_protocol.cpp:323, 346) which lives outside the files read here; the Windows design should decide whether to reproduce the hack or implement interrupt-driven DMA in the KMD from the start.
8. **hwloc dependency on Windows.** `cpuset_lib`'s device counting and NUMA binding (used to size hugepage channels and bind sysmem) is hwloc + sysfs; whether the Windows port keeps hwloc (it supports Windows) or replaces it with `GetNumaProcessorNode`-family APIs is open.
