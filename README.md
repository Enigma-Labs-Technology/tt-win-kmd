# tt-win-kmd — Windows KMDF port of the Tenstorrent kernel-mode driver

> **⚠️ NOT PRODUCTION READY — UNFINISHED WORK IN PROGRESS — NO WARRANTY.**
>
> This is an incomplete, experimental port under active development. It is
> **not** finished, **not** stable, and **not** suitable for production or for
> any machine you depend on. The driver is test-signed, runs in kernel mode,
> and **can crash, hang, or corrupt your system**, and it may leave the
> attached hardware in a state that requires a power cycle. It has only been
> exercised on a single p150a board and on the ttsim/QEMU rig. Interfaces,
> behaviour, and documentation may change without notice.
>
> This software is provided **"AS IS", WITHOUT WARRANTY OF ANY KIND**, express
> or implied, including but not limited to the warranties of merchantability,
> fitness for a particular purpose, and non-infringement. In no event shall the
> authors be liable for any claim, damages, or other liability arising from its
> use. **You use it entirely at your own risk.** It is a community effort and is
> **not produced, endorsed, or supported by Tenstorrent.** See `LICENSE` for
> the governing terms and `CHANGELOG.md` for what is and is not there.

Windows port of [tt-kmd](https://github.com/tenstorrent/tt-kmd) for Blackhole
PCIe AI accelerators. Target hardware for this release line is the **p150a**
(`PCI\VEN_1E52&DEV_B140&SUBSYS_00401E52`, the only board the release package
binds) on **Windows 11 x64**, any build from 21H2 (22000) on; the driver targets
the in-box KMDF 1.33. The driver exposes the same
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
| `PIN_PAGES` / `UNPIN_PAGES` | Implemented. Views of the handle's own DMA buffers pin without page locking and work in any DMA domain (DD-16); arbitrary user memory needs contiguous pages and an identity DMA domain |
| Resource locks (`LOCK_CTL`), power state, NOC cleanup | Implemented, tested on ttsim/QEMU |
| Reset (`RESET_DEVICE`) | Flavors 0, 3, 5, 6 available; 1 (PLDR) needs platform support the p150a lacks; **2 and 4 are refused on real hardware** because they wedged the card (DD-14) |
| `SMC_MSG` (new in tt-kmd 2.11.0) | ABI carried, returns `STATUS_NOT_SUPPORTED` (OQ-8) |
| Process ownership, DMA ceiling | Handles are bound to the opening process, a process-exit callback releases leftovers (DD-15); per-device DMA-buffer cap, registry-tunable (DD-17) |
| `FREE_DMA_BUF_EX` (Windows-only) | Implemented: releases a DMA buffer before handle close; refused while mapped or pinned (DD-16) |
| `MAP_PEER_BAR`, `EXPORT_TLB_DMABUF`, `FREE_DMA_BUF` | Not implemented (upstream's `FREE_DMA_BUF` is itself a stub); tt-umd does not use them (OQ-9) |
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
| `src/driver/` | KMDF driver (`ttkmd.sys`), release INF (`ttkmd.inf`, p150a only), test INF (`ttkmd-test.inf`, Debug builds: adds the ttsim/QEMU model and the soft load-test node), version props, vcxproj |
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

## Installing a preview build

Preview packages are test-signed, so the host must trust the test certificate
and allow test-signed kernel code. Do this on a development machine only;
test signing weakens the host's code-integrity posture until you turn it off
again. From an elevated PowerShell, with the package (`ttkmd.sys`,
`ttkmd.inf`, `ttkmd.cat`, `tt-test.cer`) in the current directory:

```powershell
bcdedit /set testsigning on          # then reboot once
certutil -addstore -f Root tt-test.cer
certutil -addstore -f TrustedPublisher tt-test.cer
pnputil /add-driver ttkmd.inf /install
Get-PnpDevice -Class TenstorrentAccelerator     # Status should read OK
.\ttinfo.exe                                     # per-request smoke test
```

To remove it: `pnputil /delete-driver ttkmd.inf /uninstall /force`, then
`bcdedit /set testsigning off` and reboot. `test/vm/guest/install-driver.ps1`
automates the same steps for the test rig.

Attestation-signed packages (EV certificate through the Microsoft Partner
Center) need none of the test-signing steps; the runbook is
`docs/signing-and-deployment.md`.

## Reporting problems

Open an issue with the Windows build, the output of `ttinfo.exe`, and, for a
crash, the bugcheck code and the `ttkmd` TraceLogging session if you have one.
Security-sensitive reports go through `SECURITY.md`.

## Using the driver from tt-umd

The Windows backend of `tt-kmd-lib` (the `windows-port` branch of the sibling
`../tt-umd` checkout) talks to this driver directly: device interface
enumeration, `DeviceIoControl`, and the `MAP`/`UNMAP` requests. Sysmem on
Windows is carved from driver DMA buffers that are pinned to the NOC exactly
like Linux hugepages, so channel 0 sits at `pcie_base`. See
`docs/tt-umd-porting-notes.md` and `../tt-umd/docs/windows.md`.
