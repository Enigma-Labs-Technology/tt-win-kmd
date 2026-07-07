# 08. Wormhole hardware (BARs, registers, ARC messaging, telemetry, reset)

## Scope

Files fully read for this section:

- `wormhole.c` — 1084 lines. The Wormhole device-class implementation (ops table `wormhole_class`).
- `wormhole.h` — 32 lines. `struct wormhole_device` and the one exported helper `wormhole_send_arc_fw_message_with_args`.
- `pcie.c` — 159 lines. Shared PCIe reset/link helpers plus the Wormhole-only `wormhole_complete_pcie_init`.
- `pcie.h` — 18 lines. `DBI_DEVICE_CONTROL_DEVICE_STATUS` and PCIe helper prototypes.
- `msgqueue.c` — 133 lines / `msgqueue.h` — 27 lines. The **shared** queue-based ARC message protocol (`arc_msg_push`/`arc_msg_pop`), used by both Wormhole (power settings only) and Blackhole (everything).

Supporting files read for cross-references (covered in depth in their own sections):
`device.h` (device-class vtable, `struct tenstorrent_device`), `telemetry.h` (`ARC_CSM_BASE`, `is_range_within_csm`, tag enum, `TELEM_TAG_CACHE_SIZE`), `tlb.h` (`struct tlb_descriptor`), `enumerate.h`/`enumerate.c` (PCI IDs, probe/remove), `module.c`/`module.h` (module params `auto_reset_timeout`, `reset_limit`, `idle_power_down_grace_ms`; PCI ID table), `ioctl.h` (`tenstorrent_noc_tlb_config`, `tenstorrent_power_state`, reset flags).

**Grayskull status:** No `grayskull.c`/`grayskull.h` remain in the tree. The only Grayskull traces are the PCI device ID `PCI_DEVICE_ID_GRAYSKULL 0xFACA` (`enumerate.h:16`) and its table entry, which is **deprecated with `NULL` driver_data** (`module.c:65-66`). At probe, a `NULL` driver_data is rejected: `if (!id->driver_data) { dev_warn(&dev->dev, "Unsupported device\n"); return -ENODEV; }` (`enumerate.c:261-264`). A Windows port does **not** need Grayskull support.

> **Porting note:** Primary port target is Blackhole. Throughout this section, mechanisms are tagged **[WH-only]** (Wormhole-specific, defer unless Wormhole support is scheduled) or **[SHARED]** (common helper reused by Blackhole; porting it benefits both). The scratch-register ARC protocol is [WH-only]; the queue-based `msgqueue.c` protocol is [SHARED].

---

## 1. Device class registration and identity

Wormhole matches PCI vendor `0x1E52` / device `0x401E` (`enumerate.h:15,17`) and binds `&wormhole_class` via `driver_data` (`module.c:67-68`). The ops table:

```c
struct tenstorrent_device_class wormhole_class = {
	.name = "Wormhole",
	.instance_size = sizeof(struct wormhole_device),
	.dma_address_bits = 32,
	.noc_dma_limit = (0xFFFE0000 - 1),
	.noc_pcie_offset = 0x800000000ULL,
	.tlb_kinds = NUM_TLB_KINDS,                    // 3
	.tlb_counts = { 156, 10, 20 },
	.tlb_sizes  = { 1M, 2M, 16M },
	...
	.defer_idle_powerdown = true,
};
```
(`wormhole.c:1055-1084`)

- `dma_address_bits = 32` — coherent DMA mask is 32-bit; legacy WH userspace assumes 32-bit DMA addresses from ALLOCATE_DMA_BUF (`enumerate.c:322-327`). Contrast Blackhole.
- `noc_dma_limit = 0xFFFE0000 - 1` (`wormhole.c:1059`).
- `noc_pcie_offset = 0x800000000ULL` (`wormhole.c:1060`) — same value as `PCIE_DBI_ADDR` (`wormhole.c:92`).
- `defer_idle_powerdown = true` (`wormhole.c:1083`) — idle power-down message is sent from delayed work armed at last-fd-close, honoring `idle_power_down_grace_ms` (default 5000, `module.c:56`).

`struct wormhole_device` (`wormhole.h:10-24`) embeds `struct tenstorrent_device tt` as first member; `tt_dev_to_wh_dev()` is `container_of` (`wormhole.h:26-27`). Fields: `kernel_tlb_mutex`, `bar2_mapping`, `bar4_mapping`, `saved_mps` (u8), `fw_ready_work` (delayed work), `telemetry_retries` (int), and two bool `*_group_registered` flags.

---

## 2. BAR layout and register map

### BARs

