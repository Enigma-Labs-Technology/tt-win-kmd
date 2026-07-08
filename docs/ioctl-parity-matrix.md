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
| 3 | ALLOCATE_DMA_BUF | `0x80FA200C` | `tenstorrent_allocate_dma_buf` (24/40/64) | BUFFERED | WdfCommonBuffer; validation order per memory.c:439-458; iATU-before-free | **tested** (M3) |
| 4 | FREE_DMA_BUF | `0x80FA2010` | no struct (zero-length buffers) | BUFFERED | upstream stub: unconditional -EINVAL (memory.c:515-523) | **tested** (M3) |
| 5 | GET_DRIVER_INFO | `0x80FA2014` | `tenstorrent_get_driver_info` (4/12/16) | BUFFERED | baseline | **tested** (M1) |
| 6 | RESET_DEVICE | `0x80FA2018` | `tenstorrent_reset_device` (8/8/16) | BUFFERED | 7-flavor dispatch; out.result=!ok; gen-bump → STATUS_DEVICE_REMOVED on stale handles; needs_hw_init window; VMA-zap (DD-9) | **tested** (M4) |
| 7 | PIN_PAGES | `0x80FA201C` | `tenstorrent_pin_pages` (24/8/32); extended out (16) via output_size_bytes | BUFFERED | MmProbeAndLockPages + PFN-contiguity (direct path); READ_ONLY→NOT_SUPPORTED (no driver IOMMU domain, DD-8) | **functional** (M3; direct path tested, RO negative) |
| 8 | LOCK_CTL | `0x80FA2020` | `tenstorrent_lock_ctl` (12/4/16) | BUFFERED | 64 locks; ACQUIRE_BLOCKING = cancellable pended request (manual WDFQUEUE); stale-gen/detached waiter → STATUS_DEVICE_REMOVED; CancelSynchronousIo → STATUS_CANCELLED | **tested** (M5) |
| 9 | MAP_PEER_BAR | `0x80FA2024` | `tenstorrent_map_peer_bar` (24/16/40) | BUFFERED | `peer_fd` (u32) carries a Windows HANDLE value of the peer device file; resolved via ObReferenceObjectByHandle — 64-bit handle truncation concern, see OQ (to file) | not-started |
| 10 | UNPIN_PAGES | `0x80FA2028` | `tenstorrent_unpin_pages` (24/0/24) | BUFFERED | exact VA+size match; MmUnlockPages dirties write-locked pages | **functional** (M3) |
| 11 | ALLOCATE_TLB | `0x80FA202C` | `tenstorrent_allocate_tlb` (16/32/48) | BUFFERED | exact-size pool (no round-up); single-owner; token encoding memory.c:924-934 | **tested** (M3) |
| 12 | FREE_TLB | `0x80FA2030` | `tenstorrent_free_tlb` (4/0/4) | BUFFERED | -EPERM if not owner, -EBUSY if mapped (memory.c:955-976) | **tested** (M3) |
| 13 | CONFIGURE_TLB | `0x80FA2034` | `tenstorrent_configure_tlb` (40/8/48); config 32 B | BUFFERED | -EPERM if not owner; 2M/4G register composition (blackhole.c:112-198) | **tested** (M3) |
| 14 | SET_NOC_CLEANUP | `0x80FA2038` | `tenstorrent_set_noc_cleanup` (32, argsz protocol) | BUFFERED | per-handle NOC write at close (skipped if detached); argsz/noc/addr-align/x-y validation | **tested** (M6) |
| 15 | SET_POWER_STATE | `0x80FA203C` | `tenstorrent_power_state` (40, argsz protocol) | BUFFERED | multi-client aggregate (flags OR + unspecified-ON, settings max) → ARC POWER_SETTING; argsz/flags/validity validation | **tested** (M5) |
| 16 | EXPORT_TLB_DMABUF | `0x80FA2040` | `tenstorrent_export_tlb_dmabuf` (32, argsz protocol) | BUFFERED | **design-pending:** dma-buf fd has no Windows equivalent; Linux consumer is RDMA P2P (`ibv_reg_dmabuf_mr`). Candidate: not supported initially → STATUS_NOT_SUPPORTED, revisit if tt-umd-on-Windows needs it | not-started |

## Windows-only extensions (no Linux nr; function codes from 0x900)

