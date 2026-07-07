# 09. ARC Firmware Message Queue Protocol

## Scope

Files covered (full reads unless noted):

- `msgqueue.c` (132 lines) — the ring-buffer push/pop engine.
- `msgqueue.h` (27 lines) — struct + header-offset macros + timeout constant.
- `blackhole.c` — every msgqueue call site: constants (lines 56-74), CSM accessors (270-296), `telemetry_probe` QCB-neighbor discovery (455-498), `send_arc_message` (500-540), `blackhole_reset` (542-570), `blackhole_init_hardware` (620-639), `blackhole_cleanup_hardware` (697-708), `blackhole_set_power_state` (796-811), class table (813-840). Supporting NOC/TLB helpers (200-268).
- `wormhole.c` — constants (74-121), CSM accessors + `wh_arc_addr_to_sysreg` (140-161), the *separate* scratch-register protocol `arc_msg_poll_completion` / `wormhole_send_arc_fw_message[_with_args]` (163-240), the queue-based `send_arc_message` (242-291), `wormhole_set_power_state` (1034-1053), class table (1055-1084).
- `telemetry.c` (232 lines) — **read in full; contains NO msgqueue call sites.** It reads telemetry via `read_telemetry_tag` (a CSM read on the tag cache), never through the ARC message queue. Included here only to record that fact.
- `telemetry.h:73-78` — `ARC_CSM_BASE`/`ARC_CSM_SIZE`/`is_range_within_csm` (the address gate shared by both arch CSM accessors).
- `device.h:110-111` — the `csm_read32`/`csm_write32` function-pointer contract.
- `ioctl.h:396-411` — `struct tenstorrent_power_state` (payload source for the POWER_SETTING message).

`msgqueue.c` is architecture-agnostic: it never touches hardware directly. All device I/O goes through the `csm_read32`/`csm_write32` ops installed by the arch class (`blackhole.c:837-838`, `wormhole.c:1080-1081`).

---

## 1. Data structures and in-memory layout

### 1.1 The message struct — 32 bytes, fixed

```c
struct arc_msg {
	u32 header;
	u32 payload[7];
};
```
(`msgqueue.h:11-14`) → exactly 8×4 = **32 bytes**. This is both the request-entry size and the response-entry size; the code hardcodes `sizeof(struct arc_msg)` everywhere for slot stride (`msgqueue.c:56,75,116`).

### 1.2 Queue header — 32 bytes, four pointer words

`ARC_MSG_QUEUE_HEADER_SIZE 32` (`msgqueue.h:16`). The four ring pointers live in this 32-byte header at these byte offsets relative to `queue_base` (`msgqueue.h:19-22`):

```c
#define ARC_MSG_QUEUE_REQ_WPTR(base) ((base) + 0x00)   // request  write ptr  — DRIVER writes
#define ARC_MSG_QUEUE_RES_RPTR(base) ((base) + 0x04)   // response read  ptr  — DRIVER writes
#define ARC_MSG_QUEUE_REQ_RPTR(base) ((base) + 0x10)   // request  read  ptr  — FW writes
#define ARC_MSG_QUEUE_RES_WPTR(base) ((base) + 0x14)   // response write ptr  — FW writes
```

Ownership split (who advances each pointer):
- **Driver owns** `REQ_WPTR` (0x00) and `RES_RPTR` (0x04) — the two "host side" indices, packed into the first 8 bytes.
- **Firmware owns** `REQ_RPTR` (0x10) and `RES_WPTR` (0x14) — the two "ARC side" indices, packed 16 bytes away.

Offsets 0x08, 0x0C, 0x18, 0x1C are inside the 32-byte header but are **never read or written by the driver** (reserved / FW-private). The 8-byte gap between the host pair (0x00/0x04) and the FW pair (starting 16 bytes from base at 0x10/0x14) reads as deliberate separation between the host and the ARC core.

### 1.3 Full queue image in CSM

Given `queue_base` and `num_entries` (call it N):

```
queue_base + 0x00 .. +0x1F         : 32-byte header (4 live pointers + reserved)
request_base  = queue_base + 32                              (msgqueue.c:15)
  request ring : N entries × 32 bytes
response_base = queue_base + 32 + N*32                       (msgqueue.c:75)
  response ring: N entries × 32 bytes
```
Total footprint = `32 + 2*N*32` bytes. Request and response rings are **separate** rings that share the same slot count N and the same 32-byte stride.

