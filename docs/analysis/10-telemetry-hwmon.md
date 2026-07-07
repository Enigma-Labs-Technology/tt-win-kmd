# 10. Telemetry and hwmon

## Scope

Files covered (paths relative to tt-kmd repo root unless prefixed):

| File | Lines | Coverage |
|---|---|---|
| `telemetry.c` | 232 | entire file |
| `telemetry.h` | 81 | entire file |
| `wormhole.c` | 1084 | entire file; telemetry glue at 74–84, 370–447, 520–784 |
| `blackhole.c` | 840 | entire file; telemetry glue at 56–74, 332–498, 641–695 |
| `wormhole.h` | 32 | entire file |
| `blackhole.h` | 28 | entire file |
| `device.h` | 127 | entire file (telemetry-related fields) |
| `enumerate.c` | 545 | entire file (init/cleanup call sites, hwmon stub) |
| `chardev.c` | (partial) | 40–130 (device registration), 185–310 (reset re-probe path) |
| `tt-umd/device/api/umd/device/types/telemetry.hpp` | 80 | entire file (cross-reference for the full tag ID space) |

---

## 1. Telemetry data model: firmware-published tag table in ARC CSM

The ARC (management) firmware publishes a telemetry table inside the ARC CSM SRAM window. CSM occupies ARC addresses `0x10000000 .. 0x10080000`:

```c
#define ARC_CSM_BASE 0x10000000
#define ARC_CSM_SIZE (1 << 19)
static inline bool is_range_within_csm(u64 addr, size_t len)
{
	return (addr >= ARC_CSM_BASE) && (addr <= (ARC_CSM_BASE + ARC_CSM_SIZE) - len);
}
```
(telemetry.h:73-78)

Two firmware-owned scratch registers point at the table:

- **Wormhole** — read through the BAR4 mapping of the RESET_UNIT block. BAR4 is iATU-remapped so that it covers SoC addresses starting at `0x1E000000` (wormhole.c:74-76, 449-458). The pointers are `ARC_TELEMETRY_PTR = RESET_UNIT_START + 0x01D0` and `ARC_TELEMETRY_DATA = RESET_UNIT_START + 0x01D4`, with `RESET_UNIT_START = 0x1FF30000 - 0x1E000000` (wormhole.c:78, 83-84).
- **Blackhole** — read over the NOC from the ARC node at `x=8, y=0` (blackhole.c:57-58). `RESET_SCRATCH(N) = 0x80030400 + N*4`; `ARC_TELEMETRY_PTR = RESET_SCRATCH(13)`, `ARC_TELEMETRY_DATA = RESET_SCRATCH(12)` (blackhole.c:59-61). NOC reads go through the kernel-reserved 2M TLB window under `kernel_tlb_mutex` (blackhole.c:243-256).

### Table layout (identical on both architectures)

`base_addr` (the value of `ARC_TELEMETRY_PTR`) points into CSM at:

| Offset from base | Contents |
|---|---|
| +0 | `u32 version`; `major = (v>>16)&0xFF`, `minor = (v>>8)&0xFF`, `patch = v&0xFF` (wormhole.c:556-559, blackhole.c:475-478) |
| +4 | `u32 num_entries` (wormhole.c:566, blackhole.c:485) |
| +8 | array of `num_entries` × `u32` tag entries: `tag_id = entry & 0xFFFF`, `offset = (entry >> 16) & 0xFFFF` (wormhole.c:549, 568-572; blackhole.c:468, 487-491) |

The value for a tag lives at `data_addr + offset * 4` where `data_addr` is the value of `ARC_TELEMETRY_DATA` (wormhole.c:572, blackhole.c:491).

### Probe-time validation (`telemetry_probe`, one per arch)

Both implementations (`wormhole.c:536-584`, `blackhole.c:455-498`):

1. `memset` the tag cache to zero (wormhole.c:545, blackhole.c:464).
2. Reject if `base_addr` or `data_addr` are outside CSM → `dev_err("Telemetry not available")`, return `-ENODEV` (wormhole.c:551-554, blackhole.c:470-473). Note the WH check uses `is_range_within_csm(addr, sizeof(u32))` while BH uses length 1 (blackhole.c:470).
3. Reject `major_ver > 1` → `dev_err("Unsupported telemetry version %u.%u.%u")`, return `-ENOTSUPP` (wormhole.c:561-564, blackhole.c:480-483).
4. Iterate entries, caching addresses for `tag_id < TELEM_TAG_CACHE_SIZE` (128) (wormhole.c:579-580, blackhole.c:493-494).
   - **WH only**: each per-tag address is bounds-checked against CSM; invalid entries log `"Telemetry tag %u has invalid address 0x%08X"` and are skipped (wormhole.c:574-577).
   - **BH**: no per-tag validation at probe; validation is deferred to read time via `csm_read32` (blackhole.c:270-277).

### The tag cache is an *address* cache, not a value cache

