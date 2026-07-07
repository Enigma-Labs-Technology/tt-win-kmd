# Porting tt-umd to Windows over ttwin_compat

This guide enumerates every OS-touching call site in tt-umd's device-access
layer and its `ttwin_compat` replacement, so tt-umd can be ported by swapping its
device-access layer with no logic changes above it. Line numbers are against the
tt-umd checkout analyzed in `docs/linux-driver-analysis.md` §13
(`device/pcie/pci_device.cpp`, `device/tt_kmd_lib/tt_kmd_lib.c`, etc.).

The ioctl request constants (`TENSTORRENT_IOCTL_*`) and every struct layout are
**unchanged** — they come from `src/include/ttkmd_ioctl.h`, which is byte-identical
to `tt-kmd/ioctl.h` (enforced by `ttkmd_abi_check.h` static_asserts). Only the
five syscalls change.

## The mechanical swap

| tt-umd (Linux) | ttwin_compat (Windows) | Notes |
|---|---|---|
| `open("/dev/tenstorrent/N", flags)` | `tt_open(N, flags)` | returns `tt_handle` (opaque) instead of `int`; `NULL` on failure |
| `ioctl(fd, TENSTORRENT_IOCTL_X, &s)` | `tt_ioctl(h, TENSTORRENT_IOCTL_X, &s)` | request constant + struct unchanged; returns 0 / -1 |
| `mmap(NULL, len, prot, MAP_SHARED, fd, off)` | `tt_mmap(h, len, prot, flags, off)` | `off` is the same mmap-offset token; returns addr / `TT_MAP_FAILED` |
| `munmap(addr, len)` | `tt_munmap(h, addr, len)` | |
| `close(fd)` | `tt_close(h)` | mapped views stay valid until `tt_munmap` (Linux semantics preserved) |
| `readdir("/dev/tenstorrent/")` + `std::stoi` | `tt_enumerate(ids, max)` | returns present device ids |
| `errno` after a failed call | `tt_get_last_error()` | Windows error code |

`tt_handle` is a pointer typedef; replace `int fd` fields with `tt_handle`. All
the `ioctl`/`mmap` argument structs and the offset-token arithmetic
(`mapping_base + byte_offset`, `mmap_offset_uc/wc`) stay exactly as written.

## Call-site inventory (every OS touch in the device-access layer)

### Device discovery
- `PCIDevice::get_all_device_ids()` — `directory_iterator("/dev/tenstorrent/")`
  + `std::stoi` (pci_device.cpp:1077-1095) → **`tt_enumerate()`**. The Windows
  device id is the interface reference-string ordinal `TTN`, matching the Linux
  `N` in `/dev/tenstorrent/N`.
- `enumerate_devices_info()` / `get_bdf_to_device_id_map()` — per-device
  open→`GET_DEVICE_INFO`→close (pci_device.cpp:352-368, 1097-1117) → `tt_open` +
  `tt_ioctl(GET_DEVICE_INFO)` + `tt_close`. The BDF comes from
  `tenstorrent_get_device_info_out.bus_dev_fn` (unchanged).
- `TT_VISIBLE_DEVICES` env filtering and BDF-sort (pci_device.cpp:227-350) is
  pure logic above the syscalls — port unchanged.
- **hwloc/sysfs path in `cpuset_lib.cpp`** (NUMA binding, hugepage-channel
  counting): not covered by the shim. See "Out of scope" below.

### The two fds per PCIDevice (pci_device.cpp:389, 414-415)
Open two `tt_handle`s exactly as tt-umd opens two fds — one for BAR/DMA/pin, one
inside `tt_kmd_lib` for TLB/power/driver-info. The shim allows multiple concurrent
handles to the same device from one process, with per-handle resource accounting
(TLB windows on handle 2, pinned pages on handle 1), matching Linux.