> **Porting note:** The entire header is memory in device CSM (chip SRAM), not a driver-side struct. On Windows the equivalent is MMIO/NOC-window register reads at these byte offsets. Do NOT model the header as a C struct with natural alignment — offset 0x08/0x0C are holes. Honor the exact offsets above.

---

## 2. Ring index arithmetic and wraparound

Pointers are kept in the range **[0, 2N)** — i.e. counters run to *twice* the slot count, not N. This is the "extra range" trick that lets full and empty be distinguished without a separate flag.

- Slot index into a ring: `slot = ptr % num_entries` (`msgqueue.c:55,115`).
- Advance: `ptr = (ptr + 1) % (2 * num_entries)` (`msgqueue.c:65,127`).
- Occupancy: `num_occupied = (wptr - rptr) % (2 * num_entries)` (`msgqueue.c:43,103`).

Push waits until there is room:
```c
num_occupied = (wptr - rptr) % (2 * num_entries);
if (num_occupied < num_entries)      // not full → space available
	break;
```
(`msgqueue.c:43-45`)

Pop waits until something is present:
```c
num_occupied = (wptr - rptr) % (2 * num_entries);
if (num_occupied > 0)                // at least one response queued
	break;
```
(`msgqueue.c:103-105`)

The subtraction `(wptr - rptr)` is unsigned `u32`; when `wptr < rptr` (post-wrap) it wraps to `2^32 - (rptr - wptr)` and is then reduced `% (2*num_entries)`. This yields the correct occupancy **only when `2*num_entries` divides `2^32`, i.e. when `num_entries` is a power of two** (so that `2^32 mod 2N == 0`). See Open Questions — the code does not validate this.

> **Porting note:** Replicate this in unsigned 32-bit arithmetic exactly (wrap then modulo). Do not "simplify" to `if (wptr >= rptr) ... else ...` unless you have confirmed `num_entries` is always a power of two; the two are only equivalent under that assumption.

---

## 3. `arc_msg_push` — enqueue a request (`msgqueue.c:12-70`)

Signature: `bool arc_msg_push(tt_dev, const struct arc_msg *msg, u32 queue_base, u32 num_entries)`.

Step by step:

1. `request_base = queue_base + ARC_MSG_QUEUE_HEADER_SIZE` (`:15`).
2. Read `REQ_WPTR` via `cls->csm_read32(...ARC_MSG_QUEUE_REQ_WPTR(queue_base)...)`; **return false on any csm error** (`:22-23`).
3. **Device-gone guard:** `if (wptr == U32_MAX)` → `dev_err("ARC queue WPTR read returned all-1s; device gone?")`, return false (`:25-28`).
4. Arm timeout: `timeout = jiffies + msecs_to_jiffies(ARC_MSG_TIMEOUT_MS)` = now + **1000 ms** (`:30`).
5. Space-wait loop (`:31-53`):
   - Read `REQ_RPTR`; return false on csm error (`:35-36`).
   - **Device-gone guard** on rptr all-1s (`:38-41`).
   - Compute occupancy; break when `< num_entries` (`:43-45`).
   - `if (time_after(jiffies, timeout))` → `dev_err("Timeout waiting for space in ARC message queue")`, return false (`:47-50`).
   - `usleep_range(100, 200)` — sleep **100–200 µs** between polls (`:52`).
6. `slot = wptr % num_entries`; `req_offset = slot * sizeof(struct arc_msg)` (`:55-56`).
7. Write all 8 words of the entry (`:57-63`):
   ```c
   for (i = 0; i < 8; ++i) {
       u32 addr = request_base + req_offset + (i * sizeof(u32));
       u32 value = (i == 0) ? msg->header : msg->payload[i - 1];
       if (cls->csm_write32(tt_dev, addr, value) != 0) return false;
   }
   ```
   → word 0 = `header`, words 1..7 = `payload[0..6]`. Any csm write failure aborts immediately (no rollback of already-written words).
8. Publish: `wptr = (wptr + 1) % (2 * num_entries)`; write it back to `REQ_WPTR` (`:65-67`). csm error → false.
9. Return true (`:69`).

Note: the entry words are written **before** the WPTR update, so the WPTR write is the publish point. No memory barrier separates them (see §6).

---

## 4. `arc_msg_pop` — dequeue a response (`msgqueue.c:72-132`)

Signature: `bool arc_msg_pop(tt_dev, struct arc_msg *msg, u32 queue_base, u32 num_entries)`.

