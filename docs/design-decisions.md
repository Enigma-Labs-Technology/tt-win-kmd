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

## DD-9: M4 reset/lifecycle design

**reset_rwsem → ERESOURCE.** A device-context `ERESOURCE` (ExInitializeResourceLite)
serializes reset against all else: RESET_DEVICE acquires exclusive, every other
ioctl and MAP/UNMAP/PIN/UNPIN and file cleanup acquire shared (under
KeEnterCriticalRegion). Exclusive acquire drains in-flight shared holders — the
Linux "reset waits for in-flight ioctls" behavior. All handlers run at PASSIVE.

**Gen-bump fd invalidation (the spec's explicit criterion).** `ResetGen` is a
device LONG64; each file object latches `OpenResetGen` at create. Destructive
flavors bump `ResetGen` and re-latch the *resetter's* file object (bump_reset_gen
parity), so every other open handle fails the gate with STATUS_DEVICE_REMOVED
(-ENODEV) permanently, while the resetter keeps a live handle to finish the
sequence. Post-reset, only GET_DEVICE_INFO / GET_DRIVER_INFO / RESET_DEVICE are
allowed until POST_RESET clears `NeedsHwInit` (chardev.c:616-624).

**VMA-zap → tracked-mapping teardown.** Windows has no zap-PTEs primitive, so the
port tracks every user mapping (already true since M3) plus the creating
PEPROCESS, and on a destructive reset unmaps them all: same-process mappings via
MmUnmapLockedPages in-context, cross-process via KeStackAttachProcess. After
unmap the user VA is gone, so any stale access faults (STATUS_ACCESS_VIOLATION) —
the "torn down or fault safely, never dangle" requirement (spec constraint 5).
DMA-buffer mappings are NOT zapped (Linux parity: they are host RAM, refcounted
by the handle). A device-global file-object list (guarded by the reset resource
held exclusive during zap) lets the resetter reach every handle's mappings.

**Reset flavors on the rig.** The seven flavors and their wrapper semantics
(gen-bump / zap / needs_hw_init / result=!ok) are ported exactly. Hardware
triggers: `pcie_timer_interrupt` (CONFIG_WRITE, BH ASIC_RESET) and the reset
marker (PCI_COMMAND parity bit) are config-space accesses to the device's OWN
config space, ported via BUS_INTERFACE_STANDARD Get/SetBusData (kept referenced
from PrepareHardware). The secondary-bus reset (RESET_PCIE_LINK) touches the
upstream bridge, which a KMDF function driver cannot do; it maps to the PCI reset
interface where available and is otherwise a documented rig no-op (zap still
runs). BH ASIC_DMC_RESET uses the ARC TRIGGER_RESET message (M2 msgqueue).

**Config save/restore + MPS (was deferred as OQ-5 — now implemented, see
DD-12).** On the ttsim rig the fake reset preserved BARs, so
`safe_pci_restore_state` was a device-present check; for silicon it is the real
pci_save_state/restore plus the DBI Max-Payload-Size snapshot (DD-12).
init_hardware re-run sets MRRS then sends ASIC_STATE0 + SET_WDT_TIMEOUT and
re-probes telemetry (blackhole.c:620-639 parity). RESET_PCIE_LINK is now a real
PLDR (DD-11), no longer a rig no-op.

**Surprise removal.** EvtDeviceSurpriseRemoval sets `Detached` and zaps mappings;
the gate then returns STATUS_DEVICE_REMOVED for everything. iATU teardown already
skips hardware writes when Detached (M3). Test-rig glue models the chip reset by
clearing the PCI_COMMAND parity marker when the interface-timer-interrupt fires,
so POST_RESET's marker check succeeds.

## DD-10: M5 telemetry / power / locks design

