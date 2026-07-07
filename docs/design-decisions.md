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
