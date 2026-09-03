// SPDX-FileCopyrightText: 2026 tt-win-kmd contributors
// SPDX-License-Identifier: GPL-2.0-only
//
// Maps to: tt-kmd/chardev.c ioctl_reset_device (chardev.c:200-310) +
// tt-kmd/pcie.c reset primitives + blackhole.c blackhole_reset/init_hardware.
// The seven reset flavors, fd-generation invalidation, the needs_hw_init reset
// window, and mapping teardown (VMA-zap equivalent). See DD-9.
#include "ttkmd.h"

#include <stddef.h>
#include "ttkmd_ioctl.h"
#include "blackhole.h"

// KeStackAttachProcess/KeUnstackDetachProcess and KAPC_STATE live in ntifs.h,
// which conflicts with the ntddk.h WDF already includes. Declare the functions
// with an opaque APC-state pointer; callers pass a suitably-sized aligned
// buffer (KAPC_STATE is 48 bytes on x64). Used to unmap a mapping from a
// foreign process on reset (DD-9).
NTKERNELAPI VOID KeStackAttachProcess(_Inout_ PEPROCESS Process,
                                      _Out_ PVOID ApcState);
NTKERNELAPI VOID KeUnstackDetachProcess(_In_ PVOID ApcState);
#define TT_KAPC_STATE_QWORDS 8   // >= sizeof(KAPC_STATE), 8-byte aligned

// PCI config offsets (uapi/linux/pci_regs.h + tt-kmd/pcie.c)
#define TT_PCI_COMMAND 0x04
#define TT_PCI_COMMAND_PARITY 0x40         // reset marker bit (pcie.c:140-158)
#define TT_INTERFACE_TIMER_CONTROL_OFF 0x930
#define TT_INTERFACE_TIMER_TARGET_OFF 0x934
#define TT_INTERFACE_TIMER_TARGET 0x1
#define TT_INTERFACE_TIMER_EN 0x1
#define TT_INTERFACE_FORCE_PENDING 0x10

// ---------------------------------------------------------------------------
// Reset resource (DD-9): reset_rwsem parity.
// ---------------------------------------------------------------------------

_Use_decl_annotations_
VOID
TtResetAcquireShared(PTT_DEVICE_CONTEXT Context)
{
    KeEnterCriticalRegion();
    ExAcquireResourceSharedLite(&Context->ResetResource, TRUE);
}

_Use_decl_annotations_
VOID
TtResetAcquireExclusive(PTT_DEVICE_CONTEXT Context)
{
    KeEnterCriticalRegion();
    ExAcquireResourceExclusiveLite(&Context->ResetResource, TRUE);
}

_Use_decl_annotations_
VOID
TtResetRelease(PTT_DEVICE_CONTEXT Context)
{
    // Acquire/release are deliberately split across wrapper functions (the lock
    // is held by the caller across a handler); PREfast cannot track that.
#pragma warning(suppress: 26110)
    ExReleaseResourceLite(&Context->ResetResource);
    KeLeaveCriticalRegion();
}

// ---------------------------------------------------------------------------
// Config-space primitives (tt-kmd/pcie.c) via BUS_INTERFACE_STANDARD.
// ---------------------------------------------------------------------------

_Use_decl_annotations_
BOOLEAN
TtCfgReadWord(PTT_DEVICE_CONTEXT Context, ULONG Offset, USHORT *Value)
{
    *Value = 0xFFFF;
    if (!Context->BusInterfaceValid) {
        return FALSE;
    }
    return Context->BusInterface.GetBusData(Context->BusInterface.Context,
                                            PCI_WHICHSPACE_CONFIG, Value,
                                            Offset, sizeof(*Value)) == sizeof(*Value);
}

static BOOLEAN
TtCfgReadDword(
    _In_ PTT_DEVICE_CONTEXT Context,
    _In_ ULONG Offset,
    _Out_ UINT32 *Value
    )
{
    *Value = 0xFFFFFFFF;
    if (!Context->BusInterfaceValid) {
        return FALSE;
    }
    return Context->BusInterface.GetBusData(Context->BusInterface.Context,
                                            PCI_WHICHSPACE_CONFIG, Value,
                                            Offset, sizeof(*Value)) == sizeof(*Value);
}

