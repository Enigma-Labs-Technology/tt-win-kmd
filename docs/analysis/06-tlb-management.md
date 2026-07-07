# 06. TLB Window Management

## Scope

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

## 1. TLB abstraction and data structures

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

## 2. Per-device TLB pools (exact tables)

### Wormhole (`wormhole_class`, wormhole.c:1055-1084)

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

### Blackhole (`blackhole_class`, blackhole.c:813-840)

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

## 3. ALLOCATE_TLB — size validation and pool selection

uAPI (ioctl.h:270-286): input is only `__u64 size` (+reserved); output is `__u32 id`, `__u64 mmap_offset_uc`, `__u64 mmap_offset_wc`.

### Core allocator (`tenstorrent_device_allocate_tlb`, tlb.c:9-54)

- Returns `-EINVAL` if the class has no TLB kinds (tlb.c:17-18).
- **Exact-size pool selection** (tlb.c:20-30): walks kinds in order, accumulating `offset += tlb_count` (per-device counts), and selects the kind where `size == tlb_size`. **No rounding** — the requested size must exactly equal one of the pool sizes (1M/2M/16M on WH; 2M/4G on BH) or the result is `-EINVAL` (tlb.c:32-33).
- **Lock-free claim loop** (tlb.c:36-50): `find_next_zero_bit(tt_dev->tlbs, offset + n, offset)`; if none free in the pool, `-ENOMEM`; otherwise `test_and_set_bit(id, ...)` and on success `refcount_set(&tt_dev->tlb_refcount[id], 1)` and return id. On a lost race it retries with `cond_resched()`, returning `-ERESTARTSYS` if a signal is pending (tlb.c:47-49). No mutex is held; correctness relies on atomic bitops.

### ioctl wrapper (`ioctl_allocate_tlb`, memory.c:893-944)

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

## 4. CONFIGURE_TLB — register programming

uAPI config struct (ioctl.h:300-313):

```c
struct tenstorrent_noc_tlb_config {
	__u64 addr;
	__u16 x_end;  __u16 y_end;  __u16 x_start;  __u16 y_start;
	__u8 noc;  __u8 mcast;  __u8 ordering;  __u8 linked;  __u8 static_vc;
	__u8 reserved0[3];  __u32 reserved1[2];
};
```

### ioctl path (`ioctl_configure_tlb`, memory.c:984-999)

- `-EFAULT` on copy-in; `-EINVAL` if `id >= TENSTORRENT_MAX_INBOUND_TLBS` (256); `-EPERM` if the fd does not own the window (`!test_bit(in.id, priv->tlbs)`, memory.c:995-996). No lock is held around the ownership test or the register write.
- Dispatches through `tenstorrent_device_configure_tlb()` (tlb.c:101-108), `-EINVAL` if the class lacks the hook.
- **No validation of coordinates or field ranges**: `x_end`/`y_end`/etc. are silently truncated to the hardware bit-field widths; `reserved0`/`reserved1` are not checked.

### Wormhole programming (wormhole.c:809-891)

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

### Blackhole programming (blackhole.c:112-226, 724-736)

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

### Kernel TLB vs. user TLBs

Each arch reserves exactly one window for kernel use (WH id 185, BH id 201, see §2). Kernel NOC access reprograms that window on every access under a per-device `kernel_tlb_mutex` (wormhole.h:12 / blackhole.h:13) with `ordering = 1 // strict` and unicast target (`wh_configure_kernel_tlb`, wormhole.c:917-929; `bh_configure_kernel_tlb`, blackhole.c:228-241), then does a single `ioread32`/`iowrite32` (`noc_read32`/`noc_write32`, wormhole.c:931-954, blackhole.c:243-268). Consumers: telemetry probe, ARC message queue, CSM read/write, reset save/restore of DBI MPS, and the per-fd NOC-cleanup write at release (chardev.c:865-875). There is no other kernel/user distinction — user windows are programmed by the same `configure_tlb` hooks.

