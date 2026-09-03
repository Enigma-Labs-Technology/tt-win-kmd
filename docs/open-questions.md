# Open Questions

Ambiguities found during the port, each with evidence, ranked hypotheses, and the
symbol behind which the answer is isolated. Resolved items move to the bottom with
their resolution.

Format: **OQ-N** — question / evidence / hypotheses (ranked) / isolation / status.

---

## OQ-1: ttsim BAR sizing for the QEMU glue device

**Question:** ttsim does not implement config-space writes, so the standard
write-ones-and-read-back BAR sizing protocol cannot be used against it. What BAR
sizes must the QEMU glue advertise to the guest?

**Evidence:** `ttsim/docs/libttsim_api.md` ("config-space writes are not currently
implemented and calling it is a fatal error"); BAR base registers are readable.

**Hypotheses (ranked):**
1. Use the Blackhole BAR sizes from tt-kmd's analysis (BAR0/BAR2/BAR4 sizes are
   compile-time constants in `tt-kmd/blackhole.c`) — the guest driver only needs
   sizes consistent with what tt-kmd expects. **(adopted, pending analysis values)**
2. Derive sizes from spacing between consecutive ttsim BAR bases — fragile, rejected.

**Isolation:** `TTSIM_GLUE_BAR_SIZES[]` table in `test/qemu-ttsim/ttsim-dev.c`.

**Status:** Partially resolved by analysis §07: tt-kmd requires BAR0 ≥ `0x1FE00000`
(driver maps three sub-ranges; real device ≈512 MB), BAR2 = DWC PCIe controller
register space (driver maps in full; only iATU block at +0x1000 used), BAR4 =
N × 4 GiB TLB windows with the driver clamping N to the actual BAR length
(`tlb_counts[1] = bar4_len / 4GB`, so the glue may advertise as little as one
4 GiB window). Exact sizes to be cross-checked against ttsim's own BAR layout in
`ttsim/src/config.h` when the glue is built (M1).

## OQ-2: Default ACL for the device interface

**Question:** Linux exposes `/dev/tenstorrent/N` as mode 0666
(`tt-kmd/udev-50-tenstorrent.rules`). Should the Windows device object allow
world-writable access by default?

**Evidence:** udev rule in upstream; Windows convention is more restrictive defaults.

**Hypotheses (ranked):**
1. Default Windows ACL (Administrators + SYSTEM full, interactive users read/write via
   `D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)`), documented delta from Linux. **(adopted)**
2. World-accessible to mirror 0666 — rejected: violates Windows least-privilege norms
   and the spec's security constraint; tooling runs as interactive user anyway.

**Isolation:** `TTKMD_DEVICE_SDDL` in `src/driver/device.c` (single SDDL string).

**Status:** Adopted hypothesis 1; revisit if tt-umd-on-Windows needs service-context
access.

## OQ-3: M0 load test runs in the VM, not on the host

**Question:** M0 acceptance requires the empty driver to install/start against a
soft device. Doing this on the development host requires enabling test signing
(`bcdedit /set testsigning on`) — a persistent, security-relevant host change.

**Decision:** All driver installation happens in the QEMU/KVM Windows 11 guest
(see `test/README.md`). The host is never put into test-signing mode. If VM bring-up fails
irrecoverably, enabling test signing on the host is an explicit user decision, never
a default.

**Status:** Adopted; test rig is task #8.

## OQ-4: ttsim models the BH ARC msgqueue natively (M2 feasibility) — RESOLVED

**Question:** Does M2's acceptance (heartbeat advancing, ARC round-trip on ttsim)
require loading management-firmware blobs into the simulator?

**Answer (2026-07-07):** No. ttsim emulates CMFW-visible state in C++ for
Blackhole (`ttsim/src/tile.cpp:266-295`): telemetry table at CSM+0x100 / values
at +0x200, ARC message QCB at CSM+0x300 and queues at +0x400 (4 queues × 8
entries × 32-byte entries, 32-byte header — matching the protocol in analysis
§09), board ID P150 (primary target p150a). M2 tests run against stock ttsim.

**Access-path note (corrected):** the fixed BAR0 CSM aperture
(`ARC_BAR0_CSM_BASE 0x1FE80000`, libttsim.cpp:37) is **Wormhole-only**
(`TT_ARCH_VERSION == 0` block). On Blackhole the driver reaches ARC CSM through
NOC TLB windows — which is exactly why `blackhole_init` claims kernel TLB
window 201 and maps `tlb_regs` (blackhole.c:587-588). M2 therefore requires
kernel-TLB window programming (config registers at BAR0+0x1FC00000) as its
first driver deliverable, before any msgqueue traffic.