1. `response_base = queue_base + ARC_MSG_QUEUE_HEADER_SIZE + (num_entries * sizeof(struct arc_msg))` (`:75`).
2. Read `RES_RPTR`; false on error; **all-1s device-gone guard** (`:82-88`).
3. Arm the same **1000 ms** timeout (`:90`).
4. Response-wait loop (`:91-113`):
   - Read `RES_WPTR`; false on error; **all-1s device-gone guard** (`:95-101`).
   - Occupancy `> 0` → break (`:103-105`).
   - Timeout → `dev_err("Timeout waiting for ARC response")`, false (`:107-110`).
   - `usleep_range(100, 200)` (`:112`).
5. `slot = rptr % num_entries`; `response_offset = slot * sizeof(struct arc_msg)` (`:115-116`).
6. Read header from `response_base + response_offset` into `msg->header` (`:117-118`), then 7 payload words (`:120-125`):
   ```c
   for (i = 0; i < 7; ++i) {
       u32 addr = response_base + response_offset + ((i + 1) * sizeof(u32));
       if (cls->csm_read32(tt_dev, addr, &msg->payload[i]) != 0) return false;
   }
   ```
   Offsets read: header at +0, payload at +4,+8,…,+28.
7. Advance consumer: `rptr = (rptr + 1) % (2 * num_entries)`; write to `RES_RPTR` (`:127-129`).
8. Return true (`:131`).

---

## 5. Request/response pairing and the transaction wrapper

`msgqueue.c` has **no pairing/matching logic** — it is a pure FIFO. Pairing is enforced by the arch `send_arc_message` doing push → interrupt → pop as one synchronous transaction, sending exactly one request and reading exactly one response. There is no request ID or tag in the protocol; correlation is purely positional/FIFO.

### 5.1 Blackhole `send_arc_message` (`blackhole.c:500-540`)

```c
timeout = jiffies + msecs_to_jiffies(ARC_MSG_READY_MS);   // 500 ms
do {
    boot_status = noc_read32(bh, ARC_X, ARC_Y, ARC_BOOT_STATUS, 0);
    if (boot_status == 0xFFFFFFFFu) return false;          // NOC is hung
    if (boot_status & ARC_BOOT_STATUS_READY_FOR_MSG) break;
} while (time_before(jiffies, timeout));
if (!(boot_status & ARC_BOOT_STATUS_READY_FOR_MSG)) return false;

queue_ctrl_addr = noc_read32(bh, ARC_X, ARC_Y, ARC_MSG_QCB_PTR, 0);
if (csm_read32(bh, queue_ctrl_addr + 0, &queue_base) != 0) return false;
if (csm_read32(bh, queue_ctrl_addr + 4, &queue_info) != 0) return false;
num_entries = queue_info & 0xFF;

if (!arc_msg_push(&bh->tt, msg, queue_base, num_entries)) return false;
noc_write32(bh, ARC_X, ARC_Y, ARC_MSI_FIFO, 0, 0);         // trigger ARC
if (!arc_msg_pop(&bh->tt, msg, queue_base, num_entries))  return false;
return msg->header == 0;                                   // header 0 == success
```

Key facts:
- Readiness gate: poll `ARC_BOOT_STATUS` (= `RESET_SCRATCH(2)` = 0x80030408) for up to **500 ms** until bit `ARC_BOOT_STATUS_READY_FOR_MSG` (0x1) is set (`blackhole.c:509-518`). `0xFFFFFFFF` here means the NOC is hung → abort (`:511-512`).
- Interrupt/doorbell: a single `noc_write32(...ARC_MSI_FIFO, 0...)` where `ARC_MSI_FIFO = 0x800B0000`; the comment says "Write 0 to trigger the ARC message queue processor" (`blackhole.c:65,534`).
- **Success = the popped response `header == 0`** (`:539`). A non-zero response header is treated as failure by the wrapper even though `arc_msg_pop` itself returned true.
- **No lock** is taken across the push/interrupt/pop transaction. Serialization is the caller's responsibility (see §8).

### 5.2 Wormhole `send_arc_message` (`wormhole.c:242-291`)

