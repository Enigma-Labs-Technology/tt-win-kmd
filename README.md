# tt-win-kmd — Windows KMDF port of the Tenstorrent kernel-mode driver

Windows port of [tt-kmd](https://github.com/tenstorrent/tt-kmd) for Blackhole-class
PCIe AI accelerators (primary target: Blackhole p150a, `PCI\VEN_1E52&DEV_B140`).

**Upstream baseline:** tt-kmd `ttkmd-2.10.0-rc1-1-g8c32c2b` (v2.10.1-pre), analyzed
from the sibling checkout at `../tt-kmd`.

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