### ioctls issued (all via `tt_ioctl`, request + struct unchanged)
| tt-umd site | ioctl | Windows status |
|---|---|---|
| pci_device.cpp:177, tt_kmd_lib.c:97 | GET_DEVICE_INFO | tested |
| tt_kmd_lib.c:158 | GET_DRIVER_INFO | tested |
| pci_device.cpp:451 | QUERY_MAPPINGS | tested |
| pci_device.cpp:611/660/714/753, tt_kmd_lib.c:356 | PIN_PAGES | functional (direct/contiguous path; see below) |
| pci_device.cpp:788, tt_kmd_lib.c:382 | UNPIN_PAGES | functional |
| pci_device.cpp:971 | ALLOCATE_DMA_BUF | tested |
| tt_kmd_lib.c:417 | ALLOCATE_TLB | tested |
| tt_kmd_lib.c:432/455 | FREE_TLB | tested |
| tt_kmd_lib.c:492/511 | CONFIGURE_TLB | tested |
| tt_kmd_lib.c:525 | SET_POWER_STATE | tested |
| pci_device.cpp:905 (send_reset_ioctl) | RESET_DEVICE | tested |

### mmaps (all via `tt_mmap`, offset token unchanged)
| tt-umd site | region | offset token |
|---|---|---|
| pci_device.cpp:503 | BAR0 | `QUERY_MAPPINGS` RESOURCE0 base (UC/WC) |
| pci_device.cpp:518 | TLB config page (BAR0 sub-range) | RESOURCE0 base + register offset |
| pci_device.cpp:538/556 | BAR2 | RESOURCE1 base |
| pci_device.cpp:975 | DMA buffer | `ALLOCATE_DMA_BUF.mapping_offset` |
| tt_kmd_lib.c:426 | TLB window | `ALLOCATE_TLB.mmap_offset_uc/wc` |

The shim keeps the driver handle open past `tt_close` until the last `tt_munmap`,
so tt-umd's destructor ordering (close fds, *then* unmap — pci_device.cpp:572-601)
works unchanged.

## Behavioral notes the porter must know

1. **PIN_PAGES requires contiguous memory.** The Windows driver implements the
   direct (no-IOMMU) path: pinned pages must be physically contiguous, and
   `READ_ONLY` is unsupported (STATUS_NOT_SUPPORTED). tt-umd's sysmem for DMA
   comes from hugepages / driver-allocated buffers, which are contiguous, so the
   common path works; a caller pinning arbitrary malloc'd memory will get
   `-EINVAL`-equivalent (this matches Linux's no-IOMMU behavior). See DD-8.
2. **O_APPEND power mode.** tt-umd currently opens the main handle *without*
   `O_APPEND` (legacy power mode; issue #2531). `tt_open` accepts `TT_O_APPEND`
   and defaults to legacy mode otherwise — same as the Linux default. When tt-umd
   re-enables O_APPEND, the shim maps it to the client-flags opt-out (DD-10).
3. **mmap survives close** is emulated by the shim (handle kept alive until last
   unmap); do not rely on the driver keeping mappings past cleanup.
4. **Reset invalidates other handles.** After `RESET_DEVICE`, every other open
   handle fails with device-removed — identical to Linux `-ENODEV`. tt-umd's
   reset flow already reopens a fresh handle per reset (warm_reset.cpp), so this
   ports unchanged.

## Out of scope for the shim (separate Windows work)

- **hugepages** (`hugepage.cpp`, tt-system-tools): Linux hugetlbfs has no direct
  Windows analogue; use the driver's coherent DMA buffers (ALLOCATE_DMA_BUF, up
  to 256 × 256 MiB) or large-page host allocations. See DD (M3 hugepage plan) and
  `docs/linux-driver-analysis.md` §14.
- **hwloc / NUMA binding** (`cpuset_lib.cpp`): replace with
  `GetNumaProcessorNode`/`GetLogicalProcessorInformationEx` or keep hwloc (it
  supports Windows). Only affects sysmem channel sizing/placement, not correctness.
- **MAP_PEER_BAR, EXPORT_TLB_DMABUF**: not implemented (single-device rig / no
  Windows dma-buf). tt-umd does not use MAP_PEER_BAR; EXPORT_TLB_DMABUF is
  RDMA-P2P only. Both return an error through `tt_ioctl` if attempted.

## Verification

`ttconform.exe` (this repo) drives every implemented ioctl and mmap **exclusively
through `ttwin_compat`** — including the mmap-survives-close behavior and the
QUERY_MAPPINGS variable-size buffer — proving the shim is a working ABI a ported
tt-umd builds against.
