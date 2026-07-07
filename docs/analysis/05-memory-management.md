# 05. Memory Management

## Scope

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

## 1. Per-fd bookkeeping structures

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

## 2. The mmap offset encoding

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

### Dispatch (`tenstorrent_mmap`, memory.c:1585-1636)

`vma_target_range()` (memory.c:1330-1342) checks that the requested window (start `vm_pgoff << PAGE_SHIFT`, length `vm_end − vm_start`) is fully contained in an entity's slot and, on match, **rewrites `vma->vm_pgoff` to be relative to the entity start** (memory.c:1337). Dispatch order: BAR0 UC/WC, BAR1(=BAR2) UC/WC, BAR2(=BAR4) UC/WC, TLB UC/WC, else DMA buffer, else `-EINVAL` (memory.c:1596-1635). Cache attributes are applied via `pgprot_device()` (UC) or `pgprot_writecombine()` (memory.c:1597-1626).

- **BAR mappings** (`map_pci_bar`, memory.c:1441-1475): `vm_iomap_memory(vma, bar_start, bar_len)` — the (rewritten) `vm_pgoff` becomes an offset within the BAR; a `tenstorrent_mmap_vma` tracking record (type `TT_VMA_BAR`) is added to `priv->vma_list` under `vma_lock`, and `vm_ops = &bar_vma_ops` installs open/close callbacks. Errors: `-ENOMEM` on tracking alloc failure, `vm_iomap_memory`'s error otherwise.
- **TLB window mappings** (`map_tlb_window`, memory.c:1494-1583): offset inside the TLB slot encodes the window: it equals the window's BAR0 offset, except windows in Blackhole BAR4 are encoded at `bar_offset + BAR0_SIZE` where `BAR0_SIZE = 1UL << 29` (memory.c:29, 926-931, 1503, 1519-1520). The code linearly scans all windows via `describe_tlb` to find a `bar_offset`/BAR match (memory.c:1523-1531). Checks: `describe_tlb` exists and `tlb_kinds != 0` else `-EINVAL` (1510-1514); window found else `-EINVAL` (1533-1534); `size <= tlb_desc.size` else `-EINVAL` (1536-1537); caller owns the window (`test_bit(id, priv->tlbs)`) else `-EPERM` (1541-1544); window inside BAR else `-ENXIO` (1547-1550); `io_remap_pfn_range` failure gives `-EAGAIN` (1561-1565). VMA is tracked as `TT_VMA_TLB` with the window id. TLB VMAs forbid splitting: `.may_split` (or `.split` pre-5.11) returns `-EINVAL` (memory.c:1478-1492).
- **DMA buffer mappings** (`vma_dmabuf_target`, memory.c:1344-1368): index = `(vm_pgoff − MMAP_OFFSET_DMA_BUF/PAGE_SIZE) / (MMAP_SIZE_DMA_BUF/PAGE_SIZE)`; must be `< TENSTORRENT_MAX_DMA_BUFS`; buffer must exist for this fd; requested range must fit within the *allocated* size (not the 4 GiB slot). The map is performed by `dma_mmap_coherent(&pdev->dev, vma, dmabuf->ptr, dmabuf->phys, dmabuf->size)` (memory.c:1629-1632). DMA-buffer VMAs are **not** tracked in `vma_list` and get no vm_ops (so they are not zapped on reset, and there is no per-VMA bookkeeping — safety comes from the mapping refcounting the struct file, per the comment at memory.c:518-521).

### VMA lifecycle tracking

`tenstorrent_vma_open` (fork) duplicates the tracking record for the child VMA; if allocation fails it zaps the child's PTEs and orphans the VMA (`vm_private_data = NULL`) rather than fail fork (memory.c:1370-1410). `tenstorrent_vma_close` unlinks and frees the record under `vma_lock` (memory.c:1412-1434). `tenstorrent_vma_zap()` (memory.c:1677-1743) walks every open fd of a device and zaps all tracked BAR/TLB VMAs with `zap_special_vma_range` (`zap_vma_ptes` pre-7.1, memory.c:34-37), used after reset so userspace cannot touch dead device memory. Lock ordering is critical and documented: `mmap_lock` before `vma_lock` (memory.c:1683-1689); the walk takes an `mmget_not_zero` reference per mm, drops `vma_lock`, takes `mmap_read_lock`, re-takes `vma_lock`, zaps all VMAs of that mm, using `list_del_init` so a concurrent `vma_close` is safe (memory.c:1690-1739).

