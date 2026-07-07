# tt-win-kmd — Windows KMDF port of the Tenstorrent kernel-mode driver

Windows port of [tt-kmd](https://github.com/tenstorrent/tt-kmd) for Blackhole-class
PCIe AI accelerators (primary target: Blackhole p150a, `PCI\VEN_1E52&DEV_B140`).

**Upstream baseline:** tt-kmd `ttkmd-2.10.0-rc1-1-g8c32c2b` (v2.10.1-pre), analyzed
from the sibling checkout at `../tt-kmd`.

## Status — all six milestones accepted

| Milestone | Scope | Result |
|---|---|---|
| M0 | Analysis + scaffolding | ACCEPTED — 6,270-line cited analysis; empty KMDF driver loads clean under Verifier |
| M1 | Enumeration, BARs, device info | ACCEPTED — binds virtual Blackhole; GET_DEVICE_INFO/DRIVER_INFO/QUERY_MAPPINGS byte-parity |
| M2 | ARC firmware messaging + heartbeat | ACCEPTED — msgqueue round-trip + advancing heartbeat vs ttsim |
| M3 | Memory management | ACCEPTED — MAP/TLB/DMA/pin; 10,000-cycle soak leak-free under Verifier DMA checks |
| M4 | Reset, hotplug, lifecycle | ACCEPTED — fd invalidation, mapping zap, 2,000-reset storm, surprise removal, no bugchecks |
| M5 | Telemetry, power, locks | ACCEPTED — hwmon-scaled telemetry, multi-client power aggregation, cancellable blocking lock |
| M6 | Compat shim + handoff | ACCEPTED — `ttwin_compat` + `ttconform` (0 failures through the shim); tt-umd porting guide |

Every functionally-portable IOCTL is at **tested** in the parity matrix. `PIN_PAGES`/
`UNPIN_PAGES` are **functional** (contiguous/direct path tested; the IOMMU/READ_ONLY
path is `STATUS_NOT_SUPPORTED` by design — no driver-controllable IOMMU domain on
Windows client SKUs). `MAP_PEER_BAR` (needs a second device the ttsim rig can't host)
and `EXPORT_TLB_DMABUF` (no Windows dma-buf equivalent; RDMA-P2P only) are deferred by
documented design (`docs/design-decisions.md`, `docs/open-questions.md`).

All acceptance testing runs against a ttsim-backed virtual Blackhole under QEMU/KVM
with Driver Verifier active (special pool, IRQL, pool tracking, I/O, deadlock, DMA);
per-milestone reports in `docs/test-reports/`.

## Layout

| Path | Contents |
|---|---|
| `src/driver/` | KMDF driver (`ttkmd.sys`), INF, vcxproj |
| `src/include/` | Shared user/kernel ABI header (`ttkmd_ioctl.h`) |
| `src/ttwin_compat/` | User-mode POSIX-shaped compatibility shim (M6) |
| `src/tests/` | User-mode test applications |
| `test/` | QEMU + ttsim test-rig for the Windows guest VM |
| `tools/` | ABI ground-truth generators and maintenance scripts |
| `docs/` | Analysis, parity matrix, design decisions, open questions, test reports |

## Key documents

- `docs/linux-driver-analysis.md` — inventory of the Linux driver surface (gates all porting work)
- `docs/ioctl-parity-matrix.md` — per-IOCTL parity status
- `docs/design-decisions.md` — every deviation from the default architecture mapping
- `docs/open-questions.md` — ambiguities, hypotheses, and evidence

## Building

Requires Windows host with WDK 10.0.22621+ and VS2022 (see `docs/design-decisions.md`
for the WDK-instead-of-EWDK rationale). From WSL: `./build.sh`; from Windows:
`powershell -File build.ps1 [-Test]`.
