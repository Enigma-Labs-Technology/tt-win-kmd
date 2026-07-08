# Real-Silicon Bring-Up Report — Blackhole p150a (first run on hardware)

**Date:** 2026-07-__ · **Status: DRAFT** (DRAFT → IN-PROGRESS → ACCEPTED / NO-GO)

> First execution of ttkmd against physical Tenstorrent Blackhole p150a silicon
> (PCI\VEN_1E52&DEV_B140&SUBSYS_00401E52). All prior milestones (M0-M6) were
> validated only against the ttsim virtual device under QEMU; this report is the
> silicon acceptance record. Ground truth for every expected behavior is tt-kmd
> (cited analysis §NN). Values that were sim-synthetic in M0-M6 are re-derived
> here from the live card and cross-checked against tt-smi on the same card.
> Code changes gating this campaign: DD-11 (PLDR link reset), DD-12
> (config/MPS/MRRS restore + ARC message content fixes), ttinfo silicon mode.

## Environment

| Item | Value |
|---|---|
| Host | DESKTOP-4OQSUDU, Windows 11 Enterprise Evaluation, build 26200 |
| Card | Blackhole p150a, S/N `0000040331921017`, ASIC ID `7B772ECCB9DB2F81` |
| PCI location | bus 0xC1 (193), dev 0, fn 0 — `PCIROOT(C0)#PCI(0101)#PCI(0000)` |
| Hardware ID | PCI\VEN_1E52&DEV_B140&SUBSYS_00401E52&REV_00 (subsystem 1e52:0040 = p150a) |
| Card origin state | dismounted into Hyper-V DDA (`PCIP\` node); remounted phase 1 |
| ARC / CMFW bundle | 19.6.0.0 expected (Linux `tt_fw_bundle_ver`; raw 0x13060000). M3/DMC app 0.22.0.0. **Firmware anchor — Windows MUST run the SAME bundle.** |
| Telemetry table version | major ≤ 1 expected (blackhole.c:513); confirm live |
| BAR0 / BAR2 / BAR4 sizes (QUERY_MAPPINGS) | 512 MiB / 1 MiB / 32 GiB (Linux sysfs `resource`; resolves OQ-1) |
| 4G TLB windows granted (BAR4_len/4GiB, clamp 8) | 8 expected (32 GiB → min(8,8)) |
| Above-4G decoding / Resizable BAR in BIOS | MUST be enabled (BAR4 = 32 GiB needs large-BAR) |
| Kernel DMA Protection (msinfo32) | __________ (Linux host has AMD-Vi ON → run the ON matrix cell) |
| Secure Boot / testsigning | off (campaign) / on |
| Driver | ttkmd.sys `[M7]` (commits 6db52c3 + 9139174 + this), KMDF 1.35, /W4+WX + CA clean |
| Driver Verifier | flags 0x9BB on ttkmd.sys — active: ____ |
| ttinfo build | silicon mode (sim oracles behind --sim-oracle) |
| Linux ground-truth capture | `real-silicon-linux/`, host omarchy, kernel 7.0.10-arch1-1, tt-kmd 2.9.1-pre, tt-smi 3.1.1 — captured 2026-07-08 |

## Linux ground truth (same card — captured BEFORE Windows values are trusted)

Full reference set in `real-silicon-linux/` (see `00-summary.md`). Card healthy:
clean probe, Gen5 x16, ~51 °C at 38 W idle, heartbeat alive, therm_trip 0, and
the upstream `ttkmd_test` suite passes end-to-end (unprivileged exit 0 with 4
root-gated skips; **root run exit 0 with zero skips**; post-reset run exit 0,
byte-identical output).

| Item | Linux value | Source |
|---|---|---|
| Link | 32.0 GT/s (Gen5) x16, negotiated = max | lspci-vvv-root.txt |
| Interrupt | single-vector plain MSI @ 0x50, IRQ 152 (NOT MSI-X). Port is polling-only — no MSI used. | pci-sysfs-devnode.txt |
| BAR0/BAR2/BAR4 | 512 MiB / 1 MiB / 32 GiB, all 64-bit prefetchable | sysfs resource, lspci |
| MPS / MRRS | **512 B in effect** (DevCap supports 1024) / **4096 B**; DevCtl @0x78 = 0x515f | lspci-vvv-root.txt |
| PCI_COMMAND baseline | 0x0406 = Mem+ BusMaster+ DisINTx+; parity bit (0x40) = 0 | lspci-config-baseline.txt |
| Cap chain (Windows walker) | 0x40 PM → 0x50 MSI → 0x70 PCIe; ext 0x100 AER → 0x148 → … | 00-summary.md |
| IOMMU | AMD-Vi ON, default domain **Translated**, card alone in group 9 | iommu.txt |
| Firmware anchor | bundle 19.6.0.0 / M3 0.22.0.0; CM-ARC & ETH FW = **None** (BH) | manifest.txt, tt-smi-snapshot.json |
| Heartbeat rate | ~10 Hz sysfs (monotonic); tt-smi displays raw/6 | heartbeat-samples.txt |

## Ladder execution log (strict order; abort on first red)

| Rung | Command | Result | Heartbeat after | Notes |
|---|---|---|---|---|
| a install+bind | 20-install-driver.ps1 | **PASS** 2026-07-08 | — | Status OK, Service ttkmd, pkg oem36. Host gauntlet first: HVCI off, /CETCOMPAT (b7a5f74), FACEIT AC disabled — see "Host security gauntlet" below |
| b info/map/telem (read-only) | ttinfo.exe | **PASS** exit 0 | advancing (in-dump assert) | present-mask 0x1fff; BARs 512M/1M/32G; all values vs GT: exacts exact, lives in range; fw 19.6.0.0 = anchor; 0 WHEA, 0 bugcheck |
| c TLB + safe reg reads | ttinfo --only tlb | **PASS** exit 0 | advancing (in-test, both paths) | 2M window to ARC (8,0) via TLB-map and open-BAR0 paths; EBUSY/double-free negatives correct |
| d DMA loopback | ttinfo --only dma | **PASS** exit 0 | advancing | 64K common buffer CPU-verified, bus=0x201a469000 (<2^58); iATU region noc=0x13ffffffffff0000 (inside 4<<58 aperture, top-down) |
| e PIN_PAGES | ttinfo --only pin | **PASS** exit 0 | 2389 advancing (post-rung) | phys=0x1f6e3ee000; identity-domain probe ALLOWED pinning (HVCI off + FACEIT_IOMMU gone -> 1:1). Note: the "remapping ON" matrix cell is untestable with a test-signed driver — HVCI must be off to load it; guard refusal path is review-validated only |
| f RESTORE/POST_RESET | ttinfo --reset restore-then-post | **PASS** exit 0 | advancing (asserted) | restore=0, post=0: first live run of config restore + DBI MPS write-back + re-init — clean |
| g CONFIG_WRITE | ttinfo --reset config-write | **DIVERGENCE D4** | all-ones after | result=0 (trigger delivered) but the chip HARD-WEDGED: link down, no retrain, restore correctly refused (vendor read FFFF), card fell off the bus. See D4 — flavor contraindicated on real BH |
| h ASIC_DMC_RESET | ttinfo --reset asic-dmc | SKIPPED pending recovery | — | flag-4 ASIC_RESET shares g's trigger — BOTH contraindicated until D4 is understood |
| i RESET_PCIE_LINK (PLDR) | ttinfo --reset pcie-link | honest result=1 | — | platform reports PLDR unsupported for this device (no ACPI _RST on this board) — DD-11 capability gate refused correctly; tested while card was wedged |

Inter-rung health checks: HC-CFG (vendor==0x1E52 via GET_DEVICE_INFO), HC-REG
(NOC_ID @BAR0 0x1FD04044 & 0x3F ∈ {2,11}), HC-HB (QUERY_TELEMETRY heartbeat
advancing over ≥1.5 s, direction-only, wrap-tolerant). Abort on any all-ones
read, WHEA event, Verifier violation, or bugcheck.

## DMA-remapping matrix (spec requirement)

Ground truth: the Linux host runs AMD-Vi **ON** (Translated domain) and there
discontiguous PIN_PAGES succeeds via the IOVA/SG path. The Windows port does
NOT implement the translated path (DD-8); its identity-domain probe at
PrepareHardware decides which cell applies. With our INF not opting into
DMA remapping, an internal PCIe device is typically left in passthrough
(identity) even on a VT-d/AMD-Vi-capable host — so the OFF row is the expected
default; the ON row occurs only under an enforcing policy (VBS/HVCI memory
integrity, "force isolation").

| Kernel DMA Protection / VT-d | Coherent DMA (rung d) | PIN_PAGES (rung e) |
|---|---|---|
| OFF / identity domain | ☐ | ☐ expect pass |
| ON / translating domain | ☐ expect pass | ☐ expect STATUS_NOT_SUPPORTED (DD-8 guard — safe refuse, NOT parity with Linux's IOMMU pin) |

**Documented divergence:** Linux's working discontiguous pin under IOMMU is NOT
reproduced on Windows by design (DD-8). Our guard refuses rather than corrupts;
tt-umd workloads needing scattered host pins require either an identity DMA
domain (VT-d off / device in passthrough) or a future translated-path
implementation. This is a known functional gap, not a regression.

## Per-IOCTL parity vs silicon

| nr | IOCTL | Sim status (matrix) | Silicon result | Divergence? |
|---|---|---|---|---|
| 0 | GET_DEVICE_INFO | tested | ☐ | |
| 2 | QUERY_MAPPINGS | tested | ☐ | BAR sizes real vs rig |
| 3 | ALLOCATE_DMA_BUF | tested | ☐ | |
| 5 | GET_DRIVER_INFO | tested | ☐ | |
| 6 | RESET_DEVICE (all flavors) | tested (sim) | ☐ | DD-11/DD-12 first silicon |
| 7/10 | PIN/UNPIN_PAGES | functional | ☐ | identity-domain guard |
| 8 | LOCK_CTL | tested | ☐ | |
| 11-13 | ALLOCATE/FREE/CONFIGURE_TLB | tested | ☐ | |
| 14 | SET_NOC_CLEANUP | tested | ☐ | |
| 15 | SET_POWER_STATE | tested (sim no-op) | ☐ | first real FW parse of 0x21 |
| — | QUERY_TELEMETRY / MAP / UNMAP | tested | ☐ | |
| 9 | MAP_PEER_BAR | not-started | n/a | single card |
| 16 | EXPORT_TLB_DMABUF | not-started | n/a | no Windows dma-buf |

## Telemetry cross-check (Windows QUERY_TELEMETRY vs tt-smi, same card)

> PASS = within tolerance of the Linux capture, never equality with a sim
> constant. Board/VREG temps (tags 13/12) are exposed by NEITHER driver
> (tt-smi reads them via tt-umd) — do not file their absence as a divergence.

| Field (tag) | Windows | Linux/tt-smi (idle) | Unit | Tol | PASS |
|---|---|---|---|---|---|
| card_type (1, decoded) | | p150a (0x40) | — | exact | ☐ |
| serial (1:2) | | 0000040331921017 | hex | exact | ☐ |
| asic_id (61:62) | | 7B772ECCB9DB2F81 | hex | exact | ☐ |
| aiclk (14) | | 800 | MHz | ±5%* | ☐ |
| axiclk (15) / arcclk (16) | | 960 / 800 | MHz | ±5%* | ☐ |
| asic_temp (11, 16.16→m°C) | | 51431 | m°C | ±2°C | ☐ |
| temp_max (56) | | 90000 | m°C | exact | ☐ |
| vcore (6) / vcore_max (9) | | 740 / 900 | mV | ±3%* | ☐ |
| current (8) / curr_max (55) | | 52000 / 200000 | mA | ±10%* | ☐ |
| power (7) / power_max (64) | | 38000000 / 150000000 | µW | ±10%* | ☐ |
| fan_rpm (41) | | 2000 | RPM | ±10%* | ☐ |
| fw_bundle_ver (28, decoded) | | 19.6.0.0 (0x13060000) | ver | exact | ☐ |
| m3app_fw_ver (26) | n/a (not in struct) | 0.22.0.0 | ver | — | — |
| therm_trip_count (60) | | 0 | count | exact | ☐ |
| heartbeat (32) advancing | Δ/1.5s = ____ | ~10 Hz | — | >0 | ☐ |

\* Live-value tolerances apply only when both captures are at the same idle
DVFS state (power_policy=on, aiclk ~800). Temp/power drift with load and
ambient — treat as sanity ranges, not equalities. All telemetry scaling
formulas were verified byte-identical to Linux hwmon against these exact raws
(e.g. tag 11 16.16→m°C, tag 7 W→µW, tag 8 A→mA); a value >10× out of range
means a firmware-unit assumption is wrong, not a scaling-code bug.

## OQ-4 resolution (silicon)

- Telemetry table discovered via ARC_TELEMETRY_PTR = RESET_SCRATCH(13)
  (0x80030434), not the ttsim fixed CSM+0x100. Live base = 0x________,
  entries = ____, version = __.__.
- ttinfo silicon mode uses QUERY_TELEMETRY (driver-side discovery): ☐

## OQ-5 resolution (silicon)

Linux ground truth (`config-reset-delta-analysis.txt`) already confirms the
mechanism our port replicates: probe `pci_save_state` → set PCI_COMMAND parity
bit as reset marker → ASIC/DMC reset → poll marker-zero (hardware clears
COMMAND) → `safe_pci_restore_state` (restores DevCtl MPS 512 / MRRS 4096) →
re-save. Across a real reset, **all architected config is byte-identical**
(only VSEC/live-counter bytes change), MPS/MRRS survive **only because the
driver restores them**, and the parity marker reads 0 outside the reset window.
This is exactly the DD-12 sequence. Windows-side confirmation still to record:

1. RESET_PCIE_LINK via PLDR (DD-11): SupportedResetTypes = ______;
   result=__; interface departure/arrival observed: ☐; fresh PrepareHardware
   re-init clean: ☐.
2. Config save/restore + DBI MPS (DD-12): after ASIC_RESET→POST_RESET —
   BAR0 decodes: ☐; Command MSE/BME restored: ☐; marker clear: ☐;
   MPS after restore = ____ (expect **512** / enc 2, NOT the 1024 device max):
   ☐; MRRS = 4096 (enc 5): ☐; WHEA/AER events post-reset: ____.

## Host security gauntlet (first-load failures and resolutions)

Four independent host mechanisms blocked the test-signed driver before first
load; each masked the next. Recorded so future bring-ups run preflight first
(00-preflight.ps1 now gates all four):

| # | Gate | Symptom | Resolution |
|---|---|---|---|
| 1 | Secure Boot | testsigning ignored | disabled in UEFI (campaign only) |
| 2 | Memory Integrity (HVCI) | Code 39, status 0xC0000022; VTL1 CI rejects test certs even in test mode | Core Isolation → Memory integrity off + reboot |
| 3 | Kernel CET shadow stacks (`KernelShadowStacks Enabled=1`) | StartService 1450 / 0xC000009A, DriverEntry never reached | binary linked /CETCOMPAT (b7a5f74) — enforcement kept ON |
| 4 | FACEIT anti-cheat (`FACEIT_AC.sys`, `FACEIT_IOMMU.sys`, system-start) | rotating generic statuses; CI verbose shows the file never opened — kernel callback veto before CI | both services disabled + reboot (campaign only) |

Teardown checklist additions: re-enable Secure Boot, Memory Integrity, FACEIT
(`sc config ... start= system`), remove test cert from Root/TrustedPublisher.

## Reset behavior expectations (from Linux ground truth)

Linux `reset-timing.txt` / `dmesg-reset.txt` establish what the Windows reset
rungs (f–i) must tolerate:

- **Timing:** ~2.75 s total; **~1.64 s telemetry outage** while the ARC
  reboots; `tt_heartbeat` **restarts from ~0** afterward (counter restart is
  the authoritative "firmware rebooted" signal). QUERY_TELEMETRY must tolerate
  a ~2 s failure window; the ttinfo reset health check retries across it.
- **Linux does an IN-PLACE reset** — the device never leaves the bus, no
  re-enumeration. **But see D1: Windows platform DPC likely diverges here.**
- Post-reset the card is healthy: Gen5 x16 relinks, GDDR retrains, telemetry
  normal, therm_trip still 0, config header byte-identical.

## Divergence log (silicon vs sim/Linux ground truth)

| # | Rung/IOCTL | Observed / expected on silicon | Ground truth (cite) | Root cause | Action |
|---|---|---|---|---|---|
| D1 | h/i ASIC-DMC / link reset | Windows platform **DPC** likely surprise-removes + re-enumerates the device during a driver-initiated reset (Linux stays in-place) | `dmesg-reset.txt`: root-port DPC containment, Uncorrectable Fatal, SDES surprise-down; tt-kmd has no `error_detected` cb so Linux logs "recovery failed" yet keeps working | The reset drops the PCIe link; the AMD root port's DPC contains it. Linux ignores AER and resets in place; Windows DPC/ACPI error handling engages and may tear the stack down | **OQ-6** filed. Run rung h under Verifier with recovery armed; expect possible re-enum (treat like DD-11 PLDR: reopen handle, run POST_RESET on a fresh one). Fix precisely only if it bugchecks. |
| D2 | e PIN_PAGES (IOMMU on) | expect STATUS_NOT_SUPPORTED where Linux's discontiguous pin succeeds | `iommu.txt` (AMD-Vi Translated); suite pins discontiguous OK | DD-8: Windows port has no translated-domain pin path; guard refuses to avoid corruption | Documented gap, not a regression. See DMA matrix. |
| D3 | telemetry | m3app_fw_ver (0.22.0.0), CM-ARC/ETH FW (None) not surfaced by QUERY_TELEMETRY | manifest.txt | struct exposes fw_bundle only; BH has no CM/ETH FW | Cosmetic coverage gap; tolerate absent, do not error. |
| D4 | g CONFIG_WRITE (and by trigger-sharing, ASIC_RESET flag 4) | Chip reset fired, then PCIe link permanently down: config reads FFFF, no autonomous retrain, endpoint absent after parent-port rescan (phantom). No DPC/AER/WHEA events on Windows. Recovery required PERST# (cold power cycle) | Linux GT never exercised this flavor — tt-smi resets via the DMC path (flag 5); Linux's DPC handler also contains+recovers the port on link-down, Windows client logged nothing | `pcie_timer_interrupt` (cfg 0x930/0x934) on real BH appears to hard-reset the DWC controller without firmware coordinating link re-train — un-validated flavor on silicon | CONTRAINDICATE flavors 2 and 4 on real BH pending upstream clarification (candidate OQ-7); tooling should use DMC (5) + POST_RESET. ttinfo gained no guard — operator ladder only |
| D5 | i RESET_PCIE_LINK | PLDR unsupported on this platform (`SupportedResetTypes` lacks PlatformLevelDeviceReset — consumer board, no ACPI _RST); honest result=1 | Linux SBR via upstream bridge works (pcie.c:61-90) | Windows exposes no supported in-band link reset for this device on this board | DD-11 refusal path validated. Windows-side link recovery escalates: parent-port restart (insufficient for a dead endpoint) → warm reboot → cold power cycle |
| D6 | post-D4 recovery | **Host bugcheck 0x124 (WHEA, arg1=0x4 PCIe)** ~25 min after warm-reboot recovery: fatal uncorrectable AER on root port c0:01.1 while the system was idle (last driver op: read-only telemetry, minutes prior). Dump: 070826-14312-01.dmp / MEMORY.DMP | Linux GT: the identical link-down class was CONTAINED by DPC and recovered non-fatally (`dmesg-reset.txt`) | Two compounding causes: (1) warm reboot does NOT re-assert PERST# — the chip's PCIe controller stayed marginal after the D4 wedge and dropped the link again; (2) Windows client has NO DPC containment, so any fatal uncorrectable PCIe error = whole-machine 0x124 | **OQ-6 RESOLVED (worst case): link-down on this host is a host crash, not a contained event.** Recovery rule hardened: after ANY chip wedge, cold power cycle is MANDATORY even if telemetry looks healthy post-warm-reboot. Mitigation for deliberate reset tests: `bcdedit /set pciexpress forcedisable` disables OS AER handling for the test window (reversible) |

## 24-hour soak

- Command: wall-clock loop of `ttinfo.exe --soak 100000` under Verifier 0x9BB.

| Counter | Before | After | Pass criterion |
|---|---|---|---|
| heartbeat (32) | | | advancing, monotonic |
| asic_temp (11) vs limit (56) | | | never ≥ limit |
| therm_trip_count (60) | | | UNCHANGED |
| Verifier Current Pool Allocations | | | 0 / 0 (no leak) |
| WHEA-Logger events | 0 | | 0 |
| WER bugcheck records (1001) | 0 | | 0 |
| soak batches exit 0 | | | all |

## Performance baseline (for the Windows TLB-throughput comparison)

Linux `ttkmd_bench` on the same card, 2 MiB BAR aperture → DRAM NoC core, card
idle (`real-silicon-linux/benchmark/results-linux.txt`):

| Metric | Linux | Windows | Notes |
|---|---|---|---|
| WC write host→dev | 7.99 GB/s | ____ | our WC mapping = MmWriteCombined (memory.c) |
| UC write host→dev | 7.99 GB/s | ____ | our UC mapping = MmNonCached |
| UC read dev→host | 0.04 GB/s | ____ | UC reads are inherently slow |
| MMIO read latency | 741 ns | ____ | 200k reads |

Note the Linux WC and UC write rates are equal here (~8 GB/s) — the write path
is bandwidth-limited beyond the mapping type. A Windows WC write far below
~8 GB/s would point at the mapping-attribute selection, not the hardware.

## Go / No-Go recommendation

- Rungs a-e (non-destructive): ☐ GREEN ☐ RED — __________
- Rungs f-i (reset, DD-11/DD-12): ☐ GREEN ☐ RED — __________
- Telemetry cross-check: ☐ within tolerance ☐ divergent — __________
- 24 h soak: ☐ clean ☐ leaks/faults — __________
- **Recommendation: GO / GO-WITH-CAVEATS / NO-GO** for attestation signing.
  Rationale: __________
- Blocking items: TT_DEBUG_INTERFACES=1 must be removed from ttkmd.vcxproj
  for the attestation build; __________

## Artifacts

`C:\tt-firstrun\<ts>\` captures; ttinfo/ttconform logs; verifier /query dump;
tt-smi capture; ETW trace (Tenstorrent.TtKmd) of first contact; soak logs.