_Use_decl_annotations_
VOID
TtCfgWriteWord(PTT_DEVICE_CONTEXT Context, ULONG Offset, USHORT Value)
{
    if (Context->BusInterfaceValid) {
        Context->BusInterface.SetBusData(Context->BusInterface.Context,
                                         PCI_WHICHSPACE_CONFIG, &Value,
                                         Offset, sizeof(Value));
    }
}

_Use_decl_annotations_
VOID
TtCfgWriteDword(PTT_DEVICE_CONTEXT Context, ULONG Offset, ULONG Value)
{
    if (Context->BusInterfaceValid) {
        Context->BusInterface.SetBusData(Context->BusInterface.Context,
                                         PCI_WHICHSPACE_CONFIG, &Value,
                                         Offset, sizeof(Value));
    }
}

// set_reset_marker (pcie.c:140-149): set PCI_COMMAND parity bit.
static VOID
TtSetResetMarker(
    _In_ PTT_DEVICE_CONTEXT Context
    )
{
    USHORT command;

    if (TtCfgReadWord(Context, TT_PCI_COMMAND, &command)) {
        TtCfgWriteWord(Context, TT_PCI_COMMAND,
                       (USHORT)(command | TT_PCI_COMMAND_PARITY));
    }
}

// is_reset_marker_zero (pcie.c:151-158): parity bit cleared proves the chip's
// config space was reset to defaults.
static BOOLEAN
TtIsResetMarkerZero(
    _In_ PTT_DEVICE_CONTEXT Context
    )
{
    USHORT command;

    if (!TtCfgReadWord(Context, TT_PCI_COMMAND, &command)) {
        return FALSE;
    }
    return (command & TT_PCI_COMMAND_PARITY) == 0;
}

// pcie_timer_interrupt (pcie.c:133-138): two config dword writes into the DWC
// controller's interface-timer registers, forcing a chip-internal reset that
// firmware services. Always "succeeds".
static BOOLEAN
TtPcieTimerInterrupt(
    _In_ PTT_DEVICE_CONTEXT Context
    )
{
    TtCfgWriteDword(Context, TT_INTERFACE_TIMER_TARGET_OFF, TT_INTERFACE_TIMER_TARGET);
    TtCfgWriteDword(Context, TT_INTERFACE_TIMER_CONTROL_OFF,
                    TT_INTERFACE_TIMER_EN | TT_INTERFACE_FORCE_PENDING);
    return TRUE;
}

// Locate the PCI Express capability (ID 0x10) and cache its offset. The walk
// is bounded (48 steps covers the 256-byte legacy space) for the same reason
// Linux bounds its cap iteration: misbehaving hardware returns all-ones.
// Split from TtPciSaveState so PrepareHardware can discover the offset BEFORE
// the first TtBhInitHardware — whose MRRS write needs it (Linux uses the
// kernel-discovered pdev->pcie_cap, present from probe).
_Use_decl_annotations_
VOID
TtDiscoverPcieCap(
    PTT_DEVICE_CONTEXT Context
    )
{
    USHORT statusReg = 0;
    UINT32 capPtrDword = 0;
    ULONG offset, steps;

    Context->PcieCapOffset = 0;
    if (!Context->BusInterfaceValid) {
        return;
    }
    if (!TtCfgReadWord(Context, 0x06, &statusReg) || statusReg == 0xFFFF ||
        (statusReg & 0x0010) == 0) {
        return;   // no capabilities list
    }
    if (!TtCfgReadDword(Context, 0x34, &capPtrDword)) {
        return;
    }
    offset = capPtrDword & 0xFC;
    for (steps = 0; steps < 48 && offset >= 0x40; steps++) {
        USHORT capHeader;

        if (!TtCfgReadWord(Context, offset, &capHeader) || capHeader == 0xFFFF) {
            break;
        }
        if ((capHeader & 0xFF) == 0x10) {   // PCI_CAP_ID_EXP
            Context->PcieCapOffset = offset;
            break;
        }
        offset = ((ULONG)capHeader >> 8) & 0xFC;
    }
}