```c
// Per-device tag-to-address cache, indexed by tag ID.
// Populated by telemetry_probe(); zero means tag not available.
// The stored value is arch-specific: WH stores a BAR4 sysreg offset,
// BH stores a raw CSM address for NOC reads.
u64 telemetry_tag_cache[TELEM_TAG_CACHE_SIZE];
```
(device.h:75-79; `TELEM_TAG_CACHE_SIZE` = 128, telemetry.h:13)

WH stores `wh_arc_addr_to_sysreg(addr) = ARC_CSM_START + (addr - ARC_CSM_BASE)` — a BAR4 offset (wormhole.c:140-143, 580). BH stores the raw CSM address (blackhole.c:491-494). A cached value of **0 means "tag not present"**; this drives both hwmon and sysfs attribute visibility.

**There is no value caching and no periodic refresh in the driver**: every sysfs/hwmon read performs a live MMIO (WH) or NOC (BH) read of the firmware-maintained value. The firmware updates values on its own schedule (tag 5 `UPDATE_TELEM_SPEED` exists in the tag space, tt-umd/device/api/umd/device/types/telemetry.hpp:16, but the KMD does not read it).

---

## 2. Tag IDs — full enumeration

### Tags the KMD knows about (telemetry.h:15-41)

| KMD name | ID | Used for |
|---|---|---|
| `TELEMETRY_BOARD_ID` | 1 | `tt_serial` (u64: tag 1 = high, tag 2 = low), `tt_card_type` |
| `TELEMETRY_VCORE` | 6 | hwmon `in*_input` (mV) |
| `TELEMETRY_POWER` | 7 | hwmon `power1_input` (W → µW) |
| `TELEMETRY_CURRENT` | 8 | hwmon `curr1_input` (A → mA) |
| `TELEMETRY_VDD_LIMITS` | 9 | hwmon `in*_max` (max in upper 16 bits, mV) |
| `TELEMETRY_THM_LIMIT_SHUTDOWN` | 10 | defined, **unused** |
| `TELEMETRY_ASIC_TEMP` | 11 | hwmon `temp1_input` (16.16 fixed → m°C) |
| `TELEMETRY_AICLK` | 14 | sysfs `tt_aiclk` |
| `TELEMETRY_AXICLK` | 15 | sysfs `tt_axiclk` |
| `TELEMETRY_ARCCLK` | 16 | sysfs `tt_arcclk` |
| `TELEMETRY_ETH_FW_VERSION` | 24 | sysfs `tt_eth_fw_ver` (WH only; special packing) |
| `TELEMETRY_BM_APP_FW_VERSION` | 26 | sysfs `tt_m3app_fw_ver` |
| `TELEMETRY_BM_BL_FW_VERSION` | 27 | sysfs `tt_m3bl_fw_ver` (WH only) |
| `TELEMETRY_FLASH_BUNDLE_VERSION` | 28 | sysfs `tt_fw_bundle_ver` |
| `TELEMETRY_CM_FW_VERSION` | 29 | sysfs `tt_arc_fw_ver` (WH only) |
| `TELEMETRY_FAN_SPEED` | 31 | defined, **unused** |
| `TELEMETRY_TIMER_HEARTBEAT` | 32 | sysfs `tt_heartbeat` |
| `TELEMETRY_FAN_RPM` | 41 | hwmon `fan1_input` (BH only) |
| `TELEMETRY_TDC_LIMIT_MAX` | 55 | hwmon `curr1_max` |
| `TELEMETRY_THM_LIMIT_THROTTLE` | 56 | hwmon `temp1_max` |
| `TELEMETRY_TT_FLASH_VERSION` | 58 | sysfs `tt_ttflash_ver` (WH only) |
| `TELEMETRY_THERM_TRIP_COUNT` | 60 | sysfs `tt_therm_trip_count` (BH only) |
| `TELEMETRY_ASIC_ID` | 61 | sysfs `tt_asic_id` (u64: tag 61 = high, tag 62 = low) |
| `TELEMETRY_AICLK_LIMIT_MAX` | 63 | defined, **unused** |
| `TELEMETRY_TDP_LIMIT_MAX` | 64 | hwmon `power1_max` |

The `u64_hex` show routine implicitly consumes `tag_id + 1` as the low word (telemetry.c:56-64), so tags 2 (`BOARD_ID_LOW`) and 62 (`ASIC_ID_LOW`) are consumed even though the KMD has no named constant for them.

### Full tag ID space (cross-reference, tt-umd/device/api/umd/device/types/telemetry.hpp:11-78)