```c
timeout = jiffies + msecs_to_jiffies(ARC_MSG_READY_MS);   // 500 ms
do { if (arc_l2_is_running(regs)) break; } while (time_before(jiffies, timeout));
if (!arc_l2_is_running(regs)) return false;

qcb_ptr = ioread32(wh->bar4_mapping + ARC_MSG_QCB_PTR);
if (qcb_ptr == 0) { dev_warn_once("ARC message queue not available (normal for old FW)"); return false; }
if (!is_range_within_csm(qcb_ptr, sizeof(u32))) return false;

if (csm_read32(wh, qcb_ptr + 0, &queue_base) != 0) return false;
if (csm_read32(wh, qcb_ptr + 4, &queue_info) != 0) return false;
queue_base += ARC_CSM_BASE;                                // <-- WH-only fixup
num_entries = queue_info & 0xFF;

if (!arc_msg_push(&wh->tt, msg, queue_base, num_entries)) return false;
arc_misc_cntl = ioread32(regs + ARC_MISC_CNTL_REG);
iowrite32(arc_misc_cntl | ARC_MISC_CNTL_IRQ0_MASK, regs + ARC_MISC_CNTL_REG);  // trigger IRQ0
if (!arc_msg_pop(&wh->tt, msg, queue_base, num_entries))  return false;
return msg->header == 0;
```

Differences vs Blackhole:
- Readiness gate is `arc_l2_is_running` = POST-code check: `(ioread32(POST_CODE_REG) & 0xFFFF0000) == 0xC0DE0000` (`wormhole.c:199-203,101-103`).
- QCB pointer is read from a BAR4 MMIO register `ARC_MSG_QCB_PTR = RESET_UNIT_START + 0x01D8` (= BAR4 offset **0x1F301D8**), not a NOC read (`wormhole.c:119,263`). A value of **0 means "old FW, no queue"** → return false (`:264-266`).
- **`queue_base += ARC_CSM_BASE`** (`wormhole.c:277`). The WH QCB stores `queue_base` as a **CSM-relative offset**; the driver adds `ARC_CSM_BASE` (0x10000000) so the value matches what `csm_read32/write32` expect. **Blackhole does NOT do this** — its QCB stores an absolute CSM address. This asymmetry must be preserved.
- Doorbell is a read-modify-write of `ARC_MISC_CNTL_REG` (0x100) setting `ARC_MISC_CNTL_IRQ0_MASK` = `1<<16` (`wormhole.c:105-106,283-285`).
- The function is tagged `__maybe_unused` (`:242`) but is live via `wormhole_set_power_state` (`:1047`).

### 5.3 QCB (Queue Control Block) discovery, both arches

The QCB is a small 2-word structure in CSM. Word 0 = `queue_base`, word 1 = `queue_info`; `num_entries = queue_info & 0xFF` (upper 24 bits ignored by the driver). Pointer to the QCB:
- BH: NOC read of `ARC_MSG_QCB_PTR = RESET_SCRATCH(11) = 0x8003042C` (`blackhole.c:59,64,520`).
- WH: MMIO read of `ARC_MSG_QCB_PTR = 0x1F301D8` (`wormhole.c:119,263`).

---

## 6. Memory barriers / ordering

`msgqueue.c` contains **no explicit memory barriers** — no `mb()`, `wmb()`, `rmb()`, `smp_*`, `dma_*`, or `READ_ONCE`/`WRITE_ONCE`. All ordering is inherited from the CSM accessors:

- **Wormhole** CSM access is `ioread32`/`iowrite32` on BAR4 (`wormhole.c:150,159`). On x86 these are strongly ordered, so entry-writes → WPTR-write → IRQ-trigger stay in program order.
- **Blackhole** CSM access is a NOC read/write through the kernel TLB window, each guarded by `mutex_lock(&bh->kernel_tlb_mutex)` around the `iowrite32`/`ioread32` (`blackhole.c:243-268`). The mutex serializes each individual 32-bit access but is dropped between accesses, so it is not a transaction lock.

The correctness of "publish the entry, then bump WPTR, then ring the doorbell" therefore rests entirely on MMIO accessor ordering, not on explicit barriers.

> **Porting note:** On Windows, `READ_REGISTER_*`/`WRITE_REGISTER_*` (or `MmMapIoSpace` + volatile access) do not guarantee the same ordering on all architectures. To match Linux x86 behavior you must ensure: (a) all 8 entry words are committed to the device before the WPTR store, and (b) the WPTR store is committed before the doorbell write. Insert explicit `KeMemoryBarrier()`/`_mm_sfence`/`MemoryBarrier()` between those phases if the target CPU or bus is weakly ordered. On the read side, ensure the RES_WPTR load is ordered before the response-entry loads.

