# IOCTL Parity Matrix

Windows ABI parity with tt-kmd (`ioctl.h`, baseline `ttkmd-2.10.0-rc1-1-g8c32c2b`).
Struct sizes/offsets are gcc x86_64 ground truth from `docs/abi-ground-truth.txt`
(regenerate with `tools/gen_abi_truth.sh`); the Windows header must `static_assert`
identical values.

**CTL_CODE scheme (DD-4):** `CTL_CODE(0x80FA, 0x800 + <linux nr>, METHOD_BUFFERED, FILE_ANY_ACCESS)`
→ code = `0x80FA2000 + 4·nr`.

**Buffering:** METHOD_BUFFERED everywhere. Justification: all structs are small
(≤ 64 bytes fixed part; QUERY_MAPPINGS out is `count × 24` bytes, ≤ 6 entries in
practice); double-buffering cost is negligible and it gives the strongest
untrusted-pointer hygiene. Bulk data NEVER moves through these ioctls (it moves
through mappings), so METHOD_NEITHER/DIRECT buys nothing. PIN_PAGES/UNPIN_PAGES pass
a user VA *by value* inside the struct — the driver probes/locks it explicitly
(`MmProbeAndLockPages` in the handler), which is the Linux-equivalent semantic
(`pin_user_pages`), not an ioctl-buffering concern.

**Baseline status-code mapping** (per-row deviations noted in Notes):

| Linux errno | NTSTATUS |
|---|---|
| -EINVAL | STATUS_INVALID_PARAMETER |
| -EFAULT | STATUS_ACCESS_VIOLATION |
| -ENOMEM | STATUS_INSUFFICIENT_RESOURCES |
| -ENODEV | STATUS_DEVICE_REMOVED |
| -EBUSY | STATUS_DEVICE_BUSY |
| -EPERM / -EACCES | STATUS_ACCESS_DENIED |
| -EINTR | STATUS_CANCELLED |
| -EAGAIN | STATUS_RETRY |
| -ETIMEDOUT | STATUS_IO_TIMEOUT |
| -ENOSPC | STATUS_INSUFFICIENT_RESOURCES (note per-row if distinct meaning matters) |
| -EOVERFLOW | STATUS_BUFFER_OVERFLOW |
| -ENOTTY (bad ioctl nr) | STATUS_INVALID_DEVICE_REQUEST |

**Parity status legend:** `not-started` → `stub` → `functional` → `tested`.

## Linux ioctls

