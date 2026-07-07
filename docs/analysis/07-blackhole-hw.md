# 07. Blackhole Hardware Personality

## Scope

Files covered (paths relative to tt-kmd repo root, baseline `ttkmd-2.10.0-rc1-1-g8c32c2b`):

| File | Lines | Role |
|---|---|---|
| `blackhole.c` | 840 | Complete Blackhole (BH) hardware personality: BAR mapping, TLB windows, NOC/CSM access, ARC messaging, telemetry, iATU, reset |
| `blackhole.h` | 28 | `struct blackhole_device` instance state |
| `device.h` | 127 | `struct tenstorrent_device` (generic per-device state) and `struct tenstorrent_device_class` ops table |

Cross-referenced (read to verify constants used by blackhole.c; covered in depth by other sections): `msgqueue.h` (28 lines), `msgqueue.c` (133 lines), `telemetry.h` (82 lines), `telemetry.c` (card-type decode only), `pcie.h`/`pcie.c` (reset helpers), `module.c` (module params), `enumerate.c`/`enumerate.h` (lifecycle call sites, PCI IDs), `ioctl.h` (ABI structs), `memory.h`, `tlb.h`.

---

## 1. Device identity and instance state

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

## 2. BAR layout

Blackhole exposes three BARs the driver uses: **BAR0** (TLB windows + NOC2AXI/TLB config registers), **BAR2** (DWC PCIe controller registers; iATU), **BAR4** (4 GB TLB windows).

### BAR0

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

### BAR2

Mapped in full: `bh->bar2_mapping = pci_iomap(bh->tt.pdev, 2, 0);` (blackhole.c:590). The only documented content is the **DesignWare iATU register block at BAR2 offset `0x1000`** (`IATU_BASE`, blackhole.c:76). Nothing else in BAR2 is touched by the driver.

### BAR4

Holds up to 8 × 4 GB TLB windows, window *j* at BAR4 offset `j * 4GB` (blackhole.c:25-28, 746-750). Not all 8 are guaranteed to be BIOS-exposed; `blackhole_init` clamps the per-device count to what BAR4 actually provides:

```c
resource_size_t bar4_len = pci_resource_len(tt_dev->pdev, 4);
// Limit 4G window count to what's available; partial windows not supported.
tt_dev->tlb_counts[1] = bar4_len / TLB_4G_WINDOW_SIZE;
```
(blackhole.c:576-580; comment at blackhole.c:25). BAR4 itself is never kernel-mapped by blackhole.c — user space mmaps windows into it via the TLB/mmap machinery (see `describe_tlb`, §4).

> **Porting note:** On Windows, BAR0 sub-range mappings become `MmMapIoSpaceEx` over portions of the CM_PARTIAL_RESOURCE_DESCRIPTOR for BAR0; the BAR4-length probe becomes reading the translated resource length for BAR4. The clamp `tlb_counts[1] = bar4_len / 4GB` must be preserved — machines with small BAR support expose fewer than 8 windows and partial windows are unsupported.

---

## 3. TLB windows

### Geometry constants

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

### 2 MB window register format (96 bits, packed)

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

### 4 GB window register format (96 bits, packed)

blackhole.c:139-163 — identical fields except `address : 32` (NOC address >> 32) and `reserved : 29`.

### Programming a window (`blackhole_configure_tlb`)

Dispatch (blackhole.c:724-736): `0 <= tlb < 202` → 2M path; `202 <= tlb < 210` → 4G path; otherwise **-EINVAL**.

2M path (blackhole.c:165-198):
- Validation: `if (config->addr & TLB_2M_WINDOW_MASK) return -EINVAL;` — address must be 2 MB-aligned (blackhole.c:172-173).
- Fields copied from `struct tenstorrent_noc_tlb_config` (ioctl.h:300-313): `addr>>21`, `x_end`, `y_end`, `x_start`, `y_start`, `noc`, `mcast`, `ordering`, `linked`, `static_vc → use_static_vc` (blackhole.c:175-184). Note `stream_header` and `static_vc` (the 3-bit VC number) are never set by this path.
- Three 32-bit MMIO writes: `low32` at +0, `mid32` at +4, `high32` at +8 (blackhole.c:186-188).
- **Strided-register scrub**: for `tlb < 32`, a fourth write zeroes the per-window strided config register at `tlb_regs + 0x9D8 + tlb*4`: "Strided TLB configuration is unsupported by the CONFIGURE_TLB API. Write zero to clear any strided configuration set by alternate means." (blackhole.c:190-195).

