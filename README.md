# tt-win-kmd — Windows KMDF port of the Tenstorrent kernel-mode driver

Windows port of [tt-kmd](https://github.com/tenstorrent/tt-kmd) for Blackhole
PCIe AI accelerators. Target hardware for this release line is the **p150a**
(`PCI\VEN_1E52&DEV_B140`) on **Windows 11 x64**. The driver exposes the same
IOCTL ABI as the Linux driver (byte-identical request structures, one Windows
control code per Linux request number) plus two Windows-only requests that
replace `mmap`/`munmap`, so the Tenstorrent user-mode driver runs on top of it
through its `tt-kmd-lib` Windows backend.

**Upstream baseline:** tt-kmd `ttkmd-2.11.0` (commit `c05f224`), checked out in
the sibling directory `../tt-kmd`. `docs/abi-ground-truth.txt` is generated
from that header and enforced by `static_assert`s in
`src/include/ttkmd_abi_check.h`.

**Licence:** GPL-2.0-only, as a derivative work of tt-kmd. See `LICENSE` and
`NOTICE`. This is a community port; it is not produced or supported by
Tenstorrent.

## Status

| Area | State |
|---|---|
| Enumeration, device/driver info, BAR queries | Implemented, tested on ttsim/QEMU and on a p150a |
| ARC firmware messaging, heartbeat, telemetry | Implemented, tested on ttsim/QEMU and on a p150a |
| BAR / TLB-window / DMA-buffer mappings (`MAP`/`UNMAP`) | Implemented, tested (10k-cycle soak under Driver Verifier) and on a p150a |
| TLB allocate/free/configure | Implemented, silicon tested |
| DMA buffers (`ALLOCATE_DMA_BUF`, up to 256 MiB, iATU-mapped) | Implemented, silicon tested |
| `PIN_PAGES` / `UNPIN_PAGES` | Implemented for physically contiguous memory (driver DMA buffers); refused in a DMA-remapped (IOMMU) domain |
| Resource locks (`LOCK_CTL`), power state, NOC cleanup | Implemented, tested on ttsim/QEMU |
| Reset (`RESET_DEVICE`) | Flavors 0, 3, 5, 6 available; 1 (PLDR) needs platform support the p150a lacks; **2 and 4 are refused on real hardware** because they wedged the card (DD-14) |
| `SMC_MSG` (new in tt-kmd 2.11.0) | ABI carried, returns `STATUS_NOT_SUPPORTED` (OQ-8) |
| `MAP_PEER_BAR`, `EXPORT_TLB_DMABUF`, `FREE_DMA_BUF` | Not implemented (OQ-9); tt-umd does not use them |
| Wormhole / Grayskull | Not supported (Blackhole only) |

Per-request detail is in `docs/ioctl-parity-matrix.md`; the silicon results are
in `docs/test-reports/real-silicon.md`.

## Branches

- `main`: the accepted milestone history (M0–M7).
- `workload`: this branch, rebased ABI to tt-kmd 2.11.0, licensing, the
  silicon reset-safety change, and the documentation for the tt-umd Windows
  backend.
- `wip/fail-closed-hardening`: an earlier, fail-closed experiment that removed
  the mapping family and gated device start behind an unapproved product
  contract. Preserved for reference only; it cannot run workloads.

## Layout

| Path | Contents |
|---|---|
| `src/driver/` | KMDF driver (`ttkmd.sys`), INF, vcxproj |
| `src/include/` | Shared user/kernel ABI header (`ttkmd_ioctl.h`) and its static ABI checks |
| `src/ttwin_compat/` | POSIX-shaped user-mode shim (`tt_open`/`tt_ioctl`/`tt_mmap`) used by the conformance test |
| `src/tests/` | `ttinfo.exe` (per-IOCTL tests, `--soak`) and `ttconform.exe` (every IOCTL through the shim) |
| `test/` | QEMU + ttsim test rig for a Windows 11 guest, silicon first-contact scripts |
| `tools/` | ABI ground-truth generator |
| `docs/` | Linux driver analysis, parity matrix, design decisions, open questions, test reports |

## Building

The supported build is the EWDK 26100.6584 ISO configured in `build.ps1`; no
installed Visual Studio or WDK is required. From Windows PowerShell:

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1 -Configuration Release [-Test]
```

`-Test` adds a rebuild plus Driver Code Analysis with warnings as errors.
Artifacts land in `out/<Configuration>/`. From WSL, `./build.sh [Release|Debug]
[--test]` stages the tree to the Windows side and copies the artifacts back.

The test rig (QEMU with the `ttsim-bh` device, Windows 11 guest, Driver
Verifier) is documented in `test/README.md`; the silicon first-contact ladder in
`test/silicon/README.md`.

## Signing and installing

Development builds are test-signed and need `bcdedit /set testsigning on` on
the target. Release builds are attestation-signed through the Microsoft
Partner Center with an EV certificate; the runbook is
`docs/signing-and-deployment.md`.

## Using the driver from tt-umd

The Windows backend of `tt-kmd-lib` (the `windows-port` branch of the sibling
`../tt-umd` checkout) talks to this driver directly: device interface
enumeration, `DeviceIoControl`, and the `MAP`/`UNMAP` requests. Sysmem on
Windows is carved from driver DMA buffers that are pinned to the NOC exactly
like Linux hugepages, so channel 0 sits at `pcie_base`. See
`docs/tt-umd-porting-notes.md` and `../tt-umd/docs/windows.md`.