| nr | Linux name | Win CTL_CODE | Struct (size B: in/out/total) | Method | Status mapping notes | Parity |
|---|---|---|---|---|---|---|
| 0 | GET_DEVICE_INFO | `0x80FA2000` | `tenstorrent_get_device_info` (4/20/24) | BUFFERED | baseline | **tested** (M1, ttinfo vs ttsim) |
| 1 | GET_HARVESTING | `0x80FA2004` | no struct; Linux has NO handler — falls to default -EINVAL (chardev.c:631-632, 694-696) | BUFFERED | -EINVAL → STATUS_INVALID_PARAMETER | **tested** (stub parity asserted by ttinfo) |
| 2 | QUERY_MAPPINGS | `0x80FA2008` | `tenstorrent_query_mappings` (8/N×24/8+flex) | BUFFERED | packed UC/WC pairs for existing BARs; min(count,valid) copied + zero-fill (memory.c:393-406) | **tested** (M1, incl. count 0/2/6/16) |
| 3 | ALLOCATE_DMA_BUF | `0x80FA200C` | `tenstorrent_allocate_dma_buf` (24/40/64) | BUFFERED | baseline | not-started |
| 4 | FREE_DMA_BUF | `0x80FA2010` | `tenstorrent_free_dma_buf` (0/0/0 — GNU empty struct, illegal in MSVC; Windows defines NO struct, zero-length buffers) | BUFFERED | semantics from analysis §04 (likely -EINVAL/unsupported upstream) | not-started |
| 5 | GET_DRIVER_INFO | `0x80FA2014` | `tenstorrent_get_driver_info` (4/12/16) | BUFFERED | baseline | **tested** (M1) |
| 6 | RESET_DEVICE | `0x80FA2018` | `tenstorrent_reset_device` (8/8/16) | BUFFERED | -EBUSY while TLB dmabuf export live → STATUS_DEVICE_BUSY | not-started |
| 7 | PIN_PAGES | `0x80FA201C` | `tenstorrent_pin_pages` (24/8/32); extended out `tenstorrent_pin_pages_out_extended` (16) via output_size_bytes | BUFFERED | baseline; user VA probed via MmProbeAndLockPages | not-started |
| 8 | LOCK_CTL | `0x80FA2020` | `tenstorrent_lock_ctl` (12/4/16) | BUFFERED | ACQUIRE_BLOCKING pends the IRP; cancel → STATUS_CANCELLED (mirrors -EINTR) | not-started |
| 9 | MAP_PEER_BAR | `0x80FA2024` | `tenstorrent_map_peer_bar` (24/16/40) | BUFFERED | `peer_fd` (u32) carries a Windows HANDLE value of the peer device file; resolved via ObReferenceObjectByHandle — 64-bit handle truncation concern, see OQ (to file) | not-started |
| 10 | UNPIN_PAGES | `0x80FA2028` | `tenstorrent_unpin_pages` (24/0/24) | BUFFERED | baseline | not-started |
| 11 | ALLOCATE_TLB | `0x80FA202C` | `tenstorrent_allocate_tlb` (16/32/48) | BUFFERED | baseline | not-started |
| 12 | FREE_TLB | `0x80FA2030` | `tenstorrent_free_tlb` (4/0/4) | BUFFERED | baseline | not-started |
| 13 | CONFIGURE_TLB | `0x80FA2034` | `tenstorrent_configure_tlb` (40/8/48); `tenstorrent_noc_tlb_config` = 32 B | BUFFERED | baseline | not-started |
| 14 | SET_NOC_CLEANUP | `0x80FA2038` | `tenstorrent_set_noc_cleanup` (32, argsz protocol) | BUFFERED | baseline | not-started |
| 15 | SET_POWER_STATE | `0x80FA203C` | `tenstorrent_power_state` (40, argsz protocol) | BUFFERED | baseline | not-started |
| 16 | EXPORT_TLB_DMABUF | `0x80FA2040` | `tenstorrent_export_tlb_dmabuf` (32, argsz protocol) | BUFFERED | **design-pending:** dma-buf fd has no Windows equivalent; Linux consumer is RDMA P2P (`ibv_reg_dmabuf_mr`). Candidate: not supported initially → STATUS_NOT_SUPPORTED, revisit if tt-umd-on-Windows needs it | not-started |

## Windows-only extensions (no Linux nr; function codes from 0x900)

| Win name | CTL_CODE | Purpose | Linux construct replaced | Parity |
|---|---|---|---|---|
| IOCTL_TENSTORRENT_MAP | `CTL_CODE(0x80FA, 0x900, ...)` = `0x80FA2400` | Map a BAR range/DMA buf/TLB window into caller VA; input = the `mapping_base`/`mmap_offset_*` value from QUERY_MAPPINGS / ALLOCATE_DMA_BUF / ALLOCATE_TLB + size; output = user VA | `mmap(fd, offset)` | design-pending (M0 analysis §03 mmap offset encoding) |
| IOCTL_TENSTORRENT_UNMAP | `0x80FA2404` | Unmap a prior MAP by VA | `munmap` | design-pending |
| IOCTL_TENSTORRENT_SET_CLIENT_FLAGS | `0x80FA2408` | Declares power-aware client immediately after open | `open(..., O_APPEND)` | design-pending (analysis §03) |
| IOCTL_TENSTORRENT_QUERY_STABLE_ID | `0x80FA240C` | ASIC-ID-based stable identity | `/dev/tenstorrent/by-id/*` udev symlinks | design-pending (analysis §01 naming scheme) |

**Rules:** rows change status only in the same commit as the code they describe.
Every `design-pending` must resolve to a documented design (DD-N) before its
milestone completes. GET_HARVESTING's struct is not defined in `ioctl.h` — filled
in from the analysis (chardev.c handler) before M1 code.