// pci_save_state parity (enumerate.c:372, pcie.c:57): snapshot the standard
// header dwords plus the PCIe capability's Device/Link Control. MSI state is
// deliberately skipped — the driver owns no WdfInterrupt (Blackhole is
// polling-only) and MSI config belongs to pci.sys. Called at PrepareHardware
// (after init_hardware, so the snapshot carries MRRS=4096), on resume, and to
// re-save after each successful restore. Resolves OQ-5 item 2 (DD-12).
_Use_decl_annotations_
VOID
TtPciSaveState(
    PTT_DEVICE_CONTEXT Context
    )
{
    ULONG i;

    Context->SavedStateValid = FALSE;

    if (!Context->BusInterfaceValid) {
        return;
    }

    for (i = 0; i < ARRAYSIZE(Context->SavedHeaderDword); i++) {
        if (!TtCfgReadDword(Context, i * 4, &Context->SavedHeaderDword[i])) {
            return;
        }
    }

    TtDiscoverPcieCap(Context);

    if (Context->PcieCapOffset != 0) {
        if (!TtCfgReadWord(Context, Context->PcieCapOffset + 0x08,
                           &Context->SavedPcieDevCtl) ||
            !TtCfgReadWord(Context, Context->PcieCapOffset + 0x10,
                           &Context->SavedPcieLnkCtl)) {
            return;
        }
    }

    Context->SavedStateValid = TRUE;
    TraceLoggingWrite(g_TtTraceProvider, "PciStateSaved",
                      TraceLoggingUInt32(Context->PcieCapOffset, "pcieCapOffset"),
                      TraceLoggingUInt16(Context->SavedPcieDevCtl, "pcieDevCtl"));
}

// safe_pci_restore_state (pcie.c:43-59): test-read the vendor ID first and
// never write a wedged link (pcie.c:52-54); require a snapshot (pcie.c:46-47);
// then rewrite the writable header state — BARs and PCIe control first,
// Command LAST so Memory-Space/Bus-Master re-enable only lands once the BARs
// decode again — and re-save like Linux. Resolves OQ-5 item 2 (DD-12).
static BOOLEAN
TtSafeRestoreState(
    _In_ PTT_DEVICE_CONTEXT Context
    )
{
    USHORT vendor;
    ULONG off;

    if (!TtCfgReadWord(Context, 0x00, &vendor) || vendor != TT_PCI_VENDOR_ID) {
        return FALSE;
    }
    if (!Context->SavedStateValid) {
        return FALSE;
    }

    for (off = 0x10; off <= 0x24; off += 4) {   // BAR0-5
        TtCfgWriteDword(Context, off, Context->SavedHeaderDword[off / 4]);
    }
    TtCfgWriteDword(Context, 0x30, Context->SavedHeaderDword[0x30 / 4]);
    TtCfgWriteDword(Context, 0x0C, Context->SavedHeaderDword[0x0C / 4]);
    TtCfgWriteWord(Context, 0x3C, (USHORT)Context->SavedHeaderDword[0x3C / 4]);
    if (Context->PcieCapOffset != 0) {
        TtCfgWriteWord(Context, Context->PcieCapOffset + 0x08,
                       Context->SavedPcieDevCtl);
        TtCfgWriteWord(Context, Context->PcieCapOffset + 0x10,
                       Context->SavedPcieLnkCtl);
    }
    TtCfgWriteWord(Context, TT_PCI_COMMAND,
                   (USHORT)Context->SavedHeaderDword[TT_PCI_COMMAND / 4]);

    TtPciSaveState(Context);
    return TRUE;
}