> **Porting note:** Windows has no mmap-offset multiplexing and no VMA callbacks. The equivalent design is ioctl-driven mapping: map BAR/TLB apertures into user space with `MmMapIoSpaceEx` + `MmMapLockedPagesSpecifyCache`(UserMode) or a section object, with UC vs WC as the cache type (`MmNonCached`/`MmWriteCombined`). The port must reproduce: (a) per-fd ownership checks before mapping a TLB window, (b) the no-split property (Windows user mappings can't be partially unmapped, so this is free), (c) revocation on reset — hardest part; there is no `zap_vma_ptes` equivalent, so the port either tracks and force-unmaps (`MmUnmapLockedPages` requires process context) or avoids the problem by failing reset while mappings exist. (d) fork does not exist on Windows; child-inheritance logic can be dropped.

---

## 3. QUERY_MAPPINGS (memory.c:331-409)

Returns up to 6 `tenstorrent_mapping` records (BARs 0/2/4 × UC/WC), each with the hard-coded `mapping_base` offsets above and `mapping_size = pci_resource_len(...)`; BARs with zero length are omitted (memory.c:352-389). The output array is written through a local flexible-array view of the user struct to keep UBSAN quiet (memory.c:323-329). If the user asked for more mappings than exist, the excess entries are zeroed with `clear_user`; a `U32_MAX / sizeof(struct tenstorrent_mapping) < extra_mappings_to_clear` overflow check guards the multiply (memory.c:393-406). Errors: `-EFAULT` only. No locks taken. TLB window offsets are *not* reported here; they come from ALLOCATE_TLB's `mmap_offset_uc/wc` (memory.c:933-934).

---

## 4. DMA buffer allocation (ALLOCATE_DMA_BUF, memory.c:425-513)

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

## 5. NOC DMA and the outbound iATU allocator

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

## 6. PIN_PAGES end to end (memory.c:544-744)

Flags (ioctl.h:168-172):

```c
#define TENSTORRENT_PIN_PAGES_CONTIGUOUS 1	// app attests that the pages are physically contiguous
#define TENSTORRENT_PIN_PAGES_NOC_DMA 2		// app wants to use the pages for NOC DMA
#define TENSTORRENT_PIN_PAGES_NOC_TOP_DOWN 4	// NOC DMA will be allocated top-down (default is bottom-up)
#define TENSTORRENT_PIN_PAGES_READ_ONLY 8	// device will only read; IOMMU enforced, requires IOMMU translation
```

`CONTIGUOUS` is accepted in `valid_flags` (memory.c:547-548) but never otherwise read — contiguity is *always* verified, the flag is vestigial. Note that `noc_dma` is triggered by *either* NOC_DMA *or* NOC_TOP_DOWN (`noc_dma = in.flags & (TENSTORRENT_PIN_PAGES_NOC_DMA | TENSTORRENT_PIN_PAGES_NOC_TOP_DOWN)`, memory.c:584), so TOP_DOWN alone implies a NOC mapping.

### Validation
1. `copy_from_user` → `-EFAULT` (memory.c:569-570).
2. `clear_user(&arg->out, in.output_size_bytes)` up front → `-EFAULT` (memory.c:572-573). This both validates writability and zero-fills for forward-compat sizing.
3. Unknown flags → `-EINVAL` (memory.c:575-576).
4. `!PAGE_ALIGNED(virtual_address) || !PAGE_ALIGNED(size) || size == 0` → `-EINVAL` (memory.c:578-579).
5. `is_pin_pages_size_safe`: on kernels ≤ 5.4 caps size at `1 << 30` (1 GiB) due to an IOMMU-unmap soft-lockup; on modern kernels always true (memory.c:532-542, 581-582).
6. `READ_ONLY` without IOMMU translation → `-EOPNOTSUPP` (memory.c:588-589). `is_iommu_translated()` = domain exists and is not `IOMMU_DOMAIN_IDENTITY` (memory.c:526-530). Rationale: read-only can only be *enforced* by the IOMMU; there is no way to make host RAM read-only to a bus master otherwise.
7. Under `priv->mutex`: duplicate (VA, page_count) pinning → `-EEXIST` — prevents UNPIN ambiguity over iATU teardown (memory.c:594-605).

### Pinning
`gup_flags = read_only ? 0 : FOLL_WRITE`; DMA direction `DMA_TO_DEVICE` if read-only else `DMA_BIDIRECTIONAL` (memory.c:591-592). Page-pointer array is `vzalloc`'d (`nr_pages = PAGE_ALIGN(size) >> PAGE_SHIFT`, memory.c:613-614). Pages are pinned with `pin_user_pages_fast(start, nr_pages, gup_flags | FOLL_LONGTERM, pages)` (memory.c:202-207; ≥5.11 path — older kernels fall back to `pin_user_pages`/`get_user_pages` with a temporary vmas array, memory.c:208-242). **FOLL_LONGTERM** is essential: these are indefinite-duration pins. A negative return propagates; a *partial* pin (`pages_pinned != nr_pages`) is `-EINVAL` after unpinning what was pinned (memory.c:621-632).

### IOMMU path (memory.c:634-684)
- Builds a chained sg_table with `alloc_chained_sgt_for_pages` (see §9); failure → `-ENOMEM`.
- `dma_map_sgtable(&pdev->dev, &dma_mapping, dir, 0)` maps it into the device's IOMMU domain (memory.c:647). Note the driver never calls `iommu_map()` directly; it relies on the DMA-IOMMU layer, then **verifies** the resulting IOVA range is contiguous and complete: iterates `for_each_sgtable_dma_sg`, requiring each entry to start where the previous ended, and total mapped length == `nr_pages * PAGE_SIZE`; any violation → `-EINVAL` with `debug_print_sgtable` diagnostics (memory.c:654-674). The comment says a discontiguous mapping "can only happen due to a misconfiguration or a bug" (memory.c:654).
- `out.physical_address = sg_dma_address(dma_mapping.sgl)` — the base **IOVA** (memory.c:676).
- READ_ONLY enforcement is implicit in `dir = DMA_TO_DEVICE`: the IOMMU PTEs are created without write permission.

### Direct (no-IOMMU) path (memory.c:685-705)
- Requires the pinned pages to be **physically contiguous**: `page_to_pfn(pages[i]) != page_to_pfn(pages[i-1]) + 1` → `-EINVAL` (memory.c:688-694). No sg_table is built (`dma_mapping` stays zeroed).
- `out.physical_address = page_to_phys(pages[0])` — a raw **CPU physical address** handed to userspace (memory.c:696).

### NOC DMA
If requested, `setup_noc_dma(priv, top_down, in.size, out.physical_address, &noc_address)` — top-down vs bottom-up per the flag (default bottom-up, memory.c:585) — and `out.noc_address` is returned (memory.c:678-684, 698-704, 707).

### Output and bookkeeping
`bytes_to_copy = min(in.output_size_bytes, sizeof(out))` where `out` is `tenstorrent_pin_pages_out_extended` {physical_address, noc_address} (memory.c:565, 708-710; ioctl.h:185-188) — old callers passing the short struct only get `physical_address`. The pinning record is added to `priv->pinnings` and `priv->mutex` released (memory.c:715-723).

### Error unwinding (memory.c:727-743)
Reverse-order labels: teardown iATU (no-op if −1) → `dma_unmap_sgtable` → `free_chained_sgt` (both no-ops on the zeroed table from the direct path) → `unpin_user_pages_dirty_lock(pages, pages_pinned, false)` (not dirtied on failure) → `vfree(pages)` → `kfree(pinning)`; `priv->mutex` released at the end.

### UNPIN_PAGES (memory.c:746-780)
Input {virtual_address, size, reserved} (ioctl.h:191-195; "original VA used to pin, not current VA if remapped"). Validation: `reserved != 0 || size == 0 || (size >> PAGE_SHIFT) == 0` → `-EINVAL`. Under `priv->mutex`, finds a pinning with matching VA; a VA match with mismatched page count is `-EINVAL`; no match at all is `-EINVAL`; partial unpin is unsupported (ioctl.h:190). On match, `unpin_pinned_page_range()` (memory.c:299-314): tears down the iATU region, `dma_unmap_sgtable` (direction per `read_only`), frees the chained sgt, `unpin_user_pages_dirty_lock(pages, count, !read_only)` — **pages are marked dirty unless the pin was read-only** — then `vfree`/`list_del`/`kfree`. The same function runs for every leftover pinning at fd release (memory.c:1656-1658).

> **Porting note:** The Windows analogue of pin_user_pages(FOLL_LONGTERM) is `MmProbeAndLockPages` on an MDL (IoWriteAccess unless read-only), held for the object's lifetime, plus `MmGetSystemAddressForMdlSafe`-free DMA via `DMA_ADAPTER` or direct PFN use. The two Linux paths translate as: (a) no-IOMMU path = require physically contiguous locked pages (or have UMD allocate via the driver instead), exposing `MmGetPhysicalAddress` of the first page; (b) IOMMU path — Windows offers no general driver-controlled IOMMU domain for arbitrary remapping on client SKUs; DMA remapping (kernel DMA protection) is opaque to drivers. A practical port either requires contiguous memory (large pages / driver-allocated buffers) or uses `GetDmaTransferInfo`/DMA v3 with scatter-gather only if the device could scatter-gather — it cannot: the device needs one contiguous bus range per pinning (that is the whole point of the contiguity check). Expect to drop READ_ONLY (-EOPNOTSUPP equivalent: STATUS_NOT_SUPPORTED) unless running with a controllable remapping layer. The dirty-marking on unpin corresponds to `MmUnlockPages` (Windows dirties automatically for write-locked MDLs).

---

## 7. MAP_PEER_BAR (memory.c:782-891)

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

## 8. EXPORT_TLB_DMABUF (memory.c:1001-1326)

Only compiled for kernels ≥ 5.8; otherwise the ioctl returns `-EOPNOTSUPP` and the revoke/has-exports helpers are no-ops (memory.c:1310-1326). Requires `MODULE_IMPORT_NS(DMA_BUF)` (memory.c:1004-1010).

Exports a TLB window's BAR aperture (a sub-range of it) as a dma-buf fd so other drivers (e.g. an RDMA NIC) can DMA directly into the window (P2P). ABI struct: {argsz, flags, tlb_id, fd (out), offset, size} (ioctl.h:449-456).

### Export (`ioctl_export_tlb_dmabuf`, memory.c:1146-1279)
Validation: `argsz != sizeof(in)` → `-EINVAL`; `flags != 0` → `-EINVAL`; `tlb_id >= TENSTORRENT_MAX_INBOUND_TLBS` (= 256, ioctl.h:42) → `-EINVAL`; `offset`/`size` page-aligned → `-EINVAL`; `describe_tlb` must exist (memory.c:1161-1177). Under `priv->mutex`: caller must own the window (`test_bit` in `priv->tlbs`) → `-EPERM` (memory.c:1180-1184); `offset >= tlb_desc.size || size > tlb_desc.size` → `-EINVAL`; `size == 0` means "to end of window" (memory.c:1193-1204); the region must lie within the BAR → `-EINVAL` (memory.c:1207-1211). `phys = pci_resource_start(bar) + bar_offset + offset` (memory.c:1213).

Before `dma_buf_export`, the driver takes **two references that live as long as the dma-buf**: `kref_get(&tt_dev->kref)` (device) and `tenstorrent_tlb_export_get(tt_dev, tlb_id)` (window) (memory.c:1227-1234). This is what makes the export survive FREE_TLB and close() of the owning fd — the window cannot return to the pool and be reprogrammed to redirect a live importer's DMA (ioctl.h doc, ioctl.h:434-438). The export record is added to the device-global `tt_dev->dmabuf_exports` list under `dmabuf_export_lock` (memory.c:1252-1254). fd creation: `get_unused_fd_flags(O_CLOEXEC)`, `copy_to_user` of the fd, then `fd_install` (memory.c:1258-1273); failures drop the dma-buf (whose release callback balances the two references).

### dma_buf ops (memory.c:1137-1144)
- **attach**: rejects importers without `peer2peer` support with `-EOPNOTSUPP` — there are no backing struct pages, only a PCI BAR (memory.c:1023-1031).
- **map**: asserts `dma_resv` held; refuses if `revoked` with `-ENODEV`; maps the whole region into the **importer's** DMA domain with one `dma_map_resource(attach->dev, phys, size, dir, 0)` call, then describes the contiguous IOVA using page-less sg entries split into `TT_TLB_DMABUF_SG_CHUNK = SZ_1G` chunks because `sg_dma_len` is 32-bit (4 GiB truncates to 0) (memory.c:1033-1090). Entry 0 holds the base address for unmap.
- **unmap**: `dma_unmap_resource` on entry 0's address for the full size, free table (memory.c:1092-1099).
- **pin / unpin**: `pin` only refuses already-revoked exports (`-ENODEV`); pinned importers are otherwise accepted (memory.c:1101-1116). Both move_notify-capable and pin-only importers work (ioctl.h:425-427).
- **release**: unlinks from `dmabuf_exports`, drops the window export ref (`tenstorrent_tlb_export_put` — returns the window to the pool if the owning fd is already gone) and the device ref (memory.c:1118-1135).

### Revocation and RESET_DEVICE interaction
`tenstorrent_revoke_tlb_dmabufs()` walks the export list, and under each buffer's `dma_resv` lock sets `revoked = true` and calls `dma_buf_invalidate_mappings` (`dma_buf_move_notify` pre-7.1) (memory.c:1281-1297, 34-37). But a **pin-only importer cannot be revoked**, so destructive resets (RESET_PCIE_LINK / CONFIG_WRITE / USER_RESET / ASIC_RESET / ASIC_DMC_RESET) are refused with `-EBUSY` while any export is live, via `tenstorrent_has_tlb_dmabuf_exports()` (memory.c:1299-1308; chardev.c:222-231, the check at chardev.c:228-229). Rationale: resetting under in-flight P2P DMA "can wedge the host hard enough to require out-of-band recovery" (ioctl.h:429-432).

> **Porting note:** dma-buf has no Windows equivalent for arbitrary cross-driver BAR sharing. Options: drop the ioctl (return STATUS_NOT_SUPPORTED, matching the pre-5.8 behavior), or design something on D3DKMT shared resources / NT handles if a concrete consumer exists. Whatever the choice, the port must preserve the *invariant* the feature encodes: a TLB window that any external agent may be DMAing into must not be freed, reprogrammed, or reset under it — the `-EBUSY`-on-reset and window-refcount semantics are the load-bearing part.

---

## 9. sg_helpers (sg_helpers.c, sg_helpers.h)

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

## 10. Where physical addresses / IOVAs cross the user boundary

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

## Key constants table

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

## Open questions

1. **MAP_PEER_BAR keeps no reference on the peer device.** `fput(peer_file)` runs before the ioctl returns (memory.c:875) and nothing pins the peer `tenstorrent_device`; the `dma_map_resource` mapping into the local device's domain persists until the *local* fd closes (memory.c:1660-1665). If the peer device is unbound/removed while the mapping lives, the local device holds a bus address into a possibly reassigned BAR. It is unclear whether this is intentional (relying on both devices sharing driver lifetime) — a Windows port must decide whether to reference the peer device object.
2. **Cleanup ordering for NOC-DMA buffers**: `tenstorrent_memory_cleanup` calls `dma_free_coherent` *before* `teardown_outbound_iatu` (memory.c:1650-1651), so the outbound iATU window briefly targets freed memory. `ioctl_allocate_dma_buf`'s error path does it in the opposite (safe) order (memory.c:499-501). Probably harmless (nothing should be issuing NOC traffic at fd close), but a port should tear down the aperture first.
3. **`TENSTORRENT_PIN_PAGES_CONTIGUOUS` is dead**: accepted in `valid_flags` (memory.c:547) but never examined; contiguity is always enforced on the non-IOMMU path and IOVA-contiguity on the IOMMU path. Should the Windows ABI keep, require, or reject it?
4. **NOC_TOP_DOWN implies NOC_DMA**: `noc_dma` is true if *either* flag is set (memory.c:584). Is TOP_DOWN-without-NOC_DMA a supported combination or an accident of the mask? The port should document/normalize this.
5. **`MMAP_OFFSET_DMA_BUF` is PAGE_SIZE-dependent** (memory.c:272): the constant differs on non-4K-page kernels. The Windows port defines its own handle/offset scheme, but any ABI-compatibility shim for tt-umd must use the 4 KiB-page value (0xF00_0000_0000).
6. **DMA-buffer VMAs are untracked**: mappings created through `dma_mmap_coherent` are not added to `vma_list` and are not zapped by `tenstorrent_vma_zap()` (memory.c:1629-1632 vs 1677-1743). Presumably safe because the buffer is host RAM, not device BAR — but confirms that reset does not revoke user access to DMA buffers; the port can mirror this.
7. **IOVA contiguity is assumed, verified, and fatal if violated** (memory.c:654-674). On Linux the dma-iommu allocator happens to produce one contiguous IOVA per sgtable map. Whether an equivalent guarantee exists on any Windows remapping path is a core feasibility question for supporting non-contiguous user buffers at all.
8. **`ioctl_free_dma_buf` returns `-EINVAL` always** (memory.c:515-523). The ABI reserves the ioctl; the port must decide whether to implement real freeing (Windows can track section mappings, so the blocking problem Linux cites may be solvable) or preserve the stub for UMD compatibility.
