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
| Card | Blackhole p150a, S/N __________ |
| PCI location | bus 0xC1 (193), dev 0, fn 0 — `PCIROOT(C0)#PCI(0101)#PCI(0000)` |
| Hardware ID | PCI\VEN_1E52&DEV_B140&SUBSYS_00401E52&REV_00 |
| Card origin state | dismounted into Hyper-V DDA (`PCIP\` node); remounted phase 1 |
| ARC / CMFW bundle | __________ (tt_fw_bundle_ver) |
| Telemetry table version | __.__ (major must be ≤ 1, blackhole.c:513) |
| BAR0 / BAR2 / BAR4 sizes (QUERY_MAPPINGS) | ______ / ______ / ______ |
| 4G TLB windows granted (BAR4_len/4GiB, clamp 8) | __ |
| Above-4G decoding / Resizable BAR in BIOS | __________ |
| Kernel DMA Protection (msinfo32) | __________ |
| Secure Boot / testsigning | off (campaign) / on |
| Driver | ttkmd.sys ver ________, KMDF 1.35, /W4+WX + CA clean |
| Driver Verifier | flags 0x9BB on ttkmd.sys — active: ____ |
| ttinfo build | silicon mode (sim oracles behind --sim-oracle) |
| Linux ground-truth capture | method: __________ (dual-boot / Linux box / DDA VM), date ____ |

## Linux ground truth (same card, BEFORE Windows values were trusted)

| Item | Linux value | Source |
|---|---|---|
| tt-smi snapshot | attached: ____________ | tt-smi -s |
| BAR sizes / MPS / MRRS | ____________ | lspci -vv |
| hwmon channels | ____________ | /sys/class/hwmon |
| tt_* sysfs | ____________ | /sys/class/tenstorrent |
| Heartbeat advance rate | ____ /s | tt_heartbeat polled |

## Ladder execution log (strict order; abort on first red)

| Rung | Command | Result | Heartbeat after | Notes |
|---|---|---|---|---|
| a install+bind | 20-install-driver.ps1 | ☐ | — | Status/Service: ____ |
| b info/map/telem (read-only) | ttinfo.exe | ☐ | H0=____ | |
| c TLB + safe reg reads | ttinfo --only tlb | ☐ | ____ | NOC_ID ∈{2,11}: __ |
| d DMA loopback | ttinfo --only dma | ☐ | ____ | identity probe: __ |
| e PIN_PAGES | ttinfo --only pin | ☐ | ____ | phys=________ |
| f RESTORE/POST_RESET | ttinfo --reset restore-then-post | ☐ | ____ | result=__, MPS after=__ |
| g CONFIG_WRITE | ttinfo --reset config-write | ☐ | ____ | stale handle→REMOVED: __ |
| h ASIC_DMC_RESET | ttinfo --reset asic-dmc | ☐ | ____ | ARC TEST ok: __ |
| i RESET_PCIE_LINK (PLDR) | ttinfo --reset pcie-link | ☐ | ____ | re-enum observed: __ |

Inter-rung health checks: HC-CFG (vendor==0x1E52 via GET_DEVICE_INFO), HC-REG
(NOC_ID @BAR0 0x1FD04044 & 0x3F ∈ {2,11}), HC-HB (QUERY_TELEMETRY heartbeat
advancing over ≥1.5 s, direction-only, wrap-tolerant). Abort on any all-ones
read, WHEA event, Verifier violation, or bugcheck.

## DMA-remapping matrix (spec requirement)

| Kernel DMA Protection / VT-d | Coherent DMA (rung d) | PIN_PAGES (rung e) |
|---|---|---|
| OFF / identity domain | ☐ | ☐ expect pass |
| ON / translating domain | ☐ expect pass | ☐ expect STATUS_NOT_SUPPORTED (DD-8 guard) |

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

| Field (tag) | Windows | Linux/tt-smi | Unit | Tol | PASS |
|---|---|---|---|---|---|
| card_type (1, decoded) | | p150a (0x40) | — | exact | ☐ |
| serial (1:2) | | | hex | exact | ☐ |
| asic_id (61:62) | | | hex | exact | ☐ |
| aiclk (14) | | | MHz | ±5% | ☐ |
| axiclk (15) / arcclk (16) | | | MHz | ±5% | ☐ |
| asic_temp (11, 16.16→m°C) | | | m°C | ±2°C | ☐ |
| temp_max (56) | | | m°C | exact | ☐ |
| vcore (6) / vcore_max (9) | | | mV | ±3% | ☐ |
| current (8) / curr_max (55) | | | mA | ±10% | ☐ |
| power (7) / power_max (64) | | | µW | ±10% | ☐ |
| fan_rpm (41) | | | RPM | ±10% | ☐ |
| fw_bundle_ver (28, decoded) | | | ver | exact | ☐ |
| therm_trip_count (60) | | | count | exact | ☐ |
| heartbeat (32) advancing | Δ/1.5s = ____ | rate: ____ | — | >0 | ☐ |

## OQ-4 resolution (silicon)

- Telemetry table discovered via ARC_TELEMETRY_PTR = RESET_SCRATCH(13)
  (0x80030434), not the ttsim fixed CSM+0x100. Live base = 0x________,
  entries = ____, version = __.__.
- ttinfo silicon mode uses QUERY_TELEMETRY (driver-side discovery): ☐

## OQ-5 resolution (silicon)

1. RESET_PCIE_LINK via PLDR (DD-11): SupportedResetTypes = ______;
   result=__; interface departure/arrival observed: ☐; fresh PrepareHardware
   re-init clean: ☐.
2. Config save/restore + DBI MPS (DD-12): after ASIC_RESET→POST_RESET —
   BAR0 decodes: ☐; Command MSE/BME restored: ☐; marker clear: ☐;
   MPS after restore = ____ (saved ____): ☐; MRRS = 4096: ☐;
   WHEA/AER events post-reset: ____.

## Divergence log (silicon vs sim/Linux ground truth)

| # | Rung/IOCTL | Observed on silicon | Ground truth (cite) | Root cause | Action |
|---|---|---|---|---|---|
| D1 | | | | | |

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