4G path (blackhole.c:200-226): same, with 4 GB alignment check (`config->addr & TLB_4G_WINDOW_MASK → -EINVAL`, blackhole.c:207-208) and `addr>>32`.

No range validation is done on `x_end/y_end/x_start/y_start` (u16 in ABI, 6-bit in hardware), `noc` (u8 → 2 bits), `ordering` (u8 → 2 bits): values are **silently truncated** to field width by the bitfield assignment.

No lock is taken in configure_tlb; per-window exclusivity is enforced by the generic TLB allocator (window ownership bitmaps `tt_dev->tlbs`, device.h:65).

### `blackhole_describe_tlb`

blackhole.c:738-753. Validation: `tlb < 0 || tlb >= 210 → -EINVAL`. Fills `struct tlb_descriptor {int bar; unsigned long size; unsigned long bar_offset;}` (tlb.h:12-16):

- 2M: `bar = 0`, `size = 0x200000`, `bar_offset = tlb * 0x200000`
- 4G: `bar = 4`, `size = 0x100000000`, `bar_offset = (tlb - 202) * 0x100000000`

(blackhole.c:746-750). This is what the mmap path uses to hand user space a window.

### Kernel-reserved TLB and NOC access primitives

`blackhole_init` claims window 201 for the kernel: `set_bit(KERNEL_TLB_INDEX, tt_dev->tlbs);` and initializes `kernel_tlb_mutex` (blackhole.c:608-610).

`bh_configure_kernel_tlb(bh, x, y, addr, noc)` (blackhole.c:228-241) programs window 201 as a unicast window to `(x_end=x, y_end=y)` at `addr & ~2M_mask` with `ordering = 1; // strict` (blackhole.c:236) and returns `kernel_tlb + (addr & 2M_mask)`.

`noc_read32` / `noc_write32` (blackhole.c:243-268) take `kernel_tlb_mutex`, reprogram the kernel window, do one 32-bit MMIO read/write, and unlock. **Every** kernel-initiated NOC access on Blackhole (ARC scratch, CSM, message queue, DBI-over-NOC) funnels through this single 2 MB window under this mutex.

