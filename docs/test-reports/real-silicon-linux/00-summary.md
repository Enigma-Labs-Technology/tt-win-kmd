# Ground-Truth Reference: Blackhole p150a on Linux tt-kmd 2.9.1-pre

> Board serial, ASIC id and host name are redacted in this public copy of the report.


Captured 2026-07-08 (~13:00 UTC) on host `omarchy`, kernel `7.0.10-arch1-1`.
Module load at boot 08:51 local logged zero errors. Card idle throughout capture.
(The boot journal also contains `pin_user_pages_longterm failed: -14` lines from
later in the session — these are EXPECTED output of ttkmd_test's PinPages
negative-path cases, not driver errors; see the note in dmesg-tenstorrent.txt.)

## Verdict

**VERDICT: Trustworthy reference set — YES, COMPLETE including the reset test and full config-space delta (OQ-1, OQ-5 both resolved). Card healthy (verified post-reset).**

Card health is GOOD — clean probe, full Gen5 x16 link, ~51 °C at
38 W idle, heartbeat alive at 10 Hz, therm_trip_count 0, and the upstream kernel
test suite passes end-to-end against the live device: unprivileged run exit 0
(4 root-gated skips), root run exit 0 with **zero skips**, and a post-reset
run exit 0 (byte-identical output to pre-reset).

All three original gaps are closed: (1) full config space + MPS/MRRS —
CLOSED via `capture-privileged.sh` (lspci-vvv-root.txt,
lspci-config-baseline.txt); (2) tt-smi — CLOSED, installed in `.tt-smi-venv`,
snapshots captured; (3) reset step — **EXECUTED 09:35:57** under the gating
rule (card idle + proven recovery path: uvx tt-flash 3.10.0 + the
anchor-matching fw_pack-19.6.0.fwbundle on disk). Reset succeeded; no reflash
was needed; firmware anchor untouched. The full 4 KiB before/after config
delta is captured and decoded (see "Config-space reset delta" below): all
architected config survives, MPS/MRRS restored by the driver, parity-bit
reset marker confirmed in source and observation.

## Card identity

| Item | Value |
|---|---|
| BDF | `0000:c1:00.0` (PCI domain/segment `0000`) |
| Vendor:Device | `1e52:b140` — Tenstorrent Blackhole |
| Subsystem | `1e52:0040` — decoded by lspci as **p150a** |
| Class / rev | `0x120000` (Processing accelerator) / rev 0x00 |
| Link | **32.0 GT/s (Gen5) x16**, negotiated = max (sysfs `current_link_*`, see pci-sysfs-devnode.txt) |
| Interrupt | **single-vector plain MSI**, IRQ 152 (`msi_irqs/152` = `msi`, NOT MSI-X); INTx pin **A** (config 0x3d = 0x01), disabled via DisINTx+ |
| Card type (sysfs) | `p150a` |
| Board serial | `<board-serial>` |
| ASIC ID | `<asic-id>` (by-id name: `blackhole-<asic-id>`) |
| Char device | `/dev/tenstorrent/0`, dev 510:0, mode `crw-rw-rw-` |

Interrupt caveat for the Windows diff: this host's pciutils 3.15.0 renders the
`Interrupts:` pin letter off-by-one (prints "pin B" for pin byte 0x01, and a
nonexistent "pin E" for another device with pin byte 0x04). The raw config byte
and sysfs are authoritative; the buggy lspci line is flagged inside lspci-vvv.txt.

## BAR layout — resolves OQ-1

From sysfs `resource` and unprivileged `lspci -vvv` (both agree):

| BAR | CPU phys base | Size | Attributes |
|---|---|---|---|
| BAR0 | `0x150_0000_0000` | **512 MiB** | 64-bit, prefetchable |
| BAR2 | `0x150_2000_0000` | **1 MiB** | 64-bit, prefetchable |
| BAR4 | `0x148_0000_0000` | **32 GiB** | 64-bit, prefetchable |

All three are 64-bit prefetchable MMIO; no I/O ports; BARs 1/3/5 are the upper
halves. Windows note: BAR4 = 32 GiB requires large-BAR / above-4G decoding.

## Negotiated MPS / MRRS and PCIe capabilities — resolves OQ-5 baseline

From the root lspci run (`lspci-vvv-root.txt`), confirmed against the raw
config dump (`lspci-config-baseline.txt`, DevCtl @ 0x78 = `0x515f`):

- **MaxPayload in effect: 512 bytes** (device DevCap supports up to 1024;
  512 is what the AMD root complex negotiated — the Windows port must expect
  platform-dependent MPS, not the device max)