`BOARD_ID_HIGH=1, BOARD_ID_LOW=2, ASIC_ID=3, HARVESTING_STATE=4, UPDATE_TELEM_SPEED=5, VCORE=6, TDP=7, TDC=8, VDD_LIMITS=9, THM_LIMIT_SHUTDOWN=10, ASIC_TEMPERATURE=11, VREG_TEMPERATURE=12, BOARD_TEMPERATURE=13, AICLK=14, AXICLK=15, ARCCLK=16, L2CPUCLK0..3=17..20, ETH_LIVE_STATUS=21, GDDR_STATUS=22, GDDR_SPEED=23, ETH_FW_VERSION=24, GDDR_FW_VERSION=25, DM_APP_FW_VERSION=26, DM_BL_FW_VERSION=27, FLASH_BUNDLE_VERSION=28, CM_FW_VERSION=29, L2CPU_FW_VERSION=30, FAN_SPEED=31, TIMER_HEARTBEAT=32, TELEMETRY_ENUM_COUNT=33, ENABLED_TENSIX_COL=34, ENABLED_ETH=35, ENABLED_GDDR=36, ENABLED_L2CPU=37, PCIE_USAGE=38, NOC_TRANSLATION=40, FAN_RPM=41, GDDR_0_1_TEMP=42, GDDR_2_3_TEMP=43, GDDR_4_5_TEMP=44, GDDR_6_7_TEMP=45, GDDR_0_1_CORR_ERRS=46, GDDR_2_3_CORR_ERRS=47, GDDR_4_5_CORR_ERRS=48, GDDR_6_7_CORR_ERRS=49, GDDR_UNCORR_ERRS=50, MAX_GDDR_TEMP=51, ASIC_LOCATION=52, BOARD_POWER_LIMIT=53, INPUT_POWER=54, TDC_LIMIT_MAX=55, THM_LIMIT_THROTTLE=56, TT_FLASH_VERSION=58, THERM_TRIP_COUNT=60, ASIC_ID_HIGH=61, ASIC_ID_LOW=62, AICLK_LIMIT_MAX=63, TDP_LIMIT_MAX=64, AICLK_ARB_MIN=65, AICLK_ARB_MAX=66, ENABLED_MIN_ARB=67, ENABLED_MAX_ARB=68, NUMBER_OF_TAGS=69`

Note the UMD's `ASIC_ID=3` vs `ASIC_ID_HIGH/LOW=61/62` — the KMD uses 61/62 for `tt_asic_id`. Tag IDs 39, 57, 59 are absent from both enumerations.

---

## 3. The core read path: `tt_telemetry_read32`

```c
int tt_telemetry_read32(struct tenstorrent_device *tt_dev, u16 tag_id, u32 *value)
{
	if (tag_id >= TELEM_TAG_CACHE_SIZE)
		return -EINVAL;
	down_read(&tt_dev->reset_rwsem);
	if (tt_dev->detached) { r = -ENODEV; goto out; }
	if (tt_dev->needs_hw_init) { r = -ENODATA; goto out; }
	r = tt_dev->dev_class->read_telemetry_tag(tt_dev, tag_id, value);
out:
	up_read(&tt_dev->reset_rwsem);
	return r;
}
```
(telemetry.c:9-33)

- **Validation**: `tag_id >= 128` → `-EINVAL`.
- **Locks**: `reset_rwsem` held for read across the hardware access; this is the same lock `tenstorrent_pci_remove` takes for write before unmapping BARs (enumerate.c:444-447), so an in-flight telemetry read cannot race BAR teardown.
- **Device state**: `detached` → `-ENODEV`; `needs_hw_init` (set between an ASIC reset and POST_RESET) → `-ENODATA` (telemetry.c:18-26).

Arch backends:

- **Wormhole** `wormhole_read_telemetry_tag` (wormhole.c:520-534): `tag_id >= 128` → `-EINVAL`; cache entry 0 → `-ENODATA`; else `*value = ioread32(wh->bar4_mapping + offset)`, returns 0.
- **Blackhole** `blackhole_read_telemetry_tag` (blackhole.c:440-453): same `-EINVAL`/`-ENODATA` checks; then `csm_read32` which re-validates CSM bounds (`-EINVAL` if outside, blackhole.c:270-277) and does a NOC read via the kernel TLB window while holding `bh->kernel_tlb_mutex` (blackhole.c:243-256). **Each BH telemetry read reprograms the kernel TLB window** (blackhole.c:228-241).

Any error from this path propagates as the return value of the sysfs/hwmon `read`/`show` callback, i.e. userspace `read(2)` on the attribute fails with that errno.

---

## 4. hwmon exposure

### Registration

- **WH**: `hwmon_device_register_with_info(dev, "wormhole", tt_dev, &wh_hwmon_chip_info, NULL)` with `dev = &tt_dev->pdev->dev` (parent is the **PCI device**) (wormhole.c:619-638). Registration is deferred to `fw_ready_work` (see §7). On failure only a `dev_warn` is emitted; the driver continues without hwmon (wormhole.c:629-632).
- **BH**: `hwmon_device_register_with_info(dev, "blackhole", tt_dev, &bh_hwmon_chip_info, NULL)`, synchronous in `blackhole_init_telemetry`, only if `telemetry_probe` succeeded; failure makes `init_telemetry` return false (blackhole.c:652-672).
- After successful registration both archs emit `kobject_uevent(&tt_dev->dev.kobj, KOBJ_CHANGE)` to tell udev the attributes are ready (wormhole.c:637, blackhole.c:671).
- Shared ops table: `tt_hwmon_ops = { .is_visible, .read, .read_string }` (telemetry.c:228-232).
- enumerate.c:78-82 contains a `!IS_ENABLED(CONFIG_HWMON)` stub for `devm_hwmon_device_register_with_info` — a function the driver **no longer calls** (both archs use the non-devm `hwmon_device_register_with_info`), so a `CONFIG_HWMON=n` build would fail to link; the stub appears stale.

