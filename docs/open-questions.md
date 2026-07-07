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
§09), board ID P150 (primary target p150a). The BAR0 CSM window
(0x1FE80000-0x1FEFFFFF) routes to this memory. M2 tests run against stock ttsim.