> **Porting note:** `kernel_tlb_mutex` is acquired in process context only and the code inside sleeps (ARC polling with `usleep_range`), so the Windows equivalent must be a PASSIVE_LEVEL synchronization object (e.g. `FAST_MUTEX`/`KGUARDED_MUTEX` won't do if you also wait with delays — a KEVENT-based mutex or ERESOURCE at PASSIVE_LEVEL is appropriate). Do not use a spinlock: `send_arc_message` holds up to 500 ms + 1000 ms of polling under repeated acquisitions of this mutex.

### CSM accessors

ARC's Code/State Memory (CSM) window on the NOC is `0x10000000 .. 0x10080000` (base `ARC_CSM_BASE 0x10000000`, size `1 << 19` = 512 KB; telemetry.h:73-74). Range check helper (telemetry.h:75-78):

```c
static inline bool is_range_within_csm(u64 addr, size_t len)
{ return (addr >= ARC_CSM_BASE) && (addr <= (ARC_CSM_BASE + ARC_CSM_SIZE) - len); }
```

`csm_read32`/`csm_write32` (blackhole.c:270-286) validate with `is_range_within_csm(addr, 4)` (→ **-EINVAL**) then perform `noc_read32/noc_write32` to node **(x=8, y=0)** (`ARC_X 8`, `ARC_Y 0`, blackhole.c:57-58) on NOC 0. Exposed as class ops `blackhole_csm_read32`/`blackhole_csm_write32` (blackhole.c:288-296) — the generic `msgqueue.c` ring code calls back through these.

---

## 4. NOC2AXI config region: PCIe instance detection and perf counters

### Active PCIe instance detection

Blackhole has two PCIe controller instances; only one is connected. The NOC ID register identifies which (blackhole.c:298-302):

```c
// BH has two PCIE instances, the function reads NOC ID to find out which one is active
static bool blackhole_detect_pcie_noc_x(struct blackhole_device *bh, u32 *noc_x) {
	*noc_x = ioread32(bh->noc2axi_cfg + NOC_ID_OFFSET) & 0x3F;
	return (*noc_x == 2 || *noc_x == 11);
}
```

`NOC_ID_OFFSET 0x4044` (blackhole.c:46) — i.e. BAR0 `0x1FD04044`. The active PCIe tile is at NOC0 coordinates `(x, y=0)` with `x ∈ {2, 11}`; anything else means detection failed and save/restore of reset state silently does nothing (blackhole.c:310-311, 323-324).

### PCIe DBI over NOC

`PCIE_DBI_ADDR 0xF800000000000000ULL` — "this points to outbound NOC_TLB_62 configured by CMFW" (blackhole.c:50-51). Reads/writes to `(pcie_x, 0)` at this NOC address reach the DWC PCIe controller's DBI registers. Used only for the Device Control/Status register: `DBI_DEVICE_CONTROL_DEVICE_STATUS 0x78` (pcie.h:8).

### PCIe performance counters (sysfs)

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

## 5. iATU (outbound address translation), BAR2

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

## 6. ARC (firmware CPU): scratch registers, boot status, message queue

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

### `send_arc_message` sequence (blackhole.c:500-540)

1. **Ready poll:** read `ARC_BOOT_STATUS` (0x80030408) via kernel TLB in a loop for up to `ARC_MSG_READY_MS` = **500 ms**. If a read returns `0xFFFFFFFF` → "NOC is hung", return false immediately (blackhole.c:509-515). If bit 0 (`READY_FOR_MSG`) never sets → false.
2. **Queue discovery:** `queue_ctrl_addr = noc_read32(ARC_MSG_QCB_PTR)` (0x8003042C); then via CSM-validated reads: `queue_base = csm[qcb+0]`, `queue_info = csm[qcb+4]`, `num_entries = queue_info & 0xFF` (blackhole.c:520-528). Because these go through `csm_read32`, a QCB pointer outside `0x10000000..0x1007FFFC` fails the send.
3. **Push** request via generic ring code `arc_msg_push` (msgqueue.c:12-70): request ring starts at `queue_base + 32` (`ARC_MSG_QUEUE_HEADER_SIZE 32`, msgqueue.h:16); pointers at `queue_base+0x00` (REQ_WPTR), `+0x04` (RES_RPTR), `+0x10` (REQ_RPTR), `+0x14` (RES_WPTR) (msgqueue.h:19-22); occupancy = `(wptr - rptr) % (2*num_entries)`; waits up to `ARC_MSG_TIMEOUT_MS 1000` (msgqueue.h:17) with `usleep_range(100, 200)` polling; slot = `wptr % num_entries`; 8 dword writes; wptr advanced mod `2*num_entries`. All-1s pointer reads abort ("device gone?", msgqueue.c:25-28).
4. **Doorbell:** `noc_write32(bh, 8, 0, ARC_MSI_FIFO /*0x800B0000*/, 0, 0)` — "Trigger ARC interrupt" (blackhole.c:533-534).
5. **Pop** response via `arc_msg_pop` (msgqueue.c:72-132): response ring at `queue_base + 32 + num_entries*32`; same 1000 ms poll for occupancy > 0; reads 8 dwords; advances RES_RPTR.
6. **Success criterion:** `return msg->header == 0;` — firmware writes 0 into the response header on success (blackhole.c:539).

The whole exchange is synchronous, polled, interrupt-free from the host side, and every CSM access re-acquires `kernel_tlb_mutex` (one lock cycle per 32-bit access).

> **Porting note:** Worst-case latency of one message ≈ 500 ms (ready) + 1000 ms (push space) + 1000 ms (response) of sleeping poll loops. On Windows this must run at PASSIVE_LEVEL (e.g. a system worker/passive-level DPC substitute or the calling IOCTL thread), never at DISPATCH_LEVEL.

---

## 7. Telemetry

### Discovery (`telemetry_probe`, class op `.probe_telemetry`) — blackhole.c:455-498

1. Zero the per-device `telemetry_tag_cache[128]` (`TELEM_TAG_CACHE_SIZE 128`, telemetry.h:13; cache field device.h:75-79 — "BH stores a raw CSM address for NOC reads").
2. `base_addr = noc_read32(ARC_TELEMETRY_PTR /*0x80030434*/)`, `data_addr = noc_read32(ARC_TELEMETRY_DATA /*0x80030430*/)`; `tags_addr = base_addr + 8` (blackhole.c:466-468).
3. Both pointers must lie in CSM (checked with length 1) else `dev_err("Telemetry not available")` and **-ENODEV** (blackhole.c:470-473).
4. Version word at `base_addr`: `major = (v>>16)&0xFF, minor = (v>>8)&0xFF, patch = v&0xFF`; `major > 1` → **-ENOTSUPP** (blackhole.c:475-483).
5. `num_entries` at `base_addr + 4`; then a tag table scan: each entry at `tags_addr + i*4` is `{u16 tag_id = entry & 0xFFFF; u16 offset = entry >> 16}`; the cached address is `data_addr + offset*4`; only tags < 128 are cached (blackhole.c:485-495). Returns 0.

So the telemetry data layout in CSM is: `[version u32][num_entries u32][tag entries u32 × N]` at `base_addr`, values array at `data_addr` (dword-indexed by `offset`).

Note the tag-table reads use raw `noc_read32` (not `csm_read32`), so per-entry addresses are **not** re-validated against CSM during the scan; validation happens later at read time.

### Read path (`blackhole_read_telemetry_tag`, class op `.read_telemetry_tag`) — blackhole.c:440-453

- `tag_id >= 128` → **-EINVAL**
- cache entry 0 (tag absent) → **-ENODATA**
- else `csm_read32(bh, addr, value)` (re-validates CSM range → -EINVAL if out; returns 0 on success).

### Consumers (sysfs + hwmon)

Telemetry tag IDs used by Blackhole (telemetry.h:15-41 for values):

- sysfs attributes (blackhole.c:413-424): `tt_aiclk` (AICLK=14), `tt_axiclk` (15), `tt_arcclk` (16), `tt_serial` + `tt_card_type` (BOARD_ID=1, the latter read as u64: tag N and N+1), `tt_fw_bundle_ver` (28), `tt_m3app_fw_ver` (26), `tt_asic_id` (61), `tt_heartbeat` (32), `tt_therm_trip_count` (60).
- hwmon channels (blackhole.c:400-411): ASIC_TEMP=11→temp_input, THM_LIMIT_THROTTLE=56→temp_max, VCORE=6→in_input, VDD_LIMITS=9→in_max, CURRENT=8→curr_input, TDC_LIMIT_MAX=55→curr_max, POWER=7→power_input, TDP_LIMIT_MAX=64→power_max, FAN_RPM=41→fan_input. hwmon device registered under name `"blackhole"` (blackhole.c:664).

---

## 8. The `tenstorrent_device_class` ops table

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

## 9. Reset sequences

Reset flags from the ioctl ABI: `TENSTORRENT_RESET_DEVICE_ASIC_RESET 4`, `TENSTORRENT_RESET_DEVICE_ASIC_DMC_RESET 5` (ioctl.h:149-150). `blackhole_reset` (blackhole.c:542-570) supports exactly these two; any other flag → returns false.

### ASIC+DMC reset (flag 5) — firmware-mediated, step by step

1. Probe firmware liveness with a **TEST message** (`header = 0x90`, no payload). If `send_arc_message` fails: `dev_warn("Couldn't communicate with firmware; NOC is likely hung.")`, return false (blackhole.c:550-555).
2. `set_reset_marker(pdev)`: sets the `PCI_COMMAND_PARITY` bit in config-space PCI_COMMAND — "pci_command_parity is used as reset marker. Set to 1, check if cleared to 0 after reset" (pcie.c:140-149; checked later by `is_reset_marker_zero`, pcie.c:151-158 — a reset clears config space, so a cleared parity bit proves the device actually reset).
3. Send **TRIGGER_RESET** (`header = 0x56`, `payload[0] = 3 // Argument for ASIC + M3 reset`) (blackhole.c:549, 559-562). The response is *not* awaited meaningfully — return value of the second send is ignored; function returns `true; // Possibly a lie...` (blackhole.c:562-563).

### ASIC-only reset (flag 4) — config-space interface timer

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

### MPS save/restore around reset

Reset wipes the endpoint's Device Control register. `blackhole_save_reset_state` captures the negotiated Max Payload Size before reset and `blackhole_restore_reset_state` re-writes it afterward — but via **NOC access to the DBI** (`0xF800000000000000 + 0x78` at tile (2|11, 0)), not via host config space (blackhole.c:304-330). Only the MPS field (`PCI_EXP_DEVCTL_PAYLOAD`) is preserved; `saved_mps` is a u8 holding the 3-bit field value (blackhole.h:19).

> **Porting note:** On Windows, config-space writes for `set_reset_marker`/`pcie_timer_interrupt` map to `BUS_INTERFACE_STANDARD`/`IoGetDeviceProperty`-style config access (`SetBusData`/`GetBusData`). Restoring MPS through the NOC (not config space) must be kept as-is: the host-side config write would race the DWC DBI state. Also note MRRS is re-set to 4096 in `init_hardware` on every hardware (re)init (blackhole.c:626) — Windows PCI stack may clamp MRRS differently; the port must re-apply it after every reset/resume.

---

## 10. Card variants: p150a, Galaxy

There is **no p150a-specific code path in blackhole.c** — all Blackhole boards share this personality. Card differentiation is by telemetry BOARD_ID (tag 1) upper bits: `card_type = (value >> 4) & 0xFFFF` where `value` is the *upper* 32 bits of the 64-bit board ID; Blackhole types: `0x36`=p100, `0x40`=**p150a**, `0x41`=p150b, `0x42`=p150c, `0x43`=p100a, `0x44`=p300b, `0x45`=p300a, `0x46`=p300c, `0x47`=galaxy-blackhole (telemetry.c:108-123).

`PCI_SUBSYSTEM_DEVICE_GALAXY 0x0047` is defined in blackhole.c:53-54 but **never used in blackhole.c** (dead constant). Galaxy-BH handling lives in enumerate.c: subsystem ID `PCI_SUBSYSTEM_ID_GALAXY_BH 0x0047` triggers deterministic ordinal assignment from PCI bus number using UBB bus prefixes `{0x0, 0x4, 0xC, 0x8}` (enumerate.c:42-46, 57-72, 356).

---

## 11. Hardware quirks, workarounds, failure behaviors

1. **BAR2 mapping not checked in init.** `blackhole_init`'s failure check tests only `tlb_regs`, `kernel_tlb`, `noc2axi_cfg` — *not* `bar2_mapping` (blackhole.c:592: `if (!bh->tlb_regs || !bh->kernel_tlb || !bh->noc2axi_cfg)`). If BAR2 mapping fails, init succeeds and a later `configure_outbound_atu` dereferences `NULL + offset`. The error path *does* unmap `bar2_mapping` if the other three failed (blackhole.c:602-603). A Windows port should treat a BAR2 mapping failure as fatal.
2. **All-1s reads as hang detection.** `0xFFFFFFFF` from `ARC_BOOT_STATUS` (blackhole.c:511-512) or from queue pointers (msgqueue.c:25-28, 38-41, 85-88, 98-101) is treated as "NOC hung / device gone" and aborts the operation. A port must preserve these checks — they are the only defense against surprise-removal / hung-NOC deadloops.
3. **Strided-TLB scrub** on every 2M window configure for windows 0..31 (blackhole.c:190-195) — protects against stale non-rectangular multicast configs programmed "by alternate means" (e.g. previous user, firmware).
4. **`reset` returning "possibly a lie"** — after TRIGGER_RESET the device may drop off the bus before responding; the true confirmation is the reset marker (PCI_COMMAND parity bit) reading 0 after re-enumeration (blackhole.c:563; pcie.c:144, 151-158).
5. **Silent field truncation** in TLB config (§3) — no `-EINVAL` for out-of-range NOC coordinates; hardware bitfields truncate.
6. **`cleanup_hardware` skipped when `detached`** (blackhole.c:702-703; `detached` flag device.h:33) — after surprise removal or reset-detach, no A3 message is attempted against dead hardware.
7. **WDT set failure is non-fatal** ("normal for old FW", blackhole.c:636) — the port must not fail init when firmware rejects opcode 0xC1.
8. **`telemetry_probe` CSM checks use length 1** (blackhole.c:470) rather than 4 — a base pointer at `CSM_END - 1` would pass the probe check but the subsequent version read would extend past CSM (raw `noc_read32`, unvalidated).

---

## 12. Lifecycle / cleanup summary (fd close, device removal)

- Nothing in blackhole.c is per-fd; all state is per-device. Per-fd TLB/iATU/pin cleanup is in the common layers (memory.c/tlb.c), which call back into `configure_tlb` / `configure_outbound_atu(region, 0, 0, 0)` (memory.c:284-296) to disable resources at fd close.
- Device removal order (enumerate.c:423-459): set `detached` → cancel `power_down_work` → `cleanup_hardware` (A3 message, skipped if detached or device already gone) → `cleanup_telemetry` (hwmon + sysfs groups) → `cleanup_device` (unmap BARs) → revoke TLB dmabufs (enumerate.c:459, after BAR unmap). Suspend calls `cleanup_hardware` only (enumerate.c:506); resume calls `init_hardware` and re-saves PCI state (enumerate.c:515-519).
- `telemetry_attrs` array is devm-allocated (blackhole.c:582) — freed automatically at unbind; a Windows port must free it explicitly in cleanup.

---

## Key constants table

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

## Open questions

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