## OQ-5: Silicon reset refinements deferred from M4 — IMPLEMENTED (M7), silicon verification pending

Two reset mechanisms were stubbed for the ttsim rig. Both are now implemented
(2026-07-07, first-silicon bring-up):

1. **Secondary-bus reset (RESET_PCIE_LINK) — implemented as PLDR.** The flavor
   now requests `PlatformLevelDeviceReset` through
   `GUID_DEVICE_RESET_INTERFACE_STANDARD`, fired from a work item because PLDR
   surprise-removes this device stack; unsupported platforms get an honest
   `result=1`. FLR is deliberately not used (never validated on this ASIC).
   Full rationale and the fds-do-not-survive delta: DD-11.

2. **Config save/restore + Max-Payload-Size — implemented.**
   `TtPciSaveState`/`TtSafeRestoreState` (reset.c) now perform the real
   pci_save_state/restore via BUS_INTERFACE_STANDARD (snapshot at
   PrepareHardware after init_hardware; ordered restore, Command last, vendor
   test-read guard), and `TtBhSaveResetState`/`TtBhRestoreResetState`
   (blackhole.c) snapshot/restore the DBI-view MPS (blackhole.c:304-330
   parity). Wired into RESTORE_STATE and POST_RESET. Details: DD-12.

**Remaining to close:** verification on real p150a silicon — MPS survives
ASIC_RESET→POST_RESET, BARs decode after restore, PLDR re-enumeration
round-trip — per the real-silicon test ladder (rungs f-i). Neither change
affects the M4-tested rig behaviors (fd invalidation, mapping zap,
reset-window semantics).

## OQ-6: Windows platform DPC during a driver-initiated reset — OPEN (silicon-only)

**Question:** On this AMD host, a driver-initiated ASIC/DMC reset drops the
PCIe link (surprise-down), which the upstream root port's Downstream Port
Containment (DPC) treats as an uncorrectable fatal error. On Linux this is
benign; what does Windows do, and does the KMDF port survive it?

**Evidence (Linux ground truth, `real-silicon-linux/`):** `dmesg-reset.txt`
shows, at the instant of `tt-smi -r 0`: `pcieport 0000:c0:01.1: DPC:
containment event ... Uncorrectable (Fatal) ... [5] SDES (First)` and
`tenstorrent 0000:c1:00.0: AER: can't recover (no error_detected callback)` /
`AER: device recovery failed`. Linux survives only because tt-kmd registers NO
AER `error_detected` callback and performs an **in-place** reset — the device
never leaves the bus and keeps working (post-reset `ttkmd_test` exit 0). The
reset is otherwise clean: ~2.75 s wall, ~1.64 s telemetry outage, heartbeat
restarts from ~0, all architected config restored by the driver.

**Hypotheses (ranked):**
1. Windows PCI DPC (pci.sys / the DPC driver on the AMD root port) contains the
   surprise-down by disabling the downstream port and **surprise-removing** our
   device, then re-enumerating when the link returns — so ASIC_DMC_RESET on
   Windows behaves like a PLDR (DD-11): stack teardown + fresh PrepareHardware,
   NOT Linux's in-place reset. Our existing surprise-removal machinery
   (`Detached`, mapping zap, ResetGen) and re-enum re-init should absorb it;
   the POST_RESET from the original handle correctly returns DEVICE_REMOVED.
   **(most likely)**
2. The platform masks/holds DPC (firmware policy) and the link recovers
   in-place as on Linux — POST_RESET completes normally.
3. DPC fires while the reset thread holds the reset ERESOURCE and is mid-MMIO
   (ARC NOC reads), racing ReleaseHardware's BAR unmap — a potential
   use-after-unmap / bugcheck. This is the failure mode Driver Verifier + the
   gated hardware run must exercise.

**Isolation:** `TtBhReset` / the RESET_DEVICE dispatch in `reset.c`;
surprise-removal in `TtEvtDeviceSurpriseRemoval`. Possible future mitigations
(only if hypothesis 3 materializes): mask SDES/DPC around the initiated reset,
or route ASIC resets through the pci.sys bus-specific reset that coordinates
with the port — both larger changes, deferred until hardware shows they are
needed.

