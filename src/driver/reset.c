// SPDX-FileCopyrightText: 2026 tt-win-kmd contributors
// SPDX-License-Identifier: GPL-2.0-only
//
// Maps to: tt-kmd/chardev.c ioctl_reset_device (chardev.c:200-310) +
// tt-kmd/pcie.c reset primitives + blackhole.c blackhole_reset/init_hardware.
// Platform reset submission, file-generation invalidation and mapping teardown.
// Linux chip-reset restoration is deliberately unsupported (DD-19/DD-20).
#include "ttkmd.h"

#include <stddef.h>
#include "ttkmd_ioctl.h"
#include "blackhole.h"


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

// Only pci.sys may restore PCI headers/capabilities. Chip-internal reset
// flavors require a separate platform recovery contract and are refused.
// PLDR is asynchronous: the IOCTL acknowledges submission, not completion.
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
        return FALSE;   // never silently join another caller's mutation
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

    TtInvalidateFiles(Context, NULL);
    Context->HardwareReady = FALSE;
    WdfWorkItemEnqueue(Context->PldrWorkItem);
    return TRUE;
}

// Invoke PLDR from an independent worker without holding ResetResource, so
// removal can drain the submitting IOCTL and acquire lifecycle exclusion.
// The worker may start before that IOCTL completes. Completion is observed as
// interface departure + arrival, not by this IOCTL's result. Pre-reset handles
// do not survive RESET_PCIE_LINK (DD-20).
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
    // Keep PldrQueued latched even when the platform refused. D0Entry must
    // not readmit this instance after a later power transition (DD-20).
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
                // Attaches to the creating process when this is not it.
                TtDestroyUserMapping(mapping);
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

// bump_reset_gen (chardev.c:195-198): invalidate files, optionally carrying a
// survivor forward. Current Windows lifecycle/reset paths invalidate all files.
_Use_decl_annotations_
VOID
TtInvalidateFiles(
    _In_ PTT_DEVICE_CONTEXT Context,
    _In_opt_ WDFFILEOBJECT FileObject
    )
{
    LONG64 gen = InterlockedIncrement64(&Context->ResetGen);

    if (FileObject != NULL) {
        TtGetFileContext(FileObject)->OpenResetGen = gen;
    }

    TtResetZapMappings(Context);
    TtMemoryReclaimStale(Context);

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

    UNREFERENCED_PARAMETER(FileObject);

    // Validate the reply before an irreversible submission. A successful
    // transport with result PENDING means only that PLDR was queued (DD-20).
    {
        PVOID buffer;
        size_t length;

        if (in.output_size_bytes < sizeof(out)) {
            return STATUS_INVALID_PARAMETER;
        }
        status = WdfRequestRetrieveOutputBuffer(Request,
            offsetof(struct tenstorrent_reset_device, out) + in.output_size_bytes,
            &buffer, &length);
        if (!NT_SUCCESS(status)) {
            return status;
        }
    }
    if (in.flags > TENSTORRENT_RESET_DEVICE_POST_RESET) {
        return STATUS_INVALID_PARAMETER;
    }
    if (in.flags != TENSTORRENT_RESET_DEVICE_RESET_PCIE_LINK) {
        return STATUS_NOT_SUPPORTED;
    }
    ok = TtPldrInitiate(Context);

    // Linux's result field is retained; Windows value 2 means submission,
    // not completion. A refused submission returns the existing failure 1.
    RtlZeroMemory(&out, sizeof(out));
    out.output_size_bytes = sizeof(out);
    out.result = ok ? TT_RESET_RESULT_PENDING : 1;

    TraceLoggingWrite(g_TtTraceProvider, "ResetDevice",
                      TraceLoggingUInt32(in.flags, "flags"),
                      TraceLoggingBoolean(ok, "ok"),
                      TraceLoggingInt64(Context->ResetGen, "resetGen"));

    return TtCompleteSizedOutBuffer(Request,
                                    offsetof(struct tenstorrent_reset_device, out),
                                    &out, sizeof(out), in.output_size_bytes);
}