---

## 7. NOC-hang early-exit detection (exact conditions)

The message-wait loops abort *early* — before the 1000 ms timeout expires — on an all-ones pointer read. These are the exact conditions:

| Location | Condition | Action |
|---|---|---|
| `msgqueue.c:25` | `wptr == U32_MAX` after reading `REQ_WPTR` | `dev_err("...WPTR...device gone?")`, return false |
| `msgqueue.c:38` | `rptr == U32_MAX` inside push wait loop | `dev_err("...RPTR...device gone?")`, return false |
| `msgqueue.c:85` | `rptr == U32_MAX` after reading `RES_RPTR` | `dev_err("...RPTR...device gone?")`, return false |
| `msgqueue.c:98` | `wptr == U32_MAX` inside pop wait loop | `dev_err("...WPTR...device gone?")`, return false |

`U32_MAX` = `0xFFFFFFFF`. These checks sit **before** the `time_after(jiffies, timeout)` check inside each loop, so a device that has fallen off the bus (reads return all-ones) aborts on the very next poll instead of spinning for a full second. Only the **pointer** reads are all-ones-checked; the entry header/payload reads are not.

Two additional (non-queue) hang gates exist in the wrappers:
- BH readiness poll: `boot_status == 0xFFFFFFFFu` → return false ("NOC is hung"), `blackhole.c:511-512`.
- WH scratch-register protocol `arc_msg_poll_completion`: `read_val == 0xFFFFFFFFu && is_hardware_hung(...)` → return `-3` (`wormhole.c:180-183`). `is_hardware_hung` checks the PCI vendor ID and `SCRATCH_REG(6) == 0xFFFFFFFF` (`wormhole.c:129-138`). This is the *legacy* SR5 protocol, not the CSM queue.

---

## 8. Error propagation, locking, cleanup

### 8.1 Return-value contract
`arc_msg_push`/`arc_msg_pop` return `bool` only. Every distinct failure — csm range error (`-EINVAL` from `is_range_within_csm`), device-gone (all-ones), and timeout — collapses to `false`. The specific errno from `csm_read32`/`csm_write32` is **discarded** (`msgqueue.c:22-23,35-36,61-62,...`). The `dev_err` log line is the only way to tell timeout from device-gone.

`csm_read32`/`csm_write32` themselves return `-EINVAL` when the address is outside `[ARC_CSM_BASE, ARC_CSM_BASE+ARC_CSM_SIZE - len]` i.e. `[0x10000000, 0x10080000)` (`telemetry.h:73-78`, `blackhole.c:272-273,281-282`, `wormhole.c:147-148,156-157`).

### 8.2 Caller-side propagation
- `blackhole_set_power_state` / `wormhole_set_power_state`: `send_arc_message` false → **return `-EINVAL`** (`blackhole.c:806-808`, `wormhole.c:1049-1050`).
- `blackhole_init_hardware`: ASTATE0 failure → `dev_err` but continues; WDT failure → `dev_warn` "normal for old FW", still returns true (`blackhole.c:628-638`).
- `blackhole_cleanup_hardware`: guarded by `if (tt_dev->detached) return;`, then ASTATE3 best-effort with `dev_err` on failure (`blackhole.c:700-708`).
- `blackhole_reset` (DMC path): `ARC_MSG_TYPE_TEST` failure → `dev_warn("...NOC is likely hung")` + return false; the subsequent `ARC_MSG_TYPE_TRIGGER_RESET` send's result is **ignored** and the function returns true ("// Possibly a lie...") (`blackhole.c:547-563`).

### 8.3 Locking around transactions
`send_arc_message` takes no lock spanning the push/doorbell/pop. Serialization comes from higher layers:
- All aggregated power-setting sends funnel through `tenstorrent_set_aggregated_power_state[_locked]` under `tt_dev->chardev_mutex` (`chardev.c:540-542`; `lockdep_assert_held` at `:484`). The ioctl, open, and release callers additionally hold `reset_rwsem` **shared** for the duration (`chardev.c:601`, `:838`, `:929`), so they are mutually excluded from the reset ioctl.
- Reset / `init_hardware` paths run under `reset_rwsem` held **exclusive** in the reset ioctl (`chardev.c:598-599`; comment at `:236-238`; the reset block at `:240-290`). `cleanup_hardware` is **not** called from the reset ioctl: it runs from the suspend path (`enumerate.c:506`) and the PCI-remove path (`enumerate.c:434`).
- The deferred `power_down_work` handler (`chardev.c:553-560`) sends power-setting messages under `chardev_mutex` but **without** `reset_rwsem`; the reset ioctl drains it with `cancel_delayed_work_sync` before disturbing the device (`chardev.c:238`), and re-arming happens only while `reset_rwsem` is held shared (`chardev.c:917` inside the release path), so it cannot overlap the reset ioctl either.