// RESET_PCIE_LINK via the pci.sys reset interface (OQ-4 -> DD-11). Returns
// TRUE only when PlatformLevelDeviceReset is supported and the work item was
// queued; FALSE is an honest "this driver cannot reset the link". FLR is
// deliberately NOT used as a fallback: Linux never validated FLR on this ASIC
// (tt-kmd has no pcie_flr call) and an FLR that resets only the PCIe function
// while leaving the NOC/ASIC state would be a success-shaped lie.
static BOOLEAN
TtPldrInitiate(
    _In_ PTT_DEVICE_CONTEXT Context
    )
{
    if (!Context->ResetInterfaceValid ||
        Context->ResetInterface.DeviceReset == NULL ||
        (Context->ResetInterface.SupportedResetTypes &
         (1u << PlatformLevelDeviceReset)) == 0 ||
        Context->PldrWorkItem == NULL) {
        TraceLoggingWrite(g_TtTraceProvider, "PldrUnsupported",
                          TraceLoggingBoolean(Context->ResetInterfaceValid, "interfaceValid"),
                          TraceLoggingUInt32(Context->ResetInterfaceValid ?
                                             Context->ResetInterface.SupportedResetTypes : 0,
                                             "supportedResetTypes"));
        return FALSE;
    }
    if (InterlockedExchange(&Context->PldrQueued, 1) != 0) {
        return TRUE;   // a reset is already in flight
    }

    // The work item can outlive ReleaseHardware's dereference of
    // ResetInterface (WDF flushes device work items at object cleanup, AFTER
    // ReleaseHardware) — so it owns a snapshot holding its own extra
    // reference, taken here while the interface is guaranteed live (this
    // ioctl blocks removal).
    Context->PldrSnapshot = Context->ResetInterface;
    if (Context->PldrSnapshot.InterfaceReference != NULL) {
        Context->PldrSnapshot.InterfaceReference(Context->PldrSnapshot.Context);
    }

    WdfWorkItemEnqueue(Context->PldrWorkItem);
    return TRUE;
}

// Work-item body: invoke PLDR outside the ioctl path. PLDR surprise-removes
// and re-enumerates this device stack, so it must not run while the reset
// ioctl (or its ERESOURCE) is outstanding; success is observed by the caller
// as interface departure + arrival, not by this ioctl's result. Windows delta
// vs Linux (DD-11): pre-reset handles do not survive RESET_PCIE_LINK.
_Use_decl_annotations_
VOID
TtPldrWorkItem(
    WDFWORKITEM WorkItem
    )
{
    PTT_DEVICE_CONTEXT context =
        TtGetDeviceContext(WdfWorkItemGetParentObject(WorkItem));
    // Local copy of the enqueue-time snapshot: never the live ResetInterface,
    // which ReleaseHardware dereferences/invalidates concurrently during the
    // PLDR-triggered removal.
    DEVICE_RESET_INTERFACE_STANDARD snapshot = context->PldrSnapshot;
    NTSTATUS status;

    status = snapshot.DeviceReset(snapshot.Context,
                                  PlatformLevelDeviceReset, 0, NULL);
    TraceLoggingWrite(g_TtTraceProvider, "PldrInvoked",
                      TraceLoggingNTStatus(status, "status"));

    // Drop the snapshot's extra reference (the device context memory itself
    // stays valid here: WDF holds it until work-item rundown at cleanup).
    if (snapshot.InterfaceDereference != NULL) {
        snapshot.InterfaceDereference(snapshot.Context);
    }
    if (!NT_SUCCESS(status)) {
        // Allow a retry only if the platform refused; on success the stack
        // is torn down and the context dies with it.
        InterlockedExchange(&context->PldrQueued, 0);
    }
}

// ---------------------------------------------------------------------------
// Mapping teardown (tenstorrent_vma_zap parity, memory.c:1677-1742).
// ---------------------------------------------------------------------------