**LOCK_CTL blocking acquire → manual WDFQUEUE (the novel mechanic).** 64 device
locks: a device 64-bit "held" bitmap + a per-file "held-by-me" bitmap, guarded
by a device WDFWAITLOCK (invariant: global bit set whenever any local bit set,
chardev.c:369-370). ACQUIRE/RELEASE/TEST are immediate bit ops. ACQUIRE_BLOCKING
that can't win is forwarded to a manual queue (WdfRequestForwardToIoQueue) — WDF
makes it cancellable and auto-completes it STATUS_CANCELLED on CancelIo / handle
close (replacing Linux -ERESTARTSYS). Wakers (RELEASE, file cleanup, RESET
gen-bump, surprise removal) drain the wait queue under the device lock: each
waiter is retried; winners complete value=1, stale-gen/detached waiters complete
STATUS_DEVICE_REMOVED (Linux -ENODEV), the rest are re-forwarded. Locks survive
reset and are released only at handle close (chardev.c:312-316 parity). This
avoids Linux's "drop reset_rwsem during the wait" hazard entirely: the request
pends outside any lock, so a blocked waiter never holds up reset/removal.

**SET_POWER_STATE multi-client aggregation.** Each file object stores its
`tenstorrent_power_state` contribution. Any change — plus file create (legacy
default: all flags on except MAX_AI_CLK) and close — re-aggregates across every
live file object (the M4 device FileList): flags OR-ed with the "unspecified
flags default ON" rule (`unspecified = ~((1<<flags_count)-1) & 0x7FFF`,
chardev.c:29,509 — old clients can't disable features they don't know),
settings by max, stale-gen fds skipped. The result is sent to firmware as an ARC
POWER_SETTING (0x21) message. The O_APPEND power-aware-client signal maps to a
SET_CLIENT_FLAGS-style opt-in: default create is legacy; the aggregated state is
also read-back via a debug ioctl for test verification (firmware doesn't echo it).

**Telemetry (hwmon-equivalent) query.** Windows has no hwmon; a Windows-extension
IOCTL_TENSTORRENT_QUERY_TELEMETRY returns a struct of the scaled sensor values
with a "present" bitmask (absent tags hidden, hwmon is_visible parity). Field
scaling is byte-identical to the Linux hwmon ABI so tooling ports mechanically:
temp m°C (tag 11 16.16-fixed → *1000/65536; tag 56 *1000), voltage mV (tags 6,
9-upper-16 as-is), current mA (tags 8, 55 *1000), power µW (tags 7, 64
*1000000), fan RPM (tag 41), and the tt_* sysfs values (aiclk/axiclk/arcclk MHz
tags 14-16, tt_heartbeat 32, tt_therm_trip_count 60, tt_asic_id u64 61:62,
tt_serial u64 1:2, tt_fw_bundle_ver 28). The exact Linux names are documented per
field in the header. The BH/WH hwmon label difference (asic_temp vs asic1_temp)
is a naming-only concern deferred until a WH device exists.

## DD-11: RESET_PCIE_LINK via PlatformLevelDeviceReset (M7, resolves OQ-5 item 1)

**Decision.** `TENSTORRENT_RESET_DEVICE_RESET_PCIE_LINK` now performs a real
reset through the pci.sys device-reset interface
(`GUID_DEVICE_RESET_INTERFACE_STANDARD`, queried at PrepareHardware with
`WdfFdoQueryForInterface`, version 1). Only `PlatformLevelDeviceReset` is used —
the platform analog of the Linux upstream-bridge secondary-bus reset
(`pcie_hot_reset_and_restore_state`, pcie.c:61-90). If the platform does not
support PLDR for this device (`SupportedResetTypes` bit clear) the flavor
returns `out.result=1`: an honest failure, never the old present-check.

**FLR is deliberately not a fallback.** Linux never validated FLR on this ASIC
(tt-kmd contains no `pcie_flr`); an FLR that resets the PCIe function but not
the NOC/ASIC would be a success-shaped lie.

**Execution model.** PLDR surprise-removes and re-enumerates this device stack.
The reset therefore fires from a WDF work item after the ioctl completes
(`TtPldrWorkItem`, guarded by a `PldrQueued` interlock): invoking it inline
could deadlock the removal against the in-flight ioctl. `out.result=0` means
"reset initiated"; success is observed as device-interface departure and
re-arrival, after which a fresh PrepareHardware re-runs the full init.

**Semantic delta vs Linux (accepted).** On Linux, RESET_PCIE_LINK keeps fds
valid (chardev.c:247-249, no gen bump). On Windows the stack teardown
invalidates every handle. The reference tooling reopens by BDF/interface after
a link reset (analysis §11), so this is compatible in practice; it is a
documented divergence, not an accident.

## DD-12: Silicon-truth fixes — config restore, MPS/MRRS, ARC message content (M7, resolves OQ-5 item 2)

Everything in this DD was masked by the ttsim rig (fake reset preserved BARs;
the ARC stub accepted any message and answered header 0 instantly; QEMU DMA'd
regardless of BME or IOMMU state).

- **Config snapshot/restore (pci_save_state parity).** `TtPciSaveState`
  (reset.c) snapshots the 16 standard-header dwords plus PCIe-cap
  DevCtl/LnkCtl at PrepareHardware — after init_hardware, so the snapshot
  carries MRRS (enumerate.c:370-372 ordering) — and on resume/after restore.
  `TtSafeRestoreState` now performs the real ordered restore: vendor-ID test
  read first (never write a wedged link, pcie.c:52-54), then BARs → ROM →
  cacheline → int-line → PCIe DevCtl/LnkCtl → **Command last** (Memory-Space /
  Bus-Master re-enable only after BARs decode). MSI state is skipped by
  design: the driver owns no interrupt objects (Blackhole is polling-only) and
  MSI config belongs to pci.sys.
- **DBI MPS snapshot (blackhole_save/restore_reset_state, blackhole.c:304-330).**
  `TtBhSaveResetState`/`TtBhRestoreResetState` read/RMW the chip's own
  config-space view (outbound NOC TLB 62 at 0xF8...0 + 0x78) to preserve the
  negotiated Max Payload Size across chip resets; wired into RESTORE_STATE and
  POST_RESET between config restore and init_hardware (chardev.c:242,277).
  **MPS is platform-negotiated, NOT the device max — snapshot, never assume.**
  Silicon ground truth (`real-silicon-linux/`): host DevCtl @0x78 = 0x515f →
  MPS **512 B** (encoding 2) even though DevCap advertises 1024; MRRS 4096
  (encoding 5). The snapshot approach captures whatever the root complex
  negotiated. The full 4 KiB config reset delta confirms all architected
  config is byte-identical across a real reset and MPS/MRRS survive *only*
  because the driver restores them — exactly this save→marker→reset→
  poll-marker-zero→restore sequence.
- **MRRS.** `TtBhInitHardware` now sets PCIe DevCtl MRRS to 4096 (encoding 5)
  first, matching `pcie_set_readrq(pdev, MAX_MRRS)` (blackhole.c:626).
- **ARC watchdog.** SET_WDT_TIMEOUT payload is now `1000 * 10` ms, matching
  the Linux `auto_reset_timeout` default (module.c:48, blackhole.c:634). The
  old payload of 0 disarmed (or worse, zero-perioded) the ARC watchdog.
- **POWER_SETTING wire format.** Header now packs
  `type | validity<<8 | flags<<16` with all 14 settings in payload[0..6]
  (blackhole.c:802-804, chardev.c:531). The previous layout put validity/flags
  in payload[0] and dropped 12 settings — real firmware parses these fields on
  every open/close.
- **init_hardware at probe + initial power state.** PrepareHardware now runs
  `TtBhInitHardware` (ASIC_STATE0 + WDT) and the initial power aggregation,
  matching enumerate.c:370,388. Previously the ASIC was never brought to A0
  until the first reset ioctl.
- **ASIC_STATE3 on power-down.** New D0Exit callback sends A3
  (cleanup_hardware parity, blackhole.c:702-707), skipped when Detached;
  D0Entry re-runs init_hardware + re-saves config on resume
  (enumerate.c:499-522). Neither path bumps ResetGen — handles survive
  suspend, like Linux.
- **Bus Master Enable.** PrepareHardware verifies/sets Command bit 2
  (pci_set_master parity, enumerate.c:352).
- **PIN_PAGES identity-domain guard.** PrepareHardware probes whether the DMA
  domain is identity-mapped (common-buffer logical address == CPU physical,
  within the 58-bit engine reach). In a translated domain PIN_PAGES returns
  STATUS_NOT_SUPPORTED — enforcing what DD-8 documented but never checked;
  raw PFNs must never be handed to the iATU under translation.
- **58-bit reach.** ALLOCATE_DMA_BUF rejects common buffers whose logical
  address exceeds 2^58-1 (`dma_set_coherent_mask(58)` parity).
- **Telemetry post-reset gate.** In the needs_hw_init window QUERY_TELEMETRY
  returns STATUS_DEVICE_NOT_READY (Linux -ENODATA, telemetry.c:23-24) instead
  of DEVICE_REMOVED, so pollers see a transient (analysis §10 porting note).

## DD-13: Windows sysmem is carved from driver DMA buffers and pinned bottom-up (tt-umd backend)

**Context.** tt-umd backs sysmem with hugetlbfs pages when no IOMMU is active,
or with one anonymous mapping pinned through the IOMMU otherwise, and tt-metal
hard-codes the NOC address of channel 0 as `pcie_base`. Windows has no
hugetlbfs, user allocations are not physically contiguous, and this driver does
not hand raw PFNs to the iATU inside a DMA-remapped domain.

**Decision.** The Windows backend of tt-kmd-lib allocates each channel with
`ALLOCATE_DMA_BUF` (index 1 upwards; index 0 remains the PCIe DMA engine
buffer), maps it with `MAP`, and pins the view with
`PIN_PAGES(CONTIGUOUS | NOC_DMA)`. Because `PIN_PAGES` allocates the NOC
aperture bottom-up while `ALLOCATE_DMA_BUF`'s own NOC option is top-down, this
is the only sequence that puts channel 0 at `pcie_base` without a driver
change. The channel size is negotiated downwards from `TT_MAX_DMA_BUF_SIZE`
(256 MiB) to 16 MiB; below 1 GiB only one channel is used.

**Consequences.** `PIN_PAGES` must keep accepting user VAs that are `MAP`
views of common buffers (silicon rung e). A boot-time 1 GiB reservation and a
larger `TT_MAX_DMA_BUF_SIZE` are the path to Linux-equivalent capacity (OQ-10).
`FREE_DMA_BUF` stays unimplemented because tt-umd releases buffers by closing
the handle (OQ-9).

## DD-14: Reset flavors 2 and 4 are refused on physical hardware

**Context.** The silicon ladder (docs/test-reports/real-silicon.md, D4) showed
that the DWC interface-timer trigger behind `CONFIG_WRITE` (2) and `ASIC_RESET`
(4) takes a p150a's link down until a cold power cycle, later followed by a
WHEA 0x124 on the host. OQ-7 asks upstream whether these flavors are meant to
work on Blackhole at all; no answer changes the observed outcome.

**Decision.** `TtIoctlResetDevice` refuses flavors 2 and 4 with `out.result = 1`
and a `ResetFlagRefusedOnSilicon` trace event unless the device reports an
all-zero PCI subsystem identity, which only the ttsim/QEMU model does. The
lifecycle tests on the rig keep exercising the code path; a real card cannot
be wedged from user mode.

**Consequences.** tt-umd's warm-reset helper, which issues `CONFIG_WRITE` on
Linux, reports failure on Windows silicon instead of hanging the machine.
`ASIC_DMC_RESET` (5), `USER_RESET` (3), `RESTORE_STATE` (0) and `POST_RESET`
(6) are unchanged.

