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

## OQ-5: Silicon reset refinements deferred from M4

Two reset mechanisms are stubbed for the ttsim rig and must be completed for
real p150a bring-up (isolated so they are single-function swaps):

1. **Secondary-bus reset (RESET_PCIE_LINK).** A KMDF function driver cannot touch
   the upstream bridge's config space (pcie.c:61-90 assert/deassert SBR). On the
   rig this flavor zaps mappings and returns device-present; on silicon it must
   request a bus/PLDR reset via `GUID_DEVICE_RESET_INTERFACE_STANDARD`
   (`DeviceReset`). Isolated in `TtIoctlResetDevice` case
   TENSTORRENT_RESET_DEVICE_RESET_PCIE_LINK.

2. **Config save/restore + Max-Payload-Size.** `safe_pci_restore_state` currently
   reduces to a vendor-ID present check (the rig's fake reset preserves BARs).
   Silicon needs the pci_save_state/restore snapshot (via BUS_INTERFACE_STANDARD
   Get/SetBusData at PrepareHardware / after reset) and the DBI-view MPS
   snapshot (wormhole.c:968-988 / blackhole.c:304-330). Isolated in
   `TtSafeRestoreState` (reset.c) and the (currently absent) save_reset_state
   hook.

Neither affects the M4-tested behaviors (fd invalidation, mapping zap,
reset-window semantics, no-crash under storm/hotplug).
