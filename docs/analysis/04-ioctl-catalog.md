# 04. IOCTL Catalog (ABI Contract)

## Scope

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

## 1. Command encoding

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

## 2. Dispatch, gating, and reset interaction (applies to ALL ioctls)

`tt_cdev_ioctl()` (chardev.c:591-706):

1. `ret` is initialized to `-EINVAL` (chardev.c:595); unknown commands fall to `default: ret = -EINVAL` (chardev.c:694-696).
2. **Reset rwsem**: `RESET_DEVICE` takes `tt_dev->reset_rwsem` **exclusive** (`down_write`); every other ioctl takes it **shared** (`down_read`) (chardev.c:596-601). The rwsem is writer-fair, so a pending reset starves out new readers.
3. **Detached gate**: `if (priv->device->detached) return -ENODEV;` — an fd on a removed/hotplugged device is permanently invalid (chardev.c:604-607).
4. **Reset-generation gate**: `if (atomic_long_read(&priv->device->reset_gen) != priv->open_reset_gen) return -ENODEV;` — an fd opened before a generation-bumping reset is permanently invalid (chardev.c:609-613). `open_reset_gen` is latched at open (chardev.c:812); the resetting fd itself is exempted because `bump_reset_gen()` updates its own `open_reset_gen` (chardev.c:195-198).
5. **needs_hw_init gate**: between a destructive reset and its `POST_RESET`, only `GET_DEVICE_INFO`, `GET_DRIVER_INFO`, and `RESET_DEVICE` are allowed; everything else gets `-ENODEV` (chardev.c:616-624).

`mmap` has the same three gates but uses `down_read_trylock` and returns `-ENODEV` if the rwsem is contended, to avoid an ABBA deadlock with `tenstorrent_vma_zap()` which takes `mmap_lock` while holding `reset_rwsem` (chardev.c:708-736).

Device removal (`tenstorrent_pci_remove`, enumerate.c:404-481) sets `detached = true` under `chardev_mutex` (enumerate.c:423-425), drains in-flight ioctls with `down_write(&reset_rwsem)` around VMA zap and BAR unmap (enumerate.c:444-447), revokes all TLB dma-buf exports (enumerate.c:459), wakes `resource_lock_waitqueue` so `ACQUIRE_BLOCKING` waiters observe `detached` (enumerate.c:461-463), and runs `tenstorrent_memory_cleanup()` on every still-open fd (enumerate.c:465-467).

## 3. Argument-passing protocols

Four distinct conventions coexist. This is the single most important ABI subtlety.

### 3a. `output_size_bytes` protocol (ioctls 0, 5, 6, 7, 8)

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

### 3b. Count-based protocol (ioctl 2, QUERY_MAPPINGS)

`in.output_mapping_count` is a count of `struct tenstorrent_mapping` slots. Kernel copies `min(count, valid)` entries and zero-fills the remaining declared slots (memory.c:393-406).

### 3c. `argsz` protocol (ioctls 14, 15, 16)

Single flat struct (no in/out split). The kernel copies the whole struct in, requires `argsz == sizeof(struct)` **exactly** (`-EINVAL` otherwise) and `flags == 0`. No truncation tolerance — a future struct growth requires a new size handshake. E.g. chardev.c:448-453, chardev.c:570-574, memory.c:1164-1169. Only EXPORT_TLB_DMABUF copies the struct back out (with `fd` filled, memory.c:1264-1269); SET_NOC_CLEANUP and SET_POWER_STATE produce no output.

### 3d. Fixed-size, no size negotiation (ioctls 3, 4, 9, 10, 11, 12, 13)

Fixed `sizeof(in)` copied in, fixed `sizeof(out)` copied out (or nothing). Any struct growth would be a hard ABI break.

## 4. mmap offset namespace (returned by ioctls 2, 3, 11)

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

### mapping_id constants

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

## 5. Per-fd and per-device state touched by ioctls

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

# The 17 ioctls

## Ioctl 0 — TENSTORRENT_IOCTL_GET_DEVICE_INFO (cmd 0xFA00)

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

## Ioctl 1 — TENSTORRENT_IOCTL_GET_HARVESTING (cmd 0xFA01)

**There is no handler.** The dispatch case is an empty `break`, leaving `ret` at its initialization value:

```c
long ret = -EINVAL;                          // chardev.c:595
...
case TENSTORRENT_IOCTL_GET_HARVESTING:
	break;                               // chardev.c:631-632
```

**Semantics:** always returns `-EINVAL` (after passing the detached/reset-gen/needs_hw_init gates, which can return `-ENODEV` first). No struct is read or written. tt-umd's vendored `ioctl.h` defines it (tt-umd/device/pcie/ioctl.h:19) but no tt-umd code calls it.

> **Porting note:** implement as an immediate STATUS_INVALID_PARAMETER (the -EINVAL analog) stub; do not invent harvesting data.