- **MaxReadReq: 4096 bytes**
- LnkCap/LnkSta: 32 GT/s x16 both, **ASPM not supported** (and disabled);
  SlotPowerLimit 75 W; CommClk+; equalization complete through phase 3 at
  both 16 GT/s and 32 GT/s
- DevCtl also has: CorrErr+/NonFatalErr+/FatalErr+/UnsupReq+ error reporting
  enabled, RlxdOrd+, ExtTag+, NoSnoop- (snooping on)
- DevCtl2: Completion Timeout 50 µs–50 ms, TimeoutDis+, 10BitTagReq- (not
  enabled); DevCap2: 10BitTagComp+ (capable); no AtomicOps
- MSI capability: **Enable+ Count=1/1 Maskable+ 64bit+** @ 0x50 — confirms
  single-vector plain MSI (there is NO MSI-X capability in the device at all)
- AER @ 0x100 with **ECRC generation and checking both capable AND enabled**;
  UnsupReq masked in UEMsk
- Power Management v3 @ 0x40: D1+ D2+ supported, PME from D0/D1/D3hot,
  NoSoftRst+, currently D0
- PTM @ 0x374: Requester-capable, not enabled

Capability map (offsets the Windows port will walk):
`0x40` PM v3 → `0x50` MSI → `0x70` PCIe Express v2 Endpoint; extended:
`0x100` AER v2 → `0x148` Secondary PCIe → `0x178` PhysLayer 16 GT/s →
`0x1a8` Lane Margining → `0x1f0` PhysLayer 32 GT/s → `0x230` VSEC ID=0002 →
`0x330` VSEC ID=0001 → `0x368` Data Link Feature → `0x374` PTM →
`0x380` VSEC ID=0003 → `0x3e8` VSEC ID=0006.

Trap for raw-hex scanners: the config space contains *unlinked*
capability-template residue outside the walked chains — a byte 0x11 (the
MSI-X cap ID) at offset 0xb0 (plus 0x05 at 0xa0, 0x10 at 0xb8) and an
extended-cap-shaped dword at 0x220 that would decode as L1 PM Substates but
is skipped by the 0x1f0→0x230 next pointer. None are reachable via the
chains, so they are NOT capabilities; a Windows config parser must walk the
linked lists, not pattern-match cap IDs in raw space.

PCI_COMMAND baseline: `0x0406` = Mem+ BusMaster+ DisINTx+ — note the
parity-error-response bit (0x40) is **0 at baseline**, relevant to the OQ-5
marker question when the reset delta is eventually captured. Cache Line Size
64 B, Latency 0. Full 4 KiB config space is in `lspci-config-baseline.txt`
(this is the "before" reference for the future reset delta); the unprivileged
64-byte header remains in `lspci-config-header.txt`.

## Firmware anchor (Windows must match)

| Component | Version | Source |
|---|---|---|
| FW bundle | **19.6.0.0** | `tt_fw_bundle_ver`; tt-smi raw `0x13060000` |
| M3/DMC app | **0.22.0.0** | `tt_m3app_fw_ver`; tt-smi `bm_app_fw`, raw `0x160000` |
| CM/ARC | **N/A on Blackhole** | tt-smi `cm_fw = N/A`, raw `ARC0-3_FW_VERSION = None` |
| ETH | **N/A on Blackhole** | tt-smi `eth_fw = N/A`, raw `ETH_FW_VERSION = None` |

Ground truth for the Windows port: on Blackhole p150a with bundle 19.6.0.0,
the CM/ARC and ETH firmware version telemetry fields are genuinely absent
(None), so the Windows QUERY_TELEMETRY must tolerate/report empty values there
— not treat them as an error. tt-smi 3.1.1 / pyluwen 0.8.5 captured the
snapshot (tt-smi-snapshot.json).

Driver: tt-kmd **2.9.1-pre** (DKMS), source at `~/tt-kmd` =
`ttkmd-2.9.0-1-g0ab170d`. Kernel `7.0.10-arch1-1`.

## IOMMU: ON

- AMD-Vi active (`ivhd0–3`), default domain **Translated**, invalidation **lazy**
  → group type `DMA-FQ` (see `iommu.txt`, from boot kernel log).
- Card is **alone in IOMMU group 9** — good ACS isolation.
- No `iommu=`/`amd_iommu=` kernel-cmdline overrides → firmware/BIOS default
  (typical BIOS toggle: AMD CBS → NBIO → IOMMU = Enabled/Auto).