### Channels (tag → hwmon attribute mapping)

Wormhole (`wh_hwmon_attrs`, wormhole.c:586-596) and Blackhole (`bh_hwmon_attrs`, blackhole.c:400-411) share eight entries; BH adds one fan entry:

| Tag | hwmon type | hwmon attr | sysfs file (hwmon ABI) | arch |
|---|---|---|---|---|
| `TELEMETRY_ASIC_TEMP` (11) | `hwmon_temp` | `temp_input` | `temp1_input` | both |
| `TELEMETRY_THM_LIMIT_THROTTLE` (56) | `hwmon_temp` | `temp_max` | `temp1_max` | both |
| `TELEMETRY_VCORE` (6) | `hwmon_in` | `in_input` | `in0_input` | both |
| `TELEMETRY_VDD_LIMITS` (9) | `hwmon_in` | `in_max` | `in0_max` | both |
| `TELEMETRY_CURRENT` (8) | `hwmon_curr` | `curr_input` | `curr1_input` | both |
| `TELEMETRY_TDC_LIMIT_MAX` (55) | `hwmon_curr` | `curr_max` | `curr1_max` | both |
| `TELEMETRY_POWER` (7) | `hwmon_power` | `power_input` | `power1_input` | both |
| `TELEMETRY_TDP_LIMIT_MAX` (64) | `hwmon_power` | `power_max` | `power1_max` | both |
| `TELEMETRY_FAN_RPM` (41) | `hwmon_fan` | `fan_input` | `fan1_input` | **BH only** |

Channel declarations: WH `HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT | HWMON_T_LABEL | HWMON_T_MAX)`, and likewise for in/curr/power (wormhole.c:606-612); BH adds `HWMON_CHANNEL_INFO(fan, HWMON_F_INPUT | HWMON_F_LABEL)` (blackhole.c:426-433). One channel per sensor type. (The concrete file names in the table follow the standard Linux hwmon sysfs ABI numbering — voltage channels start at 0, all others at 1 — given channel count 1 per type in these tables.)

### Labels — **the names differ between architectures**

WH (`wh_hwmon_labels`, wormhole.c:598-604): `"asic1_temp"`, `"vcore1"`, `"current1"`, `"power1"`.
BH (`bh_hwmon_labels`, blackhole.c:391-398): `"asic_temp"`, `"vcore"`, `"current"`, `"power"`, `"fan_rpm"`.

Labels are served by `tt_hwmon_read_string` (telemetry.c:212-226, `-EOPNOTSUPP` if no match) and are **always visible** regardless of tag availability (telemetry.c:155-158).

### Scaling arithmetic (`tt_hwmon_read`, telemetry.c:170-210)

```c
if (type == hwmon_temp) {
	if (attr == hwmon_temp_input) {
		// ASIC_TEMPERATURE is 16.16 fixed-point
		u32 int_part = raw >> 16;
		u32 frac_part = raw & 0xFFFF;
		*val = (int_part * 1000) + ((frac_part * 1000) / 0x10000);
	} else {
		// Limit tags are plain degrees C
		*val = raw * 1000;
	}
} else if (type == hwmon_curr) {
	*val = raw * 1000;	// Convert A to mA
} else if (type == hwmon_power) {
	*val = raw * 1000000;	// Convert W to uW
} else if (type == hwmon_in) {
	if (attr == hwmon_in_max)
		raw = (raw >> 16) & 0xFFFF;  // VDD_LIMITS: max in upper 16
	*val = raw;		// Reported in mV
} else if (type == hwmon_fan) {
	*val = raw;		// Reported in RPM
}
```

Firmware raw units → hwmon ABI units:

| Channel | FW raw format | Driver output (hwmon ABI) |
|---|---|---|
| `temp1_input` | 16.16 fixed-point °C | milli-°C |
| `temp1_max` | integer °C | milli-°C (`*1000`) |
| `curr1_input`/`curr1_max` | integer A | mA (`*1000`) |
| `power1_input`/`power1_max` | integer W | µW (`*1000000`) |
| `in0_input` | integer mV | mV (as-is) |
| `in0_max` | packed; max in bits 31:16, mV | mV (`(raw>>16)&0xFFFF`) |
| `fan1_input` | RPM | RPM (as-is) |

Unmatched type/attr → `-EOPNOTSUPP` (telemetry.c:209).

### Visibility