## Ioctl 2 — TENSTORRENT_IOCTL_QUERY_MAPPINGS (cmd 0xFA02)

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

## Ioctl 3 — TENSTORRENT_IOCTL_ALLOCATE_DMA_BUF (cmd 0xFA03)

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

## Ioctl 4 — TENSTORRENT_IOCTL_FREE_DMA_BUF (cmd 0xFA04)

Handler: `ioctl_free_dma_buf` (memory.c:515-523). **Unconditionally returns `-EINVAL`**, with the comment: "This is unsupported until I figure out how to block freeing as long as a mapping exists. Otherwise the dma buffer is freed when the struct file is destroyed" (memory.c:518-521). Input/output structs are empty (ioctl.h:113-122); nothing is copied. DMA buffers are only freed at fd close.

## Ioctl 5 — TENSTORRENT_IOCTL_GET_DRIVER_INFO (cmd 0xFA05)

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

tt-umd gates features on this (kmd_versions.hpp; pci_device.cpp:158).

## Ioctl 6 — TENSTORRENT_IOCTL_RESET_DEVICE (cmd 0xFA06)

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

Used by tt-umd (pci_device.cpp mention at :545 and its ioctl.h).

## Ioctl 7 — TENSTORRENT_IOCTL_PIN_PAGES (cmd 0xFA07)

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

## Ioctl 8 — TENSTORRENT_IOCTL_LOCK_CTL (cmd 0xFA08)

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

## Ioctl 9 — TENSTORRENT_IOCTL_MAP_PEER_BAR (cmd 0xFA09)

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

## Ioctl 10 — TENSTORRENT_IOCTL_UNPIN_PAGES (cmd 0xFA0A)

Handler: `ioctl_unpin_pages` (memory.c:746-780). Protocol §3d (out struct empty; nothing is written back).

**Structs** (ioctl.h:190-203):

`tenstorrent_unpin_pages_in` — 24 bytes: `virtual_address` __u64 @0 ("original VA used to pin, not current VA if remapped", ioctl.h:192); `size` __u64 @8; `reserved` __u64 @16 (**must be 0**). "unpinning subset of a pinned buffer is not supported" (ioctl.h:190).

**Validation:** `-EFAULT`; `reserved != 0 || size == 0 || (size >> PAGE_SHIFT) == 0` → `-EINVAL` (memory.c:757-760).

**Semantics:** under `priv->mutex`, scan `priv->pinnings` for exact `virtual_address` match; if found but `page_count != size>>PAGE_SHIFT` → `-EINVAL`; else `unpin_pinned_page_range` (iATU teardown → dma unmap → dirty-unpin → free) and return 0 (memory.c:762-776). No match → `-EINVAL` (initial value, memory.c:752).

**Concurrency:** `priv->mutex`. **Lifetime:** removes exactly one pinning; the rest die at close.

Used by tt-umd (pci_device.cpp:788).

## Ioctl 11 — TENSTORRENT_IOCTL_ALLOCATE_TLB (cmd 0xFA0B)

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
- Blackhole: 202 x 2 MiB in BAR0 and up to 8 x 4 GiB in BAR4 — 4G count is trimmed to `bar4_len / 4GiB` at probe (blackhole.c:20-31, 580, 820-821); 2 MiB window 201 is kernel-reserved (blackhole.c:40, 610).

**Validation & allocation:** `dev_class->describe_tlb == NULL` → `-EINVAL` (memory.c:902-903). `tenstorrent_device_allocate_tlb`: no kind with `tlb_size == size` → `-EINVAL`; scan that kind's bit range in `tt_dev->tlbs` with `find_next_zero_bit` + `test_and_set_bit`; all busy → `-ENOMEM`; on repeated races, `cond_resched()` and `-ERESTARTSYS` if a signal is pending (tlb.c:20-50). On claim, `refcount_set(&tt_dev->tlb_refcount[id], 1)` (tlb.c:43). Then `describe_tlb` failure or a window in a BAR other than 0/4 frees the window and returns `-EINVAL` (memory.c:913-922). `copy_to_user` failure frees the window, `-EFAULT` (memory.c:936-939). Success: `set_bit(id, priv->tlbs)` — **no `priv->mutex` held**; the bit op itself is atomic (memory.c:941).

**Concurrency:** allocation is lock-free atomic bitmap claim on the *device* bitmap; ownership recording on the *fd* bitmap is a bare set_bit.

**Side effects / lifetime:** window id is a per-device resource owned by this fd; freed by FREE_TLB, or at close via `tt_cdev_release_tlbs` (chardev.c:887-892), except a dma-buf export keeps the underlying window allocated via `tlb_refcount` (tlb.c:71-79). The returned mmap offsets are consumed by `mmap` (`map_tlb_window`, memory.c:1494-1583; requires ownership, `-EPERM` otherwise).

