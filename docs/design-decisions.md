# Design Decisions

Every deviation from the default architecture mapping in the porting spec is recorded
here with its rationale. Format: **DD-N** (decision), context, decision, consequences.

---

## DD-1: EWDK 26100.6584 (amended 2026-07-06)

**Context:** The spec's acceptance criteria say "EWDK build clean." First plan was
the installed WDK 10.0.22621.0 + VS2022 Community MSBuild — but the WDK VSIX that
provides the `WindowsKernelModeDriver10.0` toolset failed to install: the 22621 VSIX
is older than VS 17.14 accepts ("install the extension from Visual Studio Installer
instead", VSIXInstaller exit 9), and both the VS Installer route and a WDK 26100
install require UAC elevation (not autonomous-safe).

**Decision (amended):** Use the **EWDK 26100.6584** ("Windows 11 25H2 EWDK with
VS BuildTools 17.14.0", fwlink 2335681), downloaded to `C:\EWDK\ewdk_26100.6584.iso`
(20 GB). `build.ps1` mounts the ISO on demand (`Mount-DiskImage`, no elevation) and
runs its `BuildEnv\SetupBuildEnv.cmd` + MSBuild. Fully self-contained; matches the
spec's stated default and the Win11 24H2 (26100) test-guest OS.

**Consequences:** WDK 10.0.26100.0, KMDF **1.35**, MSVC 14.44 BuildTools. Findings
baked into the build:
- Driver vcxproj needs explicit `<FilesToPackage Include="$(TargetPath)"/>` (template
  projects carry it; hand-written ones must add it) or Inf2Cat fails with 22.9.1.
- signtool in this kit requires `/fd`; set via `DriverSign.FileDigestAlgorithm=sha256`.
- `CodeAnalysisRuleSet` must be an absolute path
  (`$(WDKContentRoot)CodeAnalysis\DriverRecommendedRules.ruleset`); bare names fail in
  the report step.
- **SDV is absent from WDK 26100** (Microsoft removed/deprecated Static Driver
  Verifier in 24H2-era kits; `Tools\sdv` does not exist). The spec's "SDV where
  applicable" resolves to: not applicable with this kit — compensated by
  DriverRecommendedRules Code Analysis on every `-Test` build and (planned) CodeQL
  with the windows-drivers query suites.

## DD-2: Driver naming and identity

- Binary/service name: `ttkmd.sys` / `ttkmd` (parallels Linux module name `tenstorrent`).
- Repo name `tt-win-kmd` to avoid confusion with the upstream `tt-kmd` checkout it is
  diffed against.
- Grayskull (`DEV_FACA`) is omitted from the INF: upstream binds it but
  unconditionally fails probe with -ENODEV (dropped support); binding a dead ID on
  Windows would only block diagnostic tooling. Revisit if upstream re-adds support.
- Custom device setup class **"TenstorrentAccelerator"**,
  ClassGuid `{17db3097-300a-4168-8a1d-6c26bba9263f}` (generated for this project).
  Rationale: tt-kmd devices are compute accelerators exposed as char devices; the
  in-box `ComputeAccelerator` class is associated with MCDM (DirectML) driver stacks,
  which this driver is not. A custom class avoids MCDM requirements while keeping
  Device Manager grouping clean.
- Device interface: `GUID_DEVINTERFACE_TENSTORRENT`
  `{e2e020f2-998c-4b6f-b98e-b259372c7986}` (generated for this project). Per-device
  reference strings carry the ordinal; stable ASIC-ID-based identity is exposed via a
  query IOCTL and an interface property (spec's mapping for `/dev/tenstorrent/by-id`).
- TraceLogging provider GUID: `{dd99fa3c-98a4-479d-91d2-5b43ef8f5c22}` — see DD-3.
  (Provider name: `Tenstorrent.TtKmd`.)

## DD-3: TraceLogging instead of WPP

**Context:** Spec requires ETW on every IOCTL entry/exit, reset event, and DMA
lifecycle event; permits "WPP or TraceLogging."

**Decision:** TraceLogging (`TraceLoggingProvider.h`). No preprocessor pass, no TMF
files, human-readable events in WPA/tracefmt without symbol lookup, and the same
provider works verbatim in the user-mode shim and tests.

**Consequences:** Slightly larger event payloads than WPP; acceptable.

## DD-4: CTL_CODE scheme

- Device type: `0x80FA` (custom range; low byte echoes the Linux ioctl magic `0xFA`
  from `tt-kmd/ioctl.h:12`).
- Function code: `0x800 + <Linux ioctl nr>` (0x800 is the start of the OEM range;
  offsets preserve the Linux numbering 0..16 mechanically).
- Transfer type: `METHOD_BUFFERED` for all IOCTLs unless the parity matrix documents
  a justified exception.
- Access: `FILE_ANY_ACCESS` (Linux enforces no capability checks on these ioctls;
  access control is the device object's ACL, as on Linux it is the chardev's mode —
  `udev-50-tenstorrent.rules` sets 0666; see open question OQ-2 on the Windows
  default ACL).

## DD-5: QEMU glue owns PCI config space; ttsim provides BAR content

**Context:** `ttsim/docs/libttsim_api.md` states `libttsim_pci_config_wr32` is
"reserved … calling it is a fatal error," yet a real guest OS writes config space
constantly (BAR programming, command register, MSI setup). ttsim's BARs live at fixed
simulator-internal physical addresses advertised through its own config space.

**Decision:** The QEMU glue device implements standard QEMU PCIe config-space
emulation (QEMU owns BAR registers, command, MSI capability). At init it reads
ttsim's config space only to learn vendor/device ID and the simulator-internal BAR
base addresses; BAR sizes come from the Linux driver analysis (Blackhole BAR layout).
Guest BAR accesses are translated as `ttsim_internal_base + offset` into
`libttsim_pci_mem_rd/wr_bytes`. Guest-physical DMA callbacks map to
`pci_dma_read/write` on the glue device. `libttsim_clock` is pumped from a QEMU timer
and around BAR accesses; all calls serialized (libttsim is single-threaded,
non-reentrant — BQL suffices).

**Consequences:** Config-space behavioral parity (e.g., what happens on config writes
the real chip honors) is limited by ttsim; acceptable because tt-kmd itself only
performs standard PCI config accesses through the kernel's PCI core.

## DD-6: Milestone work is gated on the M0 analysis

Per-milestone implementation work (M1+) starts only after
`docs/linux-driver-analysis.md` exists, because the spec makes that document the
gate for all porting work and its findings (BAR layouts, mmap offset encodings,
reset ordering) are inputs to that work. M0 and the test rig do not wait for it;
they depend only on facts already verified directly.

## DD-8: M3 memory-management design (MAP/UNMAP, TLB pool, DMA bufs, pinning)

**MAP/UNMAP ioctls** (`0x80FA2400/2404`) replace `mmap`/`munmap`. Input is the
opaque Linux mmap-offset token (QUERY_MAPPINGS `mapping_base`, ALLOCATE_DMA_BUF
`mapping_offset`, ALLOCATE_TLB `mmap_offset_uc/wc`) plus a byte offset within
the region and a length; output is the user VA. Decoding replicates
`tenstorrent_mmap` (memory.c:1585-1636): regions `(0..7)<<36` for BAR 0/2/4
UC/WC and TLB UC/WC, DMA buffers at `0xF00_0000_0000 + idx*4GiB` (the 4K-page
Linux constant, frozen for the shim per analysis §05 OQ5). Sub-range maps within
BARs are legal (tt-umd depends on them). The open-BAR security model is
preserved deliberately (analysis §06 §"How userspace uses this"): tt-umd
configures TLBs through user BAR0 register writes.

**Mapping mechanism:** device memory is mapped UC/WC with `MmMapIoSpaceEx`, then
exposed to user mode via `IoAllocateMdl` + `MmBuildMdlForNonPagedPool` +
`MmMapLockedPagesSpecifyCache(UserMode, MmNonCached/MmWriteCombined)`; DMA
common buffers map their nonpaged VA `MmCached`. Every mapping is tracked on the
owning file object and force-unmapped at handle cleanup (delivered in the owning
process context) — spec hard constraint 5 groundwork for M4 reset teardown.

**TLB pool:** device-global bitmap of 210 windows (2M ids 0..201, 4G ids
202..209); id 201 kernel-reserved at init (blackhole_init parity); 4G count
clamped by BAR4 length. Exact-size allocation only (tlb.c:20-33: no round-up).
Single owner (file object) per window replaces Linux's bitmap+refcount — the
refcount only matters for dma-buf exports, which are STATUS_NOT_SUPPORTED here.
Hardware TLB registers are deliberately NOT cleared on free (upstream parity;
analysis §06 open question).

**DMA buffers:** WDFDMAENABLER (Scatter/Gather64 profile, 58-bit BH mask) +
`WdfCommonBufferCreate` per buffer; ≤256 MiB, page-multiple, 256 slots per
handle, duplicate index -EINVAL — validation order byte-for-byte with
memory.c:439-458. FREE_DMA_BUF stays -EINVAL (upstream stub). Cleanup order
deliberately INVERTS Linux: iATU region torn down BEFORE the buffer is freed
(analysis §05 porting note — upstream frees memory first, leaving a window
where the device aperture targets freed RAM).

**iATU outbound allocator:** verbatim port (16 regions, first-fit top-down/
bottom-up over [0, 2^58-1], noc_address = (4<<58)+base, ≤1 TiB per region,
registers per blackhole.c:755-788), device-global table + PASSIVE lock,
per-file-object ownership for cleanup.

**PIN_PAGES:** `IoAllocateMdl` + `MmProbeAndLockPages(UserMode, IoWriteAccess)`
(FOLL_LONGTERM analogue: MDL held until unpin/close), then PFN-contiguity walk
— the Linux no-IOMMU path. There is no driver-controllable remapping domain on
Windows client SKUs, so: READ_ONLY → STATUS_NOT_SUPPORTED (-EOPNOTSUPP parity),
and discontiguous buffers → STATUS_INVALID_PARAMETER (-EINVAL parity). Duplicate
(VA, page-count) pin → STATUS_OBJECT_NAME_COLLISION (-EEXIST). `MmUnlockPages`
dirties write-locked pages automatically (unpin_user_pages_dirty_lock parity).

**Deferred from M3:** MAP_PEER_BAR (ioctl 9) requires two devices; libttsim is a
process singleton so the rig cannot exercise it — implementation deferred with
the matrix row noting the blocker. EXPORT_TLB_DMABUF (16) → STATUS_NOT_SUPPORTED
per the matrix design note. SET_NOC_CLEANUP (14) lands with M5 lifecycle work.