`tt_hwmon_is_visible` (telemetry.c:149-168): label attributes → always `S_IRUGO` (0444); value attributes → `S_IRUGO` only if `telemetry_tag_cache[tag_id] != 0`, else hidden (mode 0). Visibility is evaluated by the hwmon core at registration time, so a tag that appears only after a firmware update becomes visible after the next re-registration (device removal/re-probe), not merely after a tag-cache refresh.

---

## 5. Non-hwmon sysfs attributes (`telemetry_group` on the class device)

Attached via `device_add_group(&tt_dev->dev, &tt_dev->telemetry_group)` — i.e. on the **tenstorrent class device** (`/sys/class/tenstorrent/tenstorrent!<N>`, name set by `dev_set_name(&tt_dev->dev, TENSTORRENT "/%d", ...)`, chardev.c:106; `TENSTORRENT` = `"tenstorrent"`, enumerate.h:13), not on the PCI device. Group construction: `telemetry_attrs` array built in `init_device`, with `telemetry_group.is_visible = tt_sysfs_telemetry_is_visible` (wormhole.c:705-708, blackhole.c:612-615). All attributes are mode `S_IRUGO` (0444). Attributes whose tag is absent from the tag cache are hidden entirely (telemetry.c:130-143, mode 0).

### Wormhole attribute list (`wh_sysfs_attributes`, wormhole.c:370-384)

| Name | Tag | Show fn | Format |
|---|---|---|---|
| `tt_aiclk` | 14 | `tt_sysfs_show_u32_dec` | `"%u\n"` raw FW value (no scaling) |
| `tt_axiclk` | 15 | u32_dec | same |
| `tt_arcclk` | 16 | u32_dec | same |
| `tt_serial` | 1 (+2) | `tt_sysfs_show_u64_hex` | `"%08X%08X\n"` (hi=tag, lo=tag+1) |
| `tt_card_type` | 1 | `tt_sysfs_show_card_type` | decoded string, see below |
| `tt_fw_bundle_ver` | 28 | `tt_sysfs_show_u32_ver` | `"%u.%u.%u.%u\n"` |
| `tt_m3app_fw_ver` | 26 | u32_ver | same |
| `tt_ttflash_ver` | 58 | u32_ver | same |
| `tt_m3bl_fw_ver` | 27 | u32_ver | same |
| `tt_arc_fw_ver` | 29 | u32_ver | same |
| `tt_eth_fw_ver` | 24 | u32_ver | `"%u.%u.%u\n"` (ETH packing) |
| `tt_asic_id` | 61 (+62) | u64_hex | `"%08X%08X\n"` |
| `tt_heartbeat` | 32 | u32_dec | `"%u\n"` |

### Blackhole attribute list (`bh_sysfs_attributes`, blackhole.c:413-424)

| Name | Tag | Show fn |
|---|---|---|
| `tt_aiclk` | 14 | u32_dec |
| `tt_axiclk` | 15 | u32_dec |
| `tt_arcclk` | 16 | u32_dec |
| `tt_serial` | 1 (+2) | u64_hex |
| `tt_card_type` | 1 | card_type |
| `tt_fw_bundle_ver` | 28 | u32_ver |
| `tt_m3app_fw_ver` | 26 | u32_ver |
| `tt_asic_id` | 61 (+62) | u64_hex |
| `tt_heartbeat` | 32 | u32_dec |
| `tt_therm_trip_count` | 60 | u32_dec |

Differences: BH **lacks** `tt_ttflash_ver`, `tt_m3bl_fw_ver`, `tt_arc_fw_ver`, `tt_eth_fw_ver`; BH **adds** `tt_therm_trip_count`.

### Formatting details

- `tt_sysfs_show_u32_dec`: `scnprintf(buf, PAGE_SIZE, "%u\n", value)` (telemetry.c:35-47).
- `tt_sysfs_show_u64_hex`: reads `tag_id` (high word) and `tag_id + 1` (low word), prints `"%08X%08X\n"` — 16 uppercase hex digits, zero-padded, no `0x` prefix (telemetry.c:49-65).
- `tt_sysfs_show_u32_ver`: default packing `major=(v>>24)&0xFF, minor=(v>>16)&0xFF, patch=(v>>8)&0xFF, ver=v&0xFF` → `"%u.%u.%u.%u\n"`. Exception when `tag_id == TELEMETRY_ETH_FW_VERSION`: `major=(v>>16)&0xFF, minor=(v>>12)&0xF, patch=v&0xFFF` → `"%u.%u.%u\n"` (telemetry.c:67-93).
- `tt_sysfs_show_card_type`: `card_type = (value >> 4) & 0xFFFF` from `TELEMETRY_BOARD_ID` high word, decoded (telemetry.c:95-128):

```c
case 0x14: card_name = "n300"; break;
case 0x18: card_name = "n150"; break;
case 0x35: card_name = "galaxy-wormhole"; break;
case 0x36: card_name = "p100"; break;
case 0x40: card_name = "p150a"; break;
case 0x41: card_name = "p150b"; break;
case 0x42: card_name = "p150c"; break;
case 0x43: card_name = "p100a"; break;
case 0x44: card_name = "p300b"; break;
case 0x45: card_name = "p300a"; break;
case 0x46: card_name = "p300c"; break;
case 0x47: card_name = "galaxy-blackhole"; break;
default: card_name = "unknown"; break;
```
(telemetry.c:111-124)