// Unmaps every user BAR/TLB mapping across all open handles. DMA-buffer maps
// are left alone (host RAM; Linux does not zap them). Caller holds the reset
// resource EXCLUSIVE, so the file list and per-file mapping lists are stable.
_Use_decl_annotations_
VOID
TtResetZapMappings(
    PTT_DEVICE_CONTEXT Context
    )
{
    PLIST_ENTRY fileEntry;
    ULONG zapped = 0;

    WdfWaitLockAcquire(Context->FileListLock, NULL);
    for (fileEntry = Context->FileList.Flink; fileEntry != &Context->FileList;
         fileEntry = fileEntry->Flink) {
        PTT_FILE_CONTEXT fileContext =
            CONTAINING_RECORD(fileEntry, TT_FILE_CONTEXT, DeviceLink);
        PLIST_ENTRY entry;

        WdfWaitLockAcquire(fileContext->Lock, NULL);
        entry = fileContext->Mappings.Flink;
        while (entry != &fileContext->Mappings) {
            PTT_USER_MAPPING mapping =
                CONTAINING_RECORD(entry, TT_USER_MAPPING, Entry);
            PLIST_ENTRY next = entry->Flink;

            if (!mapping->IsDmaBuf) {
                RemoveEntryList(&mapping->Entry);
                if (mapping->UserVa != NULL && mapping->Mdl != NULL) {
                    if (mapping->Process == PsGetCurrentProcess()) {
                        MmUnmapLockedPages(mapping->UserVa, mapping->Mdl);
                    } else {
                        ULONG64 apc[TT_KAPC_STATE_QWORDS];

                        KeStackAttachProcess(mapping->Process, apc);
                        MmUnmapLockedPages(mapping->UserVa, mapping->Mdl);
                        KeUnstackDetachProcess(apc);
                    }
                }
                if (mapping->Mdl != NULL) {
                    IoFreeMdl(mapping->Mdl);
                }
                if (mapping->Process != NULL) {
                    ObDereferenceObject(mapping->Process);
                }
                ExFreePoolWithTag(mapping, TT_TAG_MAPPING);
                zapped++;
            }
            entry = next;
        }
        WdfWaitLockRelease(fileContext->Lock);
    }
    WdfWaitLockRelease(Context->FileListLock);

    TraceLoggingWrite(g_TtTraceProvider, "ResetZap",
                      TraceLoggingUInt32(zapped, "mappingsZapped"));
}

// bump_reset_gen (chardev.c:195-198): invalidate all other fds, carry the
// resetter's fd forward.
static VOID
TtBumpResetGen(
    _In_ PTT_DEVICE_CONTEXT Context,
    _In_ WDFFILEOBJECT FileObject
    )
{
    LONG64 gen = InterlockedIncrement64(&Context->ResetGen);

    if (FileObject != NULL) {
        TtGetFileContext(FileObject)->OpenResetGen = gen;
    }

    // Wake blocking lock waiters so stale-gen fds fail -ENODEV instead of
    // waiting forever (chardev.c:295-299 parity). Resource locks themselves
    // survive reset (chardev.c:312-316) — only close() clears the bits.
    WdfWaitLockAcquire(Context->LockLock, NULL);
    TtLocksWakeWaiters(Context);
    WdfWaitLockRelease(Context->LockLock);
}

// ---------------------------------------------------------------------------
// RESET_DEVICE handler (chardev.c:200-310). Called with the reset resource
// held EXCLUSIVE by the dispatcher.
// ---------------------------------------------------------------------------


// Reset flavors that wedged a real Blackhole (docs/test-reports/real-silicon.md
// D4: CONFIG_WRITE and ASIC_RESET left the link down until a cold power cycle)
// are refused on physical hardware. The ttsim/QEMU model, which is the only
// device with an all-zero PCI subsystem ID, keeps them for the lifecycle tests.
static BOOLEAN
TtResetLegacyFlagPermitted(
    _In_ PTT_DEVICE_CONTEXT Context,
    _In_ UINT32 Flags
    )
{
    BOOLEAN simulated = (Context->SubsysVendorId == 0 && Context->SubsysId == 0);

    if (Flags != TENSTORRENT_RESET_DEVICE_CONFIG_WRITE &&
        Flags != TENSTORRENT_RESET_DEVICE_ASIC_RESET) {
        return TRUE;
    }
    if (simulated) {
        return TRUE;
    }
    TraceLoggingWrite(g_TtTraceProvider, "ResetFlagRefusedOnSilicon",
                      TraceLoggingUInt32(Flags, "flags"),
                      TraceLoggingUInt16(Context->SubsysId, "subsysId"));
    return FALSE;
}