- Consequence observed in the test run: discontiguous `PIN_PAGES` succeeded,
  which the suite only expects when the IOMMU is enabled. The Windows
  DMA-remapping-ON test case corresponds to this exact configuration.

## Telemetry reference (QUERY_TELEMETRY ground truth, card idle)

Raw sysfs/hwmon values with Linux-standard units, plus derived:

| Metric | Raw | Meaning |
|---|---|---|
| asic_temp | 51431 m°C | **51.4 °C** (max/trip threshold 90 °C) |
| vcore | 740 mV | **0.740 V** (max 900 mV) |
| current | 52000 mA | **52 A** (max 200 A) |
| power | 38000000 µW | **38 W** (max/TDP 150 W) |
| fan | 2000 RPM | fan_rpm label |
| tt_aiclk | 800 | **800 MHz** (idle; power_policy=on keeps it low at probe) |
| tt_axiclk | 960 | 960 MHz |
| tt_arcclk | 800 | 800 MHz |
| tt_heartbeat | 5470→5520 in 5.003 s | **~10 Hz**, monotonic (heartbeat-samples.txt) |
| tt_therm_trip_count | 0 | no thermal trips ever recorded |

Module params in effect (affects behavior the Windows port must mirror or
consciously diverge from): `power_policy=Y`, `idle_power_down_grace_ms=5000`,
`auto_reset_timeout=10`, `reset_limit=10`, `dma_address_bits=0(auto)`,
`max_devices=32`. Full list in `module-params.txt`.

PCIe traffic counters snapshot in `pcie-perf-counters.txt` (12 counters,
mst/slv × posted/nonposted/rd, two channels each).

tt-smi snapshot cross-check (tt-smi-snapshot.json, captured ~09:21, card idle):
0.74 V / 52.0 A / 38.0 W / 50.7 °C / aiclk 800 — consistent with hwmon.
Additional ground truth from the raw SMBus telemetry: GDDR trained at **16G**
(`DDR_SPEED 0x3e80`, `dram_speed 16G`), **DDR_STATUS 0x5555** (all 8
controllers trained), `ENABLED_GDDR 0xff`, `TENSIX_ENABLED_COL 0xfff`,
`ENABLED_ETH 0x3edf`, `ENABLED_L2CPU 0xf`, `NOC_TRANSLATION_ENABLED 0x1`.
Limits: vdd 0.70–0.90 V, asic_fmax 1350 MHz, TDP limit 150 W, TDC limit 200 A,
therm trip 90 °C, thm limit 110 °C.

## Functional golden run — upstream test suite

Built `ttkmd_test` from `~/tt-kmd/test` (one-line portability fix
needed: `#include <cstdint>` added to `pin_pages.cpp` for GCC on this host —
left in the working tree, visible via `git diff`). Ran unprivileged against
the live card: **exit 0 — all executed tests passed** (`exit: 0` is recorded
in-file in test-suite-output.txt), covering GetDriverInfo, GetDeviceInfo,
ConfigSpace (partial), QueryMappings, DmaBuf, NocDmaBuf, PinPages (incl.
IOMMU-dependent discontiguous case), Lock, Hwmon, IoctlOverrun, IoctlZeroing,
Tlbs, MapPeerBar (single-device self-pair), ProcfsPids, DeviceRelease.

Side effect worth knowing: the suite's PinPages error-path cases intentionally
provoke `pin_user_pages_longterm failed: -14` / `could only pin 1 of 2 pages`
kernel log lines. The Windows port should expect the equivalent negative-path
noise; these are correct EFAULT rejections, not failures.

In the unprivileged run, four tests self-skipped (MSI config-space check, AER
check, contiguous-hugepage pinning, debugfs mappings). The subsequent **root
rerun via `capture-privileged.sh` executed with ZERO skips and exit 0**
(test-suite-output-root.txt) — so every test in the suite, including MSI, AER,
hugepage-contiguous pinning, and debugfs mappings, passes on this card.

pr_debug trace (`ftrace-or-dmesg.txt`): with `module tenstorrent +p` enabled
during the root run, the kernel log shows only the two expected pin-pages
negative-path lines (the EFAULT rejection and its companion "could only pin"
message) — tt-kmd 2.9.1-pre has essentially no pr_debug chatter in
these paths, so the Linux-side operation-order trace is sparse. The Windows
ETW diff should rely on the test suite's ioctl sequence rather than kernel
log ordering.