> **Porting note (registers):** The bit layouts above are the load-bearing hardware contract. In a KMDF driver, replicate them with explicit shifts/masks rather than C bitfields (MSVC bitfield layout of the 96-bit packed struct is not guaranteed to match GCC's). Writes must be 32-bit MMIO stores in low-to-high order (`WRITE_REGISTER_ULONG`); the BH 12-byte stride means odd indices are only 4-byte aligned, so 64-bit stores are unsafe (tt-umd documents the same constraint for aarch64, tt-umd/device/pcie/pci_device.cpp:857-871).

> **Porting note (kernel window):** The Windows driver needs the same reserved-window + mutex construct for its own ARC/telemetry traffic, and must mark the reserved id allocated before any user can reach the allocator.

---

## 5. FREE_TLB

### ioctl (`ioctl_free_tlb`, memory.c:946-982)

1. `-EFAULT` on copy-in; `-EINVAL` if `id >= 256` (memory.c:955-956; the `in.id < 0` half is dead code, `id` is `__u32`).
2. Takes `priv->mutex` (memory.c:958). `-EPERM` if the fd does not own the window (memory.c:960-963).
3. Under `priv->vma_lock`, scans `priv->vma_list`; if any live VMA is a `TT_VMA_TLB` for this id → `-EBUSY` (memory.c:965-974). **A window cannot be freed while mmapped.**
4. `clear_bit(in.id, priv->tlbs)` then `tenstorrent_device_free_tlb()` (memory.c:976-977).

### Core free (`tenstorrent_device_free_tlb`, tlb.c:56-81)

- `-EINVAL` if no kinds or `id >= total_tlbs` (sum of per-device counts, tlb.c:62-69); `-EPERM` if the device bit is not set (tlb.c:71-72).
- Drops the owning reference: `if (refcount_dec_and_test(&tt_dev->tlb_refcount[id])) clear_bit(id, tt_dev->tlbs);` (tlb.c:77-78) — if a dma-buf export still holds a reference, the bit stays set and the window returns to the pool only when the last export is released (comment tlb.c:74-76; `tenstorrent_tlb_export_get/put`, tlb.c:87-99).

The hardware register is **not** cleared or reprogrammed on free; the next owner inherits whatever configuration was last written until it calls CONFIGURE_TLB.

---

## 6. mmap routing and `mmap_offset_uc`/`mmap_offset_wc` encoding

### Offset namespace (memory.c:255-274)

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

### Encoding at allocation (memory.c:924-934)

```c
encoded_id = tlb_desc.bar_offset;
if (tlb_desc.bar == 4)
	encoded_id += BAR0_SIZE;           // BAR0_SIZE = (1UL << 29) = 512 MiB, memory.c:29
out.mmap_offset_uc = MMAP_OFFSET_TLB_UC + encoded_id;
out.mmap_offset_wc = MMAP_OFFSET_TLB_WC + encoded_id;
```

I.e. the offset within the TLB region equals the window's BAR0 byte offset, except Blackhole's BAR4 4G windows are re-based at 512 MiB ("the size of BAR0", comment memory.c:926-928). Maximum encoded value 0x8_2000_0000 fits well inside the 64 GiB region.

### Decode at mmap (`tenstorrent_mmap` → `map_tlb_window`, memory.c:1585-1636, 1494-1583)

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

### How userspace uses this (tt-umd)

`tt_kmd_lib.c` calls `TENSTORRENT_IOCTL_ALLOCATE_TLB`, picks `out.mmap_offset_uc` or `out.mmap_offset_wc` by cache mode, and mmaps the device fd at that offset (tt-umd/device/tt_kmd_lib/tt_kmd_lib.c:417-426). Configuration goes either through the CONFIGURE_TLB ioctl (tt-umd/device/tt_kmd_lib/tt_kmd_lib.c:492, 511) or — in `SiliconTlbHandle::configure` — by writing the TLB config registers **directly from user space through a BAR0 mapping** (tt-umd/device/pcie/silicon_tlb_handle.cpp:43-55, tt-umd/device/pcie/pci_device.cpp:845-895). The direct path works on Linux because QUERY_MAPPINGS+mmap exposes all of BAR0 (including the register file at 0x1FC00000) with no ownership checks.

---

## 7. EXPORT_TLB_DMABUF (window pinning across free/close)

`ioctl_export_tlb_dmabuf` (memory.c:1146-1279) wraps a window's BAR aperture sub-range in a dma-buf for peer-to-peer DMA (e.g. RDMA NICs; rationale in ioctl.h:413-448). Validation: `-EFAULT` on copy; `-EINVAL` for `argsz != sizeof`, nonzero `flags`, `tlb_id >= 256`, non-page-aligned `offset`/`size`, missing `describe_tlb`, out-of-window range, or range exceeding the BAR; `-EPERM` if the fd doesn't own the window (memory.c:1161-1211, under `priv->mutex`).

Lifetime rules a port must honor:
- The export takes a device kref **and** a TLB refcount (`tenstorrent_tlb_export_get`, memory.c:1233-1234), so FREE_TLB or fd close does not return the window to the pool while the export lives; only `tt_tlb_dmabuf_release` drops it (memory.c:1118-1135, tlb.c:95-99). This prevents reallocation+reconfiguration from redirecting a live importer's DMA (ioctl.h:434-438).
- Destructive resets are refused with `-EBUSY` while any export is live (chardev.c:222-233, `tenstorrent_has_tlb_dmabuf_exports`, memory.c:1299-1308).
- Exports are revoked (mappings invalidated, `revoked` flag set) on device removal and suspend (`tenstorrent_revoke_tlb_dmabufs`, memory.c:1281-1297; called at enumerate.c:459 and enumerate.c:504).
- Importers must support P2P (`attach` rejects non-peer2peer with `-EOPNOTSUPP`, memory.c:1023-1031); the BAR range is mapped into the importer's IOMMU domain with `dma_map_resource` and described in <=1 GiB scatterlist chunks (memory.c:1036-1090).

> **Porting note:** dma-buf has no Windows equivalent; the closest concepts are NTB/DirectGMA-style vendor APIs. For a first port this ioctl can return `STATUS_NOT_SUPPORTED` (Linux itself returns `-EOPNOTSUPP` on kernels < 5.8, memory.c:1310-1315) — but then the `-EBUSY`-on-reset guard and the export refcount machinery drop out too. The refcount design in tlb.c is still worth keeping if any future Windows P2P mechanism arrives.

---

## 8. Lifecycle: fd close, reset, removal

**fd close** (`tt_cdev_release`, chardev.c:922-958, run under `reset_rwsem` shared): after the NOC-cleanup write and memory cleanup, `tt_cdev_release_tlbs()` walks the per-fd bitmap and calls `tenstorrent_device_free_tlb()` for every owned window (chardev.c:887-892, called at chardev.c:934). By this point all of the process's VMAs are gone (munmap at process exit ran `tenstorrent_vma_close`), so there is no `-EBUSY` interaction; windows with live dma-buf exports stay allocated per §7. Release runs even for fds invalidated by a reset — the bitmap is device state, not generation-scoped.

**Reset** (`ioctl_reset_device`, chardev.c:200-310, under `reset_rwsem` exclusive): all destructive flavors call `tenstorrent_vma_zap(tt_dev)` (chardev.c:248-267), which unmaps the PTEs of **every** tracked BAR and TLB VMA across all open fds (memory.c:1677-1741; VFIO-derived lock-ordering dance between `mmap_lock` and `vma_lock`, comment memory.c:1683-1689). Because `tlb_vm_ops` has no `.fault` handler, any subsequent user access to a zapped mapping takes SIGBUS. The reset also bumps `reset_gen`, so every other pre-reset fd gets `-ENODEV` from all subsequent ioctls/mmaps (chardev.c:604-624, 726-729). The driver does **not** clear the device TLB allocation bitmap, the per-fd ownership bitmaps, or the hardware window registers on reset — stale owners keep their ids reserved until they close, and re-init (`init_hardware`) does not touch user TLB registers. During the `needs_hw_init` window only GET_DEVICE_INFO / GET_DRIVER_INFO / RESET_DEVICE are allowed; ALLOCATE/CONFIGURE/FREE_TLB return `-ENODEV` (chardev.c:615-624).

**Device removal** (`tenstorrent_pci_remove`, enumerate.c:404-481): sets `detached` under `chardev_mutex`, drains ioctls via `down_write(&reset_rwsem)`, zaps all VMAs, unmaps BARs (`cleanup_device` → `pci_iounmap` of `tlb_regs`/`kernel_tlb` etc., blackhole.c:710-722, wormhole.c:793-801), then revokes TLB dma-buf exports (enumerate.c:444-459; ordering rationale in the comment). Open fds survive with `-ENODEV` semantics; their TLB bookkeeping is torn down at their eventual close.

**ioctl dispatch** for the four TLB calls is at chardev.c:670-680 (ALLOCATE/FREE/CONFIGURE) and chardev.c:690-692 (EXPORT), all under `reset_rwsem` shared with the detached/reset-gen/needs_hw_init gates above.

> **Porting note:** The per-fd bitmap + release-time sweep maps cleanly onto a KMDF file-object context cleaned up in `EvtFileCleanup`/`EvtFileClose`. The reset-generation gating (fd permanently invalid after reset) and the "zap all user mappings on reset" behavior are load-bearing for safety: a Windows port must be able to cut off user MMIO access before resetting (unmap tracked user mappings, or refuse reset while mappings exist).

---

## Key constants table

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

## Open questions

1. **Hardware TLB register state after ASIC reset.** The driver never clears or reprograms user TLB config registers on reset or free; whether the hardware resets them to a benign default after `ASIC_RESET`/`ASIC_DMC_RESET` is not observable in this code. A Windows port should not assume registers are cleared (and per blackhole.c:190-195, out-of-band strided configuration can persist and is only scrubbed by the next CONFIGURE_TLB on windows 0-31).
2. **Wormhole ignores `static_vc`.** `tenstorrent_noc_tlb_config.static_vc` is silently dropped on WH (no field in `noc_tlb_non_address_bits`, wormhole.c:809-823) but drives `use_static_vc` on BH (blackhole.c:184). It is unclear whether WH hardware has an equivalent bit that the driver chooses not to expose; the port should replicate the drop-silently behavior for compatibility.
3. **Semantics of `ordering` values.** The kernel only documents `1 = strict` (comments wormhole.c:924, blackhole.c:236); the meaning of 0/2/3 is defined by NOC hardware and by tt-umd's `tlb_data` enums, not by tt-kmd. The port should treat the field as an opaque 2-bit passthrough.
4. **BAR mappings bypass TLB ownership.** QUERY_MAPPINGS + mmap of RESOURCE0/2 (BAR0/BAR4) exposes every TLB data window *and* the TLB config register file to any fd without ownership checks (memory.c:1596-1618), and tt-umd's `SiliconTlbHandle::configure` relies on this to poke registers from user space. A Windows port must decide whether to reproduce this open-BAR model (required for unmodified UMD behavior) or force all configuration through the CONFIGURE_TLB path.
5. **`blackhole_init` BAR2 error handling.** The failure check at blackhole.c:592 tests `tlb_regs`/`kernel_tlb`/`noc2axi_cfg` but not `bar2_mapping`; a failed BAR2 iomap proceeds and later iATU writes would dereference NULL (blackhole.c:104-110). Presumably a latent bug rather than intent; a port should treat BAR2 mapping failure as fatal.
6. **Exact-size ALLOCATE contract.** `tenstorrent_device_allocate_tlb` requires `size` to exactly match a pool size (tlb.c:24) — no rounding up. This appears intentional (UMD always passes exact sizes) but is worth confirming as a frozen uAPI behavior before the Windows ioctl surface copies it.
7. **`tlb_counts` divergence between allocator and `describe_tlb` on BH.** With a small BAR4, ids 202+`tlb_counts[1]`..209 are unallocatable yet `blackhole_describe_tlb` still describes them (blackhole.c:743 uses the compile-time total). Harmless on Linux (nothing reaches describe with an unallocated id except the mmap search loop, which is bounded by per-device counts, memory.c:1516-1523), but a port restructuring this code should keep the allocator bound authoritative.