### tt_heartbeat and thermal-trip counter

- `tt_heartbeat` exposes `TELEMETRY_TIMER_HEARTBEAT` (tag 32) as a raw decimal `u32`. The driver applies no interpretation; it is a firmware-incremented counter that tools poll to detect a hung ARC. Present on both archs (wormhole.c:383, blackhole.c:422).
- `tt_therm_trip_count` exposes `TELEMETRY_THERM_TRIP_COUNT` (tag 60) as raw decimal, **Blackhole only** (blackhole.c:423). The thermal *shutdown* limit tag (10) is defined but never exposed (telemetry.h:21, no users).

---

## 6. PCIe perf counter sysfs group (non-telemetry but same lifecycle)

Both archs register an `attribute_group` named `"pcie_perf_counters"` on the class device inside `init_telemetry` (wormhole.c:753-757, blackhole.c:646-650); registration failure is only logged (`"PCIe perf counters unavailable: %d"`).

Twelve read-only attributes per arch — six counters × two NOCs, names suffixed `0`/`1` (wormhole.c:419-447, blackhole.c:361-389):
`slv_posted_wr_data_word_received{0,1}`, `slv_nonposted_wr_data_word_received{0,1}`, `slv_rd_data_word_sent{0,1}`, `mst_posted_wr_data_word_sent{0,1}`, `mst_nonposted_wr_data_word_sent{0,1}`, `mst_rd_data_word_received{0,1}` — each printed `"%u\n"`.

Counter register indices (identical on both archs, wormhole.c:412-417, blackhole.c:354-359):

```c
#define SLV_POSTED_WR_DATA_WORD_RECEIVED 0x39
#define SLV_NONPOSTED_WR_DATA_WORD_RECEIVED 0x38
#define SLV_RD_DATA_WORD_SENT 0x33
#define MST_POSTED_WR_DATA_WORD_SENT 0x9
#define MST_NONPOSTED_WR_DATA_WORD_SENT 0x8
#define MST_RD_DATA_WORD_RECEIVED 0x3
```

Address computation:

- WH: `ioread32(bar4 + NIU_COUNTERS_START + 4*counter + noc*0x8000)` where `NIU_COUNTERS_START = NOC2AXI_START + 0x200` and `NOC2AXI_START = 0x1FD02000 - 0x1E000000` (wormhole.c:81, 386-396).
- BH: `ioread32(noc2axi_cfg + 0x4200 + 4*counter + noc*0x10000)` (`NOC_STATUS_OFFSET` 0x4200, `NOC1_NOC2AXI_OFFSET` 0x10000, blackhole.c:47-48, 332-339). `noc2axi_cfg` is a BAR0 range mapping at `0x1FD00000` length `0x100000` (blackhole.c:44-45, 589).

**These show functions perform raw MMIO with no `reset_rwsem`, no `detached` check, and no tag gating.** Safety at removal relies solely on ordering: `cleanup_telemetry` removes the groups (which waits for in-flight sysfs callbacks) *before* `cleanup_device` unmaps BARs (enumerate.c:436-447).

---

## 7. Lifecycle, deferral, and update intervals

### First registration

`tenstorrent_pci_probe` calls `device_class->init_telemetry(tt_dev)` only when `init_hardware` succeeded (`!needs_hw_init`) (enumerate.c:370, 382-383).

- **Blackhole** (`blackhole_init_telemetry`, blackhole.c:641-675): synchronous — register perf-counter group, run `telemetry_probe`, and on success add the telemetry group, set `hwmon_attributes`/`hwmon_labels`, register hwmon, emit `KOBJ_CHANGE`. If `telemetry_probe` fails, the telemetry group and hwmon are simply never registered (function still returns true).
- **Wormhole** (`wormhole_init_telemetry`, wormhole.c:748-764): registers the perf-counter group, then defers the rest to a delayed work item because "On some WH systems, ARC firmware may not have finished initializing by the time the PCI driver probes" (wormhole.c:649-653). `wormhole_probe_telemetry` sets `wh->telemetry_retries = 120` and schedules `fw_ready_work` immediately (wormhole.c:739-746). `fw_ready_work_func` (wormhole.c:654-681):
  - bails if `tt_dev->detached`;
  - checks readiness by validating both telemetry pointers against CSM (`is_fw_ready_for_telemetry`, wormhole.c:640-647); if not ready, reschedules itself every **1000 ms**, up to **120 retries** (~2 minutes), then logs `"Timed out waiting for FW telemetry"`;
  - once ready: `telemetry_probe`, then first-time-only `device_add_group` (guarded by `wh->telemetry_group_registered`, wormhole.h:23) and hwmon registration (guarded by `tt_dev->hwmon_dev`).

### Re-probe after reset

