# Design Decisions

Every deviation from the default architecture mapping in the porting spec is recorded
here with its rationale. Format: **DD-N** (decision), context, decision, consequences.

---

## DD-1: Installed WDK + VS2022 instead of EWDK

**Context:** The spec's acceptance criteria say "EWDK build clean." The development
host already has WDK 10.0.22621.0 (build targets + KMDF 1.33 headers), WDK 10.0.26100.0
headers (no build targets), VS2022 Community (MSVC 14.44), and the full signing
toolchain (stampinf, Inf2Cat, signtool). No EWDK ISO is present.

**Decision:** Build with the installed WDK 10.0.22621.0 + VS2022 MSBuild rather than
downloading a ~15 GB EWDK ISO. `build.ps1` pins `WindowsTargetPlatformVersion=10.0.22621.0`
(the only WDK version with `WindowsDriver.*.targets` present). The build entry point is
identical in shape to an EWDK build (`MSBuild.exe` + `WindowsKernelModeDriver10.0`
toolset), so switching to an EWDK later is a path change, not a design change.

**Consequences:** KMDF target version is 1.33 (ships with 10.0.22621). Target OS
Windows 11 21H2+ / Server 2022+, consistent with the spec.

## DD-2: Driver naming and identity

- Binary/service name: `ttkmd.sys` / `ttkmd` (parallels Linux module name `tenstorrent`).
- Repo name `tt-win-kmd` to avoid confusion with the upstream `tt-kmd` checkout it is
  diffed against.
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
