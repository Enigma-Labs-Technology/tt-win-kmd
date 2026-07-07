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

// safe_pci_restore_state (pcie.c:43-59): on the ttsim rig the fake reset
// preserves BARs, so this reduces to a device-present check (vendor ID
// readback). Full config save/restore is OQ-5.
static BOOLEAN
TtSafeRestoreState(
    _In_ PTT_DEVICE_CONTEXT Context
    )
{
    USHORT vendor;

    return TtCfgReadWord(Context, 0x00, &vendor) &&
           vendor == TT_PCI_VENDOR_ID;
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
            ok = TtBhInitHardware(Context);
        }
        break;

    case TENSTORRENT_RESET_DEVICE_RESET_PCIE_LINK: // 1 (legacy)
        // Secondary-bus reset touches the upstream bridge — not expressible
        // from a KMDF function driver (DD-9). Zap mappings and report the
        // device-present state; no gen bump (Linux keeps fds valid).
        TtResetZapMappings(Context);
        ok = TtSafeRestoreState(Context);
        break;

    case TENSTORRENT_RESET_DEVICE_CONFIG_WRITE:    // 2 (legacy)
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
                ok = Context->IsBlackhole ? TtBhInitHardware(Context) : TRUE;
                if (ok && Context->IsBlackhole) {
                    (VOID)TtBhTelemetryProbe(Context);
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