`TENSTORRENT_RESET_DEVICE_POST_RESET` re-runs `init_hardware` and then `probe_telemetry` "in case firmware was updated before this reset" (chardev.c:274-283). WH's `probe_telemetry` restarts the polling work (wormhole.c:739-746); BH's is the synchronous `telemetry_probe` (blackhole.c:828). sysfs groups and the hwmon device stay registered across resets; only the address cache is refreshed. While `needs_hw_init` is true (between ASIC reset and POST_RESET), all telemetry reads return `-ENODATA` (telemetry.c:23-26).

### Teardown (device removal / driver shutdown)

Order in `tenstorrent_pci_remove` (also used as the PCI `shutdown` callback, enumerate.c:532):

1. WH only: `cancel_delayed_work_sync(&wh->fw_ready_work)` (enumerate.c:410-413).
2. `detached = true` under `chardev_mutex` (enumerate.c:423-425).
3. `cleanup_hardware` (A3/ASTATE3) if the device still answers config reads (enumerate.c:432-434).
4. `cleanup_telemetry` — "Tear down hwmon/sysfs interfaces before unmapping BARs. This removes the sysfs files and waits for any in-flight callbacks to complete" (enumerate.c:436-440). Both arch implementations unregister hwmon, then remove the telemetry group, then the perf-counter group, all guarded by their registered flags (wormhole.c:766-784, blackhole.c:677-695).
5. BAR unmap under `reset_rwsem` write (enumerate.c:444-447).

### Update intervals — summary

- Driver-side: **none for values**. Every read is live. The only timing in the telemetry subsystem is the WH firmware-readiness poll (1 s period × 120 retries, wormhole.c:665, 743).
- Firmware-side value refresh cadence is not visible in the KMD (see open questions; tag 5 `UPDATE_TELEM_SPEED` exists in the tag space but is unread).

---

## Porting notes

> **Porting note (interface surface):** Linux exposes two distinct read-only surfaces: (a) hwmon channels with fixed ABI names/units (`temp1_input` in m°C, `in0_input`/`in0_max` in mV, `curr1_input`/`curr1_max` in mA, `power1_input`/`power1_max` in µW, `fan1_input` in RPM, plus `*_label` strings), consumed by generic tools like `lm-sensors`; and (b) the `tt_*` device attributes consumed by tt-smi/tt-flash. Windows has no hwmon; the natural mapping is a device IOCTL or WMI provider. Whatever transport is chosen, the **names, formats, and units above must be reproduced exactly** (including the WH/BH label differences `asic1_temp` vs `asic_temp`, the `%08X%08X` 16-digit uppercase hex serial format, the 4-part vs 3-part version strings, and the m°C/mV/mA/µW/RPM scaling) so that ported tooling behaves identically.

> **Porting note (address cache & visibility):** The tag→address cache and the "hidden when tag absent" semantics should be preserved: a Windows port should return distinct errors mirroring `-EINVAL` (bad tag ≥128), `-ENODEV` (surprise-removed), `-ENODATA` (mid-reset or tag not published) — e.g. `STATUS_INVALID_PARAMETER` / `STATUS_DEVICE_DOES_NOT_EXIST` / `STATUS_DEVICE_NOT_READY` — rather than collapsing them.

> **Porting note (synchronization):** `reset_rwsem` (shared for reads, exclusive during remove/reset) maps to a KMDF-style remove-lock or ERESOURCE; the BH `kernel_tlb_mutex` serializing the shared 2M NOC window is a plain mutex/fast mutex. The WH deferred probe (`fw_ready_work`, 1 s × 120) maps to a KMDF timer or system worker thread; it must be cancelled synchronously before teardown, exactly as enumerate.c:410-413 does.

> **Porting note (teardown ordering):** The invariant "remove externally-visible telemetry interfaces and wait for in-flight readers *before* unmapping BARs" (enumerate.c:436-447) is load-bearing — the PCIe perf-counter reads have no other protection against use-after-unmap.

> **Porting note (uevent):** `KOBJ_CHANGE` after late attribute registration (wormhole.c:637, blackhole.c:671) exists so udev rules re-evaluate. A Windows equivalent (if needed by tooling) would be a custom device interface arrival notification or WMI event.

---

## Key constants table