**Status:** OPEN — no code change made blind. The reset ladder runs rung h
under Verifier with the tt-flash/Linux recovery path armed; the report's
divergence log (D1) tracks the observed Windows behavior, and this OQ resolves
(or a precise fix is filed) from that run. Do NOT assume Linux's in-place
semantics on Windows.

## OQ-7: CONFIG_WRITE/ASIC_RESET interface-timer trigger wedges real Blackhole — upstream question

**Question:** `pcie_timer_interrupt` (config writes 0x934=0x1, 0x930=0x11 —
the trigger behind RESET_DEVICE flavors CONFIG_WRITE(2) and, with the marker,
ASIC_RESET(4)) hard-wedged a real p150a on first use: link down, no autonomous
retrain, endpoint absent from the bus until PERST# (cold power cycle). Is this
flavor expected to work on Blackhole silicon, or is it Wormhole-era/lab-only?

**Evidence:** M7 ladder rung g (`real-silicon.md` D4): trigger delivered
(result=0), chip reset (telemetry restarted), then link permanently down;
restore correctly refused (vendor read FFFF); parent-port restart could not
revive it; warm reboot recovered enumeration but the controller stayed
marginal and later took the host down (D6, 0x124). By contrast the
DMC-coordinated flavor (5) resets and retrains cleanly (rung h). The Linux
ground-truth capture never exercised flavors 2/4 (tt-smi uses the DMC path),
and tt-kmd's own hardware tests don't cover them on BH.

**Hypotheses (ranked):**
1. The interface-timer reset hard-resets the DWC PCIe controller without
   firmware coordinating link re-training — fine on the rig (fake reset),
   lethal on silicon unless something re-runs the PCIe init sequence.
   Firmware 19.6.0.0 does not do so autonomously. **(most likely)**
2. Host/root-port specific: the AMD port gives up after surprise-down without
   DPC recovery; other platforms might retrain.

**Isolation:** `TtPcieTimerInterrupt` (reset.c) / `TtBhTimerInterrupt`
(blackhole.c). No code change made — parity with Linux is preserved; the
flavors are operationally contraindicated on real BH (ladder + matrix note).

**Status:** OPEN upstream; mitigated in the driver by DD-14 (flavors 2/4 are
refused on physical devices and kept only for the ttsim/QEMU model).

## OQ-8: SMC_MSG (tt-kmd 2.11.0) is stubbed — OPEN

Upstream 2.11.0 added `TENSTORRENT_IOCTL_SMC_MSG`: a per-fd asynchronous ARC
message with POST/POLL/ABANDON, multiplexed by a device-wide pump
(`msgqueue.c arc_msg_pump`, per-device `arc_msg_mutex`). The Windows driver
carries the ABI (`ttkmd_ioctl.h`, static-asserted) and returns
`STATUS_NOT_SUPPORTED`, the mapping of the `-EOPNOTSUPP` upstream reports for
a device without a usable queue. The checked-out tt-umd does not issue it.
Porting it means: per-file message state, a device queue, and a pump that also
flushes in-flight user messages before the driver's own synchronous messages
(`TtBhSendArcMessage`). Kernel-side work that must be built and exercised on
the rig before it ships.

## OQ-9: FREE_DMA_BUF, MAP_PEER_BAR and EXPORT_TLB_DMABUF — RESOLVED for the Windows use, upstream gap noted

Upstream 2.11.0 dispatches `FREE_DMA_BUF` but `ioctl_free_dma_buf` still
returns -EINVAL and the request's in/out structures are empty, so it cannot
name a slot; Windows answers the same -EINVAL (exact parity). Early release on
Windows is provided by the private `FREE_DMA_BUF_EX` (DD-16), which tt-kmd-lib
uses for sysmem buffers. `MAP_PEER_BAR` needs a second device and a
Windows-native peer identity; `EXPORT_TLB_DMABUF` has no Windows equivalent.
Both stay unimplemented until a consumer appears.

## OQ-10: Sysmem capacity on Windows is capped at 256 MiB per channel — OPEN

`TT_MAX_DMA_BUF_SIZE` (2^28) bounds what tt-umd can obtain per channel, and a
contiguous 1 GiB common buffer is unlikely to be available on a booted host
even if the cap were lifted. Options: (a) a boot-start reservation of N x 1 GiB
common buffers handed out to the first opener (the Linux hugepage analogue);
(b) a registry parameter for the reservation size; (c) keep 256 MiB and let
tt-metal size its command queues from the reported channel size. (c) is what
ships now, with DD-17's per-device ceiling bounding the total. Measure whether
tt-metal workloads on a p150a are limited by it before doing (a).