The suspend-path `cleanup_hardware` send (`enumerate.c:506`) is the ARC transaction with no lock against a concurrent power-setting send (see Open Questions).

### 8.4 Cleanup at fd close / removal
The queue itself holds no per-fd state — it is stateless device memory. At fd close the aggregated power state may be recomputed and a fresh POWER_SETTING message sent (`chardev.c` release path); `blackhole_cleanup_hardware` sends ASTATE3 from the suspend path (`enumerate.c:506`). On the PCI-remove path it is also invoked (`enumerate.c:434`), but `detached` is set earlier in remove (`enumerate.c:424`), so its `if (tt_dev->detached) return;` guard skips the ASTATE3 send there. There is no queue teardown to perform.

---

## 9. Every message ID / opcode the driver sends

The message opcode occupies the **low byte** of `arc_msg.header`. For POWER_SETTING the header additionally packs `validity` into bits 8-15 and `power_flags` into bits 16-31.

### 9.1 Blackhole — CSM queue messages (`blackhole.c:67-72`)

| Constant | Value | Meaning / where sent | Payload |
|---|---|---|---|
| `ARC_MSG_TYPE_ASIC_STATE0` | `0xA0` | Enter A0 (active) power state; `blackhole_init_hardware` (`:628-630`) | none |
| `ARC_MSG_TYPE_ASIC_STATE3` | `0xA3` | Enter A3 (low) power state; `blackhole_cleanup_hardware` (`:705-707`) | none |
| `ARC_MSG_TYPE_SET_WDT_TIMEOUT` | `0xC1` | Set ARC watchdog timeout; `blackhole_init_hardware` (`:633-636`) | `payload[0] = 1000 * auto_reset_timeout` (ms; module param default 10 s → 10000) |
| `ARC_MSG_TYPE_TRIGGER_RESET` | `0x56` | Trigger ASIC+M3 reset; `blackhole_reset` DMC path (`:560-562`) | `payload[0] = 3` (ASIC + M3 reset arg) |
| `ARC_MSG_TYPE_POWER_SETTING` | `0x21` | Apply aggregated power state; `blackhole_set_power_state` (`:802-804`) | header bits 8-15 = validity, 16-31 = power_flags; `payload[0..6]` = `power_settings[0..13]` |
| `ARC_MSG_TYPE_TEST` | `0x90` | Ping/liveness probe before reset; `blackhole_reset` (`:551-552`) | none |

### 9.2 Wormhole — CSM queue message (`wormhole.c:121`)

| Constant | Value | Meaning / where sent | Payload |
|---|---|---|---|
| `ARC_MSG_TYPE_POWER_SETTING` | `0xC0` | Apply aggregated power state; `wormhole_set_power_state` (`:1040-1042`) | same packing as BH |

**Note the value differs from Blackhole's POWER_SETTING (0x21 vs 0xC0).** The opcode is arch-specific.

### 9.3 Wormhole — legacy SR5 scratch-register protocol (NOT the CSM queue) (`wormhole.c:112-116`)

These travel through Scratch Register 5, not `msgqueue.c`. Included for completeness because they are an alternate ARC-messaging path on WH.

| Constant | Value | Meaning |
|---|---|---|
| `WH_FW_MESSAGE_PRESENT` | `0xAA00` | OR'd with `message_id`, written to SR5 (`SCRATCH_REG(5)` = reset-unit offset 0x74, i.e. BAR4 off 0x1F30074) to post a message (`:112,220`) |
| `WH_FW_MSG_ASTATE0` | `0xA0` | A0 state; sent by `wormhole_init_hardware` with 10 000 µs timeout (`:725`) |
| `WH_FW_MSG_ASTATE3` | `0xA3` | A3 state; sent by `wormhole_shutdown_firmware` with 10 000 µs timeout (`:298`) |
| `WH_FW_MSG_CURR_DATE` | `0xB7` | Current date/time to FW; sent by `wormhole_send_curr_date` (`:358`) |