| Win name | CTL_CODE | Purpose | Linux construct replaced | Parity |
|---|---|---|---|---|
| IOCTL_TENSTORRENT_MAP | `0x80FA2400` | Map a BAR range/DMA buf/TLB window into caller VA (token + byte offset + length → user VA) | `mmap(fd, offset)` | **tested** (M3, DD-8) |
| IOCTL_TENSTORRENT_UNMAP | `0x80FA2404` | Unmap a prior MAP by VA | `munmap` | **tested** (M3, DD-8) |
| IOCTL_TENSTORRENT_QUERY_TELEMETRY | `0x80FA2408` | hwmon-equivalent telemetry (exact Linux names/units/scaling; present-mask) | sysfs/hwmon nodes | **tested** (M5, DD-10) |
| IOCTL_TENSTORRENT_SET_CLIENT_FLAGS | `0x80FA2408` | Declares power-aware client immediately after open | `open(..., O_APPEND)` | design-pending (analysis §03) |
| IOCTL_TENSTORRENT_QUERY_STABLE_ID | `0x80FA240C` | ASIC-ID-based stable identity | `/dev/tenstorrent/by-id/*` udev symlinks | design-pending (analysis §01 naming scheme) |

## Debug-only ioctls (function codes from 0xA00; compiled only with TT_DEBUG_INTERFACES)

| Win name | CTL_CODE | Purpose | Linux construct replaced | Parity |
|---|---|---|---|---|
| IOCTL_TENSTORRENT_DEBUG_READ_TELEMETRY | `0x80FA2800` | Read one telemetry tag (errno parity with read_telemetry_tag: ≥128 → INVALID_PARAMETER, absent → NOT_FOUND) | debugfs/telemetry inspection | **tested** (M2) |
| IOCTL_TENSTORRENT_DEBUG_ARC_MSG | `0x80FA2804` | Synchronous ARC message round-trip (send_arc_message contract) | debugfs-style poke | **tested** (M2, TEST 0x90 vs ttsim) |
| IOCTL_TENSTORRENT_DEBUG_GET_AGG_POWER | `0x80FA2808` | Read back the aggregated power state (verifies SET_POWER_STATE aggregation) | n/a | **tested** (M5) |

**Rules:** rows change status only in the same commit as the code they describe.
Every `design-pending` must resolve to a documented design (DD-N) before its
milestone completes. GET_HARVESTING's struct is not defined in `ioctl.h` — filled
in from the analysis (chardev.c handler) before M1 code.

## User-mode compatibility shim (M6)

`ttwin_compat` exposes tt_open/tt_ioctl/tt_mmap/tt_munmap/tt_close over the
above IOCTLs with the Linux request constants and structs unchanged; `ttconform`
exercises every implemented IOCTL through it. See docs/tt-umd-porting-notes.md.

## Real-silicon status (M7, Blackhole p150a, 2026-07-08)

Silicon validation per `docs/test-reports/real-silicon.md` (ladder rungs a-h,
Verifier 0x9BB active; firmware anchor 19.6.0.0 = Linux ground-truth capture):

| nr | Name | Silicon status |
|---|---|---|
| 0 | GET_DEVICE_INFO | **silicon-tested** (rung b; real BDF/subsys exact) |
| 1 | GET_HARVESTING | silicon-tested (stub parity, rung b negatives) |
| 2 | QUERY_MAPPINGS | **silicon-tested** (real BARs 512M/1M/32G — OQ-1 confirmed) |
| 3 | FREE/ALLOCATE_DMA_BUF | **silicon-tested** (rung d; 64K loopback + iATU) |
| 5 | GET_DRIVER_INFO | **silicon-tested** (rung b) |
| 6 | RESET_DEVICE | mixed by flavor: RESTORE_STATE(0) + POST_RESET(6) **silicon-tested** (rung f); ASIC_DMC_RESET(5) **silicon-tested** (rung h, under AER mitigation, marker-cleared proof); CONFIG_WRITE(2)/ASIC_RESET(4) **CONTRAINDICATED on real BH** (D4/OQ-7 — trigger wedges the link); RESET_PCIE_LINK(1) honest STATUS-unsupported on boards without ACPI _RST (D5, DD-11); USER_RESET(3) not silicon-run |
| 7 | PIN_PAGES | **silicon-tested** (rung e, identity domain; translated-domain refusal is review-validated — untestable with a test-signed build, HVCI must be off) |
| 10 | UNPIN_PAGES | **silicon-tested** (rung e teardown + negatives) |
| 11-13 | ALLOCATE/FREE/CONFIGURE_TLB | **silicon-tested** (rung c, both map paths + negatives) |
| 14 | SET_NOC_CLEANUP | sim-tested (M6); silicon pending a ttconform pass |
| 15 | SET_POWER_STATE | ARC POWER_SETTING (fixed wire format, DD-12) accepted by real FW on every open/close + probe-time aggregate; explicit ioctl path sim-tested (M5) |
| 9, 16 | MAP_PEER_BAR / EXPORT_TLB_DMABUF | deferred by design (unchanged) |
| ext | QUERY_TELEMETRY / MAP / UNMAP | **silicon-tested** (rungs b-e; telemetry cross-checked field-for-field vs tt-smi ground truth) |