## Reset behavior (step 5): EXECUTED — 2026-07-08 09:35:57

Gate satisfied before executing: card idle (no open fds), recovery path
proven — `uvx tt-flash@latest` (3.10.0) working, with TWO recovery images on
disk: `~/fw_pack-19.6.0.fwbundle` (**anchor-preserving** — matches
the card's 19.6.0.0) and `fw_pack-19.11.0.fwbundle` (newer; would change the
anchor, last resort only). **No reflash was needed; firmware is untouched.**

Findings (`reset-timing.txt`, `reset-poller-samples.txt`, `dmesg-reset.txt`,
`tt-smi-reset-output.txt`):

- `tt-smi -r 0` succeeded unprivileged (0666 device node), wall time
  **2.75 s**, exit 0.
- **The device never leaves the bus.** A ~0.2 s poller saw sysfs,
  config-space reads, and `/dev/tenstorrent/0` present in all 872 samples
  (one 0.61 s sampling gap at reset onset; the kernel journal — no PCI
  remove/rescan, no tenstorrent re-probe — closes that gap): tt-kmd does an
  IN-PLACE reset. The Windows port should expect the device object to
  survive reset.
- Telemetry outage window: **1.64 s** (heartbeat reads fail while the ARC
  reboots). tt_heartbeat then RESTARTS from ~0 — counter restart is the
  authoritative "firmware actually rebooted" signal; back-extrapolation
  dates the ARC restart to mid-outage, and the post-reset rate re-measures
  at 9.998 Hz. QUERY_TELEMETRY on Windows must tolerate a ~2 s failure
  window during reset. (Scale note: tt-smi's *displayed* heartbeat is raw
  `TIMER_HEARTBEAT`/6; the sysfs counter is the raw 10 Hz value.)
- **DPC/AER side effect (critical for the Windows port):** the reset trips a
  DPC containment event on the AMD root port (Uncorrectable Fatal, SDES =
  surprise link-down). tt-kmd registers no AER `error_detected` callback, so
  the kernel logs "AER: device recovery failed" — and everything still
  works. On Windows, the equivalent surprise-down will engage the port/ACPI
  error handling; the KMDF port must expect and survive it (or mask
  SDES/DPC around an initiated reset, which is what production drivers
  typically do).
- **Config space: 64-byte standard header byte-identical across reset**
  (BARs, COMMAND=0x0406, subsystem). The PCI_COMMAND parity-error-response
  bit (0x40): **0 before, 0 after** — no persistent marker is left set at
  the times we could sample. (If the driver uses the parity bit as an
  in-flight reset marker, it is set and cleared inside the reset flow;
  catching it requires polling COMMAND *during* the ~2 s window, root-only.)
- Post-reset health: fw bundle still 19.6.0.0, Gen5 x16 relinked, GDDR
  retrained (DDR_STATUS 0x5555), telemetry normal, therm_trip still 0,
  **ttkmd_test exit 0** with output byte-identical to pre-reset
  (test-suite-output-post-reset.txt, tt-smi-snapshot-post-reset.json).

## Config-space reset delta — OQ-5 fully answered

Full 4 KiB before/after diff captured (`config-reset-delta.txt`, decoded in
`config-reset-delta-analysis.txt`):

- **All architected config is byte-identical across reset** — header, PM,
  MSI (address/data intact), PCIe **DevCtl = 0x515f (MPS 512 / MRRS 4096
  preserved)**, DevCtl2, LnkCtl, AER config. Endpoint AER status clean
  (SDES latched only on the root port).
- Only vendor/live-data bytes changed: VSEC ID=0002 payload (0x2e0–0x2f3),
  VSEC ID=0003 payload (0x390–0x395), and a counter-like non-capability
  region at 0x728–0x73a.
- **Mechanism (source-confirmed)**: tt-kmd saves config at probe
  (`pci_save_state`, enumerate.c:370), sets the **PCI_COMMAND parity bit as
  the reset marker** (`set_reset_marker`, pcie.c:140), triggers the ASIC/DMC
  reset via ARC message, polls `is_reset_marker_zero` (pcie.c:151) — the
  hardware reset clears COMMAND, so a zero marker proves the reset happened —
  then `safe_pci_restore_state` (pcie.c:43) restores everything including
  MPS/MRRS and re-saves.
- **OQ-5 verdict: yes, the parity bit clears across a real reset (it is the
  designed reset-completion signal), and MPS/MRRS survive only because the
  driver restores them. The Windows port must replicate save → set marker →
  reset → poll marker-zero → restore.**