This table is not the complete SR5 traffic: the driver also sends `WH_FW_MSG_NOP` `0x11` (`:43`; liveness probes in `wormhole_reset`, `:481,497`), `WH_FW_MSG_TRIGGER_RESET` `0x56` (`:42`; `wormhole_reset`, `:510`), `WH_FW_MSG_PCIE_INDEX` `0x51` (`:39`; `update_device_index`, `:467`), `WH_FW_MSG_UPDATE_M3_AUTO_RESET_TIMEOUT` `0xBC` (`:41`; `wormhole_init_hardware`, `:729`), and `FW_MSG_PCIE_RETRAIN` `0xB6` (`pcie.c:16,112`) — all over SR5, none over the CSM queue.

Protocol (`wormhole.c:108-112, 205-234`): write args to `SCRATCH_REG(3)` (off 0x6C), write `0xAA00 | id` to `SCRATCH_REG(5)`, pulse IRQ0 via `ARC_MISC_CNTL_REG`, then `arc_msg_poll_completion` waits for the low 16 bits of SR5 to equal `message_id`; the high 16 bits become `*exit_code`.

### 9.4 POWER_SETTING header/payload packing (both arches)
```c
msg.header = ARC_MSG_TYPE_POWER_SETTING | (validity << 8) | (power_flags << 16);
BUILD_BUG_ON(sizeof(power_state->power_settings) != sizeof(msg.payload));   // 14*u16 == 7*u32 == 28
memcpy(msg.payload, power_state->power_settings, sizeof(msg.payload));
```
(`blackhole.c:802-804`, `wormhole.c:1040-1042`.) `power_settings` is `__u16[14]` (`ioctl.h:410`); `validity` is a `__u8` split into "valid flags count" (bits 0-3) and "valid settings count" (bits 4-7) per `TT_POWER_VALIDITY` (`ioctl.h:400-404`).

---

## 10. What telemetry.c does NOT do

`telemetry.c` reads sensor tags through `tt_dev->dev_class->read_telemetry_tag` (`telemetry.c:28`), which for BH is `blackhole_read_telemetry_tag` → a direct `csm_read32` against a per-tag cached CSM address (`blackhole.c:440-453`). This **bypasses the ARC message queue entirely** — telemetry is a plain CSM memory read, gated only by `reset_rwsem` (read), `detached`, and `needs_hw_init` checks (`telemetry.c:16-28`). The tag→address cache is populated by `telemetry_probe` (`blackhole.c:455-498`), which is likewise pure NOC/CSM reads, not queue traffic.

---

## Key constants table

| Name | Value | Source cite |
|---|---|---|
| `ARC_MSG_QUEUE_HEADER_SIZE` | 32 bytes | `msgqueue.h:16` |
| `ARC_MSG_TIMEOUT_MS` | 1000 ms | `msgqueue.h:17` |
| `ARC_MSG_QUEUE_REQ_WPTR` offset | base + 0x00 (driver writes) | `msgqueue.h:19` |
| `ARC_MSG_QUEUE_RES_RPTR` offset | base + 0x04 (driver writes) | `msgqueue.h:20` |
| `ARC_MSG_QUEUE_REQ_RPTR` offset | base + 0x10 (FW writes) | `msgqueue.h:21` |
| `ARC_MSG_QUEUE_RES_WPTR` offset | base + 0x14 (FW writes) | `msgqueue.h:22` |
| `sizeof(struct arc_msg)` | 32 bytes (1 header + 7 payload u32) | `msgqueue.h:11-14` |
| poll sleep between waits | `usleep_range(100, 200)` (100–200 µs) | `msgqueue.c:52,112` |
| device-gone sentinel | `U32_MAX` = 0xFFFFFFFF | `msgqueue.c:25,38,85,98` |
| pointer counter range | `[0, 2*num_entries)` | `msgqueue.c:65,127` |
| `num_entries` extraction | `queue_info & 0xFF` | `blackhole.c:528`, `wormhole.c:278` |
| `ARC_CSM_BASE` | 0x10000000 | `telemetry.h:73` |
| `ARC_CSM_SIZE` | 0x80000 (1<<19) | `telemetry.h:74` |
| CSM valid range | `[0x10000000, 0x10080000)` | `telemetry.h:75-78` |
| `ARC_MSG_READY_MS` (both arches) | 500 ms | `blackhole.c:66`, `wormhole.c:120` |
| BH `ARC_MSG_QCB_PTR` | RESET_SCRATCH(11) = 0x8003042C (NOC) | `blackhole.c:59,64` |
| BH `ARC_MSI_FIFO` doorbell | 0x800B0000, write 0 | `blackhole.c:65,534` |
| BH `ARC_BOOT_STATUS` | RESET_SCRATCH(2) = 0x80030408 | `blackhole.c:59,73` |
| BH `ARC_BOOT_STATUS_READY_FOR_MSG` | 0x1 | `blackhole.c:74` |
| BH `ARC_X, ARC_Y` | 8, 0 | `blackhole.c:57-58` |
| WH `ARC_MSG_QCB_PTR` | RESET_UNIT_START+0x1D8 = 0x1F301D8 (BAR4) | `wormhole.c:119` |
| WH queue_base fixup | `+= ARC_CSM_BASE` (BH does not) | `wormhole.c:277` |
| WH `ARC_MISC_CNTL_REG` / IRQ0 mask | 0x100 / (1<<16) | `wormhole.c:105-106` |
| WH `arc_l2_is_running` POST code | `(POST_CODE & 0xFFFF0000)==0xC0DE0000` | `wormhole.c:101-103,199-203` |
| WH SR5 present marker | `WH_FW_MESSAGE_PRESENT` = 0xAA00 | `wormhole.c:112` |
| BH msg opcodes | A0=0xA0, A3=0xA3, WDT=0xC1, RESET=0x56, POWER=0x21, TEST=0x90 | `blackhole.c:67-72` |
| WH queue POWER opcode | 0xC0 | `wormhole.c:121` |
| success criterion | popped `msg->header == 0` | `blackhole.c:539`, `wormhole.c:290` |
| `auto_reset_timeout` (WDT payload) | default 10 (seconds) → ×1000 ms | `module.c:48`, `blackhole.c:634` |