_Use_decl_annotations_
NTSTATUS
TtIoctlResetDevice(
    PTT_DEVICE_CONTEXT Context,
    WDFFILEOBJECT FileObject,
    WDFREQUEST Request
    )
{
    struct tenstorrent_reset_device_in in;
    struct tenstorrent_reset_device_out out;
    BOOLEAN ok = FALSE;
    NTSTATUS status;

    status = TtCopyInBuffer(Request, &in, sizeof(in));
    if (!NT_SUCCESS(status)) {
        return status;
    }

    // dmabuf -EBUSY gate (chardev.c:222-233): the port does not implement
    // EXPORT_TLB_DMABUF, so no exports ever exist and this never fires.

    switch (in.flags) {
    case TENSTORRENT_RESET_DEVICE_RESTORE_STATE:   // 0 (legacy)
        ok = TtSafeRestoreState(Context);
        if (ok && Context->IsBlackhole) {
            TtBhRestoreResetState(Context);   // MPS (chardev.c:242)
            ok = TtBhInitHardware(Context);
        }
        break;

    case TENSTORRENT_RESET_DEVICE_RESET_PCIE_LINK: // 1 (legacy)
        // pcie_hot_reset_and_restore_state parity via PLDR (DD-11): pci.sys
        // performs the platform-level reset; this stack is surprise-removed
        // and re-enumerated, so the reset fires from a work item after this
        // ioctl completes and re-init happens in the fresh PrepareHardware.
        // No gen bump (Linux keeps fds valid; here the stack teardown itself
        // invalidates every handle — DD-11 documents the delta).
        TtResetZapMappings(Context);
        ok = TtPldrInitiate(Context);
        break;

    case TENSTORRENT_RESET_DEVICE_CONFIG_WRITE:    // 2 (legacy)
        // The DWC interface-timer trigger hard-wedged a real p150a until a
        // cold power cycle (docs/test-reports/real-silicon.md, divergence D4;
        // DD-14). Only the ttsim/QEMU model, which reports an all-zero
        // subsystem ID, may still exercise it.
        if (!TtResetLegacyFlagPermitted(Context, in.flags)) {
            ok = FALSE;
            break;
        }
        TtBumpResetGen(Context, FileObject);
        TtResetZapMappings(Context);
        ok = TtPcieTimerInterrupt(Context);
        break;

    case TENSTORRENT_RESET_DEVICE_USER_RESET:      // 3
        TtBumpResetGen(Context, FileObject);
        TtResetZapMappings(Context);
        TtSetResetMarker(Context);
        ok = TRUE;
        Context->NeedsHwInit = TRUE;
        break;

    case TENSTORRENT_RESET_DEVICE_ASIC_RESET:      // 4
    case TENSTORRENT_RESET_DEVICE_ASIC_DMC_RESET:  // 5
        // ASIC_RESET (4) shares the CONFIG_WRITE wedge on silicon (DD-14);
        // ASIC_DMC_RESET (5) remains available.
        if (!TtResetLegacyFlagPermitted(Context, in.flags)) {
            ok = FALSE;
            break;
        }
        TtBumpResetGen(Context, FileObject);
        TtResetZapMappings(Context);
        ok = Context->IsBlackhole ? TtBhReset(Context, in.flags) : FALSE;
        Context->NeedsHwInit = TRUE;
        break;

    case TENSTORRENT_RESET_DEVICE_POST_RESET:      // 6 — completion
        ok = TtIsResetMarkerZero(Context);
        if (Context->NeedsHwInit) {
            Context->NeedsHwInit = FALSE;   // cleared unconditionally
            if (ok && TtSafeRestoreState(Context)) {
                if (Context->IsBlackhole) {
                    TtBhRestoreResetState(Context);   // MPS (chardev.c:277)
                    ok = TtBhInitHardware(Context);
                    if (ok) {
                        (VOID)TtBhTelemetryProbe(Context);
                    }
                } else {
                    ok = TRUE;
                }
            } else {
                ok = FALSE;
            }
        }
        break;

    default:
        return STATUS_INVALID_PARAMETER;   // -EINVAL
    }

    // out.result = !ok (chardev.c:292): a failed reset still succeeds the
    // ioctl; only validation errors return error status.
    RtlZeroMemory(&out, sizeof(out));
    out.output_size_bytes = sizeof(out);
    out.result = ok ? 0 : 1;

    TraceLoggingWrite(g_TtTraceProvider, "ResetDevice",
                      TraceLoggingUInt32(in.flags, "flags"),
                      TraceLoggingBoolean(ok, "ok"),
                      TraceLoggingInt64(Context->ResetGen, "resetGen"));

    return TtCompleteSizedOutBuffer(Request,
                                    offsetof(struct tenstorrent_reset_device, out),
                                    &out, sizeof(out), in.output_size_bytes);
}