| BAR | Role | Mapped in kernel? | Cite |
|-----|------|-------------------|------|
| BAR0 | TLB windows (userspace NOC apertures) | No (mmap'd by userspace) | `wormhole.c:23` (`TLB_1M_WINDOW_BASE 0 // BAR0`), `describe_tlb` sets `desc->bar = 0` (`wormhole.c:908`) |
| BAR2 | iATU registers | Yes, `bar2_mapping = pci_iomap(pdev, 2, 0)` | `wormhole.c:696-697`, `IATU_BASE 0x1200` relative to BAR2 (`wormhole.c:46`) |
| BAR4 | 32MB window onto system registers `0x1E000000..0x20000000` | Yes, `bar4_mapping = pci_iomap(pdev, 4, 0)` | `wormhole.c:699-700`, `BAR4_SOC_TARGET_ADDRESS 0x1E000000` (`wormhole.c:76`) |

BAR4 is directed to system registers by programming iATU **inbound region 1** as a BAR-match remap to SoC address `0x1E000000` (`map_bar4_to_system_registers`, `wormhole.c:449-458`). This is done in `init_hardware` (`wormhole.c:721`).

BAR0 total span (computed, not an explicit constant): 156×1M + 10×2M + 20×16M = 496 MB. TLB window count `TLB_WINDOW_COUNT = 186` (`wormhole.c:36`).

### System-register offsets within BAR4 (all relative to `BAR4_SOC_TARGET_ADDRESS = 0x1E000000`)

| Symbol | SoC address | BAR4 offset | Cite |
|--------|-------------|-------------|------|
| `RESET_UNIT_START` | `0x1FF30000` | `0x01F30000` | `wormhole.c:78` |
| `ARC_CSM_START` | `0x1FE80000` | `0x01E80000` | `wormhole.c:79` |
| `TLB_REGS_START` | `0x1FC00000` | `0x01C00000` | `wormhole.c:80` |
| `NOC2AXI_START` | `0x1FD02000` | `0x01D02000` | `wormhole.c:81` |
| `KERNEL_TLB_START` | `0x1E000000` | `0x00000000` | `wormhole.c:88` |

Derived pointers into the reset unit:

| Symbol | Offset | Cite |
|--------|--------|------|
| `ARC_TELEMETRY_PTR` | `RESET_UNIT_START + 0x01D0` | `wormhole.c:83` |
| `ARC_TELEMETRY_DATA` | `RESET_UNIT_START + 0x01D4` | `wormhole.c:84` |
| `ARC_MSG_QCB_PTR` | `RESET_UNIT_START + 0x01D8` | `wormhole.c:119` |

`reset_unit_regs(wh_dev)` returns `bar4_mapping + RESET_UNIT_START` (`wormhole.c:460-462`).

### Scratch registers (byte offset within the reset unit)

`SCRATCH_REG(n) = 0x60 + n*4` (`wormhole.c:90`):

| Register | Offset | Purpose | Cite |
|----------|--------|---------|------|
| SR0 = `POST_CODE_REG` | `0x60` | ARC L2 running check | `wormhole.c:101` |
| SR3 = args_reg | `0x6C` | ARC message args (arg0\|arg1<<16) | `wormhole.c:208` |
| SR5 = message_reg | `0x74` | ARC message id / completion | `wormhole.c:209` |
| SR6 = `PCIE_ARMISC_INFO_REG` | `0x78` | DBI-enable (read side) / hardware-hung sentinel | `wormhole.c:98`, `137` |
| SR7 = `PCIE_AWMISC_INFO_REG` | `0x7C` | DBI-enable (write side) | `wormhole.c:99` |

`ARC_MISC_CNTL_REG = 0x100`, IRQ0 trigger bit `ARC_MISC_CNTL_IRQ0_MASK = (1 << 16)` (`wormhole.c:105-106`).

> **Porting note:** `SCRATCH_REG(6)` (0x78) is overloaded: it is both `PCIE_ARMISC_INFO_REG` (DBI routing) and the register read by `is_hardware_hung` to detect a dead link (`== 0xFFFFFFFF`). A Windows port must preserve this dual use. All these reads/writes are `ioread32`/`iowrite32` on the BAR4 kernel mapping — on Windows, MMIO via `READ_REGISTER_ULONG`/`WRITE_REGISTER_ULONG` on the mapped BAR4 with matching little-endian semantics.

---

## 3. ARC firmware messaging — scratch-register protocol [WH-only]

This is the classic Wormhole ARC message path. **Blackhole does not use it** (Blackhole uses only the queue protocol in §4); a grep of `blackhole.c` shows no reference to `wormhole_send_arc_fw_message*`.

### Protocol

Documented at `wormhole.c:108-112`: write `0xAA00 | message_id` into SR5, wait for `message_id` to appear in the low 16 bits. FW resets SR5 to 0 then writes back `message_id` (plus a 16-bit exit code in the high half) when done. `WH_FW_MESSAGE_PRESENT = 0xAA00` (`wormhole.c:112`).

`wormhole_send_arc_fw_message_with_args` (`wormhole.c:205-234`, **exported** via `wormhole.h:29-30`):

1. Bail if ARC L2 not running: `arc_l2_is_running()` checks `(POST_CODE & 0xFFFF0000) == 0xC0DE0000` (`wormhole.c:199-203`, constants `POST_CODE_ARC_L2 0xC0DE0000`, `POST_CODE_ARC_L2_MASK 0xFFFF0000` at `wormhole.c:102-103`). On failure logs and returns `false` (`wormhole.c:214-217`).
2. `iowrite32(arg0 | (arg1<<16), SR3)` then `iowrite32(0xAA00 | message_id, SR5)` (`wormhole.c:219-220`).
3. Trigger IRQ to ARC: read-modify-write `ARC_MISC_CNTL_REG |= (1<<16)` (`wormhole.c:222-224`).
4. If `timeout_us == 0`, return `false` immediately (fire-and-forget; note caller must not treat this as an error) (`wormhole.c:226-227`).
5. Otherwise poll via `arc_msg_poll_completion` (`wormhole.c:229-233`).

`arc_msg_poll_completion` (`wormhole.c:163-197`):
- Poll period `= max(10, timeout_us/100)` µs (~100 polls) (`wormhole.c:167`).
- Match when `(read_val & 0xffff) == msg_code`; sets `*exit_code = read_val >> 16`; returns **0** (`wormhole.c:174-178`).
- `read_val == 0xFFFFFFFF` **and** hung → return **-3** (`wormhole.c:180-183`).
- `read_val == 0xFFFFFFFF` (not hung) → unrecognized message, return **-2** (`wormhole.c:185-188`).
- Deadline exceeded → return **-1** (`wormhole.c:190-193`).
- Sleeps `usleep_range(poll_period, 2*poll_period)` between polls (`wormhole.c:195`).

`is_hardware_hung` (`wormhole.c:129-138`): returns true if config-space vendor read fails or `!= 0x1E52`, **or** `ioread32(reset_unit_regs + SCRATCH_REG(6)) == 0xFFFFFFFF`.

`wormhole_send_arc_fw_message` (`wormhole.c:236-240`) is the args-less wrapper (arg0=arg1=0).

### Scratch-protocol message IDs

| Message | ID | Cite |
|---------|----|----|
| `WH_FW_MSG_PCIE_INDEX` | `0x51` | `wormhole.c:39` |
| `WH_FW_MSG_TRIGGER_RESET` | `0x56` | `wormhole.c:42` |
| `WH_FW_MSG_NOP` | `0x11` | `wormhole.c:43` |
| `WH_FW_MSG_ASTATE0` | `0xA0` | `wormhole.c:40,114` (defined twice, same value) |
| `WH_FW_MSG_ASTATE3` | `0xA3` | `wormhole.c:115` |
| `WH_FW_MSG_UPDATE_M3_AUTO_RESET_TIMEOUT` | `0xBC` | `wormhole.c:41` |
| `WH_FW_MSG_CURR_DATE` | `0xB7` | `wormhole.c:116` |
| `FW_MSG_PCIE_RETRAIN` | `0xB6` | `pcie.c:16` |

---

## 4. ARC firmware messaging — queue protocol [SHARED with Blackhole]

Used by Wormhole **only for power settings** (`send_arc_message`, `wormhole.c:242-291`), and by Blackhole for all messaging. Implemented in `msgqueue.c`/`msgqueue.h`.

### Message format (shared)

```c
struct arc_msg {
	u32 header;
	u32 payload[7];
};              // 32 bytes (8 words)
```
(`msgqueue.h:11-14`) — `ARC_MSG_QUEUE_HEADER_SIZE 32`, `ARC_MSG_TIMEOUT_MS 1000` (`msgqueue.h:16-17`).

Queue-control-block (QCB) ring pointers, relative to `queue_base` (`msgqueue.h:19-22`):

| Pointer | Offset |
|---------|--------|
| `ARC_MSG_QUEUE_REQ_WPTR` | `+0x00` |
| `ARC_MSG_QUEUE_RES_RPTR` | `+0x04` |
| `ARC_MSG_QUEUE_REQ_RPTR` | `+0x10` |
| `ARC_MSG_QUEUE_RES_WPTR` | `+0x14` |

`arc_msg_push` (`msgqueue.c:12-70`): reads WPTR/RPTR through the class `csm_read32`/`csm_write32` callbacks; treats an all-`0xFFFFFFFF` read as "device gone" and fails (`msgqueue.c:25-28,38-41`). Occupancy `= (wptr - rptr) % (2*num_entries)`; waits (100–200 µs sleeps, 1000 ms timeout) until `< num_entries` (`msgqueue.c:43-53`). Writes 8 words into slot `wptr % num_entries` at `request_base = queue_base + 32` (`msgqueue.c:55-63`); advances `wptr = (wptr+1) % (2*num_entries)` (`msgqueue.c:65-67`).

`arc_msg_pop` (`msgqueue.c:72-132`): `response_base = queue_base + 32 + num_entries*32` (`msgqueue.c:75`); waits until occupancy `> 0`; reads header + 7 payload words; advances RES_RPTR.

### Wormhole `send_arc_message` wrapper [WH-only wiring; calls SHARED core]

`send_arc_message` (`wormhole.c:242-291`, `__maybe_unused`):
1. Spin (up to `ARC_MSG_READY_MS = 500`, `wormhole.c:120`) until `arc_l2_is_running` (`wormhole.c:252-260`).
2. Read QCB pointer from `ARC_MSG_QCB_PTR` (`wormhole.c:263`). **`qcb_ptr == 0` means old FW without queue support → `dev_warn_once` and return false** (`wormhole.c:264-267`).
3. Validate `is_range_within_csm(qcb_ptr, 4)` (`wormhole.c:268-269`).
4. `csm_read32(qcb_ptr+0)` → `queue_base`, `csm_read32(qcb_ptr+4)` → `queue_info` (`wormhole.c:271-275`). `queue_base += ARC_CSM_BASE`; `num_entries = queue_info & 0xFF` (`wormhole.c:277-278`).
5. `arc_msg_push`, trigger IRQ0 (`ARC_MISC_CNTL_REG |= 1<<16`), `arc_msg_pop` (`wormhole.c:280-288`).
6. Success == `msg->header == 0` (`wormhole.c:290`).

### CSM access primitives (used by the queue protocol) [WH-only address translation]

`wh_arc_addr_to_sysreg(arc_addr) = ARC_CSM_START + (arc_addr - ARC_CSM_BASE)` (`wormhole.c:140-143`). `ARC_CSM_BASE = 0x10000000`, `ARC_CSM_SIZE = (1<<19)` (`telemetry.h:73-74`).

`csm_read32`/`csm_write32` (`wormhole.c:145-161`): validate via `is_range_within_csm(addr, 4)`, return **-EINVAL** on failure; otherwise `ioread32`/`iowrite32` at `bar4_mapping + wh_arc_addr_to_sysreg(addr)`. Exposed as class ops `wormhole_csm_read32`/`wormhole_csm_write32` (`wormhole.c:1024-1032`).

`is_range_within_csm(addr,len)` = `addr >= 0x10000000 && addr <= (0x10000000 + 0x80000) - len` (`telemetry.h:75-78`). **[SHARED]** — same check used by Blackhole.

> **Porting note:** The queue protocol reaches the CSM via the class `csm_read32`/`csm_write32` callbacks. Wormhole translates the ARC CSM address into a BAR4 sysreg offset (`wh_arc_addr_to_sysreg`); Blackhole (per `device.h:76-79`) stores/uses **raw CSM addresses** for NOC reads. The `msgqueue.c` core is address-agnostic and portable as-is; only the per-arch `csm_read32`/`csm_write32` differ.

---

## 5. TLB windows and NOC access

### Geometry (`wormhole.c:20-36`)

| Kind | Count | Shift | Window size | Base (in BAR0) |
|------|-------|-------|-------------|----------------|
| 1M | 156 (`TLB_1M_WINDOW_COUNT`) | 20 | `0x100000` | `0` |
| 2M | 10 (`TLB_2M_WINDOW_COUNT`) | 21 | `0x200000` | `156 * 1M` |
| 16M | 20 (`TLB_16M_WINDOW_COUNT`) | 24 | `0x1000000` | `2M base + 10*2M` |

`TLB_WINDOW_COUNT = 186`; `WH_NOC_BITS = 36` (`wormhole.c:36-37`). `NUM_TLB_KINDS = 3` (`wormhole.c:803`). Index/shift/size/base lookup arrays at `wormhole.c:804-807`.

`wormhole_tlb_kind(tlb)` maps a TLB index to kind 0/1/2 or `-EINVAL` (`wormhole.c:825-838`).

### TLB config register encoding [WH-only]

Non-address control bits (`struct noc_tlb_non_address_bits`, `wormhole.c:809-823`):

```c
union { u32 reg; struct {
	u64 x_end: 6; u64 y_end: 6; u64 x_start: 6; u64 y_start: 6;
	u64 noc_sel : 1; u64 mcast: 1; u64 ordering: 2; u64 linked: 1;
}; };   // 29 bits used, packed into the low 32 bits (union with u32 reg)
```

`construct_tlb_config` (`wormhole.c:840-871`) validates:
- Bad kind → **-EINVAL** (`wormhole.c:855-856`).
- `config->addr & (window_size-1)` (unaligned) → **-EINVAL** (`wormhole.c:858-860`).
- `config->addr >= (1UL << 36)` → **-EINVAL** (`wormhole.c:862-864`).

Register value (`wormhole.c:866-868`):
```c
*regs  = (u64)config->addr >> TLB_SHIFTS[kind];                       // low bits = address page
*regs |= (u64)non_address_bits.reg << (WH_NOC_BITS - TLB_SHIFTS[kind]); // NOC bits above
```

`wh_configure_tlb` (`wormhole.c:873-891`): range-checks `tlb ∈ [0, 186)` (**-EINVAL**), builds regs, writes the 64-bit value as two 32-bit stores at `TLB_REGS_START + tlb*2*4` (low word first) (`wormhole.c:886-888`). Public op `wormhole_configure_tlb` (`wormhole.c:893-898`).

`wormhole_describe_tlb` (`wormhole.c:900-915`): fills `desc->bar = 0`, `desc->size = window_size`, `desc->bar_offset = base + size*(tlb - kind_first_index)`; **-EINVAL** on bad kind. TLB windows are **BAR0-relative** for userspace mmap.

> **Porting note:** The `u64`-typed bitfields inside a `union` with `u32 reg` are compiler-layout-dependent. On MSVC/Windows the port must reproduce the exact bit packing (x_end at bit 0, …, linked at bit 28) rather than rely on struct-bitfield ABI. Prefer explicit shift/mask when porting.

### Kernel TLB and NOC read/write [WH-only]

`KERNEL_TLB_INDEX = TLB_WINDOW_COUNT - 1 = 185` — the **last 16M window**, reserved by `set_bit(KERNEL_TLB_INDEX, tt_dev->tlbs)` in `wormhole_init` (`wormhole.c:702`) so userspace cannot allocate it. Its data aperture is `bar4_mapping + KERNEL_TLB_START (=0) + offset` (`wormhole.c:88, 928`).

`wh_configure_kernel_tlb(wh, x, y, addr, noc)` (`wormhole.c:917-929`): splits `addr` at the 16M mask (`TLB_16M_WINDOW_MASK = 16M-1`, `wormhole.c:34`); programs index 185 with `x_end=x, y_end=y, ordering=1 (strict), noc=noc`; returns the window pointer at `offset = addr & mask`.

`noc_read32` / `noc_write32` (`wormhole.c:931-954`): take `kernel_tlb_mutex`, configure the kernel TLB, do a single `ioread32`/`iowrite32`, release the mutex. `wormhole_noc_write32` is the class op (`wormhole.c:1018-1022`); **no `noc_read32` class op is exported** (used internally only, e.g. save/restore reset state).

> **Porting note:** `kernel_tlb_mutex` serializes *all* kernel NOC access because there is one shared kernel TLB (index 185). Every `noc_read32`/`noc_write32`, and thus `save_reset_state`/`restore_reset_state`, contends on it. On Windows use a fast mutex / passive-level lock; these paths run at PASSIVE_LEVEL (they sleep in the ARC helpers).

---

## 6. iATU (inbound BAR remap + outbound DMA) [WH-only register layout]

iATU register block at `IATU_BASE = 0x1200` relative to BAR2 (`wormhole.c:46`). Region stride `0x100`, 16 outbound regions (`wormhole.c:49-50`). Per-direction field offsets at `wormhole.c:51-61`. `write_iatu_reg` computes `offset = IATU_BASE + (2*region + direction)*0x100 + reg` and writes to `bar2_mapping` (`wormhole.c:362-368`); `WRITE_IATU_REG` macro at `wormhole.c:123-125`.

Inbound control-2 fields (`wormhole.c:63-67`): `REGION_EN (1<<31)`, `BAR_MATCH_MODE (1<<30)`, `FUZZY_TYPE_MATCH (1<<27)`, `BAR_NUM(n) ((n)<<8)`.
Outbound control-2 fields (`wormhole.c:69-72`): `DMA_BYPASS (1<<27)`, `TLP_BYPASS (1<<21)`, `FUNC_BYPASS (1<<19)`.

`map_bar4_to_system_registers` (`wormhole.c:449-458`): inbound region 1, target `0x1E000000`, ctrl2 = `REGION_EN | BAR_MATCH_MODE | FUZZY_TYPE_MATCH | BAR_NUM(4)`.

`wormhole_configure_outbound_atu(region, base, limit, target)` (`wormhole.c:990-1016`), class op `.configure_outbound_atu`:
- `region >= 16` → **-EINVAL** (`wormhole.c:1001-1002`).
- `limit > U32_MAX` → **-EINVAL** (`wormhole.c:1004-1005`).
- ctrl1 = 0 (MEM type); ctrl2 = 0 when `limit==0` (disable) else `REGION_EN | DMA_BYPASS | TLP_BYPASS | FUNC_BYPASS` (`wormhole.c:994-995`).
- Writes lower/upper base, lower/upper target, limit, ctrl1, ctrl2 in that order (`wormhole.c:1007-1013`).

---

## 7. Telemetry [tag table WH-specific; cache/tags SHARED semantics]

### Discovery

`ARC_TELEMETRY_PTR` holds a CSM address of the telemetry table base; `ARC_TELEMETRY_DATA` holds the CSM address of the data block (`wormhole.c:83-84`).

`telemetry_probe` (`wormhole.c:536-584`):
1. Zero the tag cache (`wormhole.c:545`).
2. Read `base_addr` and `data_addr` (raw `ioread32` of the two pointer regs) (`wormhole.c:547-548`); `tags_addr = base_addr + 8`.
3. If either is **not** `is_range_within_csm` → log "Telemetry not available", return **-ENODEV** (`wormhole.c:551-554`).
4. Read version at `base_addr` (via `wh_arc_addr_to_sysreg`): `major=(v>>16)&0xFF, minor=(v>>8)&0xFF, patch=v&0xFF`. `major > 1` → **-ENOTSUPP** (`wormhole.c:556-564`).
5. `num_entries = ioread32(base_addr + 4)` (`wormhole.c:566`).
6. For each entry at `tags_addr + i*4`: `tag_id = entry & 0xFFFF`, `offset = (entry>>16)&0xFFFF`, `addr = data_addr + offset*4`. Skip (with error log) if `addr` not in CSM. If `tag_id < 128` cache `telemetry_tag_cache[tag_id] = wh_arc_addr_to_sysreg(addr)` — i.e. **a BAR4 offset** (`wormhole.c:568-581`).

`wormhole_read_telemetry_tag(tag_id, *value)` (`wormhole.c:520-534`), class op `.read_telemetry_tag`:
- `tag_id >= TELEM_TAG_CACHE_SIZE (128)` → **-EINVAL** (`wormhole.c:525-526`).
- cached offset `== 0` → **-ENODATA** (`wormhole.c:529-530`).
- else `*value = ioread32(bar4_mapping + offset)` (`wormhole.c:532`).

> **Porting note:** WH stores a **BAR4 sysreg offset** in `telemetry_tag_cache` and reads telemetry via a direct MMIO load (`bar4_mapping + offset`). Blackhole stores a **raw CSM address** and reads via NOC (`device.h:76-79`). The tag enum (`telemetry.h:15-41`) and `TELEM_TAG_CACHE_SIZE = 128` are shared, but the read mechanism is per-arch.

### Deferred FW-ready polling [WH-only]

Because ARC FW may not be ready when the driver probes/resets, WH defers telemetry setup to a delayed work item:

`is_fw_ready_for_telemetry` (`wormhole.c:640-647`): both telemetry pointers must be in CSM.

`fw_ready_work_func` (`wormhole.c:654-681`): if `tt_dev->detached` return; if FW not ready, reschedule after 1000 ms while `telemetry_retries-- > 0`, else log timeout; once ready, `telemetry_probe`, then first-time register the sysfs telemetry group and hwmon (guarded by `telemetry_group_registered` / `hwmon_dev`).

`wormhole_probe_telemetry` (`wormhole.c:739-746`), class op `.probe_telemetry`: sets `telemetry_retries = 120` and schedules the work immediately (delay 0). Returns 0. (120 retries × 1 s ⇒ ~2 min budget.)

`wormhole_init_telemetry` (`wormhole.c:748-764`), class op `.init_telemetry`: registers the PCIe perf-counters sysfs group (sets `pcie_perf_group_registered`), then calls `wormhole_probe_telemetry`.

`wormhole_cleanup_telemetry` (`wormhole.c:766-784`), class op `.cleanup_telemetry`: unregister hwmon, telemetry group, and PCIe perf group (each guarded by its flag).

> **Porting note:** The remove path (`enumerate.c:410-413`) special-cases Wormhole to `cancel_delayed_work_sync(&wh->fw_ready_work)` **before** the generic teardown. A Windows port must have an equivalent cancel-and-flush of the FW-ready polling timer/DPC before unmapping BAR4, or a late poll will touch freed MMIO.

### sysfs telemetry attributes

`wh_sysfs_attributes[]` (`wormhole.c:370-384`) maps 13 tags (aiclk, axiclk, arcclk, board serial, card type, fw bundle ver, m3app/m3bl/arc/eth fw ver, ttflash ver, asic id, heartbeat) to sysfs files via shared show callbacks (`tt_sysfs_show_*`, `telemetry.h:52-56`).

### hwmon

`wormhole_hwmon_init` (`wormhole.c:619-638`) registers an hwmon device named `"wormhole"` with `wh_hwmon_chip_info` (`wormhole.c:614-617`) and emits a `KOBJ_CHANGE` uevent. Tag→sensor mapping in `wh_hwmon_attrs` (`wormhole.c:586-596`): ASIC_TEMP→temp_input, THM_LIMIT_THROTTLE→temp_max, VCORE→in_input, VDD_LIMITS→in_max, CURRENT→curr_input, TDC_LIMIT_MAX→curr_max, POWER→power_input, TDP_LIMIT_MAX→power_max. Labels at `wormhole.c:598-604`.

### PCIe performance counters [WH-only]

NIU counters at `NIU_COUNTERS_START = NOC2AXI_START + 0x200` (`wormhole.c:386`); NOC1 bank offset `NIU_NOC1_OFFSET = 0x8000` (`wormhole.c:387`). `wh_show_pcie_single_counter` reads `ioread32(bar4 + NIU_COUNTERS_START + 4*counter_offset + noc*0x8000)` (`wormhole.c:389-397`). Counter type IDs (`wormhole.c:412-417`): `SLV_POSTED_WR_DATA_WORD_RECEIVED 0x39`, `SLV_NONPOSTED_WR_DATA_WORD_RECEIVED 0x38`, `SLV_RD_DATA_WORD_SENT 0x33`, `MST_POSTED_WR_DATA_WORD_SENT 0x9`, `MST_NONPOSTED_WR_DATA_WORD_SENT 0x8`, `MST_RD_DATA_WORD_RECEIVED 0x3`. Exposed per-NOC (suffix 0/1) in sysfs group `pcie_perf_counters` (`wormhole.c:400-447`).

---

## 8. Init / cleanup lifecycle

`wormhole_init` (`wormhole.c:683-716`), class op `.init_device`:
- `INIT_DELAYED_WORK(&fw_ready_work, fw_ready_work_func)`.
- Alloc `telemetry_attrs` array (`+1` for NULL terminator); return false (fail) if NULL.
- `pci_iomap` BAR2 then BAR4; failure unwinds (`fail_bar4` iounmaps BAR2, `fail_bar2` returns false) (`wormhole.c:696-715`).
- `set_bit(KERNEL_TLB_INDEX, tt_dev->tlbs)`; `mutex_init(&kernel_tlb_mutex)`.
- Build telemetry attr group with visibility `tt_sysfs_telemetry_is_visible`.

`wormhole_init_hardware` (`wormhole.c:718-735`), class op `.init_hardware`:
1. `map_bar4_to_system_registers` (always).
2. **Only if `arc_l2_is_running`:** send `WH_FW_MSG_CURR_DATE` (via `wormhole_send_curr_date`), `WH_FW_MSG_ASTATE0` (10 ms), `update_device_index`, `wormhole_complete_pcie_init`, and `WH_FW_MSG_UPDATE_M3_AUTO_RESET_TIMEOUT` with `auto_reset_timeout` (`wormhole.c:723-732`).

`update_device_index` (`wormhole.c:464-471`): sends `WH_FW_MSG_PCIE_INDEX` with `arg0 = ordinal | 0x80` (`INDEX_VALID`), 10 ms timeout (`10*1000` µs, `wormhole.c:470`).

`wormhole_send_curr_date` (`wormhole.c:321-360`) + `month_lookup` (`wormhole.c:303-319`): packs current UTC date/time (seconds since 2020-01-01, leap handling) into two 16-bit args and sends `WH_FW_MSG_CURR_DATE` (1 ms timeout). Purely informational to FW.

`wormhole_cleanup_hardware` (`wormhole.c:786-791`), class op `.cleanup_hardware` **and** `.reboot`: if `!detached`, call `wormhole_shutdown_firmware`.
`wormhole_shutdown_firmware` (`wormhole.c:293-301`): if hung, return false; else send `WH_FW_MSG_ASTATE3` (10 ms) — puts ARC into A3 low-power state.

`wormhole_cleanup` (`wormhole.c:793-801`), class op `.cleanup_device`: `pci_iounmap` BAR2 and BAR4 (each guarded non-NULL).

Removal order (`enumerate.c:404-446`): cancel `fw_ready_work` (WH-only) → set `detached` under `chardev_mutex` → cancel `power_down_work` → if vendor id readable, `cleanup_hardware` (A3) → `cleanup_telemetry` → `cleanup_device` (unmap BARs).

---

## 9. Reset

`wormhole_reset(tt_dev, reset_flag)` (`wormhole.c:473-518`), class op `.reset`:
- `reset_arg = 3` iff `reset_flag == TENSTORRENT_RESET_DEVICE_ASIC_DMC_RESET (5)` else `0` (`wormhole.c:477`; flag values `ioctl.h:143-151`).
- Probe responsiveness with `WH_FW_MSG_NOP` (1 ms; `1000` µs) (`wormhole.c:481`).
- If unresponsive: if `auto_reset_timeout == 0` (watchdog disabled), log and **return false** (`wormhole.c:488-491`). Else loop for `auto_reset_timeout*1000 + 500` ms, each iteration `pcie_hot_reset_and_restore_state` then retry NOP; between tries `msleep_interruptible(1000)` (signal → return false) (`wormhole.c:493-505`).
- If responsive: `set_reset_marker(pdev)`, then `WH_FW_MSG_TRIGGER_RESET` with `arg0=reset_arg` and **`timeout_us=0`** (fire-and-forget), return **true** (assumes success) (`wormhole.c:508-513`).
- Still unresponsive → log, return false (`wormhole.c:516-517`).

`auto_reset_timeout` default 10 s (`module.c:48`).

### Save/restore reset state (MPS preservation) [WH-only mechanism]

`.save_reset_state` = `wormhole_save_reset_state` (`wormhole.c:968-976`), `.restore_reset_state` = `wormhole_restore_reset_state` (`wormhole.c:978-988`):
- `open_dbi` writes `DBI_ENABLE = 0x00200000` to SR6 and SR7 (routes all outbound NOC traffic to DBI) (`wormhole.c:958-961`); `close_dbi` writes 0 back (`wormhole.c:963-966`).
- Read PCIe DEVICE_CONTROL via `noc_read32(PCIE_NOC_X=0, PCIE_NOC_Y=3, PCIE_DBI_ADDR + DBI_DEVICE_CONTROL_DEVICE_STATUS, noc=0)`; `PCIE_DBI_ADDR = 0x800000000`, `DBI_DEVICE_CONTROL_DEVICE_STATUS = 0x78` (`wormhole.c:92`, `pcie.h:8`).
- Save: extract `PCI_EXP_DEVCTL_PAYLOAD` field into `wh->saved_mps` (`wormhole.c:973-974`).
- Restore: clear the payload field and write back `saved_mps` via `noc_write32` (`wormhole.c:983-986`).

> **Porting note:** `open_dbi` warns (`wormhole.c:956-957`) that DBI routing disrupts normal NOC DMA — it must only be invoked when there is no outbound traffic. The Windows port must gate these around quiesced-DMA points (reset), and note the `open_dbi`/`noc_read32`/`close_dbi` sequence holds `kernel_tlb_mutex` transitively via `noc_read32`.

### Shared PCIe reset helpers [SHARED]

In `pcie.c` (generic, not WH-named except `wormhole_complete_pcie_init`):
- `safe_pci_restore_state` (`pcie.c:43-59`): test-reads vendor id first (guards a long capability walk), then `pci_restore_state` + `pci_save_state`.
- `pcie_hot_reset_and_restore_state` (`pcie.c:61-90`): asserts `PCI_BRIDGE_CTL_BUS_RESET` on the upstream bridge, `msleep(2)`, deassert, `msleep(500)`, then `poll_pcie_link_up(10000 ms)` + `safe_pci_restore_state`. Manages `ignore_hotplug`.
- `poll_pcie_link_up` (`pcie.c:24-41`): polls vendor id == `0x1E52` every 100 ms until timeout.
- `set_reset_marker` / `is_reset_marker_zero` (`pcie.c:140-158`): use the `PCI_COMMAND_PARITY` bit in config-space `PCI_COMMAND` as a reset marker (set before reset, expected cleared to 0 after).
- `pcie_timer_interrupt` (`pcie.c:133-138`): config-space writes to `INTERFACE_TIMER_TARGET_OFF 0x934` / `INTERFACE_TIMER_CONTROL_OFF 0x930`.

### `wormhole_complete_pcie_init` [WH-only]

`pcie.c:92-131` (declared in `pcie.h:11`). Called from `wormhole_init_hardware`. Loops up to `reset_limit` (default 10, `module.c:44`) times:
- Reads bridge target link speed (`PCI_EXP_LNKCTL2 & PCI_EXP_LNKCTL2_TLS`) and subsystem vendor id.
- Sends `FW_MSG_PCIE_RETRAIN (0xB6)` with `arg0 = target_link_speed | (last_retry<<15)`, `arg1 = subsys_vendor_id`, 200 ms timeout, capturing `exit_code`.
- `exit_code == 0` → success. Otherwise (unless last retry) `pci_save_state` + `pcie_hot_reset_and_restore_state` and try again. Uses `FW_MSG_PCIE_RETRAIN` (`pcie.c:16`); the interface-timer/force-pending constants (`pcie.c:17-22`) belong to `pcie_timer_interrupt`, not this function.

---

## 10. Power state [WH wrapper; queue core SHARED]

`wormhole_set_power_state(tt_dev, power_state)` (`wormhole.c:1034-1053`), class op `.set_power_state`:
- `msg.header = ARC_MSG_TYPE_POWER_SETTING (0xC0) | (validity << 8) | (power_flags << 16)` (`wormhole.c:1040`; constant `wormhole.c:121`).
- `BUILD_BUG_ON(sizeof(power_settings) != sizeof(msg.payload))` — the 14× `u16` `power_settings` array (`ioctl.h:410`) exactly fills the 7× `u32` payload (28 bytes) (`wormhole.c:1041-1042`).
- `send_arc_message` (queue protocol, §4); failure → **-EINVAL** (`wormhole.c:1047-1052`).

`validity` bitfield semantics and `TT_POWER_FLAG_*` bits at `ioctl.h:388-410`.

---

## Key constants table

| Name | Value | Source cite |
|------|-------|-------------|
| PCI vendor / Wormhole device | `0x1E52` / `0x401E` | `enumerate.h:15,17` |
| PCI Grayskull device (deprecated, NULL class) | `0xFACA` | `enumerate.h:16`, `module.c:65-66` |
| `dma_address_bits` | `32` | `wormhole.c:1058` |
| `noc_dma_limit` | `0xFFFE0000 - 1` | `wormhole.c:1059` |
| `noc_pcie_offset` / `PCIE_DBI_ADDR` | `0x800000000` | `wormhole.c:1060, 92` |
| `BAR4_SOC_TARGET_ADDRESS` | `0x1E000000` | `wormhole.c:76` |
| `RESET_UNIT_START` (BAR4 off) | `0x01F30000` | `wormhole.c:78` |
| `ARC_CSM_START` (BAR4 off) | `0x01E80000` | `wormhole.c:79` |
| `TLB_REGS_START` (BAR4 off) | `0x01C00000` | `wormhole.c:80` |
| `NOC2AXI_START` (BAR4 off) | `0x01D02000` | `wormhole.c:81` |
| `KERNEL_TLB_START` (BAR4 off) | `0x0` | `wormhole.c:88` |
| `ARC_TELEMETRY_PTR/DATA/QCB` | RESET_UNIT + `0x1D0/0x1D4/0x1D8` | `wormhole.c:83,84,119` |
| `SCRATCH_REG(n)` | `0x60 + 4n` | `wormhole.c:90` |
| `ARC_MISC_CNTL_REG` / IRQ0 bit | `0x100` / `(1<<16)` | `wormhole.c:105-106` |
| `POST_CODE_ARC_L2` / mask | `0xC0DE0000` / `0xFFFF0000` | `wormhole.c:102-103` |
| `WH_FW_MESSAGE_PRESENT` | `0xAA00` | `wormhole.c:112` |
| `DBI_ENABLE` | `0x00200000` | `wormhole.c:97` |
| `DBI_DEVICE_CONTROL_DEVICE_STATUS` | `0x78` | `pcie.h:8` |
| `PCIE_NOC_X / Y` | `0` / `3` | `wormhole.c:93-94` |
| `ARC_CSM_BASE` / `ARC_CSM_SIZE` | `0x10000000` / `(1<<19)` | `telemetry.h:73-74` |
| TLB kinds counts | `156 / 10 / 20` | `wormhole.c:20,25,30` |
| TLB shifts | `20 / 21 / 24` | `wormhole.c:21,26,31` |
| `TLB_WINDOW_COUNT` / `KERNEL_TLB_INDEX` | `186` / `185` | `wormhole.c:36, 87` |
| `WH_NOC_BITS` | `36` | `wormhole.c:37` |
| `IATU_BASE` (BAR2 rel) / stride / regions | `0x1200` / `0x100` / `16` | `wormhole.c:46,50,49` |
| `ARC_MSG_QUEUE_HEADER_SIZE` | `32` | `msgqueue.h:16` |
| `ARC_MSG_TIMEOUT_MS` | `1000` | `msgqueue.h:17` |
| `ARC_MSG_READY_MS` | `500` | `wormhole.c:120` |
| `ARC_MSG_TYPE_POWER_SETTING` | `0xC0` | `wormhole.c:121` |
| Scratch msg IDs | NOP `0x11`, PCIE_INDEX `0x51`, TRIGGER_RESET `0x56`, ASTATE0 `0xA0`, ASTATE3 `0xA3`, PCIE_RETRAIN `0xB6`, CURR_DATE `0xB7`, M3_TIMEOUT `0xBC` | `wormhole.c:39-43,114-116`; `pcie.c:16` |
| `TELEM_TAG_CACHE_SIZE` | `128` | `telemetry.h:13` |
| `auto_reset_timeout` default | `10` (s) | `module.c:48` |
| `reset_limit` default | `10` | `module.c:44` |
| `idle_power_down_grace_ms` default | `5000` | `module.c:56` |
| poll_completion codes | 0 ok, -3 hung, -2 unrecognized, -1 timeout | `wormhole.c:177,182,187,192` |

---

## Open questions

1. **Kernel-TLB BAR4 aperture geometry.** `KERNEL_TLB_START` computes to BAR4 offset `0` (`wormhole.c:88`), and the kernel accesses window index 185 through `bar4_mapping + 0 + offset` while userspace sees the same window as BAR0-relative (`describe_tlb` `bar=0`). The exact hardware relationship between the BAR4 system-register aperture (`0x1E000000`) and the BAR0 kernel TLB window is not spelled out in the code; I documented only the code-visible addressing. A Windows port that reuses index 185 for kernel NOC access must confirm this aperture with hardware docs.
2. **`noc_tlb_non_address_bits` bitfield ABI.** The struct uses `u64`-typed bitfields inside a `union` with a `u32 reg` (`wormhole.c:809-823`). The intended packing (29 bits into the low 32) works under GCC but is not guaranteed identical under MSVC. Exact bit positions should be validated against hardware before relying on struct layout in the port.
3. **`WH_FW_MSG_ASTATE0` duplicate define.** Defined twice with identical value `0xA0` (`wormhole.c:40` and `114`); harmless but worth noting so a port does not treat them as distinct.
4. **`timeout_us == 0` return convention.** `wormhole_send_arc_fw_message_with_args` returns `false` for fire-and-forget sends (`wormhole.c:226-227`), and `wormhole_reset` deliberately uses this for `TRIGGER_RESET` yet returns `true` (`wormhole.c:510-512`). A port must not treat the `false` from a zero-timeout send as an error.
5. **`send_arc_message` is `__maybe_unused` on Wormhole (`wormhole.c:242`)** — it is reachable only through `wormhole_set_power_state`. If the Windows port omits power-state support initially, the entire queue path (and `msgqueue.c`) is dormant for Wormhole but still required for Blackhole.