| Name | Value | Source |
|---|---|---|
| `TELEM_TAG_CACHE_SIZE` | 128 | telemetry.h:13 |
| `ARC_CSM_BASE` | `0x10000000` | telemetry.h:73 |
| `ARC_CSM_SIZE` | `1 << 19` (512 KiB) | telemetry.h:74 |
| WH `BAR4_SOC_TARGET_ADDRESS` | `0x1E000000` | wormhole.c:76 |
| WH `RESET_UNIT_START` | `0x1FF30000 - 0x1E000000` | wormhole.c:78 |
| WH `ARC_CSM_START` | `0x1FE80000 - 0x1E000000` | wormhole.c:79 |
| WH `ARC_TELEMETRY_PTR` | `RESET_UNIT_START + 0x01D0` | wormhole.c:83 |
| WH `ARC_TELEMETRY_DATA` | `RESET_UNIT_START + 0x01D4` | wormhole.c:84 |
| BH `ARC_X`, `ARC_Y` | 8, 0 | blackhole.c:57-58 |
| BH `RESET_SCRATCH(N)` | `0x80030400 + N*4` | blackhole.c:59 |
| BH `ARC_TELEMETRY_PTR` | `RESET_SCRATCH(13)` | blackhole.c:60 |
| BH `ARC_TELEMETRY_DATA` | `RESET_SCRATCH(12)` | blackhole.c:61 |
| Telemetry version field | `major=(v>>16)&0xFF`, reject `major > 1` | wormhole.c:556-563, blackhole.c:475-482 |
| Tag entry packing | `tag=lo16`, `offset=hi16`, value at `data + offset*4` | wormhole.c:570-572, blackhole.c:489-491 |
| temp input fixed point | 16.16 → m°C | telemetry.c:186-189 |
| VDD_LIMITS max | `(raw>>16)&0xFFFF` mV | telemetry.c:199-201 |
| hwmon device names | `"wormhole"` / `"blackhole"` | wormhole.c:628, blackhole.c:664 |
| WH labels | `asic1_temp, vcore1, current1, power1` | wormhole.c:598-604 |
| BH labels | `asic_temp, vcore, current, power, fan_rpm` | blackhole.c:391-398 |
| sysfs group name (PCIe) | `"pcie_perf_counters"` | wormhole.c:444-447, blackhole.c:386-389 |
| PCIe counter IDs | 0x39/0x38/0x33/0x9/0x8/0x3 | wormhole.c:412-417, blackhole.c:354-359 |
| WH NIU counter base | `NOC2AXI_START + 0x200`, NOC1 stride 0x8000 | wormhole.c:386-387 |
| BH NIU counter base | `0x4200`, NOC1 stride 0x10000 | blackhole.c:47-48, 336 |
| WH FW-ready poll | 1000 ms period, 120 retries | wormhole.c:665, 743 |
| ETH FW version packing | `maj=(v>>16)&0xFF, min=(v>>12)&0xF, patch=v&0xFFF` | telemetry.c:80-84 |
| Default version packing | `maj.min.patch.ver` from bytes 3..0 | telemetry.c:87-92 |
| Card-type field | `(BOARD_ID_HIGH >> 4) & 0xFFFF` | telemetry.c:108 |
| Class/device name | `"tenstorrent"`, `"tenstorrent/%d"` | enumerate.h:13, chardev.c:106 |
| Attribute mode | `S_IRUGO` (0444) all telemetry attrs | wormhole.c:370-384, blackhole.c:413-424, telemetry.c:157-163 |

## Open questions

1. **Firmware value refresh cadence**: the KMD never reads tag 5 (`UPDATE_TELEM_SPEED`, tt-umd/device/api/umd/device/types/telemetry.hpp:16) and contains no statement of how often the ARC firmware updates the CSM values or increments `TIMER_HEARTBEAT`. A Windows port that adds any caching layer would need this from firmware documentation, not from this driver.
2. **Clock attribute units**: `tt_aiclk`/`tt_axiclk`/`tt_arcclk` print the raw `u32` with no unit conversion (telemetry.c:35-47). The units are whatever firmware publishes (conventionally MHz), but the KMD neither documents nor enforces this.
3. **BH probe-time address validation gap**: BH `telemetry_probe` caches tag addresses without CSM bounds-checking (blackhole.c:487-495), relying on `csm_read32`'s `-EINVAL` at read time, while WH validates and skips at probe time (wormhole.c:574-577). Whether a port should normalize to the stricter WH behavior (attribute hidden) or keep BH's behavior (attribute visible but read fails) is a policy choice; they are user-visible differences for a malformed FW table.
4. **Stale `CONFIG_HWMON=n` stub**: enumerate.c:78-82 stubs `devm_hwmon_device_register_with_info`, but the driver calls the non-devm `hwmon_device_register_with_info` (wormhole.c:628, blackhole.c:664); a hwmon-less Linux build looks broken. Irrelevant on Windows but indicates the stub is not a behavior to emulate.
5. **PCIe perf counter reads bypass `reset_rwsem`/`detached`** (wormhole.c:389-397, blackhole.c:332-339): safe on Linux only because of teardown ordering (enumerate.c:436-447). It is ambiguous whether concurrent reads during an ASIC reset (`needs_hw_init` true, BARs still mapped) are meaningful; the driver allows them and returns whatever the hardware yields.
6. **`tt_card_type` unknown IDs**: values other than the twelve enumerated (telemetry.c:111-123) print `"unknown"`; new board IDs will need table updates in lockstep with the Linux driver to keep tool behavior identical.
7. **hwmon visibility is registration-time only**: a tag that appears after a POST_RESET re-probe updates the tag cache, but hwmon `is_visible` results were latched at registration. Whether tools depend on late-appearing channels is unknown; the WH deferred-registration path mitigates the common case (slow ARC boot).