Used by tt-umd (pci_device.cpp:417).

## Ioctl 12 — TENSTORRENT_IOCTL_FREE_TLB (cmd 0xFA0C)

Handler: `ioctl_free_tlb` (memory.c:946-982) → `tenstorrent_device_free_tlb` (tlb.c:56-81). Protocol §3d (out empty).

**Struct** (ioctl.h:288-298): `tenstorrent_free_tlb_in` — 4 bytes: `id` __u32 @0.

**Validation & semantics:** `-EFAULT`; `id >= TENSTORRENT_MAX_INBOUND_TLBS (256)` → `-EINVAL` (the `in.id < 0` half of the check is dead code on a u32, memory.c:955-956). Under `priv->mutex`: not owned by this fd → `-EPERM` (memory.c:960-963); any live user VMA of this window (scan `priv->vma_list` under `vma_lock` for `TT_VMA_TLB && tlb.id == id`) → `-EBUSY` — **you must munmap before freeing** (memory.c:965-974). Then clear fd bit and `tenstorrent_device_free_tlb`: `id >= total` → `-EINVAL`; device bit not set → `-EPERM`; `refcount_dec_and_test(&tlb_refcount[id])` and only then `clear_bit` — a live dma-buf export keeps the window out of the pool (tlb.c:62-79).

**Errors:** `-EFAULT`, `-EINVAL`, `-EPERM`, `-EBUSY`. **Concurrency:** `priv->mutex` + `priv->vma_lock`. **Lifetime:** same freeing runs per set bit at close (without the VMA check — VMAs are already gone or being torn down by then, chardev.c:887-892).

Used by tt-umd (pci_device.cpp:432, 455).

## Ioctl 13 — TENSTORRENT_IOCTL_CONFIGURE_TLB (cmd 0xFA0D)

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

Used by tt-umd (pci_device.cpp:492, 511).

## Ioctl 14 — TENSTORRENT_IOCTL_SET_NOC_CLEANUP (cmd 0xFA0E)

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

Present in tt-umd's vendored header; call sites exist in tt-umd (pci_device.cpp:309 area per grep).

## Ioctl 15 — TENSTORRENT_IOCTL_SET_POWER_STATE (cmd 0xFA0F)

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

Used by tt-umd (pci_device.cpp:525 area; version-gated per kmd_versions.hpp — introduced in KMD 2.6.0).

## Ioctl 16 — TENSTORRENT_IOCTL_EXPORT_TLB_DMABUF (cmd 0xFA10)

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

## open()/close() flags that shape ioctl behavior

- `O_EXCL` open: waits (or `-EAGAIN` with `O_NONBLOCK`) until **no other fd** is open, then holds device-exclusivity; non-exclusive opens wait for the O_EXCL holder to go away (`admit_chardev_open`, chardev.c:743-789). Interruptible → `-ERESTARTSYS`.
- `O_APPEND` open: marks the fd "power aware" (see ioctl 15) (chardev.c:795, 817-823).
- Every open records pid/comm for the procfs `pids` file (chardev.c:814-815, 111-113).

## tt-umd usage summary (quick grep; deep analysis is another section's job)

Called from tt-umd code: GET_DEVICE_INFO, GET_DRIVER_INFO, QUERY_MAPPINGS, PIN_PAGES (4 sites incl. extended-out and hugepage paths), UNPIN_PAGES, ALLOCATE_DMA_BUF, ALLOCATE_TLB, FREE_TLB, CONFIGURE_TLB, SET_POWER_STATE, SET_NOC_CLEANUP, RESET_DEVICE (tt-umd/device/pcie/pci_device.cpp:177, 158, 451, 611/660/714/753, 788, 971, 417, 432/455, 492/511, 525, 309, 545). Defined-but-unused by tt-umd: GET_HARVESTING, FREE_DMA_BUF, LOCK_CTL, MAP_PEER_BAR; EXPORT_TLB_DMABUF absent from its header.

## Key constants table

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
| BH TLB windows | 202 x 2M (BAR0, id 201 kernel-reserved), ≤8 x 4G (BAR4) | blackhole.c:20-31, 40, 580, 610, 820-821 |
| BH noc_dma_limit / noc_pcie_offset | (1<<58)-1 / 4<<58 | blackhole.c:817-818 |
| TT_TLB_DMABUF_SG_CHUNK | 1 GiB | memory.c:1036 |
| PCIe hot reset timings | 2 ms assert, 500 ms settle, 10000 ms link poll | pcie.c:76-80 |
| INTERFACE_TIMER_CONTROL_OFF / TARGET_OFF | 0x930 / 0x934 | pcie.c:17-18 |
| Reset marker | PCI_COMMAND parity-error bit | pcie.c:140-158 |

## Open questions

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