---

## Open questions

1. **Power-of-two `num_entries` requirement.** The occupancy math `(wptr - rptr) % (2*num_entries)` on unsigned `u32` is only correct across wrap when `2*num_entries` divides `2^32`, i.e. `num_entries` is a power of two. The driver reads `num_entries = queue_info & 0xFF` from the QCB and never validates it. Is the FW contract that `num_entries` is always a power of two? If not, occupancy is miscomputed after a wptr wrap and the queue can wedge or corrupt. A Windows port must either preserve the identical wrap-then-modulo behavior or add validation.

2. **Transaction serialization across locks.** `send_arc_message` (push → doorbell → pop) holds no lock spanning the transaction. Power-setting sends are serialized among themselves by `chardev_mutex` and against the reset ioctl by `reset_rwsem` (shared vs exclusive — see §8.3), so the ioctl/open/release/deferred-work power paths cannot interleave with reset/init. The remaining exposure is the suspend-path `cleanup_hardware` ASTATE3 send (`enumerate.c:506`), which holds neither `chardev_mutex` nor `reset_rwsem`; it presumably relies on the PM core freezing userspace before suspend callbacks run. Confirm that invariant, or give the port an explicit per-device "ARC message" mutex.

3. **Meaning of the upper 24 bits of `queue_info`.** Only bits 0-7 (`num_entries`) are consumed. Whether the remaining bits encode entry size, version, or flags is unknown; the driver hardcodes a 32-byte entry stride. If a future FW changes entry size via those bits, the port would silently mis-stride.

4. **Response header semantics beyond zero.** Both wrappers treat `header == 0` as success and any non-zero value as failure, but the meaning of a non-zero response header (error code? echoed opcode? status bitfield?) is not documented in these files. Callers cannot distinguish error causes.

5. **Barrier requirements on non-x86 Windows targets.** `msgqueue.c` relies entirely on `ioread32`/`iowrite32` (WH) and mutex-guarded NOC accesses (BH) for ordering, with no explicit barriers. On a weakly-ordered target the entry-write → WPTR-publish → doorbell ordering (and the RES_WPTR → entry-read ordering) is not guaranteed. The exact barrier placement a Windows KMDF port needs is unverified here.

6. **`ARC_MSI_FIFO` (BH) vs `ARC_MISC_CNTL` IRQ0 (WH) doorbell semantics.** The BH doorbell writes 0 to a FIFO at 0x800B0000; the WH doorbell RMW-sets bit 16 of a control register. Whether the doorbell must strictly follow the WPTR store (and whether it auto-clears) is inferred from code comments, not confirmed against hardware docs.
